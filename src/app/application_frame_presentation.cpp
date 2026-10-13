#include "application_guild_community.hpp"
#include "application_guild_menu.hpp"
#include "application_message_components.hpp"
#include "application_invite_cards.hpp"
#include "application_guild_lifecycle.hpp"
#include "application_navigation_drag.hpp"
#include "application_shortcuts.hpp"
#include "application_accessibility.hpp"
#include "application_call.hpp"
#include "application_stream.hpp"
#include "application_guild_card.hpp"
#include "application_guild_banner.hpp"
#include "application_inbox.hpp"
#include "application_cursor.hpp"
#include "application_guild_invite.hpp"
#include "application_guild_folders.hpp"
#include "application_user_settings.hpp"
#include "application_unread_banner.hpp"
#include "application_downloads.hpp"
#include "application_gif_favorite_star.hpp"
#include "application_load_trace.hpp"
#include "application_perf_mark.hpp"
#include "application_frame_presentation.hpp"
#include "application_media.hpp"
#include "application_background.hpp"
#include "application_window_resize.hpp"

#include "application_account_presentation.hpp"
#include "application_login_ui.hpp"
#include "application_state.hpp"
#include "application_ui_runtime.hpp"
#include "application_shell_input.hpp"
#include "application_text_form.hpp"
#include "application_composer_text_input.hpp"
#include "application_composer_edit_menu.hpp"
#include "application_settings.hpp"
#include "application_theme_editor.hpp"
#include "application_friends.hpp"
#include "application_account_menu.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_thread_browser.hpp"
#include "application_thread_creation.hpp"
#include "application_forum_post.hpp"
#include "application_forum_form.hpp"
#include "application_forum_attachments.hpp"
#include "application_media_picker.hpp"
#include "application_quick_switcher.hpp"
#include "application_message_forward.hpp"
#include "application_self_status.hpp"
#include "application_plugins.hpp"
#include "application_surface_motion.hpp"
#include "application_account_module.hpp"
#include "application_message_interaction.hpp"
#include "application_profile.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/secure_text_store.hpp"
#include "maicord/ui/spinner.hpp"
#include "../render/d3d11_renderer_interaction_state.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord::app_detail {
namespace {

// The FPS counter in the window title is a diagnostic; it shows only when MAICORD_GPU_METRICS=1.
bool frame_rate_title_enabled() noexcept {
    static u32 cached = 0; // 0 unknown, 1 off, 2 on
    if (cached == 0) {
        wchar_t value[2]{};
        const DWORD length = GetEnvironmentVariableW(L"MAICORD_GPU_METRICS", value, 2);
        cached = (length == 1 && value[0] == L'1') ? 2u : 1u;
    }
    return cached == 2;
}

UiSpinnerClock g_loading_spinner{};

// Each visible spinner paints the shared phase; a hidden panel or reduced
// motion disarms the clock so nothing wakes while no spinner is on screen.
void synchronize_loading_spinners(u64 now_ms) noexcept {
    auto& renderer = g_app.renderer;
    // The renderer borrows these app-owned scenes read-only; the phase is a
    // paint-only field the application frame clock owns.
    auto* pins = d3d11_renderer_pinned_messages_visible(renderer) &&
                         ui_pinned_messages_scene_spinner_visible(*renderer.pinned_messages_scene)
                     ? const_cast<UiPinnedMessagesScene*>(renderer.pinned_messages_scene)
                     : nullptr;
    auto* inbox = d3d11_renderer_inbox_visible(renderer) && ui_inbox_scene_spinner_visible(*renderer.inbox_scene)
                      ? const_cast<UiInboxScene*>(renderer.inbox_scene)
                      : nullptr;
    UiThreadBrowserScene* browser = d3d11_renderer_thread_browser_visible(renderer) &&
                                            ui_thread_browser_scene_spinner_visible(*renderer.thread_browser_scene)
                                        ? renderer.thread_browser_scene
                                        : nullptr;
    // Stream tiles (connecting, reconnecting) and the stream card's thumbnail.
    auto* call = d3d11_renderer_call_visible(renderer) && ui_call_scene_spinner_visible(*renderer.call_scene)
                     ? const_cast<UiCallScene*>(renderer.call_scene)
                     : nullptr;
    auto* card = d3d11_renderer_stream_card_visible(renderer) ? application_stream_card_spinner_scene() : nullptr;
    auto* guild = d3d11_renderer_guild_card_visible(renderer) ? application_guild_card_spinner_scene() : nullptr;
    // Settings pages' loading notices.
    auto* settings = renderer_settings_page_visible(renderer) && renderer.settings_scene &&
                             ui_settings_scene_spinner_visible(*renderer.settings_scene)
                         ? const_cast<UiSettingsScene*>(renderer.settings_scene)
                         : nullptr;
    const bool visible = g_app.renderer_ready && !renderer.live_resize &&
                         (pins || inbox || browser || call || card || guild || settings);
    ui_spinner_clock_tick(&g_loading_spinner, now_ms, visible, g_app.preferences.reduced_motion);
    // The phase is part of each panel's presentation stamp, so a new phase
    // repaints that panel without rebuilding its text or widgets.
    if (pins)
        pins->spinner_phase = g_loading_spinner.phase;
    if (inbox)
        inbox->spinner_phase = g_loading_spinner.phase;
    if (browser)
        browser->spinner_phase = g_loading_spinner.phase;
    if (call)
        call->spinner_phase = g_loading_spinner.phase;
    if (card)
        card->spinner_phase = g_loading_spinner.phase;
    if (guild)
        guild->spinner_phase = g_loading_spinner.phase;
    if (settings)
        settings->spinner_phase = g_loading_spinner.phase;
}

void record_first_present() noexcept {
    if (!g_app.first_present_recorded) {
        g_app.first_present_recorded = true;
        log_u64(LogLevel::info, "startup", "first_present_us",
                clock_microseconds(g_app.clock, g_app.process_start, clock_now()));
        log_u64(LogLevel::info, "render", "first_frame_quads", g_app.renderer.paint_stream.count);
        log_u64(LogLevel::info, "render", "first_frame_draw_calls", g_app.renderer.draw_calls_last_frame);
        log_u64(LogLevel::info, "render", "first_frame_glyph_quads",
                g_app.renderer.glyph_text.title_quad_count);
        log_u64(LogLevel::info, "render", "first_frame_cpu_us", g_app.renderer.first_frame_cpu_us);
    }
}

} // namespace

u64 application_loading_spinner_wake() noexcept {
    return g_loading_spinner.visible ? g_loading_spinner.wake_ms : 0;
}
bool application_loading_spinner_due(u64 now_ms) noexcept {
    return ui_spinner_clock_due(g_loading_spinner, now_ms);
}

namespace {
// The step that rejected the last frame, logged with the failure count.
const char* g_present_rejected_stage = "";
bool present_rejected(const char* stage) noexcept {
    g_present_rejected_stage = stage;
    return false;
}
} // namespace

bool application_present_frame() noexcept {
    if (!g_app.renderer_ready) {
        return true;
    }
    MAICORD_PERF_MARK("frame_begin");
    // Consume a due spinner step first: a rejected draw below must not leave
    // the expired deadline armed (it is re-sampled once scenes are published).
    if (ui_spinner_clock_due(g_loading_spinner, clock_monotonic_milliseconds()))
        ui_spinner_clock_tick(&g_loading_spinner, clock_monotonic_milliseconds(), true,
                              g_app.preferences.reduced_motion);
    // A due entrance must consume its deadline even when later scene/text
    // preparation rejects this draw. Retained permits cannot repeatedly wake
    // against the same past-due opacity sample. Reconcile final scene keys again
    // below, after the surfaces have published their current projections.
    application_surface_motion_synchronize(clock_monotonic_milliseconds());
    // Consume the bounded background deadline before preparing other surfaces.
    // A rejected text/scene publication must not leave its expired visual wake
    // armed and turn retained DXGI permits into a tight retry loop.
    // The selected plugin effect (or the built-in fallback) decides whether
    // the background clock runs.
    application_plugins_fx_synchronize();
    if (!application_background_synchronize(clock_monotonic_milliseconds()))
        return present_rejected("background");
    // Reduced motion, live resize, a minimized or inactive window: hold media
    // animations; resume them once none applies (restore, resize end).
    application_animation_activity_synchronize();
    if (!apply_pending_pane_layout())
        return present_rejected("pane_layout");
    if (!application_settings_synchronize())
        return present_rejected("settings");
    if (!application_theme_editor_synchronize())
        return present_rejected("theme_editor");
    if (!application_theme_editor_publish_foreground())
        return present_rejected("theme_foreground");
    if (!application_friends_synchronize())
        return present_rejected("friends");
    application_self_status_synchronize();
    application_user_settings_synchronize();
    if (!application_account_menu_synchronize())
        return present_rejected("account_menu");
    application_message_forward_tick(clock_monotonic_milliseconds());
    application_quick_switcher_tick(clock_monotonic_milliseconds());
    if (!application_shortcuts_overlay_synchronize() || !application_downloads_synchronize() ||
        !application_message_forward_synchronize() ||
        !application_quick_switcher_synchronize() || !application_search_synchronize() ||
        !application_pinned_messages_synchronize() || !application_inbox_synchronize() ||
        !application_media_picker_synchronize() ||
        !application_thread_browser_synchronize() || !application_thread_creation_synchronize() ||
        !application_forum_post_synchronize() || !application_forum_attachments_synchronize())
        return present_rejected("panels");
    if (!application_image_viewer_synchronize())
        return present_rejected("image_viewer");
    application_gif_favorite_star_synchronize();
    // After search, pins and the thread browser published: they close the call view.
    if (!application_call_synchronize())
        return present_rejected("call");
    application_guild_card_synchronize(clock_monotonic_milliseconds());
    // Banner animation timing (5 s autoplay) and hover; scroll is current here.
    application_guild_banner_project(clock_monotonic_milliseconds());
    if (g_app.login_scene.focus_visible != g_app.keyboard_focus_visible) {
        g_app.login_scene.focus_visible = g_app.keyboard_focus_visible;
        if (g_app.login.visible)
            g_app.renderer.retained_presentation.valid = false;
    }
    if (!application_text_form_synchronize())
        return present_rejected("text_form");
    if (!application_forum_form_synchronize())
        return present_rejected("forum_form");
    if (!application_composer_edit_menu_synchronize())
        return present_rejected("composer_menu");
    if (!application_composer_text_input_synchronize())
        return present_rejected("composer_input");
    const auto* focus = !g_app.keyboard_focus_visible || g_app.login.visible ||
                                (g_app.context_focus == &g_app.message_composer_scene.widgets &&
                                 message_composer_scene_focused_control(g_app.message_composer_scene) ==
                                     MessageComposerControl::edit)
                            ? nullptr
                            : g_app.context_focus;
    renderer_bind_focus_owner(&g_app.renderer, focus);
    application_navigation_drag_tick(clock_monotonic_milliseconds());
    if (!application_guild_folders_synchronize(clock_monotonic_milliseconds()))
        return present_rejected("guild_folders");
    application_guild_invite_synchronize();
    application_invite_cards_synchronize(clock_monotonic_milliseconds());
    application_age_gate_synchronize();
    application_guild_lifecycle_frame();
    application_guild_community_frame();
    // The server header shows its chevron or, while the popout is open, an X.
    application_guild_menu_synchronize();
    // A scrolled timeline retires the hover toolbar before the banner clips it
    // and before the interaction text suffix is refreshed below.
    if (!application_message_hover_observe_scroll())
        return present_rejected("message_hover");
    application_unread_banner_synchronize(clock_monotonic_milliseconds());
    // Every surface has now published its final edit/focus/scroll projection.
    // Refresh a stale retained suffix before the draw guard can suppress it.
    if (!renderer_refresh_interaction_text(&g_app.renderer, application_theme()))
        return present_rejected("interaction_text");
    if (!application_thread_browser_read_blocked())
        synchronize_visible_author_hydration(clock_monotonic_milliseconds());
    application_surface_motion_synchronize(clock_monotonic_milliseconds());
    // After every surface published its scene: sample the spinner phase for
    // this frame and arm (or disarm) the next spinner step.
    synchronize_loading_spinners(clock_monotonic_milliseconds());
    MAICORD_PERF_MARK("frame_surfaces");
    const u64 before =
        static_cast<u64>(g_app.renderer.full_present_draws) + g_app.renderer.partial_present_draws;
    const bool presented = renderer_present(&g_app.renderer, application_theme());
    const u64 after =
        static_cast<u64>(g_app.renderer.full_present_draws) + g_app.renderer.partial_present_draws;
    if (g_app.local_poster_presentation.pending)
        application_media_posters_presented(g_app, presented, before, after);
    if (!presented)
        return present_rejected("renderer");
    if (after != before)
        MAICORD_PERF_FIRST_PRESENT();
    application_composer_text_input_notify_layout();
    if (after != before && !g_app.login.visible && !application_settings_active() &&
        !application_friends_active() && g_app.load_trace.channel_started) {
        auto& trace = g_app.load_trace;
        if (!trace.shell_presented) {
            trace.shell_presented = true;
            application_trace_load_stage("channel_shell_presented");
        }
        if (trace.history_ready && !trace.history_presented) {
            trace.history_presented = true;
            application_trace_load_stage("history_presented");
        }
        if (!trace.members_presented && g_app.member_list_scene.active && g_app.member_list_scene.count) {
            trace.members_presented = true;
            application_trace_load_stage("members_presented");
        }
    }
    // Only the diagnostic title consumes the counter; without it, its two
    // trailing samples would add wakes after every presented burst.
    if (after != before && frame_rate_title_enabled()) {
        frame_rate_presented(g_app.frame_rate,
                             clock_microseconds(g_app.clock, g_app.process_start, clock_now()));
    }
    record_first_present();
    // A click or key may have moved a different target under a still pointer.
    application_cursor_refresh(g_app.window);
    // Screen reader snapshot and events; a flag test until a client connects.
    application_accessibility_frame_presented();
    return true;
}

void application_update_frame_rate(u64 now_us) noexcept {
    if (!frame_rate_update(g_app.frame_rate, now_us) || !g_app.window || !frame_rate_title_enabled())
        return;
    wchar_t title[48] = L"MaiCord | ";
    u32 cursor = 10 + decimal_write(title + 10, g_app.frame_rate.fps);
    for (wchar_t letter : L" FPS")
        title[cursor++] = letter;
    SetWindowTextW(g_app.window, title);
}

void application_present_pending_frame() noexcept {
    if (!g_app.window || !g_app.renderer_ready)
        return;
    // UI changes set frame_pending directly; WM_PAINT acknowledges OS damage.
    // Several worker completions/input events coalesce into this one frame.
    if (!g_app.frame_pending || (g_app.renderer.graphics.frame_latency_waitable &&
                                 !g_app.renderer.live_resize && !g_app.renderer.begin_frame_permit))
        return;
    g_app.frame_pending = false;
    if (!application_present_frame()) {
        application_record_presentation_failure("paint_frame_rejected");
    } else {
        application_record_presentation_success();
    }
}

void application_record_presentation_failure(const char* stage) noexcept {
    saturating_increment(g_app.presentation_failures);
    const u32 count = g_app.presentation_failures;
    if (count == 1u || (count & (count - 1u)) == 0u) {
        log_write(LogLevel::error, "render", stage);
        log_u64(LogLevel::error, "render", "presentation_failures", count);
        // The frame step that rejected (call, panels, renderer, ...).
        if (g_present_rejected_stage[0])
            log_write(LogLevel::error, "render_stage", g_present_rejected_stage);
    }
    // A layout transition (the call's chat split closing, a pane change) can
    // leave a published scene inconsistent until something relayouts it. Try
    // that once on a persistent rejection instead of rejecting every frame.
    if ((count == 4u || count == 64u || count == 1024u) && g_app.renderer_ready) {
        log_u64(LogLevel::warning, "render", "presentation_recovery", count);
        g_app.renderer.retained_presentation.valid = false;
        g_app.pane_layout_pending = true;
        const bool relaid = synchronize_shell_layout() && refresh_account_text(true);
        if (!relaid)
            log_write(LogLevel::warning, "render", "presentation_recovery_failed");
        application_request_frame();
    }
    // An overlay whose frame cannot be built (a paint stream overflow, a scene
    // that will not lay out) would reject every frame for as long as it stays
    // open, freezing the window. Once the relayout above has had its chance,
    // close the transient overlays so the shell presents again.
    if (count == 16u && g_app.renderer_ready) {
        const bool popup = g_app.popup_scene.active;
        const bool profile = g_app.profile && g_app.profile->scene.active;
        if (popup || profile) {
            log_u64(LogLevel::warning, "render", "presentation_recovery_overlays",
                    (popup ? 1u : 0u) | (profile ? 2u : 0u));
            if (popup)
                dismiss_popup();
            if (profile)
                application_profile_close();
            g_app.renderer.retained_presentation.valid = false;
            application_request_frame();
        }
    }
}

void application_record_presentation_success() noexcept {
    g_app.presentation_failures = 0;
}

void application_resize_client(HWND window) noexcept {
    if (!g_app.renderer_ready) {
        return;
    }
    RECT client{};
    if (!GetClientRect(window, &client)) {
        application_record_presentation_failure("resize_client_rect_rejected");
        return;
    }
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    application_resize_surface(static_cast<u32>(width), static_cast<u32>(height));
}

void application_resize_surface(u32 width, u32 height) noexcept {
    if (!g_app.renderer_ready || !width || !height ||
        (g_app.renderer.width == width && g_app.renderer.height == height))
        return;
    popup_scene_close(&g_app.popup_scene);
    application_plugins_menu_dismissed();
    g_app.popup_owner_kind = PopupOwnerKind::none;
    const bool resized =
        renderer_resize(&g_app.renderer, static_cast<u32>(width), static_cast<u32>(height));
    const bool shell_ready = resized && synchronize_shell_layout();
    const bool account_text_ready = shell_ready && refresh_account_text(true);
    const bool login_ready =
        account_text_ready && (!g_app.login.visible || application_synchronize_login_view());
    if (!application_present_resize_projection(login_ready)) {
        application_record_presentation_failure("resize_frame_rejected");
        return;
    }
    application_record_presentation_success();
    secure_text_store_notify_layout(&g_app.login.text_store);
}

} // namespace maicord::app_detail
