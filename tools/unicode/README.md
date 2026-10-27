# Native Unicode data and text analysis

First-party C++ with generated read-only Unicode 17.0.0 data; no ICU, utf8proc,
HarfBuzz or other Unicode/text library. Python and the official archives are
offline development inputs: normal CMake builds use the checked-in generated
files and need neither Python nor a network connection.

The native tests (`MaiCordUnicodeTests`, `MaiCordBidiTests`,
`MaiCordLineTailoredTests`) run through the build script:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1 -Test -TestNames maicord_native_unicode,maicord_native_bidi,maicord_native_line_compat
```

The corpus and Windows comparisons below run the executables directly.
`MaiCordBidiReference` and `MaiCordLineBreakReference` are DirectWrite oracles
built with `-DMAICORD_BUILD_FONT_REFERENCE=ON` (`cmake/reference_tools.cmake`).
Unicode conformance does not establish DirectWrite or app parity (layout,
carets, pixels).

## Property data

```powershell
python tools/unicode/generate.py --ucd <UCD.zip> --report <generation.json>
```

Input is the official [Unicode 17.0.0 UCD archive](https://www.unicode.org/Public/17.0.0/ucd/UCD.zip),
rejected unless its SHA-256 matches the pin in the script. `@missing` defaults
apply before explicit assignments, property aliases are canonicalized, the full
code-point range is kept and Script_Extensions takes the singleton-Script default
([UAX #44](https://www.unicode.org/reports/tr44/tr44-36.html)). The data uses the
notice in `assets/licenses/Unicode-3.0.txt`.

Output is `include/maicord/text/generated/unicode_data.hpp`,
`src/text/generated/unicode_data.inc` and the official grapheme vectors in
`tests/generated/grapheme_break_test.inc`. `--repo <scratch-root>` regenerates
into another root. The report records input hashes, enum labels and bit
positions, layout candidates, generated-file hashes and counts.

The stored properties are `FIELDS` and `FLAGS` in the script; Script_Extensions
contributes only Arabic membership and East_Asian_Width only F/W/H membership.
Records sit behind a three-level page index (shared pages, direct IDs for uniform
spans, a 128-entry ASCII index); `compact_pages.py` overlaps pages, identical
unassigned planes share one plane's top-index blocks, and reconstruction is
verified at every code point. Tables are static and allocate
nothing. Enum values and layout are tied to the pinned data version.

`python -I tools/unicode/tibetan_categories.py --ucd <UCD.zip>` derives the
Tibetan cluster categories (`src/text/generated/tibetan_categories.inc`) from
the same pinned archive: Indic syllabic and positional categories, General
Category, canonical combining classes and decompositions, by the rules in the
script.

### Independent verification

```powershell
MaiCordUnicodeTests.exe <native.bin>
python tools/unicode/verify_xml.py --xml <ucd.nounihan.flat.zip> --ucd <UCD.zip> --schema <generation.json> --native <native.bin> --output <comparison.json>
```

Expected values come from the [flat XML dataset](https://www.unicode.org/Public/17.0.0/ucdxml/ucd.nounihan.flat.zip)
(SHA-256 pinned in the script); the verifier does not import the generator. It
compares every code point, all stored fields and flags, both derived predicates
and mirror and bracket values exactly, and rejects non-contiguous coverage,
truncation and trailing data. `<native.bin>` is `MCUCDS2` plus NUL, then per code
point eleven field bytes in schema order, three little-endian uint32 (flags,
mirror, paired bracket) and one bracket-type byte.

## Grapheme clusters

`unicode_grapheme_next` implements the default extended rules of
[UAX #29 revision 47](https://www.unicode.org/reports/tr29/tr29-47.html): three
bytes of state, one pass, no allocation or backward scan. Keep the state across
chunks and reset it at the start of text. The caller supplies decoded scalar
values and handles malformed encoding. These are default Unicode boundaries, not
shaping clusters or a Windows caret policy.

## Bidirectional ordering

`bidi.hpp` provides classification, a P1 paragraph-boundary helper, P2-I2
paragraph resolution and L1/L2 ordering for a caller-chosen line range
([UAX #9 revision 51](https://www.unicode.org/reports/tr9/tr9-51.html)). Shaping,
combining-glyph handling, glyph mirroring, line breaking and newline or caret
policy are outside it; source text is never changed.

- Each input needs a 16-byte working cell and one u32 scratch index. Explicit
  levels use a fixed stack and brackets a 63-entry one; no heap allocation or
  recursion.
- Line output keeps the paragraph's resolved levels, so another wrap range can be
  reordered without another bidi pass. Line levels need one byte per input
  position; the scratch buffer can hold the visual indices.
- `level == 255` marks X9 removal from visual order; it does not allow deleting
  join controls from the shaping input. Output indices are paragraph-relative.
- `auto_rtl` is a higher-level paragraph fallback; FSI keeps its normative LTR
  fallback when its contents have no eligible strong character.

### Official suites

```powershell
python tools/unicode/generate_bidi_tests.py --ucd <UCD.zip> --schema <generation.json> --output <corpus.bin> --report <corpus.json>
MaiCordBidiTests.exe <corpus.bin>
```

The generator transcribes `BidiTest.txt` and `BidiCharacterTest.txt` without
implementing any bidi algorithm. The executable compares levels, the visual
index list and the paragraph level where given. Truncation and cases over the
512-scalar QA buffer (not an engine limit) fail. Without a corpus argument only
unit, stress and invariant checks run, so CTest alone does not establish
conformance.

### Installed DirectWrite comparison

`MaiCordBidiReference` first checks the native results against the official
vectors, then compares every character case with the installed Windows analyzer
and with no-wrap layout hit-test levels. Abstract class cases are skipped. Auto
cases pass the native base level to Windows, which exposes only a forced
paragraph direction.

```powershell
MaiCordBidiReference.exe <corpus.bin> > comparison.csv
python tools/unicode/summarize_bidi_reference.py comparison.csv --character-tests <BidiCharacterTest.txt> --out results.json --require-exact
python tools/unicode/make_bidi_supplementary.py --character-tests <BidiCharacterTest.txt> --out supplementary.bin
MaiCordBidiReference.exe supplementary.bin > supplementary.csv
python tools/unicode/summarize_bidi_reference.py supplementary.csv --character-tests <BidiCharacterTest.txt> --out supplementary-results.json --supplementary --require-exact
```

- Check the executable's exit code before accepting its CSV. The summarizer
  rejects incomplete or duplicate cases and inconsistent counters; with
  `--require-exact` it exits 2 when a difference was captured.
- The supplementary corpus replaces selected L, R and ON characters with
  supplementary-plane ones and keeps the published expected vectors. It is
  synthetic, not official conformance data.
- `candidate_levels` and `candidate_diff` record the tailored profile
  (`bidi_resolve_tailored`) that message layout uses. The public resolver stays
  Unicode 17, and these columns do not change the strict parity result.

Exploratory bracket probes use the `MCBIDIP` format. The CSV marks them
`has_expected=0` and the official summarizer rejects them: they record Windows
observations, and no native model supplies expected answers.

```powershell
python tools/unicode/bracket_probes.py generate --ucd <UCD.zip> --out probes.bin
MaiCordBidiReference.exe probes.bin > probes.csv
python tools/unicode/bracket_probes.py summarize probes.csv --manifest probes.json --out probe-results.json
```

`summarize` also writes `probe-results-failures.json` next to `--out`. To reduce
captured failures, use a frozen oracle binary and keep the output directory;
`--previous-oracle` keeps only regressions (the previous candidate matched
Windows):

```powershell
python tools/unicode/reduce_bidi_probes.py --failures probe-results-failures.json --oracle <frozen-oracle.exe> --out reduced --limit 32
python tools/unicode/reduce_bidi_probes.py --failures regressions.json --oracle <new-frozen-oracle.exe> --previous-oracle <old-frozen-oracle.exe> --out new-regressions --limit 5
```

Results are deletion-minimal, not globally minimal, and not Unicode expected
outputs. Oracle identity and batch coverage are checked; when comparing two
candidates, their Windows observations must agree.

## Line-break opportunities

`line_breaks_tailored` (`include/maicord/text/detail/line_break_tailored.hpp`)
is the en-US line-break tailoring message layout uses, matching the platform
text stack: Unicode 17 / UAX #14 revision 55 with the platform's class
remappings. It takes validated scalar input and writes `count + 1` caller-owned
bytes (0 prohibited, 1 allowed, 2 mandatory), in one linear pass with constant
additional state; invalid input leaves the output untouched. It is not a
dictionary segmenter.

```powershell
python tools/unicode/generate_line_tests.py --ucd <UCD.zip> --output tests/generated/line_break_test.inc --report generation.json
cmake --build <build> --config Release --target MaiCordLineBreakReference
MaiCordLineBreakReference.exe > windows.csv
```

- The official `LineBreakTest.txt` cases are compiled into the QA binary.
- `MaiCordLineBreakReference` records the raw before, after, whitespace and
  soft-hyphen attributes of Windows at every UTF-16 unit, plus the tailored
  output and its differences. With no argument it runs every official vector
  (`has_expected=1`); given an `MCLBPRB1` probe file it reports independent probes
  (`has_expected=0`). Check its exit code before using the CSV.

```powershell
python tools/unicode/line_probes.py generate --root <probe-root> --seed <integer>
python tools/unicode/line_probes.py capture --root <probe-root> --exe <MaiCordLineBreakReference.exe>
python tools/unicode/line_probes.py summarize --root <probe-root>
```

`generate` covers punctuation and mark contexts, seeded Latin and mixed-script
text, arbitrary scalars, emoji, regional indicators, dictionary examples,
property edges and selector chains, with no expected decisions. `summarize`
validates the complete UTF-16 capture, adjacent-condition precedence, endpoints,
native and default counters and surrogate interiors; dictionary differences are
listed, not hidden.

- `line_tailoring_data.py` derives `src/text/generated/line_tailoring_data.inc` from
  the pinned Unicode 16 and 17 line-break properties and Unicode 17 age data:
  characters first assigned in 17.0 keep their 16.0 class where it differs under
  this profile. Arguments: `--ucd`, `--previous` (the Unicode 16 property file),
  `--output`, `--report`, `--probes`.
- `line_windows_corpus.py --root <probe-root> --ucd <UCD.zip>` writes the observed
  Windows boundaries to `<root>/native-corpus.bin` (`MCWINLB1`). Dictionary cases
  are excluded and listed; any other case where candidate and Windows differ is
  rejected.
- `MaiCordLineTailoredTests` runs unit and stress checks and, given a corpus path,
  replays it. It imports only KERNEL32.dll.

QA caps: 256 scalars per case and 32 MiB per `MCLBPRB1` or `MCWINLB1` file.

## Thai word breaking data

Thai has no spaces between words, so `native_thai_word_breaks` finds break
opportunities by maximal matching: each Thai run is divided into listed words
and, where no word fits, syllables of an authored grammar, choosing the
division with the fewest unknown syllables, then the fewest segments. Both
tables are generated from open or first-party inputs only:

```powershell
python -I tools/unicode/thai_break_data.py --icu <thaidict.txt> --pythainlp <words_th.txt> [--report <generation.json>]
python -I tools/unicode/thai_syllable_data.py
```

`thai_break_data.py` pins ICU 78.3 `thaidict.txt` (Unicode License v3 and the
ICU 1.8.1-57.1 license, `assets/licenses/ICU-Thai-dictionary.txt`) and
PyThaiNLP 5.3.8 `words_th.txt` (CC0-1.0, `assets/licenses/PyThaiNLP-corpus.txt`)
by URL and SHA-256 (the `SOURCES` table in the script), and rejects other input.
It keeps every lexical ICU word and the PyThaiNLP words that are not compounds of
shorter listed words, 2 to 20 scalars long, and writes
`src/text/generated/thai_lexical_data.inc` as a suffix-shared word graph that it
decodes again and compares with the word list. `thai_syllable_data.py` holds an
authored grammar of Thai vowel spellings and writes the minimized machine to
`src/text/generated/thai_syllable_data.inc`; it reads no input. Both take
`--output` to write elsewhere. Neither table is derived from a platform text
service, and the breaks differ from DirectWrite's where the dictionaries or the
searches do (see `tests/thai_context_cases.inc`).
