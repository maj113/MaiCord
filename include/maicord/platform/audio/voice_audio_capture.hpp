#pragma once
#include "maicord/app/voice/audio_settings.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {
// Receives 48 kHz mono PCM16 on the capture thread; must not block.
using VoiceAudioCaptureSink = void (*)(void* context, ConstSlice<i16> mono, u64 now_us) noexcept;
// Runs after every wake (audio or timer) and returns the absolute time
// (voice_playback_now_us clock) of the next wanted wake, or 0 for none. The
// sink and tick never run concurrently.
using VoiceAudioCaptureTick = u64 (*)(void* context, u64 now_us) noexcept;
struct VoiceAudioCaptureStatus {
    VoiceCaptureState state;
    u32 native_error;
};
// Capture on the given endpoint (empty: the default communications capture
// device), event driven on a dedicated thread with a 256 KiB stack
// reservation for the sink's encoder. The engine converts to 48 kHz mono
// PCM16 (~40 ms buffer, ~10 ms fragments). Windows: shared-mode WASAPI, where
// communications processing asks Windows for its echo cancellation, noise
// suppression and gain control and otherwise a raw stream is preferred. Linux:
// a PulseAudio (or pipewire-pulse) record stream through libpulse; the
// endpoint ID is the source name widened to UTF-16 (a sink's ".monitor"
// source is a valid ID) and communications is ignored. Device loss and
// default-device changes reopen the stream; with no sound server the status
// stays `waiting` with a native error and the start retries; stop joins.
struct VoiceAudioCapture {
    void* runtime;
};
bool voice_audio_capture_start(VoiceAudioCapture* capture, const wchar_t* device_id, bool communications,
                               VoiceAudioCaptureSink sink, VoiceAudioCaptureTick tick, void* context) noexcept;
void voice_audio_capture_stop(VoiceAudioCapture* capture) noexcept;
bool voice_audio_capture_running(const VoiceAudioCapture& capture) noexcept;
VoiceAudioCaptureStatus voice_audio_capture_status(const VoiceAudioCapture& capture) noexcept;

// Active endpoints for the settings selectors, default first. Names are
// UTF-8 friendly names truncated at a character boundary.
inline constexpr u32 kVoiceAudioDeviceNameBytes = 96;
struct VoiceAudioDevice {
    wchar_t id[kVoiceDeviceIdCharacters];
    char name[kVoiceAudioDeviceNameBytes];
    u32 name_bytes;
    bool is_default;
};
u32 voice_audio_devices(bool capture, Slice<VoiceAudioDevice> output) noexcept;
} // namespace maicord
