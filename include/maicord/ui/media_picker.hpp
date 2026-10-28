#pragma once
#include "maicord/ui/edit_field.hpp"
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/scrollbar.hpp"

namespace maicord {
// Visible cells bound the widest pane: a wider request shrinks to the columns
// that fit (about 1600 DIP of Emoji).
inline constexpr u32 kUiMediaPickerVisibleCells = 256;
inline constexpr u32 kUiMediaPickerVisibleSections = 24;
inline constexpr u32 kUiMediaPickerWidgetCapacity = kUiMediaPickerVisibleCells + 64;
inline constexpr u32 kUiMediaPickerMaximumSections = 272;
// Holds every custom emoji the chat state can (131,072) plus Unicode and preferences.
inline constexpr u32 kUiMediaPickerMaximumItems = 262'144;
// Discord's expression picker: 498 DIP minimum (and default) width, 486 high.
inline constexpr float kUiMediaPickerWidth = 498;
inline constexpr float kUiMediaPickerHeight = 486;
// The reaction emoji popout: the standalone emoji picker, 498 x 440, left of
// its control with the popout's 8 DIP spacing.
inline constexpr float kUiMediaPickerReactionWidth = 498;
inline constexpr float kUiMediaPickerReactionHeight = 440;
// Value-owned DIP metrics let a theme choose spacing and control density without
// retaining plugin memory. Geometry and presentation consume the same snapshot.
// Grid cells are fixed: a wider pane fits more columns, never larger cells.
struct UiMediaPickerMetrics {
    float panel_width, panel_height, viewport_inset, anchor_gap;
    // header_gap separates the tabs, search field and body.
    float padding, gap, content_inset, tab_height, tab_max_width, header_gap;
    float search_height, footer_height, section_height, rail_width, rail_pitch;
    // list_inset: the grid's leading padding; the scrollbar gutter trails it.
    float scrollbar_gutter, list_inset;
    // Emoji art (large once the drawer is emoji_extent wider than its
    // minimum) with emoji_padding on every side.
    float emoji_extent, emoji_large_extent, emoji_padding;
    // Sticker art and the least gap between stickers; rows spread the rest.
    float sticker_extent, sticker_gap;
    // GIF masonry: desired column width, gutter, landing category height.
    float gif_target_width, gif_gutter, gif_category_height;
    // footer_art: the hover footer's preview.
    float icon_extent, text_inset, rail_inset, footer_art;
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
    // Discord: 16 DIP header padding, 30 DIP tabs, 12 DIP header gaps.
    m.padding = kUiFloatingSurfaceInset * 2;
    m.gap = kUiMenuRowGap;
    m.content_inset = kUiFloatingSurfaceInset * 2;
    m.tab_height = kUiMenuCompactRowHeight - 2;
    m.tab_max_width = kUiMediaControlHeight * 2;
    m.header_gap = kUiMenuPadding * 2;
    m.search_height = kUiMediaControlHeight;
    // Inspector bar 48, section heading 32, category list 48 with 40 DIP items.
    m.footer_height = kUiMediaControlHeight + kUiFloatingSurfaceInset;
    m.section_height = kUiMenuCompactRowHeight;
    m.rail_width = kUiMediaControlHeight + kUiFloatingSurfaceInset;
    m.rail_pitch = kUiMediaControlHeight;
    m.scrollbar_gutter = kUiFloatingSurfaceInset;
    m.list_inset = kUiFloatingSurfaceInset;
    // Medium (40) and large (48) emoji with 4 DIP padding.
    m.emoji_extent = kUiMediaControlHeight;
    m.emoji_large_extent = kUiMediaControlHeight + kUiFloatingSurfaceInset;
    m.emoji_padding = kUiComposerPadding;
    // 96 DIP sticker previews at least 12 DIP apart.
    m.sticker_extent = kUiMediaControlHeight * 2 + kUiFloatingSurfaceInset * 2;
    m.sticker_gap = kUiMenuPadding * 2;
    // 200 DIP desired GIF columns, 12 DIP gutters, 110 DIP category tiles.
    m.gif_target_width = kUiMediaControlHeight * 5;
    m.gif_gutter = kUiMenuPadding * 2;
    m.gif_category_height = kUiMediaControlHeight * 2 + kUiMenuCompactRowHeight - 2;
    m.icon_extent = kUiMenuIconExtent;
    m.text_inset = kUiComposerPadding;
    m.rail_inset = kUiComposerPadding;
    m.footer_art = kUiMediaControlTarget;
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
    guild,
    // A sticker pack: its cover sticker's catalog preview (asset_id,
    // icon_variant as its generation, asset_flags).
    media
};
// Category rail and section artwork are emoji glyphs, never icons. Guild
// sections show their server icon and sticker packs their cover sticker;
// either falls back to the name's initials.
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
    u64 asset_id{};
    u16 asset_flags{};
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
// A server icon (guild_id) or a catalog preview (asset_id, variant, asset_flags).
struct UiMediaPickerRailMediaSlot {
    UiMediaPickerTarget target;
    UiWidgetReference owner;
    u64 guild_id;
    u32 variant;
    bool animated;
    UiRect rectangle, clip;
    float radius;
    u64 asset_id;
    u16 asset_flags;
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
struct UiMediaPickerTile {
    float top, height;
    u16 column;
    // Category tiles keep a fixed height; media scale with the column width.
    bool fixed;
    float aspect;
};
// Work counters for resize/scroll cost tests.
struct UiMediaPickerCounters {
    u32 rebuilds, cells, resolves, masonry_layouts, allocations;
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
    u32 cell_count, header_count, rail_count;
    // Grid: fixed cell_width, column_pitch apart from grid_left; rows are
    // cell_pitch apart. Masonry GIF views place tiles instead.
    float scroll_dips, rail_scroll_dips, content_height, cell_pitch;
    float cell_width, cell_height, column_pitch, grid_left;
    // The pinned section header's bottom; cells clip below it.
    float pinned_bottom, rail_extent;
    u16 columns;
    // GIF masonry: one tile per item, laid out when the source revision or
    // column geometry changes. Owned; grows only past its committed count.
    UiMediaPickerTile* tiles;
    u32 tile_capacity, tile_count, tile_source_revision;
    float tile_width, tile_tallest, tile_extent;
    u16 tile_columns;
    bool masonry;
    // Footer text, formatted when the inspected item changes.
    u32 footer_index;
    u8 footer_title_bytes, footer_detail_bytes;
    char footer_title[96], footer_detail[96];
    UiMediaPickerCounters counters;
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
// Keyboard navigation follows the visible rows as one grid, ignoring section
// breaks: Left/Right wrap between rows, Up/Down keep the column clamped to the
// target row. Masonry moves within a column and to the nearest tile beside it.
// Home/End reach the row's ends, edge selection the grid's.
bool ui_media_picker_scene_edge_selection(UiMediaPickerScene*, bool last) noexcept;
bool ui_media_picker_scene_row_edge_selection(UiMediaPickerScene*, bool last) noexcept;
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
// The footer inspects the hovered item, else the selection, else the first
// visible item; GIF views have no footer. Its preview is footer_art square:
// custom emoji and stickers as media (this slot), Unicode emoji as text.
const UiMediaPickerCell* ui_media_picker_scene_inspected(const UiMediaPickerScene&) noexcept;
bool ui_media_picker_scene_footer_media(const UiMediaPickerScene&, UiMediaPickerMediaSlot* output) noexcept;
// ":name:" and "from <detail>" for emoji; the name and its source otherwise.
Utf8View ui_media_picker_scene_footer_title(const UiMediaPickerScene&) noexcept;
Utf8View ui_media_picker_scene_footer_detail(const UiMediaPickerScene&) noexcept;
} // namespace maicord
