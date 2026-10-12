#pragma once

#include "maicord/ui/message_context_menu.hpp"

namespace maicord {

inline constexpr float kMessageContextPreferredWidth = 224.0f;
inline constexpr u32 kMessageContextMaximumExtraItems = 8;
inline constexpr u32 kMessageContextMaximumExtraLabelBytes = 128;
// A right-clicked attachment's media and link groups, placed after the message
// sections and before the developer-mode Copy ID.
inline constexpr u32 kMessageContextMaximumMediaItems = 4;
inline constexpr u32 kMessageContextMaximumPopupItems =
    kMessageContextMaximumItems + kMessageContextMaximumMediaItems + kMessageContextMaximumExtraItems;

enum class MessageContextControllerError : u8 {
    none,
    invalid_input,
    menu,
    popup,
};

struct MessageContextControllerResult {
    MessageContextControllerError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == MessageContextControllerError::none;
    }
};

struct MessageContextController {
    MessageContextMenuModel menu;
    u64 message_id;
    u32 opens;
    u32 rejected;
    bool active;
};

// Extra descriptors are copied, while their validated labels remain borrowed.
// Extras use non-built-in actions and plain menu-row flags; the controller owns
// their section separator. Media items follow the same rules but keep their own
// group separators and sit before the trailing Copy ID row. Failed opens
// preserve the retained menu and popup.
MessageContextControllerResult message_context_controller_open(
    MessageContextController* controller, PopupScene* popup,
    const MessageRecord& message, bool can_edit_authoritative,
    bool can_delete_authoritative, bool has_reactions,
    const ReactionPreferenceStore* preferences, UiRect anchor,
    UiRect viewport, bool can_message_author = false,
    ConstSlice<PopupItem> extra_items = {}, bool can_pin_authoritative = false,
    bool can_forward_authoritative = false, bool can_mark_unread_authoritative = false,
    bool reply_target_deleted = false, bool can_create_thread_authoritative = false,
    bool developer_mode = true, MessageContextMenuExtras extras = {},
    ConstSlice<PopupItem> media_items = {}) noexcept;
void message_context_controller_close(
    MessageContextController* controller) noexcept;
bool message_context_controller_resolve_quick_reaction(
    const MessageContextController& controller, u32 action,
    MessageContextQuickReaction* reaction) noexcept;

} // namespace maicord
