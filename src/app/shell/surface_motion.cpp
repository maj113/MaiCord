#include "../chat/inbox.hpp"
#include "../media/downloads.hpp"
#include "surface_motion.hpp"
#include "state.hpp"
#include "window_state.hpp"
#include "../chat/unread_banner.hpp"
#include "../guild/folders.hpp"
#include "maicord/core/deadline.hpp"
#include "../../render/d3d11/renderer_interaction_state.hpp"

namespace maicord::app_detail {
namespace {
struct ObservedSurface {
    ApplicationSurfaceMotionKey key{};
    bool animate{};
};
// Reduced motion is the user's setting or, while it syncs with the system, the system's (application_reduced_motion);
// nothing animates before the system preference is read, while sizing or without a shown window.
bool host_motion_allowed() noexcept {
    return g_app.surface_motion.system_preference_cached && !application_reduced_motion() &&
           !g_app.renderer.live_resize && g_app.renderer_ready && g_app.window && application_window_shown();
}
ObservedSurface observe(RendererLayerSurface surface, bool allowed) noexcept {
    const auto& renderer = g_app.renderer;
    const auto* popup = renderer.popup_scene;
    const bool settings = renderer.settings_scene && renderer.settings_scene->active;
    const bool forum_form = renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active;
    const bool form = forum_form || (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active);
    if (surface == RendererLayerSurface::forum_form) {
        if (!forum_form || renderer.login_visible || settings) return {};
        const auto* scene = renderer.forum_form->scene;
        return {{reinterpret_cast<u64>(scene), g_app.plugin_account_scope_serial, scene->content.owner_serial, 0}, allowed};
    }
    if (surface == RendererLayerSurface::profile) {
        const auto* profile = renderer.profile_scene;
        if (!profile || !profile->active || renderer.login_visible || settings)
            return {};
        return {{reinterpret_cast<u64>(profile), profile->self_id, profile->opens, 0}, allowed && !form};
    }
    if (surface == RendererLayerSurface::theme_editor) {
        if (!renderer_theme_editor_visible(renderer))
            return {};
        return {{reinterpret_cast<u64>(renderer.theme_editor_scene), g_app.settings_lifetime_serial,
                 g_app.theme_editor_lifetime_serial, 0},
                allowed && !form};
    }
    if (surface == RendererLayerSurface::gallery) {
        if (!d3d11_renderer_shows<d3d11_surface_media_picker>(renderer))
            return {};
        const auto* gallery = renderer.media_picker_scene;
        const bool covered =
            (renderer.profile_scene && renderer.profile_scene->active) || (popup && popup->active) ||
            form ||
            (renderer.reaction_viewer && renderer.reaction_viewer->active);
        return {{reinterpret_cast<u64>(gallery), gallery->account_scope, 0, 0}, allowed && !covered};
    }
    if (surface == RendererLayerSurface::popup && application_downloads_active())
        return {{reinterpret_cast<u64>(renderer.download_scene), g_app.plugin_account_scope_serial,
                 g_app.downloads_lifetime_serial, 0x444C4F41u}, allowed && !form};
    // The dialog keeps the layer while its options menu is open over it: the menu does not fade the dialog again.
    if (surface == RendererLayerSurface::popup) {
        const u32 shown = d3d11_renderer_shown<d3d11_surfaces_popup>(renderer);
        if (shown & d3d11_surface_go_live)
            return {{reinterpret_cast<u64>(renderer.go_live_scene), g_app.plugin_account_scope_serial,
                     renderer.go_live_scene->opens, 0x474c4956u}, allowed && !form};
        if (shown & d3d11_surface_message_forward)
            return {{reinterpret_cast<u64>(renderer.message_forward_scene), g_app.plugin_account_scope_serial,
                     g_app.message_forward_lifetime_serial, 0x465744u}, allowed && !form};
        if (shown & d3d11_surface_quick_switcher)
            return {{reinterpret_cast<u64>(renderer.quick_switcher_scene), g_app.plugin_account_scope_serial,
                     g_app.quick_switcher_lifetime_serial, 0x51554943u}, allowed && !form};
        if (shown & d3d11_surface_inbox)
            return {{reinterpret_cast<u64>(renderer.inbox_scene), g_app.plugin_account_scope_serial,
                     application_inbox_opens(), 0x494e4258u}, allowed && !form};
    }
    if (!popup || !popup->active || renderer.login_visible)
        return {};
    const bool choice = popup->kind == PopupKind::settings_choice;
    if ((surface == RendererLayerSurface::settings_choice && (!choice || !settings)) ||
        (surface == RendererLayerSurface::popup && (choice || settings)))
        return {};
    return {{reinterpret_cast<u64>(popup), popup->owner_id, popup->opens, static_cast<u32>(popup->kind)},
            allowed && !form};
}
} // namespace

void application_surface_motion_refresh_system_preference() noexcept {
    auto& owner = g_app.surface_motion;
    owner.client_area_animation = platform_settings_read().client_area_animation;
    owner.system_preference_cached = true;
    if (application_reduced_motion()) {
        for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i) {
            const auto surface = static_cast<RendererLayerSurface>(i);
            auto& slot = owner.slots[i];
            application_surface_motion_update(&owner, surface, slot.key, slot.motion.sampled_ms, false);
            renderer_set_layer_opacity(&g_app.renderer, surface, 1);
            renderer_set_layer_transform(&g_app.renderer, surface, 1, 0, 0, 0, 0);
        }
    }
}

void application_surface_motion_begin_frame(u64 now_ms) noexcept {
    // A due shared-scheduler frame is this one: the transitions still running tick during it and schedule their next
    // frame again. Wakes armed after this point survive the whole frame, however long it takes.
    auto& owner = g_app.surface_motion;
    if (owner.scheduled_ms && now_ms >= owner.scheduled_ms)
        owner.scheduled_ms = 0;
}

void application_surface_motion_synchronize(u64 now_ms) noexcept {
    auto& owner = g_app.surface_motion;
    if (!owner.system_preference_cached)
        application_surface_motion_refresh_system_preference();
    // The system's animation setting stops motion only while reduced motion follows it.
    g_app.renderer.system_animation_off = g_app.member_list_scene.system_animation_off =
        g_app.preferences.reduced_motion_sync && !owner.client_area_animation;
    const bool allowed = host_motion_allowed();
    auto& rail = g_app.guild_rail_scene;
    bool pill_changed = false;
    const bool rail_visible = rail.active && !g_app.renderer.login_visible &&
                              !(g_app.renderer.settings_scene && g_app.renderer.settings_scene->active);
    if (rail_visible) {
        const auto* home = ui_scene_node(g_app.shell_scene, UiSemantic::home_button);
        const bool home_hovered = home && ui_widget_handle_equal(ui_scene_widget(g_app.shell_scene, UiSemantic::home_button), g_app.shell_scene.widgets.hovered);
        pill_changed = guild_rail_indicator_tick(&rail.home_indicator, {~u64{0}, 1},
                           g_app.renderer.navigation_view && !g_app.renderer.navigation_view->selected_guild_id,
                           home_hovered, false, now_ms, allowed);
    }
    for (u32 i = 0; i < rail.count; ++i) {
        auto& row = rail.rows[i];
        const bool hovered = ui_widget_handle_equal(row.widget, rail.widgets.hovered);
        // An expanded folder header hands selection and unread pills to its members.
        const bool open_folder = row.view.folder_role == GuildFolderRole::header && row.view.folder_expanded;
        pill_changed = guild_rail_indicator_tick(&row.indicator, {row.view.id, maximum<u64>(1, row.view.generation)},
                           !row.view.unavailable && row.view.selected && !open_folder, !row.view.unavailable && hovered,
                           !row.view.unavailable && row.view.unread && !open_folder, now_ms,
                           allowed && rail_visible) || pill_changed;
    }
    if (pill_changed) g_app.renderer.retained_presentation.valid = false;
    application_surface_motion_owner_initialize(&owner);
    // A due shared-scheduler wake is consumed at the frame's start (application_surface_motion_begin_frame), never here:
    // this runs again once the scenes published, and a wake a ticker armed during the frame is that ticker's next frame.
    for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i) {
        const auto surface = static_cast<RendererLayerSurface>(i);
        const auto current = observe(surface, allowed);
        auto& slot = owner.slots[i];
        // Entrances follow the theme's fast motion token, except the official client's own animations: the server
        // header popout scales from its top centre, other popouts slide toward their anchor, tooltips pop on their
        // spring, and the full profile is a modal (scale and fade in, the same back out on its exit spring).
        const bool popup = surface == RendererLayerSurface::popup && current.key.scene;
        const bool scale_popout = popup && g_app.shell_scene.guild_menu_expanded;
        const bool popover = popup && !scale_popout && current.key.kind == static_cast<u32>(PopupKind::popover);
        const bool tooltip = popup && current.key.kind == static_cast<u32>(PopupKind::tooltip);
        const auto* profile = g_app.renderer.profile_scene;
        const bool modal = surface == RendererLayerSurface::profile && profile &&
                           profile->presentation == ProfilePresentation::modal;
        slot.style = slot.close     ? kUiMotionModalExit
                     : modal        ? kUiMotionModalEnter
                     : scale_popout ? kUiMotionPopoutScale
                     : popover      ? kUiMotionPopout
                     : tooltip      ? kUiMotionTooltip
                                    : application_theme().motion.fast;
        if (slot.close && !current.key.scene) {
            // Covered or hidden while exiting (another surface took over): the owner closes now.
            auto* close = slot.close;
            application_surface_motion_retire(surface);
            close();
            continue;
        }
        const float value =
            application_surface_motion_update(&owner, surface, current.key, now_ms, current.animate);
        if (slot.close && value <= 0 && !slot.motion.active) {
            // The exit finished: the owner closes for real.
            auto* close = slot.close;
            application_surface_motion_retire(surface);
            close();
            continue;
        }
        // A profile popout slides in without a fade (application_profile).
        const bool slide = surface == RendererLayerSurface::profile && profile &&
                           profile->presentation == ProfilePresentation::popout;
        UiMotionPose pose = ui_motion_fade(slide ? 1.f : value);
        UiRect rectangle{};
        if (scale_popout || popover || tooltip)
            rectangle = g_app.popup_scene.rectangle;
        else if (modal)
            rectangle = profile->rectangle;
        float origin_y = (rectangle.top + rectangle.bottom) * .5f;
        if (scale_popout) {
            pose = ui_motion_scale(value, kUiMotionPopoutScaleFrom);
            origin_y = rectangle.top;
        } else if (popover) {
            pose = ui_motion_slide(value, ui_motion_edge(g_app.popup_scene.anchor, rectangle),
                                   kUiMotionPopoutDistance, false);
        } else if (tooltip) {
            pose = ui_motion_scale(value, kUiMotionTooltipScale);
        } else if (modal) {
            pose = ui_motion_scale(value, kUiMotionModalScale);
        }
        renderer_set_layer_opacity(&g_app.renderer, surface, pose.opacity);
        renderer_set_layer_transform(&g_app.renderer, surface, pose.scale, (rectangle.left + rectangle.right) * .5f,
                                     origin_y, pose.x, pose.y);
    }
}

u64 application_surface_motion_next_wake() noexcept {
    const bool allowed = host_motion_allowed();
    if (!allowed)
        return 0;
    u64 wake = earliest_deadline(application_unread_banner_next_wake(),
                                 application_guild_folders_next_wake());
    wake = earliest_deadline(wake, g_app.surface_motion.scheduled_ms);
    const auto& rail = g_app.guild_rail_scene;
    if (rail.active && !g_app.renderer.login_visible &&
        !(g_app.renderer.settings_scene && g_app.renderer.settings_scene->active)) {
        wake = earliest_deadline(wake, guild_rail_indicator_next_wake(rail.home_indicator));
        for (u32 i = 0; i < rail.count; ++i)
            wake = earliest_deadline(wake, guild_rail_indicator_next_wake(rail.rows[i].indicator));
    }
    for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i) {
        const auto surface = static_cast<RendererLayerSurface>(i);
        const auto current = observe(surface, allowed);
        if (!current.animate)
            continue;
        wake = earliest_deadline(
            wake, application_surface_motion_owner_next_wake(g_app.surface_motion, surface, current.key));
    }
    return wake;
}

bool application_surface_motion_request_frame_if_due(u64 now_ms, bool frame_available) noexcept {
    if (!frame_available)
        return false;
    const u64 wake = application_surface_motion_next_wake();
    if (!wake || now_ms < wake)
        return false;
    application_request_frame();
    return true;
}

void application_surface_motion_retire(RendererLayerSurface surface) noexcept {
    application_surface_motion_owner_retire(&g_app.surface_motion, surface);
    renderer_set_layer_opacity(&g_app.renderer, surface, 1);
    renderer_set_layer_transform(&g_app.renderer, surface, 1, 0, 0, 0, 0);
}

bool application_surface_motion_exit(RendererLayerSurface surface, void (*close)() noexcept) noexcept {
    if (static_cast<u32>(surface) >= kApplicationSurfaceMotionCount || !close)
        return false;
    auto& slot = g_app.surface_motion.slots[static_cast<u32>(surface)];
    if (slot.close)
        return true;
    const auto current = observe(surface, host_motion_allowed());
    if (!current.animate || !current.key.scene || !application_surface_motion_same_key(slot.key, current.key) ||
        slot.motion.value <= 0)
        return false;
    slot.close = close;
    application_request_frame();
    return true;
}

void application_surface_motion_finish_exits() noexcept {
    for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i) {
        auto* close = g_app.surface_motion.slots[i].close;
        if (!close)
            continue;
        application_surface_motion_retire(static_cast<RendererLayerSurface>(i));
        close();
    }
}

void application_motion_schedule(u64 deadline_ms) noexcept {
    auto& owner = g_app.surface_motion;
    owner.scheduled_ms = earliest_deadline(owner.scheduled_ms, deadline_ms);
}

float application_motion_tick(UiMotionTransition* transition, const UiMotionScope& scope, float target, u64 now_ms,
                              const UiMotionStyle& style) noexcept {
    if (!transition->scope.owner)
        ui_motion_transition_reset(transition, scope, target);
    const bool animate = host_motion_allowed();
    ui_motion_transition_tick(transition, scope, target, now_ms, style, !animate);
    application_motion_schedule(ui_motion_transition_next_wake(*transition, scope, !animate));
    return transition->value;
}

bool application_motion_hover_tick(UiMotionHover* hover, u64 hovered_id, u64 now_ms,
                                   const UiMotionStyle& style) noexcept {
    const bool changed = ui_motion_hover_tick(hover, hovered_id, now_ms, style, !host_motion_allowed());
    if (hover)
        application_motion_schedule(ui_motion_hover_next_wake(*hover));
    return changed;
}

} // namespace maicord::app_detail
