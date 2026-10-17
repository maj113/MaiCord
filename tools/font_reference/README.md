# Font and text reference tools

QA tools that compare MaiCord's first-party font, layout and COLR code with
Windows (DirectWrite, Direct2D) and fontTools. None ships. `maicord_font` and
the native test targets link first-party libraries and `kernel32` only.

- C++ oracles (`MaiCord*Reference`) record what DirectWrite does.
- Python generators and comparers (`*_oracle.py`, `make_*.py`) build first-party
  fixture fonts, drive the oracles and write corpora for the native tests
  (`MaiCord*Tests`). They need fontTools and run from the repository root.

## Conventions

- Oracles are opt-in: configure the Release cache with
  `-DMAICORD_BUILD_FONT_REFERENCE=ON` (`cmake/reference_tools.cmake`) and build a
  target by name; `tools\build.ps1` does not build them. Test targets exist only
  with `-DMAICORD_BUILD_TESTS=ON` (`cmake/tests.cmake`), which `tools\build.ps1
  -Test` sets temporarily before removing the executables.
- `<build>` is the build directory, `<bin>` the executables' directory, `<scratch>`
  and `<output>` scratch directories. Delete generated corpora and oracle
  executables from the build directory when done.
- Registered tests (`maicord_native_*`) run their no-argument mode in the suite;
  corpus runs are manual. Check the exit code and keep `.bin` corpora, `.json`
  identities and logs together.
- A corpus is an 8-byte magic (NUL padded), a little-endian `u32` record count and
  the records; readers reject another magic (given in parentheses below).
  `--header` / `--fixture-header` write it as a C++ array under
  `tests/generated/` that the test embeds, so a no-argument run checks it.
- Oracles taking `<output>` write `<output>/observations.json` (Windows
  observations) and `<output>/corpus.bin`. For `complexity_oracle.py`,
  `nominal_scalar_oracle.py`, `nominal_variations_oracle.py`,
  `metric_variations_oracle.py` and `gvar_outline_oracle.py`, `--exe <reference>`
  runs the reference and `--header <path>` exports from the existing
  `observations.json`; either may be used alone.
- Native verifiers also run bounded-work, capacity and guard-page mutation checks.
  Each compares only the data its section names; none establishes complete
  Windows shaping, placement or pixel parity. Fixture fonts are first-party test
  data, not application assets.

## MaiCordFontReference

`main.cpp`, `program_oracle.cpp`, `dwrite_text_system.cpp` and the paint adapter
`tools/colr_reference/dwrite_paint_builder.cpp`. Compares MaiCord's SFNT, outline,
metric and COLR readers with DirectWrite in a separate process (isolated
factory). Native counterparts: `MaiCordFontTests` and `MaiCordColrProgramTests`.

```powershell
MaiCordFontReference.exe 'C:/Windows/Fonts/seguiemj.ttf'
```

For every face it compares each Unicode scalar with
`IDWriteFontFace::GetGlyphIndices`, each glyph's raw hmtx advance and bearing with
`GetDesignGlyphMetrics`, static TrueType and CFF1 outlines with the
`text_outline_glyph` provider (command structure, every float bit including signed
zero, fill rule, control bounds), CFF resolved metrics (`CffGlyphMetrics`:
charstring advance and control box in head design units, before float rounding),
and the COLR/CPAL data and paint programs below.

`MaiCordFontReference.exe <font-path> [modifiers] [mode]`. Modifiers precede the
mode and are parsed in this order:

| Modifier | Effect |
| --- | --- |
| `--simulate=N` | DirectWrite simulation mask: 1 bold, 2 oblique, 3 both |
| `--glyphs=<first>,<count>` | limit outline probes to a glyph range |
| `--cff-experiment` | with `--simulate=1` or `3` on a CFF face, compare native `cff_outline_bold`; rejects `--owned`, `--renderer` |
| `--owned` | acquire the font through the native file/face/instance store |
| `--renderer` | decode outlines through the renderer's glyph binding (`native_glyph_outline`) |
| `--points` | with `--simulate`, dump native points; rejects `--owned`, `--renderer` |
| `--instructed` | execute glyph instructions (with `--owned`, the instance prepares them) |

| Mode | Effect |
| --- | --- |
| none | all comparisons above, plus the color, variation and paint-program passes |
| `--native-source`, `--native-source-grid` | native paint-source passes |
| `--gvar400`, `--gvar600` | native variable outlines against normalized Windows outlines at that weight; skips color, cmap, metric passes |
| `--geometry` | all glyphs through `FontGeometry`/`FontGeometryInstance` against Windows normalized outlines; skips color, cmap, metric passes, keeps CFF resolved-ink checks |
| `--instructions` | prepare the native instruction context and apply glyph programs through `FontGeometry` (static translated contexts; positioning and component transforms fail) |
| `--metrics-dump` | with `--simulate`, compare native simulation of base design metrics |

`-dump` appended to `--gvar400`, `--gvar600`, `--geometry` or `--instructions`
emits outline rows; `--instructions-state=<glyph-id>` selects one simple glyph
(not composite, empty, CFF or out of range) and also emits the VM state.

Stdout is `name=value` lines ending in CRLF: counters (`scalars_compared`,
`mapping_differences`, `outline_differences`, `metric_differences`,
`color_value_hash`, `paint_program_differences`, ...) and details of the first
failures; `reference_face_status` is the HRESULT from creating the Windows face.
Dump rows carry floats as IEEE bit patterns:

- `gvar_outline=<glyph>,<commands>,<fill rule>,<has bounds>,<4 bound bits>`, then
  seven values per command: six coordinate bits and the kind (`native_outline=`
  from `--points` has the same layout).
- `native_points=<glyph>,<units/em>,<points>,<contours>,<contour ends...>`, then x
  bits, y bits and flags per point, and `native_contour_directions=<glyph>,<status>,
  <work>,<contours>,<flags...>` (`--points`).
- `reference_metrics=<glyph>,<left>,<advance width>,<right>,<top>,<advance
  height>,<bottom>,<vertical origin y>` in design units (`--metrics-dump`).
- `tt_stack=`, `tt_function=`, `tt_point=` (`--instructions-state`): parsers of
  numeric summaries ignore every `tt_` row.
- With `--owned`: `owner_files`, `owner_faces`, `owner_mapped_bytes` and their
  `_after_release` variants (must be zero), `owner_instance_reserved_bytes` and
  `owner_instance_state_bytes` (committed prepared-state bytes, excluding file
  mappings and caller-owned slot and scratch storage).

| Exit | Meaning |
| --- | --- |
| 0 | no unsupported outline face, decoding failure or difference |
| 1 | bad arguments |
| 2 | font file could not be opened (or `--owned` acquisition failed) |
| 3 | file empty or larger than `0xffffffff` bytes |
| 4, 5 | file mapping or mapped view failed |
| 6 | any difference or comparison failure, unsupported face, or a face Windows rejects |

Standing limits: differences are reported, not filtered. Raw hmtx bearings can
differ from the CFF control-box bearings DirectWrite returns, so a CFF font can
exit 6 even when the resolved comparison matches; transformed CFF ink is checked
through the outline. A face Windows rejects has a nonzero `reference_face_status`
and its zero comparisons are not a pass. Not covered: shaping, UVS (cmap format 14,
tested by synthetic native tests only) and rendered pixels.

### Native reader limits

- cmap: nominal Unicode formats 0, 4, 6, 10, 12, 13 and UVS format 14; other
  encodings and formats (format 8, symbol mappings) are unsupported. Table
  checksums are not verified. CFF2 outlines are unsupported.
- CFF1 decoder: caller-owned path buffer, operation budget, 48 operands, 32
  transient values, subroutine depth 10 (`tests/cff_outline_test.cpp`, in
  `MaiCordFontTests`, builds its own font bytes).
- `truetype_outline` (static): caller-owned buffers, work budget, component depth
  64, no hint bytecode; `tt_outline` runs instructions. Active variable-font
  instructions and `cvar` are unsupported.
- Prepared views borrow immutable mapping storage; callers keep it alive and do
  not fabricate or mutate views.

## Fixture generators

```powershell
python tools/font_reference/make_outline_fixture.py <scratch-directory>
python tools/font_reference/make_cff_fixture.py <scratch-directory>
```

- `make_outline_fixture.py` writes `outline-1000.ttf` and `outline-2048.ttf`
  (units/em in the name).
- `make_cff_fixture.py` writes `cff-canonical.otf`, `cff-rounded.otf`,
  `cff-scale.otf`, `cff-inverse.otf`, `cff-inverse2.otf` and `cff-affine.otf`
  (different FontMatrix values; Windows rejects the affine one, so the probe exits
  6 for it). `--expanded` adds flex/equivalent-cubic and stress pairs and
  `cff-small.otf`, `cff-third.otf`, `cff-scale107.otf`.
- `tools/colr_reference/make_fixture.py` generates further COLRv1 paint cases
  (`tools/colr_reference/README.md`).

## COLR, variation and axis comparisons

```powershell
MaiCordFontReference.exe <font-path> > <native-log>
python tools/font_reference/color_table_oracle.py <font-path> --native-log <native-log>
python tools/font_reference/make_color_fixture.py <scratch-directory>/all-formats.ttf
```

The probe validates each native paint graph, compares palette bytes and COLRv1
roots with DirectWrite, and emits an FNV-1a digest (`color_value_hash`) of decoded
paint fields, clip boxes and color stops (variable fields and indices stay raw):
a structural diagnostic, not a pixel comparison.

- `color_table_oracle.py <font> [--native-log <log>] [--output <json>]
  [--variations]` is the independent fontTools parser. It prints JSON and, with
  `--native-log`, exits 1 on a digest mismatch.
- `make_color_fixture.py <output.ttf>` writes a reproducible variable COLRv1 font
  (all 32 paint formats, both clip-box formats). Options: `--variation-stress`
  (second axis, interacting regions), `--empty-index-map` (an encoded empty map is
  the identity), `--avar-remap`, `--map-last-active` (requires
  `--variation-stress`; the last map entry carries real deltas) and
  `--outline-variation` (gvar outlines).

### Variation resolution

```powershell
python tools/font_reference/make_color_fixture.py <scratch>/all-formats.ttf
python tools/font_reference/make_color_fixture.py <scratch>/variation-stress.ttf --variation-stress
python tools/font_reference/make_color_fixture.py <scratch>/empty-index-map.ttf --empty-index-map
MaiCordFontReference.exe <font-path> > <native-log>
python tools/font_reference/color_table_oracle.py <font-path> --native-log <native-log> --variations
```

With a COLR ItemVariationStore the probe resolves the normalized grid -1, -.75,
-1/3, -.25, 0, .125, 1/3, .5, .875, 1: ten instances for one axis, 100 for two
(axis zero varies fastest). The QA grid supports at most two axes (the native
evaluator has no such limit); `--variations` compares the probe's
`variation_sample=` / `variation_value_hash=` rows with fontTools'
`VarStoreInstancer`. The probe also creates Windows instances through
`IDWriteFontResource` (same float axis requests) and compares clip rectangles,
root-solid alpha and root-gradient stop position and alpha, with foreground white
and palette zero; more than two axes or avar v2 are unsupported here.

`ColrVariationMode` policies:

- `paint_reader` matches the measured Windows reader: coordinates quantized to
  1/65536, float products accumulated in float, final delta rounded with
  `floor(delta + .5)`, raw opacity kept, empty clips canonicalized, per-field
  variation indices formed with `uint32` wrapping before the map lookup (a base of
  `0xffffffff` does not suppress later fields), reciprocal palette normalization,
  signed-zero basis conversion, float radians, short sine/cosine kernels, signed
  encoded radial radii, and `VarScaleAroundCenter` with nonuniform scale where
  both center deltas change X.
- `standard` keeps fractional deltas, clamps opacity and keeps correct
  center/radius semantics; it is compared with fontTools.

### Native axis normalization

```powershell
python tools/font_reference/make_color_fixture.py <scratch>/avar-remap.ttf --avar-remap
python tools/font_reference/make_color_fixture.py <scratch>/avar-stress.ttf --variation-stress --avar-remap
MaiCordFontReference.exe <font-path> > <native-log>
python tools/font_reference/axis_oracle.py <font-path> --native-log <native-log>
```

`font_axes.cpp` normalizes through fvar and avar v1 over immutable byte views and
caller-owned arrays. Missing required avar anchors disable that axis's map, invalid
ordering is rejected, and avar v2 returns unsupported (not treated as v1). Default
precision rounds to 16.16, separate 2.14 outputs serve feature conditions, and
paint-reader mode keeps float user inputs before normalization. The probe emits
per-axis requested float bits, biased 16.16 results and biased 2.14 conditions;
`axis_oracle.py` recomputes them with exact Python fractions from independently
parsed fvar/avar, requires every grid record, rejects duplicate or missing results,
prints JSON and exits 0 on a match, 1 otherwise. Established for COLR reader values
only.

### Paint-program comparisons

For every COLRv1 base glyph the probe builds a paint program with the reference
builder `colr_paint_build` (`tools/colr_reference/`, reads DirectWrite's paint
reader directly) and with the production `colr_program_build`
(`src/render/colr_program.cpp`) at 12, 16, 20, 24, 32, 48, 64, 128, 15.5 and 31.25
pixels/em on the default instance, and compares every used upload byte and the
metadata after the arena descriptor (not arena pointers or allocation history).
Every build must succeed; a shared failure is not a pass. In this adapter pass
`program_oracle.cpp` hands the Windows reader's values to `colr_program_build`
without recomputing them, so it verifies the compiler/outline boundary, not native
paint resolution.

```powershell
MaiCordFontReference.exe <font-path> --native-source
MaiCordFontReference.exe <font-path> --native-source-grid
python tools/font_reference/make_color_fixture.py <scratch>/map-last-active.ttf --variation-stress --map-last-active
```

`--native-source` replaces the adapter with `colr_source_begin` plus native
compilation and outlines: it checks resolved element bytes through the tree, then
every used program byte and the metadata. A traversal failure is a separate failure
and an incomplete traversal never counts as equality. It skips the adapter check
(`paint_program_adapter_enabled=0`), tests default programs at 32 pixels/em and
whole programs at each selected Windows variation instance at 32 pixels/em.
`--native-source-grid` uses all ten sizes. Without either option the adapter grid
runs and the native default-program check uses 32 pixels/em. A corpus match does
not prove all floating-point inputs or all Windows or hardware versions; executed
GPU pixels are compared by `MaiCordColrReference` (`tools/colr_reference/README.md`).

## OpenType layout table oracles

Each oracle reads original font bytes, records fontTools expectations from an
independent decoder and writes a corpus. No original table is recompiled and no
native code produces expectations. Native verifiers do not execute substitutions
or positioning unless stated.

`layout_table_oracle.py` (`MCOTCM1`) covers Coverage, ClassDef, Device and
VariationIndex subtables; `make_layout_fixtures.py` writes synthetic records in the
same format. The oracle also writes `<out>.json` (fonts, subtables, hashes) whose
`fonts` list is the `--font-manifest` of the oracles below (the examples call it
`layout-corpus.json`).

```powershell
python tools/font_reference/layout_table_oracle.py --out corpus.bin <font.ttf> <font.otf> <collection.ttc>
python tools/font_reference/make_layout_fixtures.py --out synthetic.bin
cmake --build <build> --config Release --target MaiCordLayoutTableTests
MaiCordLayoutTableTests.exe corpus.bin
MaiCordLayoutTableTests.exe synthetic.bin
```

`gdef_oracle.py` (`MCGDEF1`) covers GDEF classes, attachment points, carets, mark
sets, variation-store metadata and filter decisions, plus synthetic tables;
manifest hashes and glyph counts must match.

```powershell
python tools/font_reference/gdef_oracle.py --font-manifest layout-corpus.json --out gdef-corpus.bin
cmake --build <build> --config Release --target MaiCordGdefTests
MaiCordGdefTests.exe gdef-corpus.bin
```

`layout_directory_oracle.py` (`MCLDIR1`) covers GSUB/GPOS script, language,
feature and lookup-subtable directories and checks backing bytes against the
originals; synthetic tables add variation records, and supported-only fixtures also
check a Python selection evaluator against fontTools' instancer.

```powershell
python tools/font_reference/layout_directory_oracle.py --font-manifest layout-corpus.json --out directory-corpus.bin --fixture-header tests/generated/layout_directory_fixture.inc
cmake --build <build> --config Release --target MaiCordLayoutDirectoryTests
MaiCordLayoutDirectoryTests.exe directory-corpus.bin
```

`gsub_oracle.py` (`MCGSUB1`) covers single, multiple, alternate and ligature
substitution matching; a Python evaluator applies decoded ligature rules and GDEF
filters.

```powershell
python tools/font_reference/gsub_oracle.py --font-manifest layout-corpus.json --out gsub-corpus.bin
cmake --build <build> --config Release --target MaiCordGsubTests
MaiCordGsubTests.exe gsub-corpus.bin
```

`context_oracle.py` (`MCCTX1`) covers GSUB/GPOS contextual, chaining and reverse
matching (input positions, ordered nested actions, feature-range boundaries,
prepared-storage needs); standalone fixtures cover formats absent from real fonts.
Nested lookups are not executed.

```powershell
python tools/font_reference/context_oracle.py --font-manifest layout-corpus.json --out context-corpus.bin --fixture-header tests/generated/context_fixture.inc
cmake --build <build> --config Release --target MaiCordContextTests
MaiCordContextTests.exe context-corpus.bin
```

`MaiCordGlyphBufferTests` (no corpus) compares full-record edits with a flat-array
model, including index and boundary maps, failure atomicity and a streaming
expansion copy count (an operation count, not a timing). `MaiCordGsubTests` and
`MaiCordContextTests` also test every input split with strided records around a
poisoned gap through `tests/glyph_view_check.hpp`.

```powershell
cmake --build <build> --config Release --target MaiCordGlyphBufferTests MaiCordGsubTests MaiCordContextTests
MaiCordGlyphBufferTests.exe
MaiCordGsubTests.exe gsub-corpus.bin
MaiCordContextTests.exe context-corpus.bin
```

## GSUB execution

`MaiCordGsubExecutionReference` (`gsub_execution_reference.cpp`) loads a font file
directly, takes the script from `AnalyzeScript` and records `GetGlyphs` output for
one script run (Latin, left to right, en-US). The same source builds
`MaiCordGposExecutionReference`, `MaiCordFeaturePlanReference`,
`MaiCordEmojiLineReference` and `MaiCordEmojiItemsReference` with other defines.

```text
MaiCordGsubExecutionReference.exe <font-path> <hex scalars, comma separated> <0|1>
```

The last argument disables or enables the custom `test` feature (schedules lookup
zero only). Stdout is one JSON object: `hresult`, `script`, `shapes`,
`feature_enabled`, `glyphs`, `clusters` (UTF-16 unit to glyph),
`text_shaped_alone`, `glyph_properties` (`[justification, isClusterStart,
isDiacritic, isZeroWidthSpace]` per glyph) and `dwrite_module`, the DirectWrite DLL
actually loaded. Exit 0 success, 1 failed HRESULT or output, 2 bad arguments.

`gsub_execution_oracle.py generate` writes first-party TrueType fixtures
(`<name>.ttf`, a decoded `.ttx`, `fixtures.json` with hashes) with GSUB actions
and GDEF classes. `record` runs each font with the feature disabled and
enabled and requires one DirectWrite module for all runs. Disabled runs assert the
nominal input glyphs and simple enabled controls assert activation, filtering,
stopping and scan continuation; the rest are measured observations, not Python
shaping predictions.

```powershell
python tools/font_reference/gsub_execution_oracle.py generate <fixtures>
cmake --build <build> --config Release --target MaiCordGsubExecutionReference
python tools/font_reference/gsub_execution_oracle.py record <fixtures> --exe <bin>/MaiCordGsubExecutionReference.exe --out <observations>.json
```

`export` (`MCGEX02`) keeps the original GSUB/GDEF bytes and measured Windows
outputs; `MaiCordGsubExecuteTests` compares glyph IDs at every initial gap position
(not clusters or placement). The corpus keeps one known Windows difference,
extension-wrapped reverse substitution: the native expectation is the unwrapped
lookup's result (OpenType extension equivalence), and the differing Windows
observation is also checked but not counted as parity.

```powershell
python tools/font_reference/gsub_execution_oracle.py generate <fixtures>
python tools/font_reference/gsub_execution_oracle.py record <fixtures> --exe <bin>/MaiCordGsubExecutionReference.exe --out <windows>.json
python tools/font_reference/gsub_execution_oracle.py export <fixtures> --observations <windows>.json --out <corpus>.bin --header tests/generated/gsub_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGsubExecuteTests
MaiCordGsubExecuteTests.exe <corpus>.bin
```

`export-clusters` (`MCGCL01`) adds source indices, initial boundaries, final
character-to-glyph maps and cluster-start flags. Initial boundaries come from the
recorded feature-disabled Windows run, so `MaiCordClustersTests` verifies only
changes caused by GSUB, not independent Unicode or script grouping.

```powershell
python tools/font_reference/gsub_execution_oracle.py export-clusters <fixtures> --observations <windows>.json --out <clusters>.bin --header tests/generated/cluster_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordClustersTests
MaiCordClustersTests.exe <clusters>.bin
```

## GPOS

`gpos_oracle.py` (`MCGPOS1`) covers GPOS value, anchor and single/pair matching
from a hash-checked font manifest; its synthetic fixture is what a no-path run
uses. `gpos_attachment_oracle.py` (`MCGAT01`) covers glyph, local mark-class,
ligature-component and anchor data; it checks data access, not attachment
selection.

```powershell
python tools/font_reference/gpos_oracle.py --font-manifest <layout-corpus>.json --out <gpos-corpus>.bin --fixture-header tests/generated/gpos_fixture.inc
cmake --build <build> --config Release --target MaiCordGposTests
MaiCordGposTests.exe <gpos-corpus>.bin
```

```powershell
python tools/font_reference/gpos_attachment_oracle.py --font-manifest <layout-corpus>.json --out <attachment-corpus>.bin --fixture-header tests/generated/gpos_attachment_fixture.inc
cmake --build <build> --config Release --target MaiCordGposAttachmentTests
MaiCordGposAttachmentTests.exe <attachment-corpus>.bin
```

### GPOS execution

`MaiCordGposExecutionReference` is the GSUB reference built with
`MAICORD_PLACEMENT_REFERENCE`; it adds `GetGlyphPlacements` with exact binary32
outputs.

```text
MaiCordGposExecutionReference.exe <font-path> <hex scalars> <0|1> <em-size bits> <rtl 0|1> <sideways 0|1>
```

`<em-size bits>` is the decimal bit pattern of the binary32 font size. The JSON
object gains `placement_hresult`, `em_bits`, `rtl`, `sideways` and `placement_bits`
(`[advance, advance offset, ascender offset]` float bits per glyph); float bits are
authoritative. Exit codes as for the GSUB reference.

`gpos_execution_oracle.py generate` creates first-party fonts with isolated GPOS
scheduling and optional always-on GSUB ligatures, including `components-*` fonts
(distinct component anchors, scheduled GSUB stages, mark-to-mark follow-ups).
`record` captures one Latin run, en-US, horizontal placement, both RTL values and
sizes 1000, 16, 13.25 and 17.3 DIPs. The case `components-delete-intervening-base`
has prohibited zero-length MultipleSubst output (`invalid_input` metadata); it is
not valid glyph-deletion semantics, and `export-ligatures` and
`component_shaping_oracle.py` skip it.

```powershell
python tools/font_reference/gpos_execution_oracle.py generate <fixtures>
cmake --build <build> --config Release --target MaiCordGposExecutionReference
python tools/font_reference/gpos_execution_oracle.py record <fixtures> --exe <bin>/MaiCordGposExecutionReference.exe --out <windows>.json
```

Each `export-*` mode takes `<fixtures> --observations <windows>.json --out
<corpus>.bin [--header <path>]` and keeps the full font bytes, scheduled lookup
indices, recorded glyphs and shaping flags, and exact disabled and enabled Windows
placement bits. Glyphs and shaping flags stay reference inputs. The four native
targets share `tests/gpos_execute_test.cpp`, selected by defines.
`export-basic` (`MCGPX01`) selects fixtures with only single/pair lookups,
including extensions.

```powershell
python tools/font_reference/gpos_execution_oracle.py export-basic <fixtures> --observations <windows>.json --out <positions>.bin --header tests/generated/gpos_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGposExecuteTests
MaiCordGposExecuteTests.exe <positions>.bin
```

`export-marks` (`MCGMK01`, `MAICORD_MARK_POSITION_TESTS`) adds mark-to-base and
mark-to-mark lookups; exact floats are compared under the Windows null-target
compatibility policy.

```powershell
python tools/font_reference/gpos_execution_oracle.py export-marks <fixtures> --observations <windows>.json --out <marks>.bin --header tests/generated/gpos_mark_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGposMarkExecuteTests
MaiCordGposMarkExecuteTests.exe <marks>.bin
```

`export-cursive` (`MCGCU01`, `MAICORD_CURSIVE_POSITION_TESTS`) adds cursive
lookups.

```powershell
python tools/font_reference/gpos_execution_oracle.py export-cursive <fixtures> --observations <windows>.json --out <cursive>.bin --header tests/generated/gpos_cursive_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGposCursiveExecuteTests
MaiCordGposCursiveExecuteTests.exe <cursive>.bin
```

`export-ligatures` (`MCGLP01`, `MAICORD_LIGATURE_POSITION_TESTS`) selects root GPOS
types 1 through 6 and writes attachment seeds for the ligature fixtures (unknown
history has its own component sentinel because the measured null-anchor behavior
differs). The seeds are first-party fixture inputs; no GSUB provenance is derived.
The `components-*` fonts appear only here; the other exporters omit their GPOS5
lookups.

```powershell
python tools/font_reference/gpos_execution_oracle.py export-ligatures <fixtures> --observations <windows>.json --out <ligatures>.bin --header tests/generated/gpos_ligature_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGposLigatureExecuteTests
MaiCordGposLigatureExecuteTests.exe <ligatures>.bin
```

`component_shaping_oracle.py` (`MCCMP01`) exports the valid ligature-positioning
fixtures (GPOS 1-8) as scalar inputs with expected glyphs, character maps and
placements, and no attachment seeds. The test maps scalars through native cmap, runs
the fixture's GSUB schedule with component history, then the native GPOS program.
Source clusters come from native grapheme boundaries and positioning flags from
final classes and cluster transitions; recorded flags are only compared. Initial
predecessor affinity/class grouping is test policy.

```powershell
python tools/font_reference/component_shaping_oracle.py <fixtures> --observations <windows>.json --out <components>.bin --header tests/generated/component_shaping_fixture.inc
cmake --build <build> --config Release --target MaiCordComponentShapingTests
MaiCordComponentShapingTests.exe <components>.bin
```

`MaiCordCursiveDeferredTests` compares eager and coalesced reverse-cursive
execution; `--benchmark` times it manually, and the registered run is the check.
Coalescing reuses position slots, adds 24 bytes of local state and needs bounded
integer arithmetic (it does not approximate fractional positions).

## Feature policy, lookup plans and nominal eligibility

`feature_plan_oracle.py` and `MaiCordFeaturePlanReference` measure installed
Windows default features, required features, lookup ordering, deduplication,
parameter priority and script/language fallback, limited to the features MaiCord
requires (default shaping and independent-letter mode).

```powershell
python tools/font_reference/feature_plan_oracle.py generate <folder>
python tools/font_reference/feature_plan_oracle.py record <folder> --exe <bin>/MaiCordFeaturePlanReference.exe --out <observations>.json
```

`MaiCordFeaturePlanReference` (the GPOS reference built with
`MAICORD_FEATURE_PLAN_REFERENCE`) takes `<font-path> <hex scalars>`, then
`default` or comma-separated four-character `tag=value` options (up to 32), then
`<em-size bits> <rtl> <sideways> <locale>` and an optional mode and weight:

| Mode | Use |
| --- | --- |
| `layout` | also records `GetTextComplexity` and `TextLayout` output |
| `bmp-complexity` | BMP intrinsic eligibility (feature `default`, LTR, not sideways) |
| `metrics 400\|600` | the selected face's actual weight, ten font-wide metrics (units/em, ascent, descent, line gap, cap height, x-height, underline and strikeout position and thickness) and all horizontal design advances |

`layout` and `metrics` accept a weight of 1 to 1000 (the oracles use 400 and 600).
The binary also has the modes `hit-layout`, `edit-layout`, `font-metrics`,
`metric-cost`, `ranges`, `range-profile` and `range-interior`, not covered here.
Exit 0 success, 1 failure, 2 bad arguments.

`maicord_scope_audit.py --repo <repo> --out <scope.json>` is a read-only inventory
of MaiCord's font choices (the `text_fonts` table in
`src/render/d3d11_renderer_text.cpp`), call sites, bundled font formats and axes,
and registry entries for selected system fonts (`winreg`: Windows only). It does
not prove runtime fallback-face coverage.

`lookup_plan_oracle.py` (`MCPLN03`) generates first-party fonts for the two
MaiCord typography modes (default and independent-letter) and records full
`TextAnalyzer` shaping and `TextLayout` output (`layout`); `layout-differences.json`
keeps differences between the two Windows API layers. The native planner owns no
memory and supports only the horizontal Latin/en-US full-shaping profile and the
one-scalar nominal gate; it does not decide general `TextLayout` range eligibility
or other script stages.

```powershell
cmake --build <build> --config Release --target MaiCordFeaturePlanReference
python tools/font_reference/lookup_plan_oracle.py <output> --exe <bin>/MaiCordFeaturePlanReference.exe --header tests/generated/lookup_plan_fixture.inc
cmake --build <build> --config Release --target MaiCordLookupPlanTests
MaiCordLookupPlanTests.exe
```

`complexity_oracle.py` (`MCNOM01`) exports a native bitset corpus of GSUB/GPOS
glyph participation from first-party fonts. Uppercase ASCII aliases expose
participation; glyph zero is excluded (the text caller handles missing glyphs).

```powershell
python tools/font_reference/complexity_oracle.py <output> --exe <bin>/MaiCordFeaturePlanReference.exe --header tests/generated/nominal_participation_fixture.inc
cmake --build <build> --config Release --target MaiCordNominalParticipationTests
MaiCordNominalParticipationTests.exe
MaiCordNominalParticipationTests.exe <font-file>
```

The font-file mode emits a combined GSUB/GPOS bitmap and status using the native
source-character bitmap, so `nominal_app_fonts.py` can compare actual app faces.
`nominal_cmap_probe.py <source> <out> --exe <reference> [--native <test exe>]
[--ligature]` isolates cmap effects with lookup data fixed. Table participation
alone is insufficient: cmap-dependent context pruning, Unicode eligibility and
FeatureVariations also decide eligibility.

`nominal_scalar_oracle.py` builds neutral first-party cmap 12/13 fonts mapping all
valid BMP scalars to one glyph, with no layout tables. The reference's
`bmp-complexity` mode verifies every mapping and records intrinsic eligibility; two
cmap formats and two glyph IDs must agree. The profile is calibrated to the
recorded Windows reference, not to general Unicode property data.
`sfnt_nominal_sources` walks native cmap ranges to build the reachable-glyph
bitmap, which prunes impossible format-3 contexts and ligatures. `<scope.json>` is
the `maicord_scope_audit.py` output.

```powershell
python tools/font_reference/nominal_scalar_oracle.py <output>/scalars --exe <bin>/MaiCordFeaturePlanReference.exe --header src/text/generated/nominal_scalar_data.inc --test-header tests/generated/nominal_scalar_fixture.inc
cmake --build <build> --config Release --target MaiCordNominalSourcesTests MaiCordNominalParticipationTests
MaiCordNominalSourcesTests.exe
python tools/font_reference/nominal_app_fonts.py --scope <scope.json> --native <bin>/MaiCordNominalParticipationTests.exe --reference <bin>/MaiCordFeaturePlanReference.exe --out <output>/app-printable.json --ascii-printable
```

`nominal_variations_oracle.py` (`MCNVAR1`) creates first-party variable fonts and
records default and independent-letter cases at weights 400 and 600 (the reference
selects a face instance and records the actual face and `TextLayout` run weight
bits; other weights are rejected). Participation reads base features while
full shaping uses the FeatureVariations selected for the instance, so the font-file
eligibility bitmap is shared across both weights. No general range segmentation.

```powershell
python tools/font_reference/nominal_variations_oracle.py <output> --exe <bin>/MaiCordFeaturePlanReference.exe --header tests/generated/nominal_variations_fixture.inc
cmake --build <build> --config Release --target MaiCordNominalVariationsTests
MaiCordNominalVariationsTests.exe
python tools/font_reference/nominal_app_fonts.py --scope <scope.json> --native <bin>/MaiCordNominalParticipationTests.exe --reference <bin>/MaiCordFeaturePlanReference.exe --out <output>/app-printable.json --ascii-printable --variable-weights
```

`metric_variations_oracle.py` (`MCVMET1`) records the `metrics` mode for first-party
fonts (HVAR, rounding, compressed hmtx tails, empty MVAR). The corpus embeds those
fixtures only; the installed Bahnschrift file is read for separate differential
checks. Font-file mode emits weights 400 and 600, four decoration fields and all
advances using native readers; decoration field rounding is done in the QA harness.

```powershell
python tools/font_reference/metric_variations_oracle.py <output> --exe <bin>/MaiCordFeaturePlanReference.exe --header tests/generated/metric_variations_fixture.inc
cmake --build <build> --config Release --target MaiCordMetricVariationsTests
MaiCordMetricVariationsTests.exe
MaiCordMetricVariationsTests.exe C:/Windows/Fonts/bahnschrift.ttf
```

## Outlines and TrueType instructions

`gvar_outline_oracle.py` (`MCGVAR1`) builds first-party gvar fonts with Windows
outline expectations; the real Bahnschrift file is read separately and not
embedded.

```powershell
python tools/font_reference/gvar_outline_oracle.py <output> --exe <bin>/MaiCordFontReference.exe --header tests/generated/gvar_outline_fixture.inc
cmake --build <build> --config Release --target MaiCordGvarOutlineTests
MaiCordGvarOutlineTests.exe
MaiCordFontReference.exe C:/Windows/Fonts/bahnschrift.ttf --gvar400
MaiCordFontReference.exe C:/Windows/Fonts/bahnschrift.ttf --gvar600
```

```powershell
MaiCordFontReference.exe <font> --geometry
python tools/font_reference/outline_instruction_oracle.py <output> --source <first-party-gvar-full.ttf> --exe <bin>/MaiCordFontReference.exe
```

`outline_instruction_oracle.py` removes variation tables from the first-party
source and adds its own fpgm, prep, CVT and glyph instructions (empty, SHPIX, MDAP,
IUP and INSTCTRL cases). Some cases differ from the native provider on purpose:
they record required behavior and are not passing conformance tests. With
`--context --fixture tests/generated/tt_interpreter_fixture.inc` (`MCTTVM1`) it
captures the expanded instruction cases and writes first-party bytecode and
Windows coordinate expectations. `MaiCordTtInterpreterTests` runs the native core
against them; an optional font-path argument runs the native font and preparation
programs and reports phase, status, work, offset, opcode, stack and call depth
without comparing program state with Windows or running glyph programs.

```powershell
python tools/font_reference/tt_outline_fixture.py --directories <first-party-font-dirs> --exe <oracle> --root <output> --header tests/generated/tt_outline_fixture.inc
python tools/font_reference/tt_position_oracle.py --root <output> --source <first-party-empty-instruction-font> --exe <oracle> --header tests/generated/tt_position_fixture.inc
```

- `tt_outline_fixture.py` (`MCTTGL1`) embeds complete first-party fonts and their
  Windows outlines (`<oracle>` is `MaiCordFontReference.exe`, run with
  `--geometry-dump`); `MaiCordTtOutlineTests` checks them with native preparation
  and decoding.
- `tt_position_oracle.py` generates the positioning and vector corpus from the
  first-party empty instruction font; `MaiCordTtOutlineTests position` runs it
  through the same native geometry adapter.
- `--instructions-state=<glyph-id>` adds native current and original fixed-point
  coordinates and touch bits next to the Windows commands.

`--owned` goes before a comparison mode, for example
`MaiCordFontReference <font> --owned --instructions` or `--owned --gvar600`. It uses
the store's prepared cmap, geometry and a retained native instance (normalized
coordinates, shared tuple scalars and optional TrueType state come from the instance
owner), and the final release must leave no retained files or faces. Use `--owned
--instructions` for static or default faces and `--owned --instructed --gvar600` (or
`400`) for a varied face; variable glyph instructions must be disabled by
preparation, and CFF keeps its native outline provider. `MaiCordFontInstanceTests`
covers these lifetimes.

## Simulation (bold and oblique)

- `--simulate=1` and `--simulate=3` on static TrueType exercise native bold and
  bold-oblique geometry; `--simulate=2 --instructions` compares native oblique
  geometry; `--simulate=2 --renderer --instructions` uses the actual renderer
  binding and ownership path.
- `--simulate=1 --points --instructions-dump` captures native TrueType points and
  base commands next to Windows bold commands and reports missing-function prep
  recovery (`instruction_prepare_missing_function`,
  `instruction_instance_prep_recovered`). With simulation requested `--points`
  holds styled coordinates; use `--simulate=0` for original-contour diagnostics.
  Contour-direction comparisons are meaningful for original integer simple
  contours.
- `MaiCordFontReference <font> --simulate=0|1|2|3 --metrics-dump` compares native
  simulation of resolved base design metrics and emits every glyph's
  `reference_metrics=` row. The oracle supplies the unsimulated base; native
  base-metric extraction and simulated outlines are not covered. Its metric
  difference counter feeds the exit status.

These scripts support diagnostics and regression fixtures; they do not enable
native bold in the renderer. Most take an output `<root>` directory first and
`--exe <ref>` (`MaiCordFontReference.exe`); `--help` shows each script's arguments.

- `simulation_metrics_oracle.py` (`--inventory <scope.json>`): simulation design
  metrics for every default app face.
- `bold_geometry_oracle.py` (`--metrics-header <path>`), `bold_direction_oracle.py`:
  synthetic-bold outlines, metrics and contour-direction probes.
- `bold_app_points_oracle.py` (`--exe`, `--inventory`, `--analyse-existing`):
  native pre-simulation points and Windows bold outlines for app faces.
- `bold_point_model.py <observations> --output <path>`: QA model of the bold
  scaler; not a provider.
- `bold_preparation_oracle.py` (`--inventory <scope.json>`): bold-preparation
  policy, in disposable QA processes.
- `bold_orientation_oracle.py <root> --model <path> [--probes]`: per-contour
  Windows flags from disposable reference processes, never the app;
  `orientation_first_party.py ... --output <path>` builds the native fixture and
  `orientation_native_oracle.py ... --reference <path>` compares native
  classification with the frozen flags.
- `prep_instance_oracle.py ... --reference <path>`: native preparation policy
  against frozen probes.
- `bold_native_matrix.py ... --source <path> [--renderer]`: native bold and
  bold-oblique outlines against Windows.
- `bold_outline_fixture.py ... --headers <two paths>`: nested, overlapping and
  reflected-component bold fixtures.

## Script analysis observers

`script_analysis_reference.cpp`, `script_sequence_reference.cpp`,
`script_font_reference.cpp`, `script_benchmark.cpp` and
`language_selection_benchmark.cpp` are not built by `cmake/reference_tools.cmake`
or any other CMake file. Each defines its own entry point
(`maicord_script_analysis_reference_entry`,
`maicord_script_sequence_reference_entry`, `maicord_script_font_reference_entry`,
`maicord_script_benchmark_entry`, `maicord_language_selection_benchmark_entry`);
the sequence, font and benchmark sources include `script_analysis_reference.cpp`.
The first four use an isolated DirectWrite factory. Layouts are little-endian. A
packed script ID is the DirectWrite script index with `0x8000` set for
`DWRITE_SCRIPT_SHAPES_NO_VISUAL`; `0xffff` marks an unobserved slot. Binary outputs
end with a `u32` count and that many 20-byte `DWRITE_SCRIPT_PROPERTIES`. Scalars
are valid Unicode scalar values (no surrogates, at most `0x10ffff`).

- `script_analysis_reference` (no input): header `MSA1` (`0x3141534d`), `1`, 8
  contexts, `0x110000` scalars, 6 bytes per row; the context scalars (`0, 0x61,
  0x628, 0x5d0, 0x4e2d, 0x915, 0xe01, 0x3042`); then, for each scalar and context,
  three packed IDs (before, scalar, after). Stderr says `PASS exhaustive script
  analysis` or `FAIL script analysis`. Exit 0 or 1.
- `script_sequence_reference`: stdin `MSQ1` (`0x3151534d`), case count (up to
  1,000,000), per case a scalar count (1 to 4096) and the scalars. Stdout echoes
  the header, then per case the count and one packed ID per scalar (en-US, left to
  right, no font selection). Exit 0 or 1.
- `script_font_reference <font>`: stdin `SFP1` (`0x31504653`) or `SFP2`
  (`0x32504653`) and a case count (1 to 100000); per case a scalar count (1 to 64),
  an RTL flag (0 or 1), for `SFP2` a feature count (up to 32) and that many
  `DWRITE_FONT_FEATURE` (tag, value), then the scalars. Stdout echoes the header,
  then per case UTF-16 length, glyph count, the `GetGlyphs` and
  `GetGlyphPlacements` HRESULTs, packed IDs per UTF-16 unit and, when they
  succeeded, cluster map, text properties, glyph IDs, glyph properties, advances
  and offsets (size 1000, en-US). The font must hold exactly one face and each case
  must resolve to one script run. Exit 0, 1 failure, 2 bad font argument.
- `script_benchmark`: compares `native_script_resolve` with Windows on prepared
  input and prints one JSON line per sample and length (`sample`, `scalars`,
  `utf16_units`, `frequency`, `iterations`, `batches` of tick pairs, native then
  DirectWrite), then `PASS prepared script analysis benchmark`. Results are
  verified outside the timed batches. Exit 0 or 1.
- `language_selection_benchmark.cpp` calls `previous_latin_lookup_plan`,
  `previous_common_lookup_plan` and `previous_arabic_lookup_plan`, which the tree
  does not contain, so it does not link.

`script_fixtures.py` turns these observations and a Unicode dump into a compact
compatibility table and standalone native replay fixtures:

```powershell
python tools/font_reference/script_fixtures.py --scalars <scalars.bin> --unicode-dump <unicode-dump.bin> --unicode-header include/maicord/text/generated/unicode_data.hpp --sequences <sequences.bin> --sequence-results <sequence-results.bin> --output <directory>
```

`--scalars` and `--sequence-results` are the outputs of `script_analysis_reference`
and `script_sequence_reference` (run on `--sequences`). `--unicode-dump` must start
with `MCUCD17\0` or `MCUCDS1\0`; other layouts are rejected, including the
`MCUCDS2` dump documented in `tools/unicode/README.md`. Output:
`script_compat_data.inc`, `classes.bin` (`NCS1`), `contexts.bin` (`NSC1`),
`resolved.bin` (`NSR1`) and `script-fixtures.json` (hashes).
`MaiCordScriptTests.exe <classes.bin|contexts.bin|resolved.bin>` replays one
generated file against the native script classes and resolution (no argument: the
built-in checks) and asserts that `dwrite.dll` is not loaded.

## Application text boundary

`MaiCordAppTextBoundary` (`app_text_boundary.cpp`) observes MaiCord's real
profile, style, emoji and fallback path (`renderer_text_system`,
`text_shape_utf8_styled`) without a window or graphics device. It uses synthetic
public-script samples only, never user messages. The font inventory describes the
host and the corpus; it is neither a native fallback implementation nor an
exhaustive Unicode test.

```powershell
python tools/font_reference/app_text_boundary.py prepare <qa-root>
python tools/font_reference/app_text_boundary.py capture <qa-root>
python tools/font_reference/app_text_boundary.py summarize <qa-root>
```

- `prepare` writes `corpus.bin` and `corpus.json` into `<qa-root>`. `capture` runs
  `MaiCordAppTextBoundary.exe` from `<qa-root>` on `corpus.bin` with a private
  `LOCALAPPDATA`, writes `observations-<config>-<n>.jsonl` and
  `capture-<config>-<n>.json` (exit code, stderr, hashes) and asserts exit 0.
  `summarize` (needs fontTools) writes `summary-<config>-<n>.json`.
- `--config Release|Debug` (default Release) and `--repeat <n>` (default 1) name
  the outputs, so other values keep independent captures.
- Application errors are recorded as case results: a successful capture exit
  means the observation completed, not that every app case succeeded.
