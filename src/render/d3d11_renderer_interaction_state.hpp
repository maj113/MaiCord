#pragma once

#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/audio_presentation.hpp"
#include "maicord/ui/poll_presentation.hpp"
#include "maicord/model/message_poll_store.hpp"

namespace maicord {

enum : u32 {
    d3d11_interaction_text_action_bar = 1u << 0,
    d3d11_interaction_text_popup = 1u << 1,
    d3d11_interaction_text_reaction_viewer = 1u << 2,
    d3d11_interaction_text_form = 1u << 3,
    d3d11_interaction_text_forum_form = 1u << 16,
};

inline const PopupScene* d3d11_renderer_settings_popup(const D3D11Renderer& renderer) noexcept {
    const auto* popup = renderer.popup_scene;
    return popup && popup->active && popup->kind == PopupKind::settings_choice ? popup : nullptr;
}

// The profile editor's preview card and dialog on a settings page.
inline u32 d3d11_renderer_settings_overlay_revision(const D3D11Renderer& renderer) noexcept {
    const auto* preview = renderer.settings_profile_preview;
    const auto* dialog = renderer.settings_profile_dialog;
    const u32 a = preview && preview->active ? preview->revision * 2u + 1u : 0;
    const u32 b = dialog && dialog->active ? dialog->revision * 2u + 1u : 0;
    return a ^ (b * 0x9E3779B1u);
}

inline bool d3d11_renderer_quick_switcher_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.quick_switcher_scene &&
           ui_quick_switcher_scene_current(*renderer.quick_switcher_scene) && !renderer.login_visible &&
           !renderer_settings_page_visible(renderer) && !renderer_theme_editor_visible(renderer) &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !(renderer.popup_scene && renderer.popup_scene->active) &&
           !(renderer.reaction_viewer && renderer.reaction_viewer->active) &&
           !((renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
             (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active));
}

inline bool d3d11_renderer_inbox_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.inbox_scene && ui_inbox_scene_current(*renderer.inbox_scene) && !renderer.login_visible &&
           !renderer_settings_page_visible(renderer) && !renderer_theme_editor_visible(renderer) &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !(renderer.popup_scene && renderer.popup_scene->active) &&
           !d3d11_renderer_quick_switcher_visible(renderer) &&
           !((renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
             (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active));
}

inline bool d3d11_renderer_thread_browser_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.thread_browser_scene &&
           ui_thread_browser_scene_current(*renderer.thread_browser_scene) && !renderer.login_visible &&
           !renderer_settings_page_visible(renderer) && !renderer_theme_editor_visible(renderer) &&
           !(renderer.friends_scene && renderer.friends_scene->active);
}

// Call view over the content column (or the whole window in fullscreen). The
// application closes it before search, pins or the thread browser show.
inline bool d3d11_renderer_call_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.call_scene && renderer.call_scene->active && !renderer.login_visible &&
           !renderer_settings_page_visible(renderer) && !renderer_theme_editor_visible(renderer) &&
           !(renderer.friends_requests_scene && renderer.friends_requests_scene->active) &&
           !d3d11_renderer_thread_browser_visible(renderer);
}

// Server tag popout (popup layer); menus and modal surfaces hide it.
inline bool d3d11_renderer_guild_card_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.guild_card_scene && renderer.guild_card_scene->active && !renderer.login_visible &&
           !renderer_settings_page_visible(renderer) && !renderer_theme_editor_visible(renderer) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !(renderer.popup_scene && renderer.popup_scene->active) &&
           !((renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
             (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active));
}
// Go Live preview card (popup layer); menus and modal surfaces hide it.
inline bool d3d11_renderer_stream_card_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.stream_card_scene && renderer.stream_card_scene->active && !renderer.login_visible &&
           !renderer_settings_page_visible(renderer) && !renderer_theme_editor_visible(renderer) &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !(renderer.popup_scene && renderer.popup_scene->active) &&
           !((renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
             (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active));
}


inline bool d3d11_renderer_message_forward_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.message_forward_scene &&
           ui_message_forward_scene_current(*renderer.message_forward_scene) && !renderer.login_visible &&
           !renderer_settings_page_visible(renderer) && !renderer_theme_editor_visible(renderer) &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !(renderer.popup_scene && renderer.popup_scene->active) &&
           !(renderer.reaction_viewer && renderer.reaction_viewer->active) &&
           !((renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
             (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active));
}

inline bool d3d11_renderer_message_modal_active(const D3D11Renderer& renderer) noexcept {
    return (renderer.quick_switcher_scene && renderer.quick_switcher_scene->active) ||
           (renderer.message_forward_scene && renderer.message_forward_scene->active) ||
           (renderer.download_scene && renderer.download_scene->active) ||
           (renderer.shortcuts_scene && renderer.shortcuts_scene->active);
}

inline bool d3d11_renderer_shortcuts_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.shortcuts_scene && ui_shortcuts_scene_current(*renderer.shortcuts_scene) &&
           !renderer.login_visible && !renderer_settings_page_visible(renderer) &&
           !renderer_theme_editor_visible(renderer) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !((renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
             (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active));
}

inline bool d3d11_renderer_download_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.download_scene && ui_download_scene_current(*renderer.download_scene) &&
           !renderer.login_visible && !renderer_settings_page_visible(renderer) &&
           !renderer_theme_editor_visible(renderer) &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !(renderer.popup_scene && renderer.popup_scene->active) &&
           !(renderer.reaction_viewer && renderer.reaction_viewer->active) &&
           !((renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
             (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active));
}

inline bool d3d11_renderer_media_picker_visible(const D3D11Renderer& renderer) noexcept {
    return !d3d11_renderer_thread_browser_visible(renderer) && !renderer_theme_editor_visible(renderer) &&
           renderer.media_picker_scene && ui_media_picker_scene_current(*renderer.media_picker_scene) &&
           !renderer.login_visible && !(renderer_settings_page_visible(renderer)) &&
           !(renderer.friends_scene && renderer.friends_scene->active) &&
           !(renderer.friends_requests_scene && renderer.friends_requests_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active);
}

inline const UiWidgetManager* d3d11_renderer_visible_focus_owner(const D3D11Renderer& renderer) noexcept {
    const auto* owner = renderer.focus_owner;
    if (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active) {
        const auto* front =
            renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active
                ? &renderer.text_form->scene->widgets
                : &renderer.forum_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (renderer_theme_editor_visible(renderer)) {
        const auto* front = &renderer.theme_editor_scene->widgets;
        if (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active)
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (renderer_settings_page_visible(renderer)) {
        const auto* popup = d3d11_renderer_settings_popup(renderer);
        const UiWidgetManager* front = popup ? &popup->widgets : &renderer.settings_scene->widgets;
        if (renderer.settings_profile_dialog && renderer.settings_profile_dialog->active)
            front = &renderer.settings_profile_dialog->widgets;
        if (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active)
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (d3d11_renderer_quick_switcher_visible(renderer))
        return owner == &renderer.quick_switcher_scene->widgets ? owner : nullptr;
    if (d3d11_renderer_inbox_visible(renderer) && owner == &renderer.inbox_scene->widgets)
        return owner;
    if (d3d11_renderer_message_forward_visible(renderer))
        return owner == &renderer.message_forward_scene->widgets ? owner : nullptr;
    if (d3d11_renderer_shortcuts_visible(renderer))
        return owner == &renderer.shortcuts_scene->widgets ? owner : nullptr;
    if (d3d11_renderer_download_visible(renderer))
        return owner == &renderer.download_scene->widgets ? owner : nullptr;
    if (d3d11_renderer_media_picker_visible(renderer)) {
        const UiWidgetManager* front = &renderer.media_picker_scene->widgets;
        if (renderer.profile_scene && renderer.profile_scene->active)
            front = &renderer.profile_scene->widgets;
        if (renderer.popup_scene && renderer.popup_scene->active)
            front = &renderer.popup_scene->widgets;
        if (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active)
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (renderer.poll_voters_scene && renderer.poll_voters_scene->active) {
        const UiWidgetManager* front = renderer.poll_voters_scene->owner.manager;
        if (renderer.profile_scene && renderer.profile_scene->active)
            front = &renderer.profile_scene->widgets;
        if (renderer.popup_scene && renderer.popup_scene->active)
            front = &renderer.popup_scene->widgets;
        if (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active)
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (renderer.friends_requests_scene && renderer.friends_requests_scene->active) {
        const UiWidgetManager* front = &renderer.friends_requests_scene->widgets;
        if (renderer.profile_scene && renderer.profile_scene->active)
            front = &renderer.profile_scene->widgets;
        if (renderer.popup_scene && renderer.popup_scene->active)
            front = &renderer.popup_scene->widgets;
        if (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active)
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (renderer.friends_scene && renderer.friends_scene->active &&
        ((renderer.message_scene && owner == &renderer.message_scene->widgets) ||
         (renderer.message_composer_scene && owner == &renderer.message_composer_scene->widgets) ||
         (renderer.member_list_scene && owner == &renderer.member_list_scene->widgets)))
        return nullptr;
    if (d3d11_renderer_thread_browser_visible(renderer) &&
        ((renderer.message_scene && owner == &renderer.message_scene->widgets) ||
         (renderer.message_composer_scene && owner == &renderer.message_composer_scene->widgets) ||
         (renderer.member_list_scene && owner == &renderer.member_list_scene->widgets)))
        return nullptr;
    return owner;
}

// Inline controls have one host-owned scene and disappear beneath front
// surfaces. Keep paint, text emission and stale guards on this same predicate.
inline bool d3d11_renderer_video_scene_visible(const D3D11Renderer& renderer) noexcept {
    return !(renderer.poll_voters_scene && renderer.poll_voters_scene->active) &&
           !d3d11_renderer_thread_browser_visible(renderer) && !d3d11_renderer_call_visible(renderer) &&
           renderer.video_scene && renderer.video_scene->active && !renderer.login_visible &&
           (!renderer.video_scene_search_result ||
            (renderer.search_scene && ui_search_scene_current(*renderer.search_scene))) &&
           !d3d11_renderer_message_modal_active(renderer) && !(renderer_settings_page_visible(renderer)) &&
           !(renderer.friends_scene && renderer.friends_scene->active) &&
           !(renderer.friends_requests_scene && renderer.friends_requests_scene->active) &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !d3d11_renderer_media_picker_visible(renderer) &&
           !(renderer.popup_scene && renderer.popup_scene->active) &&
           !(renderer.reaction_viewer && renderer.reaction_viewer->active) &&
           !((renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
             (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active));
}
// The media viewer hosts the same scene above its media quad.
inline bool d3d11_renderer_viewer_video_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.video_scene_viewer && renderer.video_scene && renderer.video_scene->active &&
           renderer.image_viewer && renderer.image_viewer->active && !renderer.login_visible &&
           !renderer_settings_page_visible(renderer);
}
inline u64 d3d11_renderer_video_text_revision(const D3D11Renderer& renderer) noexcept {
    return d3d11_renderer_video_scene_visible(renderer) || d3d11_renderer_viewer_video_visible(renderer)
               ? ui_video_scene_text_paint_revision(*renderer.video_scene)
               : 0;
}

inline bool d3d11_renderer_tooltip_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.tooltip_scene && !renderer.login_visible &&
           !(renderer.poll_voters_scene && renderer.poll_voters_scene->active) &&
           !d3d11_renderer_message_modal_active(renderer) && !(renderer_settings_page_visible(renderer)) &&
           !(renderer.friends_requests_scene && renderer.friends_requests_scene->active) &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !(renderer.popup_scene && renderer.popup_scene->active) &&
           !(renderer.reaction_viewer && renderer.reaction_viewer->active) &&
           !((renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
             (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active)) &&
           ui_tooltip_visible(*renderer.tooltip_scene);
}
inline u32 d3d11_renderer_tooltip_revision(const D3D11Renderer& renderer) noexcept {
    return d3d11_renderer_tooltip_visible(renderer) ? renderer.tooltip_scene->revision : 0;
}
// Timeline overlay below every popup/modal layer; covering surfaces hide it.
inline bool d3d11_renderer_unread_banner_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.unread_banner_scene && !renderer.login_visible && !renderer_settings_page_visible(renderer) &&
           !renderer_theme_editor_visible(renderer) && !(renderer.friends_scene && renderer.friends_scene->active) &&
           !(renderer.friends_requests_scene && renderer.friends_requests_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !d3d11_renderer_thread_browser_visible(renderer) && !d3d11_renderer_call_visible(renderer) &&
           renderer.message_scene && renderer.message_scene->active &&
           ui_unread_banner_visible(*renderer.unread_banner_scene);
}
inline u32 d3d11_renderer_unread_banner_revision(const D3D11Renderer& renderer) noexcept {
    return d3d11_renderer_unread_banner_visible(renderer) ? renderer.unread_banner_scene->revision : 0;
}
// The header search bar stays over the thread browser as its search entry
// point; the host never opens the results pane while the browser is open.
inline bool d3d11_renderer_search_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.search_scene && ui_search_scene_current(*renderer.search_scene) && !renderer.login_visible &&
           !(renderer_settings_page_visible(renderer)) &&
           !(renderer.friends_requests_scene && renderer.friends_requests_scene->active) &&
           !(renderer.friends_scene && renderer.friends_scene->active);
}
inline bool d3d11_renderer_pinned_messages_visible(const D3D11Renderer& renderer) noexcept {
    return !d3d11_renderer_thread_browser_visible(renderer) && renderer.pinned_messages_scene &&
           ui_pinned_messages_scene_current(*renderer.pinned_messages_scene) && !renderer.login_visible &&
           !renderer_theme_editor_visible(renderer) && !renderer_settings_page_visible(renderer) &&
           !(renderer.friends_scene && renderer.friends_scene->active) &&
           !(renderer.friends_requests_scene && renderer.friends_requests_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active);
}
inline bool d3d11_renderer_audio_scene_visible(const D3D11Renderer& renderer) noexcept {
    const auto* scene = renderer.audio_scene;
    if (!scene || !scene->active || !scene->owner.manager || renderer.video_scene ||
        (renderer.poll_voters_scene && renderer.poll_voters_scene->active) || renderer.login_visible ||
        d3d11_renderer_thread_browser_visible(renderer) || renderer_settings_page_visible(renderer) ||
        renderer_theme_editor_visible(renderer) ||
        (renderer.friends_scene && renderer.friends_scene->active) ||
        (renderer.friends_requests_scene && renderer.friends_requests_scene->active) ||
        (renderer.profile_scene && renderer.profile_scene->active) ||
        (renderer.image_viewer && renderer.image_viewer->active) ||
        d3d11_renderer_media_picker_visible(renderer) || d3d11_renderer_quick_switcher_visible(renderer) ||
        d3d11_renderer_download_visible(renderer) ||
        (renderer.popup_scene && renderer.popup_scene->active) ||
        (renderer.reaction_viewer && renderer.reaction_viewer->active) ||
        (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
        (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active))
        return false;
    const auto* widget = ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle);
    if (!widget || !(widget->flags & ui_widget_enabled) || (widget->flags & ui_widget_block_input) ||
        !ui_rect_has_area(ui_rect_intersection(scene->projection.geometry.bounds, scene->clip)))
        return false;
    switch (scene->surface) {
    case UiAudioSourceSurface::timeline:
        return !d3d11_renderer_message_modal_active(renderer) && renderer.message_scene &&
               renderer.message_scene->active && scene->owner.manager == &renderer.message_scene->widgets;
    case UiAudioSourceSurface::search: {
        const auto* messages = d3d11_renderer_search_visible(renderer)
                                   ? ui_search_scene_message_scene(*renderer.search_scene)
                                   : nullptr;
        return messages && messages->active && scene->owner.manager == &messages->widgets;
    }
    case UiAudioSourceSurface::pins:
        return d3d11_renderer_pinned_messages_visible(renderer) &&
               scene->owner.manager == &renderer.pinned_messages_scene->widgets;
    case UiAudioSourceSurface::forward:
        return d3d11_renderer_message_forward_visible(renderer) &&
               scene->owner.manager == &renderer.message_forward_scene->widgets;
    }
    return false;
}
inline bool d3d11_renderer_audio_owner(const D3D11Renderer& renderer, UiWidgetReference owner) noexcept {
    return d3d11_renderer_audio_scene_visible(renderer) &&
           owner.manager == renderer.audio_scene->owner.manager &&
           owner.handle.slot == renderer.audio_scene->owner.handle.slot &&
           owner.handle.generation == renderer.audio_scene->owner.handle.generation;
}
inline u64 d3d11_renderer_audio_text_revision(const D3D11Renderer& renderer) noexcept {
    return d3d11_renderer_audio_scene_visible(renderer)
               ? ui_audio_scene_text_revision(*renderer.audio_scene)
               : 0;
}
inline bool d3d11_renderer_poll_voters_visible(const D3D11Renderer& renderer) noexcept {
    const auto* scene = renderer.poll_voters_scene;
    return scene && scene->active && scene->owner.manager &&
           ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle) &&
           !renderer.login_visible && !d3d11_renderer_thread_browser_visible(renderer) &&
           !renderer_settings_page_visible(renderer) && !renderer_theme_editor_visible(renderer) &&
           !(renderer.friends_scene && renderer.friends_scene->active) &&
           !(renderer.friends_requests_scene && renderer.friends_requests_scene->active) &&
           !(renderer.profile_scene && renderer.profile_scene->active) &&
           !(renderer.image_viewer && renderer.image_viewer->active) &&
           !d3d11_renderer_media_picker_visible(renderer) &&
           !d3d11_renderer_quick_switcher_visible(renderer) &&
           !d3d11_renderer_message_forward_visible(renderer) &&
           !d3d11_renderer_download_visible(renderer) &&
           !(renderer.popup_scene && renderer.popup_scene->active) &&
           !(renderer.reaction_viewer && renderer.reaction_viewer->active) &&
           !(renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) &&
           !(renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active);
}
inline bool d3d11_renderer_poll_scene_visible(const D3D11Renderer& renderer) noexcept {
    const auto* scene = renderer.poll_scene;
    if (!scene || !scene->active || !scene->owner.manager || renderer.login_visible ||
        (renderer.poll_voters_scene && renderer.poll_voters_scene->active) ||
        d3d11_renderer_thread_browser_visible(renderer) || renderer_settings_page_visible(renderer) ||
        renderer_theme_editor_visible(renderer) ||
        (renderer.friends_scene && renderer.friends_scene->active) ||
        (renderer.friends_requests_scene && renderer.friends_requests_scene->active) ||
        (renderer.profile_scene && renderer.profile_scene->active) ||
        (renderer.image_viewer && renderer.image_viewer->active) ||
        d3d11_renderer_media_picker_visible(renderer) || d3d11_renderer_quick_switcher_visible(renderer) ||
        d3d11_renderer_download_visible(renderer) ||
        (renderer.popup_scene && renderer.popup_scene->active) ||
        (renderer.reaction_viewer && renderer.reaction_viewer->active) ||
        (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active) ||
        (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active))
        return false;
    const MessageScene* messages =
        scene->surface == UiPollSourceSurface::timeline
            ? (!d3d11_renderer_message_modal_active(renderer) ? renderer.message_scene : nullptr)
        : scene->surface == UiPollSourceSurface::search && d3d11_renderer_search_visible(renderer)
            ? ui_search_scene_message_scene(*renderer.search_scene)
            : nullptr;
    if (!messages || !messages->active || scene->owner.manager != &messages->widgets ||
        !ui_rect_has_area(ui_rect_intersection(scene->projection.geometry.bounds, scene->clip)))
        return false;
    const auto* widget = ui_widget_manager_resolve(messages->widgets, scene->owner.handle);
    const auto* row = widget ? message_scene_resolve(*messages, scene->owner.handle) : nullptr;
    const auto* record =
        row && !row->redacted ? message_store_resolve(*messages->timeline->store, row->message) : nullptr;
    const auto* poll = record && messages->polls && messages->polls->store
                           ? message_poll_store_find(*messages->polls->store, record->id)
                           : nullptr;
    return widget && (widget->flags & ui_widget_enabled) && !(widget->flags & ui_widget_block_input) &&
           row && row->poll.slot == scene->owner.handle.slot &&
           row->poll.generation == scene->owner.handle.generation && poll &&
           poll->channel_id == record->channel_id && scene->projection.state.owner == record->id &&
           scene->projection.state.content_revision == poll->content_revision &&
           scene->projection.state.result_revision == poll->result_revision;
}
inline bool d3d11_renderer_poll_owner(const D3D11Renderer& renderer, UiWidgetReference owner) noexcept {
    return d3d11_renderer_poll_scene_visible(renderer) &&
           owner.manager == renderer.poll_scene->owner.manager &&
           owner.handle.slot == renderer.poll_scene->owner.handle.slot &&
           owner.handle.generation == renderer.poll_scene->owner.handle.generation;
}
inline u64 d3d11_renderer_poll_text_revision(const D3D11Renderer& renderer) noexcept {
    return d3d11_renderer_poll_scene_visible(renderer) ? ui_poll_scene_text_revision(*renderer.poll_scene)
                                                       : 0;
}
inline u64 d3d11_renderer_poll_voters_text_revision(const D3D11Renderer& renderer) noexcept {
    return d3d11_renderer_poll_voters_visible(renderer)
               ? ui_poll_voters_scene_text_revision(*renderer.poll_voters_scene)
               : 0;
}
inline u32 d3d11_renderer_interaction_text_active_mask(const D3D11Renderer& renderer) noexcept {
    if (renderer_settings_page_visible(renderer))
        return (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active
                    ? d3d11_interaction_text_forum_form
                    : 0) |
               (1u << 6) | (d3d11_renderer_settings_popup(renderer) ? d3d11_interaction_text_popup : 0) |
               (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active
                    ? d3d11_interaction_text_form
                    : 0);
    u32 mask = renderer_theme_editor_visible(renderer) ? (1u << 13) : 0;
    if (d3d11_renderer_media_picker_visible(renderer))
        mask |= 1u << 12;
    if (d3d11_renderer_search_visible(renderer))
        mask |= 1u << 11;
    if (d3d11_renderer_pinned_messages_visible(renderer))
        mask |= 1u << 14;
    if (d3d11_renderer_quick_switcher_visible(renderer) ||
        d3d11_renderer_message_forward_visible(renderer) || d3d11_renderer_download_visible(renderer))
        mask |= 1u << 15;
    if (d3d11_renderer_tooltip_visible(renderer))
        mask |= 1u << 10;
    if (d3d11_renderer_unread_banner_visible(renderer))
        mask |= 1u << 20;
    if (d3d11_renderer_call_visible(renderer))
        mask |= 1u << 21;
    if (d3d11_renderer_stream_card_visible(renderer))
        mask |= 1u << 22;
    if (d3d11_renderer_guild_card_visible(renderer))
        mask |= 1u << 23;
    if (d3d11_renderer_video_scene_visible(renderer))
        mask |= 1u << 9;
    if (d3d11_renderer_audio_scene_visible(renderer))
        mask |= 1u << 17;
    if (d3d11_renderer_poll_scene_visible(renderer))
        mask |= 1u << 18;
    if (d3d11_renderer_poll_voters_visible(renderer))
        mask |= 1u << 19;
    const bool friends = renderer.friends_scene && renderer.friends_scene->active;
    if (friends)
        mask |= 1u << 7;
    if (renderer.friends_requests_scene && renderer.friends_requests_scene->active)
        mask |= 1u << 8;
    if (renderer.profile_scene && renderer.profile_scene->active)
        mask |= 1u << 5;
    if (renderer.image_viewer && renderer.image_viewer->active)
        mask |= 1u << 4;
    if (!friends && !d3d11_renderer_thread_browser_visible(renderer) && !d3d11_renderer_call_visible(renderer) &&
        renderer.message_action_bar && renderer.message_action_bar->active) {
        mask |= d3d11_interaction_text_action_bar;
    }
    if (renderer.popup_scene && renderer.popup_scene->active) {
        mask |= d3d11_interaction_text_popup;
    }
    if (renderer.reaction_viewer && renderer.reaction_viewer->active) {
        mask |= d3d11_interaction_text_reaction_viewer;
    }
    if (renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active)
        mask |= d3d11_interaction_text_form;
    if (renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active)
        mask |= d3d11_interaction_text_forum_form;
    return mask;
}

inline void d3d11_renderer_commit_interaction_text_state(D3D11Renderer* renderer) noexcept {
    if (!renderer) {
        return;
    }
    renderer->interaction_text_forum_form_revision =
        renderer->forum_form && renderer->forum_form->scene ? renderer->forum_form->scene->revision : 0;
    renderer->interaction_text_theme_editor_revision =
        renderer_theme_editor_visible(*renderer) ? renderer->theme_editor_scene->revision : 0;
    renderer->interaction_text_settings_revision =
        renderer->settings_scene ? renderer->settings_scene->revision : 0;
    renderer->interaction_text_active_mask = d3d11_renderer_interaction_text_active_mask(*renderer);
    renderer->interaction_text_thread_browser_revision =
        d3d11_renderer_thread_browser_visible(*renderer) ? renderer->thread_browser_scene->revision : 0;
    renderer->interaction_text_thread_browser_publication =
        d3d11_renderer_thread_browser_visible(*renderer)
            ? renderer->thread_browser_scene->source_publication
            : 0;
    renderer->interaction_text_video_revision = d3d11_renderer_video_text_revision(*renderer);
    renderer->interaction_text_audio_revision = d3d11_renderer_audio_text_revision(*renderer);
    renderer->interaction_text_poll_revision = d3d11_renderer_poll_text_revision(*renderer);
    renderer->interaction_text_poll_voters_revision = d3d11_renderer_poll_voters_text_revision(*renderer);
    renderer->interaction_text_tooltip_revision = d3d11_renderer_tooltip_revision(*renderer);
    renderer->interaction_text_unread_banner_revision = d3d11_renderer_unread_banner_revision(*renderer);
    renderer->interaction_text_search_revision =
        d3d11_renderer_search_visible(*renderer) ? renderer->search_scene->revision : 0;
    renderer->interaction_text_pinned_messages_revision =
        d3d11_renderer_pinned_messages_visible(*renderer) ? renderer->pinned_messages_scene->revision : 0;
    renderer->interaction_text_quick_switcher_revision =
        d3d11_renderer_quick_switcher_visible(*renderer) ? renderer->quick_switcher_scene->revision : 0;
    renderer->interaction_text_inbox_revision =
        d3d11_renderer_inbox_visible(*renderer) ? renderer->inbox_scene->revision : 0;
    renderer->interaction_text_message_forward_revision =
        d3d11_renderer_message_forward_visible(*renderer) ? renderer->message_forward_scene->revision : 0;
    renderer->interaction_text_download_revision =
        d3d11_renderer_download_visible(*renderer) ? renderer->download_scene->revision : 0;
    renderer->interaction_text_shortcuts_revision =
        d3d11_renderer_shortcuts_visible(*renderer) ? renderer->shortcuts_scene->revision : 0;
    renderer->interaction_text_call_revision =
        d3d11_renderer_call_visible(*renderer) ? renderer->call_scene->revision : 0;
    renderer->interaction_text_stream_card_revision =
        d3d11_renderer_stream_card_visible(*renderer) ? renderer->stream_card_scene->revision : 0;
    renderer->interaction_text_guild_card_revision =
        d3d11_renderer_guild_card_visible(*renderer) ? renderer->guild_card_scene->revision : 0;
    renderer->interaction_text_media_picker_revision =
        d3d11_renderer_media_picker_visible(*renderer) ? renderer->media_picker_scene->revision : 0;
    if (renderer_settings_page_visible(*renderer)) {
        const auto* popup = d3d11_renderer_settings_popup(*renderer);
        renderer->interaction_text_popup_revision = popup ? popup->revision : 0;
        renderer->interaction_text_profile_revision = d3d11_renderer_settings_overlay_revision(*renderer);
        renderer->interaction_text_form_revision =
            renderer->text_form && renderer->text_form->scene ? renderer->text_form->scene->revision : 0;
        return;
    }
    renderer->interaction_text_friends_revision =
        renderer->friends_scene ? renderer->friends_scene->revision : 0;
    renderer->interaction_text_friends_requests_revision =
        renderer->friends_requests_scene ? renderer->friends_requests_scene->revision : 0;
    renderer->interaction_text_profile_revision =
        renderer->profile_scene ? renderer->profile_scene->revision : 0;
    renderer->interaction_text_image_viewer_revision =
        renderer->image_viewer ? renderer->image_viewer->revision : 0;
    renderer->interaction_text_action_bar_revision =
        renderer->message_action_bar ? renderer->message_action_bar->revision : 0;
    renderer->interaction_text_popup_revision = renderer->popup_scene ? renderer->popup_scene->revision : 0;
    renderer->interaction_text_reaction_viewer_revision =
        renderer->reaction_viewer ? renderer->reaction_viewer->revision : 0;
    renderer->interaction_text_form_revision =
        renderer->text_form && renderer->text_form->scene ? renderer->text_form->scene->revision : 0;
}

inline bool d3d11_renderer_interaction_text_state_current(const D3D11Renderer& renderer) noexcept {
    if (renderer.interaction_text_forum_form_revision !=
        (renderer.forum_form && renderer.forum_form->scene ? renderer.forum_form->scene->revision : 0))
        return false;
    if (renderer.interaction_text_thread_browser_revision !=
            (d3d11_renderer_thread_browser_visible(renderer) ? renderer.thread_browser_scene->revision
                                                             : 0) ||
        renderer.interaction_text_thread_browser_publication !=
            (d3d11_renderer_thread_browser_visible(renderer)
                 ? renderer.thread_browser_scene->source_publication
                 : 0))
        return false;
    if (renderer.interaction_text_theme_editor_revision !=
        (renderer_theme_editor_visible(renderer) ? renderer.theme_editor_scene->revision : 0))
        return false;
    if (renderer.interaction_text_media_picker_revision !=
        (d3d11_renderer_media_picker_visible(renderer) ? renderer.media_picker_scene->revision : 0))
        return false;
    if (renderer.interaction_text_search_revision !=
        (d3d11_renderer_search_visible(renderer) ? renderer.search_scene->revision : 0))
        return false;
    if (renderer.interaction_text_pinned_messages_revision !=
        (d3d11_renderer_pinned_messages_visible(renderer) ? renderer.pinned_messages_scene->revision : 0))
        return false;
    if (renderer.interaction_text_quick_switcher_revision !=
        (d3d11_renderer_quick_switcher_visible(renderer) ? renderer.quick_switcher_scene->revision : 0))
        return false;
    if (renderer.interaction_text_inbox_revision !=
        (d3d11_renderer_inbox_visible(renderer) ? renderer.inbox_scene->revision : 0))
        return false;
    if (renderer.interaction_text_message_forward_revision !=
        (d3d11_renderer_message_forward_visible(renderer) ? renderer.message_forward_scene->revision : 0))
        return false;
    if (renderer.interaction_text_download_revision !=
        (d3d11_renderer_download_visible(renderer) ? renderer.download_scene->revision : 0))
        return false;
    if (renderer.interaction_text_shortcuts_revision !=
        (d3d11_renderer_shortcuts_visible(renderer) ? renderer.shortcuts_scene->revision : 0))
        return false;
    if (renderer.interaction_text_call_revision !=
        (d3d11_renderer_call_visible(renderer) ? renderer.call_scene->revision : 0))
        return false;
    if (renderer.interaction_text_stream_card_revision !=
        (d3d11_renderer_stream_card_visible(renderer) ? renderer.stream_card_scene->revision : 0))
        return false;
    if (renderer.interaction_text_guild_card_revision !=
        (d3d11_renderer_guild_card_visible(renderer) ? renderer.guild_card_scene->revision : 0))
        return false;
    if (renderer.interaction_text_tooltip_revision != d3d11_renderer_tooltip_revision(renderer))
        return false;
    if (renderer.interaction_text_unread_banner_revision != d3d11_renderer_unread_banner_revision(renderer))
        return false;
    if (renderer.interaction_text_video_revision != d3d11_renderer_video_text_revision(renderer))
        return false;
    if (renderer.interaction_text_audio_revision != d3d11_renderer_audio_text_revision(renderer))
        return false;
    if (renderer.interaction_text_poll_revision != d3d11_renderer_poll_text_revision(renderer) ||
        renderer.interaction_text_poll_voters_revision !=
            d3d11_renderer_poll_voters_text_revision(renderer))
        return false;
    if (renderer.interaction_text_settings_revision !=
        (renderer.settings_scene ? renderer.settings_scene->revision : 0))
        return false;
    if (renderer_settings_page_visible(renderer)) {
        const auto* popup = d3d11_renderer_settings_popup(renderer);
        return renderer.interaction_text_active_mask ==
                   d3d11_renderer_interaction_text_active_mask(renderer) &&
               renderer.interaction_text_popup_revision == (popup ? popup->revision : 0) &&
               renderer.interaction_text_profile_revision == d3d11_renderer_settings_overlay_revision(renderer) &&
               renderer.interaction_text_form_revision == (renderer.text_form && renderer.text_form->scene
                                                               ? renderer.text_form->scene->revision
                                                               : 0);
    }
    if (renderer.interaction_text_friends_revision !=
        (renderer.friends_scene ? renderer.friends_scene->revision : 0))
        return false;
    if (renderer.interaction_text_friends_requests_revision !=
        (renderer.friends_requests_scene ? renderer.friends_requests_scene->revision : 0))
        return false;
    return renderer.interaction_text_profile_revision ==
               (renderer.profile_scene ? renderer.profile_scene->revision : 0) &&
           renderer.interaction_text_image_viewer_revision ==
               (renderer.image_viewer ? renderer.image_viewer->revision : 0) &&
           renderer.interaction_text_active_mask == d3d11_renderer_interaction_text_active_mask(renderer) &&
           ((renderer.friends_scene && renderer.friends_scene->active) ||
            d3d11_renderer_thread_browser_visible(renderer) ||
            renderer.interaction_text_action_bar_revision ==
                (renderer.message_action_bar ? renderer.message_action_bar->revision : 0)) &&
           renderer.interaction_text_popup_revision ==
               (renderer.popup_scene ? renderer.popup_scene->revision : 0) &&
           renderer.interaction_text_reaction_viewer_revision ==
               (renderer.reaction_viewer ? renderer.reaction_viewer->revision : 0) &&
           renderer.interaction_text_form_revision ==
               (renderer.text_form && renderer.text_form->scene ? renderer.text_form->scene->revision : 0);
}

} // namespace maicord
