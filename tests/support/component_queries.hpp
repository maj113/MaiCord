#pragma once
// Reads a component forest (text/opentype/components.hpp) back for tests. A node whose flags hold bit 0 is a dormant
// mark alias (src/text/opentype/components.cpp); UINT32_MAX is no node. Inputs need a successfully initialized forest.
#include "maicord/text/opentype/components.hpp"
#include "../../src/text/opentype/sfnt_read.hpp"

namespace maicord {
namespace component_queries {
inline constexpr u32 kNil = 0xffffffffu;
inline constexpr u8 kAlias = 1;
} // namespace component_queries

// The script metadata (reserved's upper byte) of the first owned component; an alias stays a singleton.
inline SfntStatus sfnt_component_first_metadata(const SfntComponentForest& f, u32 node, u8* output,
                                                u64 work = 1000000, u64* used = nullptr) noexcept {
    using namespace component_queries;
    if (used)
        *used = 0;
    if (!output)
        return SfntStatus::invalid_input;
    *output = 0;
    if (f.nodes.count && !f.nodes.data)
        return SfntStatus::invalid_input;
    sfnt_read::WorkReport report{work, work, used};
    while (node != kNil) {
        if (!sfnt_read::spend(work))
            return SfntStatus::resource_limit;
        if (node >= f.nodes.count)
            return SfntStatus::invalid_input;
        const auto& n = f.nodes[node];
        if ((n.flags & kAlias) || n.left == kNil) {
            *output = static_cast<u8>(n.reserved >> 8);
            return SfntStatus::ok;
        }
        node = n.left;
    }
    return SfntStatus::ok;
}

// Logical source extent of an owned sequence or one dormant mark. Traversal does not follow a dormant mark's
// attachment into its preceding base sequence. Success sets the min/max sources. Failure leaves first=UINT32_MAX,
// last=0 when both output pointers are valid; work_used includes failed traversal.
inline SfntStatus sfnt_component_source_bounds(const SfntComponentForest& forest, u32 node, u32* first, u32* last,
                                               u64 work = 1000000, u64* used = nullptr) noexcept {
    using namespace component_queries;
    if (used)
        *used = 0;
    if (!first || !last)
        return SfntStatus::invalid_input;
    *first = kNil;
    *last = 0;
    if (node == kNil)
        return SfntStatus::ok;
    if (node >= forest.nodes.count || !forest.nodes.data)
        return SfntStatus::invalid_input;
    sfnt_read::WorkReport report{work, work, used};
    u32 pending[64], depth = 0, lo = kNil, hi = 0;
    pending[depth++] = node;
    while (depth) {
        if (!sfnt_read::spend(work))
            return SfntStatus::resource_limit;
        const u32 at = pending[--depth];
        if (at >= forest.nodes.count)
            return SfntStatus::invalid_input;
        const auto& n = forest.nodes[at];
        lo = minimum(lo, n.source);
        hi = maximum(hi, n.source);
        if (n.flags & kAlias)
            continue;
        if (n.left != kNil) {
            if (depth == 64)
                return SfntStatus::resource_limit;
            pending[depth++] = n.left;
        }
        if (n.right != kNil) {
            if (depth == 64)
                return SfntStatus::resource_limit;
            pending[depth++] = n.right;
        }
    }
    *first = lo;
    *last = hi;
    return SfntStatus::ok;
}

} // namespace maicord
