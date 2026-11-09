#include "../account/application_account_reconnect.hpp"
#include "../account/activity.hpp"
#include "maicord/text/layout/visibility.hpp"
#include "../chat/messages/feedback.hpp"
#include "../media/downloads.hpp"
#include "../chat/search/content.hpp"
#include "../chat/inbox.hpp"
#include "maicord/app/account/account_host.hpp"
#include "maicord/app/media/animation_visual_runtime.hpp"
#include "maicord/app/chat/messages/view_runtime.hpp"
#include "maicord/app/shell/background_frame_policy.hpp"
#include "maicord/app/gateway/wake_coalescing.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/os_text.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/text/secure_text_store.hpp"
#include "maicord/platform/desktop/clipboard_attachment.hpp"
#include "maicord/platform/window/pump.hpp"
#include "../account/account_ui_runtime.hpp"

#include "state.hpp"
#include "../guild/channel_overview.hpp"
#include "../guild/settings/settings.hpp"
#include "../update/updates.hpp"
#include "../plugins/catalog_service.hpp"
#include "../account/friends.hpp"
#include "../chat/composer/text_form.hpp"
#include "../chat/threads/forum_form.hpp"
#include "../chat/composer/text_input.hpp"
#include "../chat/composer/attachments.hpp"
#include "../chat/threads/forum_attachments.hpp"
#include "../chat/threads/forum_preview.hpp"
#include "../chat/composer/preview.hpp"
#include "../account/application_account_module.hpp"
#include "../account/application_account_logout.hpp"
#include "../account/application_account_switch.hpp"
#include "../account/account_presentation.hpp"
#include "../account/account_pump.hpp"
#include "content_input.hpp"
#include "frame_presentation.hpp"
#include "../chat/composer/composer.hpp"
#include "../account/login/auth.hpp"
#include "../account/login/captcha_auth.hpp"
#include "../account/login/input.hpp"
#include "../account/login/ui.hpp"
#include "scheduler.hpp"
#include "../voice/call.hpp"
#include "shutdown_metrics.hpp"
#include "../settings/settings.hpp"
#include "../settings/custom_theme.hpp"
#include "../settings/streamer_mode.hpp"
#include "../settings/account_settings.hpp"
#include "../plugins/pipeline_host.hpp"
#include "../packages/packages.hpp"
#include "scrolling.hpp"
#include "../chat/read_state.hpp"
#include "ui_runtime.hpp"
#include "window.hpp"
#include "run.hpp"
#include "window_state.hpp"
#include "../notifications/notifications.hpp"
#include "../media/media.hpp"
#include "../media/video.hpp"
#include "tooltip.hpp"
#include "notice.hpp"
#include "../chat/unread_banner.hpp"
#include "toast.hpp"
#include "../account/dm_profile.hpp"
#include "../guild/folders.hpp"
#include "../guild/invite.hpp"
#include "../notifications/taskbar_attention.hpp"
#include "../chat/composer/spellcheck.hpp"
#include "../notifications/desktop.hpp"
#include "../notifications/controls.hpp"
#include "../account/custom_status.hpp"
#include "../chat/search/search.hpp"
#include "../chat/polls/poll.hpp"
#include "../media/picker.hpp"
#include "background.hpp"
#include "load_trace.hpp"
#include "perf_mark.hpp"
#include "memory_summary.hpp"
#include "startup_record.hpp"
#include "../voice/voice.hpp"
#include "../voice/application_voice_message.hpp"
#if defined(MAICORD_TEST_HOOKS) && defined(_WIN32)
#include "maicord/platform/net/test_server.hpp"
#endif

#if defined(MAICORD_PERF_PROBE)
#include "../../../tools/perf/perf_probe.hpp"
#endif
// Isolated probe builds never touch desktop integration, notifications,
// updates or another running instance.
#if defined(MAICORD_PERF_PROBE)
#define MAICORD_ISOLATED_MAIN 1
#endif

namespace maicord {
namespace {

using namespace app_detail;

void drain_deferred_text_services() noexcept {
    static bool failed = false;
    const bool next_failed = !secure_text_store_process_deferred();
    if (next_failed && !failed)
        log_write(LogLevel::warning, "ui", "deferred_tsf_update_failed");
    failed = next_failed;
}

// Whether the waitable at `index` of the wait set is the one that ended the wait.
bool wait_ready(const WindowWaitResult& waited, u32 index) noexcept {
    return ((waited.ready_mask >> index) & 1) != 0;
}

// Before each event: the theme's consumers follow a theme change before any input meets them.
bool pump_before_event(void*) noexcept {
    if (application_synchronize_theme_consumers())
        return true;
    log_write(LogLevel::error, "theme", "theme_layout_refresh_failed");
    return false;
}

// The text-service and overlay key filters run ahead of the event's own dispatch, on the presses and releases of
// plain keys.
bool pump_key_prefilter(void*, const WindowEvent& event) noexcept {
    if (application_account_logout_blocked())
        return application_text_form_filter(event);
    return (g_app.text_form && application_forum_form_active() && application_text_form_filter(event)) ||
           application_forum_form_filter(event) || application_composer_text_input_filter(event) ||
           application_text_form_filter(event) ||
           (!g_app.text_form && g_app.login.visible &&
            secure_text_store_filter_key(&g_app.login.text_store, event));
}

// After each event, IME-consumed input included.
void pump_after_event(void*) noexcept {
    drain_deferred_text_services();
    application_downloads_pump();
}

// Once, at startup: compiled for size with the setup it inlines.
__attribute__((minsize)) bool initialize_ui_runtime() noexcept {
    u64 stage = 1;
    if (!animation_visual_runtime_initialize(&g_app.animations)) {
        goto failed;
    }
    application_preferences_apply();
    MAICORD_PERF_MARK("ui_animations");
    ++stage;
    if (!message_composer_initialize_owned(&g_app.message_composer)) {
        goto failed;
    }
    MAICORD_PERF_MARK("ui_composer");
    ++stage;
    if (!ui_scene_initialize_bootstrap(&g_app.shell_scene)) {
        goto failed;
    }
    MAICORD_PERF_MARK("ui_bootstrap_scene");
    ++stage;
    if (!application_initialize_shell_scenes(g_app)) {
        goto failed;
    }
    MAICORD_PERF_MARK("ui_shell_scenes");
    ++stage;
    if (!application_initialize_message_scene(g_app)) {
        goto failed;
    }
    MAICORD_PERF_MARK("ui_message_scene");
    ++stage;
    if (!message_composer_scene_initialize_owned(&g_app.message_composer_scene)) {
        goto failed;
    }
    MAICORD_PERF_MARK("ui_composer_scene");
    ++stage;
    if (!synchronize_shell_layout()) {
        goto failed;
    }
    MAICORD_PERF_MARK("ui_shell_layout");
    ++stage;
    if (!application_initialize_login_ui()) {
        goto failed;
    }
    MAICORD_PERF_MARK("ui_login");
    ++stage;
    // A prestarted saved account attaches next and replaces the sign-in form
    // with the restoring view; skip shaping the form it would discard.
    if (!application_account_prestart_pending() && !application_synchronize_login_view()) {
        goto failed;
    }
    return true;

failed:
    log_u64(LogLevel::error, "ui", "ui_runtime_initialization_failed", stage);
    return false;
}

int run_application() noexcept {
    g_app.clock = clock_initialize();
    g_app.process_start = clock_now();
    MAICORD_PERF_MARK("application_run");
    // GPU driver initialization is the longest startup step; overlap it with
    // the window and model setup below. A window system whose GPU device needs its connection (Wayland) is
    // connected first.
    application_platform_window_prepare();
    renderer_prestart();
    MAICORD_PERF_MARK("renderer_prestart");
    g_app.channel_pane_width = kGuildChannelShellLayoutMetrics.navigation_width;
    g_app.member_pane_width = kGuildChannelShellLayoutMetrics.member_sidebar_width;
    log_open();
    application_log_start_record();
#if defined(MAICORD_TEST_HOOKS) && defined(_WIN32)
    // A client pointed at the fake Discord (platform/net/test_server.hpp) that is not fully pointed would reach the real
    // one: it does not start.
    if (const char* refusal = test_hooks_startup_refusal()) {
        log_write(LogLevel::error, "test_hooks", refusal);
        renderer_prestart_cancel();
        log_close();
        return 3;
    }
#endif
    MAICORD_PERF_MARK("log_open");
    application_preferences_initialize();
    MAICORD_PERF_MARK("preferences");
    // Startup candidates retain the real account/login path below, while
    // avoiding process-global notification registration and desktop integration.
#if !defined(MAICORD_ISOLATED_MAIN) && !defined(MAICORD_STARTUP_PROBE)
    if (!application_notifications_initialize(&g_app))
        log_write(LogLevel::warning, "notifications", "early_activation_unavailable");
    application_notifications_set_avatar_source(application_notification_avatar_uri);
#endif
    struct NotificationLifetime {
        ~NotificationLifetime() {
            application_notifications_release(&g_app);
        }
    } notification_lifetime;
    struct TooltipLifetime {
        ~TooltipLifetime() {
            application_tooltip_release();
        }
    } tooltip_lifetime;
    log_write(LogLevel::info, "startup", "process_entry");
    // Connect first: the saved account's DNS/TCP/TLS/Gateway handshake runs on
    // its workers while the window, D3D11 device and UI are created below.
    application_account_prestart();
    MAICORD_PERF_MARK("account_prestart");
    const MessageViewRuntimeResult message_view_initialized = message_view_runtime_initialize(
        &g_app.message_view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, true);
    if (!message_view_initialized.ok()) {
        log_u64(LogLevel::error, "model", "message_view_runtime_initialization_failed",
                static_cast<u64>(message_view_initialized.error));
        renderer_prestart_cancel();
        window_fatal_message(utf8_literal("MaiCord"), utf8_literal("MaiCord could not initialize message state."));
        application_account_prestart_cancel();
        log_close();
        return 2;
    }
    MAICORD_PERF_MARK("message_view");
    if (!application_create_main_window()) {
        renderer_prestart_cancel();
        window_fatal_message(utf8_literal("MaiCord"), utf8_literal("MaiCord could not create its main window."));
        application_account_prestart_cancel();
        log_close();
        message_view_runtime_release(&g_app.message_view);
        return 2;
    }
#if !defined(MAICORD_ISOLATED_MAIN) && !defined(MAICORD_STARTUP_PROBE)
    application_notifications_attach_window(&g_app);
    application_desktop_window_created();
#endif
    application_trace_load_stage("window_created");
    application_log_start_display(g_app.window);
    MAICORD_PERF_MARK("window_created");
    // Text services attach to the window, not the renderer: activate them while
    // the prestart worker is still creating the GPU device (the main thread
    // would otherwise wait idle). A failure is retried, and reported, by
    // initialize_ui_runtime.
    application_initialize_login_ui();
    MAICORD_PERF_MARK("login_text_services");

    // Rendered content hides default-ignorable code points, as a browser does.
    text_set_browser_invisibles(true);
    // A prestarted saved account attaches before the first frame and binds its
    // content text.
    if (!renderer_initialize(&g_app.renderer, renderer_window_surface(g_app.window), application_account_prestart_pending())) {
        renderer_prestart_cancel();
        window_fatal_message(utf8_literal("Direct3D 11 unavailable"),
                             MAICORD_OS_TEXT("MaiCord requires a Direct3D 11 GPU with Tiled Resources Tier 1 or "
                                             "higher and a WDDM 2.0 or newer graphics driver. Updating the graphics "
                                             "driver may fix this.",
                                             "MaiCord requires a hardware Direct3D 11 device and the D3D11.1 runtime."));
        application_release_login_ui();
        window_destroy(g_app.window);
        application_account_prestart_cancel();
        message_view_runtime_release(&g_app.message_view);
        log_close();
        return 3;
    }
    g_app.renderer_ready = true;
    // Accessibility's Zoom level scales the window's DPI the interface is laid out at.
    renderer_set_dpi(&g_app.renderer, application_zoomed_dpi(g_app.renderer.dpi));
    g_app.renderer.zoom_lens = &g_app.image_zoom.lens;
    application_trace_load_stage("renderer_ready");
    application_log_start_gpu(g_app.renderer.graphics.device);
    MAICORD_PERF_MARK("renderer_ready");
    if (!initialize_ui_runtime()) {
        application_release_login_ui();
        application_release_widget_storage(g_app);
        message_composer_release(&g_app.message_composer);
        animation_visual_runtime_release(&g_app.animations);
        g_app.renderer_ready = false;
        renderer_shutdown(&g_app.renderer);
        window_destroy(g_app.window);
        application_account_prestart_cancel();
        message_view_runtime_release(&g_app.message_view);
        log_close();
        return 4;
    }
    MAICORD_PERF_MARK("ui_runtime");
    if (!application_custom_theme_initialize())
        log_write(LogLevel::warning, "theme", "saved_theme_load_unavailable");
    application_trace_load_stage("ui_ready");
    MAICORD_PERF_MARK("ui_ready");
    // Publish the early-started session (or resolve it now) before the first
    // visible paint. The handshake remains asynchronous behind the restoration view.
    application_account_attach_if_present();
    MAICORD_PERF_MARK("account_attach");
#if defined(MAICORD_PERF_PROBE) || defined(MAICORD_STARTUP_PROBE)
    application_platform_window_show(ShowMode::show);
#else
    // Start minimized hides a startup-entry launch in the tray.
    {
        const int show = application_desktop_initial_show();
        application_platform_window_show(show == kDesktopShowHide                ? ShowMode::hide
                                         : show == kDesktopShowMinimizedNoActive ? ShowMode::minimize_no_activate
                                                                                 : ShowMode::show_default);
    }
#endif
    application_trace_load_stage("window_shown");
    MAICORD_PERF_MARK("window_shown");
    application_request_frame();
#if !defined(MAICORD_ISOLATED_MAIN) && !defined(MAICORD_STARTUP_PROBE)
    application_updates_initialize(application_update_build_config(), application_update_build_commit());
    application_log_start_install();
    application_catalog_initialize_default();
#endif

#if !defined(MAICORD_ISOLATED_MAIN)
    if (!application_account_logout_blocked() && !g_app.account.instance && g_app.login.visible &&
        !login_browser_mode(g_app.login.status) && !application_login_begin_remote_auth()) {
        log_write(LogLevel::error, "auth", "remote_auth_initial_start_failed");
    }
#endif

    int exit_code = 0;
    constexpr u64 kFrameClockWatchdogUs = 100'000;
    constexpr u64 kFrameClockFallbackUs = 16'000;
    application_trace_load_stage("loop_entered");
    MAICORD_PERF_MARK("loop_entered");
    g_app.frame_loop_active = true;
    for (;;) {
        drain_deferred_text_services();
        application_downloads_pump();
        application_account_logout_pump();
        application_account_switch_pump();
        Waitable wait_handles[kAccountHostNotificationHandleCapacity + 19];
        const u32 account_handle_count =
            application_account_logout_blocked()
                ? 0
                : account_host_notification_handles(&g_app.account, wait_handles,
                                                    kAccountHostNotificationHandleCapacity);
        u32 wait_handle_count = account_handle_count;
        const auto push_handle = [&](Waitable handle) noexcept {
            if (handle)
                wait_handles[wait_handle_count++] = handle;
        };
        if (!application_account_logout_blocked()) {
            push_handle(application_attachment_notification());
            push_handle(application_forum_attachments_notification());
            push_handle(application_guild_settings_notification());
        }
        push_handle(application_preferences_notification());
        push_handle(application_media_auth_notification());
        push_handle(application_custom_theme_notification());
        push_handle(application_downloads_notification());
        push_handle(application_account_logout_notification());
        push_handle(application_account_switch_notification());
        push_handle(application_updates_notification());
        push_handle(application_catalog_notification());
        push_handle(application_catalog_http_notification());
        push_handle(application_catalog_dev_notification(0));
        push_handle(application_catalog_dev_notification(1));
        push_handle(application_packages_notification());
        const u32 quit_handle_index = wait_handle_count;
        push_handle(application_platform_quit_signal());
        const u32 video_handle_index = wait_handle_count;
        const bool quit_handle = video_handle_index != quit_handle_index;
        if (!application_account_logout_blocked())
            push_handle(application_call_video_notification());
        const bool video_handle = wait_handle_count != video_handle_index;
        u32 frame_handle_index = 0xFFFFFFFFu;
        const u64 wait_now_ms = clock_monotonic_milliseconds();
        const u64 wait_now_us = application_now_us();
        // A minimized or hidden window has no display slots: DXGI stops
        // signaling and the watchdog would tick a synthetic 16 ms clock for
        // frames nobody sees. Suspend the frame clock and visual timers; the
        // pending damage presents once the window is restored.
        const bool window_hidden = !application_window_shown();
        const bool frame_clock_available = !window_hidden && g_app.renderer_ready &&
                                           !g_app.renderer.live_resize &&
                                           gpu_frame_clock_waitable(&g_app.renderer.graphics.surface);
        const bool visual_frame_requested =
            !window_hidden && application_visual_frame_requested(wait_now_ms, wait_now_us);
        // The waitable object grants an available swap-chain slot, not a
        // requirement to submit an empty frame. Keep an acquired slot while
        // a media worker is producing its next frame, and use it when ready.
        const bool retained_frame_permit =
            frame_clock_available && visual_frame_requested && g_app.renderer.begin_frame_permit;
        if (frame_clock_available && visual_frame_requested && !retained_frame_permit) {
            frame_handle_index = wait_handle_count;
            wait_handles[wait_handle_count++] = gpu_frame_clock_waitable(&g_app.renderer.graphics.surface);
            if (g_app.frame_clock_watchdog_at_us == 0) {
                g_app.frame_clock_watchdog_at_us =
                    wait_now_us +
                    (g_app.frame_clock_timer_fallback ? kFrameClockFallbackUs : kFrameClockWatchdogUs);
            }
        } else {
            g_app.frame_clock_watchdog_at_us = 0;
            g_app.frame_clock_timer_fallback = false;
        }
        u32 timeout =
            application_wait_timeout(wait_now_ms, wait_now_us, frame_handle_index != 0xFFFFFFFFu, window_hidden);
        if (!application_account_logout_blocked()) {
            timeout = minimum(timeout, application_attachment_wait_timeout(wait_now_ms));
            timeout = minimum(timeout, application_voice_message_wait_timeout(wait_now_ms));
            timeout = minimum(timeout, application_forum_attachments_wait_timeout(wait_now_ms));
        }
        timeout = minimum(timeout, application_plugins_wait_timeout(wait_now_ms));
        timeout = minimum(timeout, application_catalog_wait_timeout(wait_now_ms));
        if (retained_frame_permit)
            timeout = 0;
        if (frame_handle_index != 0xFFFFFFFFu) {
            const u64 remaining_us = g_app.frame_clock_watchdog_at_us > wait_now_us
                                         ? g_app.frame_clock_watchdog_at_us - wait_now_us
                                         : 0;
            timeout = minimum(
                timeout, static_cast<u32>(minimum<u64>((remaining_us + 999u) / 1'000u, 0xFFFFFFFEu)));
        }
        // Unfocused or minimized, a timer may slip so the system can batch it with other expirations.
        const WakeContext wake_context = window_hidden                    ? WakeContext::hidden
                                         : application_window_foreground() ? WakeContext::foreground
                                                                           : WakeContext::background;
        const u32 tolerance = frame_handle_index == 0xFFFFFFFFu && !retained_frame_permit
                                  ? wake_tolerance_ms(timeout, wake_context)
                                  : 0;
        const WindowWaitResult waited = window_wait(wait_handles, wait_handle_count, timeout, tolerance);
        MAICORD_PERF_MARK("loop_wait");
        if (waited.failed) {
            log_u64(LogLevel::error, "app", "message_wait_native_error", thread_last_error());
            exit_code = 5;
            break;
        }
#if defined(MAICORD_PERF_PROBE)
        perf::probe_observe_wait(timeout, waited.timed_out,
                                 frame_handle_index != 0xFFFFFFFFu && wait_ready(waited, frame_handle_index),
                                 waited.window_input, waited.ready_mask != 0);
#endif
        // SIGTERM, SIGINT or SIGHUP (Linux): leave the way Quit does, so the normal exit below saves what a close saves.
        // Read only when this wait saw the descriptor ready: a signal after it keeps it readable for the next wait.
        if (quit_handle && wait_ready(waited, quit_handle_index) && application_platform_quit_requested() &&
            g_app.window) {
            application_desktop_request_quit();
            window_request_close(g_app.window);
        }
        u64 restart_now_ms = clock_monotonic_milliseconds();
        application_preferences_poll();
        application_media_auth_poll();
        application_custom_theme_poll();
        application_plugins_poll();
        application_catalog_poll();
        application_packages_poll();
        application_downloads_pump();
        if (application_updates_poll() && g_app.window) {
            // An update restart closes for real, never to the tray.
            application_desktop_request_quit();
            window_request_close(g_app.window);
        }
        u64 restart_now_us = application_now_us();
        const bool video_signal = video_handle && wait_ready(waited, video_handle_index);
        if (video_signal)
            application_call_video_signaled();
        const bool account_signal = account_handle_count != 0 && (waited.ready_mask & ((u64{1} << account_handle_count) - 1)) != 0;
        bool frame_signal = retained_frame_permit || (frame_handle_index != 0xFFFFFFFFu &&
                                                      wait_ready(waited, frame_handle_index));
        if (!frame_signal && frame_handle_index != 0xFFFFFFFFu) {
            // MsgWait returns the lowest ready handle. Drain a simultaneous
            // display signal even when an earlier account handle or queued
            // window message won the primary wait.
            frame_signal = gpu_frame_try_acquire(&g_app.renderer.graphics.surface);
        } else if (!frame_signal && frame_clock_available &&
                   (video_signal ||
                    (waited.timed_out && application_visual_frame_requested(restart_now_ms, restart_now_us)))) {
            // A new call picture, or a timer that just made a visual deadline
            // due, uses a free display slot in this turn instead of waking
            // again for it.
            frame_signal = gpu_frame_try_acquire(&g_app.renderer.graphics.surface);
        }
        const bool frame_timeout = frame_handle_index != 0xFFFFFFFFu && !frame_signal &&
                                   restart_now_us >= g_app.frame_clock_watchdog_at_us;
        if (frame_signal) {
            g_app.frame_clock_timer_fallback = false;
            g_app.frame_clock_watchdog_at_us = restart_now_us + kFrameClockWatchdogUs;
        } else if (frame_timeout) {
            if (!g_app.frame_clock_timer_fallback) {
                saturating_increment(g_app.frame_clock_timeouts);
                const u32 count = g_app.frame_clock_timeouts;
                if (count == 1u || (count & (count - 1u)) == 0u) {
                    log_u64(LogLevel::warning, "render", "frame_clock_timeout_fallback", count);
                }
            }
            g_app.frame_clock_timer_fallback = true;
            g_app.frame_clock_watchdog_at_us = restart_now_us + kFrameClockFallbackUs;
        }
        const bool begin_frame = frame_signal || frame_timeout;
        if (begin_frame) {
            // A timeout is the same bounded fallback Chromium uses when the
            // compositor clock stops producing usable signals (occlusion,
            // display removal, or a driver failure). Present(1) still keeps the
            // synthetic tick from tearing.
            renderer_accept_begin_frame(&g_app.renderer);
        }
        // Input precedes this frame's scroll sample. A paint event only records damage here; independent codec
        // completions must not consume display slots from inside event dispatch with an old scroll position.
        const WindowPumpHooks pump_hooks{pump_before_event, pump_key_prefilter, pump_after_event, nullptr};
        const WindowPumpResult pumped = window_pump(256, 2'000, pump_hooks);
        const bool dispatched_messages = pumped.dispatched != 0;
        if (pumped.before_failed || pumped.quit) {
            exit_code = pumped.before_failed ? 6 : pumped.exit_code;
            break;
        }
        MAICORD_PERF_MARK("loop_dispatch");
        // A theme glide steps on display frames, like smooth scrolling (see application_visual_frame_requested).
        application_theme_schedule_tick(clock_monotonic_milliseconds(), begin_frame || !frame_clock_available);
        if (!application_synchronize_theme_consumers()) {
            log_write(LogLevel::error, "theme", "theme_layout_refresh_failed");
            exit_code = 6;
            break;
        }
        // Event dispatch can spend seconds in the system's sizing modal loop. Work below must sample the resumed
        // clock, not the pre-drag time.
        restart_now_ms = clock_monotonic_milliseconds();
        restart_now_us = application_now_us();
        application_account_logout_pump();
        application_account_switch_pump();
        const bool logout_blocked = application_account_logout_blocked();
        if (!logout_blocked && (!application_pump_attachments() || !application_forum_attachments_pump() ||
                                !application_voice_message_pump())) {
            log_write(LogLevel::error, "attachment", "attachment_pump_failed");
            exit_code = 6;
            break;
        }
        const u64 browser_deadline = application_login_browser_deadline();
        if (!logout_blocked && (account_signal || browser_deadline && restart_now_ms >= browser_deadline) &&
            !application_login_pump()) {
            log_write(LogLevel::error, "auth", "login_pump_failed");
            exit_code = 7;
            break;
        }
        if (!logout_blocked)
            application_account_reconnect_tick(restart_now_ms);
        const bool account_work =
            !logout_blocked && (account_signal || application_nonvisual_wake_due(restart_now_ms) ||
                                application_notifications_gateway_pending(&g_app));
        if (account_work) {
            application_pump_account();
        }
        MAICORD_PERF_MARK("loop_account");
        // A rejected scroll projection is a presentation failure, not a reason
        // to quit: the scroll keeps its position and the next frame rebuilds
        // the projection (presentation recovery relayouts persistent failures).
        if (application_smooth_scroll_active() && (begin_frame || !frame_clock_available) &&
            !tick_smooth_scrolls(application_now_us())) {
            application_record_presentation_failure("smooth_scroll_tick_failed");
            application_repaint();
        }
        // One bounded notification/history continuation after native dispatch.
        // Its wake schedules the next outer iteration, never recursive draining.
        if (!logout_blocked)
            application_notifications_drain(&g_app);
        // Focus and modal transitions can reveal a current chat without any
        // network work. Observe once after this turn's input and scroll settle.
        if (!logout_blocked && g_app.account_ui &&
            (dispatched_messages || account_work || application_smooth_scroll_active())) {
            const u32 revision = g_app.account_ui->chat_state.channel_mutations;
            if (!application_read_state_observe())
                log_write(LogLevel::warning, "read", "visible_ack_not_admitted");
            if (revision != g_app.account_ui->chat_state.channel_mutations) {
                refresh_account_text();
                application_request_frame();
            }
        }
        // Smooth scrolling and a theme glide need a display-clock tick even
        // when this step rounds to the same pixel or colour. Media does not:
        // its completion will invalidate the real damage. Retain the slot
        // instead of presenting once for the pending decode and again for the
        // resulting video frame.
        if (begin_frame && g_app.renderer.begin_frame_permit && g_app.window &&
            (application_smooth_scroll_active() || application_theme_transition_active())) {
            application_request_frame();
        }
        // Procedural text/profile effects need a draw even when no media
        // worker publishes pixels. Consume their slot instead of retaining it
        // and immediately waking again on the same overdue effect deadline.
        const bool pre_procedural_damage = g_app.frame_pending;
        const bool effect_due = application_effect_frame_due(restart_now_ms);
        if ((begin_frame || !frame_clock_available) && effect_due)
            application_request_frame();
        const bool background_requested = application_background_request_frame_if_due(
            restart_now_ms, begin_frame || !frame_clock_available);
        const bool surface_requested = application_surface_motion_request_frame_if_due(
            restart_now_ms, begin_frame || !frame_clock_available);
        // One media drain per turn, after scroll has selected the viewport.
        // Pure animation ticks do not run the REST/Gateway projection pump.
        if (!logout_blocked &&
            (account_work || begin_frame || g_app.frame_pending ||
             (!frame_clock_available &&
              (application_animation_frame_due(restart_now_us) ||
               (application_video_next_wake() != 0 && restart_now_ms >= application_video_next_wake()))))) {
            bool repaint = false;
            // A video overlay phase needs no static-media visibility scan.
            // Real completions, viewport changes and animation deadlines keep
            // the normal drain path; this tick only reprojects selected video.
            const u64 video_wake = application_video_next_wake();
            const bool media_animation_due = application_animation_frame_due(restart_now_us);
            const bool scrolling = application_smooth_scroll_active();
            const bool background_only = background_frame_reuses_static_media(
                {background_requested, pre_procedural_damage, account_work, dispatched_messages,
                 media_animation_due, video_wake != 0 && restart_now_ms >= video_wake, scrolling,
                 effect_due || surface_requested, g_app.message_media.scan_pending});
            const bool video_overlay_only = !account_work && !g_app.frame_pending && video_wake != 0 &&
                                            restart_now_ms >= video_wake && !media_animation_due &&
                                            !scrolling;
            // Shader-only background frames reuse resident media. A completion
            // arriving after this turn's wait remains signaled for the next
            // account-priority turn; no mailbox result is acknowledged here.
            // Media is presentation: a failure here degrades pictures, never
            // the account session (each refused asset backs off on its own).
            if (!video_overlay_only && !background_only && !application_pump_visible_static_media(&repaint))
                application_record_presentation_failure("static_media_pump_failed");
            if (!application_video_pump(&repaint))
                application_record_presentation_failure("video_pump_failed");
            application_received_attachment_pump();
            application_search_content_pump();
            g_app.frame_pending = g_app.frame_pending || repaint;
        }
        if (!application_login_tick_backup_acknowledgement(restart_now_ms)) {
            log_write(LogLevel::error, "ui", "backup_acknowledgement_refresh_failed");
            exit_code = 6;
            break;
        }
        if (!application_login_tick_auth_cooldown(restart_now_ms)) {
            log_write(LogLevel::error, "ui", "auth_cooldown_refresh_failed");
            exit_code = 6;
            break;
        }
        if (!application_login_tick_recovery(restart_now_ms)) {
            log_write(LogLevel::error, "ui", "recovery_cooldown_refresh_failed");
            exit_code = 6;
            break;
        }
        if (!application_login_tick_phone(restart_now_ms)) {
            log_write(LogLevel::error, "ui", "phone_cooldown_refresh_failed");
            exit_code = 6;
            break;
        }
        if (!application_login_tick_reset(restart_now_ms)) {
            log_write(LogLevel::error, "ui", "reset_cooldown_refresh_failed");
            exit_code = 6;
            break;
        }
        if (!application_login_tick_captcha(restart_now_ms)) {
            log_write(LogLevel::error, "ui", "captcha_deadline_refresh_failed");
            exit_code = 6;
            break;
        }
        if (!logout_blocked)
            application_login_tick_caret(restart_now_ms);
        application_text_form_tick(restart_now_ms);
        // Media atlas recency clock and idle trims (one wake per upload burst).
        renderer_media_maintain(&g_app.renderer, restart_now_ms);
        if (renderer_glyph_atlas_take_stale(&g_app.renderer)) {
            // An idle trim moved glyph rows: retained quads must be built again.
            refresh_account_text(true);
            application_repaint();
        }
        if (!logout_blocked) {
            application_forum_form_tick(restart_now_ms);
            application_friends_tick(restart_now_ms);
            application_search_tick(restart_now_ms);
            application_voice_invite_tick(restart_now_ms);
            application_toast_tick(restart_now_ms);
            application_dm_profile_tick(restart_now_ms);
            application_inbox_trim();
            const u64 poll_wake = application_poll_next_wake();
            if (poll_wake && restart_now_ms >= poll_wake) {
                if (!application_poll_synchronize() || !refresh_account_text(true)) {
                    log_write(LogLevel::warning, "poll", "expiry_refresh_failed");
                }
            }
            application_media_picker_tick(restart_now_ms);
            application_channel_overview_tick(restart_now_ms);
            application_account_settings_test_open();
            if (!application_tooltip_synchronize(restart_now_ms))
                log_write(LogLevel::warning, "ui", "tooltip_synchronize_failed");
            application_notification_controls_tick(restart_now_ms);
            application_custom_status_tick(restart_now_ms);
            application_activity_tick(restart_now_ms);
            application_streamer_mode_synchronize();
            application_notice_synchronize(restart_now_ms);
            application_tick_message_composer_caret(restart_now_ms);
            application_composer_spellcheck_tick(restart_now_ms);
            application_message_interaction_tick(restart_now_ms);
        }
#if !defined(MAICORD_ISOLATED_MAIN)
        if (!logout_blocked && g_app.login.remote_restart_at_ms != 0 &&
            restart_now_ms >= g_app.login.remote_restart_at_ms && g_app.login.visible &&
            !g_app.login.remote_active && !login_browser_mode(g_app.login.status)) {
            g_app.login.remote_restart_at_ms = 0;
            if (!application_login_begin_remote_auth()) {
                log_write(LogLevel::error, "auth", "remote_auth_restart_failed");
            }
        }
#endif
        application_memory_summary_tick(restart_now_ms);
        // Relative timestamps are visual: a minimized window refreshes them on restore.
        if (!window_hidden && !application_refresh_relative_timestamps_if_due()) {
            log_write(LogLevel::error, "ui", "relative_timestamp_refresh_failed");
            exit_code = 6;
            break;
        }
        MAICORD_PERF_MARK("loop_before_present");
        // The login form and the main layout have different minimums: the compositor gets the current one with the frame.
        application_window_refresh_minimum_size();
        application_window_refresh_idle_inhibit();
        application_present_pending_frame();
        // Shell integration (taskbar button state, notification-area icon)
        // follows the frame: its first COM activation and Shell_NotifyIcon
        // round trips would otherwise delay the first image.
#if !defined(MAICORD_STARTUP_PROBE)
        if (!logout_blocked)
            application_taskbar_attention_tick(restart_now_ms);
#endif
#if !defined(MAICORD_ISOLATED_MAIN) && !defined(MAICORD_STARTUP_PROBE)
        application_desktop_tick(restart_now_ms);
#endif
#if defined(MAICORD_PERF_PROBE)
        if (perf::probe_step(&exit_code))
            break;
#endif
        application_update_frame_rate(application_now_us());
    }

    g_app.frame_loop_active = false;
    drain_deferred_text_services();
    application_media_picker_close();
    application_preferences_shutdown();
    application_media_auth_shutdown();
    // After Restart to update, a normal exit swaps in the update this process verified.
    application_updates_release(exit_code == 0);
    application_catalog_release();
    application_notifications_prepare_shutdown(&g_app);
    application_tooltip_release();
    application_unread_banner_release();
    application_toast_release();
    application_guild_invite_release();
    application_taskbar_attention_release();
    application_composer_spellcheck_release();
    application_desktop_release();
    // The account's final startup shell records the folder layout.
    application_stop_account();
    application_guild_folders_release();
    application_account_logout_release();
    application_notifications_release(&g_app);
    application_voice_message_release();
    application_release_attachments();
    application_forum_attachments_release();
    application_forum_preview_release();
    application_composer_preview_release();
    g_app.renderer_ready = false;
    application_persist_drafts_on_exit();
    application_release_account_ui(g_app);
    application_release_login_ui();
    application_release_widget_storage(g_app);
    application_packages_release();
    if (!application_custom_theme_release(kAccountStopTimeoutMs))
        log_write(LogLevel::warning, "theme", "theme_worker_shutdown_pending");
    message_composer_release(&g_app.message_composer);
    renderer_shutdown(&g_app.renderer);
    application_theme_release();
    animation_visual_runtime_release(&g_app.animations);
    message_view_runtime_release(&g_app.message_view);
    clipboard_attachment_shutdown();
    // After Restart to update: start the (now updated) executable once this
    // instance has released its window and profile.
    application_updates_relaunch();
    log_write(LogLevel::info, "shutdown", "normal_exit");
    log_close();
#if defined(MAICORD_PERF_PROBE)
    perf::startup_trace_report();
    perf::probe_finish(exit_code);
#endif
    return exit_code;
}

} // namespace

int application_run() noexcept {
    return run_application();
}

} // namespace maicord
