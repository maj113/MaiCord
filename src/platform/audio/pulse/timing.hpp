#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/audio/pulse/abi.hpp"

// The arithmetic of the PulseAudio transport: sample formats, buffer attributes, the playback timeline and the
// reopen schedule. No operating system and no clock of its own: every function takes the times it needs, so the
// unit tests (tests/platform/audio/pulse_timing.cpp) drive it with an injected clock and the workers pass the
// monotonic microsecond clock (voice_playback_now_us).
namespace maicord::pulse {

// The Windows engine timing the voice paths keep: 10 ms frames (480 samples at 48 kHz), a 20 ms render buffer
// and a 40 ms capture buffer.
inline constexpr u32 kVoiceRate = 48000;
inline constexpr u32 kVoicePeriodMs = 10;
inline constexpr u32 kRenderBufferMs = 20;
inline constexpr u32 kCaptureBufferMs = 40;
// The media player and the notification sounds tolerate more: a 40 ms media buffer, the 20 ms of the voice
// render buffer for notifications; both are asked for in 10 ms requests.
inline constexpr u32 kMediaBufferMs = 40;
inline constexpr u32 kNotificationBufferMs = 20;
// How long a requested drain (the server confirming that everything written has been heard) is waited for before the
// stream counts as finished anyway: the buffer plus any sink's latency fit in it, and a sink that never answers a
// drain (a PulseAudio null sink) does not hold a stream open forever.
inline constexpr u64 kDrainGraceMs = 500;

// Bytes of one frame (all channels) of a sample format; 0 for a format this transport does not use.
constexpr u32 frame_bytes(i32 format, u32 channels) noexcept {
    switch (format) {
    case kSampleS16LE:
        return 2 * channels;
    case kSampleFloat32LE:
        return 4 * channels;
    default:
        return 0;
    }
}

// Whole frames in `milliseconds` at `rate`, rounded down.
constexpr u32 frames_in_milliseconds(u32 rate, u32 milliseconds) noexcept {
    return static_cast<u32>(static_cast<u64>(rate) * milliseconds / 1000u);
}

// Buffer attributes of a playback stream whose whole latency (the server's queue and the device's) is asked to
// be `buffer_ms` through PA_STREAM_ADJUST_LATENCY: a target length of that many frames, a request whenever
// `period_ms` of it is free, and no prebuffering (the stream is created corked, filled once and started, like a
// WASAPI client). The maximum length is left to the server so a late worker queues instead of dropping.
constexpr BufferAttr playback_attributes(u32 rate, u32 bytes_per_frame, u32 buffer_ms, u32 period_ms) noexcept {
    return {kInvalid, frames_in_milliseconds(rate, buffer_ms) * bytes_per_frame, 0,
            frames_in_milliseconds(rate, period_ms) * bytes_per_frame, kInvalid};
}

// Buffer attributes of a record stream: at most `buffer_ms` queued on the server before it overruns, delivered in
// fragments of `period_ms` (with PA_STREAM_ADJUST_LATENCY the fragment size sets the capture latency).
constexpr BufferAttr record_attributes(u32 rate, u32 bytes_per_frame, u32 buffer_ms, u32 period_ms) noexcept {
    return {frames_in_milliseconds(rate, buffer_ms) * bytes_per_frame, kInvalid, kInvalid, kInvalid,
            frames_in_milliseconds(rate, period_ms) * bytes_per_frame};
}

// floor(microseconds * rate / 1,000,000) without overflowing the product, saturating at the u64 maximum.
constexpr u64 frames_from_microseconds(u64 microseconds, u32 rate) noexcept {
    const u64 whole = microseconds / 1'000'000u;
    const u64 fraction = microseconds % 1'000'000u * rate / 1'000'000u;
    if (rate && whole > ~u64{0} / rate)
        return ~u64{0};
    const u64 base = whole * rate;
    return base > ~u64{0} - fraction ? ~u64{0} : base + fraction;
}

// floor(frames * 1,000,000 / rate), saturating; 0 for a zero rate.
constexpr u64 microseconds_from_frames(u64 frames, u32 rate) noexcept {
    if (!rate)
        return 0;
    const u64 whole = frames / rate;
    const u64 fraction = frames % rate * 1'000'000u / rate;
    if (whole > ~u64{0} / 1'000'000u)
        return ~u64{0};
    const u64 base = whole * 1'000'000u;
    return base > ~u64{0} - fraction ? ~u64{0} : base + fraction;
}

// The frames of a stream that have been heard. `anchor` is the playback position (in frames of the source
// timeline) the stream started at, `stream_us` its own audible position (track_time), `submitted` the frames
// written to it plus the anchor, and `previous` the answer given last time: the result never goes back and never
// passes what was submitted.
constexpr u64 played_frames(u64 anchor, u64 stream_us, u32 rate, u64 submitted, u64 previous) noexcept {
    const u64 advanced = frames_from_microseconds(stream_us, rate);
    u64 played = advanced > ~u64{0} - anchor ? ~u64{0} : anchor + advanced;
    if (played > submitted)
        played = submitted;
    return played < previous ? previous : played;
}

// Whole frames to produce for a server request of `writable_bytes`, at most `limit` (0 when it asks for less than
// a frame).
constexpr u32 request_frames(usize writable_bytes, u32 bytes_per_frame, u32 limit) noexcept {
    if (!bytes_per_frame)
        return 0;
    const usize frames = writable_bytes / bytes_per_frame;
    return frames > limit ? limit : static_cast<u32>(frames);
}

// Milliseconds a timer is armed with to fire at `wake_us` (0: at once), rounded up so it never fires early;
// the timeline is the one `now_us` is on.
constexpr u32 due_milliseconds(u64 wake_us, u64 now_us) noexcept {
    if (wake_us <= now_us)
        return 0;
    const u64 microseconds = wake_us - now_us;
    const u64 milliseconds = microseconds / 1000u + (microseconds % 1000u ? 1u : 0u);
    return milliseconds > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<u32>(milliseconds);
}

// Whether a requested drain is over: the server confirmed it, or the grace period since the request has passed
// (`now_ms` and `deadline_ms` are on the same millisecond clock).
constexpr bool drain_finished(bool requested, bool confirmed, u64 deadline_ms, u64 now_ms) noexcept {
    return requested && (confirmed || now_ms >= deadline_ms);
}
// When a worker may try to open its stream again after a failure.
struct RetrySchedule {
    u64 not_before_us;
};
constexpr bool retry_due(const RetrySchedule& schedule, u64 now_us) noexcept {
    return now_us >= schedule.not_before_us;
}
constexpr void retry_after(RetrySchedule* schedule, u64 now_us, u64 delay_us) noexcept {
    schedule->not_before_us = now_us + delay_us;
}

// The pieces a captured fragment is cut into for the sink (the voice engine takes up to one 10 ms frame at a
// time): the size of the piece at `offset` of a fragment of `frames`.
constexpr u32 capture_piece(u32 frames, u32 offset, u32 piece_frames) noexcept {
    const u32 remaining = offset < frames ? frames - offset : 0;
    return remaining > piece_frames ? piece_frames : remaining;
}

} // namespace maicord::pulse
