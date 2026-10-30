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
    // The parent's 8-DIP gaps frame every separator; spacing 2 (large) adds
    // 8 above and below, and a divider is a 1-DIP rule.
    const float rule = (component.flags & gateway_message_component_divider) ? 1.0f : 0.0f;
    return rule + (component.style >= 2 ? 2 * kMessageComponentSeparatorLargeMargin : 0.0f);
}

bool section_accessory(const MessageRichRecord& rich, u32 index) noexcept {
    const auto& component = rich.components[index];
    return component.parent != kNoParent && component.order == 0xFFFFu &&
           rich.components[component.parent].type == gateway_component_section;
}

// True when every child of the row at `index` is a button or select.
bool row_holds_only_buttons(const MessageRichRecord& rich, u32 index) noexcept {
    for (u32 j = index + 1; j < rich.component_count; ++j) {
        if (rich.components[j].parent != index)
            continue;
        const auto child = message_component_kind(rich.components[j]);
        if (child != MessageComponentKind::button && child != MessageComponentKind::select)
            return false;
    }
    return true;
}

// `top` composited source-over onto `bottom` (both straight alpha).
Color over(Color top, Color bottom) noexcept {
    const float under = bottom.alpha * (1 - top.alpha);
    const float alpha = top.alpha + under;
    if (alpha <= 0)
        return {};
    return {(top.red * top.alpha + bottom.red * under) / alpha, (top.green * top.alpha + bottom.green * under) / alpha,
            (top.blue * top.alpha + bottom.blue * under) / alpha, alpha};
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
    case MessageComponentKind::select:
    case MessageComponentKind::file:
        // Generous until a label is shaped: measuring inside a narrower
        // box would clip the natural width the next pass converges to.
        return minimum(width, kMessageComponentSelectMaximumWidth);
    case MessageComponentKind::thumbnail:
        return kMessageComponentThumbnailExtent;
    case MessageComponentKind::gallery:
        return minimum(width, kMessageComponentGalleryMaximumWidth);
    case MessageComponentKind::text:
    case MessageComponentKind::separator:
        return width;
    default:
        return 0.0f;
    }
}

namespace {
// Media mosaic heights: a lone item is capped at 350, a two-item row and a
// lead item above a grid at 280, the three- and four-item layouts at 350.
constexpr float kGallerySingleMaximum = 350.0f;
constexpr float kGalleryRowMaximum = 280.0f;
constexpr float kGalleryGridMaximum = 350.0f;

float gallery_pair_height(float width) noexcept {
    return minimum(kGalleryRowMaximum, (width - kMessageComponentGalleryGap) * 0.5f);
}
// Galleries of five or more lead with count % 3 items (one full-width item or
// a pair) above rows of three squares.
float gallery_lead_height(u32 count, float width) noexcept {
    const u32 lead = count % 3;
    return lead == 1   ? minimum(kGalleryRowMaximum, width * 0.5625f)
           : lead == 2 ? gallery_pair_height(width)
                       : 0.0f;
}
} // namespace

float message_component_media_aspect(const MessageRichComponent& component) noexcept {
    if (component.type != gateway_component_thumbnail && component.type != gateway_component_gallery_item)
        return 0.0f;
    const u32 width = component.accent_color >> 16, height = component.accent_color & 0xFFFFu;
    return width && height ? static_cast<float>(height) / static_cast<float>(width) : 0.0f;
}

float message_component_gallery_aspect(const MessageRichRecord& rich, u32 gallery) noexcept {
    u32 count = 0;
    float aspect = 0.0f;
    for (u32 i = gallery + 1; i < rich.component_count && count < 2; ++i)
        if (rich.components[i].parent == gallery && rich.components[i].type == gateway_component_gallery_item) {
            aspect = message_component_media_aspect(rich.components[i]);
            ++count;
        }
    return count == 1 ? aspect : 0.0f;
}

float message_component_gallery_height(u32 count, float width, float aspect) noexcept {
    if (count == 0 || width <= 0.0f)
        return 0.0f;
    const float gap = kMessageComponentGalleryGap;
    // A lone item keeps its own aspect (16:9 until its size is known).
    if (count == 1)
        return minimum(width * (aspect > 0.0f ? aspect : 0.5625f), kGallerySingleMaximum);
    if (count == 2)
        return gallery_pair_height(width);
    if (count == 3)
        return minimum(kGalleryGridMaximum, (width - gap) * 2.0f / 3.0f);
    if (count == 4)
        return 2.0f * minimum((kGalleryGridMaximum - gap) * 0.5f, (width - gap) * 0.5f) + gap;
    const float lead = gallery_lead_height(count, width);
    const u32 rows = (count - count % 3) / 3;
    const float triple = (width - 2.0f * gap) / 3.0f;
    return lead + (lead > 0 ? gap : 0.0f) + static_cast<float>(rows) * triple + static_cast<float>(rows - 1) * gap;
}

UiRect message_component_gallery_cell(u32 count, u32 index, float width, float aspect) noexcept {
    if (index >= count || width <= 0.0f)
        return {};
    const float gap = kMessageComponentGalleryGap;
    const float height = message_component_gallery_height(count, width, aspect);
    if (count == 1)
        return {0, 0, aspect > 0.0f ? minimum(width, height / aspect) : width, height};
    const float half = (width - gap) * 0.5f;
    if (count == 2) {
        const float left = index * (half + gap);
        return {left, 0, left + half, height};
    }
    if (count == 3) {
        // One item over two-thirds of the width beside two stacked ones.
        const float large = (width - gap) * 2.0f / 3.0f;
        const float small = (height - gap) * 0.5f;
        if (index == 0)
            return {0, 0, large, height};
        const float top = (index - 1) * (small + gap);
        return {large + gap, top, width, top + small};
    }
    if (count == 4) {
        const float row = (height - gap) * 0.5f;
        const float left = (index % 2) * (half + gap), top = (index / 2) * (row + gap);
        return {left, top, left + half, top + row};
    }
    const u32 lead = count % 3;
    const float lead_height = gallery_lead_height(count, width);
    if (index < lead) {
        if (lead == 1)
            return {0, 0, width, lead_height};
        const float left = index * (half + gap);
        return {left, 0, left + half, lead_height};
    }
    const float triple = (width - 2.0f * gap) / 3.0f;
    const u32 at = index - lead;
    const float left = (at % 3) * (triple + gap);
    const float top = (lead ? lead_height + gap : 0.0f) + (at / 3) * (triple + gap);
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

u32 message_component_error_host(const MessageRichRecord& rich, u32 index) noexcept {
    if (index >= rich.component_count)
        return kMessageComponentNone;
    const u16 parent = rich.components[index].parent;
    if (parent >= rich.component_count)
        return kMessageComponentNone;
    const auto kind = message_component_kind(rich.components[parent]);
    return kind == MessageComponentKind::row || kind == MessageComponentKind::section ? parent
                                                                                       : kMessageComponentNone;
}

u32 message_component_spoiler_container(const MessageRichRecord& rich, u32 index) noexcept {
    for (u32 at = index, depth = 0; at < rich.component_count && depth < kGatewayMessageComponentMaximum;
         at = rich.components[at].parent, ++depth)
        if (rich.components[at].type == gateway_component_container)
            return (rich.components[at].flags & gateway_message_component_spoiler) ? at : kMessageComponentNone;
    return kMessageComponentNone;
}

bool message_component_hit_target(const MessageRichComponent& component) noexcept {
    return message_component_interactive(component) ||
           (component.type == gateway_component_container && (component.flags & gateway_message_component_spoiler));
}

float message_component_measure_tree(const MessageRichRecord& rich, MessageRichItemLayout* items,
                                     float available_width, u32 error_host) noexcept {
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
            if (lane > 0 && width > lane && row_holds_only_buttons(rich, i)) {
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
            const float text = sum_height[i] + static_cast<float>(text_children > 1 ? text_children - 1 : 0) *
                                                   kMessageComponentSectionTextGap;
            height = maximum(text, accessory_height[i]);
            width = max_width[i] + (accessory_width[i] > 0 ? accessory_width[i] + kMessageComponentSectionGap : 0.0f);
            break;
        }
        case MessageComponentKind::gallery:
            // The text pass seeds the gallery's own width; cells never fold.
            height = message_component_gallery_height(
                message_component_child_count(rich, i, MessageComponentKind::gallery_item), width,
                message_component_gallery_aspect(rich, i));
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
        if (i == error_host)
            height += kMessageComponentErrorHeight;
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
    // Root components stack with the root gap between them.
    return total + static_cast<float>(roots > 1 ? roots - 1 : 0) * message_component_root_gap(&rich);
}

float message_component_root_gap(const MessageRichRecord* rich) noexcept {
    return rich && !rich->components_v2 ? kMessageComponentLegacyRootGap : kMessageComponentGap;
}

u64 message_component_structure(const MessageRichRecord* rich) noexcept {
    u64 hash = kHash64Seed;
    for (u32 i = 0; rich && i < rich->component_count; ++i) {
        hash = hash64_mix(hash, rich->components[i].parent);
        hash = hash64_mix(hash, rich->components[i].order);
        hash = hash64_mix(hash, rich->components[i].type);
    }
    return rich && rich->components_v2 ? hash64_mix(hash, 1) : hash;
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
        const bool fixed = row_holds_only_buttons(rich, index);
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
            // Text stacks down the leading column, 4 apart; the accessory
            // keeps its lane 12 away. Beside a button the column is centred.
            float top = 0, column = 0;
            u32 texts = 0;
            float accessory = 0, accessory_height = 0;
            bool button = false;
            for (u32 i = parent + 1u; i < rich.component_count; ++i) {
                if (rich.components[i].parent != parent)
                    continue;
                const float child = items.heights[kMessageRichComponentItemOffset + i];
                if (section_accessory(rich, i)) {
                    accessory = items.component_widths[i];
                    accessory_height = child;
                    button = rich.components[i].type == gateway_component_button;
                    continue;
                }
                if (i < index)
                    top += child + kMessageComponentSectionTextGap;
                column += child + (texts++ ? kMessageComponentSectionTextGap : 0.0f);
            }
            if (button && accessory_height > column)
                top += (accessory_height - column) * 0.5f;
            style.margin.top = top;
            style.margin.right = accessory > 0 ? accessory + kMessageComponentSectionGap : 0.0f;
            style.horizontal_alignment = UiLayoutAlignment::stretch;
        }
    } else if (parent_kind == MessageComponentKind::gallery) {
        const u32 count = message_component_child_count(rich, parent, MessageComponentKind::gallery_item);
        const UiRect cell = message_component_gallery_cell(count, message_component_child_ordinal(rich, index),
                                                           items.component_widths[parent],
                                                           message_component_gallery_aspect(rich, parent));
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
    // Filled buttons carry a faint light rim; the neutral fill is
    // translucent, so it reads over any message background.
    colors.border = tokens.border.subtle;
    switch (static_cast<MessageComponentButtonStyle>(style)) {
    case MessageComponentButtonStyle::primary:
    case MessageComponentButtonStyle::premium:
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
    default: // secondary and link
        colors.fill = tokens.fill.muted;
        colors.text = theme.text_primary;
        break;
    }
    colors.hover = over(tokens.state.hover, colors.fill);
    if (disabled) {
        colors.fill.alpha *= 0.5f;
        colors.border.alpha *= 0.5f;
        colors.hover = colors.fill;
        colors.text.alpha *= 0.5f;
    }
    return colors;
}

} // namespace maicord
