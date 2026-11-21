# Native Unicode data and text analysis

First-party C++ with generated read-only Unicode 17.0.0 data; no ICU, utf8proc,
HarfBuzz or other Unicode/text library. The generated tables under `generated/`
are checked in; no build makes them, so a build needs no network connection and
no generator. The Python generators that made them are in the repository
history: `git log --diff-filter=D -- tools/unicode` names the commit that removed
them, and `git checkout <commit>^ -- tools/unicode` restores them for a new
Unicode version or a new word list.

The native tests (`MaiCordUnicodeTests`, `MaiCordBidiTests`,
`MaiCordLineBreakTests`) run through the build script:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1 -Test -TestNames maicord_native_unicode,maicord_native_bidi,maicord_native_line_break
```

## Property data

Input: the official [Unicode 17.0.0 UCD archive](https://www.unicode.org/Public/17.0.0/ucd/UCD.zip),
SHA-256 `2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e`. `@missing` defaults
apply before explicit assignments, property aliases are canonicalized, the full
code-point range is kept and Script_Extensions takes the singleton-Script default
([UAX #44](https://www.unicode.org/reports/tr44/tr44-36.html)). The data uses the
notice in `assets/licenses/Unicode-3.0.txt`.

Output: `include/maicord/text/unicode/generated/unicode_data.hpp`,
`src/text/unicode/generated/unicode_data.inc` (with the deflated tables in
`unicode_data_packed.bin`, inflated on the first non-ASCII lookup) and the official
break vectors: GraphemeBreakTest.txt in `tests/generated/grapheme_break_test.inc`,
LineBreakTest.txt in `tests/generated/line_break_test.inc` and `.bin`.

Script_Extensions contributes only Arabic membership and East_Asian_Width only
the UAX #14 `$EastAsian` bit (F, W or H). The first record word holds the fields
the per-scalar text passes read (bidi class, general category, line and grapheme
break, joining type, flags), the second script, combining class, the Indic
categories and the East Asian bit.
Records sit behind a three-level page index (shared pages, direct IDs for uniform
spans, a 128-entry ASCII index); overlapping pages are shared, identical
unassigned planes share one plane's top-index blocks, and the generator checked
the reconstruction at every code point. A separate verifier compared every code
point with the [flat XML dataset](https://www.unicode.org/Public/17.0.0/ucdxml/ucd.nounihan.flat.zip).
Tables are static and allocate nothing. Enum values and layout are tied to the
pinned data version.

`src/text/unicode/generated/syllabic_categories.inc` holds the Khmer, Myanmar and
Universal Shaping Engine categories, from the same archive and HarfBuzz 15.0.0's
two `ms-use` override files (`IndicSyllabicCategory-Additional.txt`, SHA-256
`b9472e3e72d5fba8cb3f2e0578e73012aaae786db25779de0f1a5a5ab69b84a6`;
`IndicPositionalCategory-Additional.txt`, SHA-256
`2baa1c1efe5a5f108c304b1e27d0d97864c806764eb2b0a1bd91db80ae26b5b8`), by the rules
of HarfBuzz's `gen-indic-table.py` and `gen-use-table.py`. The syllable machines
in `src/text/scripts` read them.

`src/text/unicode/generated/canonical_data.inc` holds the one-step canonical
decompositions and primary compositions of the same archive, which the
alphabetic shaper's HarfBuzz-style normalization reads. Hangul syllables are
algorithmic and CJK compatibility ideographs are left out (font fallback maps
them directly).

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

`bidi.hpp` provides a P1 paragraph-boundary helper and L1/L2 ordering for a caller-chosen line range
([UAX #9 revision 51](https://www.unicode.org/reports/tr9/tr9-51.html)). P2-I2 paragraph resolution is
`bidi_resolve_tailored` in `detail/bidi_internal.hpp`: it resolves bracket pairs by their surroundings, as the
platform text stack lays out message text, not by N0. `tests/support/bidi_conformance.hpp` runs the same core with N0. Shaping,
combining-glyph handling, glyph mirroring, line breaking and newline or caret
policy are outside it; source text is never changed.

- Each input needs a 16-byte working cell and one u32 scratch index. Explicit
  levels use a fixed stack and bracket matching holds at most 61 open brackets; no heap
  allocation or recursion.
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
tables come from open or first-party inputs only.

The word list in `generated/thai_lexical_data.inc` holds every lexical ICU 78.3
`thaidict.txt` word (Unicode License v3 and the ICU 1.8.1-57.1 license,
`assets/licenses/ICU-Thai-dictionary.txt`) and the PyThaiNLP 5.3.8 `words_th.txt`
words (CC0-1.0, `assets/licenses/PyThaiNLP-corpus.txt`) that are not compounds of
shorter listed words, 2 to 20 scalars long. The file header names both sources
with their URLs and SHA-256 pins. The words form a suffix-shared word graph:
its bytes, deflated, are `generated/thai_word_graph_packed.bin` (read by `#embed`
and inflated on the first Thai lookup); the generator decoded the graph again and
compared it with the word list. Node encodings, where a reference below the count
of `thai_word_references` names a shared node by absolute offset and a larger
value is a backward distance:

- 0..79: one edge with this label, then a reference; not a word end.
- 80..127: one edge described by a shared u16 of `thai_word_special` (label |
  distance << 8).
- 128 + 2 * edges + end, edges <= 59: (label, reference) pairs by label. A
  label's high bit names the terminal node at offset zero, with no reference.
- 248 + 2 * wide + end: an 80-bit label mask, then one backward distance of 2
  bytes (3 if wide) per set bit.

`generated/thai_syllable_data.inc` is the minimized machine of an authored
grammar of Thai vowel spellings (an optional leading vowel, one or two initial
consonants, vowel signs and tone marks in Unicode order, an optional final
consonant); it reads no input data. It recognizes one syllable with one scalar of
lookahead and ends closed, open (a following consonant may still be the final)
or failed. The grammar is in the removed generator `thai_syllable_data.py`.
Neither table is derived from a platform text service, and the breaks differ
from DirectWrite's where the dictionaries or the searches do (see
`tests/ui/text/thai_context_cases.inc`).
