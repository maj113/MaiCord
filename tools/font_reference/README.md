# Font and text reference tools

Windows (DirectWrite) oracles and fontTools generators that produce the data
compiled into the client under `src/**/generated` and the committed
`tests/generated` fixtures, and compare MaiCord's first-party font and COLR code
with Windows. None ships. `maicord_font` and the native test targets link
first-party libraries and `kernel32` only.

- C++ oracles (`MaiCord*Reference`) record what DirectWrite does.
- Python generators (`*_oracle.py`, `*_fixture.py`) build first-party fixture
  fonts, drive the oracles and write corpora for the native tests
  (`MaiCord*Tests`). They need fontTools and run from the repository root.
- `text_reference.py` records the application-line references from HarfBuzz and
  the engine's own layout; no test in the suite links DirectWrite.

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
  `--header` / `--fixture-header` write it as raw bytes (`tests/generated/<stem>.bin`)
  with a `#embed` wrapper (`<stem>.inc`, via `embed_fixture.py`) that the test
  includes, so a no-argument run checks it.
  A fixture with several arrays (fonts, streams) keeps each array of 256 bytes or more in
  `<stem>/<array>.bin` beside its include and embeds it from there; smaller arrays stay inline.
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

## text_reference.py

Records the committed references of `MaiCordNativeAppLineTests`
(`tests/generated/app_line_reference.{bin,inc}`, read by
`tests/ui/text/app_line_reference.cpp`). Needs uharfbuzz. HarfBuzz is the shaping
reference; no DirectWrite is involved.

```powershell
python tools/font_reference/text_reference.py record [--build <build>] [--merge] [--report <file>]
python tools/font_reference/text_reference.py explain <run key>
```

`record` runs every registered app-line test from `<build>` (default
`D:/MaiCord-build/Release`, after a `-KeepIntermediates -Test` build) with
`MAICORD_TEXT_REFERENCE_RECORD` set. Each process writes two things: a layout digest
per 64 cases, which is the engine's own output (positions, lines, hits, decorations
and editor state), and one HarfBuzz request per distinct native run. A request holds
the font file and face, direction, paragraph start and end (HarfBuzz's BOT and EOT),
independent-letter features, axes, script anchor, text, and context for joining
scripts. The tool then shapes each request with HarfBuzz and compares it with the
native shape in logical order (a right-to-left buffer is reversed) and at the engine's
cluster granularity: HarfBuzz clusters inside one engine cluster (one per syllable,
the caret contract) merge, an engine cluster HarfBuzz splits across its edge differs,
and glyph IDs and design-unit positions must match exactly (positions are left out
for tracked or simulated runs). It stores a 32-bit digest of the shape found equal and
marks the other runs divergent: the test counts those and fails only when a run that
matched HarfBuzz stops matching. `--report` lists divergent runs per script, raw and
normalised, and the normalised ones by kind, with keys for `explain`.

The reference shapes each published run alone, so a run whose line continues it in a
separate run can differ from HarfBuzz through the reference itself. Such runs are
listed in `REFERENCE_CAUSED` (key and reason); they stay divergent in the reference, and
`--report` lists them apart from the engine's divergences. The one known case is a
word-final letter followed by its line's trailing space (a separate run): the engine
kerns it with that space, as HarfBuzz shaping the whole line does, while the reference
shapes the letter alone.

Each group of 64 cases records the fonts it used, by table-directory digest. On a
machine whose system fonts (under a Windows font folder) differ, only the groups and
runs that used them are skipped; a test with nothing left to check exits 77, and
`tools\build.ps1` prints how many tests and groups were skipped. The repository's
fonts must match everywhere. Cases that need no other script run with
`--bundled-fonts` (`cmake/test_registry.cmake`): Segoe UI, Georgia and Consolas become
the bundled MuseoModerno, Zilla Slab and BioRhyme. `record` prints how many groups a
machine with other system fonts still checks (`MAICORD_TEXT_REFERENCE_FOREIGN=1`
simulates one).

`--merge` keeps the committed profiles and adds this machine's fonts and group
digests; record it from the same source revision. The manual **Text references**
workflow (`.github/workflows/text-reference.yml`) does that on the hosted
windows-2025 runner and uploads the merged reference to commit, after which CI
checks the system-font cases too. Without `--merge` the reference holds this
machine alone.

## harfbuzz_fixture.py

Re-derives the expectations of first-party shaping fixtures from HarfBuzz 15.0.0
(uharfbuzz 0.57.0), keeping each fixture's fonts and inputs; needs uharfbuzz, numpy and
fontTools. No DirectWrite is involved.

```powershell
python tools/font_reference/harfbuzz_fixture.py arabic|component|emoji|emoji_layout|gpos|hebrew|joining|marks|participation|plan [--pins <file>]
```

A font without GPOS is shaped with one holding an unused lookup under `DFLT` and
`hebr`: HarfBuzz positions the marks of a font without GPOS lookups (or, for Hebrew,
without a `hebr` GPOS script) from glyph extents, and the engine has no such fallback.

Every case is kept. Where the engine knowingly differs from HarfBuzz, the case (or, for
`gpos` and `component`, the font; for `plan`, the configuration) is marked known divergent
with its class, and its expectation is the engine's own output, pinned from the test:
run the test with `MAICORD_FIXTURE_RECORD=1` (it prints a `pin_*` line per known divergent
case instead of checking it) and regenerate with `--pins <test output>`. Without pins a
known divergent case takes HarfBuzz's output. Each test prints its known divergent count.

- `arabic`: the `arabic_*_fixture` corpora of `MaiCordTextUnitTests`
  (`maicord_native_arabic_plan`, `maicord_native_arabic_run`; pins from both), written as
  `MCARPL4`, each case shaped as Arabic in its direction. Known divergent: GPOS features
  masked by joining form or `rtlm` (1) and GPOS lookups meeting a default ignorable other
  than ZWJ and ZWNJ, which HarfBuzz skips (2). No circle is inserted where the engine
  keeps a leading default ignorable alone or reverses a left-to-right run.
- `gpos`: the basic, mark, cursive and ligature positioning corpora
  (`maicord_native_gpos_execute`), each glyph sequence shaped through a cmap that
  addresses glyphs by ID with the font's `test` lookups (right to left as Arabic, so
  HarfBuzz does not reverse the sequence). A seeded ligature is formed by a `tlig`
  lookup from copies of itself, each mark placed after the component it names. Known
  divergent fonts: HarfBuzz attaches a glyph only its mark coverage calls a mark (the
  engine attaches GDEF marks) (1); a single or pair lookup gives a mark an advance or a
  later lookup moves an attachment (HarfBuzz zeroes mark advances and resolves
  attachments after all lookups, the engine when each applies) (2); a seed names a
  component the ligature lacks (3).
- `component`: `component_shaping_fixture` (`maicord_native_component_shaping`), the
  nominal glyphs of each text shaped as `gpos` does, without and with `test`; where
  HarfBuzz forms the kept glyphs, the character map and position flags are HarfBuzz's
  too (the text's grapheme clusters joined with GSUB's merges; a decomposed ligature
  keeps its merged cluster). Known divergent fonts: mark coverage without a GDEF mark
  (1); a ligature whose mark-to-ligature component count differs from its GSUB
  components (a mark after it takes the table's last component in HarfBuzz, the
  ligature's last one in the engine) (2); marks after a multiple substitution (HarfBuzz
  never attaches to its later outputs, the engine may) (3); a context nesting a pair,
  cursive or contextual lookup (HarfBuzz lets the nested lookup match past the context's
  input, the engine does not) (4); deferred attachment as in `gpos` (5).
- `emoji_layout`: `emoji_line_fixture` and `emoji_paragraph_fixture`
  (`maicord_native_emoji_lines`, `maicord_native_emoji_paragraphs`) keep their recorded
  Windows line and paragraph geometry; a case HarfBuzz shapes to the recorded placements
  and map with other glyph IDs (a hidden default ignorable as the space glyph) takes
  HarfBuzz's glyph IDs.
- `joining`: `arabic_joining_fixture.inc`, read from a font whose form features give
  every character its own glyph.
- `marks`: `arabic_marks_fixture.inc`, the mark order HarfBuzz's normalization and
  `reorder_marks_arabic` give each sequence.
- `emoji`: `emoji_run_fixture` (`maicord_native_emoji_runs`), each run shaped left to
  right from the start of its paragraph; flags mark a cluster's first glyph (2) and a
  GDEF mark or hidden default ignorable (1). Known divergent: fonts with a direction
  feature (`ltra`, `ltrm`, `rtla`, `rtlm`; the engine's plans do not depend on direction)
  (1); HarfBuzz keeps a default ignorable's own glyph (substitution touched it; the engine
  sees an unchanged glyph and hides it) (2); a ligature whose mark-to-ligature component
  count differs from its GSUB components (3).
- `hebrew`: `hebrew_compat_fixture.inc` (`maicord_native_latin_guards`), each case shaped
  as Hebrew in its direction. Known divergent: left-to-right cases, whose graphemes
  HarfBuzz reverses (the engine does so only for Arabic) (1); GPOS meeting a default
  ignorable other than ZWJ and ZWNJ (2) or a multiple substitution's output (3).
- `participation`: `nominal_participation_fixture` (`maicord_native_nominal_participation`),
  the glyphs HarfBuzz's default features can act on.
- `plan`: `lookup_plan_fixture` (see below), the lookups of HarfBuzz's default features
  (rvrn in its own first stage), HarfBuzz's glyph and placement for `a` and the
  nominal gate. Configurations of fonts with a direction feature are known divergent and
  shaped by HarfBuzz without those features.

## syllabic_fixtures.py

Re-derives `tests/generated/syllabic_shape_fixture.inc` (`MaiCordSyllabicShapeTests`) from
HarfBuzz 15.0.0; needs uharfbuzz and fontTools.

```powershell
python tools/font_reference/syllabic_fixtures.py [--fuzz 1500] [--fonts C:/Windows/Fonts] [--words khmer=<corpus> ...]
```

- Cluster-start corpora of Sinhala, Khmer, Myanmar and Tibetan
  (`maicord_native_syllabic_shape`): the fixture's own vectors, word corpora given with
  `--words` and seeded random sequences. HarfBuzz flags every glyph after the first of a
  syllable or grapheme unsafe to break; in a font that maps every candidate character and
  has no layout tables nothing else sets the flag, so the unflagged scalars are the
  cluster starts. Vectors listed in `DIVERGENT` stay, pinned: the test requires them to
  keep differing.
- Sample strings on the Windows fonts of `maicord_native_{khmer,myanmar,sinhala,tibetan}_owned`
  (glyphs, design-unit positions and the character map) and on Segoe UI (HarfBuzz's
  default shaper for Myanmar and Sinhala there), with each font's `head`
  checkSumAdjustment: against another font version the test checks counts only.

## MaiCordFontReference

`main.cpp`, `program_oracle.cpp`, `dwrite_outline_reference.cpp` and the paint adapter
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
  transient values, subroutine depth 10 (`tests/text/opentype/cff_outline_test.cpp`, in
  `MaiCordFontTests`, builds its own font bytes).
- `truetype_outline` (static): caller-owned buffers, work budget, component depth
  64, no hint bytecode; `tt_outline` runs instructions. Active variable-font
  instructions and `cvar` are unsupported.
- Prepared views borrow immutable mapping storage; callers keep it alive and do
  not fabricate or mutate views.

## Fixture generator

```powershell
python tools/font_reference/make_cff_fixture.py <scratch-directory>
```

`make_cff_fixture.py` writes `cff-canonical.otf`, `cff-rounded.otf`, `cff-scale.otf`,
`cff-inverse.otf`, `cff-inverse2.otf` and `cff-affine.otf` (different FontMatrix
values; Windows rejects the affine one, so the probe exits 6 for it). `--expanded` adds
flex/equivalent-cubic and stress pairs and `cff-small.otf`, `cff-third.otf`,
`cff-scale107.otf`.

## COLR and variation comparison

```powershell
MaiCordFontReference.exe <font-path> > <native-log>
```

The probe validates each native paint graph, compares palette bytes and COLRv1
roots with DirectWrite, and emits an FNV-1a digest (`color_value_hash`) of decoded
paint fields, clip boxes and color stops (variable fields and indices stay raw):
a structural diagnostic, not a pixel comparison.

With a COLR ItemVariationStore the probe resolves the normalized grid -1, -.75,
-1/3, -.25, 0, .125, 1/3, .5, .875, 1: ten instances for one axis, 100 for two
(axis zero varies fastest) and writes its `variation_sample=` /
`variation_value_hash=` rows. The probe also creates Windows instances through
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
  center/radius semantics.

`text/fonts/axes.cpp` normalizes through fvar and avar v1 over immutable byte views and
caller-owned arrays. Missing required avar anchors disable that axis's map, invalid
ordering is rejected, and avar v2 returns unsupported (not treated as v1). Default
precision rounds to 16.16, separate 2.14 outputs serve feature conditions, and
paint-reader mode keeps float user inputs before normalization. The probe emits
per-axis requested float bits, biased 16.16 results and biased 2.14 conditions.

### Paint-program comparisons

For every COLRv1 base glyph the probe builds a paint program with the reference
builder `colr_paint_build` (`tools/colr_reference/`, reads DirectWrite's paint
reader directly) and with the production `colr_program_build`
(`src/render/effects/colr_program.cpp`) at 12, 16, 20, 24, 32, 48, 64, 128, 15.5 and 31.25
pixels/em on the default instance, and compares every used upload byte and the
metadata after the arena descriptor (not arena pointers or allocation history).
Every build must succeed; a shared failure is not a pass. In this adapter pass
`program_oracle.cpp` hands the Windows reader's values to `colr_program_build`
without recomputing them, so it verifies the compiler/outline boundary, not native
paint resolution.

```powershell
MaiCordFontReference.exe <font-path> --native-source
MaiCordFontReference.exe <font-path> --native-source-grid
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
GPU pixels are not compared here.

## OpenType layout table oracles

Each oracle reads original font bytes, records fontTools expectations from an
independent decoder and writes a corpus. No original table is recompiled and no
native code produces expectations. Native verifiers do not execute substitutions
or positioning unless stated.

`layout_table_oracle.py` (`MCOTCM1`) covers Coverage, ClassDef, Device and
VariationIndex subtables. The oracle also writes `<out>.json` (fonts, subtables,
hashes) whose `fonts` list is the `--font-manifest` of the oracles below (the
examples call it `layout-corpus.json`).

```powershell
python tools/font_reference/layout_table_oracle.py --out corpus.bin <font.ttf> <font.otf> <collection.ttc>
cmake --build <build> --config Release --target MaiCordLayoutTableTests
MaiCordLayoutTableTests.exe corpus.bin
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

`context_oracle.py` (`MCCTX1`) covers GSUB/GPOS contextual, chaining and reverse
matching (input positions, ordered nested actions, feature-range boundaries,
prepared-storage needs); standalone fixtures cover formats absent from real fonts.
Nested lookups are not executed.

```powershell
python tools/font_reference/context_oracle.py --font-manifest layout-corpus.json --out context-corpus.bin --fixture-header tests/generated/context_fixture.inc
cmake --build <build> --config Release --target MaiCordContextTests
MaiCordContextTests.exe context-corpus.bin
```

## GSUB execution

`MaiCordGsubExecutionReference` (`gsub_execution_reference.cpp`) loads a font file
directly, takes the script from `AnalyzeScript` and records `GetGlyphs` output for
one script run (Latin, left to right, en-US). The same source builds
`MaiCordFeaturePlanReference`, `MaiCordEmojiLineReference` and
`MaiCordEmojiItemsReference` with other defines.

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

`gpos_execution_oracle.py` creates first-party fonts with isolated GPOS scheduling
and optional always-on GSUB ligatures, including `components-*` fonts (distinct
component anchors, scheduled GSUB stages, mark-to-mark follow-ups).

```powershell
python tools/font_reference/gpos_execution_oracle.py <fixtures>
```

`MaiCordGposExecuteTests` (`tests/text/opentype/gpos_execute_test.cpp`) runs four
built-in corpora; given a corpus file, it reads the file's format from its magic. The
basic (`MCGPX02`, single/pair lookups), mark (`MCGMK02`, adds mark-to-base and
mark-to-mark), cursive (`MCGCU02`, adds cursive) and ligature (`MCGLP02`, root GPOS
types 1 through 6 with attachment seeds) corpora keep the full font bytes, scheduled
lookup indices, glyphs and shaping flags, with HarfBuzz placements (or pinned engine
placements for a known divergent font, whose class version 02 records) from
`harfbuzz_fixture.py gpos`, which also rewrites the oracle's version 01. The ligature corpus's seeds name each mark's target and
component (unknown history has its own sentinel and takes the ligature's last
component, as in HarfBuzz); they are first-party fixture inputs, and no GSUB
provenance is derived. The `components-*` fonts appear only there.

```powershell
python tools/font_reference/harfbuzz_fixture.py gpos
cmake --build <build> --config Release --target MaiCordGposExecuteTests
MaiCordGposExecuteTests.exe
```

`component_shaping_fixture` (`MCCMP02`, with each font's known divergence class) holds ligature-positioning fixtures (GPOS
1-8) as scalar inputs with expected glyphs, character maps and placements, and no
attachment seeds; `harfbuzz_fixture.py component` derives the placements. The test
maps scalars through native cmap, runs the fixture's GSUB schedule with component
history, then the native GPOS program. Source clusters come from native grapheme
boundaries and positioning flags from final classes and cluster transitions; recorded
flags are only compared. Initial predecessor affinity/class grouping is test policy.

```powershell
python tools/font_reference/harfbuzz_fixture.py component
cmake --build <build> --config Release --target MaiCordComponentShapingTests
MaiCordComponentShapingTests.exe
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

`lookup_plan_fixture` (`MCPLN04`, with each configuration's known divergence class) holds first-party fonts for the two MaiCord
typography modes (default and independent-letter); `harfbuzz_fixture.py plan`
derives its lookup lists, HarfBuzz's glyph and placement for `a`, and the nominal
gate. The native planner owns no memory and supports only the horizontal
Latin/en-US full-shaping profile and the one-scalar nominal gate; it does not decide
general `TextLayout` range eligibility or other script stages.

```powershell
python tools/font_reference/harfbuzz_fixture.py plan
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
source-character bitmap. Table participation alone is insufficient: cmap-dependent
context pruning, Unicode eligibility and FeatureVariations also decide eligibility.

`nominal_scalar_oracle.py` builds neutral first-party cmap 12/13 fonts mapping all
valid BMP scalars to one glyph, with no layout tables. The reference's
`bmp-complexity` mode verifies every mapping and records intrinsic eligibility; two
cmap formats and two glyph IDs must agree. The profile is calibrated to the
recorded Windows reference, not to general Unicode property data.
`sfnt_nominal_sources` walks native cmap ranges to build the reachable-glyph
bitmap, which prunes impossible format-3 contexts and ligatures.

```powershell
python tools/font_reference/nominal_scalar_oracle.py <output>/scalars --exe <bin>/MaiCordFeaturePlanReference.exe --header src/text/unicode/generated/nominal_scalar_data.inc --test-header tests/generated/nominal_scalar_fixture.inc
cmake --build <build> --config Release --target MaiCordNominalSourcesTests MaiCordNominalParticipationTests
MaiCordNominalSourcesTests.exe
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
```

`metric_variations_oracle.py` (`MCVMET1`) records the `metrics` mode for first-party
fonts (HVAR, rounding, compressed hmtx tails, empty MVAR). The corpus embeds those
fixtures only; the installed Bahnschrift file is read for separate differential
checks. Font-file mode emits weights 400 and 600, four decoration fields and all
advances using native readers; decoration field rounding is done in the QA harness.
The committed advances are HarfBuzz's, not Windows': HVAR is indexed by glyph ID
even in the compressed hmtx tail, and negative results clamp to zero. Windows
indexes the tail by the last long metric and wraps negatives, so a Windows
re-export differs there; the gvar phantom-advance fixture clamps negatives too.

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

These scripts support diagnostics and regression fixtures; they do not enable
native bold in the renderer. Most take an output `<root>` directory first and
`--exe <ref>` (`MaiCordFontReference.exe`); `--help` shows each script's arguments.

- `bold_geometry_oracle.py`: synthetic-bold outlines and contour-direction probes.
- `bold_app_points_oracle.py` (`--exe`, `--inventory`, `--analyse-existing`):
  native pre-simulation points and Windows bold outlines for app faces.
- `bold_point_model.py <observations> --output <path>`: QA model of the bold
  scaler; not a provider.
- `bold_orientation_oracle.py <root> --model <path> [--probes]`: per-contour
  Windows flags from disposable reference processes, never the app;
  `orientation_first_party.py ... --output <path>` builds the native fixture.
- `bold_outline_fixture.py ... --headers <two paths>`: nested, overlapping and
  reflected-component bold fixtures.
