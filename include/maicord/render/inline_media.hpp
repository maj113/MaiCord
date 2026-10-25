#pragma once
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/render/renderer_settings_visibility.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/theme_editor.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/friends_scene.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/profile_scene.hpp"
#include "maicord/render/forum_post_surface.hpp"
#include "maicord/render/text_form_surface.hpp"
#include "maicord/ui/forum_post_scene.hpp"

namespace maicord {
// Match a live renderer surface before dereferencing a borrowed widget manager.
inline bool renderer_inline_media_owner(const D3D11Renderer& renderer, UiWidgetReference owner,
                                        RendererInlineMediaLayer* layer) noexcept {
    if (!owner.manager || !layer || renderer.login_visible)
        return false;
    const auto* popup = renderer.popup_scene;
    if (popup && popup->active && owner.manager == &popup->widgets)
        *layer = popup->kind == PopupKind::settings_choice ? RendererInlineMediaLayer::settings_choice
                                                           : RendererInlineMediaLayer::popup;
    else if (renderer.profile_scene && renderer.profile_scene->active &&
             owner.manager == &renderer.profile_scene->widgets)
        *layer = RendererInlineMediaLayer::profile;
    else if (renderer.media_picker_scene && ui_media_picker_scene_current(*renderer.media_picker_scene) &&
             owner.manager == &renderer.media_picker_scene->widgets)
        *layer = RendererInlineMediaLayer::gallery;
    else if (renderer_theme_editor_visible(renderer) &&
             owner.manager == &renderer.theme_editor_scene->widgets)
        *layer = RendererInlineMediaLayer::theme_editor;
    else if (renderer_settings_page_visible(renderer) &&
             owner.manager == &renderer.settings_scene->widgets &&
             ui_settings_scene_page_owner(*renderer.settings_scene, owner))
        *layer = RendererInlineMediaLayer::settings_page;
    else if (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active &&
             owner.manager == &renderer.text_form->scene->widgets)
        *layer = RendererInlineMediaLayer::form;
    else if (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active &&
             owner.manager == &renderer.forum_form->scene->widgets)
        *layer = RendererInlineMediaLayer::forum;
    else if (renderer.search_scene && ui_search_scene_current(*renderer.search_scene) &&
             (owner.manager == &renderer.search_scene->widgets ||
              (ui_search_scene_message_scene(*renderer.search_scene) &&
               owner.manager == &ui_search_scene_message_scene(*renderer.search_scene)->widgets)))
        *layer = RendererInlineMediaLayer::search;
    else if ((!renderer_settings_page_visible(renderer) &&
              ((renderer.ui_scene && owner.manager == &renderer.ui_scene->widgets) ||
               (renderer.shell_navigation_scene && renderer.shell_navigation_scene->active &&
                owner.manager == &renderer.shell_navigation_scene->widgets) ||
               (renderer.guild_rail_scene && renderer.guild_rail_scene->active &&
                owner.manager == &renderer.guild_rail_scene->widgets) ||
               (renderer.member_list_scene && renderer.member_list_scene->active &&
                owner.manager == &renderer.member_list_scene->widgets))) ||
             (renderer_settings_page_visible(renderer) &&
              owner.manager == &renderer.settings_scene->widgets) ||
             (renderer.friends_scene && renderer.friends_scene->active &&
              owner.manager == &renderer.friends_scene->widgets) ||
             (renderer.friends_requests_scene && renderer.friends_requests_scene->active &&
              owner.manager == &renderer.friends_requests_scene->widgets))
        *layer = RendererInlineMediaLayer::base;
    else
        return false;
    return ui_widget_manager_resolve(*owner.manager, owner.handle) != nullptr;
}
inline bool renderer_inline_flag_current(const D3D11Renderer& renderer,
                                         const RendererInlineFlag& flag) noexcept {
    RendererInlineMediaLayer layer{};
    return flag.flag && renderer_inline_media_owner(renderer, flag.owner, &layer) && layer == flag.layer &&
           ui_rect_has_area(ui_rect_intersection(flag.rectangle, flag.clip));
}
} // namespace maicord
