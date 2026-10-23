#include "maicord/app/account_voice_message.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u32 kAccessDenied = 0x80070005u; // E_ACCESSDENIED: Windows microphone privacy.

bool wasapi_start(void* self, const wchar_t* device, bool processing, VoiceAudioCaptureSink sink,
                  VoiceAudioCaptureTick tick, void* context) noexcept {
    return voice_audio_capture_start(static_cast<VoiceAudioCapture*>(self), device, processing, sink, tick, context);
}
void wasapi_stop(void* self) noexcept {
    voice_audio_capture_stop(static_cast<VoiceAudioCapture*>(self));
}
VoiceAudioCaptureStatus wasapi_status(void* self) noexcept {
    return voice_audio_capture_status(*static_cast<const VoiceAudioCapture*>(self));
}
constexpr VoiceMessageCaptureOps kWasapi{wasapi_start, wasapi_stop, wasapi_status};

u64 no_wake(void*, u64) noexcept {
    return 0;
}
void stop_capture(AccountVoiceMessage* v) noexcept {
    if (v->capture_running)
        v->capture_ops->stop(v->capture_self);
    v->capture_running = v->capturing = false;
}
void stop_preview(AccountVoiceMessage* v) noexcept {
    if (voice_audio_output_running(v->preview))
        voice_audio_output_stop(&v->preview);
    v->playing = false;
}
void rewind_preview(AccountVoiceMessage* v) noexcept {
    auto& p = *v->preview_state;
    ogg_opus_demux_reset(&p.demux);
    opus_decoder_reset(&p.decoder);
    p.decoded_frames = p.decoded_offset = 0;
    v->played_frames = 0;
}
void free_preview(AccountVoiceMessage* v) noexcept {
    if (!v->preview_state)
        return;
    opus_decoder_release(&v->preview_state->decoder);
    VirtualFree(v->preview_state, 0, MEM_RELEASE);
    v->preview_state = nullptr;
}
void discard(AccountVoiceMessage* v) noexcept {
    stop_capture(v);
    stop_preview(v);
    AcquireSRWLockExclusive(v->lock.native());
    voice_message_recorder_release(&v->recorder);
    free_preview(v);
    v->played_frames = 0;
    v->state = VoiceMessageState::idle;
    v->failure = VoiceMessageFailure::none;
    v->native_error = 0;
    ReleaseSRWLockExclusive(v->lock.native());
}
void fail(AccountVoiceMessage* v, VoiceMessageFailure failure, u32 native_error = 0) noexcept {
    stop_capture(v);
    AcquireSRWLockExclusive(v->lock.native());
    voice_message_recorder_release(&v->recorder);
    v->state = VoiceMessageState::failed;
    v->failure = failure;
    v->native_error = native_error;
    ReleaseSRWLockExclusive(v->lock.native());
}
bool start(AccountVoiceMessage* v) noexcept {
    if (v->state == VoiceMessageState::recording || v->state == VoiceMessageState::finished)
        return false;
    discard(v);
    const auto begun = voice_message_recorder_begin(&v->recorder, ++v->serial ^ static_cast<u32>(GetTickCount64()),
                                                    v->input_gain_percent);
    if (begun != VoiceMessageRecorderError::none) {
        fail(v, begun == VoiceMessageRecorderError::memory ? VoiceMessageFailure::memory : VoiceMessageFailure::encoder);
        return true;
    }
    v->state = VoiceMessageState::recording;
    v->capture_running = v->capture_ops->start(v->capture_self, v->input_device, v->processing,
                                               account_voice_message_sink, no_wake, v);
    if (!v->capture_running)
        fail(v, VoiceMessageFailure::microphone);
    return true;
}
bool finish(AccountVoiceMessage* v) noexcept {
    if (v->state != VoiceMessageState::recording)
        return false;
    stop_capture(v); // Joins the capture thread: no sink runs after this.
    AcquireSRWLockExclusive(v->lock.native());
    const bool finished = voice_message_recorder_finish(&v->recorder) == VoiceMessageRecorderError::none;
    bool preview = false;
    if (finished) {
        v->state = VoiceMessageState::finished;
        v->preview_state = static_cast<VoiceMessagePreview*>(
            VirtualAlloc(nullptr, sizeof(VoiceMessagePreview), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        auto* p = v->preview_state;
        preview = p &&
                  ogg_opus_demux_initialize(&p->demux, voice_message_recorder_stream(v->recorder), {}) ==
                      OggOpusError::none &&
                  opus_decoder_initialize(&p->decoder,
                                          {reinterpret_cast<u8*>((reinterpret_cast<usize>(p->decoder_storage) +
                                                                  kOpusDecoderStateAlignment - 1) &
                                                                 ~usize{kOpusDecoderStateAlignment - 1}),
                                           32768},
                                          1)
                      .ok();
        if (!preview)
            free_preview(v); // The recording still sends; it just cannot play.
        v->played_frames = 0;
    }
    ReleaseSRWLockExclusive(v->lock.native());
    if (!finished)
        // Nothing reached the encoder: the microphone delivered no audio.
        fail(v, VoiceMessageFailure::microphone);
    return true;
}
bool play(AccountVoiceMessage* v, bool wanted) noexcept {
    if (v->state != VoiceMessageState::finished || !v->preview_state)
        return false;
    AcquireSRWLockExclusive(v->lock.native());
    if (wanted && !v->playing && v->played_frames >= v->preview_state->demux.total_frames)
        rewind_preview(v);
    v->playing = wanted;
    ReleaseSRWLockExclusive(v->lock.native());
    if (wanted && !voice_audio_output_running(v->preview) &&
        !voice_audio_output_start_device(&v->preview, v->output_device, account_voice_message_render, v)) {
        AcquireSRWLockExclusive(v->lock.native());
        v->playing = false;
        ReleaseSRWLockExclusive(v->lock.native());
    }
    return true;
}
// Fills one decoded packet; false at the end of the stream.
bool decode_next(VoiceMessagePreview* p) noexcept {
    for (;;) {
        const auto packet = ogg_opus_demux_next(&p->demux, {});
        if (!packet.ok())
            return false;
        const auto decoded = opus_decoder_decode(&p->decoder, packet.bytes, {p->decoded, kOpusDecoderMaximumFrames});
        if (!decoded.ok())
            return false;
        const u32 end = decoded.frames > packet.trim_back_frames ? decoded.frames - packet.trim_back_frames : 0;
        if (packet.trim_front_frames >= end)
            continue;
        p->decoded_offset = packet.trim_front_frames;
        p->decoded_frames = end;
        return true;
    }
}
} // namespace

void account_voice_message_initialize(AccountVoiceMessage* v, const VoiceMessageCaptureOps* ops,
                                      void* capture_self) noexcept {
    if (!v)
        return;
    v->lock = SRWLOCK_INIT;
    v->capture_ops = ops ? ops : &kWasapi;
    v->capture_self = ops ? capture_self : &v->capture;
    const auto defaults = voice_audio_settings_defaults();
    v->input_gain_percent = defaults.input_gain_percent;
    v->output_gain_percent = defaults.output_gain_percent;
    v->processing = defaults.voice_processing;
}

void account_voice_message_configure(AccountVoiceMessage* v, const VoiceAudioSettings& settings) noexcept {
    if (!v || !v->capture_ops)
        return;
    // A recording keeps the device it opened; the next one uses the new settings.
    copy_bytes(v->input_device, settings.input_device, sizeof(v->input_device));
    copy_bytes(v->output_device, settings.output_device, sizeof(v->output_device));
    v->input_device[kVoiceDeviceIdCharacters - 1] = v->output_device[kVoiceDeviceIdCharacters - 1] = 0;
    v->input_gain_percent = minimum<u16>(settings.input_gain_percent, 200);
    v->output_gain_percent = minimum<u16>(settings.output_gain_percent, 200);
    v->processing = settings.voice_processing;
}

void account_voice_message_sink(void* context, ConstSlice<i16> mono, u64) noexcept {
    auto* v = static_cast<AccountVoiceMessage*>(context);
    AcquireSRWLockExclusive(v->lock.native());
    if (v->state == VoiceMessageState::recording && v->recorder.block)
        voice_message_recorder_push(&v->recorder, mono);
    ReleaseSRWLockExclusive(v->lock.native());
}

void account_voice_message_render(void* context, Slice<i16> pcm, u64) noexcept {
    auto* v = static_cast<AccountVoiceMessage*>(context);
    const u32 frames = pcm.count / 2;
    u32 written = 0;
    AcquireSRWLockExclusive(v->lock.native());
    const u32 gain_q16 = (u32{v->output_gain_percent} << 16) / 100;
    auto* p = v->preview_state;
    while (p && v->playing && written < frames) {
        if (p->decoded_offset >= p->decoded_frames && !decode_next(p)) {
            v->playing = false; // The end: the next play starts over.
            v->played_frames = p->demux.total_frames;
            break;
        }
        const u32 count = minimum(frames - written, p->decoded_frames - p->decoded_offset);
        for (u32 i = 0; i < count; ++i) {
            const i64 scaled = (static_cast<i64>(p->decoded[p->decoded_offset + i]) * gain_q16) >> 16;
            const i16 sample = static_cast<i16>(scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled);
            pcm[(written + i) * 2] = pcm[(written + i) * 2 + 1] = sample;
        }
        p->decoded_offset += count;
        v->played_frames += count;
        written += count;
    }
    ReleaseSRWLockExclusive(v->lock.native());
    clear_bytes(pcm.data + written * 2, (pcm.count - written * 2) * static_cast<u32>(sizeof(i16)));
}

bool account_voice_message_command(AccountVoiceMessage* v, const VoiceMessageCommand* command,
                                   VoiceMessageStatus* status) noexcept {
    if (!v || !v->capture_ops)
        return false;
    bool accepted = true;
    // A microphone that cannot open (missing, or blocked by privacy settings)
    // ends the recording; the capture would otherwise retry forever.
    if (v->state == VoiceMessageState::recording && v->capture_running) {
        const auto captured = v->capture_ops->status(v->capture_self);
        v->capturing = captured.state == VoiceCaptureState::capturing;
        if (captured.state == VoiceCaptureState::waiting && captured.native_error)
            fail(v, captured.native_error == kAccessDenied ? VoiceMessageFailure::microphone_access
                                                           : VoiceMessageFailure::microphone,
                 captured.native_error);
    }
    if (command) {
        switch (command->operation) {
        case VoiceMessageOperation::status:
            break;
        case VoiceMessageOperation::start:
            accepted = start(v);
            break;
        case VoiceMessageOperation::finish:
            accepted = finish(v);
            break;
        case VoiceMessageOperation::discard:
            discard(v);
            break;
        case VoiceMessageOperation::play:
        case VoiceMessageOperation::pause:
            accepted = play(v, command->operation == VoiceMessageOperation::play);
            break;
        case VoiceMessageOperation::copy: {
            const auto stream = voice_message_recorder_stream(v->recorder);
            accepted = v->state == VoiceMessageState::finished && stream.count && command->output.data &&
                       command->output.count >= stream.count;
            if (accepted)
                copy_bytes(command->output.data, stream.data, stream.count);
            break;
        }
        default:
            accepted = false;
        }
    }
    if (status) {
        *status = {};
        AcquireSRWLockShared(v->lock.native());
        status->state = v->state;
        status->failure = v->failure;
        status->native_error = v->native_error;
        status->capturing = v->capturing;
        status->limit_reached = v->recorder.limit_reached;
        status->playing = v->playing;
        status->duration_ms = voice_message_recorder_duration_ms(v->recorder);
        status->position_ms = static_cast<u32>(v->played_frames * 1000 / kVoiceMessageSampleRate);
        status->bytes = voice_message_recorder_stream(v->recorder).count;
        status->level_count = v->recorder.live_count;
        voice_message_recorder_levels(v->recorder, {status->levels, kVoiceMessageLiveLevels});
        if (v->state == VoiceMessageState::finished)
            voice_message_recorder_waveform(v->recorder, status->waveform);
        ReleaseSRWLockShared(v->lock.native());
    }
    return accepted;
}

void account_voice_message_release(AccountVoiceMessage* v) noexcept {
    if (v && v->capture_ops)
        discard(v);
}
} // namespace maicord
