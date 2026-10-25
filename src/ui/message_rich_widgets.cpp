
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/message_component_layout.hpp"

namespace maicord {
namespace {

UiLayoutStyle region_style(float height, UiLayoutFlow flow) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = flow;
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(height);
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_allow_zero_size);
    return style;
}

UiLayoutStyle reaction_region_style(float height) noexcept {
    UiLayoutStyle style =
        region_style(height, height > 0.0f ? UiLayoutFlow::horizontal_wrap : UiLayoutFlow::overlay);
    if (height > 0.0f) {
        style.gap = kDefaultMessageReactionLayoutMetrics.gap;
    }
    return style;
}

UiLayoutStyle item_style(float height) noexcept {
    return region_style(height, UiLayoutFlow::overlay);
}

UiLayoutStyle reaction_button_style(float width) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(width);
    style.height = ui_layout_fixed(kDefaultMessageReactionLayoutMetrics.button_height);
    style.padding = UiInsets{kDefaultMessageReactionLayoutMetrics.content_left_padding,
                             kDefaultMessageReactionLayoutMetrics.content_top_padding,
                             kDefaultMessageReactionLayoutMetrics.content_right_padding,
                             kDefaultMessageReactionLayoutMetrics.content_top_padding};
    return style;
}

UiWidgetResult add_group(UiWidgetManager* manager, UiWidgetHandle parent, u32 id, const char* name,
                         float height, bool reactions, float gap = 0.0f) noexcept {
    UiLayoutStyle style = reactions ? reaction_region_style(height) : region_style(height, UiLayoutFlow::vertical);
    if (!reactions)
        style.gap = gap;
    return ui_widget_manager_add_prevalidated(
        manager, parent,
        UiWidgetDesc{id, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{Utf8View{name, c_string_length(name)}, style, UiSize{}, 0.0f, id}});
}

UiWidgetResult add_item(UiWidgetManager* manager, UiWidgetHandle parent, u32 id, const char* name,
                        UiWidgetKind kind, u16 flags, float height) noexcept {
    return ui_widget_manager_add_prevalidated(
        manager, parent,
        UiWidgetDesc{id, kind, flags,
                     UiLayoutNodeSpec{Utf8View{name, c_string_length(name)}, item_style(height), UiSize{},
                                      0.0f, id}});
}

UiWidgetResult add_reaction(UiWidgetManager* manager, UiWidgetHandle parent, u32 id, float width) noexcept {
    return ui_widget_manager_add_prevalidated(
        manager, parent,
        UiWidgetDesc{id, UiWidgetKind::button,
                     static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
                     UiLayoutNodeSpec{utf8_literal("ReactionButton"), reaction_button_style(width),
                                      UiSize{}, 0.0f, id}});
}

bool set_fixed_height(UiWidgetManager* manager, UiWidgetHandle handle, float height) noexcept {
    if (!ui_extent_valid(height)) {
        return false;
    }
    UiWidget* widget = ui_widget_manager_resolve(manager, handle);
    UiLayoutNode* node = widget ? ui_layout_engine_resolve(&manager->layout, widget->layout) : nullptr;
    if (!node) {
        return false;
    }
    node->spec.style.height = ui_layout_fixed(height);
    return true;
}

bool set_fixed_width(UiWidgetManager* manager, UiWidgetHandle handle, float width) noexcept {
    if (!ui_extent_valid(width) || width <= 0.0f) {
        return false;
    }
    UiWidget* widget = ui_widget_manager_resolve(manager, handle);
    UiLayoutNode* node = widget ? ui_layout_engine_resolve(&manager->layout, widget->layout) : nullptr;
    if (!node) {
        return false;
    }
    node->spec.style.width = ui_layout_fixed(width);
    return true;
}

bool set_interaction(UiWidgetManager* manager, UiWidgetHandle handle, bool interactive) noexcept {
    UiWidget* widget = ui_widget_manager_resolve(manager, handle);
    if (!widget) {
        return false;
    }
    widget->kind = interactive ? UiWidgetKind::button : UiWidgetKind::container;
    widget->flags =
        static_cast<u16>(ui_widget_enabled | (interactive ? ui_widget_hit_test | ui_widget_focusable : 0));
    return true;
}

bool build_preflight(UiWidgetManager& manager, UiWidgetHandle parent, u32 row_widget_id,
                     const MessageRichRecord* rich, bool poll, u32 invites) noexcept {
    const u32 reaction_count = rich ? rich->reaction_count : 0;
    const u32 attachment_count = rich ? rich->attachment_count : 0;
    const u32 embed_count = rich ? rich->embed_count : 0;
    const u32 sticker_count = rich ? rich->sticker_count : 0;
    const u32 component_count = rich ? rich->component_count : 0;
    const u32 notice_count = rich && rich->truncations != 0 ? 1u : 0u;
    if (reaction_count > kGatewayMessageReactionMaximum ||
        attachment_count > kGatewayMessageAttachmentMaximum || embed_count > kGatewayMessageEmbedMaximum ||
        sticker_count > kGatewayMessageStickerMaximum ||
        component_count > kGatewayMessageComponentMaximum ||
        row_widget_id > ~0u - (kMessageSceneWidgetsPerRow - 1u)) {
        return false;
    }
    const UiWidget* parent_widget = ui_widget_manager_resolve(manager, parent);
    const UiLayoutNode* parent_node =
        parent_widget ? ui_layout_engine_resolve(manager.layout, parent_widget->layout) : nullptr;
    if (invites > kMessageInviteCardMaximum)
        return false;
    const u32 child_count =
        reaction_count + attachment_count + embed_count + sticker_count + component_count + notice_count + invites;
    const u32 required = 8u + child_count + (poll ? 1u : 0u);
    u32 extra_depth = child_count != 0 ? 2u : 1u;
    u8 component_depth[kGatewayMessageComponentMaximum]{};
    for (u32 i = 0; i < component_count; ++i) {
        const u16 component_parent = rich->components[i].parent;
        if (component_parent != 0xFFFFu && component_parent >= i)
            return false;
        component_depth[i] =
            component_parent == 0xFFFFu ? 2 : static_cast<u8>(component_depth[component_parent] + 1);
        extra_depth = maximum(extra_depth, static_cast<u32>(component_depth[i]));
    }
    if (!parent_node || manager.count > manager.storage.count ||
        manager.layout.count > manager.layout.storage.count ||
        static_cast<u32>(parent_node->depth) + extra_depth >= kMaximumUiLayoutDepth ||
        required > kInvalidUiWidgetSlot - manager.count ||
        !ui_widget_manager_reserve(&manager, manager.count + required)) {
        return false;
    }
    return true;
}

} // namespace

void message_rich_widgets_reset_handles(MessageSceneRowWidget* row) noexcept {
    if (!row) {
        return;
    }
    row->attachments = ui_widget_invalid_handle();
    row->embeds = ui_widget_invalid_handle();
    row->stickers = ui_widget_invalid_handle();
    row->components = ui_widget_invalid_handle();
    row->reactions = ui_widget_invalid_handle();
    row->notice = ui_widget_invalid_handle();
    row->poll = ui_widget_invalid_handle();
    row->thread = ui_widget_invalid_handle();
    row->invites = ui_widget_invalid_handle();
    for (u32 index = 0; index < kGatewayMessageReactionMaximum; ++index) {
        row->reaction_buttons[index] = ui_widget_invalid_handle();
    }
    for (u32 index = 0; index < kMessageSceneRichItemMaximum; ++index) {
        row->rich_items[index] = ui_widget_invalid_handle();
    }
    row->reaction_button_count = 0;
    row->attachment_item_count = 0;
    row->embed_item_count = 0;
    row->sticker_item_count = 0;
    row->component_item_count = 0;
    row->notice_item_count = 0;
    row->invite_item_count = 0;
}

bool message_rich_widgets_structure_matches(const MessageSceneRowWidget& row, const MessageRichRecord* rich,
                                            bool poll, u32 invites) noexcept {
    const u8 reaction_count = rich ? rich->reaction_count : 0;
    const u8 attachment_count = rich ? rich->attachment_count : 0;
    const u8 embed_count = rich ? rich->embed_count : 0;
    const u8 sticker_count = rich ? rich->sticker_count : 0;
    const u8 component_count = rich ? rich->component_count : 0;
    const u8 notice_count = rich && rich->truncations != 0 ? 1 : 0;
    return (row.poll_content_revision != 0) == poll && (!poll || row.poll.slot != kInvalidUiWidgetSlot) &&
           row.reaction_button_count == reaction_count && row.attachment_item_count == attachment_count &&
           row.embed_item_count == embed_count && row.sticker_item_count == sticker_count &&
           row.component_item_count == component_count && row.notice_item_count == notice_count &&
           row.invite_item_count == invites && row.component_structure == message_component_structure(rich);
}

bool message_rich_widgets_build(UiWidgetManager* manager, UiWidgetHandle parent, u32 row_widget_id,
                                const MessageRichRecord* rich, MessageSceneRowWidget* row, u32 invites) noexcept {
    if (!manager || !row ||
        !build_preflight(*manager, parent, row_widget_id, rich, row->poll_content_revision != 0, invites)) {
        return false;
    }
    message_rich_widgets_reset_handles(row);
    if (row->poll_content_revision) {
        const auto poll =
            add_item(manager, parent, row_widget_id + kMessageSceneWidgetsPerContent - 1u, "Poll",
                     row->poll_unavailable ? UiWidgetKind::container : UiWidgetKind::button,
                     static_cast<u16>(ui_widget_enabled |
                                      (row->poll_unavailable ? ui_widget_block_input
                                                             : ui_widget_hit_test | ui_widget_focusable)),
                     row->poll_height);
        if (!poll.ok())
            return false;
        row->poll = poll.handle;
    }
    const UiWidgetResult attachments =
        add_group(manager, parent, row_widget_id + 6u, "Attachments", row->rich_layout.attachments, false);
    const UiWidgetResult embeds =
        add_group(manager, parent, row_widget_id + 7u, "Embeds", row->rich_layout.embeds, false);
    const UiWidgetResult stickers =
        add_group(manager, parent, row_widget_id + 8u, "Stickers", row->rich_layout.stickers, false);
    const UiWidgetResult components =
        add_group(manager, parent, row_widget_id + 9u, "Components", row->rich_layout.components, false,
                  kMessageComponentGap);
    const UiWidgetResult invite_group = add_group(manager, parent, row_widget_id + 13u, "Invites",
                                                  row->rich_layout.invites, false, kMessageInviteCardGap);
    const UiWidgetResult reactions =
        add_group(manager, parent, row_widget_id + 10u, "Reactions", row->rich_layout.reactions, true);
    const UiWidgetResult notice =
        add_group(manager, parent, row_widget_id + 11u, "Notice", row->rich_layout.notice, false);
    const bool thread_card = row->rich_layout.thread > 0.0f;
    const UiWidgetResult thread =
        add_item(manager, parent, row_widget_id + 12u, "ThreadCard",
                 thread_card ? UiWidgetKind::button : UiWidgetKind::container,
                 static_cast<u16>(ui_widget_enabled | (thread_card ? ui_widget_hit_test | ui_widget_focusable : 0)),
                 row->rich_layout.thread);
    if (!attachments.ok() || !embeds.ok() || !stickers.ok() || !components.ok() || !reactions.ok() ||
        !notice.ok() || !thread.ok() || !invite_group.ok()) {
        return false;
    }
    row->invites = invite_group.handle;
    row->invite_item_count = static_cast<u8>(invites);
    row->thread = thread.handle;
    row->attachments = attachments.handle;
    row->embeds = embeds.handle;
    row->stickers = stickers.handle;
    row->components = components.handle;
    row->reactions = reactions.handle;
    row->notice = notice.handle;
    row->reaction_button_count = rich ? rich->reaction_count : 0;
    row->attachment_item_count = rich ? rich->attachment_count : 0;
    row->embed_item_count = rich ? rich->embed_count : 0;
    row->sticker_item_count = rich ? rich->sticker_count : 0;
    row->component_item_count = rich ? rich->component_count : 0;
    row->notice_item_count = rich && rich->truncations != 0 ? 1 : 0;
    row->component_structure = message_component_structure(rich);

    for (u32 index = 0; index < row->reaction_button_count; ++index) {
        const UiWidgetResult button =
            add_reaction(manager, reactions.handle, row_widget_id + kMessageSceneBaseWidgetsPerRow + index,
                         row->reaction_widths[index] > 0.0f
                             ? row->reaction_widths[index]
                             : message_rich_reaction_button_width(rich->reactions[index]));
        if (!button.ok() ||
            (!message_rich_display().reactions && !set_interaction(manager, button.handle, false))) {
            return false;
        }
        row->reaction_buttons[index] = button.handle;
    }

    const u32 rich_id = row_widget_id + kMessageSceneBaseWidgetsPerRow + kGatewayMessageReactionMaximum;
    for (u32 index = 0; index < row->attachment_item_count; ++index) {
        const u32 slot = kMessageSceneAttachmentItemOffset + index;
        const bool interactive = rich && rich->attachments[index].url.bytes != 0;
        const UiWidgetResult item =
            add_item(manager, attachments.handle, rich_id + slot, "AttachmentItem",
                     interactive ? UiWidgetKind::button : UiWidgetKind::container,
                     static_cast<u16>(ui_widget_enabled |
                                      (interactive ? ui_widget_hit_test | ui_widget_focusable : 0)),
                     row->rich_item_layout.heights[slot]);
        if (!item.ok()) {
            return false;
        }
        row->rich_items[slot] = item.handle;
    }
    for (u32 index = 0; index < row->embed_item_count; ++index) {
        const u32 slot = kMessageSceneEmbedItemOffset + index;
        const bool interactive = rich && rich->embeds[index].url.bytes != 0 && message_rich_display().embeds;
        const UiWidgetResult item =
            add_item(manager, embeds.handle, rich_id + slot, "EmbedItem",
                     interactive ? UiWidgetKind::button : UiWidgetKind::container,
                     static_cast<u16>(ui_widget_enabled |
                                      (interactive ? ui_widget_hit_test | ui_widget_focusable : 0)),
                     row->rich_item_layout.heights[slot]);
        if (!item.ok()) {
            return false;
        }
        row->rich_items[slot] = item.handle;
    }
    for (u32 index = 0; index < row->sticker_item_count; ++index) {
        const u32 slot = kMessageSceneStickerItemOffset + index;
        const UiWidgetResult item =
            add_item(manager, stickers.handle, rich_id + slot, "StickerItem", UiWidgetKind::button,
                     static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
                     row->rich_item_layout.heights[slot]);
        if (!item.ok()) {
            return false;
        }
        row->rich_items[slot] = item.handle;
    }
    for (u32 index = 0; index < row->component_item_count; ++index) {
        const u32 slot = kMessageSceneComponentItemOffset + index;
        const bool interactive = rich && message_component_interactive(rich->components[index]);
        const u16 parent_index = rich->components[index].parent;
        const auto component_parent =
            parent_index == 0xFFFFu ? components.handle
                                    : row->rich_items[kMessageSceneComponentItemOffset + parent_index];
        const UiWidgetResult item = ui_widget_manager_add_prevalidated(
            manager, component_parent,
            UiWidgetDesc{rich_id + slot,
                         interactive ? UiWidgetKind::button : UiWidgetKind::container,
                         static_cast<u16>(ui_widget_enabled |
                                          (interactive ? ui_widget_hit_test | ui_widget_focusable : 0)),
                         {utf8_literal("ComponentItem"),
                          message_component_style(*rich, index, row->rich_item_layout),
                          {},
                          0,
                          rich_id + slot}});
        if (!item.ok()) {
            return false;
        }
        row->rich_items[slot] = item.handle;
    }
    for (u32 index = 0; index < row->invite_item_count; ++index) {
        const u32 slot = kMessageRichInviteItemOffset + index;
        const UiWidgetResult item =
            add_item(manager, invite_group.handle, rich_id + slot, "InviteItem", UiWidgetKind::button,
                     static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
                     row->rich_item_layout.heights[slot]);
        if (!item.ok())
            return false;
        row->rich_items[slot] = item.handle;
    }
    if (row->notice_item_count != 0) {
        const u32 slot = kMessageSceneNoticeItemOffset;
        const UiWidgetResult item =
            add_item(manager, notice.handle, rich_id + slot, "NoticeItem", UiWidgetKind::text,
                     ui_widget_enabled, row->rich_item_layout.heights[slot]);
        if (!item.ok()) {
            return false;
        }
        row->rich_items[slot] = item.handle;
    }
    return true;
}

bool message_rich_widgets_apply(UiWidgetManager* manager, const MessageRichRecord* rich,
                                MessageSceneRowWidget* row) noexcept {
    if (!manager || !row ||
        !message_rich_widgets_structure_matches(*row, rich, row->poll_content_revision != 0,
                                                row->invite_item_count) ||
        !set_fixed_height(manager, row->invites, row->rich_layout.invites) ||
        (row->poll_content_revision && (!set_fixed_height(manager, row->poll, row->poll_height) ||
                                        !set_interaction(manager, row->poll, !row->poll_unavailable))) ||
        !set_fixed_height(manager, row->attachments, row->rich_layout.attachments) ||
        !set_fixed_height(manager, row->embeds, row->rich_layout.embeds) ||
        !set_fixed_height(manager, row->stickers, row->rich_layout.stickers) ||
        !set_fixed_height(manager, row->components, row->rich_layout.components) ||
        !set_fixed_height(manager, row->reactions, row->rich_layout.reactions) ||
        !set_fixed_height(manager, row->notice, row->rich_layout.notice) ||
        !set_fixed_height(manager, row->thread, row->rich_layout.thread) ||
        !set_interaction(manager, row->thread, row->rich_layout.thread > 0.0f)) {
        return false;
    }
    if (row->poll_unavailable) {
        auto* poll = ui_widget_manager_resolve(manager, row->poll);
        if (!poll)
            return false;
        poll->flags |= ui_widget_block_input;
    }
    for (u32 index = 0; index < row->reaction_button_count; ++index) {
        if (!rich || !set_fixed_width(manager, row->reaction_buttons[index],
                                      row->reaction_widths[index] > 0.0f
                                          ? row->reaction_widths[index]
                                          : message_rich_reaction_button_width(rich->reactions[index])) ||
            !set_interaction(manager, row->reaction_buttons[index], message_rich_display().reactions)) {
            return false;
        }
    }
    const u32 counts[6]{row->attachment_item_count, row->embed_item_count, row->sticker_item_count,
                        row->component_item_count, row->notice_item_count, row->invite_item_count};
    const MessageSceneRichItemKind kinds[6]{
        MessageSceneRichItemKind::attachment, MessageSceneRichItemKind::embed,
        MessageSceneRichItemKind::sticker, MessageSceneRichItemKind::component,
        MessageSceneRichItemKind::notice, MessageSceneRichItemKind::invite};
    for (u32 kind_index = 0; kind_index < 6; ++kind_index) {
        for (u32 item = 0; item < counts[kind_index]; ++item) {
            u32 slot = 0;
            if (!message_rich_item_slot(kinds[kind_index], item, &slot) ||
                !set_fixed_height(manager, row->rich_items[slot], row->rich_item_layout.heights[slot])) {
                return false;
            }
        }
    }
    for (u32 index = 0; index < row->component_item_count; ++index) {
        const bool interactive = rich && message_component_interactive(rich->components[index]);
        auto* widget =
            ui_widget_manager_resolve(manager, row->rich_items[kMessageSceneComponentItemOffset + index]);
        auto* node = widget ? ui_layout_engine_resolve(&manager->layout, widget->layout) : nullptr;
        if (!node || !rich)
            return false;
        node->spec.style = message_component_style(*rich, index, row->rich_item_layout);
        if (!set_interaction(manager, row->rich_items[kMessageSceneComponentItemOffset + index],
                             interactive)) {
            return false;
        }
    }
    for (u32 index = 0; index < row->attachment_item_count; ++index) {
        if (!set_interaction(manager, row->rich_items[kMessageSceneAttachmentItemOffset + index],
                             rich && rich->attachments[index].url.bytes != 0))
            return false;
    }
    for (u32 index = 0; index < row->embed_item_count; ++index) {
        const bool interactive = rich && rich->embeds[index].url.bytes != 0 && message_rich_display().embeds;
        if (!set_interaction(manager, row->rich_items[kMessageSceneEmbedItemOffset + index], interactive)) {
            return false;
        }
    }
    return true;
}

} // namespace maicord
