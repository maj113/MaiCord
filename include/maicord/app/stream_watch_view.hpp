#pragma once
#include "maicord/app/gateway_stream_control.hpp"

namespace maicord {
// What the call view shows for the stream the user asked to watch, from the
// account module's stream snapshot. requested is the client's desired key
// (none: not watching); a request the worker has not picked up yet, and the
// previous stream's lingering snapshot, both read as connecting.
enum class StreamWatchState : u8 {
    none,
    connecting,
    live,
    reconnecting,
    ended,
};
// Mirrors UiCallStreamNotice.
enum class StreamWatchNotice : u8 {
    none,
    ended,
    full,
    not_allowed,
    limited,
    timed_out,
    failed,
    interrupted,
};
// An old snapshot of the same key that ended is ignored this long after a
// new request (the worker answers within a frame or two).
inline constexpr u64 kStreamWatchRequestGraceMs = 1'000;
struct StreamWatchView {
    GatewayStreamKey key;
    StreamWatchState state;
    StreamWatchNotice notice;
    u32 viewers;
    u32 video_ssrc; // Tagged SSRC for voice_video_frame; zero until announced.
    u16 volume_percent;
    bool paused;
};
StreamWatchView stream_watch_view(const GatewayClientStreamSnapshot& snapshot, const GatewayStreamKey& requested,
                                  u64 requested_ms, u64 now_ms) noexcept;
// Local stops and leaving the channel end silently; the rest explain themselves.
StreamWatchNotice stream_watch_notice(GatewayStreamEnd end, GatewayStreamDeleteReason reason) noexcept;
} // namespace maicord
