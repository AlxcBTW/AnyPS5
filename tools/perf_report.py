import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

THRESHOLD = 1.0
CACHEGRIND_SUMMARY = re.compile(r"^summary:\s*(\d+)\s*$", re.M)
PERF_COUNT = re.compile(r"^\s*(\d+),,instructions:u", re.M)

WORKLOADS = {
    "shader-control-flow": ("agc_shader_control_flow_tests", ()),
    "shader-structurizer": ("agc_structurizer_tests", ()),
    "agc-draw-mock": ("agc_driver_graphics_tests", ()),
}


def targets():
    return sorted({target for target, _ in WORKLOADS.values()})


def counter_command(counter, executable, arguments, scratch):
    if counter == "valgrind":
        log = scratch / "cachegrind.out"
        return ["valgrind", "--tool=cachegrind", "--cache-sim=no", "--fair-sched=yes",
                f"--cachegrind-out-file={log}", "--", str(executable), *arguments], log
    return ["perf", "stat", "-x,", "-e", "instructions:u", "--", str(executable), *arguments], None


def parse_count(counter, log, stderr):
    if counter == "valgrind":
        found = CACHEGRIND_SUMMARY.search(log.read_text()) if log.exists() else None
    else:
        found = PERF_COUNT.search(stderr)
    return int(found.group(1)) if found else None


def pick_counter(requested):
    if requested != "auto":
        return requested
    return "valgrind" if shutil.which("valgrind") else "perf"


def measure(counter, executable, arguments, repeats):
    environment = dict(os.environ, ANYPS5_NO_SHADER_CACHE="1")
    counts = []
    for _ in range(repeats):
        with tempfile.TemporaryDirectory() as scratch:
            command, log = counter_command(counter, executable, arguments, Path(scratch))
            done = subprocess.run(command, cwd=Path.cwd(), env=environment, capture_output=True, text=True)
            count = parse_count(counter, log, done.stderr)
            if done.returncode != 0 or count is None:
                tail = (done.stdout + done.stderr).strip().splitlines()[-5:]
                return {"error": f"exit {done.returncode}: " + " | ".join(tail)[:300]}
            counts.append(count)
    return {"ir": int(statistics.median(counts)), "runs": counts}


def run(args):
    counter = pick_counter(args.counter)
    result = {"counter": counter, "workloads": {}}
    for name, (target, arguments) in WORKLOADS.items():
        executable = Path(args.build).resolve() / "tests" / target
        if not executable.exists():
            executable = executable.with_suffix(".exe")
        result["workloads"][name] = measure(counter, executable, arguments, args.repeats)
        print(f"{name}: {result['workloads'][name]}", file=sys.stderr)
    Path(args.output).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if any("error" in entry for entry in result["workloads"].values()):
        sys.exit(1)


def delta(base, head):
    return (head - base) / base * 100 if base else 0.0


def verdict(percent, threshold):
    if percent > threshold:
        return "slower"
    if percent < -threshold:
        return "faster"
    return "ok"


def rows(base, head, threshold):
    table = []
    for name in sorted(set(base["workloads"]) | set(head["workloads"])):
        before, after = base["workloads"].get(name, {}), head["workloads"].get(name, {})
        if "ir" in before and "ir" in after:
            percent = delta(before["ir"], after["ir"])
            table.append((name, before["ir"], after["ir"], percent, verdict(percent, threshold)))
        else:
            table.append((name, before.get("ir"), after.get("ir"), None, "no data"))
    return table


def spread(side):
    worst = 0.0
    for entry in side["workloads"].values():
        runs = entry.get("runs", [])
        if len(runs) > 1 and min(runs):
            worst = max(worst, (max(runs) - min(runs)) / min(runs) * 100)
    return worst


def cell(value):
    return "n/a" if value is None else f"{value:,}"


def compare(args):
    base, head = (json.loads(Path(path).read_text()) for path in (args.base, args.head))
    table = rows(base, head, args.threshold)
    if not table:
        return
    lines = ["| Workload | Base Ir | Head Ir | Delta | |", "| --- | ---: | ---: | ---: | --- |"]
    for name, before, after, percent, flag in table:
        shown = "n/a" if percent is None else f"{percent:+.3f} %"
        lines.append(f"| `{name}` | {cell(before)} | {cell(after)} | {shown} | {'' if flag == 'ok' else flag} |")
    machine = {name: [before, after] for name, before, after, _, _ in table}
    lines += [
        "",
        f"Instructions retired in user mode ({head['counter']}), fixed single-threaded test workloads, no game data and no GPU. "
        f"Flagged beyond +/-{args.threshold:g} %. Run-to-run spread on this runner: {max(spread(base), spread(head)):.3f} %.",
        f"<!-- perf-data {json.dumps(machine, sort_keys=True, separators=(',', ':'))} -->",
    ]
    print("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description="Instruction-count performance report for pull requests")
    commands = parser.add_subparsers(dest="command", required=True)
    runner = commands.add_parser("run", help="measure every workload of a build tree")
    runner.add_argument("build")
    runner.add_argument("output")
    runner.add_argument("--counter", choices=("auto", "valgrind", "perf"), default="auto")
    runner.add_argument("--repeats", type=int, default=1)
    runner.set_defaults(handler=run)
    comparer = commands.add_parser("compare", help="print a markdown table for base and head measurements")
    comparer.add_argument("base")
    comparer.add_argument("head")
    comparer.add_argument("--threshold", type=float, default=THRESHOLD)
    comparer.set_defaults(handler=compare)
    lister = commands.add_parser("targets", help="print the CMake targets the workloads need")
    lister.set_defaults(handler=lambda args: print(" ".join(targets())))
    args = parser.parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
