"""Summarize all rounds; report regressions as measurements, never filter them."""
import argparse
import csv
import hashlib
import json
import math
import statistics
from pathlib import Path


def sha256(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--rounds", type=int, default=6)
    args = parser.parse_args()
    rows = []
    for corpus in sorted(args.directory.glob("*-glyphs.txt")):
        name = corpus.name.removesuffix("-glyphs.txt")
        inputs = [line.split() for line in corpus.read_text().splitlines()]
        if not inputs or any(len(fields) != 2 for fields in inputs):
            raise ValueError(f"{corpus}: invalid corpus")
        sizes = sorted({int(fields[1]) for fields in inputs})
        count = len(inputs)
        grouped = {"existing": [], "native": []}
        run_rows, identities = [], []
        for run in range(1, args.runs + 1):
            path = args.directory / f"{name}-{run}.csv"
            data = list(csv.DictReader(path.open()))
            if len(data) != args.rounds * 2:
                raise ValueError(f"{path}: incomplete rounds")
            for position, row in enumerate(data):
                round_index, order = divmod(position, 2)
                mode = "native" if (round_index + order) % 2 else "existing"
                if (int(row["round"]) != round_index or int(row["order"]) != order
                        or row["provider"] != mode or int(row["glyphs"]) != count):
                    raise ValueError(f"{path}: unexpected benchmark order or corpus")
                for key in ("seconds", "p50_us", "p95_us", "p99_us"):
                    value = float(row[key])
                    if not math.isfinite(value) or value < 0 or (key == "seconds" and value == 0):
                        raise ValueError(f"{path}: invalid timing")
                grouped[mode].append(row)
            medians = {mode: statistics.median(float(row["seconds"]) for row in data if row["provider"] == mode)
                       for mode in grouped}
            run_rows.append({"run": run, "median_batch_seconds": medians,
                             "native_time_change_percent": (medians["native"] / medians["existing"] - 1) * 100})
            identities.append({"path": path.name, "sha256": sha256(path)})
        program_bytes = {int(row["program_bytes"]) for values in grouped.values() for row in values}
        if len(program_bytes) != 1:
            raise ValueError(f"{name}: providers/rounds built different program byte counts")
        providers = {}
        for mode, values in grouped.items():
            times = [float(row["seconds"]) for row in values]
            providers[mode] = {
                "batch_seconds": {"median": statistics.median(times), "min": min(times), "max": max(times)},
                "median_process_cycles": statistics.median(int(row["process_cycles"]) for row in values),
                **{f"median_{key}": statistics.median(float(row[key]) for row in values)
                   for key in ("p50_us", "p95_us", "p99_us")}}
        old, new = providers["existing"], providers["native"]
        ratio = new["batch_seconds"]["median"] / old["batch_seconds"]["median"]
        row = {"name": name, "glyphs_per_round": count, "pixels_per_em": sizes, "program_bytes_per_round": program_bytes.pop(),
               "corpus_sha256": sha256(corpus), "providers": providers, "runs": run_rows, "identities": identities,
               "native_batch_time_change_percent": (ratio - 1) * 100,
               "native_throughput_change_percent": (1 / ratio - 1) * 100,
               "native_p95_change_percent": (new["median_p95_us"] / old["median_p95_us"] - 1) * 100,
               "native_p99_change_percent": (new["median_p99_us"] / old["median_p99_us"] - 1) * 100}
        rows.append(row)
        print(f"{name}: batch {row['native_batch_time_change_percent']:+.2f}%, "
              f"p95 {row['native_p95_change_percent']:+.2f}%, p99 {row['native_p99_change_percent']:+.2f}%")
    if not rows:
        raise ValueError("No benchmark corpora")
    result = {"process_runs": args.runs, "rounds_per_process": args.rounds,
              "scope": "warm CPU paint compilation", "rows": rows}
    (args.directory / "results.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
