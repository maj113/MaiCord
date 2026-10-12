"""Require exact app RGBA equality, including bounds/placement in paired mode."""
import argparse
from collections import Counter
import hashlib
import json
import re
import sys
import threading
import time
from pathlib import Path

from fontTools.ttLib import TTFont
from compare import compare

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_native_text import run_check, CheckFailure


def sha256(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--fixtures", type=Path)
    parser.add_argument("--font", action="append", type=Path, help="Explicit font corpus (repeatable), instead of the standard fixture/emoji matrix")
    parser.add_argument("--emoji", default="C:/Windows/Fonts/seguiemj.ttf", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sizes", default="12,16,20,24,32,48,64,128")
    parser.add_argument("--paired", action="store_true", help="Render both app providers directly, omitting Direct2D draws")
    parser.add_argument("--app-instances", action="store_true", help="Use actual native app ownership and selected weight/width instances; requires --paired")
    parser.add_argument("--compare-limits", action="store_true", help="Record equal oversized bounds separately; unrendered cases do not become pixel passes")
    parser.add_argument("--emoji-sizes", help="Optional separate size list for the installed emoji corpus")
    parser.add_argument("--batch-size", type=int, default=128, help="1-256 records per five-second GPU child")
    parser.add_argument("--baseline", type=Path, help="Prior run with identical corpus, for before/after RGBA equality")
    args = parser.parse_args()
    executable = args.executable.resolve()
    def parse_sizes(value):
        try:
            sizes = [int(item) for item in value.split(",")]
        except ValueError:
            parser.error("Sizes must be integers between 1 and 256")
        if not sizes or any(size < 1 or size > 256 for size in sizes):
            parser.error("Sizes must be integers between 1 and 256")
        return sizes
    sizes = parse_sizes(args.sizes)
    emoji_sizes = parse_sizes(args.emoji_sizes) if args.emoji_sizes else sizes
    if not 1 <= args.batch_size <= 256:
        parser.error("Batch size must be 1-256")
    if args.app_instances and not args.paired:
        parser.error("--app-instances requires --paired")
    if args.compare_limits and (not args.app_instances or args.baseline):
        parser.error("--compare-limits requires --app-instances and a run without --baseline")
    names = ["paint-tests", "all-formats", "variation-stress", "empty-index-map",
             "avar-remap", "avar-stress", "map-last-active"]
    if bool(args.fixtures) == bool(args.font):
        parser.error("Select --fixtures or one or more --font paths")
    if args.font:
        fonts = [(path.stem, path.resolve()) for path in args.font]
        if len({name for name, _ in fonts}) != len(fonts):
            parser.error("Explicit fonts need distinct file stems")
    else:
        fonts = [(name, (args.fixtures / f"{name}.ttf").resolve()) for name in names]
        fonts.append(("emoji", args.emoji.resolve()))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "results.json").exists():
        parser.error("Choose a new output directory")
    rows = []
    report = dict(status="running", executable=str(executable), executable_sha256=sha256(executable),
                  sizes=sizes, emoji_sizes=emoji_sizes, instance="weight-width-grid" if args.app_instances else "default", paired=args.paired,
                  native_provider="app-run-instance-renderer" if args.app_instances else "paint-source",
                  compare_limits=args.compare_limits,
                  batch_size=args.batch_size, child_timeout_seconds=5, rows=rows)
    def save():
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    start = time.perf_counter()
    save()
    try:
        jobs = []
        for name, path in fonts:
            with TTFont(path) as font:
                glyphs = sorted({font.getGlyphID(item.BaseGlyph)
                                for item in font["COLR"].table.BaseGlyphList.BaseGlyphPaintRecord})
                axes = {axis.axisTag: (axis.minValue, axis.defaultValue, axis.maxValue)
                        for axis in font["fvar"].axes} if "fvar" in font else {}
            profiles = [("default", [])]
            if args.app_instances and axes:
                if set(axes) - {"wght", "wdth"}:
                    raise ValueError(f"{name}: unexpected app-instance axis")
                grid = [(-1, -1), (-1, 1), (1, -1), (1, 1), (-.75, -.5), (.25, .5), (.5, -.5), (0, 1), (0, -1)] if len(axes) == 2 else [(-1, -1), (-.75, -.75), (.25, .25), (.5, .5), (1, 1)]
                for index, point in enumerate(grid):
                    arguments = []
                    for tag, flag, coordinate in (("wght", "--weight", point[0]), ("wdth", "--width", point[1])):
                        if tag not in axes:
                            continue
                        low, default, high = axes[tag]
                        value = default + coordinate * (default - low if coordinate < 0 else high - default)
                        arguments.extend([flag, format(value, ".9g")])
                    profiles.append((f"instance-{index}", arguments))
            jobs.extend((name, path, glyphs, profile, arguments) for profile, arguments in profiles)
        for name, path, glyphs, profile, arguments in jobs:
            records = [(glyph, size) for size in (emoji_sizes if name == "emoji" else sizes) for glyph in glyphs]
            if not records:
                raise ValueError(f"{name}: empty corpus")
            for first in range(0, len(records), args.batch_size):
                batch = records[first:first + args.batch_size]
                relative = Path(name) / profile / str(first) if args.app_instances else Path(name) / str(first)
                directory = output / relative
                directory.mkdir(parents=True)
                corpus = directory / "glyphs.txt"
                corpus.write_text("".join(f"{glyph} {size}\n" for glyph, size in batch))
                row = dict(name=name, profile=profile, axis_arguments=arguments, palette=0, first=first, records=len(batch), font=str(path),
                           font_sha256=sha256(path), corpus_sha256=sha256(corpus), processes=[])
                rows.append(row)
                save()
                for mode in (("paired",) if args.paired else ("existing", "native")):
                    run = directory / mode
                    run.mkdir()
                    command = [str(executable), str(corpus), str(path)]
                    if mode == "native":
                        command.append("--native")
                    elif mode == "paired":
                        command.append("--paired-app" if args.app_instances else "--paired-native")
                        command.extend(arguments)
                        if args.compare_limits:
                            command.append("--compare-limits")
                    try:
                        process = run_check("probe", command, run, run, 5, threading.Event())
                    except CheckFailure as error:
                        row["processes"].append(error.row)
                        raise
                    row["processes"].append(process)
                mode = "paired" if args.paired else "native"
                pixels = directory / mode / "pixels.bin"
                log = Path(row["processes"][-1]["log"]).read_text(encoding="utf-8", errors="replace")
                limits = [json.loads(line[6:]) for line in log.splitlines() if line.startswith("LIMIT ")]
                if limits and not args.compare_limits:
                    raise ValueError("Unexpected unrendered records")
                if any(item.get("exact_bounds") is not True or item.get("kind") not in ("app-raster-policy", "probe-atlas") for item in limits):
                    raise ValueError("Invalid limit observation")
                row["unrendered"] = limits
                expected_pixels = len(batch) - len(limits)
                if expected_pixels < 0:
                    raise ValueError("Too many unrendered records")
                if not expected_pixels:
                    if pixels.stat().st_size:
                        raise ValueError("Unexpected pixels for fully unrendered batch")
                    result = dict(glyphs=0, channels=0, changed_channels=0, maximum_channel_delta=0, rows=[])
                else:
                    result = compare(pixels, None if args.paired else directory / "existing/pixels.bin",
                                     expected_records=expected_pixels)
                observed = Counter((item["glyph"], item["em"]) for item in result["rows"] + limits)
                if observed != Counter(batch):
                    raise ValueError("Pixel and limit records do not cover the exact corpus")
                summary = re.search(r"PASS (\d+) glyphs; input=(\d+) empty_bounds=(\d+)", log)
                if not summary or int(summary[1]) != expected_pixels or int(summary[2]) != len(batch):
                    raise ValueError("Incomplete probe summary")
                row["empty_records"] = int(summary[3]) if args.paired else 0
                (directory / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
                row.update({key: value for key, value in result.items() if key != "rows"})
                row["pixels_sha256"] = sha256(pixels)
                if result["changed_channels"]:
                    raise RuntimeError(f"{name}/{first}: {result['changed_channels']} changed RGBA channels")
                if args.baseline:
                    previous = args.baseline.resolve() / relative
                    if sha256(previous / "glyphs.txt") != row["corpus_sha256"]:
                        raise ValueError("Baseline corpus differs")
                    result = compare(pixels, previous / mode / "pixels.bin", expected_records=len(batch))
                    (directory / "baseline-comparison.json").write_text(json.dumps(result, indent=2) + "\n")
                    row["baseline"] = {key: value for key, value in result.items() if key != "rows"}
                    if result["changed_channels"]:
                        raise RuntimeError(f"{name}/{first}: baseline pixels differ")
                if sha256(executable) != report["executable_sha256"]:
                    raise RuntimeError("Executable changed during capture")
                save()
            print(f"{name}/{profile}: compared {len(records)} records; see separate pixel/limit counts", flush=True)
        report["totals"] = {key: sum(row[key] for row in rows) for key in ("glyphs", "channels", "changed_channels")}
        report["totals"]["maximum_channel_delta"] = max(row["maximum_channel_delta"] for row in rows)
        report["totals"]["unrendered_records"] = sum(len(row["unrendered"]) for row in rows)
        report["totals"]["empty_records"] = sum(row["empty_records"] for row in rows)
        report["status"] = "completed-with-unrendered-records" if report["totals"]["unrendered_records"] else "passed"
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report["seconds"] = time.perf_counter() - start
        save()
    print(json.dumps(report["totals"]), flush=True)


if __name__ == "__main__":
    main()
