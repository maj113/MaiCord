#pragma once
#include "maicord/app/voice_message.hpp"
#include "maicord/media/ogg_opus.hpp"
#include "maicord/media/opus_decoder.hpp"
#include "maicord/platform/voice_audio_capture.hpp"
#include "maicord/platform/voice_audio_output.hpp"

#include "maicord/core/win32_records.hpp"

namespace maicord {
// The microphone behind a recording. Production opens shared-mode WASAPI on
// the selected input (voice_audio_capture); tests substitute a synthetic
// source that calls the sink themselves.
struct VoiceMessageCaptureOps {
    bool (*start)(void* self, const wchar_t* device, bool processing, VoiceAudioCaptureSink sink,
                  VoiceAudioCaptureTick tick, void* context) noexcept;
    void (*stop)(void* self) noexcept;
    VoiceAudioCaptureStatus (*status)(void* self) noexcept;
};

// Preview decode state, committed only while a finished recording exists.
struct VoiceMessagePreview {
    OggOpusDemuxer demux;
    OpusDecoder decoder;
    i16 decoded[kOpusDecoderMaximumFrames];
    u8 decoder_storage[32768 + kOpusDecoderStateAlignment]; // Aligned in use.
    u32 decoded_frames, decoded_offset;
};
// Account-module owner of one recording at a time: capture, encode, preview.
// The capture thread feeds the recorder under the lock; finish stops (joins)
// the capture before closing the stream, so the finished stream is immutable
// and the preview renders it on the output thread.
struct AccountVoiceMessage {
    SrwLock lock;
    const VoiceMessageCaptureOps* capture_ops;
    void* capture_self;
    VoiceAudioCapture capture; // The WASAPI source's state.
    VoiceAudioOutput preview;
    VoiceMessageRecorder recorder;
    VoiceMessageState state;
    VoiceMessageFailure failure;
    u32 native_error;
    u32 serial;
    bool capture_running;
    bool capturing;
    // Latest voice settings: input device, processing and gains.
    wchar_t input_device[kVoiceDeviceIdCharacters];
    wchar_t output_device[kVoiceDeviceIdCharacters];
    u16 input_gain_percent, output_gain_percent;
    bool processing;
    // Preview decode, on the output thread under the lock.
    VoiceMessagePreview* preview_state;
    u64 played_frames; // Audible frames rendered since the preview started.
    bool playing;
};
// Zeroed storage; null ops select WASAPI.
void account_voice_message_initialize(AccountVoiceMessage*, const VoiceMessageCaptureOps* ops = nullptr,
                                      void* capture_self = nullptr) noexcept;
void account_voice_message_configure(AccountVoiceMessage*, const VoiceAudioSettings&) noexcept;
// Applies the operation (if any) and fills status. False rejects the command.
bool account_voice_message_command(AccountVoiceMessage*, const VoiceMessageCommand*, VoiceMessageStatus*) noexcept;
void account_voice_message_release(AccountVoiceMessage*) noexcept;
// The capture sink, exposed for synthetic sources.
void account_voice_message_sink(void* context, ConstSlice<i16> mono, u64 now_us) noexcept;
// The preview render callback (48 kHz stereo PCM16), exposed for tests.
void account_voice_message_render(void* context, Slice<i16> pcm, u64 now_us) noexcept;
} // namespace maicord
