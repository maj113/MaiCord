#pragma once
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/navigation_context.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
enum class UnreadContextKind : u8 { none, message, channel };
struct UnreadContextSource {
    u64 account_serial, profile_key, self_user_id;
    const void* account_instance;
    const ChatStateStore* state;
    const MessageStore* messages;
    const MessageScene* message_scene;
    const NavigationView* navigation;
    const ShellNavigationScene* navigation_scene;
    const PopupScene* popup;
    bool available;
};
struct UnreadContextTarget {
    UnreadContextSource source;
    u64 id, channel_id;
    u32 message_revision, channel_revision, popup_opens, source_revision, action;
    u16 message_generation, channel_generation, popup_generation;
    UiWidgetHandle row;
    UnreadContextKind kind;
};
// Preflight native action visibility before opening a popup.
bool unread_context_available(UnreadContextSource source, UnreadContextKind kind, u64 id) noexcept;
// The popup must already be published. Nonzero action binds an enabled row.
// No strings or mutable model pointers are retained; identity and borrowed
// owner addresses fence later activation.
bool unread_context_capture(UnreadContextSource source, UnreadContextKind kind,
    u64 id, UnreadContextTarget* target, u32 action = 0) noexcept;
// Keyboard focus/hover may change without expiring this target. A reopened
// popup, retired row, account scope or authoritative record does expire it.
bool unread_context_current(const UnreadContextTarget& target, UnreadContextSource source) noexcept;
} // namespace maicord
