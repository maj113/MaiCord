#pragma once
#include "maicord/protocol/cdn_url.hpp"

namespace maicord {

// Only signed/unsigned CDN attachment origins qualify. External proxy
// resources and arbitrary URLs must never enter the authenticated resolver.
bool attachment_refresh_needed(Utf8View url, u64 now_seconds) noexcept;
u32 attachment_refresh_body(Utf8View url, Slice<u8> output) noexcept;
u32 attachment_refresh_response(Utf8View body, Utf8View original,
                                u64 now_seconds, Slice<char> output) noexcept;

} // namespace maicord
