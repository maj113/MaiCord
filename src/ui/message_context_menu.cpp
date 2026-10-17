#include "maicord/ui/message_context_menu.hpp"

#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/reaction_preferences.hpp"
#include "maicord/ui/emoji_catalog.hpp"

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
    for (u32 index = 0; index < model->count; ++index) {
        auto& item = model->items[index];
        UiIcon icon = UiIcon::count;
        switch (static_cast<MessageContextAction>(item.action)) {
        case MessageContextAction::add_reaction:
            icon = UiIcon::add_reaction;
            break;
        case MessageContextAction::view_reactions:
            icon = UiIcon::emoji;
            break;
        case MessageContextAction::edit_message:
            icon = UiIcon::edit;
            break;
        case MessageContextAction::reply:
            icon = UiIcon::reply;
            break;
        case MessageContextAction::forward_message:
            icon = UiIcon::forwarded;
            break;
        case MessageContextAction::create_thread:
            icon = UiIcon::thread;
            break;
        case MessageContextAction::pin_message:
            icon = UiIcon::pin;
            break;
        case MessageContextAction::unpin_message:
            icon = UiIcon::unpin;
            break;
        case MessageContextAction::mark_unread:
            // Every row of the menu keeps an icon; mark unread uses the chat bubble.
            icon = UiIcon::direct_message;
            break;
        case MessageContextAction::copy_text:
            icon = UiIcon::copy;
            break;
        case MessageContextAction::copy_id:
            icon = UiIcon::identifier;
            break;
        case MessageContextAction::copy_message_link:
            icon = UiIcon::link;
            break;
        case MessageContextAction::delete_message:
        case MessageContextAction::discard_failed:
            icon = UiIcon::trash;
            break;
        case MessageContextAction::retry_send:
            icon = UiIcon::retry;
            break;
        case MessageContextAction::message_author:
            icon = UiIcon::user;
            break;
        case MessageContextAction::remove_embeds:
            icon = UiIcon::eye_off;
            break;
        case MessageContextAction::speak_message:
            icon = UiIcon::speaker;
            break;
        case MessageContextAction::report_message:
            icon = UiIcon::flags;
            break;
        case MessageContextAction::apps_menu:
            item.presentation =
                ui_menu_row_presentation(UiIcon::chevron_right, false, UiMenuIconPlacement::trailing);
            break;
        default:
            break;
        }
        if (icon != UiIcon::count)
            item.presentation = ui_menu_row_presentation(icon);
    }
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
    if (!model) {
        return false;
    }
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
        model->items[model->count++] = PopupItem{
            catalog[catalog_index].glyph(),
            action(
                static_cast<MessageContextAction>(action(MessageContextAction::quick_reaction_0) + index)),
            static_cast<u16>(popup_item_enabled | popup_item_emoji | popup_item_quick_reaction), 0};
    }
    return true;
}

bool append_bytes(Utf8View value, Slice<char> output, u32* used) noexcept {
    if (!used || (!value.data && value.bytes != 0) || (!output.data && output.count != 0) ||
        *used > output.count || value.bytes > output.count - *used) {
        return false;
    }
    copy_bytes(output.data + *used, value.data, value.bytes);
    *used += value.bytes;
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
    MessageContextMenuModel candidate{};
    const bool failed = message.delivery == MessageDeliveryState::failed;
    const bool pending = message.delivery == MessageDeliveryState::pending;
    can_message_author = can_message_author && message.author_id != 0 && !failed && !pending;
    can_pin_authoritative = can_pin_authoritative && !failed && !pending;
    can_forward_authoritative = can_forward_authoritative && !failed && !pending;
    can_mark_unread_authoritative = can_mark_unread_authoritative && !failed && !pending;
    can_create_thread_authoritative = can_create_thread_authoritative && !failed && !pending;
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

    if (failed) {
        candidate.items[candidate.count++] = PopupItem{
            reply_target_deleted ? utf8_literal("Original message was deleted") : utf8_literal("Retry send"),
            action(MessageContextAction::retry_send),
            static_cast<u16>(reply_target_deleted ? 0 : popup_item_enabled), 0};
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Copy text"), action(MessageContextAction::copy_text), popup_item_enabled, 0};
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Delete message"), action(MessageContextAction::discard_failed),
            static_cast<u16>(popup_item_enabled | popup_item_danger | popup_item_separator_before), 0};
        decorate_context_actions(&candidate);
        *model = candidate;
        return MessageContextMenuResult{model->count, MessageContextMenuError::none};
    }
    if (pending) {
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Copy text"), action(MessageContextAction::copy_text), popup_item_enabled, 0};
        decorate_context_actions(&candidate);
        *model = candidate;
        return MessageContextMenuResult{model->count, MessageContextMenuError::none};
    }

    if (!append_quick_reactions(&candidate, preferences)) {
        return MessageContextMenuResult{0, MessageContextMenuError::invalid_input};
    }
    candidate.items[candidate.count++] = PopupItem{
        utf8_literal("Add reaction"), action(MessageContextAction::add_reaction), popup_item_enabled, 0};
    if (has_reactions) {
        candidate.items[candidate.count++] =
            PopupItem{utf8_literal("View reactions"), action(MessageContextAction::view_reactions),
                      popup_item_enabled, 0};
    }

    if (can_edit_authoritative) {
        candidate.items[candidate.count++] =
            PopupItem{utf8_literal("Edit message"), action(MessageContextAction::edit_message),
                      static_cast<u16>(popup_item_enabled | popup_item_separator_before), 0};
    }
    if (can_message_author)
        candidate.items[candidate.count++] =
            PopupItem{utf8_literal("Message author"), action(MessageContextAction::message_author),
                      popup_item_enabled, 0};
    if (can_reply)
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Reply"), action(MessageContextAction::reply),
            static_cast<u16>(popup_item_enabled | (!can_edit_authoritative ? popup_item_separator_before : 0)),
            0};
    if (can_forward_authoritative)
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Forward"), action(MessageContextAction::forward_message), popup_item_enabled, 0};
    if (can_create_thread_authoritative)
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Create thread"), action(MessageContextAction::create_thread), popup_item_enabled, 0};
    if (can_pin_authoritative) {
        const bool pinned = (message.presentation_flags & message_presentation_pinned) != 0;
        candidate.items[candidate.count++] = PopupItem{
            pinned ? utf8_literal("Unpin message") : utf8_literal("Pin message"),
            action(pinned ? MessageContextAction::unpin_message : MessageContextAction::pin_message),
            popup_item_enabled, 0};
    }
    if (can_mark_unread_authoritative)
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Mark unread"), action(MessageContextAction::mark_unread), popup_item_enabled, 0};
    if (extras.remove_embeds)
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Remove embeds"), action(MessageContextAction::remove_embeds), popup_item_enabled, 0};
    if (extras.apps)
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Apps"), action(MessageContextAction::apps_menu), popup_item_enabled, 0};
    candidate.items[candidate.count++] =
        PopupItem{utf8_literal("Copy text"), action(MessageContextAction::copy_text),
                  static_cast<u16>(popup_item_enabled | popup_item_separator_before), 0};
    candidate.items[candidate.count++] =
        PopupItem{utf8_literal("Copy message link"), action(MessageContextAction::copy_message_link),
                  popup_item_enabled, 0};
    if (extras.speak)
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Speak message"), action(MessageContextAction::speak_message), popup_item_enabled, 0};
    if (can_delete_authoritative) {
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Delete message"), action(MessageContextAction::delete_message),
            static_cast<u16>(popup_item_enabled | popup_item_danger | popup_item_separator_before), 0};
    }
    if (extras.report)
        candidate.items[candidate.count++] = PopupItem{
            utf8_literal("Report message"), action(MessageContextAction::report_message),
            static_cast<u16>(popup_item_enabled | popup_item_danger |
                             (can_delete_authoritative ? 0 : popup_item_separator_before)),
            0};
    if (developer_mode)
        candidate.items[candidate.count++] =
            PopupItem{utf8_literal("Copy message ID"), action(MessageContextAction::copy_id),
                      static_cast<u16>(popup_item_enabled | popup_item_separator_before), 0};
    decorate_context_actions(&candidate);
    *model = candidate;
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

u32 message_context_format_snowflake(u64 value, Slice<char> output) noexcept {
    if (value == 0 || !output.data || output.count == 0) {
        return 0;
    }
    char reverse[20]{};
    u32 digits = 0;
    do {
        reverse[digits++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && digits < static_cast<u32>(sizeof(reverse)));
    if (digits > output.count) {
        return 0;
    }
    for (u32 index = 0; index < digits; ++index) {
        output[index] = reverse[digits - index - 1];
    }
    return digits;
}

u32 message_context_format_link(u64 guild_id, u64 channel_id, u64 message_id, Slice<char> output) noexcept {
    if (!output.data || channel_id == 0 || message_id == 0) {
        return 0;
    }
    u32 used = 0;
    if (!append_bytes(utf8_literal("https://discord.com/channels/"), output, &used)) {
        return 0;
    }
    if (guild_id == 0) {
        if (!append_bytes(utf8_literal("@me"), output, &used)) {
            return 0;
        }
    } else {
        const u32 bytes = message_context_format_snowflake(
            guild_id, Slice<char>{output.data + used, output.count - used});
        if (bytes == 0) {
            return 0;
        }
        used += bytes;
    }
    if (!append_bytes(utf8_literal("/"), output, &used)) {
        return 0;
    }
    u32 bytes =
        message_context_format_snowflake(channel_id, Slice<char>{output.data + used, output.count - used});
    if (bytes == 0) {
        return 0;
    }
    used += bytes;
    if (!append_bytes(utf8_literal("/"), output, &used)) {
        return 0;
    }
    bytes =
        message_context_format_snowflake(message_id, Slice<char>{output.data + used, output.count - used});
    if (bytes == 0) {
        return 0;
    }
    return used + bytes;
}

} // namespace maicord
