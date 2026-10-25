#pragma once
#include "maicord/app/video_stream_decoders.hpp"
#include "maicord/model/voice_state_store.hpp"

namespace maicord {
// Call view video bookkeeping, independent of the window and renderer:
// - receive wants: a remote camera is wanted only while its tile is visible
//   (op 15 quality 0 otherwise), sent once per change;
// - frames: visible streams are polled; a picture is presented only when its
//   sequence (or the tile's reduction extent) changed, so presentation never
//   outpaces decoding;
// - every texture reference returned by the frame source is released in the
//   same pump, presented or not.
inline constexpr u32 kCallVideoStreams = 16;
// Longest presented side: 1440p streams stay native, 4K is reduced on the GPU.
inline constexpr u32 kCallVideoMaximumExtent = 2560;

struct CallVideoHost {
    void* context;
    // Latest picture of a stream; true transfers one texture reference.
    bool (*frame)(void* context, u32 ssrc, VideoStreamFrame* output) noexcept;
    // Copies the picture for user/ssrc, reduced so its longer side fits extent (0: native).
    bool (*present)(void* context, u64 user_id, u32 ssrc, const VideoStreamFrame& frame, u32 extent) noexcept;
    void (*release)(void* context, VideoStreamFrame* frame) noexcept;
    bool (*wanted)(void* context, u64 user_id, bool wanted) noexcept;
    // Optional: whether the presented picture is still resident (atlas eviction).
    bool (*resident)(void* context, u64 user_id, u32 ssrc) noexcept;
};
// One announced camera this pump: its tile's visibility and longest side in pixels.
struct CallVideoTarget {
    u64 user_id;
    u32 ssrc;
    u32 tile_pixels;
    bool visible;
};
enum class CallVideoWant : u8 {
    unknown,
    unwanted,
    wanted
};
struct CallVideoStream {
    u64 user_id;
    u32 ssrc, sequence, extent;
    u16 display_width, display_height; // Zero until a picture was presented.
    CallVideoWant want;
    bool ready;
};
struct CallVideoPresenter {
    CallVideoStream streams[kCallVideoStreams];
    u32 count;
    u64 acquired, released;
};
struct CallVideoPumpResult {
    bool presented;    // A new picture went to the renderer.
    bool changed;      // Stream readiness or extents changed (relayout tiles).
    bool visible;      // Some visible stream exists: keep polling.
    bool want_pending; // A want could not be sent; retry on the next pump.
};

void call_video_presenter_reset(CallVideoPresenter*) noexcept;
// Reconciles streams with targets (sorted or not), toggles wants, and when
// poll is set fetches and presents visible pictures.
CallVideoPumpResult call_video_presenter_pump(CallVideoPresenter*, const CallVideoHost&,
                                              ConstSlice<CallVideoTarget> targets, bool poll) noexcept;
const CallVideoStream* call_video_presenter_find(const CallVideoPresenter&, u64 user_id) noexcept;
// The call view's next stream poll in milliseconds (zero: none). A module
// that signals new pictures is polled on its signal, with a slow fallback for
// wants and pictures it cannot signal; otherwise visible streams are sampled
// at display rate. A want that could not be sent retries on its own interval.
inline constexpr u64 kCallVideoPollMs = 15;
inline constexpr u64 kCallVideoSignaledPollMs = 250;
inline constexpr u64 kCallVideoWantRetryMs = 250;
constexpr u64 call_video_poll_interval(bool signaled) noexcept {
    return signaled ? kCallVideoSignaledPollMs : kCallVideoPollMs;
}
constexpr bool call_video_poll_due(u64 next_ms, u64 now_ms) noexcept {
    return !next_ms || now_ms >= next_ms;
}
// The schedule after a pump at now_ms that polled pictures or only wants.
u64 call_video_poll_after(u64 next_ms, u64 now_ms, bool polled, const CallVideoPumpResult& result,
                          bool signaled) noexcept;
// A new-picture signal makes an armed poll due; nothing visible stays unarmed.
constexpr u64 call_video_poll_signaled(u64 next_ms, u64 now_ms) noexcept {
    return next_ms ? now_ms : 0;
}
// Reduction extent for a picture shown in a tile_pixels tile: the largest
// integer GPU reduction whose result still covers the tile, capped at
// kCallVideoMaximumExtent. Zero keeps the native picture.
u32 call_video_reduction_extent(u32 width, u32 height, u32 tile_pixels) noexcept;
// Cameras the voice server announced (op 12) whose users get a call tile
// without a voice state in the channel. Only while the channel's voice states
// are unknown (none yet): once known they are authoritative, and a user who
// left or moved (VOICE_STATE_UPDATE) stays announced until op 13, which can
// arrive late or never. Returns the count written to output.
u32 call_video_unlisted_users(const VoiceStateStore& states, u64 channel_id, ConstSlice<CallVideoTarget> cameras,
                              Slice<u64> output) noexcept;
// The announced cameras (users and SSRCs, in order) differ: tiles reproject.
bool call_video_roster_changed(ConstSlice<CallVideoTarget> before, ConstSlice<CallVideoTarget> after) noexcept;
} // namespace maicord
