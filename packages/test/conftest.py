import os
import re
import subprocess
import xml.etree.ElementTree as ET

import pytest
from rich.console import Console
from rich.table import Table


def l1_by_pu():
    topo = subprocess.run(
        ["lstopo", "--of", "xml"], capture_output=True, text=True, check=True
    ).stdout

    by_pu = {}

    def walk(obj):
        if obj.get("type") == "L1Cache":
            expected = {
                "size_bytes": int(obj.get("cache_size")),
                "line_size": int(obj.get("cache_linesize")),
                "associativity": int(obj.get("cache_associativity")),
            }
            for pu in obj.iter("object"):
                if pu.get("type") == "PU":
                    by_pu[int(pu.get("os_index"))] = expected
            return

        for child in obj:
            if child.tag == "object":
                walk(child)

    root = ET.fromstring(topo)
    for child in root:
        if child.tag == "object":
            walk(child)

    return by_pu


def pytest_addoption(parser):
    parser.addoption(
        "--benchmark",
        default="l1info",
        help="benchmark binary to test (default: from PATH)",
    )
    parser.addoption(
        "--cpu",
        type=int,
        default=None,
        help="validate only this CPU",
    )
    parser.addoption(
        "--runs",
        type=int,
        default=12,
        help="benchmark runs per core (default: 12)",
    )
    parser.addoption(
        "--success-ratio",
        type=float,
        default=100 * 10 / 12,
        help="percentage of a core's runs that must match lstopo for the "
        "core to pass, 0-100 (default: 83.33, i.e. 10/12)",
    )


def pytest_configure(config):
    if config.getoption("--runs") < 1:
        raise pytest.UsageError("--runs must be >= 1")

    ratio = config.getoption("--success-ratio")
    if not 0 < ratio <= 100:
        raise pytest.UsageError("--success-ratio must be within (0, 100]")

    by_pu = l1_by_pu()
    cpus = sorted(os.sched_getaffinity(0))
    targets = {c: dict(by_pu[c], cpu=c) for c in cpus if c in by_pu}
    if not targets:
        raise pytest.UsageError(
            "no L1d instance in the lstopo topology covers "
            f"the available CPUs {cpus}"
        )

    cpu = config.getoption("--cpu")
    if cpu is not None:
        if cpu not in targets:
            raise pytest.UsageError(
                f"CPU {cpu} belongs to no L1d instance in the lstopo "
                "topology, or is outside the affinity mask"
            )
        targets = {cpu: targets[cpu]}

    config._l1_targets = targets


def describe(expected):
    return (
        f"size={expected['size_bytes']} B line={expected['line_size']} "
        f"assoc={expected['associativity']}"
    )


def _min_pass(config):
    return (
        config.getoption("--success-ratio") / 100 * config.getoption("--runs")
    )


def pytest_generate_tests(metafunc):
    if "core" not in metafunc.fixturenames:
        return

    targets = metafunc.config._l1_targets.values()
    runs = metafunc.config.getoption("--runs")

    metafunc.parametrize(
        "core,run",
        [(target, run) for target in targets for run in range(runs)],
        ids=lambda p: f"cpu{p['cpu']}" if isinstance(p, dict) else f"#{p + 1}",
    )


def pytest_report_header(config):
    targets = getattr(config, "_l1_targets", {})
    runs = config.getoption("--runs")
    expected = ", ".join(sorted({describe(t) for t in targets.values()}))

    return (
        f"l1info vs lstopo: {len(targets)} core(s) x {runs} run(s), "
        f"expected {expected}, need >= {_min_pass(config)}/{runs} "
        f"({config.getoption('--success-ratio'):.1f}%) matching per core"
    )


def _core_stats(terminalreporter):
    stats = {}
    for outcome in ("passed", "failed", "error"):
        for report in terminalreporter.stats.get(outcome, []):
            if report.when != "call":
                continue

            match = re.search(r"cpu(\d+)", report.nodeid)
            if not match:
                continue

            entry = stats.setdefault(
                int(match.group(1)), {"good": 0, "total": 0}
            )
            entry["total"] += 1
            entry["good"] += outcome == "passed"
    return stats


def _failing_cores(config, stats):
    return [
        cpu for cpu, entry in stats.items() if entry["good"] < _min_pass(config)
    ]


@pytest.hookimpl(trylast=True)
def pytest_sessionfinish(session, exitstatus):
    stats = _core_stats(
        session.config.pluginmanager.get_plugin("terminalreporter")
    )
    if stats:
        session.exitstatus = 1 if _failing_cores(session.config, stats) else 0


def pytest_terminal_summary(terminalreporter):
    stats = _core_stats(terminalreporter)
    targets = getattr(terminalreporter.config, "_l1_targets", {})

    if not stats:
        return

    min_pass = _min_pass(terminalreporter.config)

    table = Table(title="per-core verdict")
    table.add_column("CPU", justify="right", style="cyan")
    table.add_column("lstopo")
    table.add_column("matching runs", justify="right")
    table.add_column("result", justify="center")

    for cpu in sorted(stats):
        entry = stats[cpu]
        target = targets.get(cpu, {})
        passed = entry["good"] >= min_pass

        table.add_row(
            str(cpu),
            describe(target) if target else "?",
            f"{entry['good']}/{entry['total']} (need >= {min_pass})",
            "[green]PASS[/]" if passed else "[bold red]FAIL[/]",
        )

    Console().print(table)
    failing = _failing_cores(terminalreporter.config, stats)
    if failing:
        Console().print(
            f"[bold red]{len(failing)} core(s) failed validation[/]"
        )
    else:
        Console().print(
            "[green]all cores validated: measurements match lstopo[/]"
        )
