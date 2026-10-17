#pragma once

#include "maicord/model/message_store.hpp"
#include "maicord/model/reaction_preferences.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {

inline constexpr u32 kMessageContextQuickReactionCount = 4;
inline constexpr u32 kMessageContextMaximumItems = 21;
inline constexpr u32 kMessageContextMaximumLinkBytes = 128;

enum class MessageContextAction : u32 {
    none,
    quick_reaction_0,
    quick_reaction_1,
    quick_reaction_2,
    quick_reaction_3,
    add_reaction,
    view_reactions,
    edit_message,
    reply,
    copy_text,
    copy_message_link,
    delete_message,
    copy_id,
    retry_send,
    discard_failed,
    message_author,
    pin_message,
    unpin_message,
    forward_message,
    mark_unread,
    create_thread,
    apps_menu,
    remove_embeds,
    speak_message,
    report_message,
};
inline constexpr MessageContextAction kMessageContextLastAction = MessageContextAction::report_message;

// Rows gated by facts the menu cannot see: loaded message commands, embed
// suppression authority (own message or Manage Messages with embeds shown),
// text-to-speech availability and report eligibility (others' messages).
struct MessageContextMenuExtras {
    bool apps, remove_embeds, speak, report;
};

enum class MessageContextMenuError : u8 {
    none,
    invalid_input,
    capacity,
};

struct MessageContextMenuResult {
    u32 count;
    MessageContextMenuError error;

    constexpr bool ok() const noexcept {
        return error == MessageContextMenuError::none;
    }
};

struct MessageContextMenuModel {
    PopupItem items[kMessageContextMaximumItems];
    u16 quick_catalog_indices[kMessageContextQuickReactionCount];
    u32 count;
};

struct MessageContextQuickReaction {
    Utf8View glyph;
    u32 catalog_index;
};

// Pin authority includes the caller's permission and mutation-in-flight gates.
// Only authoritative messages expose pin actions; the retained pinned flag selects
// one semantic Pin or Unpin action. Revalidate that state before dispatch.
MessageContextMenuResult message_context_menu_build(
    MessageContextMenuModel* model,
    const MessageRecord& message, bool can_edit_authoritative,
    bool can_delete_authoritative, bool has_reactions,
    const ReactionPreferenceStore* preferences = nullptr, bool can_message_author = false,
    bool can_pin_authoritative = false, bool can_forward_authoritative = false,
    bool can_mark_unread_authoritative = false, bool reply_target_deleted = false,
    bool can_create_thread_authoritative = false, bool developer_mode = true,
    MessageContextMenuExtras extras = {}) noexcept;
bool message_context_menu_resolve_quick_reaction(
    const MessageContextMenuModel& model, u32 action,
    MessageContextQuickReaction* reaction) noexcept;
u32 message_context_format_snowflake(u64 value, Slice<char> output) noexcept;
u32 message_context_format_link(u64 guild_id, u64 channel_id, u64 message_id,
                                Slice<char> output) noexcept;

} // namespace maicord
