#pragma once
// The Unicode bidi algorithm as UAX #9 rev 51 states it: the client's resolver core (bidi_resolve_with_brackets, P2
// through I2 on one paragraph) with the standard N0 bracket-pair rule instead of the tailored pairs message layout uses
// (bidi_tailored_brackets). The conformance data (BidiTest, BidiCharacterTest) checks the shared core through it. A B
// is allowed only at the end of the cells; FSI always uses P2/P3's LTR fallback. Needs one u32 scratch slot per cell.
#include "maicord/text/unicode/detail/bidi_internal.hpp"

namespace maicord {
namespace bidi_conformance {
using B = UnicodeBidiClass;
inline constexpr u32 kNone = ~0u;

inline B strong(B type) noexcept {
    return type == B::en || type == B::an ? B::r : type;
}
// U+232A pairs with U+3008 as U+3009 does (BD16 canonical equivalence).
inline u32 canonical_bracket(u32 scalar) noexcept {
    return scalar == 0x232a ? 0x3009 : scalar;
}

// N0 over one isolating run sequence: BD16 pairs (at most 63 open; an overflow pairs nothing), each resolved by its
// enclosed strong types, then by the preceding context.
inline void brackets(Slice<UnicodeBidiCell> cells, ConstSlice<u32> order, B sos, B, B embedding_direction) noexcept {
    struct Open {
        u32 paired, position;
    };
    Open stack[63];
    u32 depth = 0;
    for (u32 i = 0; i < order.count; ++i) {
        auto& cell = cells[order[i]];
        if (cell.resolved != B::on)
            continue;
        cell.link = kNone;
        const auto bracket = unicode_bidi_bracket(cell.scalar);
        if (bracket.type == UnicodeBracketType::open) {
            if (depth == 63)
                return;
            stack[depth++] = {canonical_bracket(bracket.paired), i};
        } else if (bracket.type == UnicodeBracketType::close) {
            const u32 scalar = canonical_bracket(cell.scalar);
            for (u32 j = depth; j > 0; --j)
                if (stack[j - 1].paired == scalar) {
                    cells[order[stack[j - 1].position]].link = i;
                    depth = j - 1;
                    break;
                }
        }
    }
    B previous = sos;
    for (u32 i = 0; i < order.count; ++i) {
        auto& cell = cells[order[i]];
        if (cell.resolved == B::on && cell.link != kNone) {
            const u32 close = cell.link;
            bool same = false, opposite = false;
            for (u32 j = i + 1; j < close; ++j) {
                const B type = strong(cells[order[j]].resolved);
                if (type == embedding_direction) {
                    same = true;
                    break;
                }
                if (type == B::l || type == B::r)
                    opposite = true;
            }
            if (same || opposite) {
                const B value = same ? embedding_direction : previous;
                cell.resolved = value;
                cells[order[close]].resolved = value;
                for (u32 j = i + 1; j < order.count && cells[order[j]].original == B::nsm; ++j)
                    cells[order[j]].resolved = value;
                for (u32 j = close + 1; j < order.count && cells[order[j]].original == B::nsm; ++j)
                    cells[order[j]].resolved = value;
            }
        }
        const B type = strong(cell.resolved);
        if (type == B::l || type == B::r)
            previous = type;
    }
}
} // namespace bidi_conformance

inline UnicodeBidiStatus unicode_bidi_resolve(Slice<UnicodeBidiCell> cells, UnicodeParagraphDirection direction,
                                              Slice<u32> scratch, UnicodeBidiParagraph* paragraph) noexcept {
    return bidi_resolve_with_brackets(cells, direction, scratch, paragraph, bidi_conformance::brackets);
}

} // namespace maicord
