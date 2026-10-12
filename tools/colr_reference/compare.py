"""Strict RGBA readback comparison. NumPy is a QA dependency only."""
import argparse
import json
import struct
from pathlib import Path

import numpy as np


def record(stream):
    header = stream.read(16)
    if not header:
        return None
    if len(header) != 16:
        raise ValueError("Truncated pixel-record header")
    glyph, em, width, height = struct.unpack("<4I", header)
    if not width or not height or width > 512 or height > 512:
        raise ValueError("Invalid pixel-record dimensions")
    size = width * height * 4
    ours, reference = stream.read(size), stream.read(size)
    if len(ours) != size or len(reference) != size:
        raise ValueError("Truncated pixel-record payload")
    return header, glyph, em, width, height, ours, reference


def compare(path, baseline_path=None, expected_records=None):
    rows = []
    total = absolute = changed = maximum = 0
    baseline = open(baseline_path, "rb") if baseline_path else None
    try:
        with open(path, "rb") as stream:
            while item := record(stream):
                header, glyph, em, width, height, ours, reference = item
                if baseline:
                    previous = record(baseline)
                    if previous is None or previous[0] != header:
                        raise ValueError("Candidate/baseline geometry or record count differs")
                    reference = previous[5]
                candidate = np.frombuffer(ours, np.uint8).astype(np.int16)
                reference = np.frombuffer(reference, np.uint8).astype(np.int16)
                delta = np.abs(candidate - reference)
                count = int(np.count_nonzero(delta))
                peak = int(delta.max())
                total += delta.size
                absolute += int(delta.sum())
                changed += count
                maximum = max(maximum, peak)
                rows.append({"glyph": glyph, "em": em, "width": width, "height": height,
                             "changed_channels": count, "mean_absolute_rgba": float(delta.mean()),
                             "maximum_channel_delta": peak})
        if baseline and record(baseline) is not None:
            raise ValueError("Candidate is missing baseline records")
    finally:
        if baseline:
            baseline.close()
    if not rows:
        raise ValueError("No pixel records were compared")
    if expected_records is not None and len(rows) != expected_records:
        raise ValueError(f"Expected {expected_records} records, found {len(rows)}")
    return {"channels": total, "changed_channels": changed, "maximum_channel_delta": maximum,
            "glyphs": len(rows), "mean_absolute_rgba": absolute / total, "rows": rows}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("pixels", nargs="?", default="pixels.bin")
    parser.add_argument("--baseline", help="Earlier app candidate pixels, instead of Direct2D")
    parser.add_argument("--output", default="comparison.json")
    parser.add_argument("--expected-records", type=int)
    parser.add_argument("--require-exact", action="store_true")
    args = parser.parse_args()
    result = compare(args.pixels, args.baseline, args.expected_records)
    Path(args.output).write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "rows"}))
    raise SystemExit(1 if args.require_exact and result["changed_channels"] else 0)
