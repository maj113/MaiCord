#pragma once

// Stream audio for Go Live ("sound sharing"): what the computer plays, never MaiCord's own output (viewers must not hear
// the call), as 48 kHz stereo PCM16 blocks stamped with the client clock, for the stream's Opus encoder.
//
// Windows 10 2004 (build 19041) and later: WASAPI process loopback. Sharing a window records that window's process
// tree; sharing a screen records every process but MaiCord's own tree. Earlier builds have no loopback that can leave
// MaiCord out, so sound sharing is unavailable there.
// Linux: a PipeWire capture stream (libpipewire-0.3, dlopen()ed) linked, through the PipeWire registry, to the output
// of every application's playback stream except MaiCord's own (kept up to date as streams come and go); PipeWire mixes
// them. Without PipeWire (the library or the daemon) sound sharing is unavailable: never a sink monitor, which would
// carry the call.

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kStreamAudioRate = 48'000;
inline constexpr u32 kStreamAudioChannels = 2;

enum class StreamAudioScope : u8 {
    everything,  // every application but MaiCord (screen sharing)
    application, // the shared window's process (its tree on Windows); everything but MaiCord without a process id
};

// What the running system gives, and why not when it gives nothing.
enum class StreamAudioMethod : u8 {
    unavailable_windows_version, // Windows 10 before 2004 (build 19041): no loopback that leaves MaiCord out
    unavailable_no_pipewire,     // Linux without libpipewire-0.3 or a running PipeWire daemon
    process_loopback,            // Windows: per process tree
    application_streams,         // Linux: the PipeWire playback streams of the other applications
};
constexpr bool stream_audio_available(StreamAudioMethod method) noexcept {
    return method == StreamAudioMethod::process_loopback || method == StreamAudioMethod::application_streams;
}
StreamAudioMethod stream_audio_method() noexcept;

// Called on the capture thread with interleaved stereo samples (frames = samples / 2, any count) whose first frame was
// heard at timestamp_us (microseconds of clock_microseconds(clock_initialize(), 0, clock_now())). Must not block.
struct StreamAudioSink {
    void* context;
    void (*block)(void* context, ConstSlice<i16> stereo, u64 timestamp_us) noexcept;
};

struct StreamAudioOptions {
    StreamAudioScope scope;
    u32 process_id; // application: the shared window's process
};

// Starts the capture thread; the device or daemon may come and go and the capture reopens. False when the system has no
// method (stream_audio_method) or nothing could start. Stop joins the thread.
struct StreamAudioCapture {
    void* runtime;
};
bool stream_audio_start(StreamAudioCapture* capture, const StreamAudioOptions& options,
                        const StreamAudioSink& sink) noexcept;
void stream_audio_stop(StreamAudioCapture* capture) noexcept;

} // namespace maicord
