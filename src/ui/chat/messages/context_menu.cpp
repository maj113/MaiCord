#include "maicord/ui/chat/messages/context_menu.hpp"

#include "maicord/core/decimal.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/model/chat/message_store.hpp"
#include "maicord/model/chat/reaction_preferences.hpp"
#include "maicord/ui/text/emoji_catalog.hpp"

namespace maicord {
namespace {

inline constexpr Utf8View kQuickReactionFallbackNames[] = {
    utf8_literal("hundred_points"),
    utf8_literal("thumbs_up"),
    utf8_literal("thumbs_down"),
    utf8_literal("red_heart"),
};

constexpr u32 action(MessageContextAction value) noexcept {
    return static_cast<u32>(value);
}

void decorate_context_actions(MessageContextMenuModel* model) noexcept {
    // Indexed by MessageContextAction; UiIcon::count keeps the row's own presentation.
    static constexpr UiIcon icons[] = {
        UiIcon::count,        UiIcon::count,   UiIcon::count,       UiIcon::count,  UiIcon::count,
        UiIcon::add_reaction, UiIcon::emoji,   UiIcon::edit,        UiIcon::reply,  UiIcon::copy,
        UiIcon::link,         UiIcon::trash,   UiIcon::identifier,  UiIcon::retry,  UiIcon::trash,
        UiIcon::user,         UiIcon::pin,     UiIcon::unpin,       UiIcon::forwarded,
        UiIcon::direct_message, // mark unread: every row of the menu keeps an icon
        UiIcon::thread,       UiIcon::count,   UiIcon::eye_off,     UiIcon::speaker, UiIcon::flags};
    static_assert(sizeof(icons) / sizeof(icons[0]) == static_cast<u32>(kMessageContextLastAction) + 1);
    for (u32 index = 0; index < model->count; ++index) {
        auto& item = model->items[index];
        if (item.action < sizeof(icons) / sizeof(icons[0]) && icons[item.action] != UiIcon::count)
            item.presentation = ui_menu_row_presentation(icons[item.action]);
        else if (item.action == action(MessageContextAction::apps_menu))
            item.presentation =
                ui_menu_row_presentation(UiIcon::chevron_right, false, UiMenuIconPlacement::trailing);
    }
}

// The 64-byte row is mostly zero and padded: one out-of-line copy of its stores, so
// each call site passes only the label, the action and the flags.
__declspec(noinline) void push(MessageContextMenuModel* model, const Utf8View& label, MessageContextAction value,
                               u32 flags) noexcept {
    model->items[model->count++] = PopupItem{label, action(value), static_cast<u16>(flags), 0};
}

bool already_selected(const u16* selected, u32 count, u16 value) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (selected[index] == value) {
            return true;
        }
    }
    return false;
}

bool append_quick_reactions(MessageContextMenuModel* model,
                            const ReactionPreferenceStore* preferences) noexcept {
    const ConstSlice<EmojiCatalogEntry> catalog = unicode_emoji_catalog();
    u32 selected = 0;
    if (preferences) {
        const ConstSlice<ReactionEmojiRef> frequently = reaction_preferences_frequently(*preferences);
        for (u32 index = 0; index < frequently.count && selected < kMessageContextQuickReactionCount;
             ++index) {
            const u16 catalog_index = frequently[index].catalog_index;
            if (frequently[index].custom() || catalog_index >= catalog.count ||
                already_selected(model->quick_catalog_indices, selected, catalog_index)) {
                continue;
            }
            model->quick_catalog_indices[selected++] = catalog_index;
        }
    }
    for (u32 fallback = 0; fallback < static_cast<u32>(sizeof(kQuickReactionFallbackNames) /
                                                       sizeof(kQuickReactionFallbackNames[0])) &&
                           selected < kMessageContextQuickReactionCount;
         ++fallback) {
        for (u32 index = 0; index < catalog.count; ++index) {
            if (!utf8_equal(catalog[index].name(), kQuickReactionFallbackNames[fallback]) ||
                index > 0xFFFEu ||
                already_selected(model->quick_catalog_indices, selected, static_cast<u16>(index))) {
                continue;
            }
            model->quick_catalog_indices[selected++] = static_cast<u16>(index);
            break;
        }
    }
    if (selected != kMessageContextQuickReactionCount) {
        return false;
    }
    for (u32 index = 0; index < kMessageContextQuickReactionCount; ++index) {
        const u16 catalog_index = model->quick_catalog_indices[index];
        push(model, catalog[catalog_index].glyph(),
             static_cast<MessageContextAction>(action(MessageContextAction::quick_reaction_0) + index),
             popup_item_enabled | popup_item_emoji | popup_item_quick_reaction);
    }
    return true;
}
} // namespace

MessageContextMenuResult message_context_menu_build(MessageContextMenuModel* model,
                                                    const MessageRecord& message,
                                                    bool can_edit_authoritative,
                                                    bool can_delete_authoritative, bool has_reactions,
                                                    const ReactionPreferenceStore* preferences,
                                                    bool can_message_author, bool can_pin_authoritative,
                                                    bool can_forward_authoritative,
                                                    bool can_mark_unread_authoritative,
                                                    bool reply_target_deleted,
                                                    bool can_create_thread_authoritative,
                                                    bool developer_mode, MessageContextMenuExtras extras) noexcept {
    if (!model || message.id == 0 || message.channel_id == 0) {
        return MessageContextMenuResult{0, MessageContextMenuError::invalid_input};
    }
    const bool failed = message.delivery == MessageDeliveryState::failed;
    // A local send in flight and a kept deleted message can only be copied.
    const bool pending = message.delivery == MessageDeliveryState::pending || message.delivery == MessageDeliveryState::retained;
    can_message_author = can_message_author && message.author_id != 0;
    const bool can_reply = message.type == 0 || message.type == 19 || message.type == 20 || message.type == 23;
    const u32 required = failed ? 3u
                         : pending
                             ? 1u
                             : 8u + (can_reply ? 1u : 0u) + (has_reactions ? 1u : 0u) + (can_edit_authoritative ? 1u : 0u) +
                                   (can_delete_authoritative ? 1u : 0u) + (can_message_author ? 1u : 0u) +
                                   (can_pin_authoritative ? 1u : 0u) + (can_forward_authoritative ? 1u : 0u) +
                                   (can_mark_unread_authoritative ? 1u : 0u) +
                                   (can_create_thread_authoritative ? 1u : 0u) + (extras.apps ? 1u : 0u) +
                                   (extras.remove_embeds ? 1u : 0u) + (extras.speak ? 1u : 0u) +
                                   (extras.report ? 1u : 0u);
    if (required > kMessageContextMaximumItems) {
        return MessageContextMenuResult{0, MessageContextMenuError::capacity};
    }

    reset_value(model);
    if (failed) {
        push(model,
             reply_target_deleted ? utf8_literal("Original message was deleted") : utf8_literal("Retry send"),
             MessageContextAction::retry_send, reply_target_deleted ? 0 : popup_item_enabled);
        push(model, utf8_literal("Copy text"), MessageContextAction::copy_text, popup_item_enabled);
        push(model, utf8_literal("Delete message"), MessageContextAction::discard_failed,
             popup_item_enabled | popup_item_danger | popup_item_separator_before);
        decorate_context_actions(model);
        return MessageContextMenuResult{model->count, MessageContextMenuError::none};
    }
    if (pending) {
        push(model, utf8_literal("Copy text"), MessageContextAction::copy_text, popup_item_enabled);
        decorate_context_actions(model);
        return MessageContextMenuResult{model->count, MessageContextMenuError::none};
    }

    if (!append_quick_reactions(model, preferences)) {
        return MessageContextMenuResult{0, MessageContextMenuError::invalid_input};
    }
    push(model, utf8_literal("Add reaction"), MessageContextAction::add_reaction, popup_item_enabled);
    if (has_reactions)
        push(model, utf8_literal("View reactions"), MessageContextAction::view_reactions, popup_item_enabled);

    if (can_edit_authoritative)
        push(model, utf8_literal("Edit message"), MessageContextAction::edit_message,
             popup_item_enabled | popup_item_separator_before);
    if (can_message_author)
        push(model, utf8_literal("Message author"), MessageContextAction::message_author, popup_item_enabled);
    if (can_reply)
        push(model, utf8_literal("Reply"), MessageContextAction::reply,
             popup_item_enabled | (!can_edit_authoritative ? popup_item_separator_before : 0));
    if (can_forward_authoritative)
        push(model, utf8_literal("Forward"), MessageContextAction::forward_message, popup_item_enabled);
    if (can_create_thread_authoritative)
        push(model, utf8_literal("Create thread"), MessageContextAction::create_thread, popup_item_enabled);
    if (can_pin_authoritative) {
        const bool pinned = (message.presentation_flags & message_presentation_pinned) != 0;
        push(model, pinned ? utf8_literal("Unpin message") : utf8_literal("Pin message"),
             pinned ? MessageContextAction::unpin_message : MessageContextAction::pin_message, popup_item_enabled);
    }
    if (can_mark_unread_authoritative)
        push(model, utf8_literal("Mark unread"), MessageContextAction::mark_unread, popup_item_enabled);
    if (extras.remove_embeds)
        push(model, utf8_literal("Remove embeds"), MessageContextAction::remove_embeds, popup_item_enabled);
    if (extras.apps)
        push(model, utf8_literal("Apps"), MessageContextAction::apps_menu, popup_item_enabled);
    push(model, utf8_literal("Copy text"), MessageContextAction::copy_text,
         popup_item_enabled | popup_item_separator_before);
    push(model, utf8_literal("Copy message link"), MessageContextAction::copy_message_link,
         popup_item_enabled);
    if (extras.speak)
        push(model, utf8_literal("Speak message"), MessageContextAction::speak_message, popup_item_enabled);
    if (can_delete_authoritative)
        push(model, utf8_literal("Delete message"), MessageContextAction::delete_message,
             popup_item_enabled | popup_item_danger | popup_item_separator_before);
    if (extras.report)
        push(model, utf8_literal("Report message"), MessageContextAction::report_message,
             popup_item_enabled | popup_item_danger | (can_delete_authoritative ? 0 : popup_item_separator_before));
    if (developer_mode)
        push(model, utf8_literal("Copy message ID"), MessageContextAction::copy_id,
             popup_item_enabled | popup_item_separator_before);
    decorate_context_actions(model);
    return MessageContextMenuResult{model->count, MessageContextMenuError::none};
}

bool message_context_menu_resolve_quick_reaction(const MessageContextMenuModel& model, u32 value,
                                                 MessageContextQuickReaction* reaction) noexcept {
    const u32 first = action(MessageContextAction::quick_reaction_0);
    if (!reaction || model.count < kMessageContextQuickReactionCount || value < first ||
        value >= first + kMessageContextQuickReactionCount) {
        return false;
    }
    const u32 slot = value - first;
    const u32 catalog_index = model.quick_catalog_indices[slot];
    const ConstSlice<EmojiCatalogEntry> catalog = unicode_emoji_catalog();
    if (catalog_index >= catalog.count) {
        return false;
    }
    *reaction = MessageContextQuickReaction{catalog[catalog_index].glyph(), catalog_index};
    return true;
}

u32 message_context_format_snowflake(u64 value, const Slice<char>& output) noexcept {
    if (value == 0 || !output.data || output.count == 0) {
        return 0;
    }
    if (decimal_digits(value) > output.count) {
        return 0;
    }
    return decimal_write(output.data, value);
}

u32 message_context_format_link(u64 guild_id, u64 channel_id, u64 message_id, const Slice<char>& output) noexcept {
    if (!output.data || channel_id == 0 || message_id == 0) {
        return 0;
    }
    TextWriter writer{output};
    writer.add("https://discord.com/channels/");
    if (guild_id == 0)
        writer.add("@me");
    else
        writer.number(guild_id);
    writer.add("/");
    writer.number(channel_id);
    writer.add("/");
    writer.number(message_id);
    return writer.finish();
}

} // namespace maicord
