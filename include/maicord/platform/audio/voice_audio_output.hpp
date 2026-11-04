#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Fills interleaved 48 kHz stereo PCM16 on the audio thread; must not block.
using VoiceAudioRender = void (*)(void* context, Slice<i16> pcm, u64 now_us) noexcept;
// The default console render endpoint, event driven on a dedicated thread
// (~10 ms period, ~20 ms buffer); the engine converts PCM16 48 kHz stereo to
// the device format. Windows: shared-mode WASAPI. Linux: a PulseAudio (or
// pipewire-pulse) playback stream through libpulse, where the endpoint ID is
// the sink name widened to UTF-16. Device loss and default-device changes
// reopen the stream; with no sound server the start retries every second;
// stop joins the thread.
struct VoiceAudioOutput {
    void* runtime;
};
// Opens the given endpoint ID, falling back to the default while it is missing;
// null selects the default.
bool voice_audio_output_start_device(VoiceAudioOutput* output, const wchar_t* device_id, VoiceAudioRender render,
                                     void* context) noexcept;
void voice_audio_output_stop(VoiceAudioOutput* output) noexcept;
bool voice_audio_output_running(const VoiceAudioOutput& output) noexcept;
} // namespace maicord
