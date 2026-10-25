#include "maicord/ui/regional_flag.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
// Licensed Twemoji v17.0.3. All 262 canonical filenames exist in the upstream SVG tree.
// Unicode identities remain unchanged; matching artwork lives in the account media module.
// See assets/licenses/Twemoji-graphics.txt for attribution and provenance.
constexpr u16 sequences[] = {3, 4, 5, 6, 7, 9, 12, 13, 15, 17, 18, 19, 20, 21, 23, 24, 26, 27, 28, 30, 31, 32, 33, 34, 35, 36, 38, 39, 40, 41, 43, 44, 45, 46, 48, 49, 51, 52, 53, 55, 56, 58, 59, 60, 61, 63, 64, 65, 66, 67, 68, 69, 70, 73, 74, 75, 76, 77, 78, 83, 85, 88, 89, 91, 93, 104, 105, 107, 109, 111, 112, 122, 123, 124, 125, 139, 140, 141, 143, 145, 148, 157, 158, 160, 161, 162, 163, 164, 165, 168, 169, 170, 172, 173, 174, 175, 176, 177, 179, 181, 193, 195, 196, 200, 202, 203, 211, 212, 213, 220, 221, 222, 223, 225, 226, 227, 228, 239, 247, 249, 250, 265, 267, 268, 269, 273, 274, 276, 278, 283, 285, 286, 287, 288, 289, 295, 297, 304, 305, 306, 307, 308, 311, 313, 315, 316, 317, 318, 319, 320, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 341, 343, 344, 345, 347, 350, 353, 354, 356, 359, 364, 377, 391, 395, 396, 397, 398, 401, 402, 403, 404, 408, 409, 410, 413, 415, 417, 447, 457, 461, 463, 465, 469, 470, 471, 472, 473, 475, 476, 477, 478, 479, 480, 481, 482, 483, 486, 487, 488, 490, 492, 493, 494, 495, 497, 498, 500, 501, 502, 504, 505, 506, 507, 508, 509, 512, 514, 516, 517, 520, 521, 527, 533, 534, 539, 545, 546, 547, 549, 551, 553, 555, 560, 567, 578, 591, 609, 629, 644, 651, 663, 673, 677, 678, 679};
static_assert(sizeof(sequences) == kRegionalFlagAssetCount * 2);
consteval bool metadata_valid() noexcept {
    if (sizeof(sequences) / sizeof(sequences[0]) != kRegionalFlagAssetCount) return false;
    for (u32 i = 0; i < kRegionalFlagAssetCount; ++i)
        if (!sequences[i] || sequences[i] > 679 || (i && sequences[i - 1] >= sequences[i])) return false;
    return true;
}
static_assert(metadata_valid());
u32 find(u16 sequence) noexcept {
    u32 first = 0, last = kRegionalFlagAssetCount;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (sequences[middle] < sequence) first = middle + 1;
        else last = middle;
    }
    return first < kRegionalFlagAssetCount && sequences[first] == sequence ? first : kRegionalFlagAssetCount;
}
}


u16 regional_flag_sequence(Utf8View glyph) noexcept {
    if (!glyph.data || (glyph.bytes != 8 && glyph.bytes != 28)) return 0;
    u32 scalars[7]{};
    const u32 count = glyph.bytes / 4;
    for (u32 i = 0; i < count; ++i) {
        const auto scalar = utf8_decode_one(glyph.data + i * 4, 4);
        if (!scalar.valid || scalar.width != 4) return 0;
        scalars[i] = scalar.codepoint;
    }
    u16 sequence = 0;
    if (count == 2) {
        if (scalars[0] < 0x1f1e6 || scalars[0] > 0x1f1ff ||
            scalars[1] < 0x1f1e6 || scalars[1] > 0x1f1ff) return 0;
        sequence = static_cast<u16>(1 + (scalars[0] - 0x1f1e6) * 26 + scalars[1] - 0x1f1e6);
    } else {
        if (scalars[0] != 0x1f3f4 || scalars[1] != 0xe0067 ||
            scalars[2] != 0xe0062 || scalars[6] != 0xe007f) return 0;
        static constexpr char subdivisions[3][3] = {{'e', 'n', 'g'}, {'s', 'c', 't'}, {'w', 'l', 's'}};
        for (u32 i = 0; i < 3; ++i)
            if (scalars[3] == 0xe0000u + static_cast<u32>(subdivisions[i][0]) &&
                scalars[4] == 0xe0000u + static_cast<u32>(subdivisions[i][1]) &&
                scalars[5] == 0xe0000u + static_cast<u32>(subdivisions[i][2])) {
                sequence = static_cast<u16>(677 + i);
                break;
            }
    }
    return find(sequence) < kRegionalFlagAssetCount ? sequence : 0;
}

bool regional_flag_next(Utf8View text, u32* cursor, RegionalFlagRange* range) noexcept {
    if (range) reset_value(range);
    if (!cursor || !range || (!text.data && text.bytes) || *cursor > text.bytes) return false;
    u32 position = *cursor;
    while (position < text.bytes) {
        const u32 first = position;
        const auto base = utf8_decode_one(text.data + position, text.bytes - position);
        if (!base.valid) return false;
        position += base.width;
        const bool ri = base.codepoint >= 0x1f1e6 && base.codepoint <= 0x1f1ff;
        bool candidate = base.codepoint == 0x1f3f4;
        if (ri && position < text.bytes) {
            const auto second = utf8_decode_one(text.data + position, text.bytes - position);
            if (!second.valid) return false;
            if (second.codepoint >= 0x1f1e6 && second.codepoint <= 0x1f1ff) {
                position += second.width; candidate = true;
            }
        }
        // Do not turn a recognized prefix of a longer emoji sequence into art.
        bool joined = false;
        while (position < text.bytes) {
            const auto next = utf8_decode_one(text.data + position, text.bytes - position);
            if (!next.valid) return false;
            const u32 cp = next.codepoint;
            if (joined || cp == 0x200d || cp == 0xfe0e || cp == 0xfe0f ||
                (cp >= 0x1f3fb && cp <= 0x1f3ff) || (cp >= 0xe0020 && cp <= 0xe007f)) {
                position += next.width; joined = cp == 0x200d; continue;
            }
            break;
        }
        const u16 sequence = candidate ? regional_flag_sequence({text.data + first, position - first}) : 0;
        if (sequence) { *cursor = position; *range = {first, position, sequence}; return true; }
    }
    *cursor = position;
    return false;
}

u32 regional_flag_svg_target(u16 sequence, Slice<char> output) noexcept {
    if (find(sequence) == kRegionalFlagAssetCount || !output.data) return 0;
    u32 scalars[7]{};
    u32 count = 2;
    if (sequence <= 676) {
        scalars[0] = 0x1f1e6u + (sequence - 1) / 26;
        scalars[1] = 0x1f1e6u + (sequence - 1) % 26;
    } else {
        static constexpr char subdivisions[3][3] = {{'e', 'n', 'g'}, {'s', 'c', 't'}, {'w', 'l', 's'}};
        count = 7; scalars[0] = 0x1f3f4; scalars[1] = 0xe0067; scalars[2] = 0xe0062;
        for (u32 i = 0; i < 3; ++i) scalars[3 + i] = 0xe0000u + static_cast<u32>(subdivisions[sequence - 677][i]);
        scalars[6] = 0xe007f;
    }
    const u32 bytes = kRegionalFlagSvgTargetPrefixBytes + count * 6 - 1 + 4;
    if (output.count < bytes) return 0;
    for (u32 i = 0; i < kRegionalFlagSvgTargetPrefixBytes; ++i) output[i] = kRegionalFlagSvgTargetPrefix[i];
    static constexpr char hexadecimal[] = "0123456789abcdef", suffix[] = ".svg";
    u32 at = kRegionalFlagSvgTargetPrefixBytes;
    for (u32 i = 0; i < count; ++i) {
        if (i) output[at++] = '-';
        for (u32 digit = 0; digit < 5; ++digit) output[at++] = hexadecimal[(scalars[i] >> ((4 - digit) * 4)) & 15];
    }
    for (u32 i = 0; i < 4; ++i) output[at++] = suffix[i];
    return bytes;
}

} // namespace maicord
