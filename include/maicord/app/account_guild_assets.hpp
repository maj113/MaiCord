#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/guild_assets.hpp"

namespace maicord {
// Prepares any picked image (validate, crop/resize, encode), sizes the body,
// builds it in a bank allocated for this call and submits it on the DETAILS
// owner. Bodies over the ordinary REST ceiling use the large-body path. The
// bank and any re-encoded image are erased and released before returning;
// request->info, when set, receives the preparation outcome.
RestClientSubmitResult account_guild_assets_submit(RestClient*, const GuildAssetRequest*,
                                                   u64 request_id) noexcept;
// Exact request ID only; a published response remains available for rest_take.
RestClientSubmitResult account_guild_assets_cancel(RestClient*, u64 request_id) noexcept;
// image_probe: preparation only, no transport. False when the image is unusable.
bool account_guild_assets_probe(const GuildAssetRequest*) noexcept;
// Upload timeout for bodies on the large path.
inline constexpr u32 kGuildAssetLargeTimeoutMs = 120 * 1000;
} // namespace maicord
