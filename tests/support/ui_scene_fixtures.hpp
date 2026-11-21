#pragma once
// Caller-storage initializers for UI scenes. The shipping client builds its popup and message scenes
// from owned arenas, so the fixed-capacity variants the tests rely on live here, not in the UI library.
#include "maicord/core/memory.hpp"
#include "maicord/ui/chat/messages/scene.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/shell/text_presentation.hpp"

namespace maicord {
inline bool popup_scene_initialize(PopupScene* scene, Slice<PopupItem> items, Slice<PopupRowWidget> rows,
                                   Slice<UiWidget> widgets, Slice<UiLayoutNode> layout_nodes) noexcept {
    if (!scene || !items.data || items.count == 0 || !rows.data || rows.count < items.count ||
        !widgets.data || widgets.count < 2 || !layout_nodes.data || layout_nodes.count < widgets.count) {
        return false;
    }
    reset_value<PopupScene>(scene);
    scene->items = items;
    scene->rows = rows;
    scene->selected_index = ~0u;
    return ui_widget_manager_initialize(&scene->widgets, widgets, layout_nodes);
}

// The shell text as a command stream. The shipping client builds it into a text output.
inline bool shell_text_presentation_build(const UiScene& scene, const ChatStateStore& state,
                                          const NavigationView& navigation, const Theme& theme,
                                          const ShellNavigationScene& channel_scene,
                                          const MessageComposerScene* composer_scene, const MemberListScene* member_scene,
                                          const UiRect* composer_text_rectangle, UiTextCommandStream* stream,
                                          const UserStore* users = nullptr, const GuildRailScene* guild_scene = nullptr,
                                          u64 visual_selected_channel_id = 0) noexcept {
    UiTextOutput output{stream, nullptr};
    return shell_text_presentation_build_output(scene, state, navigation, theme, channel_scene, composer_scene,
                                                member_scene, composer_text_rectangle, &output, users, guild_scene,
                                                visual_selected_channel_id);
}

// The most rows a test message scene may grow to (message_scene_initialize_owned's maximum_rows).
inline constexpr u32 kMessageSceneTestRows = 512;
// Text commands for a test stream that holds the shell chrome's text (shell_text_presentation_build).
inline constexpr u32 kMaximumShellChromeTextCommands = 384;

// Opens a registered settings page: selects it, rebuilds the scene over its content and focuses its row.
inline bool ui_settings_scene_select(UiSettingsScene* scene, UiPageKey page) noexcept {
    if (!scene || !scene->active || !scene->registry || !ui_page_registry_select(scene->registry, page).ok())
        return false;
    if (!ui_settings_scene_update(scene, scene->registry, scene->viewport))
        return false;
    return ui_settings_scene_focus(scene, {UiSettingsTargetKind::page, page, 0});
}
} // namespace maicord
