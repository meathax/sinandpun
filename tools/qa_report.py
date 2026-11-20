#!/usr/bin/env python3
"""Summarize and gate one build/qa/<name> run without launching the game."""

import argparse
import csv
import json
import re
import sys
from pathlib import Path


PERF_RE = re.compile(
    r"\[perf\] fps=([\d.]+) guest=([\d.]+).*?speed=([\d.]+) vi_time=([\d.]+)"
    r".*?cpu=(\d+)%.*?ws=(\d+)MB"
)
UPSCALE_RE = re.compile(
    r"\[perf-upscale\].*?active=(\S+).*?internal=(\d+)x(\d+) output=(\d+)x(\d+)"
)
BAD_RE = re.compile(
    r"FATAL|Runtime error|not translated|Access violation|unhandled|"
    r"\babort\b|OSFatal|crash|sound-slot-guard|DANGLING",
    re.I,
)
EVENT_RE = re.compile(
    r"autostage\].*(?:F state|stage \d+ load reached|sequence:|watcher done)|"
    r"RSOLinkListFixed|RSOUnLinkList|scene=.*(?:GAMEOVER|RESULT|CONT|SAVE)|"
    r"gover\.brstm off=0x0 len=0x40$|sound-slot|DANGLING",
    re.I,
)
STAGE_RE = re.compile(r"stage (\d+) load reached")


def read_text(path):
    data = Path(path).read_bytes()
    if data.startswith((b"\xff\xfe", b"\xfe\xff")) or b"\x00" in data[:200]:
        return data.decode("utf-16", errors="replace")
    return data.decode("utf-8", errors="replace")


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, int(len(ordered) * fraction))
    return ordered[index]


def parse_log_sizes(directory):
    path = directory / "log_growth.csv"
    if not path.exists():
        return []
    rows = []
    try:
        with path.open(newline="", encoding="utf-8-sig") as stream:
            for row in csv.DictReader(stream):
                rows.append({key: int(float(value)) for key, value in row.items()})
    except (OSError, ValueError, TypeError):
        return []
    return rows


def analyze(directory):
    directory = Path(directory)
    logs = []
    for name in ("err.log", "out.log"):
        path = directory / name
        if path.exists():
            logs.extend(read_text(path).splitlines())

    perf = []
    upscales = []
    errors = []
    events = []
    stages = []
    underruns = 0
    for line in logs:
        match = PERF_RE.search(line)
        if match:
            perf.append({
                "present": float(match.group(1)),
                "guest": float(match.group(2)),
                "speed": float(match.group(3)),
                "vi_time": float(match.group(4)),
                "cpu": int(match.group(5)),
                "ws_mb": int(match.group(6)),
            })
        match = UPSCALE_RE.search(line)
        if match:
            upscales.append({
                "active": match.group(1),
                "internal": (int(match.group(2)), int(match.group(3))),
                "output": (int(match.group(4)), int(match.group(5))),
            })
        if re.search(r"audio-dbg.*underrun", line, re.I):
            underruns += 1
        match = STAGE_RE.search(line)
        if match:
            stages.append(int(match.group(1)))
        is_bad = bool(BAD_RE.search(line) and "PARKING" not in line)
        if is_bad:
            errors.append(line[:240])
        elif EVENT_RE.search(line):
            events.append(line[:220])

    summary_text = ""
    summary = directory / "summary.txt"
    if summary.exists():
        summary_text = read_text(summary)
    duration_match = re.search(r"elapsed\s+(\d+)s", summary_text)
    duration = int(duration_match.group(1)) if duration_match else 0

    log_bytes = sum(path.stat().st_size for path in directory.glob("*.log"))
    log_lines = len(logs)
    growth = parse_log_sizes(directory)
    max_growth_rate = 0.0
    for before, after in zip(growth, growth[1:]):
        seconds = after["elapsed_s"] - before["elapsed_s"]
        if seconds <= 0:
            continue
        delta = (
            after["err_bytes"] + after["out_bytes"] + after["runtime_bytes"]
            - before["err_bytes"] - before["out_bytes"] - before["runtime_bytes"]
        )
        max_growth_rate = max(max_growth_rate, max(0, delta) * 60.0 / seconds / (1024 * 1024))

    guests = [row["guest"] for row in perf]
    working_sets = [row["ws_mb"] for row in perf]
    return {
        "directory": str(directory),
        "duration_s": duration,
        "log_bytes": log_bytes,
        "log_lines": log_lines,
        "growth_samples": len(growth),
        "max_growth_mib_per_min": max_growth_rate,
        "perf_samples": len(perf),
        "guest_fps_min": min(guests) if guests else 0.0,
        "guest_fps_p1": percentile(guests, 0.01),
        "guest_fps_median": percentile(guests, 0.5),
        "guest_fps_avg": sum(guests) / len(guests) if guests else 0.0,
        "guest_fps_below_55": sum(value < 55.0 for value in guests),
        "working_set_first_mb": working_sets[0] if working_sets else 0,
        "working_set_last_mb": working_sets[-1] if working_sets else 0,
        "working_set_max_mb": max(working_sets) if working_sets else 0,
        "speed_min": min((row["speed"] for row in perf), default=0.0),
        "vi_time_min": min((row["vi_time"] for row in perf), default=0.0),
        "vi_time_max": max((row["vi_time"] for row in perf), default=0.0),
        "upscales": upscales,
        "audio_underruns": underruns,
        "stages_reached": stages,
        "errors": errors,
        "events": events,
    }


def validate(report, args):
    failures = []
    if args.sequence:
        expected = [int(value) for value in args.sequence.split(",")]
        actual = report["stages_reached"]
        position = 0
        for stage in actual:
            if position < len(expected) and stage == expected[position]:
                position += 1
        if position != len(expected):
            failures.append(f"stage sequence {expected} not reached in order (saw {actual})")
    if args.require_resolution:
        width, height = (int(value) for value in args.require_resolution.lower().split("x", 1))
        if not any(row["output"] == (width, height) for row in report["upscales"]):
            failures.append(f"output resolution {width}x{height} was not logged")
    if args.min_perf_samples and report["perf_samples"] < args.min_perf_samples:
        failures.append(f"only {report['perf_samples']} perf samples; need {args.min_perf_samples}")
    if args.min_guest_avg and report["guest_fps_avg"] < args.min_guest_avg:
        failures.append(f"average guest fps {report['guest_fps_avg']:.2f} is below {args.min_guest_avg:.2f}")
    if args.max_growth_mib_per_min and report["max_growth_mib_per_min"] > args.max_growth_mib_per_min:
        failures.append(
            f"log growth {report['max_growth_mib_per_min']:.2f} MiB/min exceeds "
            f"{args.max_growth_mib_per_min:.2f}"
        )
    if args.max_growth_mib_per_min and report["growth_samples"] < 2:
        failures.append("log growth evidence is missing (need at least two samples)")
    if args.no_errors and report["errors"]:
        failures.append(f"{len(report['errors'])} fatal/error markers")
    return failures


def print_report(report, event_limit):
    print(
        f"{report['directory']}: logs={report['log_bytes']} bytes/{report['log_lines']} lines, "
        f"duration={report['duration_s']}s, perf samples={report['perf_samples']}, "
        f"ws first/last/max={report['working_set_first_mb']}/{report['working_set_last_mb']}/"
        f"{report['working_set_max_mb']} MB, underruns={report['audio_underruns']}"
    )
    if report["perf_samples"]:
        print(
            f"guest fps min={report['guest_fps_min']:.1f} p1={report['guest_fps_p1']:.1f} "
            f"median={report['guest_fps_median']:.1f} avg={report['guest_fps_avg']:.1f}; "
            f"seconds<55: {report['guest_fps_below_55']}; speed min={report['speed_min']:.3f}; "
            f"vi_time={report['vi_time_min']:.3f}..{report['vi_time_max']:.3f}"
        )
    print(
        f"stages reached={report['stages_reached']}; log growth samples={report['growth_samples']} "
        f"max={report['max_growth_mib_per_min']:.2f} MiB/min"
    )
    if report["upscales"]:
        row = report["upscales"][-1]
        print(
            f"upscale active={row['active']} internal={row['internal'][0]}x{row['internal'][1]} "
            f"output={row['output'][0]}x{row['output'][1]}"
        )
    for error in report["errors"][:event_limit]:
        print("!! " + error)
    for event in report["events"][:event_limit]:
        print(event)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", help="QA run directory, or a name below build/qa")
    parser.add_argument("event_limit", nargs="?", type=int, default=60)
    parser.add_argument("--sequence", help="require stage load order, for example 1,2,1")
    parser.add_argument("--require-resolution", help="require logged output WxH, for example 3840x2160")
    parser.add_argument("--min-perf-samples", type=int, default=0)
    parser.add_argument("--min-guest-avg", type=float, default=0.0)
    parser.add_argument("--max-growth-mib-per-min", type=float, default=0.0)
    parser.add_argument("--no-errors", action="store_true")
    parser.add_argument("--json-out", help="write the machine-readable report")
    args = parser.parse_args()

    path = Path(args.directory)
    directory = path if path.is_dir() else Path("build/qa") / args.directory
    if not directory.is_dir():
        parser.error(f"QA run directory not found: {directory}")
    report = analyze(directory)
    failures = validate(report, args)
    print_report(report, args.event_limit)
    if args.json_out:
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
