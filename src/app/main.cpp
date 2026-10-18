#include "application_activity.hpp"
#include "maicord/text/visibility.hpp"
#include "application_message_feedback.hpp"
#include "application_downloads.hpp"
#include "application_search_content.hpp"
#include "application_inbox.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/app/animation_visual_runtime.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/app/background_frame_policy.hpp"
#include "maicord/app/wake_coalescing.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/module.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/secure_text_store.hpp"
#include "maicord/platform/clipboard_attachment.hpp"

#include "application_state.hpp"
#include "application_channel_overview.hpp"
#include "application_guild_settings.hpp"
#include "application_update_startup.hpp"
#include "application_updates.hpp"
#include "application_plugin_browser.hpp"
#include "application_friends.hpp"
#include "application_text_form.hpp"
#include "application_forum_form.hpp"
#include "application_composer_text_input.hpp"
#include "application_attachments.hpp"
#include "application_forum_attachments.hpp"
#include "application_forum_preview.hpp"
#include "application_composer_preview.hpp"
#include "application_account_module.hpp"
#include "application_account_logout.hpp"
#include "application_account_switch.hpp"
#include "application_account_presentation.hpp"
#include "application_account_pump.hpp"
#include "application_content_input.hpp"
#include "application_frame_presentation.hpp"
#include "application_composer.hpp"
#include "application_login_auth.hpp"
#include "application_captcha_auth.hpp"
#include "application_login_input.hpp"
#include "application_login_ui.hpp"
#include "application_scheduler.hpp"
#include "application_shutdown_metrics.hpp"
#include "application_settings.hpp"
#include "application_custom_theme.hpp"
#include "application_plugins.hpp"
#include "application_scrolling.hpp"
#include "application_read_state.hpp"
#include "application_ui_runtime.hpp"
#include "application_window.hpp"
#include "application_window_runtime.hpp"
#include "application_notifications.hpp"
#include "application_media.hpp"
#include "application_video.hpp"
#include "application_tooltip.hpp"
#include "application_unread_banner.hpp"
#include "application_guild_folders.hpp"
#include "application_guild_invite.hpp"
#include "application_taskbar_attention.hpp"
#include "application_composer_spellcheck.hpp"
#include "application_desktop.hpp"
#include "application_notification_controls.hpp"
#include "application_custom_status.hpp"
#include "application_search.hpp"
#include "application_poll.hpp"
#include "application_media_picker.hpp"
#include "application_background.hpp"
#include "application_load_trace.hpp"
#include "application_perf_mark.hpp"
#include "application_voice.hpp"
#include "application_voice_message.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#if defined(MAICORD_PERF_PROBE)
#include "../../tools/perf/perf_probe.hpp"
#endif
#if defined(MAICORD_PERF_PROBE) || defined(MAICORD_STARTUP_PROBE)
#include "../../tools/perf/probe_window.hpp"
#endif
// Isolated probe builds never touch desktop integration, notifications,
// updates or another running instance.
#if defined(MAICORD_PERF_PROBE)
#define MAICORD_ISOLATED_MAIN 1
#endif

namespace maicord {
namespace {

using namespace app_detail;

// Main-thread wakes after startup settles, by cause; logged at shutdown so idle
// wake rates are measurable without tracing.
struct LoopWakeLedger {
    u64 settled_at_ms;
    u64 wakes;
    u64 timeouts;
    u64 messages;
    u64 frames;
    u64 handles;
};

void drain_deferred_text_services() noexcept {
    static bool failed = false;
    const bool next_failed = !secure_text_store_process_deferred();
    if (next_failed && !failed)
        log_write(LogLevel::warning, "ui", "deferred_tsf_update_failed");
    failed = next_failed;
}

bool initialize_ui_runtime() noexcept {
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

int application_run() noexcept {
    g_app.clock = clock_initialize();
    g_app.process_start = clock_now();
    MAICORD_PERF_MARK("application_run");
    // GPU driver initialization is the longest startup step; overlap it with
    // the window and model setup below.
    renderer_prestart();
    MAICORD_PERF_MARK("renderer_prestart");
    g_app.channel_pane_width = kGuildChannelShellLayoutMetrics.navigation_width;
    g_app.member_pane_width = kGuildChannelShellLayoutMetrics.member_sidebar_width;
    g_app.instance = module_instance();
    log_open();
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
    log_write(LogLevel::info, "startup", "output_debug_string_mirror_enabled");

    // The embedded manifest establishes PerMonitorV2 before process entry.
    log_write(LogLevel::info, "startup", "dpi_awareness_manifest_per_monitor_v2");
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
        MessageBoxW(nullptr, L"MaiCord could not initialize message state.", L"MaiCord",
                    MB_OK | MB_ICONERROR);
        application_account_prestart_cancel();
        log_close();
        return 2;
    }
    log_u64(LogLevel::info, "model", "message_records", g_app.message_view.store.count);
    log_u64(LogLevel::info, "model", "message_text_bytes", g_app.message_view.store.text_used);
    MAICORD_PERF_MARK("message_view");
    if (!application_create_main_window(application_window_proc)) {
        renderer_prestart_cancel();
        MessageBoxW(nullptr, L"MaiCord could not create its main window.", L"MaiCord",
                    MB_OK | MB_ICONERROR);
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
    MAICORD_PERF_MARK("window_created");
    // Text services attach to the window, not the renderer: activate them while
    // the prestart worker is still creating the GPU device (the main thread
    // would otherwise wait idle). A failure is retried, and reported, by
    // initialize_ui_runtime.
    application_initialize_login_ui();
    MAICORD_PERF_MARK("login_text_services");

    // Rendered content hides default-ignorable code points, as a browser does.
    text_set_browser_invisibles(true);
    if (!renderer_initialize(&g_app.renderer, g_app.window)) {
        renderer_prestart_cancel();
        MessageBoxW(g_app.window,
                    L"MaiCord requires a hardware Direct3D 11 device and the D3D11.1 runtime.",
                    L"Direct3D 11 unavailable", MB_OK | MB_ICONERROR);
        application_release_login_ui();
        DestroyWindow(g_app.window);
        application_account_prestart_cancel();
        message_view_runtime_release(&g_app.message_view);
        log_close();
        return 3;
    }
    g_app.renderer_ready = true;
    application_trace_load_stage("renderer_ready");
    MAICORD_PERF_MARK("renderer_ready");
    if (!initialize_ui_runtime()) {
        application_release_login_ui();
        application_release_widget_storage(g_app);
        message_composer_release(&g_app.message_composer);
        animation_visual_runtime_release(&g_app.animations);
        g_app.renderer_ready = false;
        renderer_shutdown(&g_app.renderer);
        DestroyWindow(g_app.window);
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
    // Shown without activation at the bottom of the z-order: the probe never
    // takes keyboard focus or covers another window.
    probe_window::show(g_app.window);
#else
    // Start minimized hides a startup-entry launch in the tray.
    ShowWindow(g_app.window, application_desktop_initial_show());
#endif
    application_trace_load_stage("window_shown");
    MAICORD_PERF_MARK("window_shown");
    application_request_frame();
#if !defined(MAICORD_ISOLATED_MAIN) && !defined(MAICORD_STARTUP_PROBE)
    application_updates_confirm_startup();
    application_updates_initialize(application_update_build_config());
    application_plugin_browser_initialize();
#endif

#if !defined(MAICORD_ISOLATED_MAIN)
    if (!application_account_logout_blocked() && !g_app.account.instance && g_app.login.visible &&
        !login_browser_mode(g_app.login.status) && !application_login_begin_remote_auth()) {
        log_write(LogLevel::error, "auth", "remote_auth_initial_start_failed");
    }
#endif

    MSG message{};
    int exit_code = 0;
    bool quit = false;
    constexpr u64 kFrameClockWatchdogUs = 100'000;
    constexpr u64 kFrameClockFallbackUs = 16'000;
    constexpr u64 kWakeLedgerSettleMs = 5'000;
    LoopWakeLedger wake_ledger{clock_monotonic_milliseconds() + kWakeLedgerSettleMs};
    application_trace_load_stage("loop_entered");
    MAICORD_PERF_MARK("loop_entered");
    // Synchronization timer for tolerant background waits (SetWaitableTimerEx).
    const HANDLE wake_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    g_app.frame_loop_active = true;
    while (!quit) {
        drain_deferred_text_services();
        application_downloads_pump();
        application_account_logout_pump();
        application_account_switch_pump();
        HANDLE wait_handles[kAccountHostNotificationHandleCapacity + 13]{};
        const u32 account_handle_count =
            application_account_logout_blocked()
                ? 0
                : account_host_notification_handles(&g_app.account, wait_handles,
                                                    kAccountHostNotificationHandleCapacity);
        u32 wait_handle_count = account_handle_count;
        const auto push_handle = [&](HANDLE handle) noexcept {
            if (handle)
                wait_handles[wait_handle_count++] = handle;
        };
        if (!application_account_logout_blocked()) {
            push_handle(application_attachment_notification());
            push_handle(application_forum_attachments_notification());
            push_handle(application_guild_settings_notification());
        }
        push_handle(application_preferences_notification());
        push_handle(application_custom_theme_notification());
        push_handle(application_plugins_notification());
        push_handle(application_downloads_notification());
        push_handle(application_account_logout_notification());
        push_handle(application_account_switch_notification());
        push_handle(application_updates_notification());
        push_handle(application_plugin_browser_notification());
        u32 frame_handle_index = 0xFFFFFFFFu;
        const u64 wait_now_ms = clock_monotonic_milliseconds();
        const u64 wait_now_us = application_now_us();
        // A minimized or hidden window has no display slots: DXGI stops
        // signaling and the watchdog would tick a synthetic 16 ms clock for
        // frames nobody sees. Suspend the frame clock and visual timers; the
        // pending damage presents once the window is restored.
        const bool window_hidden = g_app.window && (IsIconic(g_app.window) || !IsWindowVisible(g_app.window));
        const bool frame_clock_available = !window_hidden && g_app.renderer_ready &&
                                           !g_app.renderer.live_resize &&
                                           g_app.renderer.graphics.frame_latency_waitable;
        const bool visual_frame_requested =
            !window_hidden && application_visual_frame_requested(wait_now_ms, wait_now_us);
        // The waitable object grants an available swap-chain slot, not a
        // requirement to submit an empty frame. Keep an acquired slot while
        // a media worker is producing its next frame, and use it when ready.
        const bool retained_frame_permit =
            frame_clock_available && visual_frame_requested && g_app.renderer.begin_frame_permit;
        if (frame_clock_available && visual_frame_requested && !retained_frame_permit) {
            frame_handle_index = wait_handle_count;
            wait_handles[wait_handle_count++] = g_app.renderer.graphics.frame_latency_waitable;
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
        if (retained_frame_permit)
            timeout = 0;
        if (frame_handle_index != 0xFFFFFFFFu) {
            const u64 remaining_us = g_app.frame_clock_watchdog_at_us > wait_now_us
                                         ? g_app.frame_clock_watchdog_at_us - wait_now_us
                                         : 0;
            timeout = minimum(
                timeout, static_cast<u32>(minimum<u64>((remaining_us + 999u) / 1'000u, INFINITE - 1u)));
        }
        // Unfocused or minimized, a timer may slip so Windows can batch it with
        // other expirations; the waitable timer replaces the exact timeout.
        u32 wake_timer_index = 0xFFFFFFFFu;
        DWORD wait_timeout = timeout;
        const WakeContext wake_context = window_hidden ? WakeContext::hidden
                                         : g_app.window && GetForegroundWindow() == g_app.window
                                             ? WakeContext::foreground
                                             : WakeContext::background;
        const u32 tolerance = frame_handle_index == 0xFFFFFFFFu && !retained_frame_permit
                                  ? wake_tolerance_ms(timeout, wake_context)
                                  : 0;
        if (tolerance && wake_timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(timeout) * 10'000;
            if (SetWaitableTimerEx(wake_timer, &due, 0, nullptr, nullptr, nullptr, tolerance)) {
                wake_timer_index = wait_handle_count;
                wait_handles[wait_handle_count++] = wake_timer;
                wait_timeout = INFINITE;
            }
        }
        DWORD waited =
            MsgWaitForMultipleObjectsEx(wait_handle_count, wait_handle_count != 0 ? wait_handles : nullptr,
                                        wait_timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wake_timer_index != 0xFFFFFFFFu) {
            if (waited == WAIT_OBJECT_0 + wake_timer_index)
                waited = WAIT_TIMEOUT;
            else
                CancelWaitableTimer(wake_timer);
        }
        MAICORD_PERF_MARK("loop_wait");
        if (waited == WAIT_FAILED) {
            log_u64(LogLevel::error, "app", "message_wait_native_error", thread_last_error());
            exit_code = 5;
            break;
        }
#if defined(MAICORD_PERF_PROBE)
        perf::probe_observe_wait(timeout, waited == WAIT_TIMEOUT,
                                 frame_handle_index != 0xFFFFFFFFu && waited == WAIT_OBJECT_0 + frame_handle_index,
                                 waited == WAIT_OBJECT_0 + wait_handle_count,
                                 waited >= WAIT_OBJECT_0 && waited < WAIT_OBJECT_0 + wait_handle_count);
#endif
        u64 restart_now_ms = clock_monotonic_milliseconds();
        if (restart_now_ms >= wake_ledger.settled_at_ms) {
            ++wake_ledger.wakes;
            if (waited == WAIT_TIMEOUT)
                ++wake_ledger.timeouts;
            else if (waited == WAIT_OBJECT_0 + wait_handle_count)
                ++wake_ledger.messages;
            else if (frame_handle_index != 0xFFFFFFFFu && waited == WAIT_OBJECT_0 + frame_handle_index)
                ++wake_ledger.frames;
            else
                ++wake_ledger.handles;
        }
        application_preferences_poll();
        application_custom_theme_poll();
        application_plugins_poll();
        application_plugin_browser_poll();
        application_downloads_pump();
        if (application_updates_poll() && g_app.window) {
            // An update restart closes for real, never to the tray.
            application_desktop_request_quit();
            PostMessageW(g_app.window, WM_CLOSE, 0, 0);
        }
        u64 restart_now_us = application_now_us();
        const bool account_signal = account_handle_count != 0 && waited >= WAIT_OBJECT_0 &&
                                    waited < WAIT_OBJECT_0 + account_handle_count;
        bool frame_signal = retained_frame_permit || (frame_handle_index != 0xFFFFFFFFu &&
                                                      waited == WAIT_OBJECT_0 + frame_handle_index);
        if (!frame_signal && frame_handle_index != 0xFFFFFFFFu) {
            // MsgWait returns the lowest ready handle. Drain a simultaneous
            // display signal even when an earlier account handle or queued
            // window message won the primary wait.
            frame_signal = d3d11_device_owner_wait(g_app.renderer.graphics, 0) == WAIT_OBJECT_0;
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
        // Input precedes this frame's scroll sample. WM_PAINT only records
        // damage here; independent codec completions must not consume display
        // slots from inside DispatchMessage with an old scroll position.
        const i64 dispatch_start = clock_now();
        bool dispatched_messages = false;
        for (u32 dispatched = 0; dispatched < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
             ++dispatched) {
            if (!application_synchronize_theme_consumers()) {
                log_write(LogLevel::error, "theme", "theme_layout_refresh_failed");
                exit_code = 6;
                quit = true;
                break;
            }
            dispatched_messages = true;
            if (message.message == WM_QUIT) {
                exit_code = static_cast<int>(message.wParam);
                quit = true;
                break;
            }
            const bool filtered =
                application_account_logout_blocked()
                    ? application_text_form_filter_message(message)
                    : (g_app.text_form && application_forum_form_active() &&
                       application_text_form_filter_message(message)) ||
                          application_forum_form_filter_message(message) ||
                          application_composer_text_input_filter(message) ||
                          application_text_form_filter_message(message) ||
                          (!g_app.text_form && g_app.login.visible &&
                           secure_text_store_filter_message(&g_app.login.text_store, message));
            if (!filtered) {
                if (message.message == WM_KEYDOWN || message.message == WM_KEYUP ||
                    message.message == WM_SYSKEYDOWN || message.message == WM_SYSKEYUP)
                    TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            drain_deferred_text_services();
            application_downloads_pump();
            // Include IME-consumed input in the soft fairness budget. A slow
            // handler cannot be preempted; pending messages remain queued.
            if ((dispatched & 7u) == 7u &&
                clock_microseconds(g_app.clock, dispatch_start, clock_now()) >= 2'000u)
                break;
        }
        if (quit)
            break;
        MAICORD_PERF_MARK("loop_dispatch");
        if (!application_synchronize_theme_consumers()) {
            log_write(LogLevel::error, "theme", "theme_layout_refresh_failed");
            exit_code = 6;
            break;
        }
        // DispatchMessage can spend seconds in DefWindowProc's sizing modal
        // loop. Work below must sample the resumed clock, not the pre-drag time.
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
            if (!application_read_state_observe(clock_monotonic_milliseconds()))
                log_write(LogLevel::warning, "read", "visible_ack_not_admitted");
            if (revision != g_app.account_ui->chat_state.channel_mutations) {
                refresh_account_text();
                application_request_frame();
            }
        }
        // Smooth scrolling needs a display-clock tick even when this step
        // rounds to the same pixel. Media does not: its completion will
        // invalidate the real damage. Retain the slot instead of presenting
        // once for the pending decode and again for the resulting video frame.
        if (begin_frame && g_app.renderer.begin_frame_permit && g_app.window &&
            application_smooth_scroll_active()) {
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
            if ((!video_overlay_only && !background_only &&
                 !application_pump_visible_static_media(&repaint)) ||
                !application_video_pump(&repaint)) {
                log_write(LogLevel::error, "media", "static_media_pump_failed");
                application_stop_account();
            }
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
        if (!logout_blocked) {
            application_forum_form_tick(restart_now_ms);
            application_friends_tick(restart_now_ms);
            application_search_tick(restart_now_ms);
            application_voice_timer_tick(restart_now_ms);
            application_inbox_trim();
            const u64 poll_wake = application_poll_next_wake();
            if (poll_wake && restart_now_ms >= poll_wake) {
                if (!application_poll_synchronize() || !refresh_account_text(true)) {
                    log_write(LogLevel::warning, "poll", "expiry_refresh_failed");
                }
            }
            application_media_picker_tick(restart_now_ms);
            application_channel_overview_tick(restart_now_ms);
            if (!application_tooltip_synchronize(restart_now_ms))
                log_write(LogLevel::warning, "ui", "tooltip_synchronize_failed");
            application_notification_controls_tick(restart_now_ms);
            application_custom_status_tick(restart_now_ms);
            application_activity_tick(restart_now_ms);
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
        // Relative timestamps are visual: a minimized window refreshes them on restore.
        if (!window_hidden && !application_refresh_relative_timestamps_if_due()) {
            log_write(LogLevel::error, "ui", "relative_timestamp_refresh_failed");
            exit_code = 6;
            break;
        }
        MAICORD_PERF_MARK("loop_before_present");
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
    if (wake_timer)
        CloseHandle(wake_timer);
    const u64 wake_end_ms = clock_monotonic_milliseconds();
    log_u64(LogLevel::info, "app", "steady_wake_window_ms",
            wake_end_ms > wake_ledger.settled_at_ms ? wake_end_ms - wake_ledger.settled_at_ms : 0);
    log_u64(LogLevel::info, "app", "steady_wakes", wake_ledger.wakes);
    log_u64(LogLevel::info, "app", "steady_wake_timeouts", wake_ledger.timeouts);
    log_u64(LogLevel::info, "app", "steady_wake_messages", wake_ledger.messages);
    log_u64(LogLevel::info, "app", "steady_wake_frames", wake_ledger.frames);
    log_u64(LogLevel::info, "app", "steady_wake_handles", wake_ledger.handles);
    drain_deferred_text_services();
    application_media_picker_close();
    application_preferences_shutdown();
    application_updates_release();
    application_plugin_browser_release();
    application_notifications_prepare_shutdown(&g_app);
    application_tooltip_release();
    application_unread_banner_release();
    application_guild_folders_release();
    application_guild_invite_release();
    application_taskbar_attention_release();
    application_composer_spellcheck_release();
    application_desktop_release();
    application_stop_account();
    application_account_logout_release();
    application_notifications_release(&g_app);
    application_voice_message_release();
    application_release_attachments();
    application_forum_attachments_release();
    application_forum_preview_release();
    application_composer_preview_release();
    g_app.renderer_ready = false;
    application_log_shutdown_metrics();
    application_release_account_ui(g_app);
    application_release_login_ui();
    application_release_widget_storage(g_app);
    if (!application_custom_theme_release(kAccountStopTimeoutMs))
        log_write(LogLevel::warning, "theme", "theme_worker_shutdown_pending");
    message_composer_release(&g_app.message_composer);
    renderer_shutdown(&g_app.renderer);
    application_theme_release();
    animation_visual_runtime_release(&g_app.animations);
    message_view_runtime_release(&g_app.message_view);
    clipboard_attachment_shutdown();
    log_write(LogLevel::info, "shutdown", "normal_exit");
    log_close();
#if defined(MAICORD_PERF_PROBE)
    perf::startup_trace_report();
    perf::probe_finish(exit_code);
#endif
    return exit_code;
}

} // namespace

} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_entry() noexcept {
#if defined(MAICORD_STARTUP_PROBE)
    if (!maicord::app_detail::application_startup_probe_initialize())
        ExitProcess(126);
#endif
#if defined(MAICORD_PERF_PROBE)
    maicord::perf::startup_trace_initialize();
    if (!maicord::perf::probe_initialize())
        ExitProcess(125);
#endif
#if defined(MAICORD_ISOLATED_MAIN) || defined(MAICORD_STARTUP_PROBE)
    const auto startup = maicord::app_detail::application_update_startup_gate(
        maicord::app_detail::application_update_startup_command(GetCommandLineW()));
#else
    const auto command = maicord::app_detail::application_update_startup_command(GetCommandLineW());
    // A second launch hands its discord:// link to the running client.
    if (maicord::app_detail::application_desktop_forward_existing(command))
        ExitProcess(0);
    const auto startup = maicord::app_detail::application_update_startup_run(
        command, maicord::app_detail::application_update_build_config());
#endif
    if (startup.mode == maicord::app_detail::ApplicationUpdateStartupMode::refused ||
        startup.mode == maicord::app_detail::ApplicationUpdateStartupMode::dispatched)
        ExitProcess(startup.exit_code);
#if !defined(MAICORD_ISOLATED_MAIN) && !defined(MAICORD_STARTUP_PROBE)
    maicord::app_detail::application_desktop_prepare(
        command, startup.mode == maicord::app_detail::ApplicationUpdateStartupMode::child);
#endif
    const int result = maicord::application_run();
    ExitProcess(static_cast<UINT>(result));
}
