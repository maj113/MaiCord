#pragma once
#include "maicord/ui/edit_field.hpp"
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/scrollbar.hpp"

namespace maicord {
inline constexpr u32 kUiMediaPickerVisibleCells = 80;
inline constexpr u32 kUiMediaPickerVisibleSections = 16;
inline constexpr u32 kUiMediaPickerWidgetCapacity = 128;
inline constexpr u32 kUiMediaPickerMaximumSections = 272;
inline constexpr u32 kUiMediaPickerMaximumItems = 65'536;
inline constexpr float kUiMediaPickerWidth = 432;
inline constexpr float kUiMediaPickerHeight = 480;
// The reaction emoji popout: the standalone emoji picker, 498 x 440, left of
// its control with the popout's 8 DIP spacing.
inline constexpr float kUiMediaPickerReactionWidth = 498;
inline constexpr float kUiMediaPickerReactionHeight = 440;
// Value-owned DIP metrics let a theme choose spacing and control density without
// retaining plugin memory. Geometry and presentation consume the same snapshot.
struct UiMediaPickerMetrics {
    float panel_width, panel_height, viewport_inset, anchor_gap;
    float padding, gap, content_inset, tab_height, tab_max_width;
    float search_height, footer_height, section_height, rail_width, rail_pitch;
    float scrollbar_gutter, sticker_inset, emoji_inset;
    float gif_min_height, emoji_min_height, gif_aspect_ratio, gif_target_width;
    float icon_extent, text_inset, rail_inset;
};
struct UiMediaPickerStyle {
    UiMediaPickerMetrics metrics;
    float panel_radius, control_radius, edit_radius, selection_width, category_opacity;
};
constexpr UiMediaPickerStyle ui_media_picker_style() noexcept {
    UiMediaPickerStyle style{};
    auto& m = style.metrics;
    m.panel_width = kUiMediaPickerWidth;
    m.panel_height = kUiMediaPickerHeight;
    m.viewport_inset = kUiFloatingSurfaceInset;
    m.anchor_gap = kUiAutocompleteGap;
    m.padding = kUiMenuPadding * 2;
    m.gap = kUiMenuRowGap;
    m.content_inset = kUiFloatingSurfaceInset;
    m.tab_height = kUiMenuCompactRowHeight;
    m.tab_max_width = kUiMediaControlHeight * 2;
    m.search_height = kUiMediaControlHeight;
    m.footer_height = kUiMediaControlHeight + kUiFloatingSurfaceInset;
    m.section_height = kUiMediaControlTarget;
    m.rail_width = kUiMediaControlHeight + kUiComposerPadding;
    m.rail_pitch = kUiMediaControlHeight;
    m.scrollbar_gutter = kUiMenuPadding * 2;
    m.sticker_inset = kUiFloatingSurfaceInset;
    m.emoji_inset = kUiMenuPadding * .5f;
    m.gif_min_height = kUiMenuShortcutMaximumWidth;
    m.emoji_min_height = kUiMediaControlHeight + kUiFloatingSurfaceInset;
    m.gif_aspect_ratio = .60f;
    // Keep two native columns even when the category rail is present.
    m.gif_target_width = (m.panel_width - m.rail_width - m.padding * 2 - m.scrollbar_gutter - m.gap) * .5f;
    m.icon_extent = kUiMenuIconExtent;
    m.text_inset = kUiComposerPadding;
    m.rail_inset = kUiComposerPadding;
    style.panel_radius = kUiSearchMenuCornerRadius;
    style.control_radius = kUiControlCornerRadius;
    style.edit_radius = kUiEditCornerRadius;
    style.selection_width = 1.5f;
    style.category_opacity = kUiMediaHoverOpacity;
    return style;
}
// Non-finite, out-of-bounds or geometrically incompatible overrides fall back
// as a complete style to these safe defaults, never to partially mixed metrics.
bool ui_media_picker_style_valid(const UiMediaPickerStyle&) noexcept;
// The reaction popout is the emoji picker without the tab selector, at the
// popout size and spacing; every other metric is shared.
constexpr UiMediaPickerStyle ui_media_picker_reaction_style() noexcept {
    auto style = ui_media_picker_style();
    style.metrics.panel_width = kUiMediaPickerReactionWidth;
    style.metrics.panel_height = kUiMediaPickerReactionHeight;
    style.metrics.anchor_gap = 8;
    return style;
}
enum class UiMediaPickerKind : u8 {
    gif,
    sticker,
    emoji
};
enum class UiMediaPickerItemKind : u8 {
    category,
    gif,
    sticker,
    emoji
};
enum class UiMediaPickerPhase : u8 {
    loading,
    ready,
    empty,
    failed
};
enum class UiMediaPickerAction : u8 {
    none,
    tab,
    edit,
    back,
    item,
    category,
    section,
    section_jump,
    favorite
};
constexpr u8 ui_media_picker_tab_bit(UiMediaPickerKind kind) noexcept {
    return static_cast<u8>(1u << static_cast<u8>(kind));
}
// The catalog owns all strings and asset URLs. These bounded projections borrow
// its bank only while the source revision is current. IDs are owner-generated
// monotonic tokens; provider IDs and URL hashes are not identities.
struct UiMediaPickerItem {
    u64 id, asset_id;
    Utf8View label, detail;
    u32 variant, width, height;
    UiMediaPickerItemKind kind;
    bool enabled;
    u16 asset_flags;
    // Favorite is the last confirmed owner state; pending/failed never predict
    // a successful mutation. Only enabled, favoritable GIF records expose it.
    bool favoritable = false, favorite = false, favorite_pending = false, favorite_failed = false;
    Utf8View glyph;
    u32 regional_flag;
};
enum class UiMediaPickerSectionVisual : u8 {
    generic,
    recent,
    favorite,
    people,
    nature,
    food,
    activity,
    travel,
    objects,
    symbols,
    flags,
    guild
};
// Category rail and section artwork are emoji glyphs, never icons. Guild
// sections show their server icon instead.
Utf8View ui_media_picker_section_glyph(UiMediaPickerSectionVisual) noexcept;
// expression: the GIF/Sticker/Emoji picker above the composer.
// reaction: the same emoji picker without the tab selector, beside the anchor
// (a message's Add Reaction control).
enum class UiMediaPickerVariant : u8 {
    expression,
    reaction
};
struct UiMediaPickerSection {
    u64 id;
    u32 first, count;
    Utf8View label;
    bool expanded;
    UiMediaPickerSectionVisual visual{};
    u64 guild_id{};
    u32 icon_variant{};
    bool icon_animated{};
};
struct UiMediaPickerSource {
    void* context;
    u64 account_scope, query_generation;
    u32 count, revision;
    ConstSlice<UiMediaPickerSection> sections;
    bool (*resolve)(void*, u32, UiMediaPickerItem*) noexcept;
};
struct UiMediaPickerContent {
    UiEditView edit;
    Utf8View placeholder, status;
    u64 account_scope, query_generation;
    u32 revision;
    UiMediaPickerKind kind;
    UiMediaPickerPhase phase;
    u8 available_tabs;
    bool back;
    UiMediaPickerVariant variant;
};
struct UiMediaPickerTarget {
    u64 account_scope, query_generation, item_id;
    u32 source_revision, source_index, scene_generation;
    UiMediaPickerAction action;
    UiMediaPickerKind tab;
};
bool ui_media_picker_target_equal(UiMediaPickerTarget, UiMediaPickerTarget) noexcept;
struct UiMediaPickerCell {
    UiMediaPickerItem item;
    UiRect rectangle, clip;
    UiWidgetHandle widget;
    u32 source_index;
};
struct UiMediaPickerSectionSlot {
    UiRect rectangle;
    UiWidgetHandle widget;
    u16 section_index;
};
enum class UiMediaPickerFit : u8 {
    cover,
    contain
};
struct UiMediaPickerMediaSlot {
    UiMediaPickerTarget target;
    UiWidgetReference owner;
    u64 asset_id;
    UiRect rectangle, clip;
    float radius;
    float opacity;
    u32 variant, width, height;
    UiMediaPickerFit fit;
    u16 asset_flags;
    UiMediaPickerItemKind kind;
    u32 regional_flag;
};
struct UiMediaPickerRailMediaSlot {
    UiMediaPickerTarget target;
    UiWidgetReference owner;
    u64 guild_id;
    u32 variant;
    bool animated;
    UiRect rectangle, clip;
    float radius;
};
struct UiMediaPickerProjection {
    UiMediaPickerCell cells[kUiMediaPickerVisibleCells];
    UiMediaPickerSectionSlot headers[kUiMediaPickerVisibleSections], rail[kUiMediaPickerVisibleSections];
    struct WidgetTarget {
        u32 source_index;
        u16 section_index;
        UiMediaPickerAction action;
        UiMediaPickerKind tab;
    } targets[kUiMediaPickerWidgetCapacity];
};
struct UiMediaPickerScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiMediaPickerProjection* projection;
    const UiMediaPickerSource* source;
    const UiMediaPickerContent* content;
    UiRect viewport, panel, tabs, field, text, body, rail, footer;
    UiMediaPickerStyle style;
    UiWidgetHandle root, edit, back, tab_widgets[3];
    UiMediaPickerTarget pressed;
    u64 account_scope, query_generation;
    u32 source_revision, content_revision, generation, revision, selected_index;
    u32 cell_count, header_count, rail_count, media_count;
    float scroll_dips, rail_scroll_dips, content_height, cell_pitch;
    u8 columns;
    UiMediaPickerKind kind;
    UiMediaPickerVariant variant;
    u16 layout_flags;
    bool active, favorite_hovered;
    UiScrollbarVisualState body_scrollbar_state{}, rail_scrollbar_state{};
};
// Placement shrinks into the available viewport. Fixed storage commits only on
// first use and remains bounded independently of the source item count.
// Zero preferred width uses the theme default; expression drawers may grow to
// the available width. Reaction popouts retain their own themed width.
bool ui_media_picker_scene_update(UiMediaPickerScene*, const UiMediaPickerSource*,
                                  const UiMediaPickerContent*, UiRect viewport, UiRect anchor,
                                  const UiMediaPickerStyle* style = nullptr,
                                  float preferred_width = 0) noexcept;
bool ui_media_picker_scene_current(const UiMediaPickerScene&) noexcept;
// The section at the top of the body (its rail entry is the current category).
u32 ui_media_picker_scene_current_section(const UiMediaPickerScene&) noexcept;
bool ui_media_picker_scene_rail_media(const UiMediaPickerScene&, u32, UiMediaPickerRailMediaSlot*) noexcept;
void ui_media_picker_scene_close(UiMediaPickerScene*) noexcept;
void ui_media_picker_scene_release(UiMediaPickerScene*) noexcept;
UiMediaPickerTarget ui_media_picker_scene_hit_test(const UiMediaPickerScene&, float x, float y) noexcept;
bool ui_media_picker_scene_hover(UiMediaPickerScene*, float x, float y, u64 now_ms) noexcept;
bool ui_media_picker_scene_pointer_down(UiMediaPickerScene*, float x, float y) noexcept;
UiMediaPickerTarget ui_media_picker_scene_pointer_up(UiMediaPickerScene*, float x, float y) noexcept;
void ui_media_picker_scene_cancel_capture(UiMediaPickerScene*) noexcept;
bool ui_media_picker_scene_scroll(UiMediaPickerScene*, float delta, bool rail = false) noexcept;
UiScrollbarGeometry ui_media_picker_scene_scrollbar(const UiMediaPickerScene&, bool rail = false) noexcept;
bool ui_media_picker_scene_set_scrollbar_state(UiMediaPickerScene*, bool rail,
                                               UiScrollbarVisualState) noexcept;
bool ui_media_picker_scene_scroll_section(UiMediaPickerScene*, u64 section_id) noexcept;
bool ui_media_picker_scene_select(UiMediaPickerScene*, u32 source_index) noexcept;
bool ui_media_picker_scene_move_selection(UiMediaPickerScene*, i32 horizontal, i32 vertical) noexcept;
bool ui_media_picker_scene_page_selection(UiMediaPickerScene*, bool reverse) noexcept;
bool ui_media_picker_scene_edge_selection(UiMediaPickerScene*, bool last) noexcept;
UiMediaPickerTarget ui_media_picker_scene_selected_target(const UiMediaPickerScene&) noexcept;
// Reuses a cell's widget and returns only the visible inset button hit region.
// Pending remains visible but cannot activate or fall through to item Send.
bool ui_media_picker_scene_favorite(const UiMediaPickerScene&, u32 visible_cell, UiRect* rectangle,
                                    UiRect* full_rectangle = nullptr) noexcept;
// Caller chooses the shortcut; edit ownership or stale/unfocused selection
// never produces a favorite action. The result carries the usual source fence.
UiMediaPickerTarget ui_media_picker_scene_selected_favorite_target(const UiMediaPickerScene&) noexcept;
ConstSlice<UiMediaPickerCell> ui_media_picker_scene_cells(const UiMediaPickerScene&) noexcept;
// Root resolves each opaque asset ID in the exact owning bank, validates its
// origin, and requests only these visible slots. The same clip/radius applies
// to native media drawing and any missing-image surface.
bool ui_media_picker_scene_media(const UiMediaPickerScene&, u32 visible_cell,
                                 UiMediaPickerMediaSlot* output) noexcept;
Utf8View ui_media_picker_scene_tooltip(const UiMediaPickerScene&, u64 now_ms, u64 delay_ms = 500) noexcept;
} // namespace maicord
