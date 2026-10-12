#pragma once

#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/ui/layout_engine.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {

struct Theme;

inline constexpr float kMessageComponentPadding = 16.0f;
inline constexpr float kMessageComponentGap = 8.0f;
inline constexpr float kMessageComponentMaximumWidth = 432.0f;
inline constexpr float kMessageComponentButtonHeight = 32.0f;
inline constexpr float kMessageComponentButtonPadding = 16.0f;
inline constexpr float kMessageComponentButtonMinimumWidth = 60.0f;
inline constexpr float kMessageComponentEmojiExtent = 18.0f;
inline constexpr float kMessageComponentSelectHeight = 40.0f;
inline constexpr float kMessageComponentSelectMaximumWidth = 400.0f;
inline constexpr float kMessageComponentThumbnailExtent = 85.0f;
inline constexpr float kMessageComponentFileHeight = 56.0f;
inline constexpr float kMessageComponentGalleryMaximumWidth = 520.0f;
inline constexpr float kMessageComponentGalleryGap = 4.0f;
inline constexpr float kMessageComponentTextEstimate = 20.0f;

// Presentation role of a stored component. Select options are data for the
// select popup and never occupy layout; gallery items are grid cells.
enum class MessageComponentKind : u8 {
    hidden,
    row,
    button,
    select,
    text,
    section,
    thumbnail,
    gallery,
    gallery_item,
    file,
    separator,
    container,
};

// API button style (the component's style field).
enum class MessageComponentButtonStyle : u8 {
    primary = 1,
    secondary = 2,
    success = 3,
    danger = 4,
    link = 5,
    premium = 6,
};

MessageComponentKind message_component_kind(const MessageRichComponent& component) noexcept;
// Buttons that dispatch an interaction or open a link, and enabled selects.
// Premium (SKU) buttons are shown but inert.
bool message_component_interactive(const MessageRichComponent& component) noexcept;
bool message_component_is_link(const MessageRichComponent& component) noexcept;
// Pre-measurement extent of a leaf (zero for parents and hidden nodes).
float message_component_leaf_height(const MessageRichComponent& component) noexcept;
// Leaf width before text measurement; available is the component region width.
float message_component_leaf_width(const MessageRichComponent& component, float available) noexcept;
// Gallery grid: 1 item fills, 2 side by side, 3 is one large plus two stacked,
// 4 is a 2x2 grid, and larger galleries use rows of three after a top pair.
UiRect message_component_gallery_cell(u32 count, u32 index, float width) noexcept;
float message_component_gallery_height(u32 count, float width) noexcept;
u32 message_component_child_count(const MessageRichRecord& rich, u32 parent, MessageComponentKind kind) noexcept;
u32 message_component_child_ordinal(const MessageRichRecord& rich, u32 index) noexcept;

// The transport keeps parents before children. A reverse fold measures the
// tree once, without recursion, heap allocations, or double-counting children.
// With an available width, action rows wrap their buttons onto more lines
// (inside containers, within the container's padded lane), as the official client does.
float message_component_measure_tree(const MessageRichRecord& rich, MessageRichItemLayout* items,
                                     float available_width = 0) noexcept;
u64 message_component_structure(const MessageRichRecord* rich) noexcept;
UiLayoutStyle message_component_style(const MessageRichRecord& rich, u32 index,
                                     const MessageRichItemLayout& items) noexcept;

struct MessageComponentButtonColors {
    Color fill;
    Color hover;
    Color text;
    Color border;
};
MessageComponentButtonColors message_component_button_colors(const Theme& theme, u16 style,
                                                             bool disabled) noexcept;
} // namespace maicord
