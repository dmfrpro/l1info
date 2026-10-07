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
        "size_bytes": int(props["size_bytes"]),
        "line_size": int(props["line_size"]),
        "associativity": int(props["associativity"]),
    }


def describe(size, line, ways):
    return f"size={size} B line={line} assoc={ways}"


def test_l1_matches_lstopo(core, run, pytestconfig):
    benchmark = pytestconfig.getoption("--benchmark")
    got = measure(benchmark, core["cpu"])

    exp = {
        "size_bytes": core["size_bytes"],
        "line_size": core["line_size"],
        "associativity": core["associativity"],
    }

    mismatches = []
    if got["size_bytes"] != exp["size_bytes"]:
        mismatches.append(
            f"capacity {got['size_bytes']} B ({got['size_bytes'] / 1024} KiB), "
            f"expected {exp['size_bytes']} B ({exp['size_bytes'] / 1024} KiB)"
        )

    if got["line_size"] != exp["line_size"]:
        mismatches.append(
            f"line size {got['line_size']} B, expected {exp['line_size']} B"
        )

    if got["associativity"] != exp["associativity"]:
        mismatches.append(
            f"associativity {got['associativity']}-way, "
            f"expected {exp['associativity']}-way"
        )

    if mismatches:
        expected = describe(
            exp["size_bytes"], exp["line_size"], exp["associativity"]
        )
        measured = describe(
            got["size_bytes"], got["line_size"], got["associativity"]
        )

        pytest.fail(
            f"cpu {core['cpu']} run #{run + 1}: measured {measured}, "
            f"expected {expected}\n" + "\n".join(mismatches),
            pytrace=False,
        )
