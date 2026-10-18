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
// Longest presented side; 4K pictures are reduced on the GPU.
inline constexpr u32 kCallVideoMaximumExtent = 1920;

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
