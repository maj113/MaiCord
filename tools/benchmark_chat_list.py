#!/usr/bin/env python3
"""Runs MaiCordChatListBenchmark (tools/build_linux.sh --benchmarks) like tools/benchmark_chat_list.ps1 does on Windows:
one process per size and repetition, medians of the numeric fields of the runs that finished (sorted, index
floor((n-1)/2)), results.json and results.md in BUI's columns.

  tools/benchmark_chat_list.py --binary ~/mc-build/MaiCordChatListBenchmark [--sizes 100,10000,1000000,10000000]
      [--reps 3] [--seconds 5] [--cap-mb 4096] [--limit-seconds 150] [--out DIR] [--label NAME]

The process needs a Wayland compositor (WAYLAND_DISPLAY, XDG_RUNTIME_DIR in the environment; WSLg's, or headless sway).
Methodology and differences from BUI's harness: tools/perf/README.md, "Chat list benchmark".
"""
import argparse
import json
import os
import subprocess
import sys


def median(values):
    ordered = sorted(values)
    return ordered[(len(ordered) - 1) // 2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--sizes", default="100,10000,1000000,10000000")
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--seconds", default="5")
    parser.add_argument("--cap-mb", default="4096")
    parser.add_argument("--limit-seconds", default="150")
    parser.add_argument("--out", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "maicord-chat-list-bench"))
    parser.add_argument("--label", default="MaiCord timeline (Linux)")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    results, raw = [], []
    alive = True
    for size in (int(s) for s in args.sizes.split(",")):
        if not alive:
            results.append({"lib": args.label, "n": size, "finished": 0, "failed": 0, "skipped": "failed at a smaller size"})
            continue
        runs, fails = [], []
        for i in range(args.reps):
            try:
                process = subprocess.run(
                    [args.binary, "--n", str(size), "--seconds", args.seconds, "--cap-mb", args.cap_mb,
                     "--limit-seconds", args.limit_seconds],
                    capture_output=True, text=True, timeout=int(args.limit_seconds) + 120)
                output, status = process.stdout, process.returncode
            except subprocess.TimeoutExpired:
                output, status = "", "timeout"
            line = next((l for l in output.splitlines() if l.startswith("{")), None)
            if not line:
                fails.append({"failed": f"no output (exit {status})"})
                print(f"{args.label} {size} run {i + 1}: no output (exit {status})", file=sys.stderr)
                continue
            run = json.loads(line)
            if run.get("failed"):
                fails.append(run)
                print(f"{args.label} {size} run {i + 1}: FAILED {run['failed']} in {run.get('stage')}", file=sys.stderr)
                continue
            runs.append(run)
            raw.append(run)
            print(f"{args.label} {size} run {i + 1}: p50 {run['frame_ms_p50']} ms, first {run['first_frame_ms']} ms",
                  file=sys.stderr)
        row = {"lib": args.label, "n": size, "finished": len(runs), "failed": len(fails), "failures": fails}
        for key, value in (runs[0].items() if runs else []):
            if isinstance(value, (int, float)) and not isinstance(value, bool) and key not in row:
                values = [r[key] for r in runs]
                row[key], row[key + "__min"], row[key + "__max"] = median(values), min(values), max(values)
        results.append(row)
        alive = bool(runs)

    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump(results, f, indent=2)
    with open(os.path.join(args.out, "results_raw.json"), "w") as f:
        json.dump(raw, f, indent=2)
    columns = [("setup_ms", "Setup ms"), ("first_frame_ms", "First frame ms"), ("frame_ms_p50", "Scroll p50 ms"),
               ("frame_ms_p95", "p95"), ("frame_ms_p99", "p99"), ("cpu_ms_per_frame", "CPU ms/frame"),
               ("main_allocs_per_frame", "Allocs/frame"), ("still_frame_ms_p50", "Repaint p50 ms"),
               ("jump_ms_p50", "Jump p50 ms"), ("ram_above_data_mb", "RAM above data MB"), ("peak_above_data_mb", "Peak MB")]
    text = "| Library | Items | Finished | " + " | ".join(c[1] for c in columns) + " |\n"
    text += "|---|---:|---:|" + "|".join("---:" for _ in columns) + "|\n"
    for row in results:
        if row.get("skipped"):
            text += f"| {row['lib']} | {row['n']:,} | skipped ({row['skipped']}) |" + " |" * len(columns) + "\n"
        elif not row["finished"]:
            failure = (row["failures"] or [{}])[0]
            text += (f"| {row['lib']} | {row['n']:,} | 0/{args.reps}: {failure.get('failed', 'failed')} in "
                     f"{failure.get('stage', '?')} |" + " |" * len(columns) + "\n")
        else:
            cells = [f"{row.get(k, 0):.0f}" if ("allocs" in k or "_mb" in k) else f"{row.get(k, 0):.2f}" for k, _ in columns]
            text += f"| {row['lib']} | {row['n']:,} | {row['finished']}/{args.reps} | " + " | ".join(cells) + " |\n"
    with open(os.path.join(args.out, "results.md"), "w") as f:
        f.write(text)
    print(text)


if __name__ == "__main__":
    main()
