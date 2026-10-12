# COLRv1 reference tools

QA tools that compare MaiCord's COLRv1 paint compiler and compute shader with
Windows (`DrawPaintGlyphRun` through Direct2D). Direct2D, DirectWrite and the C
runtime link into these tools only, never into the application.

## Build and prerequisites

Optional Release targets (`cmake/reference_tools.cmake`), enabled by configuring
the cache with `-DMAICORD_BUILD_FONT_REFERENCE=ON`:

```powershell
cmake --build <build> --config Release --target MaiCordColrReference MaiCordColrBuildBenchmark MaiCordNativeColrCost
```

`<build>` is `..\MaiCord-build\Release`. The probe and benchmark link the
shipping archives by path and skip the freestanding contract; production code is
not recompiled with other floating-point options.

The scripts need Python with fontTools (`corpus.py`, `make_fixture.py`,
`run_native_matrix.py`) and NumPy (`compare.py`). Fixture fonts are first-party
synthetic data; no system font data is redistributed.

## GPU probe: MaiCordColrReference

```
MaiCordColrReference.exe [glyphs.txt [font.ttf [mode [--weight <v>] [--width <v>] [--compare-limits]]]]
```

`glyphs.txt` (default `glyphs.txt`) has lines of `<glyph id> <em size>` (id up to
65535, em size 1-256). `font.ttf` is the absolute path of a private test font;
the default is the installed Segoe UI Emoji. The probe needs a hardware D3D11
adapter and writes `pixels.bin` and `timings.csv` to the current directory, so
run it in a scratch directory.

| Mode | Candidate image | Second image |
|---|---|---|
| none | existing app provider (DirectWrite paint reader) | Direct2D |
| `--native` | `ColrNativeSource` | Direct2D |
| `--paired-native` | existing app provider | `ColrNativeSource` |
| `--paired-app` | existing app provider | app path: native session, instance, run and renderer |

- Paired modes render both images with the unchanged app compute shader, without
  a Direct2D draw. They require equal raster origin and extent and clear each GPU
  resource to a different poison color before its bake. An empty-bounds glyph
  becomes a 1x1 transparent record, counted as empty.
- `--paired-app` only: `--weight` and `--width` (once each) set variation values;
  palette 0 and a white foreground are fixed. `--compare-limits` logs a rejected
  raster as a `LIMIT` line after checking both programs have equal bounds; kind
  `app-raster-policy` is rejection by the shared app bounds function and
  `probe-atlas` is this tool's 500-pixel usable extent. Without the flag an
  oversized reference fails the run.

`pixels.bin` records: four little-endian uint32 (glyph, em size, width, height),
then the candidate and second image as tightly packed RGBA (`width * height * 4`
bytes each). `timings.csv` has one row per glyph (program statistics, times in
microseconds).

Exit codes: 0 pass; 1 a Direct3D, DirectWrite or Direct2D call failed; 2 Segoe UI
Emoji missing; 3 paint build failed or glyph has no paint; 4 bounds differ or
reference exceeds the 500-pixel extent; 5 rasterization failed; 6 paired
placement or bounds differ; 10 bad argument or input; 11 native font or instance
preparation failed; 12 I/O error.

Limits: timings include CPU submission and GPU readback, so they are not a GPU
benchmark. Pixel equality with Direct2D is not expected (different edge
rasterizer). Paired modes use one allocation at (2,2) in a 512-square atlas and
do not test atlas eviction or compositing into a window.

### Compare against Direct2D

In a scratch directory (`<repo>` is the repository root):

```powershell
python <repo>/tools/colr_reference/corpus.py --sizes 32
MaiCordColrReference.exe glyphs.txt
python <repo>/tools/colr_reference/compare.py pixels.bin --output comparison.json
```

`corpus.py` writes `glyphs.txt` for every COLRv1 base glyph of the installed
Segoe UI Emoji; `--font`, `--sizes` (comma-separated) and `--output` select
another font, sizes and file. `make_fixture.py` writes `paint-tests.ttf`, a
matching glyph list (`glyphs-synthetic.txt`) and a case-name map
(`synthetic-cases.json`) to the current directory; run the probe with that list
and the absolute font path.

`compare.py [pixels.bin] [--output comparison.json] [--baseline <old pixels.bin>]
[--expected-records <count>] [--require-exact]` reports per-record and aggregate
premultiplied RGBA differences. `--baseline` compares against an earlier
candidate instead of the second image; geometry and record counts must match.
`--require-exact` exits 1 if any channel differs. Truncated or empty input,
mismatched geometry and a wrong record count raise an error;
`python tools/colr_reference/test_compare.py` checks these cases.

## Matrix runner: run_native_matrix.py

Runs the probe in batches and requires zero changed RGBA channels.

```powershell
python tools/colr_reference/run_native_matrix.py --executable <ColrReference.exe> --fixtures <font fixture directory> --output <scratch directory> --paired
```

The fixture directory holds `paint-tests.ttf` (from `make_fixture.py`) and six
fonts from `tools/font_reference/make_color_fixture.py <name>.ttf [flags]`:
`all-formats` (no flag), `variation-stress` (`--variation-stress`),
`empty-index-map` (`--empty-index-map`), `avar-remap` (`--avar-remap`),
`avar-stress` (`--variation-stress --avar-remap`) and `map-last-active`
(`--variation-stress --map-last-active`). The runner also covers every COLRv1
base glyph of the installed emoji font (`--emoji` selects another).

- `--output` must be a new directory. `--font <path>` (repeatable) replaces
  `--fixtures`: only those fonts run, with distinct file stems.
- `--sizes` (default `12,16,20,24,32,48,64,128`) and `--emoji-sizes` are integers
  1-256; `--batch-size` is 1-256 records per probe child (default 128).
- `--paired` runs `--paired-native`. Without it the existing-provider and
  `--native` probes run separately and their pixels are compared; their headers
  hold no raster origin, so placement is not checked.
- `--app-instances` (needs `--paired`) runs `--paired-app` at each variable font's
  default instance plus a fixed grid of `wght`/`wdth` settings between the axis
  minimum and maximum; other axes are rejected. Static fonts run once per size.
- `--compare-limits` (needs `--app-instances`, no `--baseline`) is passed to the
  probe.
- `--baseline <dir>` also requires pixels exactly equal to an earlier run with the
  same corpus, sizes and batch size.

Each child has a fixed five-second timeout and runs sequentially; a failed or
timed-out child fails the run. Output goes to
`<output>/<font>[/<instance>]/<first-record>/<mode>/` (`probe.log`, `pixels.bin`,
`timings.csv`, plus `glyphs.txt` and `comparison.json` one level up).
`results.json` holds progress, final totals and SHA-256 of the executable, fonts,
corpus and readbacks; `status` is `running`, `failed`, `passed` or
`completed-with-unrendered-records`. Records rejected under `--compare-limits`
are listed under `unrendered` and never count as pixel passes. Each empty record
(`empty_records`) adds four channels to `channels`; subtract them to get rendered
channels. Palettes other than 0 and app atlas placement are not covered.

```powershell
python tools/colr_reference/run_native_matrix.py --executable D:/MaiCord-build/Release/MaiCordColrReference.exe --fixtures <fixtures> --output <new-output> --paired --sizes 12,32,128 --emoji-sizes 32
```

```powershell
python tools/colr_reference/run_native_matrix.py --executable D:/MaiCord-build/Release/MaiCordColrReference.exe --fixtures <fixtures> --output <new-output> --paired --app-instances --compare-limits --sizes 12,32,128 --emoji-sizes 32
```

To test paint and outline variation together, generate a font with curved,
simple and nested composite gvar outlines and phantom-point deltas
(`--variation-stress` adds the second axis, `--avar-remap` adds remapping) and
pass it with `--font`:

```powershell
python tools/font_reference/make_color_fixture.py <font.ttf> --outline-variation
```

## Program equality: MaiCordNativeColrCost

```powershell
MaiCordNativeColrCost.exe <font.ttf> --program-work
```

Builds every COLRv1 base glyph at 32 px with the native source and the Windows
reader and compares the paint programs. Prints one JSON line (program bytes, node
counts, arena commitment); exits 0 when all programs are equal, otherwise 1 with
`different glyph <id>`. `--program-work` is the default. `--program-cost <n>`
instead times six rounds over a 128-glyph cohort from base-glyph index `n`, and
`--program-stages <n>` profiles that cohort by build stage. Not in the CTest
suite.

## CPU paint-build benchmark: MaiCordColrBuildBenchmark

```
MaiCordColrBuildBenchmark <font.ttf> <glyphs.txt> <rounds> <results.csv>
```

`rounds` is 2-100. Times the complete per-glyph paint build (including root
selection) for the existing and native providers, without a GPU device. Each
provider has its own reusable program arena and one full warmup, and the order
alternates by round. Warmup, font mapping, scratch allocation and percentile
sorting are outside the timing; QPC sampling and byte accounting are inside it.

`results.csv` columns: `round, order, provider, glyphs, seconds, process_cycles,
program_bytes, p50_us, p95_us, p99_us`. Exit codes: 0 success; 1 bad arguments or
files; 2 build failed; 3 cycle counter failed. Run several fresh processes after
pixel testing has finished, then summarize:

```powershell
python tools/colr_reference/summarize_benchmark.py <directory> [--runs 3] [--rounds 6]
```

It validates `<name>-glyphs.txt` with `<name>-1.csv` through `<name>-<runs>.csv`
(`--rounds` alternating rounds each), prints the native change in batch time, p95
and p99 per corpus, and writes `<directory>/results.json`. Regressions are
reported, not filtered.

Limit: a warm paint-compilation microbenchmark only (not shaping, cache-hit
rendering, cold start, whole-app latency, GPU cost, memory or shipping size).
