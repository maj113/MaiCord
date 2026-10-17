#pragma once
#include "maicord/ui/surface_geometry.hpp"
#include "maicord/ui/edit_field.hpp"
#include "maicord/ui/menu_row.hpp"
#include <stddef.h>

#include "maicord/core/arena.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {

inline constexpr float kPopupRowHeight = kUiMenuCompactRowHeight;
inline constexpr float kPopupAvatarRowHeight = 44.0f;
inline constexpr float kPopupOuterMargin = 8.0f;
inline constexpr float kPopupPanelPadding = kUiMenuPadding;
inline constexpr u32 kPopupWidgetsPerItem = 2;
inline constexpr u32 kPopupQuickReactionCount = 4;
inline constexpr float kPopupQuickReactionExtent = 44.0f;
inline constexpr float kPopupQuickReactionGap = 4.0f;
inline constexpr float kPopupQuickReactionTop = 10.0f;
inline constexpr float kPopupQuickReactionBottomGap = 8.0f;
inline constexpr float kPopupContextSeparatorGap = 16.0f;

enum class PopupKind : u8 {
    none,
    context_menu,
    autocomplete,
    popover,
    tooltip,
    settings_choice,
};
enum class PopupPanelMaterial : u8 {
    tinted,
    gradient
};

struct UiPopupFeedbackStyle {
    float height, text_inset;
};
constexpr UiPopupFeedbackStyle ui_popup_feedback_style() noexcept {
    return {kUiMenuRowHeight, kUiMenuPadding};
}
// The host owns persistent text. A reserved footer is decorative, never a row
// or input target, and changing its text does not rebuild the widget tree.
struct UiPopupFeedback {
    Utf8View text;
    UiPopupFeedbackStyle style;
    bool error;
    // Optional slider under the text (fewer than two steps: none). The host owns
    // value and pointer input; see popup_feedback_slider.
    u16 slider_count, slider_value;
};

enum PopupItemFlag : u16 {
    popup_item_none = 0,
    popup_item_enabled = 1u << 0,
    popup_item_danger = 1u << 1,
    popup_item_separator_before = 1u << 2,
    popup_item_checked = 1u << 3,
    popup_item_command_header = 1u << 4,
    popup_item_command_close = 1u << 5,
    popup_item_emoji = 1u << 6,
    popup_item_custom_emoji = 1u << 7,
    popup_item_quick_reaction = 1u << 8,
    popup_item_avatar = 1u << 9,
    popup_item_emoji_preview = 1u << 12,
    popup_item_guild_icon = 1u << 13,
    popup_item_command_row = 1u << 14,
    popup_item_command_category = 1u << 15,
};

enum PopupShortcut : u16 {
    popup_shortcut_none,
    popup_shortcut_undo,
    popup_shortcut_redo,
    popup_shortcut_cut,
    popup_shortcut_copy,
    popup_shortcut_paste,
    popup_shortcut_delete,
    popup_shortcut_select_all,
    popup_shortcut_count,
};

struct PopupItem {
    Utf8View label;
    u32 action;
    u16 flags;
    // Standard edit-command hint (PopupShortcut); zero is none.
    u16 shortcut;
    u64 media_id;
    u8 media_flags;
    // Occupies the padding after media_flags: positional initializers leave it
    // unset, emoji rows assign it by name.
    u16 regional_flag; // Unicode preview identity; never a custom emoji snowflake.
    Utf8View detail;
    u32 media_variant;
    u8 status; // 0 unknown, 1 online, 2 idle, 3 busy, 4 offline
    // Published descriptors use aggregate/value initialization, not a default
    // constructor, so large constinit banks stay plain data.
    UiMenuRowPresentation presentation;
};
static_assert(sizeof(PopupItem) == 64);
// media_flags bit for rows without media: media_variant is a 0xRRGGBB tint
// for the leading menu icon (a role colour). Zero keeps the theme colour.
inline constexpr u8 kPopupItemMediaIconTint = 1u << 1;
static_assert(__is_trivial(PopupItem) && __is_standard_layout(PopupItem));
static_assert(offsetof(PopupItem, media_flags) == 32 && offsetof(PopupItem, regional_flag) == 34);
static_assert(offsetof(PopupItem, detail) == 40 && offsetof(PopupItem, media_variant) == 56);
static_assert(offsetof(PopupItem, status) == 60 && offsetof(PopupItem, presentation) == 61);

constexpr bool popup_item_menu_row(const PopupItem& item) noexcept {
    constexpr u16 specialized = popup_item_emoji | popup_item_custom_emoji | popup_item_quick_reaction |
                                popup_item_command_category | popup_item_command_header | popup_item_command_close;
    return (item.flags & specialized) == 0;
}

constexpr UiMenuRowPresentation popup_item_menu_presentation(const PopupItem& item) noexcept {
    UiMenuRowPresentation presentation = item.presentation;
    if (item.flags & popup_item_command_row) {
        presentation.secondary_line = true;
        if (!presentation.icon_plus_one && !(item.flags & (popup_item_avatar | popup_item_emoji_preview)))
            presentation = ui_menu_row_presentation(UiIcon::command, true);
    }
    return presentation;
}

constexpr float popup_item_menu_row_height(const PopupItem& item, bool compact = true,
                                           const UiResolvedThemeStyles* styles = nullptr) noexcept {
    // A trailing submenu chevron keeps the row's rhythm; only a leading icon
    // column makes menu rows taller.
    const bool leading_icon = item.presentation.icon_plus_one && !item.presentation.trailing_icon;
    if (styles && styles->ready) {
        if (item.presentation.secondary_line || (item.flags & popup_item_command_row))
            return styles->metrics.menu_detail_height;
        if (leading_icon || !compact)
            return styles->metrics.menu_row_height;
        return styles->metrics.menu_compact_height;
    }
    if (item.presentation.secondary_line || (item.flags & popup_item_command_row))
        return kUiMenuDetailRowHeight;
    if (leading_icon)
        return kUiMenuRowHeight;
    return compact ? kUiMenuCompactRowHeight : kUiMenuRowHeight;
}

struct PopupRowWidget {
    UiWidgetHandle widget;
    UiWidgetHandle label;
    u32 source_index;
};

enum class PopupSceneError : u8 {
    none,
    invalid_input,
    capacity,
    widget,
    layout,
};

struct PopupSceneResult {
    PopupSceneError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == PopupSceneError::none;
    }
};

struct PopupHit {
    u32 action;
    u32 source_index;
    bool inside;
    bool enabled;
};

struct PopupItemLayout {
    const UiLayoutNode* row;
    const UiLayoutNode* label;
    UiRect visible_rectangle;
    UiRect clip_rectangle;
    bool visible;
};

struct PopupScene {
    UiPopupFeedback feedback;
    bool feedback_reserved;
    bool themed_menu;
    // Header popouts (the server dropdown and its follow-up menus) stay below
    // their anchor, shortened to the space down to the viewport bottom and
    // scrolled internally, never flipping or sliding over the header.
    bool anchored_below;
    u32 source_revision;
    u32 theme_geometry_revision;
    // Borrowed apply-time bank. Specialized command geometry remains independent.
    const UiResolvedThemeStyles* theme_styles;
    // Optional dormant backing used by initialize_owned: address space is
    // reserved at initialization, pages and arrays are committed when the first
    // popup opens.
    Arena owned_storage;
    UiWidgetManager widgets;
    Slice<PopupItem> items;
    Slice<PopupRowWidget> rows;
    UiWidgetHandle root;
    UiWidgetHandle panel;
    UiRect viewport;
    UiRect anchor;
    UiRect rectangle;
    u64 owner_id;
    u32 item_count;
    u32 selected_index;
    u32 revision;
    u32 opens;
    u32 closes;
    u32 rejected;
    u32 owned_capacity;
    float preferred_width;
    float scroll_offset;
    float scroll_max;
    PopupKind kind;
    bool prefer_above;
    bool active;
    // Occupies the tail padding after active (see the size assert).
    PopupPanelMaterial panel_material;
};
static_assert(sizeof(PopupScene) ==
              ((offsetof(PopupScene, active) + sizeof(bool) + alignof(PopupScene) - 1) /
               alignof(PopupScene)) *
                  alignof(PopupScene));

bool popup_scene_initialize(PopupScene* scene, Slice<PopupItem> items, Slice<PopupRowWidget> rows,
                            Slice<UiWidget> widgets, Slice<UiLayoutNode> layout_nodes) noexcept;
bool popup_scene_initialize_owned(PopupScene* scene, u32 item_capacity, Slice<UiWidget> widgets,
                                  Slice<UiLayoutNode> layout_nodes) noexcept;
void popup_scene_release(PopupScene* scene) noexcept;
// Reflows an active ordinary menu in place, preserving input/widget identity.
// Paint-only changes and unchanged bank/revision pairs keep retained geometry.
bool popup_scene_bind_theme_styles(PopupScene*, const UiResolvedThemeStyles*) noexcept;
// Autocomplete anchors are the outer composer surface: match its width and
// stay kUiAutocompleteGap above it, shrinking the list to the available height.
// The scroll fractions position the command palette's body and category rail.
PopupSceneResult popup_scene_open(PopupScene* scene, PopupKind kind, u64 owner_id,
                                  ConstSlice<PopupItem> items, UiRect anchor, UiRect viewport,
                                  float preferred_width, bool prefer_above = false,
                                  float grid_scroll_fraction = 0.0f, float category_scroll_fraction = 0.0f,
                                  u32 source_revision = 0,
                                  const UiPopupFeedback* feedback = nullptr) noexcept;
// popup_scene_open for a header popout: always below the anchor, capped to the
// height left below it. A later open with the same anchor (a follow-up menu
// that replaces the popout) keeps this placement.
PopupSceneResult popup_scene_open_below(PopupScene* scene, PopupKind kind, u64 owner_id,
                                        ConstSlice<PopupItem> items, UiRect anchor, UiRect viewport,
                                        float preferred_width) noexcept;
UiRect popup_feedback_rectangle(const PopupScene&) noexcept;
bool popup_scene_set_feedback(PopupScene*, Utf8View text, bool error = false) noexcept;
// Footer slider track area (empty without a slider) and value update.
UiRect popup_feedback_slider(const PopupScene&) noexcept;
bool popup_scene_set_feedback_slider(PopupScene*, u16 value) noexcept;
bool popup_scene_close(PopupScene* scene) noexcept;
bool popup_scene_set_panel_material(PopupScene*, PopupPanelMaterial) noexcept;
PopupHit popup_scene_hit_test(const PopupScene& scene, float x, float y) noexcept;
// Actionable pointer hover transfers focus. Padding, disabled rows and leaving
// the popup clear pointer hover while preserving its visible keyboard selection.
bool popup_scene_set_hover(PopupScene* scene, float x, float y, bool* changed) noexcept;
bool popup_scene_move_selection(PopupScene* scene, i32 direction, bool* changed) noexcept;
// Pages one content viewport without wrapping, preserving visible focus position.
// Command virtual models keep their owner-specific paging policy.
// A quick-reaction strip is one logical paging row; arrows select its reactions.
bool popup_scene_page_selection(PopupScene* scene, bool reverse, bool* changed) noexcept;
bool popup_scene_scroll(PopupScene* scene, float delta, bool* changed) noexcept;
bool popup_scene_select_index(PopupScene* scene, u32 index, bool* changed) noexcept;
u32 popup_scene_selected_action(const PopupScene& scene) noexcept;
const UiLayoutNode* popup_scene_layout_node(const PopupScene& scene, UiWidgetHandle widget) noexcept;
bool popup_scene_resolve_item_layout(const PopupScene& scene, u32 index, PopupItemLayout* layout) noexcept;
bool popup_scene_menu_row_geometry(const PopupScene& scene, u32 index,
                                   UiMenuRowGeometry* geometry) noexcept;

} // namespace maicord
