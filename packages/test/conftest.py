import math
import re
import subprocess
import xml.etree.ElementTree as ET

import pytest
from rich.console import Console
from rich.table import Table


def pytest_addoption(parser):
    parser.addoption(
        "--benchmark",
        default="l1info",
        help="l1info binary to test (default: from PATH)",
    )
    parser.addoption(
        "--cpu",
        type=int,
        default=None,
        help="validate only the L1d instance containing this PU",
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

    instances = l1_instances()
    cpu = config.getoption("--cpu")
    if cpu is not None:
        instances = [i for i in instances if cpu in i["pus"]]
        if not instances:
            raise pytest.UsageError(
                f"CPU {cpu} belongs to no L1d instance "
                "in the lstopo topology"
            )

    targets = [
        dict(instance, cpu=cpu if cpu is not None else min(instance["pus"]))
        for instance in instances
    ]
    config._l1_targets = {t["cpu"]: t for t in targets}


def _min_pass(config):
    return math.ceil(
        config.getoption("--success-ratio") / 100 * config.getoption("--runs")
    )


def l1_instances():
    topo = subprocess.run(
        ["lstopo", "--of", "xml"], capture_output=True, text=True, check=True
    ).stdout
    instances = []

    def walk(obj, l2_group):
        obj_type = obj.get("type")
        if obj_type == "L2Cache":
            l2_group = obj.get("gp_index")

        if obj_type != "L1Cache":
            for child in obj:
                if child.tag == "object":
                    walk(child, l2_group)
            return

        pus = [
            int(pu.get("os_index"))
            for pu in obj.iter("object")
            if pu.get("type") == "PU"
        ]
        if pus:
            instances.append(
                {
                    "pus": pus,
                    "size": int(obj.get("cache_size")),
                    "line": int(obj.get("cache_linesize")),
                    "ways": int(obj.get("cache_associativity")),
                    "l2_group": l2_group,
                }
            )

    root = ET.fromstring(topo)
    for child in root:
        if child.tag == "object":
            walk(child, None)

    return sorted(instances, key=lambda e: e["pus"][0])


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

    return (
        f"l1info vs lstopo: {len(targets)} core(s) x {runs} run(s), "
        f"need >= {_min_pass(config)}/{runs} "
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
        expected = (
            f"{target.get('size', 0) // 1024} KiB/"
            f"{target.get('line', '?')} B/{target.get('ways', '?')}-way"
        )
        passed = entry["good"] >= min_pass

        table.add_row(
            str(cpu),
            expected,
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
