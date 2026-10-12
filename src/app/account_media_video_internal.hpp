#pragma once
#include "maicord/app/account_media_worker.hpp"
namespace maicord {
// Called on the existing lane. Returns true when selection was handled;
// stopped means the lane is terminal; a failed terminal lane keeps retained
// ownership, and account stop must reject video_destroy before source release.
// The same reserved lane hosts MP4 or Ogg Opus; audio-only has no GPU lease.
bool account_media_video_dispatch(AccountMediaWorkerLane*, bool* stopped) noexcept;
bool account_media_video_reserved(const AccountMediaWorker*, u32 lane) noexcept;
bool account_media_video_destroy(AccountMediaWorker*) noexcept;
// UI thread: hands a refreshed signed URL to the selected progressive source
// waiting for one. False when no selected source wants this key's URL.
bool account_media_video_offer_url(AccountMediaWorker*, MediaAssetKey, Utf8View url) noexcept;
} // namespace maicord
