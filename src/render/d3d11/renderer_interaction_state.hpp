#pragma once
#include "maicord/ui/store/store_scene.hpp"

#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/render/d3d11/renderer_settings_visibility.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/voice/audio_presentation.hpp"
#include "maicord/ui/chat/polls/presentation.hpp"
#include "maicord/model/chat/message_poll_store.hpp"
#include "maicord/ui/chat/search/scene.hpp"
#include "maicord/ui/shell/shortcuts_scene.hpp"
#include "maicord/ui/media/download_scene.hpp"
#include "maicord/ui/voice/go_live_scene.hpp"
#include "maicord/ui/chat/messages/forward_scene.hpp"
#include "maicord/ui/chat/unread_banner_scene.hpp"
#include "maicord/ui/widgets/toast.hpp"
#include "maicord/ui/chat/pinned_messages_scene.hpp"
#include "maicord/ui/shell/quick_switcher_scene.hpp"
#include "maicord/ui/chat/inbox_scene.hpp"
#include "maicord/ui/widgets/tooltip.hpp"
#include "maicord/ui/chat/threads/browser_scene.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/media/picker.hpp"
#include "maicord/ui/voice/stream_card_scene.hpp"
#include "maicord/ui/chat/names_card.hpp"
#include "maicord/ui/settings/theme_editor.hpp"
#include "maicord/ui/guild/card_scene.hpp"
#include "maicord/ui/media/expression_card_scene.hpp"
#include "maicord/ui/account/profile_dialog_scene.hpp"
#include "maicord/ui/voice/video_scene.hpp"
#include "maicord/ui/guild/member_list_scene.hpp"
#include "maicord/ui/media/image_viewer.hpp"
#include "maicord/ui/account/friends_scene.hpp"
#include "maicord/ui/chat/messages/reaction_viewer.hpp"
#include "maicord/ui/chat/messages/action_bar.hpp"
#include "maicord/ui/voice/call_scene.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/account/profile_scene.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/render/text/forum_post_surface.hpp"
#include "maicord/render/text/form_surface.hpp"
#include "maicord/ui/chat/threads/forum_post_scene.hpp"

namespace maicord {

enum : u32 {
    d3d11_interaction_text_action_bar = 1u << 0,
    d3d11_interaction_text_popup = 1u << 1,
    d3d11_interaction_text_reaction_viewer = 1u << 2,
    d3d11_interaction_text_form = 1u << 3,
    d3d11_interaction_text_forum_form = 1u << 16,
};

template <class Scene> inline bool d3d11_renderer_active(const Scene* scene) noexcept {
    return scene && scene->active;
}

// Login, the settings pages and the theme editor replace the shell.
inline bool d3d11_renderer_page_open(const D3D11Renderer& renderer) noexcept {
    return renderer.login_visible || renderer_settings_page_visible(renderer) ||
           renderer_theme_editor_visible(renderer);
}

inline const PopupScene* d3d11_renderer_settings_popup(const D3D11Renderer& renderer) noexcept {
    const auto* popup = renderer.popup_scene;
    return popup && popup->active && popup->kind == PopupKind::settings_choice ? popup : nullptr;
}

// A text or forum form is the front-most surface: while one is open, what lies beneath is hidden.
inline bool d3d11_renderer_text_form_active(const D3D11Renderer& renderer) noexcept {
    return renderer.text_form && renderer.text_form->scene && renderer.text_form->scene->active;
}
inline bool d3d11_renderer_forum_form_active(const D3D11Renderer& renderer) noexcept {
    return renderer.forum_form && renderer.forum_form->scene && renderer.forum_form->scene->active;
}
inline bool d3d11_renderer_form_active(const D3D11Renderer& renderer) noexcept {
    return d3d11_renderer_text_form_active(renderer) || d3d11_renderer_forum_form_active(renderer);
}

// The profile editor's preview card and dialog on a settings page.
inline u32 d3d11_renderer_settings_overlay_revision(const D3D11Renderer& renderer) noexcept {
    const auto* preview = renderer.settings_profile_preview;
    const auto* dialog = renderer.settings_profile_dialog;
    const u32 a = preview && preview->active ? preview->revision * 2u + 1u : 0;
    const u32 b = dialog && dialog->active ? dialog->revision * 2u + 1u : 0;
    const auto* store = renderer.settings_store;
    const u32 c = store && store->active ? (store->revision + store->motion * 7u) * 2u + 1u : 0;
    const auto* guild = renderer.settings_guild_preview;
    const u32 d = guild && guild->active ? guild->revision * 2u + 1u : 0;
    // The Server Profile overlays also show the hovered option (the banner upload tile's shade).
    const u32 e = renderer.settings_scene ? renderer.settings_scene->palette_hovered_index + 1u : 0;
    return a ^ (b * 0x9E3779B1u) ^ (c * 0x85EBCA6Bu) ^ (d * 0xC2B2AE35u) ^ (e * 0x27D4EB2Fu);
}

// What a pass over the renderer shows: one bit per surface, each the surface's own rule (open, current, not covered by
// what lies above it). A pass computes the mask once, so all its decisions agree and nothing is re-derived per use.
enum : u32 {
    d3d11_surface_quick_switcher = 1u << 0,
    d3d11_surface_inbox = 1u << 1,
    d3d11_surface_thread_browser = 1u << 2,
    d3d11_surface_call = 1u << 3,
    d3d11_surface_guild_card = 1u << 4,
    d3d11_surface_stream_card = 1u << 5,
    d3d11_surface_names_card = 1u << 6,
    d3d11_surface_expression_card = 1u << 7,
    d3d11_surface_message_forward = 1u << 8,
    d3d11_surface_shortcuts = 1u << 9,
    d3d11_surface_download = 1u << 10,
    d3d11_surface_go_live = 1u << 11,
    d3d11_surface_media_picker = 1u << 12,
    d3d11_surface_video = 1u << 13,
    d3d11_surface_tooltip = 1u << 14,
    d3d11_surface_unread_banner = 1u << 15,
    d3d11_surface_toast = 1u << 16,
    d3d11_surface_search = 1u << 17,
    d3d11_surface_pinned_messages = 1u << 18,
    d3d11_surface_audio = 1u << 19,
    d3d11_surface_poll_voters = 1u << 20,
    // Only d3d11_renderer_frame_surfaces sets it.
    d3d11_surface_poll = 1u << 21,
    d3d11_surface_settings_page = 1u << 22,
    d3d11_surface_theme_editor = 1u << 23,
    d3d11_surface_form = 1u << 24,
    // A message modal is bound and open (even when something covers it).
    d3d11_surface_message_modal = 1u << 25,
    // Surfaces that hide an inline audio or poll scene beneath them.
    d3d11_surface_inline_covered = 1u << 26,
    // The media viewer hosts the inline video scene above its media quad.
    d3d11_surface_viewer_video = 1u << 27,
};

// The inline audio control is bound, enabled and drawable on a surface that shows (`shown` holds the bits below it).
__declspec(noinline) inline bool d3d11_renderer_audio_shown(const D3D11Renderer& renderer, u32 shown) noexcept {
    const auto* scene = renderer.audio_scene;
    if (!scene || !scene->active || !scene->owner.manager || renderer.video_scene ||
        d3d11_renderer_active(renderer.poll_voters_scene) || (shown & d3d11_surface_inline_covered))
        return false;
    const auto* widget = ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle);
    if (!widget || !(widget->flags & ui_widget_enabled) || (widget->flags & ui_widget_block_input) ||
        !ui_rect_has_area(ui_rect_intersection(scene->projection.geometry.bounds, scene->clip)))
        return false;
    switch (scene->surface) {
    case UiAudioSourceSurface::timeline:
        return !(shown & d3d11_surface_message_modal) && renderer.message_scene &&
               renderer.message_scene->active && scene->owner.manager == &renderer.message_scene->widgets;
    case UiAudioSourceSurface::search: {
        const auto* messages = (shown & d3d11_surface_search)
                                   ? ui_search_scene_message_scene(*renderer.search_scene)
                                   : nullptr;
        return messages && messages->active && scene->owner.manager == &messages->widgets;
    }
    case UiAudioSourceSurface::pins:
        return (shown & d3d11_surface_pinned_messages) &&
               scene->owner.manager == &renderer.pinned_messages_scene->widgets;
    case UiAudioSourceSurface::forward:
        return (shown & d3d11_surface_message_forward) &&
               scene->owner.manager == &renderer.message_forward_scene->widgets;
    }
    return false;
}
__declspec(noinline) inline bool d3d11_renderer_poll_voters_shown(const D3D11Renderer& renderer, u32 shown) noexcept {
    const auto* scene = renderer.poll_voters_scene;
    return scene && scene->active && scene->owner.manager &&
           ui_widget_manager_resolve(*scene->owner.manager, scene->owner.handle) &&
           !(shown & (d3d11_surface_inline_covered | d3d11_surface_message_forward));
}
__declspec(noinline) inline bool d3d11_renderer_poll_shown(const D3D11Renderer& renderer, u32 shown) noexcept {
    const auto* scene = renderer.poll_scene;
    if (!scene || !scene->active || !scene->owner.manager ||
        d3d11_renderer_active(renderer.poll_voters_scene) || (shown & d3d11_surface_inline_covered))
        return false;
    const MessageScene* messages =
        scene->surface == UiPollSourceSurface::timeline
            ? (!(shown & d3d11_surface_message_modal) ? renderer.message_scene : nullptr)
        : scene->surface == UiPollSourceSurface::search && (shown & d3d11_surface_search)
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

// What lies over the shell, one bit each (settings, editor and form are their surface bits).
enum : u32 {
    d3d11_over_login = 1u << 0,
    d3d11_over_friends = 1u << 1,
    d3d11_over_requests = 1u << 2,
    d3d11_over_profile = 1u << 3,
    d3d11_over_viewer = 1u << 4,
    d3d11_over_popup = 1u << 5,
    d3d11_over_reaction = 1u << 6,
    d3d11_over_page = d3d11_over_login | d3d11_surface_settings_page | d3d11_surface_theme_editor,
    // No page, viewer, menu or form sits above a modal dialog.
    d3d11_over_modal = d3d11_over_page | d3d11_over_profile | d3d11_over_viewer | d3d11_over_popup |
                       d3d11_over_reaction | d3d11_surface_form,
    d3d11_over_all = d3d11_over_modal | d3d11_over_friends | d3d11_over_requests,
};

// A scene's own `current`, called out of line: each surface query would otherwise inline its own copy of the body.
template <auto kCurrent, class Scene> __declspec(noinline) inline bool d3d11_current(const Scene& scene) noexcept {
    return kCurrent(scene);
}

// renderer_theme_editor_visible, with its `current` out of line.
__forceinline bool d3d11_renderer_editor_open(const D3D11Renderer& renderer) noexcept {
    return !renderer.login_visible && d3d11_renderer_active(renderer.theme_editor_scene) &&
           d3d11_current<ui_theme_editor_current>(*renderer.theme_editor_scene);
}

// What lies over the shell, of the bits asked for (the one copy every query that asks about several surfaces shares).
__declspec(noinline) inline u32 d3d11_renderer_over(const D3D11Renderer& renderer, u32 bits) noexcept {
    const bool editor_open =
        (bits & (d3d11_surface_settings_page | d3d11_surface_theme_editor)) && d3d11_renderer_editor_open(renderer);
    return ((bits & d3d11_over_login) && renderer.login_visible ? d3d11_over_login : 0) |
           (editor_open ? d3d11_surface_theme_editor : 0) |
           ((bits & d3d11_surface_settings_page) && !editor_open && d3d11_renderer_active(renderer.settings_scene)
                ? d3d11_surface_settings_page
                : 0) |
           ((bits & d3d11_over_friends) && d3d11_renderer_active(renderer.friends_scene) ? d3d11_over_friends : 0) |
           ((bits & d3d11_over_requests) && d3d11_renderer_active(renderer.friends_requests_scene)
                ? d3d11_over_requests
                : 0) |
           ((bits & d3d11_over_profile) && d3d11_renderer_active(renderer.profile_scene) ? d3d11_over_profile : 0) |
           ((bits & d3d11_over_viewer) && d3d11_renderer_active(renderer.image_viewer) ? d3d11_over_viewer : 0) |
           ((bits & d3d11_over_popup) && d3d11_renderer_active(renderer.popup_scene) ? d3d11_over_popup : 0) |
           ((bits & d3d11_over_reaction) && d3d11_renderer_active(renderer.reaction_viewer) ? d3d11_over_reaction
                                                                                           : 0) |
           ((bits & d3d11_surface_form) && d3d11_renderer_form_active(renderer) ? d3d11_surface_form : 0);
}

// Whether any of the covers in `bits` lies over the shell: the short-circuit form of d3d11_renderer_over, for the
// rule of one surface. A question about more than four covers is not worth its inline copy: it shares the mask's.
__forceinline bool d3d11_renderer_covered(const D3D11Renderer& renderer, u32 bits) noexcept {
    if (__builtin_popcount(bits) > 4)
        return d3d11_renderer_over(renderer, bits) != 0;
    if (((bits & d3d11_over_login) && renderer.login_visible) ||
        ((bits & d3d11_over_friends) && d3d11_renderer_active(renderer.friends_scene)) ||
        ((bits & d3d11_over_requests) && d3d11_renderer_active(renderer.friends_requests_scene)))
        return true;
    if (bits & (d3d11_surface_settings_page | d3d11_surface_theme_editor)) {
        const bool editor_open = d3d11_renderer_editor_open(renderer);
        if ((bits & d3d11_surface_theme_editor) && editor_open)
            return true;
        if ((bits & d3d11_surface_settings_page) && !editor_open && d3d11_renderer_active(renderer.settings_scene))
            return true;
    }
    return ((bits & d3d11_over_profile) && d3d11_renderer_active(renderer.profile_scene)) ||
           ((bits & d3d11_over_viewer) && d3d11_renderer_active(renderer.image_viewer)) ||
           ((bits & d3d11_over_popup) && d3d11_renderer_active(renderer.popup_scene)) ||
           ((bits & d3d11_over_reaction) && d3d11_renderer_active(renderer.reaction_viewer)) ||
           ((bits & d3d11_surface_form) && d3d11_renderer_form_active(renderer));
}

// The surface bits of the surfaces that stand alone: each rule reads only what lies over the shell and, for the two
// that others read, the quick switcher and the thread browser. Any set of them can be asked for by itself.
constexpr u32 d3d11_surface_simple =
    d3d11_surface_quick_switcher | d3d11_surface_inbox | d3d11_surface_thread_browser | d3d11_surface_call |
    d3d11_surface_guild_card | d3d11_surface_stream_card | d3d11_surface_names_card |
    d3d11_surface_expression_card | d3d11_surface_message_forward | d3d11_surface_shortcuts |
    d3d11_surface_download | d3d11_surface_go_live | d3d11_surface_media_picker | d3d11_surface_search |
    d3d11_surface_pinned_messages | d3d11_surface_settings_page | d3d11_surface_theme_editor | d3d11_surface_form;
// Every surface but the inline poll (see d3d11_renderer_frame_surfaces).
constexpr u32 d3d11_surface_all = ((1u << 28) - 1) & ~d3d11_surface_poll;
// The modal and popup surfaces of the popup layer.
constexpr u32 d3d11_surfaces_popup = d3d11_surface_quick_switcher | d3d11_surface_inbox |
                                     d3d11_surface_message_forward | d3d11_surface_shortcuts |
                                     d3d11_surface_download | d3d11_surface_go_live;

// Every surface rule, from the scene bindings. Paint, text emission, stamps and stale guards of a pass all read the
// one answer of d3d11_renderer_surfaces. Login, the settings pages and the theme editor replace the shell; a text or
// forum form is the front-most surface; modal dialogs sit under menus and viewers; the thread browser covers the
// chat column. A site that asks one question instantiates this for those bits alone: the other rules fold away, and
// each asked rule tests its scene before anything that lies over it, so it costs what that surface's own rule costs.
// Each scene's own `current` begins with its `active`, so an inactive scene is skipped without the call.
template <u32 kWant>
__declspec(noinline) inline u32 d3d11_renderer_shown(const D3D11Renderer& renderer) noexcept {
    constexpr bool full = kWant == d3d11_surface_all;
    static_assert(full || !(kWant & ~d3d11_surface_simple), "only the whole mask holds the inline surfaces");
    constexpr u32 login = d3d11_over_login, friends = d3d11_over_friends, requests = d3d11_over_requests,
                  profile = d3d11_over_profile, viewer = d3d11_over_viewer, popup = d3d11_over_popup,
                  reaction = d3d11_over_reaction, settings = d3d11_surface_settings_page,
                  editor = d3d11_surface_theme_editor, form = d3d11_surface_form, page = d3d11_over_page,
                  modal = d3d11_over_modal;
    // The whole mask reads every cover once, before anything. A query of one surface reads the covers its rule names,
    // where it names them; one of several reads them all, once, when a scene it asks about is open.
    constexpr bool single = (kWant & (kWant - 1)) == 0 && !(kWant & (settings | editor | form));
    u32 above = full ? d3d11_renderer_over(renderer, d3d11_over_all) : 0;
    bool read = full;
    const auto over = [&](u32 bits) __attribute__((always_inline)) noexcept {
        if constexpr (single) {
            return d3d11_renderer_covered(renderer, bits);
        } else {
            if (!read) {
                above = d3d11_renderer_over(renderer, d3d11_over_all);
                read = true;
            }
            return (above & bits) != 0;
        }
    };
    const auto quick_rule = [&]() __attribute__((always_inline)) noexcept {
        return d3d11_renderer_active(renderer.quick_switcher_scene) && !over(modal) &&
               d3d11_current<ui_quick_switcher_scene_current>(*renderer.quick_switcher_scene);
    };
    const auto browser_rule = [&]() __attribute__((always_inline)) noexcept {
        return d3d11_renderer_active(renderer.thread_browser_scene) && !over(page | friends) &&
               d3d11_current<ui_thread_browser_scene_current>(*renderer.thread_browser_scene);
    };
    u32 shown = 0;
    if constexpr (kWant & (settings | editor | form)) {
        over(0);
        shown = above & (settings | editor | form);
    }
    // The surfaces that read the quick switcher or the thread browser take its bit when the mask has it, else its rule.
    const auto quick_up = [&]() __attribute__((always_inline)) noexcept {
        return full ? (shown & d3d11_surface_quick_switcher) != 0 : quick_rule();
    };
    const auto browser_up = [&]() __attribute__((always_inline)) noexcept {
        return full ? (shown & d3d11_surface_thread_browser) != 0 : browser_rule();
    };
    if constexpr (kWant & d3d11_surface_quick_switcher) {
        if (quick_rule())
            shown |= d3d11_surface_quick_switcher;
    }
    if constexpr (kWant & d3d11_surface_inbox) {
        if (d3d11_renderer_active(renderer.inbox_scene) && !over(page | profile | viewer | popup | form) &&
            !quick_up() && d3d11_current<ui_inbox_scene_current>(*renderer.inbox_scene))
            shown |= d3d11_surface_inbox;
    }
    if constexpr (kWant & d3d11_surface_thread_browser) {
        if (browser_rule())
            shown |= d3d11_surface_thread_browser;
    }
    if constexpr (kWant & d3d11_surface_message_forward) {
        if (d3d11_renderer_active(renderer.message_forward_scene) && !over(modal) &&
            d3d11_current<ui_message_forward_scene_current>(*renderer.message_forward_scene))
            shown |= d3d11_surface_message_forward;
    }
    if constexpr (kWant & d3d11_surface_shortcuts) {
        if (d3d11_renderer_active(renderer.shortcuts_scene) && !over(page | viewer | form) &&
            d3d11_current<ui_shortcuts_scene_current>(*renderer.shortcuts_scene))
            shown |= d3d11_surface_shortcuts;
    }
    if constexpr (kWant & d3d11_surface_download) {
        if (d3d11_renderer_active(renderer.download_scene) && !over(modal) &&
            d3d11_current<ui_download_scene_current>(*renderer.download_scene))
            shown |= d3d11_surface_download;
    }
    // The dialog takes all input while open, so the only popup over it is its own options menu, drawn above it.
    if constexpr (kWant & d3d11_surface_go_live) {
        if (d3d11_renderer_active(renderer.go_live_scene) && !over(page | profile | viewer | reaction | form))
            shown |= d3d11_surface_go_live;
    }
    if constexpr (kWant & d3d11_surface_media_picker) {
        if (d3d11_renderer_active(renderer.media_picker_scene) &&
            !over(login | settings | editor | friends | requests | viewer) && !browser_up() &&
            d3d11_current<ui_media_picker_scene_current>(*renderer.media_picker_scene))
            shown |= d3d11_surface_media_picker;
    }
    // Call view over the content column (or the whole window in fullscreen). The application closes it before
    // search, pins or the thread browser show.
    if constexpr (kWant & d3d11_surface_call) {
        if (d3d11_renderer_active(renderer.call_scene) && !over(page | requests) && !browser_up())
            shown |= d3d11_surface_call;
    }
    // The popouts (popup layer): menus and modal surfaces hide them.
    if constexpr (kWant & d3d11_surface_guild_card) {
        if (d3d11_renderer_active(renderer.guild_card_scene) && !over(page | viewer | popup | form))
            shown |= d3d11_surface_guild_card;
    }
    if constexpr (kWant & d3d11_surface_stream_card) {
        if (d3d11_renderer_active(renderer.stream_card_scene) && !over(page | profile | viewer | popup | form))
            shown |= d3d11_surface_stream_card;
    }
    if constexpr (kWant & d3d11_surface_names_card) {
        if (d3d11_renderer_active(renderer.names_card_scene) && !over(page | profile | viewer | popup | form))
            shown |= d3d11_surface_names_card;
    }
    if constexpr (kWant & d3d11_surface_expression_card) {
        if (d3d11_renderer_active(renderer.expression_card_scene) && !over(page | viewer | popup | form))
            shown |= d3d11_surface_expression_card;
    }
    // The header search bar stays over the thread browser as its search entry point; the host never opens the
    // results pane while the browser is open.
    if constexpr (kWant & d3d11_surface_search) {
        if (d3d11_renderer_active(renderer.search_scene) && !over(login | settings | requests | friends) &&
            d3d11_current<ui_search_scene_current>(*renderer.search_scene))
            shown |= d3d11_surface_search;
    }
    if constexpr (kWant & d3d11_surface_pinned_messages) {
        if (d3d11_renderer_active(renderer.pinned_messages_scene) &&
            !over(login | editor | settings | friends | requests | viewer) && !browser_up() &&
            d3d11_current<ui_pinned_messages_scene_current>(*renderer.pinned_messages_scene))
            shown |= d3d11_surface_pinned_messages;
    }
    if constexpr (full) {
        const bool message_modal = d3d11_renderer_active(renderer.quick_switcher_scene) ||
                                   d3d11_renderer_active(renderer.message_forward_scene) ||
                                   d3d11_renderer_active(renderer.download_scene) ||
                                   d3d11_renderer_active(renderer.go_live_scene) ||
                                   d3d11_renderer_active(renderer.shortcuts_scene);
        if (message_modal)
            shown |= d3d11_surface_message_modal;
        if ((above & (login | settings | editor | friends | requests | profile | viewer | popup | reaction | form)) ||
            (shown & (d3d11_surface_thread_browser | d3d11_surface_media_picker | d3d11_surface_quick_switcher |
                      d3d11_surface_download | d3d11_surface_go_live)))
            shown |= d3d11_surface_inline_covered;
        const bool voters_active = d3d11_renderer_active(renderer.poll_voters_scene);
        // Inline controls have one host-owned scene and disappear beneath front surfaces.
        if (!voters_active &&
            !(shown & (d3d11_surface_thread_browser | d3d11_surface_call | d3d11_surface_media_picker |
                       d3d11_surface_message_modal)) &&
            d3d11_renderer_active(renderer.video_scene) &&
            !(above & (login | settings | friends | requests | profile | viewer | popup | reaction | form)) &&
            (!renderer.video_scene_search_result ||
             (renderer.search_scene && d3d11_current<ui_search_scene_current>(*renderer.search_scene))))
            shown |= d3d11_surface_video;
        if (renderer.video_scene_viewer && d3d11_renderer_active(renderer.video_scene) && (above & viewer) &&
            !(above & (login | settings)))
            shown |= d3d11_surface_viewer_video;
        if (renderer.tooltip_scene && !(above & (login | requests | popup | reaction | form)) && !voters_active &&
            !message_modal && ui_tooltip_visible(*renderer.tooltip_scene))
            shown |= d3d11_surface_tooltip;
        // Timeline overlay below every popup/modal layer; covering surfaces hide it.
        if (renderer.unread_banner_scene && !(above & (page | friends | requests | viewer)) &&
            !(shown & (d3d11_surface_thread_browser | d3d11_surface_call)) &&
            d3d11_renderer_active(renderer.message_scene) && ui_unread_banner_visible(*renderer.unread_banner_scene))
            shown |= d3d11_surface_unread_banner;
        // Toasts show over every surface except the sign-in screen.
        if (renderer.toast_scene && !(above & login) && ui_toast_visible(*renderer.toast_scene))
            shown |= d3d11_surface_toast;
        if (renderer.audio_scene && d3d11_renderer_audio_shown(renderer, shown))
            shown |= d3d11_surface_audio;
        if (renderer.poll_voters_scene && d3d11_renderer_poll_voters_shown(renderer, shown))
            shown |= d3d11_surface_poll_voters;
    }
    return shown;
}

__forceinline u32 d3d11_renderer_surfaces(const D3D11Renderer& renderer) noexcept {
    return d3d11_renderer_shown<d3d11_surface_all>(renderer);
}

// One surface by itself.
template <u32 kSurface> __forceinline bool d3d11_renderer_shows(const D3D11Renderer& renderer) noexcept {
    return (d3d11_renderer_shown<kSurface>(renderer) & kSurface) != 0;
}

// The surfaces and the inline poll (d3d11_surface_poll), which only the render passes read: the other callers link
// without the message scene it resolves its row in.
__declspec(noinline) inline u32 d3d11_renderer_frame_surfaces(const D3D11Renderer& renderer) noexcept {
    const u32 shown = d3d11_renderer_surfaces(renderer);
    return renderer.poll_scene && d3d11_renderer_poll_shown(renderer, shown) ? shown | d3d11_surface_poll : shown;
}

__declspec(noinline) inline const UiWidgetManager* d3d11_renderer_visible_focus_owner(const D3D11Renderer& renderer,
                                                                                     u32 shown) noexcept {
    const auto* owner = renderer.focus_owner;
    if (d3d11_renderer_forum_form_active(renderer)) {
        const auto* front = d3d11_renderer_text_form_active(renderer)
                ? &renderer.text_form->scene->widgets
                : &renderer.forum_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (shown & d3d11_surface_theme_editor) {
        const auto* front = &renderer.theme_editor_scene->widgets;
        if (d3d11_renderer_text_form_active(renderer))
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (shown & d3d11_surface_settings_page) {
        const auto* popup = d3d11_renderer_settings_popup(renderer);
        const UiWidgetManager* front = popup ? &popup->widgets : &renderer.settings_scene->widgets;
        if (d3d11_renderer_active(renderer.settings_profile_dialog))
            front = &renderer.settings_profile_dialog->widgets;
        if (d3d11_renderer_text_form_active(renderer))
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (shown & d3d11_surface_quick_switcher)
        return owner == &renderer.quick_switcher_scene->widgets ? owner : nullptr;
    if ((shown & d3d11_surface_inbox) && owner == &renderer.inbox_scene->widgets)
        return owner;
    if (shown & d3d11_surface_message_forward)
        return owner == &renderer.message_forward_scene->widgets ? owner : nullptr;
    if (shown & d3d11_surface_shortcuts)
        return owner == &renderer.shortcuts_scene->widgets ? owner : nullptr;
    if (shown & d3d11_surface_download)
        return owner == &renderer.download_scene->widgets ? owner : nullptr;
    // The screen share dialog yields only to its own options menu (the popup layer above it).
    if (shown & d3d11_surface_go_live)
        return owner == &renderer.go_live_scene->widgets ||
                       (d3d11_renderer_active(renderer.popup_scene) && owner == &renderer.popup_scene->widgets)
                   ? owner
                   : nullptr;
    // Inline results never take focus: the composer (and its popup) keep it.
    if ((shown & d3d11_surface_media_picker) &&
        renderer.media_picker_scene->variant != UiMediaPickerVariant::inline_results) {
        const UiWidgetManager* front = &renderer.media_picker_scene->widgets;
        if (d3d11_renderer_active(renderer.profile_scene))
            front = &renderer.profile_scene->widgets;
        if (d3d11_renderer_active(renderer.popup_scene))
            front = &renderer.popup_scene->widgets;
        if (d3d11_renderer_text_form_active(renderer))
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (d3d11_renderer_active(renderer.poll_voters_scene)) {
        const UiWidgetManager* front = renderer.poll_voters_scene->owner.manager;
        if (d3d11_renderer_active(renderer.profile_scene))
            front = &renderer.profile_scene->widgets;
        if (d3d11_renderer_active(renderer.popup_scene))
            front = &renderer.popup_scene->widgets;
        if (d3d11_renderer_text_form_active(renderer))
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (d3d11_renderer_active(renderer.friends_requests_scene)) {
        const UiWidgetManager* front = &renderer.friends_requests_scene->widgets;
        if (d3d11_renderer_active(renderer.profile_scene))
            front = &renderer.profile_scene->widgets;
        if (d3d11_renderer_active(renderer.popup_scene))
            front = &renderer.popup_scene->widgets;
        if (d3d11_renderer_text_form_active(renderer))
            front = &renderer.text_form->scene->widgets;
        return owner == front ? owner : nullptr;
    }
    if (((shown & d3d11_surface_thread_browser) || d3d11_renderer_active(renderer.friends_scene)) &&
        ((renderer.message_scene && owner == &renderer.message_scene->widgets) ||
         (renderer.message_composer_scene && owner == &renderer.message_composer_scene->widgets) ||
         (renderer.member_list_scene && owner == &renderer.member_list_scene->widgets)))
        return nullptr;
    return owner;
}

// A layout found the picture under the lens (it resets the source each build).
inline bool d3d11_renderer_zoom_lens_visible(const D3D11Renderer& renderer) noexcept {
    return renderer.zoom_lens && !renderer.login_visible && renderer.zoom_lens_source.ready;
}
// The magnifier over a chat preview needs the timeline uncovered: every surface that hides an inline scene, plus
// the front modals, the call view and the popout cards. Over the viewer's image it has no other condition.
inline bool d3d11_renderer_zoom_lens_allowed(const D3D11Renderer& renderer, u32 shown) noexcept {
    return renderer.zoom_lens &&
           !(shown & (d3d11_surface_inline_covered | d3d11_surface_message_modal | d3d11_surface_call)) &&
           !d3d11_renderer_active(renderer.poll_voters_scene) &&
           !d3d11_renderer_active(renderer.thread_browser_scene) &&
           !d3d11_renderer_active(renderer.guild_card_scene) && !d3d11_renderer_active(renderer.stream_card_scene) &&
           !d3d11_renderer_active(renderer.expression_card_scene);
}
inline bool d3d11_renderer_audio_owner(const D3D11Renderer& renderer, u32 shown, UiWidgetReference owner) noexcept {
    return (shown & d3d11_surface_audio) && owner.manager == renderer.audio_scene->owner.manager &&
           owner.handle.slot == renderer.audio_scene->owner.handle.slot &&
           owner.handle.generation == renderer.audio_scene->owner.handle.generation;
}
inline bool d3d11_renderer_poll_owner(const D3D11Renderer& renderer, u32 shown, UiWidgetReference owner) noexcept {
    return (shown & d3d11_surface_poll) && owner.manager == renderer.poll_scene->owner.manager &&
           owner.handle.slot == renderer.poll_scene->owner.handle.slot &&
           owner.handle.generation == renderer.poll_scene->owner.handle.generation;
}
// Inline controls of a timeline message are message content: their paint
// joins the timeline media overlays and their text leads the overlay text, so
// the unread banner, call view, panels and popups drawn later cover them like
// message text. Search, pins and forward copies stay with their own surface.
inline bool d3d11_renderer_timeline_video_visible(const D3D11Renderer& renderer, u32 shown) noexcept {
    return (shown & d3d11_surface_video) && !renderer.video_scene_search_result;
}
inline bool d3d11_renderer_timeline_audio_visible(const D3D11Renderer& renderer, u32 shown) noexcept {
    return (shown & d3d11_surface_audio) && renderer.audio_scene->surface == UiAudioSourceSurface::timeline;
}
inline bool d3d11_renderer_timeline_poll_visible(const D3D11Renderer& renderer, u32 shown) noexcept {
    return (shown & d3d11_surface_poll) && renderer.poll_scene->surface == UiPollSourceSurface::timeline;
}
// The committed interaction text records the revision of every source it
// depends on: the commit stores them (`shown` is the pass's surface mask), the staleness check compares them.
void d3d11_renderer_commit_interaction_text_state(D3D11Renderer* renderer, u32 shown) noexcept;
bool d3d11_renderer_interaction_text_state_current(const D3D11Renderer& renderer) noexcept;

} // namespace maicord
