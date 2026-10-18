#include "maicord/app/voice_message.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/base64.hpp"

namespace maicord {
void voice_message_waveform(ConstSlice<u8> levels, u8 (&waveform)[kVoiceMessageWaveformSamples]) noexcept {
    const u32 frames = levels.data ? levels.count : 0;
    for (u32 i = 0; i < kVoiceMessageWaveformSamples; ++i) {
        if (!frames) {
            waveform[i] = 0;
            continue;
        }
        u32 first = static_cast<u32>(u64{i} * frames / kVoiceMessageWaveformSamples);
        u32 last = static_cast<u32>(u64{i + 1} * frames / kVoiceMessageWaveformSamples);
        if (last <= first)
            last = first + 1;
        u8 peak = 0;
        for (u32 f = first; f < last && f < frames; ++f)
            peak = levels[f] > peak ? levels[f] : peak;
        waveform[i] = peak;
    }
}

u32 voice_message_waveform_base64(const u8 (&waveform)[kVoiceMessageWaveformSamples], Slice<char> output) noexcept {
    static_assert(base64_encoded_size(kVoiceMessageWaveformSamples) == kVoiceMessageWaveformBase64Bytes);
    const auto encoded = base64_encode({waveform, kVoiceMessageWaveformSamples}, output);
    return encoded.ok() ? encoded.bytes : 0;
}

bool voice_message_available(const VoiceMessageGate& gate) noexcept {
    constexpr u32 required =
        channel_action_send_messages | channel_action_attach_files | channel_action_send_voice_messages;
    return (gate.channel_actions & required) == required && gate.draft_empty && !gate.attachments_pending &&
           !gate.editing && !gate.sending_restricted && !gate.slowmode_cooling;
}

VoiceMessageAction voice_message_flow_apply(VoiceMessageFlow* flow, VoiceMessageEvent event, u64 now_ms,
                                            u64 channel_id) noexcept {
    if (!flow)
        return VoiceMessageAction::none;
    auto& f = *flow;
    const auto reset = [&](VoiceMessageNotice notice) noexcept {
        f = {};
        f.notice = notice;
    };
    if (f.phase != VoiceMessagePhase::idle &&
        (event == VoiceMessageEvent::cancel || event == VoiceMessageEvent::channel_changed)) {
        reset(VoiceMessageNotice::none);
        return VoiceMessageAction::discard;
    }
    switch (f.phase) {
    case VoiceMessagePhase::idle:
        if ((event != VoiceMessageEvent::press && event != VoiceMessageEvent::toggle) || !channel_id)
            return VoiceMessageAction::none;
        f = {};
        f.phase = VoiceMessagePhase::recording;
        f.channel_id = channel_id;
        f.held = event == VoiceMessageEvent::press;
        f.hold_inside = f.held;
        f.pressed_ms = now_ms;
        return VoiceMessageAction::start;
    case VoiceMessagePhase::recording:
        switch (event) {
        case VoiceMessageEvent::release_inside:
            if (!f.held)
                return VoiceMessageAction::none;
            f.held = f.hold_inside = false;
            // A tap locks the recording hands-free; a hold sends on release.
            if (now_ms - f.pressed_ms < kVoiceMessageHoldMs)
                return VoiceMessageAction::none;
            f.phase = VoiceMessagePhase::sending;
            return VoiceMessageAction::finish_and_send;
        case VoiceMessageEvent::release_outside:
            if (!f.held)
                return VoiceMessageAction::none;
            reset(VoiceMessageNotice::none);
            return VoiceMessageAction::discard;
        case VoiceMessageEvent::send:
            f.held = f.hold_inside = false;
            f.phase = VoiceMessagePhase::sending;
            return VoiceMessageAction::finish_and_send;
        case VoiceMessageEvent::stop:
        case VoiceMessageEvent::deactivated:
        case VoiceMessageEvent::limit_reached:
            f.held = f.hold_inside = false;
            f.phase = VoiceMessagePhase::review;
            f.notice = event == VoiceMessageEvent::deactivated     ? VoiceMessageNotice::inactive
                       : event == VoiceMessageEvent::limit_reached ? VoiceMessageNotice::limit
                                                                   : VoiceMessageNotice::none;
            return VoiceMessageAction::finish;
        case VoiceMessageEvent::capture_failed:
            reset(VoiceMessageNotice::microphone);
            return VoiceMessageAction::discard;
        default:
            return VoiceMessageAction::none;
        }
    case VoiceMessagePhase::review:
        switch (event) {
        case VoiceMessageEvent::send:
            f.phase = VoiceMessagePhase::sending;
            f.notice = VoiceMessageNotice::none;
            return VoiceMessageAction::send;
        case VoiceMessageEvent::play:
            return VoiceMessageAction::toggle_preview;
        default:
            return VoiceMessageAction::none;
        }
    case VoiceMessagePhase::sending:
        switch (event) {
        case VoiceMessageEvent::upload_ready:
            return VoiceMessageAction::queue;
        case VoiceMessageEvent::queued:
            reset(VoiceMessageNotice::none);
            return VoiceMessageAction::release;
        case VoiceMessageEvent::upload_failed:
            f.phase = VoiceMessagePhase::failed;
            f.notice = VoiceMessageNotice::upload;
            return VoiceMessageAction::none;
        default:
            return VoiceMessageAction::none;
        }
    case VoiceMessagePhase::failed:
        switch (event) {
        case VoiceMessageEvent::retry:
        case VoiceMessageEvent::send:
            f.phase = VoiceMessagePhase::sending;
            f.notice = VoiceMessageNotice::none;
            return VoiceMessageAction::retry;
        case VoiceMessageEvent::play:
            return VoiceMessageAction::toggle_preview;
        default:
            return VoiceMessageAction::none;
        }
    }
    return VoiceMessageAction::none;
}
} // namespace maicord
