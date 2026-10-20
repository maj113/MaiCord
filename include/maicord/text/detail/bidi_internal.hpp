#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/bidi.hpp"

namespace maicord {
// Internal analysis boundary: a bracket policy receives one fully weak-resolved
// isolating run sequence. The public resolver always uses Unicode 17 N0.
using BidiBracketResolver=void (*)(Slice<UnicodeBidiCell>,ConstSlice<u32>,UnicodeBidiClass,
                                 UnicodeBidiClass,UnicodeBidiClass) noexcept;
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
