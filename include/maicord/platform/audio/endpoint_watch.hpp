#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

// The vocabulary of audio endpoint watching, shared by the Windows (MMDevice notifications) and Linux (PulseAudio
// subscription) backends. A watcher records what changed in a bit set and signals the audio thread's wake event;
// the audio thread takes the bits (an atomic exchange with zero) and decides what to reopen. The observer types
// themselves live with their backend: src/platform/audio/wasapi_endpoint_watch.hpp (COM observers) and, for Linux,
// the Session of src/platform/audio/pulse/pulse.hpp (a pa_context_subscribe subscription; session_take_dirty).
namespace maicord {

// Bits a watcher accumulates until the audio thread takes them.
inline constexpr LONG kAudioEndpointDefaultChanged = 1, kAudioEndpointTopologyChanged = 2;

// Which side of the device list a stream or a watcher is about.
enum class AudioFlow : u8 { render, capture };

// Text of an endpoint ID, compared as an opaque case-sensitive value: owned_id is NUL-terminated with
// owned_chars characters, and event_id (system-provided) is read at most that far plus its terminator.
inline bool audio_endpoint_id_matches(const wchar_t* event_id, const wchar_t* owned_id, usize owned_chars) noexcept {
    if (!event_id || !owned_id || !owned_chars)
        return false;
    for (usize i = 0; i < owned_chars; ++i)
        if (!event_id[i] || event_id[i] != owned_id[i])
            return false;
    return !event_id[owned_chars];
}

} // namespace maicord
