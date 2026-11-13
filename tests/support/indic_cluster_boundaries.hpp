#pragma once
// Indic syllable boundaries of one run of a modern Indic script through the client's syllable scanner
// (src/text/scripts/indic_syllables.hpp), as HarfBuzz segments it: vowel-constraint dotted circles, canonical
// decomposition and combining order (every part treated as mapped), the syllable grammar of the Microsoft Indic shaping
// specs. One output byte per scalar: 1 where a syllable starts. A syllable whose characters combining order moved
// starts at its lowest scalar. At most 256 scalars; sixteen work units each. Views are disjoint; errors preserve output.
#include "../../src/text/scripts/indic_syllables.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/unicode/unicode.hpp"

namespace maicord {

inline SfntStatus sfnt_indic_cluster_boundaries(SfntIndicProfile profile, ConstSlice<u32> scalars, Slice<u8> output,
                                                u64 work = 1000000) noexcept {
    if (!sfnt_indic_script(profile) || (scalars.count && !scalars.data) || (output.count && !output.data))
        return SfntStatus::invalid_input;
    if (output.count < scalars.count)
        return SfntStatus::output_too_small;
    if (static_cast<u64>(scalars.count) * 16 > work)
        return SfntStatus::resource_limit;
    if (byte_ranges_overlap_or_wrap(scalars.data, static_cast<u64>(scalars.count) * 4, output.data, scalars.count))
        return SfntStatus::invalid_input;
    // Up to 256 scalars: at most four UTF-8 bytes each, and three pieces plus a circle.
    if (scalars.count > 256)
        return SfntStatus::resource_limit;
    char text[1024];
    IndicPiece pieces[1024];
    u32 bytes = 0;
    for (u32 i = 0; i < scalars.count; ++i) {
        if (!unicode_is_scalar(scalars[i]))
            return SfntStatus::invalid_input;
        bytes += utf8_encode(scalars[i], text + bytes);
    }
    // Map UTF-16 sources back to scalar indices.
    u32 index[512];
    for (u32 i = 0, units = 0; i < scalars.count; ++i) {
        index[units++] = i;
        if (scalars[i] > 0xffff)
            index[units++] = i;
    }
    u8 starts[256]{};
    IndicReader reader{profile, {text, bytes}, nullptr};
    for (;;) {
        u32 length = 0;
        IndicSyllable outcome{};
        const auto status = indic_read_syllable(reader, {pieces, 1024}, &length, &outcome);
        if (status != SfntStatus::ok)
            return status;
        if (!length)
            break;
        u32 first = pieces[0].source;
        for (u32 i = 1; i < length; ++i)
            first = minimum(first, pieces[i].source);
        starts[index[first]] = 1;
    }
    copy_bytes(output.data, starts, scalars.count);
    return SfntStatus::ok;
}

} // namespace maicord
