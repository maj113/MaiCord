#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
// One complete native MP4 source. Image body/cache limits remain separate.
inline constexpr u32 kMediaMaximumEncodedVideoBytes = 64u * 1024u * 1024u;
// Progressive (HTTP Range) sources only: a sparse address reservation, never a
// commitment. Resident bytes are bounded separately by progressive_source.hpp.
inline constexpr u32 kMediaMaximumProgressiveVideoBytes = 1024u * 1024u * 1024u;
} // namespace maicord
