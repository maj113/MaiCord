#pragma once
#include "maicord/ui/widgets/badge.hpp"
#include "maicord/model/chat/typing_store.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/layout/density.hpp"

namespace maicord {
struct ChatStateStore;
struct PaintStream;
struct RelationshipStore;
struct UserStore;
struct PopupScene;
struct UiTextMeasureCache;
// Borrowed by the navigation view for DM and group DM rows and the DM header.
struct UiPrivateRowSource {
    const ChatStateStore* state;
    const UserStore* users;
    const RelationshipStore* relationships;
    const TypingStore* typing;
    // Measures the DM header's name for the activity after it (none: no line).
    UiTextMeasureCache* measure;
    // The popup scene: a conversation whose context menu is open keeps its nameplate (none: no menus).
    const PopupScene* menu;
};
enum class UiPrivateRowStatus : u8 { none, online, idle, dnd, offline };
enum class UiActivityGlyph : u8;
// Conversation rows of 38, 42 or 48 DIPs (UI Density) with 32-DIP avatars.
inline float ui_private_row_height() noexcept {
    return ui_density_list_item(g_ui_density);
}
inline constexpr float kUiPrivateRowAvatar = 32.f;
inline constexpr float kUiPrivateHeaderAvatar = 24.f;
enum class UiPresence : u8;
// A user's presence as DMs show it: a friend's relationship presence, else
// the last presence any scope reported (offline without one).
UiPresence ui_user_presence(const UiPrivateRowSource& source, u64 user) noexcept;
// The one-to-one peer's presence (ui_user_presence); a group is online while
// any other member is online, else none.
UiPrivateRowStatus ui_private_row_status(const UiPrivateRowSource& source, u64 channel_id) noexcept;
// "Typing…" while someone else types, "N members" for groups, and an online
// peer's activity line, whose glyph (none otherwise) leads it.
Utf8View ui_private_row_subtitle(const UiPrivateRowSource& source, u64 channel_id, const Slice<char>& storage,
                                 UiActivityGlyph* glyph = nullptr) noexcept;
// The subtitle line of a conversation row's label (its content rectangle).
UiRect ui_private_row_subtitle_rectangle(const UiRect& label) noexcept;
// The first two other members, for the generated group icon.
u32 ui_private_row_group_members(const ChatStateStore& state, u64 channel_id, u64 output[2]) noexcept;
// Presence dot at the avatar's lower right; the ring is cut from the row.
UiRect ui_private_row_status_rectangle(const UiRect& avatar) noexcept;
// Close (hide) button shown on the hovered or selected row.
UiRect ui_private_row_close_rectangle(const UiRect& row) noexcept;
// Generated group icon: one avatar behind at the top left, one in front at the bottom right.
UiRect ui_private_row_group_avatar(const UiRect& avatar, u32 index) noexcept;
// A group DM's custom icon hash (validated), else empty.
Utf8View ui_private_row_custom_icon(const ChatStateStore& state, u64 channel_id) noexcept;
// Avatar shape for placeholders and decorations: a group with a custom icon
// draws one round image like a DM (1); otherwise the channel type.
u8 ui_private_avatar_shape(const ChatStateStore* state, u64 channel_id, u8 type) noexcept;

struct ShellNavigationScene;
struct UiScene;
// Base layer below avatar media; group icons get two placeholders.
bool ui_private_avatar_placeholder_paint(PaintStream* stream, u8 type, const UiRect& avatar, const Theme& theme) noexcept;
// Above avatar media: presence dots, group icon cutouts and the hovered row's close button.
bool ui_private_rows_overlay_paint(const ShellNavigationScene& scene, const UiRect& clip, const UiPrivateRowSource& source,
                                   u64 selected_channel_id, const Theme& theme, PaintStream* stream) noexcept;
// Avatar beside the conversation title; the title text starts after it.
UiRect ui_private_header_avatar(const UiScene& scene) noexcept;
bool ui_private_header_overlay_paint(const UiScene& scene, const UiPrivateRowSource& source, u64 channel_id,
                                     const Theme& theme, PaintStream* stream) noexcept;
// A one-to-one DM header's identity, what a click opens the full profile of and a hover shows the other names
// of: the avatar and the name after it (measured; false without a measure cache or room for the name).
struct UiPrivateHeaderIdentity {
    u64 user;
    UiRect avatar, name;
};
bool ui_private_header_identity(const UiScene& scene, const UiPrivateRowSource& source, u64 channel_id,
                                UiPrivateHeaderIdentity* output) noexcept;
// The width of a one-to-one DM header's identity button (avatar, gap, the whole name), 0 without a measured name.
float ui_private_header_identity_width(const UiPrivateRowSource& source, u64 channel_id) noexcept;
// A one-to-one DM header's activity, 8 DIPs after the name (text-sm): the
// glyph, 4 DIPs, then the line, cut off at the header's end. False without an
// activity, a measured name or room for it.
struct UiPrivateHeaderActivity {
    Utf8View line;
    UiActivityGlyph glyph;
    UiRect icon, text;
};
inline constexpr float kUiPrivateHeaderActivityGap = 8.f, kUiPrivateHeaderActivityMinimum = 24.f;
bool ui_private_header_activity(const UiScene& scene, const UiPrivateRowSource& source, u64 channel_id,
                                const Slice<char>& storage, UiPrivateHeaderActivity* output) noexcept;
struct ProfileMedia;
// The chip after a one-to-one row's name: the peer's server tag (its badge leads), or OFFICIAL with a check for
// Discord's system account. The name keeps the room before it, 8 DIPs apart; the chip is never cut short by it.
enum class UiPrivateRowChipKind : u8 { none, server_tag, official };
struct UiPrivateRowChip {
    UiPrivateRowChipKind kind;
    UiBadgeWidget badge;
    UiRect name;
    u64 guild_id;
};
inline constexpr float kUiPrivateRowChipGap = 8.f;
// The name line of a conversation row's label (its content rectangle), up to `right`; a subtitle lifts it.
UiRect ui_private_row_name_rectangle(const UiRect& content, bool subtitle, float right) noexcept;
// Lays the chip out on the row's name line (owner: the row's label widget; right: where its text ends). False
// without one, with the name's rectangle set either way.
bool ui_private_row_chip(const UiPrivateRowSource& source, u64 channel_id, UiWidgetHandle owner, const UiRect& content,
                         float right, bool subtitle, UiPrivateRowChip* output) noexcept;
// When a conversation row shows its peer's nameplate: while hovered, selected or with its context menu open
// (never at rest), animated only under the pointer and while motion is allowed; its still is drawn behind the
// animation until that is resident.
struct UiPrivateRowPlate {
    bool shown, animated;
};
constexpr UiPrivateRowPlate ui_private_row_plate(bool selected, bool hovered, bool animate, bool menu = false) noexcept {
    return {selected || hovered || menu, hovered && animate};
}
// The conversation whose context menu is open (the source's popup), else zero.
u64 ui_private_row_menu_channel(const UiPrivateRowSource& source) noexcept;
// Where a hovered or selected row shows its peer's nameplate: the row's highlight.
UiRect ui_private_row_highlight(const UiRect& row, const UiRect& avatar) noexcept;
// A one-to-one row's pictures: the peer's nameplate while the row is hovered or selected (animated under the
// pointer when `animate`, with the still behind it, as the member list) and the server tag's badge.
enum class UiPrivateRowMedia : u8 { nameplate, nameplate_still, tag };
bool ui_private_row_media(const ShellNavigationScene& scene, u32 index, const UiPrivateRowSource& source,
                          u64 selected_channel_id, bool animate, UiPrivateRowMedia layer, ProfileMedia* output) noexcept;
struct MemberListScene;
// Crown on the group owner's row of a group DM member list.
bool ui_private_member_owner_paint(const MemberListScene& scene, const ChatStateStore& state, const Theme& theme,
                                   PaintStream* stream) noexcept;
// The crown sits right after the owner's name: label is the row's
// name line, name_width the name's measured width (clamped to the line).
UiRect ui_private_member_crown_rectangle(const UiRect& label, float name_width) noexcept;
inline constexpr float kUiPrivateMemberCrownExtent = 14.f, kUiPrivateMemberCrownGap = 4.f;
} // namespace maicord
