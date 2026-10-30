#pragma once

#include "maicord/ui/theme.hpp"

namespace maicord {
struct MessageRichComponent;
struct MessageRichItemLayout;
struct MessageRichRecord;
struct UiLayoutStyle;
struct UiRect;

struct Theme;

// Official client metrics (Components V1 buttons/selects, V2 layout): a
// container pads 16 and stacks children 8 apart; an action row wraps its
// controls 8 apart; V2 content is at most 600 wide.
inline constexpr float kMessageComponentPadding = 16.0f;
inline constexpr float kMessageComponentGap = 8.0f;
inline constexpr float kMessageComponentMaximumWidth = 600.0f;
// Top-level components of a legacy (non-V2) message: action rows 6 apart.
inline constexpr float kMessageComponentLegacyRootGap = 6.0f;
// Small buttons: 32 tall, 12 from the border box to the content, at least 60
// wide; a 14-DIP label follows a 1.375em emoji and 4 of space.
inline constexpr float kMessageComponentButtonHeight = 32.0f;
inline constexpr float kMessageComponentButtonPadding = 12.0f;
inline constexpr float kMessageComponentButtonMinimumWidth = 60.0f;
inline constexpr float kMessageComponentEmojiExtent = 19.25f;
inline constexpr float kMessageComponentEmojiGap = 4.0f;
// A link button's trailing launch icon and its gap from the label.
inline constexpr float kMessageComponentLinkIconExtent = 16.0f;
inline constexpr float kMessageComponentLinkIconGap = 8.0f;
inline constexpr float kMessageComponentSelectHeight = 40.0f;
inline constexpr float kMessageComponentSelectMaximumWidth = 400.0f;
// Selects pad 12 before the value; the 18-DIP chevron sits 8 from the end,
// 8 after the value.
inline constexpr float kMessageComponentSelectPadding = 12.0f;
inline constexpr float kMessageComponentSelectEndPadding = 8.0f;
inline constexpr float kMessageComponentSelectIconLane = 34.0f;
inline constexpr float kMessageComponentThumbnailExtent = 85.0f;
inline constexpr float kMessageComponentFileHeight = 56.0f;
inline constexpr float kMessageComponentGalleryMaximumWidth = 600.0f;
inline constexpr float kMessageComponentGalleryGap = 4.0f;
inline constexpr float kMessageComponentTextEstimate = 20.0f;
// Sections: 12 between the text column and the accessory, 4 between texts.
inline constexpr float kMessageComponentSectionGap = 12.0f;
inline constexpr float kMessageComponentSectionTextGap = 4.0f;
// Separators: spacing 2 (large) adds 8 above and below; a divider is a 1-DIP rule.
inline constexpr float kMessageComponentSeparatorLargeMargin = 8.0f;

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
// Gallery mosaic: 1 item keeps its aspect (`aspect` = height / width, zero
// for 16:9) up to 350 tall, 2 side by side, 3 is one large plus two stacked,
// 4 is a 2x2 grid, larger galleries lead with count % 3 items over rows of three.
UiRect message_component_gallery_cell(u32 count, u32 index, float width, float aspect = 0) noexcept;
float message_component_gallery_height(u32 count, float width, float aspect = 0) noexcept;
// Height / width of a thumbnail or gallery item from its media metadata, or
// zero when the size is unknown. Media items keep width << 16 | height in
// accent_color.
float message_component_media_aspect(const MessageRichComponent& component) noexcept;
// The aspect of a gallery's only item, else zero.
float message_component_gallery_aspect(const MessageRichRecord& rich, u32 gallery) noexcept;
u32 message_component_child_count(const MessageRichRecord& rich, u32 parent, MessageComponentKind kind) noexcept;
u32 message_component_child_ordinal(const MessageRichRecord& rich, u32 index) noexcept;

// A failed interaction shows its error under the action row or section that
// holds the control: 4 above and below a 20-DIP line led by a 16-DIP icon 8
// before the text.
inline constexpr u32 kMessageComponentNone = 0xFFFFu;
inline constexpr float kMessageComponentErrorHeight = 28.0f;
inline constexpr float kMessageComponentErrorIconExtent = 16.0f;
inline constexpr float kMessageComponentErrorIconGap = 8.0f;
// The row or section that shows the error of the component at `index`, or
// kMessageComponentNone (the message annotation shows it instead).
u32 message_component_error_host(const MessageRichRecord& rich, u32 index) noexcept;
// The spoiler container at or above `index` (containers do not nest), or
// kMessageComponentNone. Until revealed it hides everything inside it.
u32 message_component_spoiler_container(const MessageRichRecord& rich, u32 index) noexcept;
// Widgets that take pointer input: interactive controls, and spoiler
// containers (a click anywhere inside reveals them).
bool message_component_hit_target(const MessageRichComponent& component) noexcept;

// The transport keeps parents before children. A reverse fold measures the
// tree once, without recursion, heap allocations, or double-counting children.
// With an available width, action rows wrap their buttons onto more lines
// (inside containers, within the container's padded lane). `error_host`
// (a row or section) grows by the error line.
float message_component_measure_tree(const MessageRichRecord& rich, MessageRichItemLayout* items,
                                     float available_width = 0, u32 error_host = kMessageComponentNone) noexcept;
// Structure hash: the tree shape and the root gap.
u64 message_component_structure(const MessageRichRecord* rich) noexcept;
// The gap between top-level components (V2 messages 8, legacy rows 6).
float message_component_root_gap(const MessageRichRecord* rich) noexcept;
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
