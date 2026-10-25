#include "application_load_trace.hpp"
#include "maicord/core/memory.hpp"
#include "application_channel_session.hpp"
#include "application_channel_snapshots.hpp"

#include "application_account_module.hpp"
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_composer_input.hpp"
#include "application_message_edit.hpp"
#include "application_state.hpp"
#include "application_quick_switcher.hpp"
#include "application_video.hpp"
#include "application_voice_message.hpp"

#include "maicord/app/member_list_channel_binding.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_history_sync.hpp"
#include "maicord/app/reaction_viewer_controller.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail {

bool synchronize_selected_channel() noexcept {
    if (!g_app.account_ui) {
        return true;
    }
    AccountUiRuntime& ui = *g_app.account_ui;
    ChatStateStore& state = ui.chat_state;
    const u64 now_ms = clock_monotonic_milliseconds();
    if (ui.observed_selection_revision == state.selection_revision) {
        if (state.selected_channel_id && state.selected_channel_id == g_app.member_list_scene.restore_channel_id)
            g_app.member_list_scene.restore_channel_id = 0;
        return synchronize_member_subscription(now_ms);
    }
    if (ui.active_channel_id == state.selected_channel_id) {
        if (state.selected_channel_id && state.selected_channel_id == g_app.member_list_scene.restore_channel_id)
            g_app.member_list_scene.restore_channel_id = 0;
        ui.observed_selection_revision = state.selection_revision;
        return synchronize_member_subscription(now_ms);
    }
    application_video_close();
    g_app.context_focus = nullptr;
    application_composer_text_input_suspend();
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    if (g_app.reaction_viewer.active && g_app.reaction_viewer.channel_id != state.selected_channel_id) {
        const ReactionViewerControllerResult closed =
            reaction_viewer_controller_close(&g_app.reaction_viewer_controller);
        if (!closed.ok()) {
            log_write(LogLevel::error, "reaction_viewer", "channel_switch_close_failed");
            return false;
        }
    }
    if (g_app.message_edit.editor_active && g_app.message_edit.channel_id != state.selected_channel_id &&
        !cancel_message_edit_editor()) {
        log_write(LogLevel::error, "message", "message_edit_channel_cancel_failed");
        return false;
    }
    if (ui.active_channel_id != 0 && !stop_local_typing(ui.active_channel_id)) {
        log_write(LogLevel::warning, "typing", "channel_switch_stop_failed");
    }
    end_message_composer_pointer_selection();
    application_voice_message_channel_changed(state.selected_channel_id);
    const MessageComposerControllerResult selected = message_composer_controller_select_channel(
        &ui.message_composer_controller, state.selected_channel_id);
    if (!selected.ok()) {
        log_u64(LogLevel::error, "message", "composer_channel_select_error",
                static_cast<u64>(selected.error));
        return false;
    }
    member_list_subscription_complete(&g_app.member_list_subscription);
    guild_member_store_clear_pins(&ui.guild_member_store);
    const u64 restore_channel_id = g_app.member_list_scene.restore_channel_id;
    const bool restoring_members = restore_channel_id &&
        (state.selected_channel_id == restore_channel_id ||
         (!state.selected_channel_id && state.restore_channel_id == restore_channel_id));
    if (!restoring_members) {
        g_app.member_list_scene.scroll_dips = 0;
        g_app.member_list_scene.focused_user_id = 0;
        g_app.member_list_scene.focused_source_index = kUiListNoSelection;
    }
    if (state.selected_channel_id || !state.restore_channel_id)
        g_app.member_list_scene.restore_channel_id = 0;
    application_channel_snapshot_capture(ui.active_channel_id);
    reset_value<MessageHistoryCatchup>(&g_app.message_catchup);
    reset_value<MessageHistoryOlder>(&g_app.message_older);
    reset_value<AttachmentRefresh>(&g_app.attachment_refresh);
    reset_value<ReplyReferenceRefresh>(&g_app.reply_refresh);
    if (!message_timeline_reset(&g_app.message_view.timeline) ||
        !message_timeline_set_scroll(&g_app.message_view.timeline, 0, true)) {
        log_write(LogLevel::error, "app", "selected_channel_timeline_reset_failed");
        return false;
    }
    // A retained window is presented at once; catch-up then fetches only
    // messages after its newest row and merges them.
    const bool restored = application_channel_snapshot_restore(state.selected_channel_id);
    g_app.message_pump.catchup_required = state.selected_channel_id != 0;
    g_app.message_view.timeline.newer_refetch_required = state.selected_channel_id != 0;
    if (state.selected_channel_id != 0 &&
        !message_history_catchup_begin(&g_app.message_catchup, &g_app.message_pump,
                                       state.selected_channel_id, kMessageHistoryRequestLimit) &&
        !restored) {
        log_write(LogLevel::error, "app", "selected_channel_catchup_begin_failed");
        return false;
    }
    // Commit the observation only after every derived owner accepted the
    // transition, so a transient allocation failure remains retryable.
    ui.active_channel_id = state.selected_channel_id;
    application_quick_switcher_note_channel(ui.active_channel_id);
    const u64 epoch = g_app.load_trace.channel_epoch + 1;
    reset_value(&g_app.load_trace);
    g_app.load_trace.channel_epoch = epoch;
    g_app.load_trace.channel_started = clock_now();
    application_trace_load_stage("channel_selected");
    if (restored) {
        g_app.load_trace.history_ready = true;
        application_trace_load_stage("snapshot_restored");
    }
    ui.observed_selection_revision = state.selection_revision;
    log_u64(LogLevel::info, "model", "selected_guild_id", state.selected_guild_id);
    log_u64(LogLevel::info, "model", "selected_channel_id", state.selected_channel_id);
    return synchronize_member_subscription(now_ms);
}

} // namespace maicord::app_detail
