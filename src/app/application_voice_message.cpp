#include "application_account_presentation.hpp"
#include "application_attachments.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_composer_text_input.hpp"
#include "application_message_feedback.hpp"
#include "application_message_history.hpp"
#include "application_reply_draft.hpp"
#include "application_state.hpp"
#include "application_thread_actions.hpp"

#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/voice_message.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
static_assert(kUiComposerVoiceLevels == kVoiceMessageLiveLevels &&
              kUiComposerVoiceWaveform == kVoiceMessageWaveformSamples);
constexpr u64 kNoticeMs = 6'000;
constexpr u32 kRecordingPollMs = 50, kIdlePollMs = 100;

// The finished stream, copied out of the module: the upload's memory source.
struct UploadBuffer {
    volatile LONG references;
    u32 bytes;
    u8* data() noexcept {
        return reinterpret_cast<u8*>(this + 1);
    }
    const u8* data() const noexcept {
        return reinterpret_cast<const u8*>(this + 1);
    }
};
void buffer_retain(void* context) noexcept {
    InterlockedIncrement(&static_cast<UploadBuffer*>(context)->references);
}
void buffer_release(void* context) noexcept {
    auto* buffer = static_cast<UploadBuffer*>(context);
    if (InterlockedDecrement(&buffer->references) == 0) {
        secure_clear_bytes(buffer->data(), buffer->bytes);
        VirtualFree(buffer, 0, MEM_RELEASE);
    }
}
ConstSlice<u8> buffer_read(void* context, u64 offset, Slice<u8> scratch) noexcept {
    const auto* buffer = static_cast<const UploadBuffer*>(context);
    if (offset >= buffer->bytes || !scratch.count)
        return {};
    // The finished recording stays immutable until its last source release.
    if (!scratch.data && scratch.count > buffer->bytes - offset)
        return {};
    return {buffer->data() + offset, static_cast<u32>(minimum<u64>(buffer->bytes - offset, scratch.count))};
}
constexpr UploadSourceOps kBufferOps{buffer_retain, buffer_release, buffer_read};

struct VoiceState {
    VoiceMessageFlow flow;
    VoiceMessageStatus status;
    UiComposerVoiceView view;
    const void* instance; // Module instance the recording belongs to.
    u64 draft_id;
    u64 next_poll_ms;
    u64 notice_until_ms;
    VoiceMessageNotice notice; // After the flow ended (shown in the status row).
    char text[160];
    u32 text_bytes;
};
VoiceState g_voice;

bool module_ready() noexcept {
    return g_app.account_ui && g_app.account.instance && g_app.account.api.voice_message;
}
bool command(VoiceMessageOperation operation, Slice<u8> output = {}) noexcept {
    if (!module_ready())
        return false;
    VoiceMessageCommand c{};
    c.operation = operation;
    c.output = output;
    return g_app.account.api.voice_message(g_app.account.instance, &c, &g_voice.status);
}
Utf8View notice_text(VoiceMessageNotice notice) noexcept {
    switch (notice) {
    case VoiceMessageNotice::inactive:
        return utf8_literal("Voice message recording stopped because MaiCord wasn\xE2\x80\x99t active.");
    case VoiceMessageNotice::limit:
        return utf8_literal("Limit reached!");
    case VoiceMessageNotice::microphone:
        return utf8_literal("Failed to start recording. Check your microphone.");
    case VoiceMessageNotice::microphone_access:
        return utf8_literal("Microphone access is blocked. Allow it in Windows privacy settings.");
    case VoiceMessageNotice::upload:
        return utf8_literal("Upload failed. Press Enter to retry.");
    case VoiceMessageNotice::failed_start:
        return utf8_literal("Failed to start recording");
    default:
        return {};
    }
}
void end_notice(VoiceMessageNotice notice) noexcept {
    g_voice.notice = notice;
    g_voice.notice_until_ms = notice != VoiceMessageNotice::none ? clock_monotonic_milliseconds() + kNoticeMs : 0;
}
void remove_draft() noexcept {
    if (g_voice.draft_id)
        application_attachment_voice_remove(g_voice.draft_id);
    g_voice.draft_id = 0;
}
// Copies the finished recording into an upload buffer and adds its draft.
bool begin_upload() noexcept {
    command(VoiceMessageOperation::status);
    const auto& s = g_voice.status;
    if (s.state != VoiceMessageState::finished || !s.bytes || !s.duration_ms)
        return false;
    const usize bytes = sizeof(UploadBuffer) + s.bytes;
    auto* buffer = static_cast<UploadBuffer*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!buffer)
        return false;
    buffer->references = 1;
    buffer->bytes = s.bytes;
    if (!command(VoiceMessageOperation::copy, {buffer->data(), s.bytes})) {
        buffer_release(buffer);
        return false;
    }
    char waveform[kVoiceMessageWaveformBase64Bytes];
    u8 samples[kVoiceMessageWaveformSamples];
    copy_bytes(samples, g_voice.status.waveform, sizeof(samples));
    const u32 waveform_bytes = voice_message_waveform_base64(samples, {waveform, sizeof(waveform)});
    const UploadSource source{buffer, &kBufferOps, s.bytes};
    u64 id = 0;
    const bool added = waveform_bytes && application_attachment_add_voice(g_voice.flow.channel_id, source,
                                                                          {waveform, waveform_bytes},
                                                                          g_voice.status.duration_ms, &id);
    buffer_release(buffer); // The draft holds its own reference.
    if (!added)
        return false;
    g_voice.draft_id = id;
    pump_message_history();
    return true;
}
// Queues the IS_VOICE_MESSAGE send once the upload is ready.
bool queue_send() noexcept {
    auto& ui = *g_app.account_ui;
    const u64 channel = g_voice.flow.channel_id;
    UploadedAttachment reference{};
    u64 size = 0;
    if (channel != ui.active_channel_id || g_app.message_composer.channel_id != channel ||
        g_app.message_composer.bytes || !application_thread_actions_channel_sendable(channel) ||
        !application_attachment_voice_reference(g_voice.draft_id, &reference, &size))
        return false;
    const auto reply = application_reply_draft();
    if (reply.message_id && !application_channel_readable(g_app, reply.channel_id))
        return false;
    const auto queued = message_composer_controller_queue_send(
        &ui.message_composer_controller, g_app.message_composer_scene.channel_allows_send, ui.chat_state.self_user_id,
        clock_unix_milliseconds(), clock_monotonic_milliseconds(), reply.message_id, {&reference, 1}, {&size, 1},
        nullptr, {}, reply.mention_reply_author, &reply);
    if (!queued.ok()) {
        log_u64(LogLevel::error, "voice_message", "queue_error", static_cast<u64>(queued.error));
        return false;
    }
    const u64 id = g_voice.draft_id;
    application_attachment_did_queue({&id, 1}, queued.nonce);
    g_voice.draft_id = 0; // The send owns the draft now; completion removes it.
    return true;
}
// The recording ends without a message: the module discards it and the bar leaves with `notice`.
void abort_recording(VoiceMessageNotice notice) noexcept {
    command(VoiceMessageOperation::discard);
    reset_value(&g_voice.flow);
    end_notice(notice);
}
void apply(VoiceMessageEvent event, u64 channel = 0) noexcept;
void perform(VoiceMessageAction action) noexcept {
    switch (action) {
    case VoiceMessageAction::start:
        // The recording bar replaces the editor: release its focus and any
        // text service composition so nothing types into the hidden draft.
        if (g_app.message_composer.focused)
            release_message_composer_focus();
        application_composer_text_input_suspend();
        g_voice.instance = g_app.account.instance;
        g_voice.notice = VoiceMessageNotice::none;
        if (!command(VoiceMessageOperation::start) || g_voice.status.state == VoiceMessageState::failed)
            abort_recording(g_voice.status.failure == VoiceMessageFailure::microphone_access
                                ? VoiceMessageNotice::microphone_access
                                : VoiceMessageNotice::failed_start);
        g_voice.next_poll_ms = 0;
        break;
    case VoiceMessageAction::discard:
        command(VoiceMessageOperation::discard);
        remove_draft();
        break;
    case VoiceMessageAction::finish:
    case VoiceMessageAction::finish_and_send:
        command(VoiceMessageOperation::finish);
        if (g_voice.status.state != VoiceMessageState::finished) {
            abort_recording(VoiceMessageNotice::microphone);
            break;
        }
        if (action == VoiceMessageAction::finish)
            break;
        [[fallthrough]];
    case VoiceMessageAction::send:
        command(VoiceMessageOperation::pause);
        if (!begin_upload())
            apply(VoiceMessageEvent::upload_failed);
        break;
    case VoiceMessageAction::toggle_preview:
        command(VoiceMessageOperation::status);
        command(g_voice.status.playing ? VoiceMessageOperation::pause : VoiceMessageOperation::play);
        g_voice.next_poll_ms = 0;
        break;
    case VoiceMessageAction::retry:
        if (g_voice.draft_id ? !application_attachment_voice_retry(g_voice.draft_id) : !begin_upload())
            apply(VoiceMessageEvent::upload_failed);
        else
            pump_message_history();
        break;
    case VoiceMessageAction::queue:
        if (queue_send())
            apply(VoiceMessageEvent::queued);
        else
            apply(VoiceMessageEvent::upload_failed);
        break;
    case VoiceMessageAction::release:
        command(VoiceMessageOperation::discard);
        pump_message_history();
        break;
    case VoiceMessageAction::none:
        break;
    }
}
void apply(VoiceMessageEvent event, u64 channel) noexcept {
    perform(voice_message_flow_apply(&g_voice.flow, event, clock_monotonic_milliseconds(), channel));
}
// Fills the bar from the latest module status.
void project(u64 now_ms) noexcept {
    auto& v = g_voice.view;
    const auto& f = g_voice.flow;
    const auto& s = g_voice.status;
    v.mode = f.phase == VoiceMessagePhase::recording ? UiComposerVoiceMode::recording
             : f.phase == VoiceMessagePhase::review  ? UiComposerVoiceMode::review
             : f.phase == VoiceMessagePhase::sending ? UiComposerVoiceMode::sending
                                                     : UiComposerVoiceMode::failed;
    v.playing = s.playing;
    v.held = f.held;
    v.hold_inside = f.hold_inside;
    v.pulse = (now_ms / 500) % 2 == 0;
    v.elapsed_ms = s.duration_ms;
    v.position_ms = s.playing || s.position_ms < s.duration_ms ? s.position_ms : 0;
    v.level_count = minimum(s.level_count, kVoiceMessageLiveLevels);
    copy_bytes(v.levels, s.levels, sizeof(v.levels));
    copy_bytes(v.waveform, s.waveform, sizeof(v.waveform));
    if (g_voice.draft_id) {
        MessageAttachmentDraftPhase phase{};
        u64 uploaded = 0, total = 0;
        if (application_attachment_voice_progress(g_voice.draft_id, &phase, &uploaded, &total) && total)
            v.progress = static_cast<float>(static_cast<double>(uploaded) / static_cast<double>(total));
    } else {
        v.progress = 0;
    }
    Utf8View status{};
    v.error = false;
    if (f.phase == VoiceMessagePhase::recording) {
        status = f.held ? (f.hold_inside ? (s.limit_reached ? utf8_literal("Limit reached! Release to Send")
                                                            : utf8_literal("Release to Send"))
                                         : (s.limit_reached ? utf8_literal("Limit reached! Release to Cancel")
                                                            : utf8_literal("Release to Cancel")))
                        : !s.capturing ? utf8_literal("Starting microphone\xE2\x80\xA6")
                                       : utf8_literal("Esc to delete \xC2\xB7 Enter to send");
    } else if (f.phase == VoiceMessagePhase::sending) {
        status = utf8_literal("Sending\xE2\x80\xA6");
    } else if (f.notice != VoiceMessageNotice::none) {
        status = notice_text(f.notice);
        v.error = f.notice == VoiceMessageNotice::upload;
    } else {
        status = utf8_literal("Enter to send");
    }
    g_voice.text_bytes = minimum<u32>(status.bytes, sizeof(g_voice.text));
    copy_bytes(g_voice.text, status.data, g_voice.text_bytes);
    v.status = {g_voice.text, g_voice.text_bytes};
}
bool stale() noexcept {
    return voice_message_flow_active(g_voice.flow) &&
           (!module_ready() || g_voice.instance != g_app.account.instance);
}
} // namespace

const UiComposerVoiceView* application_voice_message_view(u32 channel_actions, bool sending_restricted) noexcept {
    if (stale()) {
        remove_draft();
        reset_value(&g_voice.flow);
    }
    if (!module_ready())
        return nullptr;
    const u64 now = clock_monotonic_milliseconds();
    if (voice_message_flow_active(g_voice.flow)) {
        project(now);
        return &g_voice.view;
    }
    const u64 channel = g_app.account_ui->active_channel_id;
    const VoiceMessageGate gate{channel_actions,
                                g_app.message_composer.bytes == 0,
                                application_attachment_channel_pending(channel),
                                g_app.message_edit.editor_active,
                                sending_restricted,
                                application_composer_slowmode_active(channel)};
    if (!channel || !voice_message_available(gate))
        return nullptr;
    reset_value(&g_voice.view);
    g_voice.view.mode = UiComposerVoiceMode::available;
    return &g_voice.view;
}

void application_voice_message_pointer(UiButtonPointerEvent event, bool over_send, bool activated) noexcept {
    auto& f = g_voice.flow;
    if (event == UiButtonPointerEvent::press && over_send && !voice_message_flow_active(f) &&
        message_composer_scene_voice_mode(g_app.message_composer_scene) == UiComposerVoiceMode::available) {
        apply(VoiceMessageEvent::press, g_app.account_ui ? g_app.account_ui->active_channel_id : 0);
        refresh_message_composer_ui();
        return;
    }
    if (!f.held)
        return;
    if (event == UiButtonPointerEvent::move && f.hold_inside != over_send) {
        f.hold_inside = over_send;
        refresh_message_composer_ui();
    } else if ((event == UiButtonPointerEvent::release || event == UiButtonPointerEvent::cancel) && !activated) {
        apply(VoiceMessageEvent::release_outside);
        refresh_message_composer_ui();
    }
}

bool application_voice_message_activate(MessageComposerControl control) noexcept {
    const auto mode = message_composer_scene_voice_mode(g_app.message_composer_scene);
    auto& f = g_voice.flow;
    if (control == MessageComposerControl::send) {
        if (f.phase == VoiceMessagePhase::recording && f.held)
            apply(VoiceMessageEvent::release_inside);
        else if (!voice_message_flow_active(f) && mode == UiComposerVoiceMode::available)
            apply(VoiceMessageEvent::toggle, g_app.account_ui ? g_app.account_ui->active_channel_id : 0);
        else if (voice_message_flow_active(f))
            apply(f.phase == VoiceMessagePhase::failed ? VoiceMessageEvent::retry : VoiceMessageEvent::send);
        else
            return false;
        return refresh_message_composer_ui();
    }
    if (!voice_message_flow_active(f) || !ui_composer_voice_bar_visible(mode))
        return false;
    if (control == MessageComposerControl::add_attachment)
        apply(VoiceMessageEvent::cancel);
    else if (control == MessageComposerControl::emoji)
        apply(f.phase == VoiceMessagePhase::recording ? VoiceMessageEvent::stop : VoiceMessageEvent::play);
    else
        return false;
    return refresh_message_composer_ui();
}

bool application_voice_message_bar_click() noexcept {
    if (!voice_message_flow_active(g_voice.flow) ||
        !ui_composer_voice_bar_visible(message_composer_scene_voice_mode(g_app.message_composer_scene)))
        return false;
    if (g_voice.flow.phase == VoiceMessagePhase::review || g_voice.flow.phase == VoiceMessagePhase::failed)
        apply(VoiceMessageEvent::play);
    refresh_message_composer_ui();
    return true;
}

bool application_voice_message_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if (!voice_message_flow_active(g_voice.flow))
        return false;
    // The recording replaces the draft: nothing types into it meanwhile.
    if (kind == UiInputKind::character)
        return true;
    if (kind != UiInputKind::key_down || repeated)
        return kind == UiInputKind::key_down && (code == VK_ESCAPE || code == VK_RETURN);
    const bool modifiers = (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000);
    if (code == VK_ESCAPE) {
        apply(VoiceMessageEvent::cancel);
        return refresh_message_composer_ui();
    }
    if (code == VK_RETURN && !modifiers) {
        apply(g_voice.flow.phase == VoiceMessagePhase::failed ? VoiceMessageEvent::retry : VoiceMessageEvent::send);
        return refresh_message_composer_ui();
    }
    return false;
}

void application_voice_message_channel_changed(u64 channel_id) noexcept {
    if (voice_message_flow_active(g_voice.flow) && g_voice.flow.channel_id != channel_id)
        apply(VoiceMessageEvent::channel_changed);
}

void application_voice_message_deactivated() noexcept {
    if (g_voice.flow.phase == VoiceMessagePhase::recording) {
        apply(VoiceMessageEvent::deactivated);
        refresh_message_composer_ui();
    }
}

bool application_voice_message_pump() noexcept {
    if (stale()) {
        remove_draft();
        reset_value(&g_voice.flow);
        return refresh_message_composer_ui();
    }
    const u64 now = clock_monotonic_milliseconds();
    if (g_voice.notice != VoiceMessageNotice::none && now >= g_voice.notice_until_ms) {
        g_voice.notice = VoiceMessageNotice::none;
        refresh_account_frame(true);
    }
    if (!voice_message_flow_active(g_voice.flow) || now < g_voice.next_poll_ms)
        return true;
    const auto phase = g_voice.flow.phase;
    g_voice.next_poll_ms = now + (phase == VoiceMessagePhase::recording ? kRecordingPollMs : kIdlePollMs);
    command(VoiceMessageOperation::status);
    const auto& s = g_voice.status;
    if (phase == VoiceMessagePhase::recording) {
        if (s.state == VoiceMessageState::failed) {
            const auto failure = s.failure;
            apply(VoiceMessageEvent::capture_failed);
            end_notice(failure == VoiceMessageFailure::microphone_access ? VoiceMessageNotice::microphone_access
                                                                         : VoiceMessageNotice::microphone);
            g_voice.flow.notice = VoiceMessageNotice::none;
        } else if (s.limit_reached && !g_voice.flow.held) {
            apply(VoiceMessageEvent::limit_reached);
        }
    } else if (phase == VoiceMessagePhase::sending) {
        MessageAttachmentDraftPhase draft{};
        u64 uploaded = 0, total = 0;
        if (!g_voice.draft_id || !application_attachment_voice_progress(g_voice.draft_id, &draft, &uploaded, &total))
            apply(VoiceMessageEvent::upload_failed);
        else if (draft == MessageAttachmentDraftPhase::ready)
            apply(VoiceMessageEvent::upload_ready);
        else if (draft == MessageAttachmentDraftPhase::failed)
            apply(VoiceMessageEvent::upload_failed);
    }
    return refresh_account_frame(true);
}

u32 application_voice_message_wait_timeout(u64 now_ms) noexcept {
    u32 timeout = INFINITE;
    if (g_voice.notice != VoiceMessageNotice::none)
        timeout = g_voice.notice_until_ms > now_ms ? static_cast<u32>(minimum<u64>(g_voice.notice_until_ms - now_ms,
                                                                                 INFINITE - 1))
                                                   : 0;
    if (voice_message_flow_active(g_voice.flow)) {
        const bool polling = g_voice.flow.phase == VoiceMessagePhase::recording ||
                             g_voice.flow.phase == VoiceMessagePhase::sending || g_voice.status.playing;
        if (polling)
            timeout = minimum(timeout, g_voice.next_poll_ms > now_ms
                                           ? static_cast<u32>(minimum<u64>(g_voice.next_poll_ms - now_ms, 1000))
                                           : 0u);
    }
    return timeout;
}

Utf8View application_voice_message_notice(bool* error) noexcept {
    if (error)
        *error = g_voice.notice != VoiceMessageNotice::none && g_voice.notice != VoiceMessageNotice::inactive;
    return voice_message_flow_active(g_voice.flow) ? Utf8View{} : notice_text(g_voice.notice);
}

void application_voice_message_release() noexcept {
    if (voice_message_flow_active(g_voice.flow) && module_ready() && g_voice.instance == g_app.account.instance)
        command(VoiceMessageOperation::discard);
    remove_draft();
    reset_value(&g_voice);
}
} // namespace maicord::app_detail
