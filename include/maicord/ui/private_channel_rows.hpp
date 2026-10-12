#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/model/relationship_store.hpp"
#include "maicord/model/typing_store.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/paint.hpp"

namespace maicord {
// Borrowed by the navigation view for DM and group DM rows and the DM header.
struct UiPrivateRowSource {
    const ChatStateStore* state;
    const UserStore* users;
    const RelationshipStore* relationships;
    const TypingStore* typing;
};
enum class UiPrivateRowStatus : u8 { none, online, idle, dnd, offline };
// 42-DIP conversation rows with 32-DIP avatars.
inline constexpr float kUiPrivateRowHeight = 42.f;
inline constexpr float kUiPrivateRowAvatar = 32.f;
inline constexpr float kUiPrivateHeaderAvatar = 24.f;
// Friends' presence for a one-to-one peer; none for groups and strangers.
UiPrivateRowStatus ui_private_row_status(const UiPrivateRowSource& source, u64 channel_id) noexcept;
// The online friend is online on mobile only: the row draws the phone status.
bool ui_private_row_mobile(const UiPrivateRowSource& source, u64 channel_id) noexcept;
// "Typing…" while someone else types, otherwise "N members" for groups.
Utf8View ui_private_row_subtitle(const UiPrivateRowSource& source, u64 channel_id, Slice<char> storage) noexcept;
// The first two other members, for the generated group icon.
u32 ui_private_row_group_members(const ChatStateStore& state, u64 channel_id, u64 output[2]) noexcept;
// Presence dot at the avatar's lower right; the ring is cut from the row.
UiRect ui_private_row_status_rectangle(UiRect avatar) noexcept;
// Close (hide) button shown on the hovered or selected row.
UiRect ui_private_row_close_rectangle(UiRect row) noexcept;
// Generated group icon: one avatar behind at the top left, one in front at the bottom right.
UiRect ui_private_row_group_avatar(UiRect avatar, u32 index) noexcept;
// A group DM's custom icon hash (validated), else empty.
Utf8View ui_private_row_custom_icon(const ChatStateStore& state, u64 channel_id) noexcept;
// Avatar shape for placeholders and decorations: a group with a custom icon
// draws one round image like a DM (1); otherwise the channel type.
u8 ui_private_avatar_shape(const ChatStateStore* state, u64 channel_id, u8 type) noexcept;

struct ShellNavigationScene;
struct UiScene;
// Base layer below avatar media; group icons get two placeholders.
bool ui_private_avatar_placeholder_paint(PaintStream* stream, u8 type, UiRect avatar, const Theme& theme) noexcept;
// Above avatar media: presence dots, group icon cutouts and the hovered row's close button.
bool ui_private_rows_overlay_paint(const ShellNavigationScene& scene, UiRect clip, const UiPrivateRowSource& source,
                                   u64 selected_channel_id, const Theme& theme, PaintStream* stream) noexcept;
// Avatar beside the conversation title; the title text starts after it.
UiRect ui_private_header_avatar(const UiScene& scene) noexcept;
bool ui_private_header_overlay_paint(const UiScene& scene, const UiPrivateRowSource& source, u64 channel_id,
                                     const Theme& theme, PaintStream* stream) noexcept;
struct MemberListScene;
// Crown on the group owner's row of a group DM member list.
bool ui_private_member_owner_paint(const MemberListScene& scene, const ChatStateStore& state, const Theme& theme,
                                   PaintStream* stream) noexcept;
// The crown sits right after the owner's name: label is the row's
// name line, name_width the name's measured width (clamped to the line).
UiRect ui_private_member_crown_rectangle(UiRect label, float name_width) noexcept;
inline constexpr float kUiPrivateMemberCrownExtent = 14.f, kUiPrivateMemberCrownGap = 4.f;
} // namespace maicord
