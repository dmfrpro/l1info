import subprocess
import pytest


def measure(benchmark, cpu):
    proc = subprocess.run(
        ["taskset", "-c", str(cpu), benchmark],
        capture_output=True,
        text=True,
        check=True,
    )

    props = {}
    for line in proc.stdout.strip().splitlines():
        key, _, value = line.partition("=")
        props[key] = value

    return {
        "cpu": int(props["cpu"]),
        "size_bytes": int(props["size_bytes"]),
        "line_size": int(props["line_size"]),
        "associativity": int(props["associativity"]),
    }


def describe(size, line, ways):
    return f"{size // 1024} KiB/{line} B/{ways}-way"


def test_l1_matches_lstopo(core, run, pytestconfig):
    benchmark = pytestconfig.getoption("--benchmark")
    got = measure(benchmark, core["cpu"])

    mismatches = []
    if got["size_bytes"] != core["size"]:
        mismatches.append(
            f"capacity {got['size_bytes'] // 1024} KiB, "
            f"expected {core['size'] // 1024} KiB"
        )

    if got["line_size"] != core["line"]:
        mismatches.append(
            f"line size {got['line_size']} B, expected {core['line']} B"
        )

    if got["associativity"] != core["ways"]:
        mismatches.append(
            f"associativity {got['associativity']}-way, "
            f"expected {core['ways']}-way"
        )

    if got["cpu"] != core["cpu"]:
        mismatches.append(f"ran on CPU {got['cpu']} instead of {core['cpu']}")

    if mismatches:
        expected = describe(core["size"], core["line"], core["ways"])
        measured = describe(
            got["size_bytes"], got["line_size"], got["associativity"]
        )

        pytest.fail(
            f"cpu {core['cpu']} run #{run + 1}: measured {measured}, "
            f"expected {expected}\n" + "\n".join(mismatches),
            pytrace=False,
        )
