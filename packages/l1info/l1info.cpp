#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#define WORD_BYTES (sizeof(size_t))
#define PAGE_BYTES 4096

#define LINE_REGION_BYTES (256ul << 10)
#define LINE_MIN_STEPS 4000000

#define CAPACITY_MIN_STEPS 2000000
#define CAPACITY_GRID_STEP (4ul << 10)
#define CAPACITY_GRID_MAX (128ul << 10)

#define ASSOC_STRIDE_ELEMS (PAGE_BYTES / WORD_BYTES)
#define ASSOC_MAX_SPOTS 16
#define ASSOC_MIN_STEPS 2000000

#define CHASE_RUNS 3

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

size_t make_chase(size_t *buf, size_t n_elems, size_t stride, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    const size_t n = n_elems / stride;

    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i)
        order[i] = i * stride;
    std::shuffle(order.begin(), order.end(), rng);

    for (size_t i = 0; i < n; ++i)
        buf[order[i]] = order[(i + 1) % n];

    return n;
}

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

double chase_avg(size_t *buf, size_t count, size_t min_steps)
{
    chase(buf, count);

    double total = 0.0;
    for (size_t i = 0; i < CHASE_RUNS; ++i)
        total += chase(buf, std::max(count, size_t(min_steps)));
    return total / CHASE_RUNS;
}

struct Scan
{
    std::vector<size_t> x;
    std::vector<double> lat;
};

size_t argmax_rise(const Scan &scan)
{
    size_t jump = 0;
    for (size_t i = 1; i + 1 < scan.lat.size(); ++i)
    {
        if (scan.lat[i + 1] - scan.lat[i] > scan.lat[jump + 1] - scan.lat[jump])
            jump = i;
    }

    return jump;
}

size_t measure_line_size(Scan &scan)
{
    const size_t strides_b[] = {8, 16, 32, 64, 128, 256, 512, 1024};

    AlignedBuffer buf(LINE_REGION_BYTES);
    const size_t n = LINE_REGION_BYTES / WORD_BYTES;

    for (size_t stride_b : strides_b)
    {
        const size_t count = make_chase(buf.p, n, stride_b / WORD_BYTES, 7 + stride_b);
        scan.x.push_back(stride_b);
        scan.lat.push_back(chase_avg(buf.p, count, LINE_MIN_STEPS));
    }

    return scan.x[argmax_rise(scan) + 1];
}

size_t measure_capacity(size_t line_bytes, Scan &scan)
{
    const size_t stride = line_bytes / WORD_BYTES;

    std::vector<size_t> sizes = {2ul << 10, 4ul << 10, 8ul << 10};
    for (size_t s = 16ul << 10; s <= CAPACITY_GRID_MAX; s += CAPACITY_GRID_STEP)
        sizes.push_back(s);

    for (size_t s : sizes)
    {
        AlignedBuffer buf(s);
        const size_t n = s / WORD_BYTES;
        const size_t count = make_chase(buf.p, n, stride, 12345 + s);

        scan.x.push_back(s);
        scan.lat.push_back(chase_avg(buf.p, count, CAPACITY_MIN_STEPS));
    }

    return scan.x[argmax_rise(scan)];
}

size_t measure_associativity(Scan &scan)
{
    AlignedBuffer buf(PAGE_BYTES * ASSOC_MAX_SPOTS);

    for (size_t k = 1; k <= ASSOC_MAX_SPOTS; ++k)
    {
        const size_t count = make_chase(buf.p, k * ASSOC_STRIDE_ELEMS, ASSOC_STRIDE_ELEMS, 99 + k);
        scan.x.push_back(k);
        scan.lat.push_back(chase_avg(buf.p, count, ASSOC_MIN_STEPS));
    }

    return scan.x[argmax_rise(scan)];
}

std::ostream &operator<<(std::ostream &os, const Scan &scan)
{
    os.precision(10);
    for (size_t i = 0; i < scan.x.size(); ++i)
    {
        if (i > 0)
            os << ',';
        os << scan.x[i] << ':' << scan.lat[i];
    }
    return os;
}

struct Measurement
{
    size_t capacity;
    size_t line;
    size_t ways;
    Scan line_scan;
    Scan capacity_scan;
    Scan assoc_scan;
};

Measurement measure()
{
    Measurement m{};

    m.line = measure_line_size(m.line_scan);
    m.capacity = measure_capacity(m.line, m.capacity_scan);
    m.ways = measure_associativity(m.assoc_scan);

    return m;
}

std::ostream &operator<<(std::ostream &os, const Measurement &m)
{
    // clang-format off
    return os << "method=pointer-chase\n"
              << "size_bytes=" << m.capacity << '\n'
              << "line_size=" << m.line << '\n'
              << "associativity=" << m.ways << '\n'
              << "line_scan_ns=" << m.line_scan << '\n'
              << "capacity_scan_ns=" << m.capacity_scan << '\n'
              << "assoc_scan_ns=" << m.assoc_scan << '\n'
              << "checksum=" << g_checksum << '\n';
    // clang-format on
}

} // namespace

int main()
{
    std::cout << measure();
    return 0;
}
