#pragma once
#include "application_plugins.hpp"

namespace maicord::app_detail {
// Popover rows as popup items: buttons and links enabled, labels inert,
// separators fold into the next row. Labels borrow nodes.
inline u32 application_plugins_popover_items(const NativePluginSdkPopoverNode* nodes, u32 count,
                                             Slice<PopupItem> output) noexcept {
    u32 used = 0;
    bool separator = false;
    for (u32 i = 0; nodes && i < count && used < output.count; ++i) {
        const auto& node = nodes[i];
        if (node.kind == MC_NODE_SEPARATOR) {
            separator = used != 0;
            continue;
        }
        const Utf8View label{node.text, minimum(node.text_bytes, static_cast<u32>(sizeof(node.text)))};
        if (!label.bytes) continue;
        const bool enabled = (node.kind == MC_NODE_BUTTON && node.action_id) || (node.kind == MC_NODE_LINK && node.value_bytes);
        u16 flags = enabled ? popup_item_enabled : popup_item_none;
        if (node.tone == MC_TONE_CRITICAL) flags = static_cast<u16>(flags | popup_item_danger);
        if (separator) flags = static_cast<u16>(flags | popup_item_separator_before);
        separator = false;
        PopupItem item{};
        item.label = label;
        item.action = kApplicationPluginPopoverActionFirst + i;
        item.flags = flags;
        if (node.icon < static_cast<u16>(UiIcon::count))
            item.presentation = ui_menu_row_presentation(static_cast<UiIcon>(node.icon));
        else if (node.kind == MC_NODE_LINK)
            item.presentation = ui_menu_row_presentation(UiIcon::link);
        output[used++] = item;
    }
    return used;
}
} // namespace maicord::app_detail
