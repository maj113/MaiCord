#include "../guild/community.hpp"
#include "../guild/menu.hpp"
#include "../chat/messages/components.hpp"
#include "../guild/invite_cards.hpp"
#include "../guild/lifecycle.hpp"
#include "navigation_drag.hpp"
#include "shortcuts.hpp"
#include "../accessibility/accessibility.hpp"
#include "../voice/call.hpp"
#include "../media/video.hpp"
#include "../voice/stream.hpp"
#include "../voice/go_live.hpp"
#include "../guild/card.hpp"
#include "../media/expression_card.hpp"
#include "../guild/banner.hpp"
#include "../chat/inbox.hpp"
#include "cursor.hpp"
#include "../guild/invite.hpp"
#include "../guild/folders.hpp"
#include "../settings/user_settings.hpp"
#include "../chat/unread_banner.hpp"
#include "toast.hpp"
#include "../media/downloads.hpp"
#include "../media/gif_favorite_star.hpp"
#include "../media/media_page.hpp"
#include "load_trace.hpp"
#include "perf_mark.hpp"
#if defined(MAICORD_STARTUP_PROBE)
#include "shutdown_metrics.hpp"
#endif
#include "caption.hpp"
#include "../update/updates.hpp"
#include "frame_presentation.hpp"
#include "../media/media.hpp"
#include "background.hpp"
#include "maicord/core/data_path.hpp"
#include "window_resize.hpp"
#include "window_state.hpp"

#include "../account/account_presentation.hpp"
#include "../account/account_text_retry.hpp"
#include "../account/login/ui.hpp"
#include "state.hpp"
#include "input.hpp"
#include "../chat/composer/text_form.hpp"
#include "../chat/composer/text_input.hpp"
#include "../chat/composer/edit_menu.hpp"
#include "../settings/settings.hpp"
#include "../settings/modal.hpp"
#include "content_input.hpp"
#include "../settings/theme_editor.hpp"
#include "../store/store_host.hpp"
#include "../account/friends.hpp"
#include "../account/account_menu.hpp"
#include "../chat/search/search.hpp"
#include "../chat/messages/pinned_messages.hpp"
#include "../chat/threads/browser.hpp"
#include "../chat/threads/creation.hpp"
#include "../chat/threads/forum_post.hpp"
#include "../chat/threads/forum_form.hpp"
#include "../chat/threads/forum_attachments.hpp"
#include "../media/picker.hpp"
#include "quick_switcher.hpp"
#include "../chat/messages/forward.hpp"
#include "../account/self_status.hpp"
#include "../plugins/plugins.hpp"
#include "surface_motion.hpp"
#include "../account/application_account_module.hpp"
#include "../chat/messages/interaction.hpp"
#include "../account/profile.hpp"
#include "../account/dm_profile.hpp"

#include "maicord/core/log_event.hpp"
#include "memory_summary.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/text/secure_text_store.hpp"
#include "maicord/ui/widgets/spinner.hpp"
#include "maicord/ui/chat/messages/typing_indicator.hpp"
#include "../../render/d3d11/renderer_interaction_state.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/gpu/api.hpp"


namespace maicord::app_detail {
namespace {

// The FPS counter in the window title is a diagnostic; it shows only when MAICORD_FPS_TITLE=1.
bool frame_rate_title_enabled() noexcept {
    static u32 cached = 0; // 0 unknown, 1 off, 2 on
    if (cached == 0) {
        wchar_t value[2]{};
        const u32 length = environment_variable(L"MAICORD_FPS_TITLE", value, 2);
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
    // paint-only field the application frame clock owns. Each entry is the
    // phase of a panel whose spinner is on screen, null otherwise.
    const auto phase = [](auto* scene) noexcept -> u8* { return scene ? &scene->spinner_phase : nullptr; };
    const u32 surfaces =
        d3d11_renderer_shown<d3d11_surface_pinned_messages | d3d11_surface_inbox | d3d11_surface_thread_browser |
                             d3d11_surface_call | d3d11_surface_stream_card | d3d11_surface_guild_card |
                             d3d11_surface_settings_page | d3d11_surface_media_picker>(renderer);
    u8* const phases[]{
        phase((surfaces & d3d11_surface_pinned_messages) &&
                      ui_pinned_messages_scene_spinner_visible(*renderer.pinned_messages_scene)
                  ? const_cast<UiPinnedMessagesScene*>(renderer.pinned_messages_scene)
                  : nullptr),
        phase((surfaces & d3d11_surface_inbox) && ui_inbox_scene_spinner_visible(*renderer.inbox_scene)
                  ? const_cast<UiInboxScene*>(renderer.inbox_scene)
                  : nullptr),
        phase((surfaces & d3d11_surface_thread_browser) &&
                      ui_thread_browser_scene_spinner_visible(*renderer.thread_browser_scene)
                  ? renderer.thread_browser_scene
                  : nullptr),
        // Stream tiles (connecting, reconnecting) and the stream card's thumbnail.
        phase((surfaces & d3d11_surface_call) && ui_call_scene_spinner_visible(*renderer.call_scene)
                  ? const_cast<UiCallScene*>(renderer.call_scene)
                  : nullptr),
        phase((surfaces & d3d11_surface_stream_card) ? application_stream_card_spinner_scene() : nullptr),
        phase((surfaces & d3d11_surface_guild_card) ? application_guild_card_spinner_scene() : nullptr),
        // Settings pages' loading notices.
        phase((surfaces & d3d11_surface_settings_page) &&
                      ui_settings_scene_spinner_visible(*renderer.settings_scene)
                  ? const_cast<UiSettingsScene*>(renderer.settings_scene)
                  : nullptr),
        // The Plugins, Themes and Effects pages' busy buttons and the install dialog's check.
        phase((surfaces & d3d11_surface_settings_page) ? application_store_spinner_scene() : nullptr),
        // The media viewer while neither of its image tiers is resident.
        phase(ui_image_viewer_spinner_visible(g_app.image_viewer.scene) ? &g_app.image_viewer.scene : nullptr),
        // A media page while its picture loads.
        phase(application_media_page_spinner()),
        // The composer's inline /gif results while their search loads.
        phase((surfaces & d3d11_surface_media_picker) &&
                      ui_media_picker_scene_spinner_visible(*renderer.media_picker_scene)
                  ? const_cast<UiMediaPickerScene*>(renderer.media_picker_scene)
                  : nullptr),
    };
    bool shown = false;
    for (const u8* at : phases)
        shown = shown || at;
    ui_spinner_clock_tick(&g_loading_spinner, now_ms, g_app.renderer_ready && !renderer.live_resize && shown,
                          application_reduced_motion());
    // The phase is part of each panel's presentation stamp, so a new phase
    // repaints that panel without rebuilding its text or widgets.
    for (u8* at : phases)
        if (at)
            *at = g_loading_spinner.phase;
}

UiTypingDotsClock g_typing_dots{};

// As the official client: an inactive window holds the dots at full size.
bool typing_dots_animate() noexcept {
    return !application_reduced_motion() && !application_window_inactive() && !application_window_minimized();
}
// The "is typing" dots step only while someone types in the shown chat; the
// renderer patches the sampled phase into its retained stream.
void synchronize_typing_dots(u64 now_ms) noexcept {
    auto& renderer = g_app.renderer;
    auto& scene = g_app.message_composer_scene;
    const bool visible = g_app.renderer_ready && !renderer.live_resize && !d3d11_renderer_page_open(renderer) &&
                         renderer.message_composer_scene == &scene && scene.active && scene.typing_dots &&
                         scene.typing_label_bytes && !application_friends_active() &&
                         !d3d11_renderer_shows<d3d11_surface_thread_browser>(renderer);
    ui_typing_dots_clock_tick(&g_typing_dots, now_ms, visible, typing_dots_animate());
    scene.typing_phase = g_typing_dots.phase;
}

UiTypingDotsClock g_component_dots{};
// A pending message component's loading dots step only while its row is on
// screen; otherwise they rest and the clock disarms.
void synchronize_component_dots(u64 now_ms) noexcept {
    auto& renderer = g_app.renderer;
    auto& scene = g_app.message_scene;
    const bool on_screen = g_app.renderer_ready && !renderer.live_resize && !d3d11_renderer_page_open(renderer) &&
                           renderer.message_scene == &scene;
    bool visible = false;
    if (on_screen && scene.interaction.pending)
        for (u32 i = 0; i < scene.count && !visible; ++i) {
            const auto& row = scene.rows[i];
            const auto* node = row.message_id == scene.interaction.message_id
                                   ? message_scene_layout_node(scene, row.widget)
                                   : nullptr;
            visible = node && ui_rect_intersects(node->rectangle, scene.viewport);
        }
    // A local command message's or deferred response's status dots on screen
    // step on the same clock.
    if (!visible && on_screen && g_app.account_ui && g_app.account_ui->message_text.status_dots) {
        const auto& text = g_app.account_ui->message_text;
        for (u32 i = 0; i < text.command_count && !visible; ++i) {
            const auto& command = text.commands[i];
            for (u32 s = 0; s < command.semantics.count && !visible; ++s)
                visible = command.semantics[s].kind == MessageMarkdownSemanticKind::chip_icon &&
                          command.semantics[s].target == static_cast<u64>(MessageMarkdownChipIcon::pending_dots) &&
                          ui_rect_intersects(command.rectangle, scene.viewport);
        }
    }
    ui_typing_dots_clock_tick(&g_component_dots, now_ms, visible, typing_dots_animate());
    scene.interaction_phase = g_component_dots.phase;
}

void record_first_present() noexcept {
    if (!g_app.first_present_recorded) {
        g_app.first_present_recorded = true;
        application_updates_ready();
        const u64 first[] = {application_now_us(), g_app.renderer.paint_stream.count,
                             g_app.renderer.draw_calls_last_frame, g_app.renderer.glyph_text.title_quad_count,
                             g_app.renderer.first_frame_cpu_us};
        LogEvent("first_present").fields("us\0quads\0draw_calls\0glyph_quads\0cpu_us", first).write(LogLevel::info, "startup");
        application_memory_summary("startup");
    }
}

} // namespace

u64 application_loading_spinner_wake() noexcept {
    return earliest_deadline(earliest_deadline(g_loading_spinner.visible ? g_loading_spinner.wake_ms : 0,
                                               g_typing_dots.visible ? g_typing_dots.wake_ms : 0),
                             g_component_dots.visible ? g_component_dots.wake_ms : 0);
}
bool application_loading_spinner_due(u64 now_ms) noexcept {
    return ui_spinner_clock_due(g_loading_spinner, now_ms) || ui_typing_dots_clock_due(g_typing_dots, now_ms) ||
           ui_typing_dots_clock_due(g_component_dots, now_ms);
}

namespace {
// Re-run WM_SETCURSOR after a frame changed the scene under a still pointer.
inline void application_cursor_refresh(Window window) noexcept {
    if (!g_application_cursor_refresh)
        return;
    g_application_cursor_refresh = false;
    window_request_cursor_refresh(window);
}

// The step that rejected the last frame, logged with the failure count.
const char* g_present_rejected_stage;
bool present_rejected(const char* stage) noexcept {
    g_present_rejected_stage = stage;
    return false;
}

// The GPU device was lost (a reset, a driver update, a removed adapter): the renderer rebuilds itself on a new
// one, the loop rebuilds text for the empty atlas and media owners request their images again. A failed attempt
// waits a second; the next requested frame tries again. Rare, so compiled for size with the rebuild it inlines.
__attribute__((minsize)) bool application_recover_device() noexcept {
    static u64 retry_ms = 0;
    const u64 now = clock_monotonic_milliseconds();
    if (now < retry_ms)
        return false;
    if (!renderer_recover_device(&g_app.renderer)) {
        retry_ms = now + 1000;
        return false;
    }
    retry_ms = 0;
    application_request_frame();
    return true;
}
} // namespace

bool application_present_frame() noexcept {
    if (!g_app.renderer_ready) {
        return true;
    }
    // A lost device waits for its recovery before anything draws.
    if (g_app.renderer.device_lost && !application_recover_device())
        return present_rejected("device_lost");
    MAICORD_PERF_MARK("frame_begin");
    // The clock is sampled once per stage of the frame: before the scenes publish, before the panels do, before the
    // shell's own surfaces do and once they all have. The shared motion scheduler's due wake is consumed first, so
    // every tick of this frame (the caption's included) arms the next one, and only the next frame's start drops it.
    u64 now_ms = clock_monotonic_milliseconds();
    application_surface_motion_begin_frame(now_ms);
    application_caption_synchronize();
    // Consume a due spinner step first: a rejected draw below must not leave
    // the expired deadline armed (it is re-sampled once scenes are published).
    if (ui_spinner_clock_due(g_loading_spinner, now_ms))
        ui_spinner_clock_tick(&g_loading_spinner, now_ms, true, application_reduced_motion());
    if (ui_typing_dots_clock_due(g_typing_dots, now_ms))
        ui_typing_dots_clock_tick(&g_typing_dots, now_ms, true, typing_dots_animate());
    if (ui_typing_dots_clock_due(g_component_dots, now_ms))
        ui_typing_dots_clock_tick(&g_component_dots, now_ms, true, typing_dots_animate());
    // A due entrance must consume its deadline even when later scene/text
    // preparation rejects this draw. Retained permits cannot repeatedly wake
    // against the same past-due opacity sample. Reconcile final scene keys again
    // below, after the surfaces have published their current projections.
    application_surface_motion_synchronize(now_ms);
    // A closing settings dialog fades and shrinks out (settings_modal_close).
    settings_modal_exit_synchronize(now_ms);
    // Consume the bounded background deadline before preparing other surfaces.
    // A rejected text/scene publication must not leave its expired visual wake
    // armed and turn retained DXGI permits into a tight retry loop.
    // The selected package effect (or the built-in fallback) decides whether the background clock runs.
    application_effects_synchronize();
    if (!application_background_synchronize(now_ms))
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
    application_dm_profile_synchronize();
    application_profile_synchronize();
    application_self_status_synchronize();
    application_user_settings_synchronize();
    if (!application_account_menu_synchronize())
        return present_rejected("account_menu");
    now_ms = clock_monotonic_milliseconds();
    application_message_forward_tick(now_ms);
    application_quick_switcher_tick(now_ms);
    application_shortcuts_overlay_synchronize();
    if (!application_downloads_synchronize() ||
        !application_go_live_modal_synchronize() ||
        !application_message_forward_synchronize() ||
        !application_quick_switcher_synchronize() || !application_search_synchronize() ||
        !application_pinned_messages_synchronize() || !application_inbox_synchronize() ||
        !application_media_picker_synchronize() ||
        !application_thread_browser_synchronize() || !application_thread_creation_synchronize() ||
        !application_forum_post_synchronize() || !application_forum_attachments_synchronize())
        return present_rejected("panels");
    if (!application_image_viewer_synchronize())
        return present_rejected("image_viewer");
    if (!application_media_page_synchronize())
        return present_rejected("media_page");
    application_gif_favorite_star_synchronize();
    // After search, pins and the thread browser published: they close the call view.
    if (!application_call_synchronize())
        return present_rejected("call");
    if (g_app.renderer.live_resize && !application_video_synchronize())
        return present_rejected("video_geometry");
    application_guild_card_synchronize(now_ms);
    application_expression_card_synchronize(now_ms);
    // Banner animation timing (5 s autoplay) and hover; scroll is current here.
    application_guild_banner_project(now_ms);
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
    now_ms = clock_monotonic_milliseconds();
    application_navigation_drag_tick(now_ms);
    application_navigation_chevron_synchronize(now_ms);
    if (!application_guild_folders_synchronize(now_ms))
        return present_rejected("guild_folders");
    application_guild_invite_synchronize();
    application_invite_cards_synchronize(now_ms);
    application_age_gate_synchronize();
    application_guild_lifecycle_frame();
    application_guild_community_frame();
    // The server header shows its chevron or, while the popout is open, an X.
    application_guild_menu_synchronize();
    // A scrolled timeline retires the hover toolbar before the banner clips it
    // and before the interaction text suffix is refreshed below.
    if (!application_message_hover_observe_scroll())
        return present_rejected("message_hover");
    application_unread_banner_synchronize(now_ms);
    // A modal, popup or panel that opened over the picture under a still pointer ends the magnifier.
    application_image_zoom_synchronize();
    application_toast_synchronize(now_ms);
    // Every surface has now published its final edit/focus/scroll projection.
    // Refresh a stale retained suffix before the draw guard can suppress it.
    if (!renderer_refresh_interaction_text(&g_app.renderer, application_theme()))
        return present_rejected("interaction_text");
    now_ms = clock_monotonic_milliseconds();
    if (!application_thread_browser_read_blocked())
        synchronize_visible_author_hydration(now_ms);
    application_surface_motion_synchronize(now_ms);
    // After every surface published its scene: sample the spinner phase for
    // this frame and arm (or disarm) the next spinner step.
    synchronize_loading_spinners(now_ms);
    synchronize_typing_dots(now_ms);
    synchronize_component_dots(now_ms);
    MAICORD_PERF_MARK("frame_surfaces");
    const u64 before =
        static_cast<u64>(g_app.renderer.full_present_draws) + g_app.renderer.partial_present_draws;
    const bool presented = renderer_present(&g_app.renderer, application_theme());
    const u64 after =
        static_cast<u64>(g_app.renderer.full_present_draws) + g_app.renderer.partial_present_draws;
#if defined(MAICORD_STARTUP_PROBE)
    if (presented && after != before)
        application_startup_probe_first_present();
#endif
    if (g_app.local_poster_presentation.pending)
        application_media_posters_presented(g_app, presented, before, after);
    if (!presented) {
        // The GPU was removed or reset during this frame: rebuild on a new device and draw again.
        if (renderer_device_lost(&g_app.renderer)) {
            application_recover_device();
            return true;
        }
        return present_rejected("renderer");
    }
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
        frame_rate_presented(g_app.frame_rate, application_now_us());
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
    char title[48] = "MaiCord | ";
    u32 cursor = 10 + decimal_write(title + 10, g_app.frame_rate.fps);
    for (char letter : " FPS")
        title[cursor++] = letter;
    window_set_title(g_app.window, {title, cursor - 1});
}

void application_present_pending_frame() noexcept {
    if (!g_app.window || !g_app.renderer_ready)
        return;
    // UI changes set frame_pending directly; WM_PAINT acknowledges OS damage.
    // Several worker completions/input events coalesce into this one frame.
    if (!g_app.frame_pending || (gpu_frame_clock_waitable(&g_app.renderer.graphics.surface) &&
                                 !g_app.renderer.live_resize && !g_app.renderer.begin_frame_permit))
        return;
    // Pointer drags that move a pane follow the pointer once per frame here,
    // not once per WM_MOUSEMOVE.
    application_media_picker_drag_tick();
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
        LogEvent line(stage);
        line.field("presentation_failures", count);
        // The frame step that rejected (call, panels, renderer, ...).
        if (g_present_rejected_stage)
            line.field("stage", g_present_rejected_stage);
        line.write(LogLevel::error, "render");
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
            application_repaint();
        }
    }
}

void application_record_presentation_success() noexcept {
    g_app.presentation_failures = 0;
}

void application_resize_client() noexcept {
    if (!g_app.renderer_ready) {
        return;
    }
    const LONG width = g_window_state.client_width;
    const LONG height = g_window_state.client_height - static_cast<LONG>(g_app.renderer.caption_height);
    if (width <= 0 || height <= 0) {
        return;
    }
    application_resize_surface(static_cast<u32>(width), static_cast<u32>(height));
}

void application_resize_surface(u32 width, u32 height) noexcept {
    // A caption height change (DPI, fullscreen) keeps the content size but
    // needs a taller or shorter target.
    if (!g_app.renderer_ready || !width || !height ||
        (g_app.renderer.width == width && g_app.renderer.height == height &&
         gpu_surface_height(&g_app.renderer.graphics.surface) >= height + g_app.renderer.caption_height))
        return;
    popup_scene_close(&g_app.popup_scene);
    application_plugins_menu_dismissed();
    g_app.popup_owner_kind = PopupOwnerKind::none;
    const bool resized = renderer_resize(&g_app.renderer, width, height);
    const bool shell_ready = resized && synchronize_shell_layout();
    const bool account_text_ready = shell_ready && refresh_account_text_for_layout();
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
