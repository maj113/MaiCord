#include "maicord/ui/message_component_layout.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/layout_engine.hpp"
#include "maicord/ui/message_rich_layout.hpp"

namespace maicord {
namespace {

constexpr u16 kNoParent = 0xFFFFu;

float separator_height(const MessageRichComponent& component) noexcept {
    // Separator spacing 1 (small) and 2 (large) frame an optional 1 DIP rule.
    return component.style >= 2 ? 32.0f : 16.0f;
}

bool section_accessory(const MessageRichRecord& rich, u32 index) noexcept {
    const auto& component = rich.components[index];
    return component.parent != kNoParent && component.order == 0xFFFFu &&
           rich.components[component.parent].type == gateway_component_section;
}

Color mix(Color a, Color b, float t) noexcept {
    return {a.red + (b.red - a.red) * t, a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t,
            a.alpha + (b.alpha - a.alpha) * t};
}

} // namespace

MessageComponentKind message_component_kind(const MessageRichComponent& component) noexcept {
    switch (component.type) {
    case gateway_component_action_row:
        return MessageComponentKind::row;
    case gateway_component_button:
        return MessageComponentKind::button;
    case gateway_component_string_select:
    case gateway_component_user_select:
    case gateway_component_role_select:
    case gateway_component_mentionable_select:
    case gateway_component_channel_select:
        return MessageComponentKind::select;
    case gateway_component_section:
        return MessageComponentKind::section;
    case gateway_component_text_display:
        return MessageComponentKind::text;
    case gateway_component_thumbnail:
        return MessageComponentKind::thumbnail;
    case gateway_component_media_gallery:
        return MessageComponentKind::gallery;
    case gateway_component_gallery_item:
        return MessageComponentKind::gallery_item;
    case gateway_component_file:
        return MessageComponentKind::file;
    case gateway_component_separator:
        return MessageComponentKind::separator;
    case gateway_component_container:
        return MessageComponentKind::container;
    default:
        // Unknown wire types still carry text when present.
        return component.content.bytes != 0 ? MessageComponentKind::text : MessageComponentKind::hidden;
    }
}

bool message_component_is_link(const MessageRichComponent& component) noexcept {
    return component.type == gateway_component_button &&
           component.style == static_cast<u16>(MessageComponentButtonStyle::link) && component.url.bytes != 0;
}

bool message_component_interactive(const MessageRichComponent& component) noexcept {
    if (component.disabled)
        return false;
    if (component.type == gateway_component_button)
        return message_component_is_link(component) ||
               (component.style >= 1 && component.style <= 4 && component.custom_id.bytes != 0);
    return gateway_component_is_select(component.type) && component.custom_id.bytes != 0;
}

float message_component_leaf_height(const MessageRichComponent& component) noexcept {
    switch (message_component_kind(component)) {
    case MessageComponentKind::button:
        return kMessageComponentButtonHeight;
    case MessageComponentKind::select:
        return kMessageComponentSelectHeight;
    case MessageComponentKind::text:
        return component.content.bytes != 0 ? kMessageComponentTextEstimate : 0.0f;
    case MessageComponentKind::thumbnail:
        return kMessageComponentThumbnailExtent;
    case MessageComponentKind::file:
        return kMessageComponentFileHeight;
    case MessageComponentKind::separator:
        return separator_height(component);
    default:
        return 0.0f;
    }
}

float message_component_leaf_width(const MessageRichComponent& component, float available) noexcept {
    const float width = maximum(1.0f, available);
    switch (message_component_kind(component)) {
    case MessageComponentKind::button:
        // Generous until the label is shaped: measuring inside a narrower
        // box would clip the natural width the next pass converges to.
        return minimum(width, kMessageComponentSelectMaximumWidth);
    case MessageComponentKind::select:
        return minimum(width, kMessageComponentSelectMaximumWidth);
    case MessageComponentKind::thumbnail:
        return kMessageComponentThumbnailExtent;
    case MessageComponentKind::gallery:
        return minimum(width, kMessageComponentGalleryMaximumWidth);
    case MessageComponentKind::file:
        return minimum(width, kMessageComponentSelectMaximumWidth);
    case MessageComponentKind::text:
    case MessageComponentKind::separator:
        return width;
    default:
        return 0.0f;
    }
}

float message_component_gallery_height(u32 count, float width) noexcept {
    if (count == 0 || width <= 0.0f)
        return 0.0f;
    const float gap = kMessageComponentGalleryGap;
    if (count == 1)
        return minimum(width * 0.5625f, 300.0f);
    if (count == 2)
        return (width - gap) * 0.5f;
    if (count == 3)
        return width * 2.0f / 3.0f;
    if (count == 4)
        return width; // two square rows of (width - gap) / 2 plus the gap
    // A top pair followed by rows of three.
    const u32 rest = count - 2;
    const u32 rows = (rest + 2) / 3;
    const float pair = (width - gap) * 0.5f;
    const float triple = (width - 2.0f * gap) / 3.0f;
    return pair + static_cast<float>(rows) * (triple + gap);
}

UiRect message_component_gallery_cell(u32 count, u32 index, float width) noexcept {
    if (index >= count || width <= 0.0f)
        return {};
    const float gap = kMessageComponentGalleryGap;
    const float height = message_component_gallery_height(count, width);
    if (count == 1)
        return {0, 0, width, height};
    if (count == 2) {
        const float cell = (width - gap) * 0.5f;
        const float left = index * (cell + gap);
        return {left, 0, left + cell, cell};
    }
    if (count == 3) {
        const float large = width * 2.0f / 3.0f - gap * 0.5f;
        const float small = (height - gap) * 0.5f;
        if (index == 0)
            return {0, 0, large, height};
        const float top = (index - 1) * (small + gap);
        return {large + gap, top, width, top + small};
    }
    if (count == 4) {
        const float cell = (width - gap) * 0.5f;
        const float left = (index % 2) * (cell + gap), top = (index / 2) * (cell + gap);
        return {left, top, left + cell, top + cell};
    }
    const float pair = (width - gap) * 0.5f;
    if (index < 2) {
        const float left = index * (pair + gap);
        return {left, 0, left + pair, pair};
    }
    const float triple = (width - 2.0f * gap) / 3.0f;
    const u32 at = index - 2;
    const float left = (at % 3) * (triple + gap);
    const float top = pair + gap + (at / 3) * (triple + gap);
    return {left, top, left + triple, top + triple};
}

u32 message_component_child_count(const MessageRichRecord& rich, u32 parent, MessageComponentKind kind) noexcept {
    u32 count = 0;
    for (u32 i = parent + 1; i < rich.component_count; ++i)
        if (rich.components[i].parent == parent && message_component_kind(rich.components[i]) == kind)
            ++count;
    return count;
}

u32 message_component_child_ordinal(const MessageRichRecord& rich, u32 index) noexcept {
    const u16 parent = rich.components[index].parent;
    u32 ordinal = 0;
    for (u32 i = 0; i < index; ++i)
        if (rich.components[i].parent == parent && rich.components[i].type == rich.components[index].type)
            ++ordinal;
    return ordinal;
}

float message_component_measure_tree(const MessageRichRecord& rich, MessageRichItemLayout* items,
                                     float available_width) noexcept {
    if (!items)
        return 0.0f;
    // Per-parent accumulators for the reverse fold.
    float sum_height[kGatewayMessageComponentMaximum]{}, max_height[kGatewayMessageComponentMaximum]{};
    float sum_width[kGatewayMessageComponentMaximum]{}, max_width[kGatewayMessageComponentMaximum]{};
    float accessory_height[kGatewayMessageComponentMaximum]{}, accessory_width[kGatewayMessageComponentMaximum]{};
    u8 children[kGatewayMessageComponentMaximum]{};
    float total = 0;
    u32 roots = 0;
    for (u32 cursor = rich.component_count; cursor != 0; --cursor) {
        const u32 i = cursor - 1;
        const auto& component = rich.components[i];
        float& height = items->heights[kMessageRichComponentItemOffset + i];
        float& width = items->component_widths[i];
        const u32 gaps = children[i] > 1 ? children[i] - 1u : 0u;
        switch (message_component_kind(component)) {
        case MessageComponentKind::row: {
            height = max_height[i];
            width = sum_width[i] + static_cast<float>(gaps) * kMessageComponentGap;
            const bool contained = component.parent < i && message_component_kind(rich.components[component.parent]) ==
                                                               MessageComponentKind::container;
            const float lane = available_width <= 0 ? 0.0f
                               : contained ? minimum(available_width, kMessageComponentMaximumWidth) -
                                                 2 * kMessageComponentPadding
                                           : available_width;
            bool buttons = true;
            for (u32 j = i + 1; buttons && j < rich.component_count; ++j) {
                const auto child = message_component_kind(rich.components[j]);
                buttons = rich.components[j].parent != i || child == MessageComponentKind::button ||
                          child == MessageComponentKind::select;
            }
            if (buttons && lane > 0 && width > lane) {
                // Replay the children in order to count the wrapped lines.
                u32 lines = 1;
                float line = 0, widest = 0;
                for (u32 j = i + 1; j < rich.component_count; ++j) {
                    if (rich.components[j].parent != i)
                        continue;
                    const float child = items->component_widths[j];
                    if (line > 0 && line + kMessageComponentGap + child > lane) {
                        ++lines;
                        widest = maximum(widest, line);
                        line = child;
                    } else {
                        line += (line > 0 ? kMessageComponentGap : 0.0f) + child;
                    }
                }
                width = maximum(widest, line);
                height = max_height[i] * static_cast<float>(lines) + static_cast<float>(lines - 1) * kMessageComponentGap;
            }
            break;
        }
        case MessageComponentKind::container:
            height = sum_height[i] + static_cast<float>(gaps) * kMessageComponentGap + 2 * kMessageComponentPadding;
            width = minimum(kMessageComponentMaximumWidth, max_width[i] + 2 * kMessageComponentPadding);
            break;
        case MessageComponentKind::section: {
            const u32 text_children = children[i] - (accessory_height[i] > 0 ? 1u : 0u);
            const float text = sum_height[i] +
                               static_cast<float>(text_children > 1 ? text_children - 1 : 0) * kMessageComponentGap;
            height = maximum(text, accessory_height[i]);
            width = max_width[i] + (accessory_width[i] > 0 ? accessory_width[i] + kMessageComponentPadding : 0.0f);
            break;
        }
        case MessageComponentKind::gallery:
            // The text pass seeds the gallery's own width; cells never fold.
            height = message_component_gallery_height(
                message_component_child_count(rich, i, MessageComponentKind::gallery_item), width);
            break;
        case MessageComponentKind::hidden:
        case MessageComponentKind::gallery_item:
            height = 0;
            if (component.type == gateway_component_select_option)
                width = 0;
            continue; // options and cells never contribute to the parent
        default:
            break; // measured leaf
        }
        if (component.parent < i) {
            const u32 parent = component.parent;
            if (section_accessory(rich, i)) {
                accessory_height[parent] = height;
                accessory_width[parent] = width;
                ++children[parent];
                continue;
            }
            if (message_component_kind(rich.components[parent]) == MessageComponentKind::select ||
                message_component_kind(rich.components[parent]) == MessageComponentKind::gallery)
                continue;
            ++children[parent];
            sum_height[parent] += height;
            max_height[parent] = maximum(max_height[parent], height);
            sum_width[parent] += width;
            max_width[parent] = maximum(max_width[parent], width);
        } else {
            total += height;
            ++roots;
        }
    }
    // Root components stack with the block gap between them.
    return total + static_cast<float>(roots > 1 ? roots - 1 : 0) * kMessageComponentGap;
}

u64 message_component_structure(const MessageRichRecord* rich) noexcept {
    u64 hash = kHash64Seed;
    for (u32 i = 0; rich && i < rich->component_count; ++i) {
        hash = hash64_mix(hash, rich->components[i].parent);
        hash = hash64_mix(hash, rich->components[i].order);
        hash = hash64_mix(hash, rich->components[i].type);
    }
    return hash;
}

UiLayoutStyle message_component_style(const MessageRichRecord& rich, u32 index,
                                     const MessageRichItemLayout& items) noexcept {
    const auto& component = rich.components[index];
    const auto kind = message_component_kind(component);
    const float height = items.heights[kMessageRichComponentItemOffset + index];
    const float width = items.component_widths[index];
    auto style = ui_layout_style();
    style.height = ui_layout_fixed(height);
    style.width = ui_layout_fill();
    style.flags = ui_layout_node_visible | ui_layout_node_allow_zero_size;
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    if (component.type == gateway_component_select_option || kind == MessageComponentKind::hidden) {
        style.width = ui_layout_fixed(0);
        style.height = ui_layout_fixed(0);
        style.flags = ui_layout_node_allow_zero_size;
        return style;
    }
    const u16 parent = component.parent;
    const auto parent_kind =
        parent != kNoParent ? message_component_kind(rich.components[parent]) : MessageComponentKind::hidden;
    switch (kind) {
    case MessageComponentKind::row: {
        // Buttons wrap onto further lines where the measure tree counted them;
        // rows holding anything else (a structural row around text) stay one line.
        bool fixed = true;
        for (u32 i = index + 1u; fixed && i < rich.component_count; ++i) {
            if (rich.components[i].parent != index)
                continue;
            const auto child = message_component_kind(rich.components[i]);
            fixed = child == MessageComponentKind::button || child == MessageComponentKind::select;
        }
        style.flow = fixed ? UiLayoutFlow::horizontal_wrap : UiLayoutFlow::horizontal;
        style.gap = kMessageComponentGap;
        break;
    }
    case MessageComponentKind::container:
        style.flow = UiLayoutFlow::vertical;
        style.gap = kMessageComponentGap;
        style.padding = {kMessageComponentPadding, kMessageComponentPadding, kMessageComponentPadding,
                         kMessageComponentPadding};
        // Its measured width at most: narrow messages shrink it to the row
        // instead of letting the card run past the timeline edge.
        style.maximum_size.width = minimum(kMessageComponentMaximumWidth, maximum(1.0f, width));
        style.width = ui_layout_fill();
        style.horizontal_alignment = UiLayoutAlignment::start;
        break;
    case MessageComponentKind::section:
    case MessageComponentKind::gallery:
    case MessageComponentKind::select:
        style.flow = UiLayoutFlow::overlay;
        break;
    default:
        break;
    }
    if (kind == MessageComponentKind::button || kind == MessageComponentKind::select ||
        kind == MessageComponentKind::thumbnail || kind == MessageComponentKind::gallery ||
        kind == MessageComponentKind::file) {
        style.width = ui_layout_fixed(maximum(1.0f, width));
        style.horizontal_alignment = UiLayoutAlignment::start;
    }
    if (parent_kind == MessageComponentKind::section) {
        style.vertical_alignment = UiLayoutAlignment::start;
        if (section_accessory(rich, index)) {
            style.horizontal_alignment = UiLayoutAlignment::end;
        } else {
            // Text stacks down the leading column; the accessory keeps its lane.
            float top = 0;
            float accessory = 0;
            for (u32 i = parent + 1u; i < rich.component_count; ++i) {
                if (rich.components[i].parent != parent)
                    continue;
                if (section_accessory(rich, i))
                    accessory = items.component_widths[i];
                else if (i < index)
                    top += items.heights[kMessageRichComponentItemOffset + i] + kMessageComponentGap;
            }
            style.margin.top = top;
            style.margin.right = accessory > 0 ? accessory + kMessageComponentPadding : 0.0f;
            style.horizontal_alignment = UiLayoutAlignment::stretch;
        }
    } else if (parent_kind == MessageComponentKind::gallery) {
        const u32 count = message_component_child_count(rich, parent, MessageComponentKind::gallery_item);
        const UiRect cell = message_component_gallery_cell(count, message_component_child_ordinal(rich, index),
                                                           items.component_widths[parent]);
        style.width = ui_layout_fixed(maximum(1.0f, ui_rect_width(cell)));
        style.height = ui_layout_fixed(maximum(1.0f, ui_rect_height(cell)));
        style.margin.left = cell.left;
        style.margin.top = cell.top;
        style.horizontal_alignment = UiLayoutAlignment::start;
        style.vertical_alignment = UiLayoutAlignment::start;
    } else if (parent_kind == MessageComponentKind::row) {
        style.vertical_alignment = UiLayoutAlignment::start;
    }
    return style;
}

MessageComponentButtonColors message_component_button_colors(const Theme& theme, u16 style,
                                                             bool disabled) noexcept {
    MessageComponentButtonColors colors{};
    const auto& tokens = theme.tokens;
    switch (static_cast<MessageComponentButtonStyle>(style)) {
    case MessageComponentButtonStyle::primary:
        colors.fill = tokens.emphasis.fill;
        colors.text = tokens.emphasis.text;
        break;
    case MessageComponentButtonStyle::success:
        colors.fill = tokens.accent.success;
        colors.text = tokens.text.on_accent;
        break;
    case MessageComponentButtonStyle::danger:
        colors.fill = tokens.accent.danger;
        colors.text = tokens.text.on_accent;
        break;
    default: // secondary, link, premium
        colors.fill = theme.interactive;
        colors.text = theme.text_primary;
        break;
    }
    colors.border = colors.fill;
    // Hover composites the theme's hover state over the fill.
    const Color hover = tokens.state.hover;
    colors.hover = mix(colors.fill, {hover.red, hover.green, hover.blue, colors.fill.alpha}, hover.alpha);
    if (disabled) {
        colors.fill.alpha *= 0.5f;
        colors.border.alpha *= 0.5f;
        colors.hover = colors.fill;
        colors.text.alpha *= 0.5f;
    }
    return colors;
}

} // namespace maicord
