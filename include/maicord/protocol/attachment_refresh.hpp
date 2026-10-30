#pragma once
#include "maicord/protocol/cdn_url.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

// Only signed/unsigned CDN attachment origins qualify. External proxy
// resources and arbitrary URLs must never enter the authenticated resolver.
bool attachment_refresh_needed(Utf8View url, u64 now_seconds) noexcept;
u32 attachment_refresh_body(Utf8View url, Slice<u8> output) noexcept;
u32 attachment_refresh_response(Utf8View body, Utf8View original,
                                u64 now_seconds, Slice<char> output) noexcept;
// The official refresh request: POST /api/v10/attachments/refresh-urls with the
// attachment_refresh_body JSON, on one route without a major parameter.
inline constexpr Utf8View kAttachmentRefreshTarget = utf8_literal("/api/v10/attachments/refresh-urls");
inline constexpr RestRouteKey kAttachmentRefreshRoute{0x4154545245465245ull, 0, RestMethod::post,
                                                      RestMajorParameter::none};

} // namespace maicord
