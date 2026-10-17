#pragma once

#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/scene.hpp"

namespace maicord {
struct GuildRailScene;
struct MemberListScene;
struct MessageComposerPresentationInput;
struct MessageComposerScene;
struct ShellNavigationScene;

struct ShellPresentationRegions {
    UiRect message_view;
    UiRect member_sidebar;
    bool member_sidebar_visible;
};
UiRect caret_pixel_rectangle(UiRect caret, UiRect clip, u32 dpi) noexcept;
// The reply bar's mention switch (no button box): tinted @ icon and divider.
bool shell_presentation_paint_reply_mention(const MessageComposerScene& scene, const Theme& theme,
                                            PaintStream* stream) noexcept;
bool shell_presentation_build_composer_overlay(const MessageComposerScene* scene,
    const MessageComposerPresentationInput& input, const Theme& theme, PaintStream* paint, u32 dpi) noexcept;

// A nonzero visual selection only changes the sidebar location highlight.
// Navigation admission and composer permissions still use their ordinary owners.
bool shell_presentation_build_chrome(
    const UiScene& scene, const NavigationView* navigation,
    const GuildRailScene* guild_scene,
    const ShellNavigationScene* channel_scene,
    const MessageComposerScene* composer_scene,
    const MemberListScene* member_scene,
    const Theme& theme, float viewport_width, float viewport_height,
    PaintStream* stream, ShellPresentationRegions* regions,
    const MessageComposerPresentationInput* composer_presentation,
    u64 visual_selected_channel_id = 0) noexcept;

// Status badges are composited after avatar media so the avatar texture cannot
// cover them; a distinct retained pass keeps the layer order independent of
// whether an avatar is resident.
bool shell_presentation_build_member_status_overlay(
    const MemberListScene* member_scene, const Theme& theme,
    PaintStream* stream) noexcept;
// Server rail mention badges (with their rail-coloured cutout ring) and
// plugin corner badges, likewise composited after the server icons' media.
bool shell_presentation_build_guild_rail_overlay(const UiScene& scene, const GuildRailScene* guild_scene,
                                                 const Theme& theme, PaintStream* stream) noexcept;

// The selected server's banner layers above its picture (media): the header
// scrim, the glass edge, and the header dropdown's hover fill and chevron,
// which the chrome leaves out while the banner shows.
bool shell_presentation_build_guild_banner_overlay(const UiScene& scene, const NavigationView* navigation,
                                                   const ShellNavigationScene* channel_scene, const Theme& theme,
                                                   PaintStream* stream) noexcept;
// Whether the banner shows behind the header (white header content).
bool shell_presentation_guild_banner_visible(const UiScene& scene, const ShellNavigationScene* channel_scene) noexcept;

inline bool shell_presentation_build_chrome(
    const UiScene& scene, const NavigationView* navigation,
    const GuildRailScene* guild_scene,
    const ShellNavigationScene* channel_scene,
    const MessageComposerScene* composer_scene,
    const MemberListScene* member_scene,
    const Theme& theme, float viewport_width, float viewport_height,
    PaintStream* stream, ShellPresentationRegions* regions) noexcept {
    return shell_presentation_build_chrome(
        scene, navigation, guild_scene, channel_scene, composer_scene,
        member_scene, theme, viewport_width, viewport_height, stream, regions,
        nullptr);
}

inline bool shell_presentation_build_chrome(
    const UiScene& scene, const NavigationView* navigation,
    const GuildRailScene* guild_scene,
    const ShellNavigationScene* channel_scene,
    const Theme& theme, float viewport_width, float viewport_height,
    PaintStream* stream, ShellPresentationRegions* regions) noexcept {
    return shell_presentation_build_chrome(
        scene, navigation, guild_scene, channel_scene, nullptr, nullptr, theme,
        viewport_width, viewport_height, stream, regions, nullptr);
}

} // namespace maicord
