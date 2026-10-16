#!/usr/bin/env python3
"""Measurement table for md2-nav.ps1 rows (agent gp).

  md2-fps.py <result-dir> [<result-dir> ...] [--all] [--states]

For every hold/walk window recorded in end.json ("marks": the [present] and [gputime] line counters at its start and
end) prints the 10-second windows wholly inside it (the first line after the start covers the 10 s before it and is dropped,
as in md2-summary.py): presents per 10 s (FPS = presents / 10), GPU busy and idle gaps per
frame, the top [gputime] classes per present, and RAM from machine.csv (appWsGiB, freeGiB). With --all it also prints
every [present] window of the whole row (boot, navigation, loading) so cold stretches can be told apart. With
--states it prints one line per screen state of nav.log ("screen state" transitions): how long the screen was up and the
FPS of the 10-second windows that lie wholly inside it.
"""

import csv
import json
import os
import re
import statistics
import sys

PRESENT = re.compile(r"^\[present\] (\d+) presents over 10 s.*?per frame, presented or dropped: GPU busy ([0-9.]+) ms, idle gaps ([0-9.]+) ms")
CLASS = re.compile(r"([a-z][a-z-]*) x(\d+) ([0-9.]+)ms ([0-9.]+)MiB \(per present x([0-9.]+) ([0-9.]+)ms ([0-9.]+)MiB\)")
PROGRAM = re.compile(r"(0x[0-9a-f]+) x(\d+) (\d+)ms")


def read_lines(path):
    present, gpu = [], []
    with open(path, "rb") as handle:
        for raw in handle:
            if raw.startswith(b"[present]"):
                present.append(raw.decode("utf-8", "replace"))
            elif raw.startswith(b"[gputime]"):
                gpu.append(raw.decode("utf-8", "replace"))
    return present, gpu


def parse_present(line):
    m = PRESENT.match(line)
    if not m:
        return None
    return int(m.group(1)), float(m.group(2)), float(m.group(3))


def parse_gpu(line):
    head, _, tail = line.partition("by class:")
    classes = {m.group(1): (float(m.group(6)), float(m.group(7))) for m in CLASS.finditer(tail)}
    programs = [(m.group(1), int(m.group(2)), int(m.group(3))) for m in PROGRAM.finditer(head.partition("by program:")[2])]
    total = re.match(r"\[gputime\] (\d+) ms of GPU time", line)
    return (int(total.group(1)) if total else 0), classes, programs


def machine_rows(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def window_report(name, present, gpu, start_s, end_s, machine):
    print(f"\n== window {name}: [present] lines {len(present)}  ({start_s:.0f}s..{end_s:.0f}s into the row)")
    fps_list, busy_list, idle_list = [], [], []
    for index, line in enumerate(present):
        parsed = parse_present(line)
        if not parsed:
            continue
        count, busy, idle = parsed
        fps_list.append(count / 10.0); busy_list.append(busy); idle_list.append(idle)
        print(f"  win {index + 1:3d}: {count / 10.0:6.1f} FPS   GPU busy {busy:6.1f} ms   idle gaps {idle:6.1f} ms per frame")
    if fps_list:
        print(f"  mean {statistics.mean(fps_list):.2f} FPS  median {statistics.median(fps_list):.2f}  min {min(fps_list):.1f}  max {max(fps_list):.1f}   "
              f"GPU busy mean {statistics.mean(busy_list):.1f} ms, idle gaps mean {statistics.mean(idle_list):.1f} ms per frame")
    agg = {}
    prog = {}
    for line in gpu:
        _, classes, programs = parse_gpu(line)
        for key, (ms, mib) in classes.items():
            a = agg.setdefault(key, [0.0, 0.0, 0]); a[0] += ms; a[1] += mib; a[2] += 1
        for addr, count, ms in programs:
            p = prog.setdefault(addr, [0, 0]); p[0] += count; p[1] += ms
    if agg:
        top = sorted(agg.items(), key=lambda kv: -kv[1][0] / max(1, kv[1][2]))[:8]
        print("  [gputime] per present, mean over windows: " + "  ".join(f"{k} {v[0] / v[2]:.2f} ms {v[1] / v[2]:.2f} MiB" for k, v in top))
    if prog:
        top = sorted(prog.items(), key=lambda kv: -kv[1][1])[:5]
        print("  top programs (summed ms over the window): " + "  ".join(f"{a} x{c} {ms}ms" for a, (c, ms) in top))
    rows = [r for r in machine if start_s <= float(r["elapsedSeconds"]) <= end_s]
    if rows:
        ws = [float(r["appWsGiB"]) for r in rows]; free = [float(r["freeGiB"]) for r in rows]
        print(f"  RAM: appWsGiB {min(ws):.2f}..{max(ws):.2f} (last {ws[-1]:.2f}), freeGiB {min(free):.2f}..{max(free):.2f} (last {free[-1]:.2f})")


STATE = re.compile(r"^\[\s*([0-9.,]+)s pl=(\d+) gl=(\d+)\] screen state: '([^']*)' -> '([^']*)'")
END = re.compile(r"^\[\s*([0-9.,]+)s pl=(\d+) gl=(\d+)\] end:")


def state_report(directory, present):
    path = os.path.join(directory, "nav.log")
    if not os.path.exists(path):
        return
    marks = []
    last = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = STATE.match(line)
        if m:
            marks.append((float(m.group(1).replace(",", ".")), int(m.group(2)), m.group(5) or "-"))
        e = END.match(line)
        if e:
            last = (float(e.group(1).replace(",", ".")), int(e.group(2)))
    if last:
        marks.append((last[0], last[1], "(end)"))
    print("\n== screen states (windows wholly inside each state)")
    for i in range(len(marks) - 1):
        t0, p0, name = marks[i]
        t1, p1, _ = marks[i + 1]
        wins = [parse_present(line) for line in present[p0 + 1:p1]]
        wins = [w for w in wins if w]
        if wins:
            fps = [w[0] / 10.0 for w in wins]
            print(f"  {name:14s} {t0:6.1f}s..{t1:6.1f}s ({t1 - t0:5.1f}s): {len(wins)} windows, FPS mean {statistics.mean(fps):5.1f} (min {min(fps):.1f}, max {max(fps):.1f}), GPU busy {statistics.mean(w[1] for w in wins):6.1f} ms, idle gaps {statistics.mean(w[2] for w in wins):6.1f} ms per frame")
        else:
            print(f"  {name:14s} {t0:6.1f}s..{t1:6.1f}s ({t1 - t0:5.1f}s): no full window")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    for directory in args:
        print(f"\n######## {directory}")
        end_path = os.path.join(directory, "end.json")
        end = json.load(open(end_path, encoding="utf-8-sig")) if os.path.exists(end_path) else {}
        present, gpu = read_lines(os.path.join(directory, "stderr.log"))
        machine = machine_rows(os.path.join(directory, "machine.csv"))
        print(f"tag {end.get('tag')} phase {end.get('phase')} stop {end.get('stopReason')} lines present {len(present)} gputime {len(gpu)}")
        for mark in end.get("marks") or []:
            ps, pe = mark["startPresentLines"], mark["endPresentLines"]
            gs, ge = mark["startGpuLines"], mark["endGpuLines"]
            window_report(mark["name"] + (f" (screen {mark.get('startState')} -> {mark.get('endState')})" if "startState" in mark else ""), present[ps + 1:pe], gpu[gs + 1:ge], mark["startS"], mark["endS"], machine)
        if show_all or not end.get("marks"):
            window_report("whole row", present, gpu, 0, 1e9, machine)
        if "--states" in sys.argv:
            state_report(directory, present)


if __name__ == "__main__":
    main()
