#include "application_inbox.hpp"
#include "application_downloads.hpp"
#include "application_surface_motion.hpp"

#include <Windows.h>

#if !defined(MAICORD_SURFACE_MOTION_CONTROLLER_TEST)
#include "application_state.hpp"
#include "application_unread_banner.hpp"
#include "application_guild_folders.hpp"
#include "../render/d3d11_renderer_interaction_state.hpp"
#endif

namespace maicord::app_detail {
namespace {
bool valid_surface(RendererLayerSurface surface) noexcept {
    return static_cast<u32>(surface) < kApplicationSurfaceMotionCount;
}
bool valid_key(RendererLayerSurface surface, const ApplicationSurfaceMotionKey& key) noexcept {
    return key.scene && (surface == RendererLayerSurface::gallery ? key.owner != 0 : key.lifetime != 0);
}
bool same_key(const ApplicationSurfaceMotionKey& a, const ApplicationSurfaceMotionKey& b) noexcept {
    return a.scene == b.scene && a.owner == b.owner && a.lifetime == b.lifetime && a.kind == b.kind;
}
bool stale_key(RendererLayerSurface surface, const ApplicationSurfaceMotionKey& previous,
               const ApplicationSurfaceMotionKey& key) noexcept {
    if (!previous.scene)
        return false;
    if (surface == RendererLayerSurface::gallery)
        return key.owner < previous.owner;
    return key.scene == previous.scene && key.lifetime < previous.lifetime;
}
} // namespace

void application_surface_motion_owner_initialize(ApplicationSurfaceMotionOwner* owner) noexcept {
    if (!owner || owner->styles_initialized)
        return;
    for (auto& slot : owner->slots)
        slot.style = kUiDefaultMotionStyle;
    owner->styles_initialized = true;
}

void application_surface_motion_owner_retire(ApplicationSurfaceMotionOwner* owner,
                                             RendererLayerSurface surface) noexcept {
    if (!owner || !valid_surface(surface))
        return;
    auto& slot = owner->slots[static_cast<u32>(surface)];
    ui_motion_transition_cancel(&slot.motion);
    slot.key = {};
}

float application_surface_motion_update(ApplicationSurfaceMotionOwner* owner, RendererLayerSurface surface,
                                        ApplicationSurfaceMotionKey key, u64 now_ms,
                                        bool animate) noexcept {
    if (!owner || !valid_surface(surface))
        return 1;
    application_surface_motion_owner_initialize(owner);
    auto& slot = owner->slots[static_cast<u32>(surface)];
    if (!valid_key(surface, key)) {
        application_surface_motion_owner_retire(owner, surface);
        return 1;
    }
    if (stale_key(surface, slot.key, key))
        return slot.motion.value;
    if (!ui_motion_style_valid(slot.style))
        slot.style = kUiDefaultMotionStyle;
    if (!same_key(slot.key, key)) {
        const bool successor = slot.key.scene != 0;
        slot.key = key;
        // Account/source replacement is not an opening animation. Popup opens
        // supply their own lifetime, so their entrance can start.
        const bool entrance = animate && (!successor || surface != RendererLayerSurface::gallery);
        if (owner->scope_serial == ~u64{0}) {
            ui_motion_transition_cancel(&slot.motion);
            slot.motion.value = 1;
            return 1;
        }
        const UiMotionScope scope{static_cast<u64>(surface) + 1, ++owner->scope_serial};
        ui_motion_transition_reset(&slot.motion, scope, entrance ? 0.0f : 1.0f);
    }
    ui_motion_transition_tick(&slot.motion, slot.motion.scope, 1, now_ms, slot.style, !animate, true);
    return slot.motion.value;
}

u64 application_surface_motion_owner_next_wake(const ApplicationSurfaceMotionOwner& owner,
                                               RendererLayerSurface surface,
                                               ApplicationSurfaceMotionKey key, u64 now_ms) noexcept {
    if (!valid_surface(surface) || !valid_key(surface, key))
        return 0;
    const auto& slot = owner.slots[static_cast<u32>(surface)];
    return same_key(slot.key, key)
               ? ui_motion_transition_next_wake(slot.motion, slot.motion.scope, now_ms, false, true)
               : 0;
}

#if !defined(MAICORD_SURFACE_MOTION_CONTROLLER_TEST)
namespace {
struct ObservedSurface {
    ApplicationSurfaceMotionKey key{};
    bool animate{};
};
bool host_motion_allowed() noexcept {
    const auto& owner = g_app.surface_motion;
    return owner.system_preference_cached && owner.client_area_animation &&
           !g_app.preferences.reduced_motion && !g_app.renderer.reduced_motion &&
           !g_app.renderer.live_resize && g_app.renderer_ready && g_app.window &&
           IsWindowVisible(g_app.window) && !IsIconic(g_app.window);
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
    if (surface == RendererLayerSurface::settings_page) {
        if (!renderer_settings_page_visible(renderer) || renderer.login_visible)
            return {};
        const auto* page = renderer.settings_scene;
        return {{reinterpret_cast<u64>(page), g_app.settings_lifetime_serial, page->page_generation, 0},
                allowed && !form};
    }
    if (surface == RendererLayerSurface::theme_editor) {
        if (!renderer_theme_editor_visible(renderer))
            return {};
        return {{reinterpret_cast<u64>(renderer.theme_editor_scene), g_app.settings_lifetime_serial,
                 g_app.theme_editor_lifetime_serial, 0},
                allowed && !form};
    }
    if (surface == RendererLayerSurface::gallery) {
        if (!d3d11_renderer_media_picker_visible(renderer))
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
    if (surface == RendererLayerSurface::popup && d3d11_renderer_message_forward_visible(renderer))
        return {{reinterpret_cast<u64>(renderer.message_forward_scene), g_app.plugin_account_scope_serial,
                 g_app.message_forward_lifetime_serial, 0x465744u}, allowed && !form};
    if (surface == RendererLayerSurface::popup && d3d11_renderer_quick_switcher_visible(renderer))
        return {{reinterpret_cast<u64>(renderer.quick_switcher_scene), g_app.plugin_account_scope_serial,
                 g_app.quick_switcher_lifetime_serial, 0x51554943u}, allowed && !form};
    if (surface == RendererLayerSurface::popup && d3d11_renderer_inbox_visible(renderer))
        return {{reinterpret_cast<u64>(renderer.inbox_scene), g_app.plugin_account_scope_serial,
                 application_inbox_opens(), 0x494e4258u}, allowed && !form};
    if (!popup || !popup->active || renderer.login_visible)
        return {};
    const bool choice = popup->kind == PopupKind::settings_choice;
    if ((surface == RendererLayerSurface::settings_choice && (!choice || !settings)) ||
        (surface == RendererLayerSurface::popup && (choice || settings)))
        return {};
    const bool covered = form;
    return {{reinterpret_cast<u64>(popup), popup->owner_id, popup->opens, static_cast<u32>(popup->kind)},
            allowed && !covered};
}
} // namespace

void application_surface_motion_refresh_system_preference() noexcept {
    BOOL enabled = FALSE;
    const bool queried = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) != FALSE;
    auto& owner = g_app.surface_motion;
    owner.client_area_animation = queried && enabled;
    owner.system_preference_cached = true;
    if (!owner.client_area_animation) {
        for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i) {
            const auto surface = static_cast<RendererLayerSurface>(i);
            auto& slot = owner.slots[i];
            application_surface_motion_update(&owner, surface, slot.key, slot.motion.sampled_ms, false);
            renderer_set_layer_opacity(&g_app.renderer, surface, 1);
        }
    }
}

void application_surface_motion_synchronize(u64 now_ms) noexcept {
    auto& owner = g_app.surface_motion;
    if (!owner.system_preference_cached)
        application_surface_motion_refresh_system_preference();
    const bool allowed = host_motion_allowed();
    auto& rail = g_app.guild_rail_scene;
    bool pill_changed = false;
    const bool rail_visible = rail.active && !g_app.renderer.login_visible &&
                              !(g_app.renderer.settings_scene && g_app.renderer.settings_scene->active);
    if (rail_visible) {
        const auto* home = ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::home_button));
        const bool home_hovered = home && home->widget.slot == g_app.shell_scene.widgets.hovered.slot &&
                                  home->widget.generation == g_app.shell_scene.widgets.hovered.generation;
        pill_changed = guild_rail_indicator_tick(&rail.home_indicator, {~u64{0}, 1},
                           g_app.renderer.navigation_view && !g_app.renderer.navigation_view->selected_guild_id,
                           home_hovered, false, now_ms, allowed);
    }
    for (u32 i = 0; i < rail.count; ++i) {
        auto& row = rail.rows[i];
        const bool hovered = row.widget.slot == rail.widgets.hovered.slot && row.widget.generation == rail.widgets.hovered.generation;
        // An expanded folder header hands selection and unread pills to its members.
        const bool open_folder = row.view.folder_role == GuildFolderRole::header && row.view.folder_expanded;
        pill_changed = guild_rail_indicator_tick(&row.indicator, {row.view.id, maximum<u64>(1, row.view.generation)},
                           !row.view.unavailable && row.view.selected && !open_folder, !row.view.unavailable && hovered,
                           !row.view.unavailable && row.view.unread && !open_folder, now_ms,
                           allowed && rail_visible) || pill_changed;
    }
    if (pill_changed) g_app.renderer.retained_presentation.valid = false;
    application_surface_motion_owner_initialize(&owner);
    for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i) {
        const auto surface = static_cast<RendererLayerSurface>(i);
        const auto current = observe(surface, allowed);
        // Surface entrances follow the theme's fast motion token. The server
        // header popout uses the official client's scale popout instead: opacity and
        // scale(.01 -> 1) about its top centre, 120 ms CSS ease-out.
        const bool scale_popout = surface == RendererLayerSurface::popup && current.key.scene &&
                                  g_app.shell_scene.guild_menu_expanded;
        owner.slots[i].style = scale_popout ? UiMotionStyle{120, UiMotionCurve::css_ease_out}
                                            : application_theme().motion.fast;
        const float value =
            application_surface_motion_update(&owner, surface, current.key, now_ms, current.animate);
        // A profile popout slides in without a fade (application_profile);
        // the full profile modal keeps the surface fade.
        const bool slide = surface == RendererLayerSurface::profile && g_app.renderer.profile_scene &&
                           g_app.renderer.profile_scene->presentation == ProfilePresentation::popout;
        renderer_set_layer_opacity(&g_app.renderer, surface, slide ? 1.f : value);
        if (surface == RendererLayerSurface::popup) {
            const UiRect popup = g_app.popup_scene.rectangle;
            renderer_set_popup_scale(&g_app.renderer, scale_popout ? .01f + .99f * value : 1.f,
                                     (popup.left + popup.right) * .5f, popup.top);
        }
    }
}

u64 application_surface_motion_next_wake(u64 now_ms) noexcept {
    const bool allowed = host_motion_allowed();
    if (!allowed)
        return 0;
    u64 wake = application_unread_banner_next_wake(now_ms);
    if (const u64 folders = application_guild_folders_next_wake(now_ms); folders && (!wake || folders < wake))
        wake = folders;
    const auto& rail = g_app.guild_rail_scene;
    if (rail.active && !g_app.renderer.login_visible &&
        !(g_app.renderer.settings_scene && g_app.renderer.settings_scene->active)) {
        const u64 home = guild_rail_indicator_next_wake(rail.home_indicator, now_ms);
        if (home && (!wake || home < wake)) wake = home;
        for (u32 i = 0; i < rail.count; ++i) {
            const u64 deadline = guild_rail_indicator_next_wake(rail.rows[i].indicator, now_ms);
            if (deadline && (!wake || deadline < wake)) wake = deadline;
        }
    }
    for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i) {
        const auto surface = static_cast<RendererLayerSurface>(i);
        const auto current = observe(surface, allowed);
        if (!current.animate)
            continue;
        const u64 deadline =
            application_surface_motion_owner_next_wake(g_app.surface_motion, surface, current.key, now_ms);
        if (deadline && (!wake || deadline < wake))
            wake = deadline;
    }
    return wake;
}

bool application_surface_motion_request_frame_if_due(u64 now_ms, bool frame_available) noexcept {
    if (!frame_available)
        return false;
    const u64 wake = application_surface_motion_next_wake(now_ms);
    if (!wake || now_ms < wake)
        return false;
    application_request_frame();
    return true;
}

void application_surface_motion_retire(RendererLayerSurface surface) noexcept {
    application_surface_motion_owner_retire(&g_app.surface_motion, surface);
    renderer_set_layer_opacity(&g_app.renderer, surface, 1);
    if (surface == RendererLayerSurface::popup)
        renderer_set_popup_scale(&g_app.renderer, 1, 0, 0);
}
void application_surface_motion_retire_all() noexcept {
    for (u32 i = 0; i < kApplicationSurfaceMotionCount; ++i)
        application_surface_motion_retire(static_cast<RendererLayerSurface>(i));
}
#endif

} // namespace maicord::app_detail
