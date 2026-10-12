# Native font reference probe

`script_font_reference.cpp` batches font-dependent marker observations while
retaining raw script, glyph, cluster and placement output. The binary input
starts with `SFP1` and a 32-bit case count, followed by each scalar count,
glyph-direction flag and scalar array (all little-endian 32-bit integers).
It accepts a font path and uses isolated QA font ownership.
`language_selection_benchmark.cpp` compares current native planners with renamed
copies of the preceding first-party source.

The first-party `script_analysis_reference.cpp` and
`script_sequence_reference.cpp` observers capture en-US script analysis without
font selection. `script_fixtures.py` converts these binary observations and the
verified Unicode dump into a compact compatibility table and standalone native
replay fixtures. It takes explicit `--scalars`, `--unicode-dump`,
`--unicode-header`, `--sequences`, `--sequence-results` and `--output` paths.
`MaiCordScriptTests` accepts one generated `classes.bin`, `contexts.bin` or
`resolved.bin` path. The optional `script_benchmark.cpp` measures prepared-input
analysis against Windows and verifies its sample results outside timed batches.

`--simulate=2 --instructions` compares native oblique geometry; in a native
outline build, `--simulate=2 --renderer --instructions` uses the actual renderer
binding/ownership path. CFF metric checks concern the base charstring box;
transformed ink is checked through the outline. For outline comparisons,
synthetic bold flags 1 and 3 remain diagnostic modes with expected native differences.

The optional `MaiCordAppTextBoundary` target observes the real application profile,
style, emoji and fallback path without a window or graphics device. Use
`app_text_boundary.py prepare|capture|summarize <qa-root>`; pass `--config Debug`
for Debug and `--repeat N` to retain independent captures. Application errors are
recorded as case results, so a successful capture exit means the observation
completed, not that every app case succeeded.

This optional, freestanding QA executable compares the first-party SFNT reader
against DirectWrite in a separate process. It is excluded from default builds
and is not a production dependency. `maicord_font` and `MaiCordFontTests` do not
link DirectWrite or third-party libraries.

Configure the single Release cache with `MAICORD_BUILD_FONT_REFERENCE=ON`, then build
`MaiCordFontReference` and `MaiCordFontTests`. Run:

```powershell
MaiCordFontReference.exe 'C:/Windows/Fonts/seguiemj.ttf'
```

The probe opens every face in the supplied SFNT/TTC file. For each face it checks
all 1,112,064 Unicode scalar values against `IDWriteFontFace::GetGlyphIndices`,
then compares every glyph's raw hmtx advance/bearing with
`GetDesignGlyphMetrics`. It also compares static TrueType and CFF1 outlines with MaiCord's
current `text_outline_glyph` provider: command structure, every float bit
(including signed zero), fill rule and stored control bounds. Exit 0 requires
no unsupported outline faces, no decoding failure, and zero differences in these
comparisons. It does not prove shaping, variation-instance, UVS, color-paint or
rendered-pixel parity. The synthetic native tests separately exercise
default/nondefault cmap format-14 sequences.

Exit 6 includes an unsupported reader case or any comparison failure. Differences
are reported rather than filtered. In particular, raw hmtx bearings can differ
from CFF control-box bearings returned by DirectWrite. The engine must eventually
resolve those outline metrics before replacing rendering geometry. The current
reader intentionally exposes raw table values and must not be presented as a
complete replacement for DirectWrite glyph metrics. The CFF provider now also
returns optional `CffGlyphMetrics`: charstring advances and a control box in
head design units, before normalized-float rounding. The probe separately checks
the rounded advance and outward-rounded box against all corresponding Windows
design-metric fields. All four packaged CFF fonts pass that resolved comparison.
Compagnon, NeoCastel and Sinistre still exit 6 because the probe retains its raw
hmtx/ink comparison as well; the raw differences are not filtered from reports.

Create additional first-party geometry fixtures with:

```powershell
python tools/font_reference/make_outline_fixture.py <scratch-directory>
python tools/font_reference/make_cff_fixture.py <scratch-directory>
```

This development-only writer needs fontTools. The generated 1000- and
2048-units/em fonts each contain 31 glyphs covering off-curve starts, isolated
points, conversion rounding, repeated and nested composites, contour-point
attachments, transform variants, scaled/unscaled offsets, and inherited metrics.
These fixtures are not application fonts or production assets.

The default CFF writer produces six 16-glyph fonts: canonical, rounded-reciprocal,
double-scale, two nonintegral-scale and affine FontMatrix cases. Programs exercise
fractional numbers, hints, local/global subroutines, legacy accents, arithmetic,
stack/transient storage, curves and all four flex operators. The canonical,
rounded, double-scale and both nonintegral-scale fixtures pass current comparisons.
The flex-only fixed-point transform/inverse-transform round trip now matches the
oracle. Windows rejects the
affine face (`reference_face_status` is nonzero), so zero comparisons for that file
must never be interpreted as a pass. Keep that rejected case in the report.

Pass `--expanded` to generate nine 88-glyph fonts. These add isolated
flex/equivalent-cubic pairs, 32 deterministic negative/fractional stress pairs,
ordinary segments after flexes, and three additional scales. The eight opened
fonts have 704 exact outline and resolved-metric comparisons. The remaining
affine font is rejected by the oracle as above.

`tests/cff_outline_test.cpp` independently builds tiny font bytes and checks
analytic geometry, numeric parsing, error atomicity, masks, subroutine cycles,
charset forms, CID FDSelect 0/3 and 20,000 guarded truncations/mutations. CFF1
decoding allocates no heap storage, has a caller-owned path buffer, an operation
budget, 48 operands, 32 transient values and a subroutine-depth limit of 10.
The additional Source Han Sans SC reference covers 65,535 glyphs and 18 CID
dictionaries with FDSelect 3; outlines and resolved metrics agree exactly.
It is an unmodified external test font, not an application asset or runtime
library. General affine transforms, per-FD matrices and random arithmetic still
need further parity coverage.

The first-party reader supports nominal Unicode cmap formats 0, 4, 6, 10, 12,
13 and UVS format 14. Other encodings/formats, including format 8 and symbol
mapping, remain explicit unsupported paths. Table checksums, complete font
sanitization, CFF2 outlines, HVAR/gvar, vertical metrics, layout and shaping remain work.
The native static TrueType outline provider has caller-owned point/contour/path
buffers, a work budget and an explicit depth limit of 64. It does not interpret
hint bytecode. Phantom-point attachments and variable outlines are explicit
unsupported cases; contour-point attachments are implemented.
Prepared views borrow immutable mapping storage; callers must preserve its
lifetime and must not fabricate or mutate prepared views.

The probe now checks native CPAL/COLR table decoding when a COLR table is present.
It compares palette bytes and available COLRv1 roots with DirectWrite, validates
each native graph, and emits a canonical FNV-1a digest of decoded paint fields,
clip boxes and color stops. This is a structural/value diagnostic, not a rendered
pixel comparison. Variable fields and indices are retained. The variation
comparisons described below extend this raw-table diagnostic; native paint
compilation remains required.

Use the independent development parser to compare the decoded field stream:

```powershell
MaiCordFontReference.exe <font-path> > <native-log>
python tools/font_reference/color_table_oracle.py <font-path> --native-log <native-log>
python tools/font_reference/make_color_fixture.py <scratch-directory>/all-formats.ttf
```

`make_color_fixture.py` generates a reproducible first-party variable COLRv1
font covering all 32 paint formats, both clip-box formats, all compact transform
families, unsigned radii, unsorted/tied gradient stops, foreground color, two
palettes and CPAL v1 labels/types. It has an fvar axis and a real ItemVariationStore;
native decoding preserves their references and the separate resolver applies deltas.
The existing `tools/colr_reference/make_fixture.py` supplies 56 additional cases,
including all composite modes.

### Variation resolution

```powershell
python tools/font_reference/make_color_fixture.py <scratch>/all-formats.ttf
python tools/font_reference/make_color_fixture.py <scratch>/variation-stress.ttf --variation-stress
python tools/font_reference/make_color_fixture.py <scratch>/empty-index-map.ttf --empty-index-map
MaiCordFontReference.exe <font-path> > <native-log>
python tools/font_reference/color_table_oracle.py <font-path> --native-log <native-log> --variations
```

For fonts with a COLR ItemVariationStore the probe resolves a fixed normalized
grid: -1, -.75, -1/3, -.25, 0, .125, 1/3, .5, .875, 1. It covers ten instances
for one axis or 100 for two, with axis zero varying fastest. More than two axes
are explicitly unsupported by this QA grid, not by the native scalar evaluator.
The Python comparison uses fontTools' independent VarStoreInstancer. Its digest
includes resolved paint fields, outward clip bounds and sorted color stops.
Standard resolution retains fractional deltas and clamps opacity.

The stress fixture adds a second axis, three interacting regions, mixed-width
delta rows including LONG_WORDS, map reuse and no-variation sentinels. The
empty-map fixture verifies an encoded empty map's identity behavior. All three
generators use fixed timestamps and produce reproducible font bytes.

The probe separately creates Windows instances via IDWriteFontResource and
checks clip rectangles, root-solid alpha and root-gradient stop position/alpha.
It supplies foreground white and palette zero. It now uses native fvar axis
selection and avar v1 normalization from the same float-valued axis requests
sent to Windows. Fonts with more than two axes or avar v2 remain explicitly
unsupported by this Windows probe.

The explicit `ColrVariationMode::paint_reader` policy quantizes normalized
coordinates to 1/65536, accumulates float products in float, rounds the final
delta with floor(delta + .5), preserves raw opacity, and canonicalizes empty
clips. Those measured rules match the retained fixture comparisons. They are
not evidence of complete Windows paint-tree or final pixel equivalence. The
standard policy remains separately compared with the independent parser.

fontTools and DirectWrite are development oracles only; neither is linked into
the first-party native font component.

### Native axis normalization

```powershell
python tools/font_reference/make_color_fixture.py <scratch>/avar-remap.ttf --avar-remap
python tools/font_reference/make_color_fixture.py <scratch>/avar-stress.ttf --variation-stress --avar-remap
MaiCordFontReference.exe <font-path> > <native-log>
python tools/font_reference/axis_oracle.py <font-path> --native-log <native-log>
```

`font_axes.cpp` opens fvar metadata and named-instance coordinates, selects
settings and normalizes through avar v1 maps. It retains immutable byte views
and caller-owned map/output arrays. It preserves axis/instance record strides,
clamps settings, supplies defaults, and handles degenerate ranges. Missing
required avar anchors disable that axis's map; invalid ordering is rejected.
avar v2 returns unsupported and is not silently treated as v1.

The default precision mode rounds input and intermediate operations to 16.16.
Separate 2.14 outputs are available for feature conditions. Paint-reader mode
preserves float user inputs before default normalization, with the same 16.16
normalization/remapping steps. The Windows fixtures establish selected COLR
reader values only; they do not prove other Windows consumers' numeric behavior.

The probe emits per-axis requested float bits, biased 16.16 results and biased
2.14 conditions. `axis_oracle.py` uses independently parsed fvar/avar data and
exact Python fractions for its arithmetic. It requires every grid record and
rejects duplicate/missing results. The two-axis limit belongs to this QA grid;
the core handles the font's axis count subject to caller storage/work budgets.

### Paint-program compiler comparison

The probe now compares `colr_program_build` with the production
`colr_paint_build` for every COLRv1 base glyph at 12, 16, 20, 24, 32, 48, 64, 128,
15.5 and 31.25 pixels/em. It checks every used upload byte and the program's
metadata after the arena descriptor. The arena pointers/allocation history are
not compared. Every build must succeed; shared failure is not a pass.

The experimental compiler takes first-party paint elements through callbacks.
`program_oracle.cpp` translates the Windows reader's values without recomputing
them; native TrueType/CFF providers supply the new compiler's outlines. Thus the
program comparison verifies the compiler/outline boundary, not native paint
resolution. Program checks use the face's default instance. The separate axis
and selected Windows variation checks retain their earlier grid behavior.

The native TrueType COLR policy preserves the old simple-color fast path:
normalize before computing implied points, retain single-point contours and use
rounded 2/3. Ordinary text preserves its separate precise subtraction constant.
Composite/large glyphs retain the text policy, following the existing fallback
boundary. This distinction is required by MaiCord's current app output.

`maicord_paint_program` is first-party and DirectWrite-free. Production still
links the old compiler; only QA links the experimental one. The shaders have
not changed. Neither byte-equivalent programs nor green component tests alone
claim executed pixel equivalence, performance gains or a smaller shipping app.

### Fully native paint source

The probe also compares `colr_source_begin` plus native compilation/outlines with
the existing renderer. It checks resolved element bytes throughout the tree,
then every used program byte and program metadata. Gradient-stop values are
covered by the program bytes. Element traversal failures are separate failures;
an incomplete traversal cannot count as equality.

```powershell
MaiCordFontReference.exe <font-path> --native-source
MaiCordFontReference.exe <font-path> --native-source-grid
python tools/font_reference/make_color_fixture.py <scratch>/map-last-active.ttf --variation-stress --map-last-active
```

`--native-source` skips the previously verified Windows-adapter compiler check
and tests native-source default programs at 32 pixels/em. `--native-source-grid`
uses all ten sizes listed above. The log reports the adapter as disabled rather
than treating zero adapter comparisons as new evidence. Both modes also check
whole native programs at each selected Windows variation instance, at 32
pixels/em. Without either option, the older adapter grid remains enabled and
the native default-program check uses 32 pixels/em.

The native source reads all 32 formats, emits legacy layer events, preserves
source/backdrop order, folds glyph-plus-solid leaves and resolves palettes,
gradients, clips and compact/general transforms. It uses supplied immutable
views and caller-owned frame/stop storage, with cycle/depth/work limits.

The paint-reader policy additionally preserves measured Windows details:
reciprocal palette normalization, signed-zero basis conversion, float radians,
short sine/cosine kernels, signed encoded radial radii, and the nonuniform
VarScaleAroundCenter behavior where both center deltas change X. In that policy,
per-field variation indices wrap before map lookup; a base of 0xffffffff does
not suppress every subsequent field. The standard policy retains its earlier
no-variation/overflow behavior and correct center/radius semantics. The active
last-map fixture specifically verifies wrapping when the last map entry has
real deltas, avoiding an accidental pass from a no-variation last entry.

The source and its scalar math are first-party code. Neither the CRT nor an
external font/math library is linked into the native component. Windows/UCRT
behavior is observed only by development probes. Current corpus agreement does
not prove all floating-point inputs or all Windows/hardware versions.

Actual GPU pixels, production integration, variable outlines and the final
resource/performance gates remain separate work.

## Common OpenType layout tables

`layout_table_oracle.py` observes fontTools decompilation of Coverage, ClassDef,
Device and VariationIndex subtables. It captures original table slices and
independent expected mappings; it does not recompile the real-font data.
Repeated references to the same face/table/offset/type are deduplicated.
The separate native verifier links only first-party libraries and Kernel32.

```powershell
python tools/font_reference/layout_table_oracle.py --out corpus.bin <font.ttf> <font.otf> <collection.ttc>
python tools/font_reference/make_layout_fixtures.py --out synthetic.bin
cmake --build <build> --config Release --target MaiCordLayoutTableTests
MaiCordLayoutTableTests.exe corpus.bin
MaiCordLayoutTableTests.exe synthetic.bin
```

With no corpus argument, CTest runs native units and guard-page mutation checks
only. Explicit corpus runs are needed for fontTools comparisons. Check native
exit codes and retain the binary corpora, JSON identities and logs together.
Synthetic records are labeled separately from original font records.

Prepared views borrow mapped bytes and do not execute substitutions or positioning.
Coverage/class counts and references still need validation by their lookup owner.
VariationIndex records are returned distinctly; the pixel-delta query rejects
them rather than silently treating font-unit variation as a pixel correction.

## GDEF and mark filtering

`gdef_oracle.py` captures original GDEF bytes, decoded fontTools expectations,
and three labeled synthetic table fixtures. The input manifest is the `fonts`
list from `layout_table_oracle.py`; file hashes and glyph counts must match.
The native executable uses only first-party code and Kernel32.

```powershell
python tools/font_reference/gdef_oracle.py --font-manifest layout-corpus.json --out gdef-corpus.bin
cmake --build <build> --config Release --target MaiCordGdefTests
MaiCordGdefTests.exe gdef-corpus.bin
```

CTest without a corpus argument runs native units and protected-page mutations.
Explicit corpus runs compare classes, attachment points, carets, mark sets,
variation-store metadata and filter decisions. This does not execute lookups
or compare final caret pixels.

## GSUB/GPOS directories and feature variations

`layout_directory_oracle.py` captures original tables and fontTools decompilation
positions for scripts, languages, features and resolved lookup subtables. It
checks backing bytes against the originals and never recompiles real tables.
Four labeled synthetic tables add variation records; supported-only fixtures
also check the separate Python selection evaluator against fontTools' instancer.
Compatibility-edge expectations use decoded objects and explicit spec rules.

```powershell
python tools/font_reference/layout_directory_oracle.py --font-manifest layout-corpus.json --out directory-corpus.bin --fixture-header tests/generated/layout_directory_fixture.inc
cmake --build <build> --config Release --target MaiCordLayoutDirectoryTests
MaiCordLayoutDirectoryTests.exe directory-corpus.bin
```

The fixture header is generated QA data. No fontTools code or runtime is shipped.
With no corpus argument, the native test runs exhaustive coordinate units and
observable protected-page mutations. Reading and selecting lookup data does not
execute glyph substitutions or positioning.

## Basic GSUB matching

`gsub_oracle.py` captures original GSUB/GDEF tables and fontTools mappings for
single, multiple, alternate and ligature substitutions. An independent Python
evaluator uses decoded ligature rules and GDEF filters to produce input-position
and output expectations. No original table is recompiled and no native matcher
is used to generate expectations.

```powershell
python tools/font_reference/gsub_oracle.py --font-manifest layout-corpus.json --out gsub-corpus.bin
cmake --build <build> --config Release --target MaiCordGsubTests
MaiCordGsubTests.exe gsub-corpus.bin
```

The native verifier checks every font glyph for the simple mapping types, all
alternate choices and generated ligature probes. With no path it runs native
fixtures and observable protected-page mutations. fontTools remains offline QA
only. These comparisons validate matching and returned edits, not a complete
Windows shaping run.

## Contextual and reverse matching

`context_oracle.py` scans the recorded fonts for GSUB/GPOS contextual subtables,
captures their original bytes/positions, and generates expectations through a
separate evaluator over fontTools-decoded patterns. Seven standalone fixtures
cover the formats absent from the original corpus, including reverse substitution.
All scanned font identities are recorded, including faces without relevant tables.

```powershell
python tools/font_reference/context_oracle.py --font-manifest layout-corpus.json --out context-corpus.bin --fixture-header tests/generated/context_fixture.inc
cmake --build <build> --config Release --target MaiCordContextTests
MaiCordContextTests.exe context-corpus.bin
```

Native comparisons include input positions, ordered nested actions, feature-range
boundaries, reverse outputs and prepared-storage requirements. These are matching
results; nested lookups are not executed. No-path CTest runs units and observable
protected-page mutations. fontTools remains offline QA only.

## Glyph gap-buffer interoperability

`MaiCordGlyphBufferTests` compares full-record edits against a separate flat-array
model and checks index/boundary maps, failure atomicity and a streaming expansion
copy count. It needs no external corpus. The GSUB/contextual corpus verifiers now
also test every input split with strided records around a poisoned gap through
`tests/glyph_view_check.hpp`.

```powershell
cmake --build <build> --config Release --target MaiCordGlyphBufferTests MaiCordGsubTests MaiCordContextTests
MaiCordGlyphBufferTests.exe
MaiCordGsubTests.exe gsub-corpus.bin
MaiCordContextTests.exe context-corpus.bin
```

Those corpus inputs and independent expectations are unchanged. View equivalence
does not establish nested-action rebasing or Windows cluster parity; those remain
separate execution checks. The copy-count test is not a wall-clock performance
benchmark.

## Windows nested-GSUB execution observations

`gsub_execution_oracle.py` creates first-party TrueType fixtures with explicit
GSUB actions and GDEF classes. The custom `test` feature schedules only lookup
zero. `MaiCordGsubExecutionReference` loads each font directly, obtains the script
through `AnalyzeScript`, and records `GetGlyphs` output with the feature disabled
and enabled. It captures glyph IDs, UTF-16 cluster maps, documented shaping
properties and the actual loaded DirectWrite module path. It accepts a single
script run in this bounded Latin/LTR/en-US harness.

```powershell
python tools/font_reference/gsub_execution_oracle.py generate <fixtures>
cmake --build <build> --config Release --target MaiCordGsubExecutionReference
python tools/font_reference/gsub_execution_oracle.py record <fixtures> --exe <bin>/MaiCordGsubExecutionReference.exe --out <observations>.json
```

The optional target needs `MAICORD_BUILD_FONT_REFERENCE=ON`; DirectWrite and
fontTools remain QA dependencies only. Each font has a decoded TTX description
and recorded hash. Disabled-feature runs assert nominal input glyphs; ten simple
enabled controls assert activation, filtering, stopping and scan continuation.
The remaining results are measured observations, not Python shaping predictions.
Release and Debug agree on all 100 enabled and 100 disabled runs.

## Native nested-GSUB execution

The fixture generator now also covers contextual formats 1/2, alternate and
reverse substitution, extension wrappers, edits starting outside an ancestor's
matched membership and ligature class propagation. `export` retains original
GSUB/GDEF bytes and measured Windows outputs in a portable native test corpus:

```powershell
python tools/font_reference/gsub_execution_oracle.py generate <fixtures>
python tools/font_reference/gsub_execution_oracle.py record <fixtures> --exe <bin>/MaiCordGsubExecutionReference.exe --out <windows>.json
python tools/font_reference/gsub_execution_oracle.py export <fixtures> --observations <windows>.json --out <corpus>.bin --header tests/generated/gsub_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGsubExecuteTests
MaiCordGsubExecuteTests.exe <corpus>.bin
```

The native target imports Kernel32 only. With no path, it runs the embedded
first-party corpus; both modes also run capacity/work/cycle checks and protected
page mutations. It compares glyph IDs at every initial gap position. It does not
claim cluster, placement or pixel parity.

The corpus explicitly retains one Windows difference: extension-wrapped reverse
substitution. Its native expectation is the unwrapped lookup's recorded result,
as required by OpenType extension equivalence; its differing Windows observation
is also checked. It is not silently counted as Windows parity.

## Native cluster boundary updates

`export-clusters` adds source indices, initial boundaries, final character-to-glyph
maps and cluster-start flags to the measured fixture corpus:

```powershell
python tools/font_reference/gsub_execution_oracle.py export-clusters <fixtures> --observations <windows>.json --out <clusters>.bin --header tests/generated/cluster_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordClustersTests
MaiCordClustersTests.exe <clusters>.bin
```

Initial boundaries come from the recorded feature-disabled Windows run. The native
test verifies changes caused by GSUB; it does not claim independent initial Unicode
or script grouping. `MaiCordClustersTests` also compares the hierarchical boundary
bitmap against a dense interval model, tests protected-page storage and checks
glyph/bitmap consistency under every work threshold of a ligature fixture.
It imports Kernel32 only.

## Native GPOS values, anchors and basic matching

`gpos_oracle.py` decodes original GPOS/GDEF bytes from a hash-checked font manifest.
It captures immediate-parent record offsets and independent value, anchor and
single/pair matching expectations. A labeled first-party fixture supplies device,
variation and uncommon anchor formats. fontTools is an offline QA dependency.

```powershell
python tools/font_reference/gpos_oracle.py --font-manifest <layout-corpus>.json --out <gpos-corpus>.bin --fixture-header tests/generated/gpos_fixture.inc
cmake --build <build> --config Release --target MaiCordGposTests
MaiCordGposTests.exe <gpos-corpus>.bin
```

With no path, the native test uses the embedded synthetic fixture; both modes
also run units and protected-page mutations. Full original-font coverage requires
the separate corpus argument. The executable imports Kernel32 only. These checks
establish table interpretation and basic matching, not final Windows placement or
pixel parity.

## Native GPOS attachment data

`gpos_attachment_oracle.py` captures original GPOS/GDEF bytes and independent
glyph, local mark-class, ligature-component and anchor expectations. Its labeled
synthetic fixture adds direct/extension forms, shared subtables, missing anchors,
three classes and multiple components. fontTools remains an offline QA dependency.

```powershell
python tools/font_reference/gpos_attachment_oracle.py --font-manifest <layout-corpus>.json --out <attachment-corpus>.bin --fixture-header tests/generated/gpos_attachment_fixture.inc
cmake --build <build> --config Release --target MaiCordGposAttachmentTests
MaiCordGposAttachmentTests.exe <attachment-corpus>.bin
```

The no-path/CTest mode uses the embedded first-party fixture. Both modes exercise
malformed-data cases, work thresholds and protected-page mutations. The full
original-font corpus requires the explicit file argument. Checks establish data
access only, not sequence attachment selection or final Windows placement.

## Windows GPOS execution observations

`gpos_execution_oracle.py` creates first-party fonts with isolated GPOS scheduling
and optional always-on GSUB ligatures. The optional `MaiCordGposExecutionReference`
target shares the GSUB harness source through `MAICORD_PLACEMENT_REFERENCE` and
adds GetGlyphPlacements with exact binary32 outputs.

```powershell
python tools/font_reference/gpos_execution_oracle.py generate <fixtures>
cmake --build <build> --config Release --target MaiCordGposExecutionReference
python tools/font_reference/gpos_execution_oracle.py record <fixtures> --exe <bin>/MaiCordGposExecutionReference.exe --out <windows>.json
```

The target requires `MAICORD_BUILD_FONT_REFERENCE=ON`. Its CLI takes font path,
comma-separated hex scalars, feature enable flag, decimal binary32 font-size bits,
RTL flag and sideways flag. The recorded corpus uses one Latin run, en-US,
horizontal placement, both RTL flags and sizes 1000/16/13.25/17.3 DIPs. Float bits
are authoritative; decoded numbers aid inspection. fontTools and DirectWrite are
reference dependencies only. These observations do not establish native placement
or app pixel parity.

## Native basic positioning execution

`export-basic` selects the measured fixtures containing only single/pair GPOS
lookups, including extensions. It retains full font bytes, scheduled lookup indices,
recorded glyphs/shaping flags and exact disabled/enabled Windows placement bits.

```powershell
python tools/font_reference/gpos_execution_oracle.py export-basic <fixtures> --observations <windows>.json --out <positions>.bin --header tests/generated/gpos_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGposExecuteTests
MaiCordGposExecuteTests.exe <positions>.bin
```

Without a path, the native target uses its embedded corpus. Both modes also check
work thresholds, pair preflight, fractional variation corrections and protected
storage. The native executable imports Kernel32 only. Initial glyphs and shaping
flags remain reference inputs; these tests do not prove independent script policy,
attachment execution, Windows variable placement or app pixel parity.

## Native mark positioning execution

`export-marks` retains measured fixtures whose lookups are single/pair,
mark-to-base or mark-to-mark, including extensions. The combined native target
shares the scalar test source through `MAICORD_MARK_POSITION_TESTS`, preserving
the original basic target and its corpus.

```powershell
python tools/font_reference/gpos_execution_oracle.py export-marks <fixtures> --observations <windows>.json --out <marks>.bin --header tests/generated/gpos_mark_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGposMarkExecuteTests
MaiCordGposMarkExecuteTests.exe <marks>.bin
```

The no-path mode uses the embedded corpus. Native tests compare exact floats
under the explicit Windows null-target compatibility policy; separate units check
strict skipping, bounded reads, callbacks and variable-data preflight. A long
mark sequence checks rolling-search complexity. Glyphs, effective classes and
shaping flags remain reference inputs; ligature provenance, cursive/nested
execution and complete app parity are not established.

## Native cursive execution

`export-cursive` retains measured root single/pair, cursive and mark-to-base/
mark-to-mark fixtures. The combined native target adds
`MAICORD_CURSIVE_POSITION_TESTS` to the shared positioning test source.

```powershell
python tools/font_reference/gpos_execution_oracle.py export-cursive <fixtures> --observations <windows>.json --out <cursive>.bin --header tests/generated/gpos_cursive_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGposCursiveExecuteTests
MaiCordGposCursiveExecuteTests.exe <cursive>.bin
```

Without a path, it uses the embedded corpus. Both modes check long chains,
direction-dependent arithmetic, bounded scratch, stale-bit reuse and failed
unresolved attempts. Glyphs and shaping flags remain reference inputs. Cursive
root ranges, variable/hinted behavior and complete shaping/app parity are separate
contracts.

## Ligature-component observations

`generate` now includes 32 `components-*` fonts with distinct component anchors,
explicitly scheduled GSUB stages and selected mark-to-mark follow-up lookups.
`record` captures 1,280 pairs across all 160 fonts. The component cases remain
reference-only: native `export-basic`, `export-marks` and `export-cursive` omit
their GPOS5 lookups because native mark-to-ligature execution is unfinished.

The case named `components-delete-intervening-base` deliberately has prohibited
zero-length MultipleSubst output. Its `invalid_input` metadata prevents treating
the observation as valid glyph-deletion semantics. The native validator already
rejects that construct.

The native cluster tests now cover boundary restoration and interleaved membership
export, including the measured decomposition character map. They do not derive
component ownership or Windows glyph-start flags.

## Native ligature attachment execution

`export-ligatures` selects all supported root GPOS types 1 through 6 and writes
explicit attachment seeds for the ligature fixtures. Owner slots resolve through
a sorted target-glyph directory. The seed function assigns first-party fixture
inputs; it does not implement GSUB provenance. Unknown history has a separate
component sentinel because the measured null-anchor behavior differs.

```powershell
python tools/font_reference/gpos_execution_oracle.py export-ligatures <fixtures> --observations <windows>.json --out <ligatures>.bin --header tests/generated/gpos_ligature_execute_fixture.inc
cmake --build <build> --config Release --target MaiCordGposLigatureExecuteTests
MaiCordGposLigatureExecuteTests.exe <ligatures>.bin
```

The combined target retains the earlier scalar, mark and cursive units and adds
component selection, owner restrictions, bounded exact distance caching, ordered
fallback, point resolution and work/capacity checks. No-path mode uses the embedded
corpus. Earlier exporters retain their formats and scope.

## Native component history from scalar inputs

`component_shaping_oracle.py` exports the valid ligature-positioning fixtures with
scalar inputs and expected glyphs, character maps and placements. It supplies no
attachment seeds. The test maps scalars through native cmap, executes the fixture's
GSUB feature schedule with component history, exports associations and runs GPOS.

```powershell
python tools/font_reference/component_shaping_oracle.py <fixtures> --observations <windows>.json --out <components>.bin --header tests/generated/component_shaping_fixture.inc
cmake --build <build> --config Release --target MaiCordComponentShapingTests
MaiCordComponentShapingTests.exe <components>.bin
```

Initial predecessor affinity/class grouping is explicit test policy; final shaping
flags are still recorded inputs. The suite additionally checks AVL order/rank,
bounded mutation journals, combined glyph/tree/cluster commits and sealed affinity
caching. It is not independent general script shaping or app parity.

## Native positioning flags

The positioning-flag milestone selected all valid fixtures using root GPOS1-6.
`MaiCordComponentShapingTests` now seeds initial source clusters with native
Unicode grapheme boundaries and computes positioning flags from final classes
and source-cluster transitions. Recorded flags are comparisons only; they are
not copied into positioning state.

The corpus format remains compatible with the earlier 63-font corpus. Running
that file now additionally compares its flags. The expanded corpus has 172 fonts
and retains the existing command line. General script policies remain unfinished.

## Native contextual GPOS program

The current component exporter also includes GPOS7/8 context/chaining fixtures.
`MaiCordComponentShapingTests` prepares the complete native positioning program
and follows caller-selected lookup schedules. Inputs are scalars and original
font bytes; Windows glyphs, clusters, flags and placements remain expectations.
The corpus format and command line are unchanged. Offline fontTools generates
first-party fixture data only and is not a shipping dependency.

The 225-font Windows reference supplies 224 valid native cases; one prohibited
zero-output GSUB MultipleSubst case remains excluded. Context fixtures exercise
formats 1/2/3, extension wrapping, first-match blocking, nested ranges/filtering,
ordered marks/ligature attachment and directional cursive connection replacement.
Native units also check bounded cycles, work/capacity limits and guarded buffers.

## Exact reverse-cursive coalescing

The current reference adds two long contexts (32 and 64 glyphs) and a 64-glyph
chain interrupted by a position adjustment. It now contains 228 Windows fonts,
with 227 valid scalar-to-placement cases in the native corpus.
`MaiCordCursiveDeferredTests` compares eager and coalesced execution, checks
failure boundaries/callback visibility, and runs a 4,096-glyph contextual program.
Its optional `--benchmark` mode records 31 alternating-order timing samples per
implementation after warmup, including equal state initialization.

Coalescing reuses position slots and adds 24 bytes of local state. It requires
bounded integer arithmetic; it does not approximate fractional positions. The
existing 8-byte per-glyph connection state is unchanged.

## Feature policy and revised app scope

`feature_plan_oracle.py` and the QA-only `MaiCordFeaturePlanReference` measure
installed Windows default features, required features, lookup ordering,
deduplication, parameter priority and script/language fallback. The reference
accepts `default` or comma-separated four-character `tag=value` options, followed
by em-size bits, direction/sideways flags and a locale. Existing GSUB/GPOS
reference command lines retain their behavior.

The target is limited to features MaiCord requires. The broader
typography/locale probes are diagnostic evidence, not shipping requirements.
`maicord_scope_audit.py` records actual app font choices, relevant call sites and
font formats/axes. The app needs its default shaping and independent-letter
mode, not an arbitrary typography-control API.

## Native Latin lookup plan

`lookup_plan_oracle.py` generates 164 first-party fonts for the two MaiCord
typography modes, then records both full TextAnalyzer shaping and TextLayout
output. The reference's optional final `layout` argument also records
GetTextComplexity. An ordinary invocation without that argument is unchanged.
The embedded `MCPLN03` corpus tests full shaping and the one-scalar nominal gate;
separate `layout-differences.json` retains differences between the two Windows
API layers. The native test derives the gate from font data, not recorded flags.

```powershell
cmake --build <build> --config Release --target MaiCordFeaturePlanReference
python tools/font_reference/lookup_plan_oracle.py <output> --exe <bin>/MaiCordFeaturePlanReference.exe --header tests/generated/lookup_plan_fixture.inc
cmake --build <build> --config Release --target MaiCordLookupPlanTests
MaiCordLookupPlanTests.exe
```

The native planner owns no memory and supports only the measured horizontal
Latin/en-US full-shaping profile. Tests cover selected lookup execution,
required/duplicate features, script/language selection, variation replacements,
word boundaries, bounded work, short buffers and guard pages. The planner does
not decide general TextLayout range eligibility or other script stages.

## Font participation and nominal eligibility

`complexity_oracle.py` generates 88 first-party fonts, records 4,312 Windows
observations and exports a native bitset corpus. Its optional `--header` can
export existing observations without running the reference again. Uppercase
ASCII aliases expose glyph participation; glyph zero is excluded from the
bitmap comparison because missing-glyph handling belongs to the text caller.

```powershell
python tools/font_reference/complexity_oracle.py <output> --exe <bin>/MaiCordFeaturePlanReference.exe --header tests/generated/nominal_participation_fixture.inc
cmake --build <build> --config Release --target MaiCordNominalParticipationTests
MaiCordNominalParticipationTests.exe
MaiCordNominalParticipationTests.exe <font-file>
```

The font-file mode emits a combined GSUB/GPOS bitmap and status, allowing
`nominal_app_fonts.py` to compare actual app faces. `nominal_cmap_probe.py`
isolates character-map effects while keeping lookup data fixed. These probes
show that table participation alone is insufficient: cmap-dependent context
pruning, Unicode eligibility and FeatureVariations remain required work.

## Native nominal source characters and cmap pruning

`nominal_scalar_oracle.py` builds neutral first-party cmap 12/13 fonts with all
valid BMP scalars mapped to one glyph and no layout tables. The reference's
`bmp-complexity` mode verifies every mapping and records intrinsic eligibility.
Two cmap formats and two glyph IDs must agree. This profile is calibrated to
the recorded Windows reference; it is not general Unicode property data.

```powershell
python tools/font_reference/nominal_scalar_oracle.py <output>/scalars --exe <bin>/MaiCordFeaturePlanReference.exe --header src/text/generated/nominal_scalar_data.inc --test-header tests/generated/nominal_scalar_fixture.inc
cmake --build <build> --config Release --target MaiCordNominalSourcesTests MaiCordNominalParticipationTests
MaiCordNominalSourcesTests.exe
python tools/font_reference/nominal_app_fonts.py --scope <scope.json> --native <bin>/MaiCordNominalParticipationTests.exe --reference <bin>/MaiCordFeaturePlanReference.exe --out <output>/app-printable.json --ascii-printable
```

`sfnt_nominal_sources` walks native cmap ranges to construct the reachable-glyph
bitmap. Supplying it to the participation pass prunes impossible format-3
contexts and ligatures. Font-file mode now uses that native source bitmap;
empty-source table-only calls remain available for diagnostics. The cmap probe's
`--native` option compares the native result, and `--ligature` selects the
corresponding ligature case.

## Required normal/semibold feature variations

`nominal_variations_oracle.py` creates seven first-party variable fonts and
records 84 default/independent-letter cases at weights 400/600. The feature
reference accepts an optional weight after `layout`, selects an explicit face
instance and records the actual face and TextLayout run weight bits. Unsupported
weights are rejected. The native corpus compares base-feature eligibility,
selected variation replacements, full glyph/placement output and scoped
one-word TextLayout behavior; it does not implement general range segmentation.

```powershell
python tools/font_reference/nominal_variations_oracle.py <output> --exe <bin>/MaiCordFeaturePlanReference.exe --header tests/generated/nominal_variations_fixture.inc
cmake --build <build> --config Release --target MaiCordNominalVariationsTests
MaiCordNominalVariationsTests.exe
python tools/font_reference/nominal_app_fonts.py --scope <scope.json> --native <bin>/MaiCordNominalParticipationTests.exe --reference <bin>/MaiCordFeaturePlanReference.exe --out <output>/app-printable.json --ascii-printable --variable-weights
```

Nominal participation deliberately reads base features. Full shaping uses
FeatureVariations selected for the instance. The font-file eligibility bitmap
can therefore be shared across the required weights.

## Native metric variations

The feature reference's `metrics 400` or `metrics 600` mode uses an explicitly
selected face and emits its actual weight, ten font-wide metric fields and all
horizontal design advances. Existing `layout` and `bmp-complexity` modes retain
their outputs. The metric array is units/em, ascent, descent, line gap, cap
height, x-height, underline position/thickness, strikeout position/thickness.

```powershell
python tools/font_reference/metric_variations_oracle.py <output> --exe <bin>/MaiCordFeaturePlanReference.exe --header tests/generated/metric_variations_fixture.inc
cmake --build <build> --config Release --target MaiCordMetricVariationsTests
MaiCordMetricVariationsTests.exe
MaiCordMetricVariationsTests.exe C:/Windows/Fonts/bahnschrift.ttf
```

Six first-party fonts isolate implicit/mapped HVAR indices, signed/fractional
deltas, half-unit rounding, compressed hmtx tails and empty MVAR. The native
corpus embeds those fixtures only; the installed Bahnschrift file is read for
separate differential checks. Font-file mode emits weights 400/600, four
decoration fields and all advances, using native readers throughout. Decoration
field rounding currently lives in the QA harness pending native layout metric
ownership.

## Native gvar outlines

`MaiCordFontReference <font> --gvar400` / `--gvar600` compares native variable
outlines with normalized Windows outlines for an explicitly verified weight.
These modes skip unrelated color/cmap/metric passes. Appending `-dump` emits
`gvar_outline=` rows with the Windows glyph ID, command count, fill rule, bounds
flag, four bound float bits and seven words per command (six coordinate float
bits and kind). Existing static and native-color modes remain separate.

```powershell
python tools/font_reference/gvar_outline_oracle.py <output> --exe <bin>/MaiCordFontReference.exe --header tests/generated/gvar_outline_fixture.inc
cmake --build <build> --config Release --target MaiCordGvarOutlineTests
MaiCordGvarOutlineTests.exe
MaiCordFontReference.exe C:/Windows/Fonts/bahnschrift.ttf --gvar400
MaiCordFontReference.exe C:/Windows/Fonts/bahnschrift.ttf --gvar600
```

Ten first-party fonts isolate sparse/explicit deltas, one-point contours of
influence, fractional and overlapping regions, component translations, nested
composites and scaling. The generated native corpus contains those fonts and
Windows expectations only. The real Bahnschrift file is read separately and
is not embedded.

## Prepared native geometry and instruction probes

`MaiCordFontReference <font> --geometry` compares all glyphs through the unified
`FontGeometry`/`FontGeometryInstance` provider against Windows normalized outlines.
It skips unrelated color/cmap/metric passes, while preserving CFF resolved-ink
checks. `--geometry-dump` adds the same outline-record format as gvar dumps.

```powershell
MaiCordFontReference.exe <font> --geometry
python tools/font_reference/outline_instruction_oracle.py <output> --source <first-party-gvar-full.ttf> --exe <bin>/MaiCordFontReference.exe
```

The instruction probe removes variation tables from the first-party source and
adds its own fpgm, prep, CVT and glyph instructions. It captures empty, SHPIX,
MDAP, IUP and INSTCTRL cases. Some deliberately differ from the current native
provider: they are evidence of required missing behavior, not passing native
conformance tests. No Windows font is embedded in these fixtures.

Add `--context --fixture tests/generated/tt_interpreter_fixture.inc` to capture
60 expanded instruction cases and emit first-party bytecode/Windows coordinate
expectations. `MaiCordTtInterpreterTests` directly executes the native core and
compares these bits; it also exercises work/capacity boundaries and guarded
program mutations. An optional font path runs native font/preparation programs
and reports phase, status, work, offset, opcode, stack and call depth. That
diagnostic mode does not compare program state with Windows or execute glyph
programs.

`MaiCordFontReference <font> --instructions` prepares the native instruction
context and applies glyph programs through `FontGeometry`. Add `-dump` to emit
Windows command records. This initial mode handles static translated contexts;
unsupported positioning or component transforms remain explicit failures.
`tt_outline_fixture.py --directories <first-party-font-dirs> --exe <oracle>
--root <output> --header tests/generated/tt_outline_fixture.inc` embeds complete
first-party fonts and Windows outlines. `MaiCordTtOutlineTests` checks that corpus
with native preparation/decoding, state isolation and failure/work boundaries.

`tt_position_oracle.py` generates the dedicated positioning/vector corpus from
the first-party empty instruction font. `MaiCordTtPositionTests` runs it through
the same native geometry adapter, including reversed root order, preparation
state isolation and exact work boundaries. The reference's
`--instructions-state=<glyph-id>` selects one simple glyph and emits native
current/original fixed-point coordinates and touch bits alongside Windows
commands. Composite/CFF targets and invalid IDs are rejected; empty programs do
not produce VM state rows.

The expanded corpus has 283 fonts covering signed projection ties, MIRP cut-in
precision, short/long and reversed IUP spans, IP with different original/current
vectors, translated points and odd-width phantom points. State dumps also expose
the remaining stack and prepared function bytecode; parsers should ignore all
`tt_` rows when reading numeric comparison summaries. All four audited Georgia
styles now match exactly.

Place `--owned` before a comparison mode to acquire the font through the native
file/face store, for example `MaiCordFontReference <font> --owned --instructions`
or `--owned --gvar600`. The mode uses the store's prepared cmap and geometry;
Windows remains the offline comparison oracle. Owner rows report mapping
lifetimes, and the final release must leave no retained files/faces. Native
ownership tests cover file aliases, collections, stale handles, overflow,
capacity failures, sharing restrictions and cleanup.

`--owned` now prepares a retained native instance as well as the file/face.
Its normalized coordinates, shared tuple scalars and optional TrueType state
come from the instance owner; per-glyph mutable arrays remain reusable scratch.
`owner_instance_state_bytes` reports committed prepared-state bytes, excluding
file mappings and caller-owned slot/scratch storage. `MaiCordFontInstanceTests`
checks the existing instruction and variable-font fixtures through these owned
lifetimes, with read-only state, released caller handles, concurrent live axis
instances and failure/work-budget boundaries.

The instructed path now covers all 40 audited app files. Use `--owned
--instructions` uniformly for static/default faces, and `--owned --instructed
--gvar600` (or 400) for a varied face with preparation enabled. Variable glyph
instructions must be disabled by preparation; active variable instructions and
cvar remain unsupported. CFF keeps its native outline provider. The additional
`owner_instance_reserved_bytes` field records the reservation, while
`owner_instance_state_bytes` records retained commit after unused preparation
pages are decommitted.

## Simulation metrics and bold geometry probes

`--simulate=1 --points --instructions-dump` additionally captures native TrueType
points and base commands alongside Windows bold commands. It rejects `--owned`
and `--renderer`, and explicitly reports missing-function prep recovery. The
`bold_direction_oracle.py`, `bold_point_model.py`, `bold_app_points_oracle.py` and
`bold_preparation_oracle.py` tools support diagnostic research only.

`MaiCordFontReference <font> --simulate=0|1|2|3 --metrics-dump` compares native
simulation of resolved base design metrics and emits every reference glyph's
seven fields. The oracle supplies the unsimulated base; this mode does not claim
native base-metric extraction or simulated outline support. Its metric difference
counter contributes to the process exit status.

`simulation_metrics_oracle.py` captures all default faces in an app inventory.
`bold_geometry_oracle.py` generates first-party outlines and metrics, with optional
`--metrics-header` for the native regression fixture.

The `--points` diagnostic also emits `native_contour_directions=glyph,status,work,count,flags...`
and the retained preparation recovery flag/instance size. Direction comparisons
are meaningful for original integer simple contours; the tool's current points
can already be assembled, positioned or obliqued. This diagnostic does not enable
native bold or substitute assembled-contour classification for component policy.
`bold_orientation_oracle.py` records Windows flags/probes in disposable debugger
processes. `orientation_first_party.py` generates the native fixture, and
`orientation_native_oracle.py` compares native flags with those frozen captures.
`prep_instance_oracle.py` checks the retained preparation policy separately.

Static TrueType `--simulate=1` and `--simulate=3` now exercise integrated native
bold and bold-oblique geometry. `--renderer --instructions` exercises the actual
renderer binding as well. `bold_native_matrix.py` captures both styles for the
observed required fonts; `bold_outline_fixture.py` generates first-party nested,
overlapping and reflected-component regression fixtures.
`--points` now contains styled coordinates when simulation is requested. Use
simulation zero for standalone original-contour diagnostics. The earlier
bold-point model analysis uses its frozen pre-integration captures.
