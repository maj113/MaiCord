#pragma once
// What the session's audio transport did and what it holds, read through the session's private workspace
// (src/app/media/account_video_session_internal.hpp) on the codec worker's thread.
#include "../../src/app/media/account_video_session_internal.hpp"

namespace maicord {

struct AccountVideoAudioProgress {
    u64 epoch{}, written_frames{}, submitted_frames{}, consumed_frames{};
};

// The transport's frame counts, only after it observed this session's playback epoch. Zero on another thread.
inline AccountVideoAudioProgress account_video_session_audio_progress(const AccountVideoSession* s) noexcept {
    if (!account_video_session_detail::thread(*s) || !s->workspace)
        return {};
    const auto a = media_audio_status(account_video_session_detail::audio_owner(*s));
    if (a.observed_epoch != s->playback_epoch)
        return {};
    return {a.observed_epoch, a.written_frames, a.submitted_frames, a.consumed_frames};
}

// The bytes of the session's workspace pages, zero without one.
inline u32 account_video_session_owned_bytes(const AccountVideoSession* s) noexcept {
    return s->workspace ? s->workspace->allocation_bytes : 0;
}

} // namespace maicord
