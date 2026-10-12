#include "application_activity.hpp"
#include "application_guild_banner.hpp"
#include "application_invite_cards.hpp"
#include "application_message_components.hpp"
#include "application_action_captcha.hpp"
#include "application_emoji_preferences.hpp"
#include "application_downloads.hpp"
#include "application_message_feedback.hpp"
#include "application_commands.hpp"
#include "application_friends.hpp"
#include "application_scheduler.hpp"
#include "application_stream.hpp"
#include "application_stage.hpp"
#include "application_soundboard.hpp"
#include "application_account_text_retry.hpp"
#include "application_profile.hpp"
#include "application_read_state.hpp"
#include "application_scoped_read.hpp"
#include "application_notifications.hpp"
#include "application_video.hpp"
#include "application_tooltip.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_media_picker.hpp"
#include "application_quick_switcher.hpp"
#include "application_message_forward.hpp"
#include "application_message_reaction.hpp"
#include "application_self_status.hpp"
#include "application_surface_motion.hpp"
#include "application_channel_overview.hpp"
#include "application_channel_snapshots.hpp"
#include "application_background.hpp"
#include "application_thread_actions.hpp"
#include "application_thread_browser.hpp"
#include "application_thread_creation.hpp"
#include "application_forum_post.hpp"
#include "application_forum_form.hpp"
#include "application_forum_preview.hpp"
#include "application_composer.hpp"
#include "application_composer_preview.hpp"
#include "application_poll.hpp"
#include "application_member_moderation.hpp"
#include "application_user_settings.hpp"
#include "application_guild_folders.hpp"
#include "application_account_settings.hpp"
#include "application_notification_controls.hpp"
#include "application_custom_status.hpp"
#include "application_inbox.hpp"
#include "application_frame_presentation.hpp"

#include "application_account_presentation.hpp"
#include "application_account_module.hpp"
#include "application_account_pump.hpp"
#include "application_account_logout.hpp"
#include "application_login_auth.hpp"
#include "application_captcha_auth.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_voice.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail {
namespace {

u64 application_next_wake_at_ms() noexcept {
    if (application_account_logout_blocked())
        return application_account_logout_next_wake();
    u64 wake_at = earliest_deadline(application_profile_next_wake(), application_commands_next_wake());
    wake_at = earliest_deadline(wake_at, application_friends_next_wake());
    wake_at = earliest_deadline(wake_at, application_forum_preview_next_wake());
    wake_at = earliest_deadline(wake_at, application_composer_preview_next_wake());
    wake_at = earliest_deadline(wake_at, composer_spellcheck_next_wake(g_app.composer_spellcheck));
    wake_at = earliest_deadline(wake_at, application_search_next_wake());
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake_at = earliest_deadline(wake_at, application_pinned_messages_next_wake());
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake_at = earliest_deadline(wake_at, application_thread_browser_next_wake());
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake_at = earliest_deadline(wake_at, application_thread_creation_next_wake());
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake_at = earliest_deadline(wake_at, application_forum_post_next_wake());
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake_at = earliest_deadline(wake_at, application_poll_next_wake());
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake_at = earliest_deadline(wake_at, application_member_moderation_next_wake());
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake_at = earliest_deadline(wake_at, application_account_settings_next_wake());
    wake_at = earliest_deadline(wake_at, application_media_picker_next_wake());
    wake_at = earliest_deadline(wake_at, application_gif_favorites_next_wake());
    wake_at = earliest_deadline(wake_at, application_emoji_preferences_next_wake());
    wake_at = earliest_deadline(wake_at, application_action_captcha_next_wake());
    wake_at = earliest_deadline(wake_at, application_self_status_next_wake());
    wake_at = earliest_deadline(wake_at, application_user_settings_next_wake());
    wake_at = earliest_deadline(wake_at, application_guild_folders_request_wake());
    wake_at = earliest_deadline(wake_at, application_custom_status_next_wake());
    wake_at = earliest_deadline(wake_at, application_activity_next_wake());
    wake_at = earliest_deadline(wake_at, application_notification_controls_next_wake());
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake_at = earliest_deadline(wake_at, application_inbox_next_wake());
    if (g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake_at = earliest_deadline(wake_at, application_stream_preview_next_wake());
    wake_at = earliest_deadline(
        wake_at, application_stage_next_wake(g_app.details_request_owner == MessageHistoryRequestOwner::none));
    wake_at = earliest_deadline(
        wake_at, application_soundboard_next_wake(g_app.details_request_owner == MessageHistoryRequestOwner::none));
    wake_at = earliest_deadline(wake_at, application_message_components_next_wake());
    wake_at = earliest_deadline(wake_at, application_invite_cards_next_wake());
    wake_at = earliest_deadline(wake_at, application_channel_overview_next_wake());
    wake_at = earliest_deadline(wake_at, application_read_state_next_wake());
    wake_at = earliest_deadline(wake_at, application_scoped_read_next_wake());
    wake_at = earliest_deadline(wake_at, application_notifications_next_wake(&g_app));
    wake_at = earliest_deadline(wake_at, application_voice_next_wake());
    wake_at = earliest_deadline(wake_at, application_presence_next_wake());
    if (!application_thread_browser_read_blocked())
        wake_at = earliest_deadline(wake_at, media_url_next_wake(g_app.media_urls));
    if (g_app.account_ui) {
        wake_at = earliest_deadline(wake_at, application_account_text_retry_wake());
        const auto& request = g_app.account_ui->private_conversation;
        if (g_app.message_history_request_owner == MessageHistoryRequestOwner::none) {
            if (request.phase == PrivateConversationPhase::queued)
                wake_at = earliest_deadline(wake_at, 1);
            if (request.phase == PrivateConversationPhase::retry_wait)
                wake_at = earliest_deadline(wake_at, request.retry_at_ms);
        }
        wake_at = earliest_deadline(wake_at, g_app.account_ui->chat_state.restore_deadline_ms);
        wake_at = earliest_deadline(wake_at, g_app.account_ui->message_draft_session.persist_at_ms);
        wake_at = earliest_deadline(wake_at, message_composer_controller_next_wake_at(
                                                 g_app.account_ui->message_composer_controller));
        wake_at =
            earliest_deadline(wake_at, typing_store_next_wake_at(g_app.account_ui->typing_store));
    }
    if (g_app.message_catchup.state == MessageHistoryCatchupState::waiting_retry) {
        wake_at = earliest_deadline(wake_at, g_app.message_catchup.retry_at_ms);
    }
    if (g_app.message_older.state == MessageHistoryOlderState::waiting_retry) {
        wake_at = earliest_deadline(wake_at, g_app.message_older.retry_at_ms);
    }
    if (g_app.attachment_refresh.state == AttachmentRefreshState::waiting_retry) {
        wake_at = earliest_deadline(wake_at, g_app.attachment_refresh.retry_at_ms);
    }
    if (g_app.reply_refresh.state == AttachmentRefreshState::waiting_retry)
        wake_at = earliest_deadline(wake_at, g_app.reply_refresh.retry_at_ms);
    if (g_app.message_delete.message_id != 0) {
        wake_at = earliest_deadline(wake_at, g_app.message_delete.retry_at_ms);
    }
    if (g_app.message_edit.message_id != 0 &&
        g_app.message_edit.phase == MessageEditRequestPhase::retry_wait) {
        wake_at = earliest_deadline(wake_at, g_app.message_edit.retry_at_ms);
    }
    wake_at = earliest_deadline(wake_at, application_message_reaction_next_wake());
    wake_at = earliest_deadline(wake_at, application_thread_actions_next_wake());
    wake_at = earliest_deadline(wake_at, application_channel_prefetch_next_wake());
    wake_at = earliest_deadline(wake_at, g_app.member_list_subscription.retry_at_ms);
    wake_at = earliest_deadline(wake_at, application_author_hydration_next_wake());
    wake_at = earliest_deadline(wake_at, application_mention_member_query_next_wake());
    wake_at = earliest_deadline(wake_at, g_app.thread_member_list_subscription.retry_at_ms);
    wake_at = earliest_deadline(wake_at,
                                reaction_viewer_controller_next_wake_at(g_app.reaction_viewer_controller));
    return application_thread_browser_read_blocked()
               ? wake_at
               : earliest_deadline(wake_at, g_app.message_media.retry_at_ms);
}

} // namespace

u32 application_wait_timeout(u64 now_ms, u64 now_us, bool frame_clock_armed, bool visual_suspended) noexcept {
    frame_clock_armed = frame_clock_armed || visual_suspended;
    if (application_account_logout_blocked()) {
        u32 timeout = INFINITE;
        const u64 operation_wakes[] = {application_account_logout_next_wake(),
                                       application_text_form_next_wake(),
                                       application_downloads_next_wake()};
        for (const u64 wake : operation_wakes) {
            if (wake)
                timeout = minimum(timeout, wait_milliseconds_until(now_ms, wake));
        }
        if (!frame_clock_armed) {
            const u64 visual_wakes[] = {application_background_next_wake(now_ms),
                                        application_surface_motion_next_wake(now_ms)};
            for (const u64 wake : visual_wakes) {
                if (wake)
                    timeout = minimum(timeout, wait_milliseconds_until(now_ms, wake));
            }
            if (application_effect_frame_due(now_ms))
                timeout = 0;
            else if (g_app.renderer.text_effect_animated)
                timeout =
                    minimum(timeout, wait_milliseconds_until(now_ms, g_app.renderer.text_effect_next_ms));
            if (g_app.frame_pending)
                timeout = 0;
        }
        if (g_app.frame_rate.next_update_us) {
            const u64 remaining =
                g_app.frame_rate.next_update_us > now_us ? g_app.frame_rate.next_update_us - now_us : 0;
            timeout = minimum(timeout, static_cast<u32>((remaining + 999) / 1'000));
        }
        return timeout;
    }
    if (g_app.account_ui) {
        AccountUiRuntime& ui = *g_app.account_ui;
        message_draft_session_observe(ui.message_draft_session, ui.message_drafts, now_ms);
    }
    u32 timeout = account_host_wait_timeout(&g_app.account, now_ms);
    const u64 browser_deadline = application_login_browser_deadline();
    if (browser_deadline)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, browser_deadline));
    // One wake per media upload burst: the idle atlas/frame-cache trim.
    if (const u64 media_trim = renderer_media_next_trim_ms(g_app.renderer))
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, media_trim));
    const u64 account_wake_at = application_next_wake_at_ms();
    if (account_wake_at != 0) {
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, account_wake_at));
    }
    const MessageTextPresentation* text = g_app.account_ui ? &g_app.account_ui->message_text : nullptr;
    if (text && !visual_suspended && text->next_timestamp_update_unix_seconds != 0 &&
        application_account_timestamp_refresh_eligible(now_ms)) {
        const u64 now_seconds = clock_unix_milliseconds() / 1'000;
        const u64 next = text->next_timestamp_update_unix_seconds;
        const u64 wait = next <= now_seconds ? 0 : minimum<u64>((next - now_seconds) * 1'000, INFINITE - 1);
        timeout = minimum(timeout, static_cast<u32>(wait));
    }
    if (g_app.message_media.scan_pending && !application_thread_browser_read_blocked()) {
        timeout = 0;
    }
    if (!frame_clock_armed) {
        const u64 background_wake = application_background_next_wake(now_ms);
        if (background_wake)
            timeout = minimum(timeout, wait_milliseconds_until(now_ms, background_wake));
        const u64 video_wake = application_video_next_wake();
        if (video_wake)
            timeout = minimum(timeout, wait_milliseconds_until(now_ms, video_wake));
        const u64 tooltip_wake = application_tooltip_next_wake();
        if (tooltip_wake)
            timeout = minimum(timeout, wait_milliseconds_until(now_ms, tooltip_wake));
        const u64 banner_wake = application_guild_banner_next_wake();
        if (banner_wake)
            timeout = minimum(timeout, wait_milliseconds_until(now_ms, banner_wake));
        const u64 surface_wake = application_surface_motion_next_wake(now_ms);
        if (surface_wake)
            timeout = minimum(timeout, wait_milliseconds_until(now_ms, surface_wake));
        const u64 spinner_wake = application_loading_spinner_wake();
        if (spinner_wake)
            timeout = minimum(timeout, wait_milliseconds_until(now_ms, spinner_wake));
        // Once a visual frame is due, DXGI paces it. An overdue visual timer
        // must not turn the display-slot wait into a zero-timeout polling loop.
        if (g_app.renderer.text_effect_animated ||
            profile_scene_animating(g_app.renderer.profile_scene, now_ms))
            timeout = minimum(timeout, wait_milliseconds_until(now_ms, g_app.renderer.text_effect_next_ms));
        timeout = minimum(timeout, animation_timeline_wait_timeout_ms(&g_app.animations.timeline, now_us));
    }
    if (g_app.frame_rate.next_update_us != 0) {
        const u64 remaining =
            g_app.frame_rate.next_update_us > now_us ? g_app.frame_rate.next_update_us - now_us : 0;
        timeout = minimum(timeout, static_cast<u32>((remaining + 999) / 1'000));
    }
    if (g_app.login.remote_restart_at_ms != 0) {
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, g_app.login.remote_restart_at_ms));
    }
    const u64 recovery_wake = application_login_recovery_deadline();
    if (recovery_wake)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, recovery_wake));
    const u64 phone_wake = application_login_phone_deadline();
    if (phone_wake)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, phone_wake));
    const u64 reset_wake = application_login_reset_deadline();
    if (reset_wake)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, reset_wake));
    const u64 captcha_wake = application_login_captcha_deadline();
    if (captcha_wake)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, captcha_wake));
    if (g_app.login.status == RendererLoginStatus::mfa_backup_acknowledgement &&
        g_app.login.backup_acknowledgement_at_ms != 0) {
        const u64 remaining = g_app.login.backup_acknowledgement_at_ms <= now_ms
                                  ? 0
                                  : g_app.login.backup_acknowledgement_at_ms - now_ms;
        const u64 next_second = remaining == 0 ? 0 : remaining % 1'000 == 0 ? 1'000 : remaining % 1'000;
        timeout = minimum(timeout, next_second >= INFINITE ? INFINITE - 1 : static_cast<u32>(next_second));
    }
    if (g_app.login.caret_blink_at_ms != 0) {
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, g_app.login.caret_blink_at_ms));
    }
    if (g_app.message_composer_caret_blink_at_ms != 0) {
        timeout =
            minimum(timeout, wait_milliseconds_until(now_ms, g_app.message_composer_caret_blink_at_ms));
    }
    if (application_smooth_scroll_active() && !frame_clock_armed) {
        timeout = minimum(timeout, 16u);
    }
    const u64 form_wake = application_text_form_next_wake();
    if (form_wake)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, form_wake));
    const u64 interaction_wake = application_message_interaction_next_wake(now_ms);
    if (interaction_wake)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, interaction_wake));
    const u64 forum_form_wake = application_forum_form_next_wake();
    if (forum_form_wake)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, forum_form_wake));
    const u64 search_wake = application_friends_search_next_wake();
    if (search_wake && !frame_clock_armed)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, search_wake));
    const u64 message_search_caret = application_search_edit_next_wake();
    if (message_search_caret && !frame_clock_armed)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, message_search_caret));
    const u64 picker_caret = application_media_picker_caret_wake();
    if (picker_caret && !frame_clock_armed)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, picker_caret));
    const u64 downloads_wake = application_downloads_next_wake();
    if (downloads_wake)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, downloads_wake));
    const u64 forward_caret = application_message_forward_caret_wake();
    if (forward_caret && !frame_clock_armed)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, forward_caret));
    const u64 quick_caret = application_quick_switcher_caret_wake();
    if (quick_caret && !frame_clock_armed)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, quick_caret));
    const u64 overview_caret = application_channel_overview_caret_wake();
    if (overview_caret && !frame_clock_armed)
        timeout = minimum(timeout, wait_milliseconds_until(now_ms, overview_caret));
    // A dirty frame must also run when no DXGI waitable clock is available.
    if (g_app.frame_pending && !frame_clock_armed)
        timeout = 0;
    return timeout;
}

bool application_smooth_scroll_active() noexcept {
    return ui_scroll_host_active(g_app.scroll_host);
}

bool application_animation_frame_due(u64 now_us) noexcept {
    const u64 wake = animation_timeline_next_wake_at_us(&g_app.animations.timeline);
    return wake != 0 && now_us >= wake;
}

bool application_effect_frame_due(u64 now_ms) noexcept {
    // Loading spinners share the procedural-effect frame path: a due step
    // requests one paced frame, which samples the next phase and deadline.
    return (now_ms >= g_app.renderer.text_effect_next_ms &&
            (g_app.renderer.text_effect_animated ||
             profile_scene_animating(g_app.renderer.profile_scene, now_ms))) ||
           application_loading_spinner_due(now_ms);
}

bool application_visual_frame_requested(u64 now_ms, u64 now_us) noexcept {
    if (application_account_logout_blocked())
        return g_app.frame_pending || application_effect_frame_due(now_ms) ||
               deadline_due(now_ms, application_background_next_wake(now_ms)) ||
               deadline_due(now_ms, application_surface_motion_next_wake(now_ms)) ||
               deadline_due(now_ms, application_downloads_next_wake());
    return g_app.frame_pending || application_effect_frame_due(now_ms) ||
           deadline_due(now_ms, application_background_next_wake(now_ms)) ||
           application_smooth_scroll_active() || application_animation_frame_due(now_us) ||
           deadline_due(now_ms, application_video_next_wake()) ||
           deadline_due(now_ms, application_tooltip_next_wake()) ||
           deadline_due(now_ms, application_guild_banner_next_wake()) ||
           deadline_due(now_ms, application_surface_motion_next_wake(now_ms)) ||
           deadline_due(now_ms, application_search_edit_next_wake()) ||
           deadline_due(now_ms, application_media_picker_caret_wake()) ||
           deadline_due(now_ms, application_downloads_next_wake()) ||
           deadline_due(now_ms, application_message_forward_caret_wake()) ||
           deadline_due(now_ms, application_quick_switcher_caret_wake()) ||
           deadline_due(now_ms, application_channel_overview_caret_wake());
}

bool application_nonvisual_wake_due(u64 now_ms) noexcept {
    if (application_account_logout_blocked())
        return deadline_due(now_ms, application_account_logout_next_wake());
    const AccountHost& host = g_app.account;
    return host.instance && !host.stop_failed &&
           ((g_app.message_media.scan_pending && !application_thread_browser_read_blocked()) ||
            account_host_wake_due(&host, now_ms) || deadline_due(now_ms, application_next_wake_at_ms()));
}
u64 application_login_browser_deadline() noexcept {
    if (application_account_logout_blocked())
        return 0;
    const auto& host = g_app.account;
    const auto phase = host.login_status.phase;
    const bool active = phase == AccountLoginPhase::browser_starting ||
                        phase == AccountLoginPhase::browser_waiting ||
                        phase == AccountLoginPhase::browser_exchange_pending;
    return host.login_instance && !host.stop_failed && active ? host.login_status.browser_deadline_ms : 0;
}

bool application_refresh_relative_timestamps_if_due() noexcept {
    if (application_account_logout_blocked())
        return true;
    const MessageTextPresentation* text = g_app.account_ui ? &g_app.account_ui->message_text : nullptr;
    if (!application_account_timestamp_refresh_eligible(clock_monotonic_milliseconds()) || !text ||
        text->next_timestamp_update_unix_seconds == 0 ||
        clock_unix_milliseconds() / 1'000 < text->next_timestamp_update_unix_seconds) {
        return true;
    }
    if (!refresh_account_text(true)) {
        return false;
    }
    application_request_frame();
    return true;
}

} // namespace maicord::app_detail
