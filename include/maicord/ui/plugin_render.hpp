#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {

// Plugin message decorations (mc.msg.render@1) as the message scene, text
// projection and renderer read them. The application installs one query that
// returns the host's cached verdict for a message revision; it never runs
// plugin code (decorators run only in the application's prepare pass). Views
// borrow plugin host storage until g_ui_plugin_render_revision changes.
inline constexpr u32 kUiPluginRenderBadges = 4;
inline constexpr u32 kUiPluginRenderAccessories = 8;
enum : u8 { ui_plugin_render_normal, ui_plugin_render_collapsed, ui_plugin_render_hidden };
enum : u8 { ui_plugin_media_show, ui_plugin_media_blur, ui_plugin_media_hide };
// MC_TONE_ROLE: the author's role colour (name prefix/suffix only).
inline constexpr u8 kUiPluginToneRole = 5;
// Node kinds match MC_NODE_*: 0 label, 1 icon, 2 badge, 3 button, 4 link.
inline constexpr u8 kUiPluginRenderLabel = 0;
inline constexpr u8 kUiPluginRenderButton = 3;
inline constexpr u8 kUiPluginRenderLink = 4;
inline constexpr u16 kUiPluginRenderNoIcon = 0xffff;

// MessageTextCommand::source_id selectors for decoration text. Rich selectors
// use 0x8000/0x4000; plugin text owns 0x2000 with its own low bits.
inline constexpr u16 kUiPluginRenderSourceMask = 0xE000u;
inline constexpr u16 kUiPluginRenderSource = 0x2000u;
inline constexpr u16 kUiPluginRenderContentSource = 0x2001u;   // replacement markdown
inline constexpr u16 kUiPluginRenderGeneratedSource = 0x2002u; // collapsed label, accessories
// Semantic targets on generated text: accessories are index + 1.
inline constexpr u64 kUiPluginRenderShowTarget = 0x100u;

struct UiPluginRenderNode {
    u64 owner_id, action_id;
    Utf8View text, tooltip, value;
    u16 icon; // UiIcon value or kUiPluginRenderNoIcon
    u8 kind, tone;
};
struct UiPluginRenderDecoration {
    u8 visibility, media, name_tone;
    bool replace_content;
    Utf8View collapsed_label, name_prefix, name_suffix, content;
    u32 badge_count, accessory_count;
    UiPluginRenderNode badges[kUiPluginRenderBadges];
    UiPluginRenderNode accessories[kUiPluginRenderAccessories];
};

using UiPluginRenderQuery = bool (*)(u64 message_id, u32 revision, UiPluginRenderDecoration* output) noexcept;
inline UiPluginRenderQuery g_ui_plugin_render_query = nullptr;
// Changes whenever any decoration (or a session reveal) changes.
inline u64 g_ui_plugin_render_revision = 0;

// False means "no decoration": the message renders unchanged.
inline bool ui_plugin_render(u64 message_id, u32 revision, UiPluginRenderDecoration* output) noexcept {
    *output = {};
    return g_ui_plugin_render_query && message_id && g_ui_plugin_render_query(message_id, revision, output);
}
constexpr bool ui_plugin_render_source(u16 source_id) noexcept {
    return (source_id & kUiPluginRenderSourceMask) == kUiPluginRenderSource;
}
inline Utf8View ui_plugin_render_collapsed_label(const UiPluginRenderDecoration& decoration) noexcept {
    return decoration.collapsed_label.bytes ? decoration.collapsed_label
                                            : utf8_literal("Message hidden by a plugin");
}

} // namespace maicord
