# Native Unicode data and text analysis

The native implementation is first-party C++ with generated read-only Unicode
data. It does not link ICU, utf8proc, HarfBuzz or another Unicode/text library.
Python and the official archives are offline development inputs only. Normal
CMake builds use the checked-in generated files and require neither Python nor
a network connection.

## Data generation

The generator pins the SHA-256 of the official [Unicode 17.0.0 UCD archive](https://www.unicode.org/Public/17.0.0/ucd/UCD.zip).
It processes range-specific `@missing` defaults before explicit assignments,
canonicalizes property aliases, retains the full code-point range and applies
Script_Extensions' singleton-Script default. See [UAX #44](https://www.unicode.org/reports/tr44/tr44-36.html)
for the UCD conventions. The data uses the existing
`assets/licenses/Unicode-3.0.txt` notice.

```powershell
python tools/unicode/generate.py --ucd <UCD.zip> --report <generation.json>
```

This writes the property enum/record header, the native lookup tables and the
official grapheme-test vectors. `--repo <scratch-root>` supports isolated
regeneration. The report records input hashes, enum labels and bit positions,
all layout candidates, generated-file hashes and test counts.

Eleven enumerated/numeric fields and eight flags share 1,653 distinct 64-bit
records. Script extensions contribute only Arabic membership; East Asian Width
contributes only membership in F/W/H, as required by the current consumers.
Mirror and bracket mappings are sparse sorted arrays. A three-level index
shares identical pages and marks uniform spans with a direct record ID. Uniform
spans bypass lower levels. A 128-entry ASCII index avoids that traversal for
ASCII. The generator checks 36 page-size combinations and selects the smallest
index in that family, then overlaps identical windows and matching page tails
and heads using direct element offsets. It verifies reconstruction at every code point. This
does not claim global optimality among all possible compression schemes.

The resulting tables total 55,030 bytes, including mappings and QA script tags.
This is 11,164 bytes below the prior 74-bit properties stored in 96-bit records.
They allocate no runtime memory. Property records are immutable/static; enum
identities and packed layout are tied to the pinned data version. ISO 15924
script tags are not automatically OpenType script tags.

The compactor is first-party offline code in `compact_pages.py`; no runtime
library or decoder is added. Layout candidates describe the aligned search;
the selected layout additionally reports its pre-compaction size and direct-offset
addressing.

## Independent native-output verification

`MaiCordUnicodeTests` runs native tests and can dump the actual getter results:

```powershell
MaiCordUnicodeTests.exe <native.bin>
python tools/unicode/verify_xml.py --xml <ucd.nounihan.flat.zip> --ucd <UCD.zip> --schema <generation.json> --native <native.bin> --output <comparison.json>
```

The independent [flat XML dataset](https://www.unicode.org/Public/17.0.0/ucdxml/ucd.nounihan.flat.zip)
supplies expected values. The verifier does not import the generator or rebuild
its trie/packing logic. It checks every code point, all retained fields and flags, both derived
predicates, and mirror/bracket values exactly. It requires contiguous full
coverage and rejects truncation and trailing data. Enum labels in the schema
describe the native output ABI; expected property assignments come from XML.

The dump begins with `MCUCDS2` plus NUL. Each code point contributes eleven
field bytes in schema order, three little-endian uint32s (flags, mirror, paired
bracket), and one bracket-type byte. The verifier also reads earlier full-set
dumps. Surrogates are included in the data comparison; scalar validity excludes them.

## Streaming extended grapheme clusters

`unicode_grapheme_next` implements the default extended rules from
[UAX #29 revision 47](https://www.unicode.org/reports/tr29/tr29-47.html). It uses
three bytes of state, one pass and no allocation or backward scan. State tracks
the previous break class, regional-indicator parity, pictograph/ZWJ context and
Indic linker context. Preserve it across chunks; reset it at start of text.
The caller supplies decoded scalar values and handles malformed encoding.

Tests include every official `GraphemeBreakTest.txt` vector and long-context
cases for emoji, Indic conjuncts and regional indicators. These are default
Unicode grapheme boundaries, not font shaping clusters or a Windows caret policy.
Data and grapheme conformance do not establish DirectWrite layout/pixel parity.
Word/line-break algorithms, normalization, shaping and app integration remain
separate work. Production does not select this new module yet.

## Bidirectional paragraph and line ordering

`bidi.hpp` provides classification, a P1 paragraph-boundary helper, P2–I2
paragraph resolution and L1/L2 ordering for a caller-selected line range, using
[UAX #9 revision 51](https://www.unicode.org/reports/tr9/tr9-51.html). It covers
isolates, explicit embeddings/overrides, weak/neutral resolution and paired
brackets. Font shaping, combining-glyph handling and glyph mirroring remain
separate; this module does not choose line breaks or change source text.

Each input uses a 16-byte working cell and one u32 scratch index. Isolate
matching uses those cells, including overflow isolates. Explicit levels use a
fixed stack; bracket handling observes the standard's 63-entry limit. No heap
allocation or recursion is used. Line output preserves the paragraph's resolved
levels, allowing a different wrap range to be reordered without another bidi
pass. The previous scratch buffer can hold visual indices; line levels need one
byte per input position. Source/transcoding and retained-layout storage are
outside these working-buffer counts.

`level==255` represents X9 removal from visual ordering. It does not authorize
deleting join controls from the shaping input. Output indices remain relative
to the paragraph, and higher-level newline/caret policies belong to the app
adapter. `auto_rtl` is an explicit paragraph fallback; FSI keeps its normative
LTR fallback when its contents have no eligible strong character.

The complete official suites are transcribed without implementing an expected
bidi algorithm in the generator:

```powershell
python tools/unicode/generate_bidi_tests.py --ucd <UCD.zip> --schema <generation.json> --output <corpus.bin> --report <corpus.json>
MaiCordBidiTests.exe <corpus.bin>
```

The corpus expands every requested class-test direction and includes every
character test. It contains 861,948 cases. The executable compares levels and
the full visual index list, plus expected paragraph levels where supplied.
Truncation and unsupported QA capacity fail explicitly. The 512-cell QA case
buffer is not an engine limit; native stress tests also use 20,001-cell inputs.
Without a corpus argument, CTest runs the unit/stress/invariant checks only.
Both the corpus and its generator report must be retained to establish full
conformance coverage. These tests do not establish DirectWrite app parity.

## Installed DirectWrite bidi comparison

With `MAICORD_BUILD_FONT_REFERENCE=ON`, build `MaiCordBidiReference`. This
separate QA binary compares every character case against the installed Windows
analyzer and no-wrap layout hit-test levels. Abstract class cases are excluded;
auto cases supply the corpus-verified native base to Windows. It verifies native
results against the official expected vectors before comparing Windows.

```powershell
MaiCordBidiReference.exe <corpus.bin> > comparison.csv
python tools/unicode/summarize_bidi_reference.py comparison.csv --character-tests <BidiCharacterTest.txt> --out results.json --require-exact
python tools/unicode/make_bidi_supplementary.py --character-tests <BidiCharacterTest.txt> --out supplementary.bin
MaiCordBidiReference.exe supplementary.bin > supplementary.csv
python tools/unicode/summarize_bidi_reference.py supplementary.csv --character-tests <BidiCharacterTest.txt> --out supplementary-results.json --supplementary --require-exact
```

Check the native process exit before accepting its CSV. The summarizer rejects
incomplete/duplicate cases and inconsistent counters; strict parity returns 2
for captured differences. The supplementary corpus replaces selected L/R/ON
characters while retaining published expected vectors. It is synthetic and
must be labeled separately. The comparison does not establish glyph/pixel parity.
The first captured Windows comparison identified real compatibility gaps,
including short bracket sequences.

The internal experimental Windows bracket candidate is captured separately as
`candidate_levels` and `candidate_diff`. The public resolver remains Unicode 17.
Candidate counters are checked against the captured vectors; they do not replace
the standards comparison or change the strict parity outcome.

```powershell
python tools/unicode/bracket_probes.py generate --ucd <UCD.zip> --out probes.bin
MaiCordBidiReference.exe probes.bin > probes.csv
python tools/unicode/bracket_probes.py summarize probes.csv --manifest probes.json --out probe-results.json
```

The distinct `MCBIDIP` format has no expected levels or visual indices and CSV
marks it `has_expected=0`. The official summarizer rejects exploratory captures.
These deterministic inputs record Windows observations; no native model supplies
their expected answers. Bracket candidate captures show newly discovered
regressions as well as improved cases.

To reduce captured failures, use a frozen oracle binary and retain the output
directory. An optional previous oracle preserves a newly introduced regression
instead of merely retaining any candidate/Windows mismatch:

```powershell
python tools/unicode/reduce_bidi_probes.py --failures probe-results-failures.json --oracle <frozen-oracle.exe> --out reduced --limit 32
python tools/unicode/reduce_bidi_probes.py --failures regressions.json --oracle <new-frozen-oracle.exe> --previous-oracle <old-frozen-oracle.exe> --out new-regressions --limit 5
```

The results are deletion-minimal examples, not globally minimal strings or
Unicode expected outputs. Oracle identity, full batch coverage and candidate
vector/counter consistency are checked. When comparing two candidates, Windows
observations must agree.

## Default word segmentation

`unicode_word_boundaries` implements Unicode 17 default word boundaries over
validated scalar input. It needs `count+1` caller-owned output bytes, which also
hold temporary property tags. No additional scratch array or allocation is
needed. Input decoding/storage and retained layout are separate costs. The
whole-span API rejects invalid input before changing the output.

```powershell
python tools/unicode/generate_word_tests.py --ucd <UCD.zip> --output tests/generated/word_break_test.inc --report generation.json
cmake --build <build> --config Release --target MaiCordWordTests
MaiCordWordTests.exe
```

All 1,944 official cases are compiled into this QA binary; ordinary CMake builds
need no Python or network. Both preceding and end boundaries are checked.
Default word segmentation differs from the app's existing first-party
`utf8_word_range` policy. This component does not switch navigation policy,
perform dictionary segmentation or identify shaping/caret clusters.

## Default line-break opportunities

`unicode_line_breaks` implements Unicode 17 / UAX #14 revision 55 over validated
scalar input. It requires `count+1` caller-owned bytes and constant additional
state. The bytes temporarily carry compact class/property tags, then become
prohibited (0), allowed (1) or mandatory (2) boundaries. It allocates nothing and
does not recurse. Invalid input leaves output untouched. Empty input writes one
prohibited boundary; nonempty input ends with a mandatory boundary.

```powershell
python tools/unicode/generate_line_tests.py --ucd <UCD.zip> --output tests/generated/line_break_test.inc --report generation.json
cmake --build <build> --config Release --target MaiCordLineBreakTests MaiCordLineBreakReference
MaiCordLineBreakTests.exe
MaiCordLineBreakReference.exe > windows.csv
python tools/unicode/summarize_line_reference.py --ucd <UCD.zip> --csv windows.csv --executable <MaiCordLineBreakReference.exe> --output windows.json
```

Check the oracle's exit code before summarizing. All 19,338 official cases are
compiled into the QA binaries. The native suite also checks 16,293 derived
combining-mark cases against the original published boundary expectations,
mandatory breaks, protected pages, invalid inputs and 300,003-scalar contexts.
The property-contract check visits every Unicode code point: the compact future
emoji tags preserve both ID and XX classes, including the derived data's
`@missing` ranges. Ordinary builds require neither Python nor network access.

The separate Windows oracle records raw before/after/whitespace/soft-hyphen
attributes at every UTF-16 unit. Its summary independently resolves adjacent
conditions, verifies complete output and keeps endpoints separate from interior
comparisons. An unresolved neutral pair is recorded explicitly. Unicode
conformance is checked before any Windows observation is accepted.

`MaiCordLineBreakTests.exe --bench` measures the native analysis API on two
repeating scalar patterns at three sizes. Input construction and output digest
verification are outside the timed regions. This is a component microbenchmark,
not an app-layout or DirectWrite-performance comparison.

The API does not implement dictionary segmentation, shaping, line selection,
hyphen insertion or Windows-specific tailoring. Do not replace the app's current
break policy with the default merely because conformance passes.

## Windows line compatibility observations

`line_windows_candidate` is a private, first-party en-US profile. It retains
linear work, constant additional state and the one-byte output buffer. It is
not yet an app paragraph backend or a complete dictionary segmenter. The default
Unicode API remains unchanged.

`MaiCordLineBreakReference` additionally records candidate output and differences.
With no argument it checks every official vector first (`has_expected=1`). An
optional `MCLBPRB1` file supplies independent probes (`has_expected=0`); such
records are rejected by the official conformance summarizer.

```powershell
python tools/unicode/line_probes.py generate --root <probe-root> --seed <integer>
python tools/unicode/line_probes.py capture --root <probe-root> --exe <MaiCordLineBreakReference.exe>
python tools/unicode/line_probes.py summarize --root <probe-root>
```

The generator covers punctuation/mark contexts, seeded Latin and mixed-script
text, arbitrary scalars, emoji, regional indicators, dictionary examples,
property edges and selector chains. It generates no expected break decisions.
The summarizer validates complete UTF-16 capture, adjacent-condition precedence,
endpoints, native/default counters and surrogate interiors. Remaining dictionary
differences are retained explicitly.

`line_windows_data.py` derives the 128-byte correction table from pinned Unicode
16/17 line properties and Unicode 17 age data. It includes nontrivial `@missing`
defaults and drops changes that are equivalent under this profile. Its companion
probes cover all 4,803 newly assigned characters in six contexts each.

`line_windows_corpus.py` exports observed Windows boundaries for native-only
`MCWINLB1` replay. It excludes and lists dictionary cases, and rejects every
other unresolved mismatch. `MaiCordLineWindowsTests` runs unit/stress checks
without arguments and replays a corpus when passed its path. It imports only
KERNEL32.dll.
