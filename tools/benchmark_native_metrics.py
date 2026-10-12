"""Compare prepared native advance callbacks with DirectWrite's batched API."""
import argparse
import json
import statistics
import threading
from pathlib import Path

from benchmark_native_text_performance import identity
from verify_native_text import run_check


def run(args):
    args.output_dir.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parent.parent
    fonts = [(p, w, "gvar") for p in sorted(args.fixtures.glob("*.ttf")) for w in (400, 600, 800)]
    if not fonts:
        raise ValueError("No phantom-metric fixtures")
    fonts += [(Path("C:/Windows/Fonts") / name, weight, kind) for name, weight, kind in (
        ("segoeui.ttf", 400, "static"), ("SitkaVF.ttf", 400, "HVAR"),
        ("bahnschrift.ttf", 400, "HVAR"), ("bahnschrift.ttf", 700, "HVAR"))]
    samples = []
    for font, weight, kind in fonts:
        for trial in range(args.runs):
            pair = {}
            for side in (("native", "dwrite") if trial % 2 == 0 else ("dwrite", "native")):
                command = ([args.native, font, weight, "metric-cost"] if side == "native" else
                           [args.reference, font, "61", "default", 1148846080, 0, 0, "en-US", "metric-cost", weight])
                name = f"{font.stem}-{weight}-{trial}-{side}"
                execution = run_check(name, list(map(str, command)), root, args.output_dir, 10, threading.Event())
                rows = [json.loads(line) for line in Path(execution["log"]).read_text(encoding="utf-8").splitlines()
                        if line.startswith('{"kind":"metric_cost"')]
                assert [r["phase"] for r in rows] == [0, 1, 2, 3]
                pair[side] = rows
                samples.append(dict(font=identity(font), weight=weight, kind=kind, trial=trial, side=side,
                                    execution=execution, rows=rows))
            for n, d in zip(pair["native"], pair["dwrite"]):
                assert n["batch"] == d["batch"] and n["iterations"] == d["iterations"]
                assert n["advances"] == d["advances"], (font, weight, n, d)
    summary = []
    for font, weight, kind in fonts:
        row = dict(font=font.name, weight=weight, kind=kind)
        for side in ("native", "dwrite"):
            trials = [s for s in samples if s["font"]["path"] == str(font) and s["weight"] == weight and s["side"] == side]
            phases = []
            for phase in range(4):
                data = [s["rows"][phase] for s in trials]
                phases.append(dict(batch=data[0]["batch"],
                                   us=statistics.median(r["ticks"] * 1e6 / r["frequency"] / r["iterations"] for r in data),
                                   cycles=statistics.median(r["cycles"] / r["iterations"] for r in data)))
            row[side] = phases
        summary.append(row)
    report = dict(status="passed", scope="First 256-request batch after face/plan preparation, then 4096 repeated batches of 1, 16 and 256 glyphs. Glyph IDs cycle from 1 through each face. No font/file cache flush. Excludes face/plan preparation, shaping, rasterization and drawing. Synthetic gvar fixtures contain five glyphs each; installed static/HVAR fonts are controls.",
                  runs=args.runs, executables=dict(native=identity(args.native), dwrite=identity(args.reference)),
                  summary=summary, samples=samples)
    (args.output_dir / "comparison.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Passed {len(samples)} workers and {len(samples) // 2 * 529} advance comparisons")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--native", type=Path, default=Path("D:/MaiCord-build/Release/MaiCordRunPlanTests.exe"))
    parser.add_argument("--reference", type=Path, default=Path("D:/MaiCord-build/Release/MaiCordFeaturePlanReference.exe"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--runs", type=int, choices=range(1, 32), default=5)
    run(parser.parse_args())
