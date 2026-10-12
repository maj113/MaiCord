#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Fills interleaved 48 kHz stereo PCM16 on the audio thread; must not block.
using VoiceAudioRender = void (*)(void* context, Slice<i16> pcm, u64 now_us) noexcept;
enum class VoiceAudioOutputState : u8 {
    stopped,
    opening,
    playing,
    waiting // No usable device; retried periodically and on endpoint changes.
};
struct VoiceAudioOutputStatus {
    VoiceAudioOutputState state;
    u32 buffer_frames, period_frames, opens, native_error;
};
// Shared-mode WASAPI on the default console render endpoint, event driven on
// a dedicated thread (~10 ms engine period, ~20 ms buffer). The engine
// converts PCM16 48 kHz stereo to the mix format. Device loss and
// default-device changes reopen the stream; stop joins the thread.
struct VoiceAudioOutput {
    void* runtime;
};
bool voice_audio_output_start(VoiceAudioOutput* output, VoiceAudioRender render, void* context) noexcept;
// Opens the given endpoint ID instead, falling back to the default while it is missing.
bool voice_audio_output_start_device(VoiceAudioOutput* output, const wchar_t* device_id, VoiceAudioRender render,
                                     void* context) noexcept;
void voice_audio_output_stop(VoiceAudioOutput* output) noexcept;
bool voice_audio_output_running(const VoiceAudioOutput& output) noexcept;
VoiceAudioOutputStatus voice_audio_output_status(const VoiceAudioOutput& output) noexcept;
} // namespace maicord
