#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/bidi.hpp"

namespace maicord {
// Internal analysis boundary: a bracket policy receives one fully weak-resolved
// isolating run sequence. The public resolver always uses Unicode 17 N0.
using BidiBracketResolver=void (*)(Slice<UnicodeBidiCell>,ConstSlice<u32>,UnicodeBidiClass,
                                 UnicodeBidiClass,UnicodeBidiClass) noexcept;
UnicodeBidiStatus bidi_resolve_with_brackets(Slice<UnicodeBidiCell>,UnicodeParagraphDirection,
                                            Slice<u32>,UnicodeBidiParagraph*,BidiBracketResolver,bool windows_profile=false) noexcept;
// Experimental model of Windows bracket behavior. It does not model Windows'
// other bidi deviations and is not a production profile.
void bidi_windows_brackets_candidate(Slice<UnicodeBidiCell>,ConstSlice<u32>,UnicodeBidiClass,
                                    UnicodeBidiClass,UnicodeBidiClass) noexcept;
// Experimental combined control/weak/bracket policy. Isolate controls get the
// BN processing class; scalar/original inputs remain unchanged, and level 255
// means exclusion from ordering rather than deletion from source. The public
// Unicode resolver does not select this profile; bracket and directional-control
// differences remain.
UnicodeBidiStatus bidi_windows_resolve_candidate(Slice<UnicodeBidiCell>,UnicodeParagraphDirection,
    Slice<u32>,UnicodeBidiParagraph*) noexcept;
}
