#!/usr/bin/env python3
"""Writes src/media/audio/aac_tables.inc: the Huffman codebooks, scalefactor band layouts and TNS band limits of
ISO/IEC 14496-3 that the AAC-LC decoder (src/media/audio/aac_lc.cpp) needs.

The tables are normative data of the standard (Annex 4.A.1 and the band tables of 4.5.4 and 4.6.9). This script reads
them out of the static libavcodec of a Linux host (`apt install libavcodec-dev`): it extracts aactab.o from
libavcodec.a, links a small dumper against it with clang, runs it and writes the numbers. It also checks that the
codebook index order is the lexicographic one the decoder derives its values from (so no value tables are stored).
Run it in WSL or on Linux; the output is committed, the build never runs this.

    tools/generate_aac_tables.py [--archive PATH] [--clang CLANG]
"""

import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(ROOT, "src", "media", "audio", "aac_tables.inc")

DUMPER = r"""
#include <math.h>
#include <stdint.h>
#include <stdio.h>

extern const uint16_t *const ff_aac_spectral_codes[11];
extern const uint8_t *const ff_aac_spectral_bits[11];
extern const uint16_t ff_aac_spectral_sizes[11];
extern const uint32_t ff_aac_scalefactor_code[121];
extern const uint8_t ff_aac_scalefactor_bits[121];
extern const uint16_t *const ff_swb_offset_1024[13];
extern const uint16_t *const ff_swb_offset_128[13];
extern const uint8_t ff_aac_num_swb_1024[13];
extern const uint8_t ff_aac_num_swb_128[13];
extern const uint8_t ff_tns_max_bands_1024[13];
extern const uint8_t ff_tns_max_bands_128[13];
extern const float *const ff_aac_codebook_vectors[11];

static const int dimension[11] = {4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2};
static const int modulus[11] = {3, 3, 3, 3, 9, 9, 8, 8, 13, 13, 17};
static const int offset[11] = {-1, -1, 0, 0, -4, -4, 0, 0, 0, 0, 0};

int main(void) {
    for (int book = 0; book < 11; ++book)
        for (int index = 0; index < ff_aac_spectral_sizes[book]; ++index) {
            int remaining = index;
            int values[4];
            for (int d = dimension[book] - 1; d >= 0; --d) {
                values[d] = remaining % modulus[book] + offset[book];
                remaining /= modulus[book];
            }
            for (int d = 0; d < dimension[book]; ++d) {
                /* libavcodec stores the values already raised to 4/3; book 11 keeps the escape marker 16 as it is */
                if (book == 10 && values[d] == 16)
                    continue;
                const double expected = (values[d] < 0 ? -1.0 : 1.0) * pow(fabs((double)values[d]), 4.0 / 3.0);
                if (fabs(ff_aac_codebook_vectors[book][index * dimension[book] + d] - expected) > 1e-4) {
                    fprintf(stderr, "codebook %d index %d differs\n", book + 1, index);
                    return 1;
                }
            }
        }
    printf("sizes");
    for (int i = 0; i < 11; ++i) printf(" %d", ff_aac_spectral_sizes[i]);
    printf("\nspectral_bits");
    for (int b = 0; b < 11; ++b) for (int i = 0; i < ff_aac_spectral_sizes[b]; ++i) printf(" %d", ff_aac_spectral_bits[b][i]);
    printf("\nspectral_codes");
    for (int b = 0; b < 11; ++b) for (int i = 0; i < ff_aac_spectral_sizes[b]; ++i) printf(" %d", ff_aac_spectral_codes[b][i]);
    printf("\nscalefactor_bits");
    for (int i = 0; i < 121; ++i) printf(" %d", ff_aac_scalefactor_bits[i]);
    printf("\nscalefactor_codes");
    for (int i = 0; i < 121; ++i) printf(" %u", ff_aac_scalefactor_code[i]);
    for (int r = 0; r < 13; ++r) {
        printf("\nlong %d %d", r, ff_aac_num_swb_1024[r]);
        for (int i = 0; i <= ff_aac_num_swb_1024[r]; ++i) printf(" %d", ff_swb_offset_1024[r][i]);
    }
    for (int r = 0; r < 13; ++r) {
        printf("\nshort %d %d", r, ff_aac_num_swb_128[r]);
        for (int i = 0; i <= ff_aac_num_swb_128[r]; ++i) printf(" %d", ff_swb_offset_128[r][i]);
    }
    printf("\ntns_long");
    for (int i = 0; i < 13; ++i) printf(" %d", ff_tns_max_bands_1024[i]);
    printf("\ntns_short");
    for (int i = 0; i < 13; ++i) printf(" %d", ff_tns_max_bands_128[i]);
    printf("\n");
    return 0;
}
"""


def canonical(bits, codes):
    """([codewords of each length 1..19], symbols in code order) of a codebook, checking that its codewords are the
    canonical ones: the shortest first, consecutive within a length, in symbol order."""
    order = sorted(range(len(bits)), key=lambda symbol: (bits[symbol], codes[symbol]))
    counts = [sum(1 for length in bits if length == n) for n in range(1, 20)]
    assert max(bits) <= 19 and max(counts) < 256
    expected, first = [], 0
    for n in range(1, 20):
        for i in range(counts[n - 1]):
            expected.append(first + i)
        first = (first + counts[n - 1]) << 1
    assert [codes[symbol] for symbol in order] == expected, "a codebook is not canonical"
    assert [bits[symbol] for symbol in order] == sorted(bits)
    return counts, order


def band_rows(bands, limit, width):
    """(the distinct offset rows padded to `width`, the row of each sampling frequency index)."""
    distinct, index = [], []
    for r in range(13):
        count, offsets = bands[r]
        assert len(offsets) == count + 1 and count <= limit
        row = offsets + [0] * (width - len(offsets))
        if row not in distinct:
            distinct.append(row)
        index.append(distinct.index(row))
    return distinct, index


def rows(values, per_line, width=0):
    lines = []
    for start in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v).rjust(width) for v in values[start:start + per_line]) + ",")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--archive", default="/usr/lib/x86_64-linux-gnu/libavcodec.a")
    parser.add_argument("--clang", default="clang")
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        subprocess.run(["ar", "x", os.path.abspath(arguments.archive), "aactab.o"], cwd=directory, check=True)
        source = os.path.join(directory, "dump.c")
        with open(source, "w") as handle:
            handle.write(DUMPER)
        binary = os.path.join(directory, "dump")
        subprocess.run([arguments.clang, "-fuse-ld=lld", "-Wl,--unresolved-symbols=ignore-all", "-o", binary, source, "-lm",
                        os.path.join(directory, "aactab.o")], check=True)
        result = subprocess.run([binary], capture_output=True, text=True)
        if result.returncode:
            sys.exit("the dumper failed: " + result.stderr)
        text = result.stdout
    data = {}
    longs, shorts = {}, {}
    for line in text.splitlines():
        words = line.split()
        if words[0] in ("long", "short"):
            (longs if words[0] == "long" else shorts)[int(words[1])] = (int(words[2]), [int(w) for w in words[3:]])
        else:
            data[words[0]] = [int(w) for w in words[1:]]
    sizes = data["sizes"]
    assert sizes == [81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289], sizes
    assert len(data["spectral_bits"]) == len(data["spectral_codes"]) == sum(sizes)
    # Every codebook is canonical: its codewords of one length are consecutive and follow those of the shorter lengths in
    # symbol order within the length, so a book is the number of codewords of each length and its symbols in code order.
    counts, symbols, at = [], [], 0
    for book, size in enumerate(sizes):
        c, s = canonical(data["spectral_bits"][at:at + size], data["spectral_codes"][at:at + size])
        counts.append(c)
        symbols.append(s)
        at += size
    c, s = canonical(data["scalefactor_bits"], data["scalefactor_codes"])
    counts.append(c)
    symbols.append(s)
    # Books 1..10 and the scalefactor book have symbols below 256; book 11 (289 pairs) needs 16 bits.
    small = [book for book in range(12) if book != 10]
    assert all(max(symbols[book]) < 256 for book in small) and max(symbols[10]) == 288
    bases, base = [], 0
    for book in range(12):
        bases.append(base if book != 10 else 0)
        if book != 10:
            base += len(symbols[book])
    out = []
    out.append("// Generated by tools/generate_aac_tables.py: the tables of ISO/IEC 14496-3 the AAC-LC decoder reads.")
    out.append("// Codebook order is lexicographic (the first value of a pair or quad varies slowest), which the generator checked.")
    out.append("")
    out.append("// The Huffman codebooks: 0..10 the spectral books 1..11, 11 the scalefactor book (121 symbols, delta + 60). Each is")
    out.append("// canonical (the generator checked): the number of codewords of each length 1..19, then the symbols in code order.")
    out.append("inline constexpr u8 kAacCodeCounts[12][19] = {")
    for book in range(12):
        out.append("    {%s}," % ", ".join(map(str, counts[book])))
    out.append("};")
    out.append("// Where the symbols of each book start in kAacSymbols8 (book 10's are in kAacSymbols16).")
    out.append("inline constexpr u16 kAacSymbolBase[12] = {%s};" % ", ".join(map(str, bases)))
    flat = [value for book in small for value in symbols[book]]
    out.append("inline constexpr u8 kAacSymbols8[%d] = {" % len(flat))
    out.append(rows(flat, 24, 3))
    out.append("};")
    out.append("inline constexpr u16 kAacSymbols16[%d] = {" % len(symbols[10]))
    out.append(rows(symbols[10], 12, 3))
    out.append("};")
    out.append("")
    out.append("// Scalefactor band offsets per sampling frequency index (0 = 96000 Hz ... 12 = 7350 Hz); the rows are the distinct")
    out.append("// layouts, the Row tables say which one a rate uses. Long windows (1024 lines): count, then count + 1 offsets")
    out.append("// padded to 53 entries.")
    out.append("inline constexpr u8 kAacLongBandCount[13] = {%s};" % ", ".join(str(longs[r][0]) for r in range(13)))
    long_rows, long_index = band_rows(longs, 52, 53)
    out.append("inline constexpr u8 kAacLongBandRow[13] = {%s};" % ", ".join(map(str, long_index)))
    out.append("inline constexpr u16 kAacLongBands[%d][53] = {" % len(long_rows))
    for row in long_rows:
        out.append("    {%s}," % ", ".join(map(str, row)))
    out.append("};")
    out.append("// Short windows (128 lines): count, then count + 1 offsets padded to 16 entries.")
    out.append("inline constexpr u8 kAacShortBandCount[13] = {%s};" % ", ".join(str(shorts[r][0]) for r in range(13)))
    short_rows, short_index = band_rows(shorts, 15, 16)
    out.append("inline constexpr u8 kAacShortBandRow[13] = {%s};" % ", ".join(map(str, short_index)))
    out.append("inline constexpr u16 kAacShortBands[%d][16] = {" % len(short_rows))
    for row in short_rows:
        out.append("    {%s}," % ", ".join(map(str, row)))
    out.append("};")
    out.append("")
    out.append("// The highest scalefactor band TNS filters, per sampling frequency index.")
    out.append("inline constexpr u8 kAacTnsMaxBandsLong[13] = {%s};" % ", ".join(map(str, data["tns_long"])))
    out.append("inline constexpr u8 kAacTnsMaxBandsShort[13] = {%s};" % ", ".join(map(str, data["tns_short"])))
    out.append("")
    with open(OUTPUT, "w", newline="\n") as handle:
        handle.write("\n".join(out))
    print("wrote", OUTPUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
