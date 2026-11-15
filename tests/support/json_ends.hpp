#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

// The deepest nesting that json_index_ends follows.
constexpr u32 kJsonEndsDepth = 64;

// The exact table of bracket ends of an index (see json_reader.hpp), built in one pass over the index: the
// reference for the tables that the checked visits and the ETF transcoder write. False when the brackets do not
// pair, when they nest deeper than kJsonEndsDepth or when `ends` is shorter than the index.
inline bool json_index_ends(const Utf8View& input, const ConstSlice<u32>& structurals, const Slice<u32>& ends) noexcept {
    if (ends.count < structurals.count) return false;
    // The open containers: the index position of each bracket and the deepest level seen inside it.
    u32 open[kJsonEndsDepth], deepest[kJsonEndsDepth];
    u32 depth = 0;
    for (u32 index = 0; index < structurals.count; ++index) {
        const u32 at = structurals[index];
        if (at >= input.bytes) return false;
        const u8 value = static_cast<u8>(input.data[at]);
        // Of the indexed bytes only the four brackets have bit 6. Bit 2 separates } and ] from { and [.
        if (!(value & 0x40)) continue;
        if (!(value & 4)) {
            if (depth == kJsonEndsDepth) return false;
            open[depth] = index;
            deepest[depth] = depth + 1;
            ++depth;
            continue;
        }
        if (!depth) return false;
        --depth;
        const u32 first = open[depth];
        // { is 7B and } is 7D, [ is 5B and ] is 5D: a pair differs in bits 1 and 2 only.
        if ((static_cast<u8>(input.data[structurals[first]]) ^ value) != 6) return false;
        ends.data[first] = index;
        ends.data[index] = deepest[depth] - depth;
        if (depth && deepest[depth] > deepest[depth - 1]) deepest[depth - 1] = deepest[depth];
    }
    return depth == 0;
}

} // namespace maicord
