// L1 data cache characteristics benchmark
//
// Capacity and line size via dependent-load pointer chases:
//   https://github.com/intel/lmbench/blob/master/src/lat_mem_rd.c (S&S)
//   https://scispace.com/pdf/automatic-measurement-of-memory-hierarchy-parameters-4jms0yotfe.pdf
//   (X-Ray)
//
// Associativity via a random linked eviction cycle in one cache set:
//   https://yuval.yarom.org/pdfs/LiuYGHL15.pdf
//   https://arxiv.org/abs/1810.01497

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

#include <cpuid.h>
#include <sched.h>

#define WORD_BYTES (sizeof(size_t))
#define PAGE_BYTES 4096

#define LINE_REGION_BYTES (1ul << 20) // 1MB
#define LINE_MIN_STEPS 20000000

#define CAPACITY_SCAN_MAX (8ul << 20)
#define CAPACITY_MIN_STEPS 10000000
#define CAPACITY_JUMP_FACTOR 1.5
#define CAPACITY_FIT_THRESHOLD 1.15
#define CAPACITY_REFINE_STEP 1024

#define ASSOC_MAX_K 20
#define ASSOC_MIN_STEPS 4000000
#define ASSOC_KNEE_EPS 0.25

using Clock = std::chrono::steady_clock;

namespace
{

size_t g_checksum = 0;

struct AlignedBuffer
{
    size_t *p = nullptr;

    explicit AlignedBuffer(size_t bytes)
    {
        void *ptr = nullptr;
        if (posix_memalign(&ptr, PAGE_BYTES, bytes) != 0 || ptr == nullptr)
        {
            std::fprintf(stderr, "error: cannot allocate %zu bytes\n", bytes);
            std::exit(1);
        }
        p = static_cast<size_t *>(ptr);
    }
    ~AlignedBuffer()
    {
        std::free(p);
    }

    AlignedBuffer(const AlignedBuffer &) = delete;
    AlignedBuffer &operator=(const AlignedBuffer &) = delete;
};

bool pin_to_current_cpu(int &cpu)
{
    cpu = sched_getcpu();

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    return sched_setaffinity(0, sizeof set, &set) == 0;
}

struct CacheParams
{
    size_t size_bytes = 0;
    size_t line = 0;
    size_t ways = 0;
    size_t sets = 0;
};

CacheParams cpuid_l1d()
{
    CacheParams cp;

    unsigned eax, ebx, ecx, edx;
    for (unsigned i = 0;; ++i)
    {
        __cpuid_count(4, i, eax, ebx, ecx, edx);

        const unsigned type = eax & 0x1f; // 1 = data, 3 = unified
        if (type == 0)
            break;
        if (type != 1 || ((eax >> 5) & 0x7) != 1)
            continue;

        cp.line = (ebx & 0xfff) + 1;
        cp.ways = ((ebx >> 22) & 0x3ff) + 1;
        cp.sets = ecx + 1;
        cp.size_bytes = static_cast<size_t>(cp.ways) * cp.line * cp.sets;
        break;
    }

    return cp;
}

// Link 0, stride, 2*stride, ... (< n_elems) into a cycle visiting a
// random permutation of block_elems-sized permuted blocks
size_t make_chase(size_t *buf, size_t n_elems, size_t stride, size_t block_elems, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    const size_t per_block = block_elems / stride;
    const size_t n_blocks = n_elems / block_elems;

    std::vector<size_t> blocks(n_blocks);
    std::iota(blocks.begin(), blocks.end(), 0);
    std::shuffle(blocks.begin(), blocks.end(), rng);

    std::vector<size_t> within(per_block);
    std::vector<size_t> order;
    order.reserve(n_blocks * per_block);
    for (size_t b : blocks)
    {
        std::iota(within.begin(), within.end(), 0);
        std::shuffle(within.begin(), within.end(), rng);
        for (size_t j : within)
            order.push_back(b * block_elems + j * stride);
    }

    for (size_t i = 0; i < order.size(); ++i)
        buf[order[i]] = order[(i + 1) % order.size()];

    return order.size();
}

// Load-use dependency chain (warmup)
double chase(size_t *buf, size_t steps)
{
    size_t p = 0;

    const auto t0 = Clock::now();
    for (size_t i = 0; i < steps; ++i)
        p = buf[p];
    const auto t1 = Clock::now();

    g_checksum ^= p;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(steps);
}

// While stride < line one fetched line serves line/stride
// accesses. The curve flattens once stride >= line. The first stride at
// 90% of the plateau latency is the line size
size_t measure_line_size()
{
    const size_t strides_b[] = {8, 16, 32, 64, 128, 256, 512};
    constexpr size_t kN = sizeof(strides_b) / sizeof(strides_b[0]);

    AlignedBuffer buf(LINE_REGION_BYTES);
    const size_t n = LINE_REGION_BYTES / WORD_BYTES;

    double lat[kN];
    for (size_t i = 0; i < kN; ++i)
    {
        const size_t count = make_chase(buf.p, n, strides_b[i] / WORD_BYTES, PAGE_BYTES / WORD_BYTES, 7 + i);

        chase(buf.p, count);
        lat[i] = chase(buf.p, std::max(count, size_t(LINE_MIN_STEPS)));
    }

    const double plateau = 0.5 * (lat[kN - 1] + lat[kN - 2]);
    for (size_t i = 0; i < kN; ++i)
    {
        if (lat[i] >= 0.9 * plateau)
            return strides_b[i];
    }

    return strides_b[kN - 1];
}

// ns/access over a fully-random chase of bytes with the given stride
double time_region(size_t bytes, size_t stride, uint64_t seed)
{
    AlignedBuffer buf(bytes);
    const size_t count = make_chase(buf.p, bytes / WORD_BYTES, stride, bytes / WORD_BYTES, seed);

    chase(buf.p, count);
    return chase(buf.p, std::max(count, size_t(CAPACITY_MIN_STEPS)));
}

// With the stride fixed at the line size, grow the region
// until latency jumps. A coarse x1.5 scan brackets the jump,
// a 1 KiB refine finds the largest fitting region
size_t measure_capacity(size_t line_bytes)
{
    const size_t stride = line_bytes / WORD_BYTES;

    double base = 0, prev_lat = 0;
    size_t lo = 0, hi = 0;
    for (size_t s = PAGE_BYTES; s <= CAPACITY_SCAN_MAX; s += s / 2)
    {
        const double lat = time_region(s, stride, 12345 + s);

        if (base == 0)
            base = lat;
        if (prev_lat > 0 && lat > CAPACITY_JUMP_FACTOR * base && lat > CAPACITY_JUMP_FACTOR * prev_lat)
        {
            hi = s;
            break;
        }

        prev_lat = lat;
    }
    if (hi == 0)
    {
        std::fprintf(stderr, "error: no latency jump found up to %d MiB\n", static_cast<int>(CAPACITY_SCAN_MAX >> 20));
        std::exit(1);
    }
    lo = hi / 3 * 2;

    const double threshold = CAPACITY_FIT_THRESHOLD * base;
    size_t capacity = lo;
    for (size_t s = lo & ~(size_t(CAPACITY_REFINE_STEP) - 1); s <= hi; s += CAPACITY_REFINE_STEP)
    {
        if (time_region(s, stride, 777 + s) < threshold)
            capacity = s;
    }

    return capacity;
}

// Link the k+1 conflicting lines (slots 0..k) into one random-order
// dependent cycle
void link_conflict_cycle(size_t *buf, size_t spacing, size_t k, uint64_t seed)
{
    std::vector<size_t> perm(k + 1);
    std::iota(perm.begin(), perm.end(), 0);

    std::mt19937_64 rng(seed);
    std::shuffle(perm.begin(), perm.end(), rng);

    for (size_t j = 0; j <= k; ++j)
        buf[perm[j] * spacing] = perm[(j + 1) % (k + 1)] * spacing;
}

// The associativity is the start of the latency rise: the first k
// reaching 1+eps times the hit latency, taken as the curve minimum
size_t detect_knee(const std::vector<double> &lat)
{
    const double hit = *std::min_element(lat.begin(), lat.end());

    for (size_t i = 0; i < lat.size(); ++i)
    {
        if (lat[i] >= (1.0 + ASSOC_KNEE_EPS) * hit)
            return i + 1;
    }

    return lat.size();
}

// For every k, chase the k+1-line conflict cycle:
// while k+1 <= ways every access hits, at k == ways the
// cycle no longer fits and the latency rises
size_t measure_associativity()
{
    const size_t spacing = PAGE_BYTES / WORD_BYTES;
    AlignedBuffer buf(PAGE_BYTES * (ASSOC_MAX_K + 1));

    std::vector<double> lat(ASSOC_MAX_K);
    for (size_t k = 1; k <= ASSOC_MAX_K; ++k)
    {
        link_conflict_cycle(buf.p, spacing, k, 99 + k);

        chase(buf.p, ASSOC_MIN_STEPS / 4);
        lat[k - 1] = chase(buf.p, ASSOC_MIN_STEPS);
    }

    return detect_knee(lat);
}

struct Measurement
{
    int cpu;
    size_t capacity;
    size_t line;
    size_t ways;
    CacheParams cpuid;
    size_t checksum;
};

Measurement measure()
{
    Measurement m{};

    if (!pin_to_current_cpu(m.cpu))
        std::fprintf(stderr, "warning: could not pin to CPU %d", m.cpu);

    m.cpuid = cpuid_l1d();

    // Hello from Asahi Linux M1 Pro with 16K pages
    if (m.cpuid.sets * m.cpuid.line > PAGE_BYTES)
        std::fprintf(stderr, "warning: CPUID reports %zu sets x %zu B line (> 4 KiB page):\n", m.cpuid.sets,
                     m.cpuid.line);

    m.line = measure_line_size();
    m.capacity = measure_capacity(m.line);
    m.ways = measure_associativity();
    m.checksum = g_checksum;

    return m;
}

std::ostream &operator<<(std::ostream &os, const Measurement &m)
{
    // clang-format off
    return os << "cpu=" << m.cpu << '\n'
              << "method=chase+eviction-cycle\n"
              << "size_bytes=" << m.capacity << '\n'
              << "line_size=" << m.line << '\n'
              << "associativity=" << m.ways << '\n'
              << "cpuid_size_bytes=" << m.cpuid.size_bytes << '\n'
              << "cpuid_line_size=" << m.cpuid.line << '\n'
              << "cpuid_ways=" << m.cpuid.ways << '\n'
              << "cpuid_sets=" << m.cpuid.sets << '\n'
              << "checksum=" << m.checksum << '\n';
    // clang-format on
}

} // namespace

int main()
{
    std::cout << measure();
    return 0;
}
