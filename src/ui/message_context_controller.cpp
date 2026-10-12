#include "maicord/ui/message_context_controller.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {

MessageContextControllerResult reject(MessageContextController* controller,
                                      MessageContextControllerError error) noexcept {
    if (controller) saturating_increment(controller->rejected);
    return MessageContextControllerResult{error, false};
}

bool valid_extras(ConstSlice<PopupItem> items, u32 maximum = kMessageContextMaximumExtraItems) noexcept {
    if (items.count > maximum || (!items.data && items.count))
        return false;
    constexpr u16 allowed_flags = popup_item_enabled | popup_item_danger |
        popup_item_checked | popup_item_separator_before;
    for (u32 index = 0; index < items.count; ++index) {
        const auto& item = items[index];
        // Reserve the entire built-in action namespace, including actions absent
        // from this particular message's menu. Extras remain ordinary menu rows.
        if (item.action <= static_cast<u32>(kMessageContextLastAction) ||
            !item.label.data || !item.label.bytes || item.label.bytes > kMessageContextMaximumExtraLabelBytes ||
            !utf8_validate(item.label).valid ||
            (item.flags & static_cast<u16>(~allowed_flags)) != 0) return false;
        for (u32 previous = 0; previous < index; ++previous)
            if (items[previous].action == item.action) return false;
    }
    return true;
}

// popup_scene_open preflights descriptors and capacity but can still fail in a
// caller's measurement callback after resetting its shared arrays. Only this
// bounded prefix can be touched by our plain context-menu tree; journal it so
// the retained popup and all its handles survive such failures too.
struct PopupRollback {
    static constexpr u32 maximum_widgets = 2 + kMessageContextMaximumPopupItems * kPopupWidgetsPerItem;
    PopupScene scene;
    PopupItem items[kMessageContextMaximumPopupItems];
    PopupRowWidget rows[kMessageContextMaximumPopupItems];
    UiWidget widgets[maximum_widgets];
    UiLayoutNode layout[maximum_widgets];
    u32 item_count;
    u32 widget_count;
    u32 layout_count;

    void save(const PopupScene& value, u32 new_count) noexcept {
        scene = value;
        item_count = minimum(value.item_count, new_count);
        widget_count = minimum(value.widgets.count, 2 + new_count * kPopupWidgetsPerItem);
        layout_count = minimum(value.widgets.layout.count, 2 + new_count * kPopupWidgetsPerItem);
        for (u32 index = 0; index < item_count; ++index) {
            items[index] = value.items[index];
            rows[index] = value.rows[index];
        }
        for (u32 index = 0; index < widget_count; ++index) widgets[index] = value.widgets.storage[index];
        for (u32 index = 0; index < layout_count; ++index) layout[index] = value.widgets.layout.storage[index];
    }

    void restore(PopupScene* value) noexcept {
        const auto retained_items = value->items;
        const auto retained_rows = value->rows;
        const auto retained_widgets = value->widgets.storage;
        const auto retained_layout = value->widgets.layout.storage;
        const auto retained_owned_storage = value->owned_storage;
        const u32 rejected = value->rejected;
        *value = scene;
        // Successful backing materialization/reservation is reusable even when
        // the commit fails; reserve guarantees that existing pointers stay put.
        value->items = retained_items;
        value->rows = retained_rows;
        value->widgets.storage = retained_widgets;
        value->widgets.layout.storage = retained_layout;
        value->owned_storage = retained_owned_storage;
        value->rejected = rejected;
        for (u32 index = 0; index < item_count; ++index) {
            value->items[index] = items[index];
            value->rows[index] = rows[index];
        }
        for (u32 index = 0; index < widget_count; ++index) value->widgets.storage[index] = widgets[index];
        for (u32 index = 0; index < layout_count; ++index) value->widgets.layout.storage[index] = layout[index];
    }
};

} // namespace

MessageContextControllerResult message_context_controller_open(
    MessageContextController* controller, PopupScene* popup, const MessageRecord& message,
    bool can_edit_authoritative, bool can_delete_authoritative, bool has_reactions,
    const ReactionPreferenceStore* preferences, UiRect anchor, UiRect viewport, bool can_message_author,
    ConstSlice<PopupItem> extra_items, bool can_pin_authoritative, bool can_forward_authoritative,
    bool can_mark_unread_authoritative, bool reply_target_deleted, bool can_create_thread_authoritative,
    bool developer_mode, MessageContextMenuExtras extras, ConstSlice<PopupItem> media_items) noexcept {
    if (!controller || !popup || message.id == 0 || !valid_extras(extra_items) ||
        !valid_extras(media_items, kMessageContextMaximumMediaItems)) {
        return reject(controller, MessageContextControllerError::invalid_input);
    }
    for (u32 media = 0; media < media_items.count; ++media)
        for (u32 extra = 0; extra < extra_items.count; ++extra)
            if (media_items[media].action == extra_items[extra].action)
                return reject(controller, MessageContextControllerError::invalid_input);
    MessageContextMenuModel menu{};
    const MessageContextMenuResult built = message_context_menu_build(
        &menu, message, can_edit_authoritative, can_delete_authoritative, has_reactions, preferences,
        can_message_author, can_pin_authoritative, can_forward_authoritative,
        can_mark_unread_authoritative, reply_target_deleted, can_create_thread_authoritative, developer_mode,
        extras);
    if (!built.ok()) {
        return reject(controller, MessageContextControllerError::menu);
    }
    PopupItem combined[kMessageContextMaximumPopupItems];
    // Media groups precede the developer-mode Copy ID, which stays last of
    // the built-in rows; plugin extras follow everything.
    const bool trailing_id =
        menu.count && menu.items[menu.count - 1].action == static_cast<u32>(MessageContextAction::copy_id);
    const u32 leading = menu.count - (trailing_id ? 1u : 0u);
    u32 count = 0;
    for (u32 index = 0; index < leading; ++index)
        combined[count++] = menu.items[index];
    for (u32 index = 0; index < media_items.count; ++index) {
        combined[count] = media_items[index];
        if (index == 0)
            combined[count].flags = static_cast<u16>(combined[count].flags | popup_item_separator_before);
        ++count;
    }
    if (trailing_id)
        combined[count++] = menu.items[menu.count - 1];
    for (u32 index = 0; index < extra_items.count; ++index) {
        combined[count] = extra_items[index];
        combined[count].flags = static_cast<u16>((extra_items[index].flags & ~popup_item_separator_before) |
                                                 (index == 0 ? popup_item_separator_before : 0));
        ++count;
    }
    PopupRollback rollback;
    rollback.save(*popup, count);
    const PopupSceneResult opened =
        popup_scene_open(popup, PopupKind::context_menu, message.id, {combined, count},
                         anchor, viewport, kMessageContextPreferredWidth);
    if (!opened.ok()) {
        rollback.restore(popup);
        return reject(controller, MessageContextControllerError::popup);
    }
    controller->menu = menu;
    controller->message_id = message.id;
    controller->active = true;
    saturating_increment(controller->opens);
    return MessageContextControllerResult{MessageContextControllerError::none, true};
}

void message_context_controller_close(MessageContextController* controller) noexcept {
    if (!controller) {
        return;
    }
    controller->message_id = 0;
    controller->active = false;
}

bool message_context_controller_resolve_quick_reaction(const MessageContextController& controller,
                                                       u32 action,
                                                       MessageContextQuickReaction* reaction) noexcept {
    return controller.active && controller.message_id != 0 &&
           message_context_menu_resolve_quick_reaction(controller.menu, action, reaction);
}

} // namespace maicord
