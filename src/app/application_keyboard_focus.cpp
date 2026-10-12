#include "application_profile.hpp"
#include "application_keyboard_focus.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_content_input.hpp"
#include "application_friends.hpp"
#include "application_message_interaction.hpp"
#include "application_thread_header.hpp"
#include "application_scrolling.hpp"
#include "application_state.hpp"
#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/widget_scroll.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {

enum Pane : u32 { guilds, channels, thread_header, messages, members, composer, pane_count };
struct ThreadHeaderKeyboardCapture {
    ApplicationThreadActionTicket ticket;
    UiThreadHeaderTarget target;
    bool armed;
};
ThreadHeaderKeyboardCapture thread_header_keyboard;
u32 current_pane() noexcept {
    const auto* owner = g_app.context_focus;
    if (owner == &g_app.shell_scene.widgets) {
        if (ui_thread_header_widget_target(g_app.thread_header, owner->focused).thread_id) return thread_header;
        const auto* widget = ui_widget_manager_resolve(*owner, owner->focused);
        if (widget && widget->id == static_cast<u32>(UiSemantic::threads_toggle) + 1u)
            return thread_header;
        return widget && (widget->id == static_cast<u32>(UiSemantic::friends_button) + 1u ||
                          widget->id == static_cast<u32>(UiSemantic::guild_menu) + 1u)
                   ? channels
                   : guilds;
    }
    if (owner == &g_app.guild_rail_scene.widgets) return guilds;
    if (g_app.renderer.friends_scene && owner == &g_app.renderer.friends_scene->widgets) return messages;
    if (owner == &g_app.shell_navigation_scene.widgets) return channels;
    if (owner == &g_app.message_scene.widgets) return messages;
    if (owner == &g_app.member_list_scene.widgets) return members;
    if (owner == &g_app.message_composer_scene.widgets) return composer;
    return kUiListNoSelection;
}

bool finish_focus(UiWidgetManager* owner) noexcept {
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    thread_header_keyboard = {};
    ui_scroll_host_cancel(&g_app.scroll_host);
    if (owner != &g_app.message_composer_scene.widgets && g_app.message_composer.focused) {
        stop_local_typing(g_app.message_composer.channel_id);
        g_app.message_composer.focused = false;
        ++g_app.message_composer.revision;
        reset_message_composer_caret_timing(false);
    }
    g_app.context_focus = owner;
    return refresh_account_frame(true);
}

UiWidgetManager* focus_navigation(bool guild, UiListMove move, bool entering) noexcept {
    const auto& view = g_app.account_ui->navigation_view;
    u64 focused = guild ? g_app.guild_rail_scene.focused_guild_id : g_app.shell_navigation_scene.focused_channel_id;
    if (!guild) {
        // Friends leads the conversation list; the server header button sits
        // above a server's channels, reached by moving up from the first one.
        const auto friends = g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::friends_button)];
        const auto header = g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::guild_menu)];
        const auto button = friends.generation ? friends : header;
        const auto* owner = ui_widget_manager_resolve(g_app.shell_scene.widgets, g_app.shell_scene.widgets.focused);
        const bool on_button = g_app.context_focus == &g_app.shell_scene.widgets && owner &&
            (owner->id == static_cast<u32>(UiSemantic::friends_button) + 1u ||
             owner->id == static_cast<u32>(UiSemantic::guild_menu) + 1u);
        const auto first = navigation_focus_target(view, false, focused, UiListMove::first);
        if (button.generation && ((friends.generation && (entering || move == UiListMove::first)) ||
            (move == UiListMove::previous && (on_button || focused == first.id))))
            return ui_widget_manager_focus(&g_app.shell_scene.widgets, button) ? &g_app.shell_scene.widgets : nullptr;
        if (on_button) { focused = 0; move = UiListMove::first; }
    }
    if (guild && g_app.context_focus == &g_app.shell_scene.widgets) focused = 0;
    else if (entering && !focused) focused = guild ? view.selected_guild_id : view.selected_channel_id;
    const auto target = navigation_focus_target(view, guild, focused, move);
    if (target.home) {
        return ui_widget_manager_focus(&g_app.shell_scene.widgets,
            g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::home_button)]) ? &g_app.shell_scene.widgets : nullptr;
    }
    if (!target.id) return nullptr;
    if (guild) return guild_rail_scene_reveal_guild(&g_app.guild_rail_scene, view, target.id) ?
        &g_app.guild_rail_scene.widgets : nullptr;
    return shell_navigation_scene_reveal_channel(&g_app.shell_navigation_scene, view, target.id) ?
        &g_app.shell_navigation_scene.widgets : nullptr;
}

UiWidgetManager* focus_pane(u32 pane, UiListMove move, bool entering) noexcept {
    switch (pane) {
    case guilds: return focus_navigation(true, move, entering);
    case channels: return focus_navigation(false, move, entering);
    case thread_header: {
        auto& header = g_app.thread_header;
        const auto browser = g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::threads_toggle)];
        if (!browser.generation && (!header.active || header.manager != &g_app.shell_scene.widgets)) return nullptr;
        const UiWidgetHandle candidates[]{browser, header.widgets.primary, header.widgets.overflow, header.widgets.cancel};
        u32 current = kUiListNoSelection;
        for (u32 i = 0; !entering && i < 4; ++i)
            if (candidates[i].slot == g_app.shell_scene.widgets.focused.slot &&
                candidates[i].generation == g_app.shell_scene.widgets.focused.generation) current = i;
        struct Eligible { const UiThreadHeader* header; const UiWidgetHandle* candidates; } eligible{&header, candidates};
        const u32 index = ui_list_move(4, current, move,
            [](void* opaque, u32 i) noexcept {
                const auto& source = *static_cast<Eligible*>(opaque);
                if (i == 0)
                    return ui_widget_manager_resolve(g_app.shell_scene.widgets, source.candidates[i]) != nullptr;
                return ui_thread_header_widget_target(*source.header, source.candidates[i]).thread_id != 0;
            }, &eligible);
        return index != kUiListNoSelection && ui_widget_manager_focus(&g_app.shell_scene.widgets, candidates[index])
            ? &g_app.shell_scene.widgets : nullptr;
    }
    case messages:
        if (application_friends_active() && g_app.renderer.friends_scene) {
            auto* scene = const_cast<UiFriendsScene*>(g_app.renderer.friends_scene);
            const auto target = ui_friends_scene_navigate(scene, move);
            if (target.action == UiFriendsAction::none) {
                const auto existing = ui_friends_scene_focused(*scene);
                if (existing.action != UiFriendsAction::none && ui_friends_scene_focus(scene, existing)) return &scene->widgets;
                const auto first = ui_widget_manager_next_focus(scene->widgets, ui_widget_invalid_handle(), false, false);
                const auto* node = ui_widget_manager_layout_node(scene->widgets, first);
                if (!node || !ui_friends_scene_focus(scene, ui_friends_scene_hit_test(*scene,
                    (node->rectangle.left + node->rectangle.right) * .5f,
                    (node->rectangle.top + node->rectangle.bottom) * .5f))) return nullptr;
            }
            return &scene->widgets;
        }
        return message_scene_navigate(&g_app.message_scene, move).message_id ? &g_app.message_scene.widgets : nullptr;
    case members:
        if (application_friends_active()) return nullptr;
        member_list_scene_navigate(&g_app.member_list_scene, move);
        return g_app.member_list_scene.active && g_app.member_list_scene.source_count ? &g_app.member_list_scene.widgets : nullptr;
    case composer:
        if (application_friends_active()) return nullptr;
        return focus_message_composer_control(MessageComposerControl::edit) ? &g_app.message_composer_scene.widgets : nullptr;
    default: return nullptr;
    }
}

void finish_or_log(UiWidgetManager* owner) noexcept {
    if (owner && !finish_focus(owner)) log_write(LogLevel::warning, "ui", "keyboard_focus_refresh_failed");
}

UiWidgetManager* tab_composer(bool reverse, bool entering) noexcept {
    auto& scene = g_app.message_composer_scene;
    if (!scene.active) return nullptr;
    const auto candidate = ui_widget_manager_next_focus(scene.widgets,
        entering ? ui_widget_invalid_handle() : scene.widgets.focused, reverse, false);
    if (!candidate.generation || !ui_widget_manager_focus(&scene.widgets, candidate) ||
        !ui_widget_reveal(&scene.widgets, candidate).ok()) return nullptr;
    const auto control = message_composer_scene_focused_control(scene);
    return focus_message_composer_control(control) ? &scene.widgets : nullptr;
}

void move_tab_focus(u32 pane, bool reverse) noexcept {
    if (pane == composer)
        if (auto* owner = tab_composer(reverse, false)) { finish_or_log(owner); return; }
    if (pane == thread_header) {
        const auto previous = g_app.shell_scene.widgets.focused;
        if (auto* owner = focus_pane(thread_header, reverse ? UiListMove::previous : UiListMove::next, false))
            if (owner->focused.slot != previous.slot || owner->focused.generation != previous.generation) {
                finish_or_log(owner); return;
            }
    }
    u32 candidate = pane;
    for (u32 i = 0; i < pane_count; ++i) {
        candidate = ui_list_next(pane_count, candidate, reverse, true,
            [](void*, u32) noexcept { return true; }, nullptr);
        auto* owner = candidate == composer ? tab_composer(reverse, true)
            : focus_pane(candidate, candidate == thread_header && reverse ? UiListMove::last : UiListMove::restore, true);
        if (owner) { finish_or_log(owner); return; }
    }
}

UiWidgetManager* page_pane(u32 pane, bool reverse) noexcept {
    const auto& view = g_app.account_ui->navigation_view;
    switch (pane) {
    case guilds: {
        const bool home = g_app.context_focus == &g_app.shell_scene.widgets;
        if (reverse && home) return nullptr;
        if (reverse && g_app.guild_rail_scene.scroll_dips == 0 &&
            navigation_focus_target(view, true, g_app.guild_rail_scene.focused_guild_id,
            UiListMove::previous).home) return focus_navigation(true, UiListMove::first, false);
        return guild_rail_scene_page(&g_app.guild_rail_scene, view, reverse, home).id ? &g_app.guild_rail_scene.widgets : nullptr;
    }
    case channels:
        return shell_navigation_scene_page(&g_app.shell_navigation_scene, view, reverse).id ?
            &g_app.shell_navigation_scene.widgets : nullptr;
    case messages:
        if (application_friends_active() && g_app.renderer.friends_scene) {
            auto* scene = const_cast<UiFriendsScene*>(g_app.renderer.friends_scene);
            return ui_friends_scene_page(scene, reverse).action != UiFriendsAction::none ? &scene->widgets : nullptr;
        }
        return message_scene_page(&g_app.message_scene, reverse).message_id ? &g_app.message_scene.widgets : nullptr;
    case members:
        member_list_scene_page(&g_app.member_list_scene, reverse);
        return g_app.member_list_scene.active && g_app.member_list_scene.source_count ? &g_app.member_list_scene.widgets : nullptr;
    default: return nullptr;
    }
}

} // namespace

bool handle_thread_header_key(u32 key, bool down, bool repeated) noexcept {
    const auto logical = key == VK_RETURN ? UiButtonKey::enter : key == VK_SPACE ? UiButtonKey::space
        : key == VK_ESCAPE ? UiButtonKey::escape : UiButtonKey::other;
    if (logical == UiButtonKey::other) return false;
    const bool focused = g_app.context_focus == &g_app.shell_scene.widgets &&
        ui_thread_header_widget_target(g_app.thread_header, g_app.shell_scene.widgets.focused).thread_id;
    if (!focused && !thread_header_keyboard.armed) return false;
    if (!g_app.account_ui || g_app.login.visible || g_app.popup_scene.active || g_app.reaction_viewer.active ||
        (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) {
        ui_button_keyboard_cancel(&g_app.button_keyboard);
        thread_header_keyboard = {};
        return true;
    }
    if (logical == UiButtonKey::escape) {
        ui_button_keyboard_cancel(&g_app.button_keyboard);
        thread_header_keyboard = {};
        return false;
    }
    const auto target = ui_thread_header_widget_target(g_app.thread_header, g_app.shell_scene.widgets.focused);
    if (down && !repeated && target.thread_id) {
        thread_header_keyboard = {application_thread_actions_view(target.thread_id).ticket, target, true};
    }
    const auto result = ui_button_keyboard_key(&g_app.button_keyboard,
        focused ? &g_app.shell_scene.widgets : nullptr, logical, down, repeated);
    if (result.activate && thread_header_keyboard.armed) {
        const auto captured = thread_header_keyboard;
        thread_header_keyboard = {};
        return application_thread_header_activate(
            application_thread_header_activation_source(&g_app.thread_header, captured.ticket), captured.target).handled;
    }
    if (!down || (logical == UiButtonKey::enter && down)) thread_header_keyboard = {};
    return result.handled || focused;
}

bool handle_content_focus_key(u32 key) noexcept {
    if (key != VK_TAB && key != VK_F6 && key != VK_ESCAPE && key != VK_RETURN && key != VK_SPACE && key != VK_UP && key != VK_DOWN &&
        key != VK_HOME && key != VK_END && key != VK_PRIOR && key != VK_NEXT) return false;
    if (!g_app.account_ui || g_app.login.visible || g_app.reaction_viewer.active ||
        (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return false;
    const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const u32 pane = current_pane();
    if (pane == thread_header && (key == VK_RETURN || key == VK_SPACE))
        return handle_thread_header_key(key, true, false);
    if (key == VK_SPACE) {
        const auto* widget = g_app.context_focus == &g_app.shell_scene.widgets ?
            ui_widget_manager_resolve(g_app.shell_scene.widgets, g_app.shell_scene.widgets.focused) : nullptr;
        return widget && (widget->id == static_cast<u32>(UiSemantic::friends_button) + 1u ||
                          widget->id == static_cast<u32>(UiSemantic::guild_menu) + 1u) &&
               activate_focused_navigation();
    }
    if (key == VK_TAB) {
        move_tab_focus(pane, reverse);
        return true;
    }
    if (key == VK_F6) {
        if (g_app.popup_scene.active && !dismiss_popup()) return true;
        u32 candidate = pane;
        for (u32 i = 0; i < pane_count; ++i) {
            candidate = ui_list_next(pane_count, candidate, reverse, true,
                [](void*, u32) noexcept { return true; }, nullptr);
            if (auto* owner = focus_pane(candidate, UiListMove::restore, true)) {
                finish_or_log(owner);
                break;
            }
        }
        return true;
    }
    if (pane == composer && key == VK_ESCAPE &&
        message_composer_scene_focused_control(g_app.message_composer_scene) != MessageComposerControl::edit) {
        finish_or_log(focus_pane(composer, UiListMove::restore, true));
        return true;
    }
    if (pane == composer || pane == kUiListNoSelection || reverse) return false;
    if (key == VK_ESCAPE) {
        finish_or_log(focus_pane(composer, UiListMove::restore, true));
        return true;
    }
    if (key == VK_RETURN) {
        if (pane == guilds || pane == channels) return activate_focused_navigation();
        if(pane==members){UiRect anchor{};ui_widget_manager_context_anchor(g_app.member_list_scene.widgets,&anchor);
            return application_profile_open(g_app.member_list_scene.focused_user_id,anchor,ProfileAnchor::member);}
        return open_focused_context_popup();
    }
    UiListMove move{};
    // The rail glides to a keyboard target like a wheel scroll (after
    // finish_focus, which retires running curves).
    const float rail_before = g_app.guild_rail_scene.scroll_dips;
    switch (key) {
    case VK_UP: move = UiListMove::previous; break;
    case VK_DOWN: move = UiListMove::next; break;
    case VK_HOME: move = UiListMove::first; break;
    case VK_END: move = UiListMove::last; break;
    case VK_PRIOR:
    case VK_NEXT:
        finish_or_log(page_pane(pane, key == VK_PRIOR));
        if (pane == guilds && !animate_guild_rail_scroll_from(rail_before))
            log_write(LogLevel::warning, "ui", "guild_rail_scroll_animation_failed");
        if (pane == messages && key == VK_PRIOR) request_older_history_if_near_start();
        return true;
    default: return false;
    }
    finish_or_log(focus_pane(pane, move, false));
    if (pane == guilds && !animate_guild_rail_scroll_from(rail_before))
        log_write(LogLevel::warning, "ui", "guild_rail_scroll_animation_failed");
    if (pane == messages && (move == UiListMove::previous || move == UiListMove::first))
        request_older_history_if_near_start();
    return true;
}
} // namespace maicord::app_detail
