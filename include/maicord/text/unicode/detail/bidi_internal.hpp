#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/unicode/bidi.hpp"

namespace maicord {
// Internal analysis boundary: a bracket policy receives one fully weak-resolved
// isolating run sequence. The client's policy is bidi_tailored_brackets; the tests
// check the shared core with Unicode's N0 (tests/support/bidi_conformance.hpp).
using BidiBracketResolver=void (*)(Slice<UnicodeBidiCell>,ConstSlice<u32>,UnicodeBidiClass,
                                 UnicodeBidiClass,UnicodeBidiClass) noexcept;
// UAX #9 rev 51, P2 through I2, on one paragraph. A B is allowed only at the
// end; the caller splits paragraphs (P1). FSI always uses P2/P3's LTR
// fallback; auto_rtl is an explicit higher-level paragraph fallback.
// Needs one u32 scratch slot per cell; no allocation. Buffers must not overlap
// and must stay exclusively owned during resolution. On failure, working
// outputs are unusable and paragraph is cleared.
UnicodeBidiStatus bidi_resolve_with_brackets(Slice<UnicodeBidiCell>,UnicodeParagraphDirection,
                                            Slice<u32>,UnicodeBidiParagraph*,BidiBracketResolver,bool tailored=false) noexcept;
// Bracket pairs matched by scalar (at most 61 open), resolved by the context
// of the pair's surroundings instead of UAX #9 N0, as the platform text stack
// lays out message text.
void bidi_tailored_brackets(Slice<UnicodeBidiCell>,ConstSlice<u32>,UnicodeBidiClass,
                            UnicodeBidiClass,UnicodeBidiClass) noexcept;
// The profile message layout uses: tailored brackets, a numeric-context weak
// pass, and isolate controls processed as BN. scalar/original inputs stay
// unchanged; level 255 means exclusion from ordering, not deletion.
UnicodeBidiStatus bidi_resolve_tailored(Slice<UnicodeBidiCell>,UnicodeParagraphDirection,
    Slice<u32>,UnicodeBidiParagraph*) noexcept;
}
