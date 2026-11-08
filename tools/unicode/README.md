# Native Unicode data and text analysis

First-party C++ with generated read-only Unicode 17.0.0 data; no ICU, utf8proc,
HarfBuzz or other Unicode/text library. Python and the official archives are
offline development inputs: normal CMake builds use the checked-in generated
files and need neither Python nor a network connection.

The native tests (`MaiCordUnicodeTests`, `MaiCordBidiTests`,
`MaiCordLineBreakTests`) run through the build script:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1 -Test -TestNames maicord_native_unicode,maicord_native_bidi,maicord_native_line_break
```

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

Output is `include/maicord/text/unicode/generated/unicode_data.hpp`,
`src/text/unicode/generated/unicode_data.inc` (with the deflated tables in
`unicode_data_packed.bin`, inflated on the first non-ASCII lookup) and the official
break vectors: GraphemeBreakTest.txt in `tests/generated/grapheme_break_test.inc`,
LineBreakTest.txt in `tests/generated/line_break_test.inc` and `.bin`. `--repo <scratch-root>` regenerates
into another root. The report records input hashes, enum labels and bit
positions, layout candidates, generated-file hashes and counts.

The stored properties are `FIELDS` and `FLAGS` in the script; Script_Extensions
contributes only Arabic membership and East_Asian_Width only the UAX #14
`$EastAsian` bit (F, W or H). The first record word holds the fields the
per-scalar text passes read (bidi class, general category, line and grapheme
break, joining type, flags), the second script, combining class, the Indic
categories and the East Asian bit.
Records sit behind a three-level page index (shared pages, direct IDs for uniform
spans, a 128-entry ASCII index); `compact_pages.py` overlaps pages, identical
unassigned planes share one plane's top-index blocks, and reconstruction is
verified at every code point. Tables are static and allocate
nothing. Enum values and layout are tied to the pinned data version.

`python -I tools/unicode/syllabic_categories.py --ucd <UCD.zip> --harfbuzz <harfbuzz-15.0.0/src>`
derives the Khmer, Myanmar and Universal Shaping Engine categories
(`src/text/unicode/generated/syllabic_categories.inc`) from the same pinned archive and
HarfBuzz's two pinned `ms-use` override files, by the rules of HarfBuzz's
`gen-indic-table.py` and `gen-use-table.py`. `tools/syllable_machines.py` turns
HarfBuzz's Ragel grammars into the syllable machines that read them.

`python -I tools/unicode/canonical_data.py --ucd <UCD.zip>` writes the one-step
canonical decompositions and primary compositions
(`src/text/unicode/generated/canonical_data.inc`) that the alphabetic shaper's
HarfBuzz-style normalization reads. Hangul syllables are algorithmic and CJK
compatibility ideographs are left out (font fallback maps them directly).

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

`unicode_grapheme_boundary` implements the default extended rules of
[UAX #29 revision 47](https://www.unicode.org/reports/tr29/tr29-47.html): three
bytes of state, one pass, no allocation or backward scan. Keep the state across
chunks and reset it at the start of text. The caller supplies decoded scalar
values and handles malformed encoding. These are default Unicode boundaries, not
shaping clusters or a Windows caret policy. `MaiCordUnicodeTests` passes all 766
cases of GraphemeBreakTest.txt. The engine has no UAX #29 word or sentence
segmenter.

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

`MaiCordBidiTests` runs the unit, stress and invariant checks. The official
`BidiTest.txt` and `BidiCharacterTest.txt` suites are not compiled into the
repository, so CTest alone does not establish conformance.

## Line-break opportunities

`unicode_line_breaks` (`include/maicord/text/unicode/detail/line_break.hpp`)
implements the default rules of
[UAX #14 revision 55](https://www.unicode.org/reports/tr14/tr14-55.html) (Unicode
17.0.0) without tailoring, LB1 included (AI, SG, XX to AL; SA to CM or AL; CJ to
NS); Thai runs then take dictionary breaks from `native_thai_word_breaks`. It
takes scalar values as a UTF decoder yields them and writes `count + 1`
caller-owned bytes (0 prohibited, 1 allowed, 2 mandatory), in one linear pass
with constant additional state; null input or short output leaves the output
untouched. Rules that read ahead (LB15b, LB15c, LB19a, LB25, LB28a) keep their
boundary's fallback until the next one or two bases arrive. Pairs whose answer
depends on their two classes alone come from a table built from the rules at
compile time; the rest run the rules in order. The ASCII paragraph path
(`ascii_paragraph.cpp`) applies the same rules to bytes.

`MaiCordLineBreakTests` passes all 19,338 cases of LineBreakTest.txt and runs
unit and stress checks; `maicord_native_run_plans` checks the ASCII path against
it over every byte pair and every string of up to five bytes over the ASCII
classes.

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
`src/text/unicode/generated/thai_lexical_data.inc` as a suffix-shared word graph (its
bytes, deflated, in `thai_word_graph_packed.bin` beside it, read by `#embed` and
inflated on the first Thai lookup) that it
decodes again and compares with the word list. `thai_syllable_data.py` holds an
authored grammar of Thai vowel spellings and writes the minimized machine to
`src/text/unicode/generated/thai_syllable_data.inc`; it reads no input. Both take
`--output` to write elsewhere. Neither table is derived from a platform text
service, and the breaks differ from DirectWrite's where the dictionaries or the
searches do (see `tests/ui/text/thai_context_cases.inc`).
