# Native COLRv1 reference comparison

The `MaiCordColrReference` QA executable compares MaiCord's paint compiler and
compute shader with Windows `DrawPaintGlyphRun`. Direct2D and the C runtime are
reference-tool dependencies only; they are never added to the application.

`MaiCordColrReference` and `MaiCordColrBuildBenchmark` are optional targets of
the Release build configured with `MAICORD_BUILD_FONT_REFERENCE=ON`
(`cmake/reference_tools.cmake`).
They link the actual renderer, text, native-font, native-paint and core libraries,
including generated shaders. They do not recompile production code with different
floating-point options.

In a scratch output directory:

1. Run `corpus.py --sizes 32` to generate `glyphs.txt` for every COLRv1 base glyph in the installed Segoe UI Emoji font. Optional `--font`, `--sizes` and `--output` arguments select another font or sizes.
2. Run `ColrReference.exe <absolute path to glyphs.txt>`. An optional second argument supplies an absolute private test-font path.
3. Run `compare.py pixels.bin --output comparison.json` to report per-glyph and aggregate premultiplied RGBA differences.

`make_fixture.py` creates 56 synthetic COLRv1 paint cases, `paint-tests.ttf`, a matching glyph list and case-name map. These outlines and paint descriptions are first-party test data. Run the reference executable with the generated list and the absolute test-font path as its two arguments.

Corpus/fixture generation needs Python fontTools; comparison needs NumPy. Neither tool redistributes system font data. Output is `pixels.bin` (four little-endian uint32 fields: glyph, integer em size, width, height; followed by candidate then reference tightly packed RGBA pixels) and `timings.csv`. The log identifies the providers, adapter and driver. Timings include CPU submission and GPU readback, so they are not a pure GPU benchmark. Pixel-perfect equality is not expected against Direct2D's different edge rasterizer.

Use `compare.py <new pixels.bin> --baseline <old pixels.bin>` for exact candidate-to-candidate regression checks. Geometry and record counts must match.

## Native provider and paired GPU gate

Pass `--native` as the third argument after the corpus and explicit font path to
replace the candidate paint/outline provider with `ColrNativeSource`. The source
opens mapped font views and default-instance scalar storage once, then reuses
caller-owned scratch and the program arena. DirectWrite remains solely in the
old-provider/Direct2D QA branches. No new library is linked by the application.

Pass `--paired-native` instead to write existing-app RGBA followed by first-party
RGBA in each record. Both execute the unchanged application compute shader.
The probe checks identical raster origin and extent before rendering the
second image. Separate GPU resources are cleared to different poison colors
before each bake, so an unwritten texel cannot pass by retaining its counterpart's
pixels. This mode omits Direct2D draw/readback, but this QA executable still
initializes Direct2D. It uses one allocation at (2,2) in a 512-square atlas and
does not test the full app's atlas eviction or compositing into a window.

`run_native_matrix.py --executable <ColrReference.exe> --fixtures <font fixture directory>
--output <scratch directory> --paired` runs the seven deterministic paint/variation
fixtures and every installed Segoe UI Emoji COLRv1 base glyph at eight integer
sizes. Generate the fixtures using `../font_reference/make_color_fixture.py`;
the fixture filenames match the native paint-source evidence. Default instances
are tested here; varied paint values were checked separately in the program
oracle. `--sizes` and `--emoji` select an explicit size list/font.

The runner splits every selected record into batches of 128 by default, with a
fixed **five-second timeout per GPU child**. `--batch-size` accepts 1-256;
failed/timed-out children remain failed evidence. Runs are sequential to avoid
contending GPU workloads. Capture logs, readbacks and comparisons live under
`<output>/<font>/<first-record>/<mode>/`. The top-level `results.json` records
partial progress and the completed totals. Use a new output directory each time.

For quick ABI/shader development, explicitly select three synthetic sizes and
one complete installed-emoji size:

```powershell
python tools/colr_reference/run_native_matrix.py --executable D:/MaiCord-build/Release/MaiCordColrReference.exe --fixtures <fixtures> --output <new-output> --paired --sizes 12,32,128 --emoji-sizes 32
```

Add `--baseline <previous-output>` to require exact before/after pixels as well
as paired native/Windows-provider equality. Keep the same sizes and batch size.
Without the size options, the original full eight-size matrix remains available;
batching does not drop cases. That default-only run excludes active instances;
the actual-app mode below covers selected weight/width instances. Broader palette
and full-app atlas-placement gates remain separate.

## Actual app instances

`--paired-app` selects native session, instance, run and renderer ownership for
the second image. Unlike `--paired-native`, it does not construct a standalone
paint-source owner. Add `--weight <value>` and/or `--width <value>` after that
mode to create matching Windows and native instances. The settings are converted
to the same float values for both providers. Palette zero and white foreground
remain explicit probe inputs; the app currently uses palette zero.

The matrix runner's `--app-instances --paired` includes default settings, selected
weight/width extrema, intermediate values and avar breakpoints for the supplied
variable fixtures. Static fonts, including installed emoji, run once per size.
Every selected glyph/size remains accounted for with a five-second child cap.

```powershell
python tools/colr_reference/run_native_matrix.py --executable D:/MaiCord-build/Release/MaiCordColrReference.exe --fixtures <fixtures> --output <new-output> --paired --app-instances --compare-limits --sizes 12,32,128 --emoji-sizes 32
```

By default an oversized reference fails the probe. Explicit `--compare-limits`
instead requires successful compilation and exactly equal bounds, then records
the unrendered case separately. `app-raster-policy` identifies rejection by the
shared application bounds function; `probe-atlas` identifies this tool's
500-pixel usable extent. Neither counts as a pixel pass. A run with such records
returns `completed-with-unrendered-records`, not `passed`; inspect `unrendered`
and the totals. This option requires app-instance mode and does not support a
saved-pixel baseline. Corpus multisets must equal pixel plus unrendered records.

When both paired programs have no bounds, the probe emits a canonical one-texel
transparent record and counts it in `empty_records`. These records verify equal
empty output, not a GPU draw. Subtract their four channels each from `channels`
when reporting actual rendered channels. All ordinary records still compare
actual readbacks, origins and extents with poisoned GPU resources.

The coordinate-only checkpoint covers 8,328 selected records: 7,635 rendered
exactly, 540 equal empty outputs and 153 equal application-policy rejections.

To exercise paint and outline variation together, generate a first-party font
with `../font_reference/make_color_fixture.py <font.ttf> --outline-variation`.
Optional `--variation-stress` adds the second axis; `--avar-remap` adds remapping.
The new outlines include quadratic curves, transformed/nested components and
phantom-point deltas. The ordinary fixture generator output is unchanged.

Use one or more `--font <path>` arguments **instead of** `--fixtures` for an
explicit corpus. Only those fonts run; the standard emoji corpus is not added.
Combine with `--paired --app-instances --compare-limits --sizes 12,32,128` for
the selected instance grid and separately reported raster rejections. Explicit
font paths must have distinct file stems. The recorded combined-variation run
covers 3,384 records in 5.39 seconds without rebuilding the app.

`MaiCordNativeColrCost.exe <font.ttf> --program-work` performs a single full-corpus
native/Windows paint-program equality pass, reporting bytes, node counts and
arena commitment without the six timed benchmark rounds. On the current emoji
font it takes about 0.46 seconds under the two-second correctness deadline.

The runner requires the expected number of records and zero changed RGBA
channels. It records per-glyph results and SHA-256 identities for fonts, corpus,
executable and readbacks. `compare.py --require-exact --expected-records <count>`
also exposes this gate directly; truncation, empty results, mismatched geometry
and missing records fail explicitly. `test_compare.py` exercises those failures.
Separate-process mode remains available by omitting `--paired`, but its binary
headers do not store raster origins; use the paired probe for placement proof.

## CPU paint-build benchmark

`MaiCordColrBuildBenchmark <font.ttf> <glyphs.txt> <rounds> <results.csv>` measures only
the complete per-glyph paint build (including root selection). It links the
same existing/native implementations as the GPU probe, without creating a GPU
device. Each provider keeps its own reusable program arena, receives one full
warmup, and alternates execution order by round. Both font owners stay alive.
Mapping/font construction, scratch allocation, warmup and percentile sorting
are outside measurement. QPC sampling and byte accounting are included in
batch wall time; process cycles include the measurement loop.

Run multiple fresh processes after pixel testing has finished. Results report
batch time, process cycles, used program bytes and per-glyph p50/p95/p99 build
time. This is a warm paint-compilation microbenchmark, not shaping, cache-hit
rendering, cold start, whole-app latency, GPU cost, RAM/VRAM or shipping size.
The QA owner's maximum-capacity buffers are not a production memory policy.
`summarize_benchmark.py <directory>` validates and aggregates `<name>-glyphs.txt`
with `<name>-1.csv` through `<name>-3.csv`, six alternating rounds each by
default. It reports all measured regressions as well as improvements.

`../measure_emoji_scroll.ps1` runs the full native picker catalog with a fresh process/atlas per run, alternating an optional baseline executable. It records per-page layout/submission times and whole-probe peak process memory under `MaiCord-build/qa`. Driver and OS caches are left alone.
