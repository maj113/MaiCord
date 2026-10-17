#pragma once

#include "maicord/model/chat_state.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
struct GuildRailScene;
struct MemberListScene;
struct MessageComposerScene;
struct ShellNavigationScene;
struct UserStore;

inline constexpr u32 kMaximumShellChromeTextCommands = 384;

// A nonzero visual selection projects its channel name and sidebar emphasis;
// the navigation model and selected chat remain unchanged.
bool shell_text_presentation_build(
    const UiScene& scene, const ChatStateStore& state,
    const NavigationView& navigation, const Theme& theme,
    const ShellNavigationScene& channel_scene,
    const MessageComposerScene* composer_scene,
    const MemberListScene* member_scene,
    const UiRect* composer_text_rectangle,
    UiTextCommandStream* stream, const UserStore* users = nullptr,
    const GuildRailScene* guild_scene = nullptr,
    u64 visual_selected_channel_id = 0) noexcept;

inline bool shell_text_presentation_build(
    const UiScene& scene, const ChatStateStore& state,
    const NavigationView& navigation, const Theme& theme,
    const ShellNavigationScene& channel_scene,
    const MessageComposerScene* composer_scene,
    const MemberListScene* member_scene,
    UiTextCommandStream* stream) noexcept {
    return shell_text_presentation_build(
        scene, state, navigation, theme, channel_scene, composer_scene,
        member_scene, nullptr, stream);
}

inline bool shell_text_presentation_build(
    const UiScene& scene, const ChatStateStore& state,
    const NavigationView& navigation, const Theme& theme,
    const ShellNavigationScene& channel_scene,
    UiTextCommandStream* stream) noexcept {
    return shell_text_presentation_build(scene, state, navigation, theme,
                                         channel_scene, nullptr, nullptr, nullptr,
                                         stream);
}

} // namespace maicord

namespace maicord {
bool shell_text_presentation_build_output(const UiScene& scene, const ChatStateStore& state,
                                   const NavigationView& navigation, const Theme& theme,
                                   const ShellNavigationScene& channel_scene,
                                   const MessageComposerScene* composer_scene,
                                   const MemberListScene* member_scene,
                                   const UiRect* composer_text_rectangle, UiTextOutput* stream,
                                   const UserStore* users, const GuildRailScene* guild_scene,
                                   u64 visual_selected_channel_id = 0) noexcept;
// The member list rows alone: the commands that end build_output's stream
// when it is given the member scene. Appends without resetting the output.
bool shell_text_presentation_member_output(const MemberListScene& member_scene, const Theme& theme,
                                           UiTextOutput* stream) noexcept;
} // namespace maicord
