#include "maicord/ui/media/picker.hpp"
#include "picker_internal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/text/emoji_catalog.hpp"
#include "maicord/ui/text/regional_flag.hpp"

namespace maicord {
namespace ui_media_picker_detail {
namespace {
static_assert(sizeof(UiMediaPickerScene) + sizeof(UiMediaPickerProjection) +
                  kUiMediaPickerWidgetCapacity * (sizeof(UiWidget) + sizeof(UiLayoutNode)) <=
              128 * 1024);
// The rail's separator after the last server: 12 + 1 + 12 DIP.
constexpr float kRailSeparator = 25;
// Masonry columns are capped at 8; extreme GIF aspect ratios are clamped.
constexpr u32 kMasonryColumns = 8;
constexpr float kGifMinimumAspect = .25f, kGifMaximumAspect = 4, kGifDefaultAspect = .5625f;
} // namespace
bool finite_rectangle(const UiRect& r) noexcept {
    return bounded(r.left, -kMaximumUiCoordinate, kMaximumUiCoordinate) &&
           bounded(r.top, -kMaximumUiCoordinate, kMaximumUiCoordinate) &&
           bounded(r.right, -kMaximumUiCoordinate, kMaximumUiCoordinate) &&
           bounded(r.bottom, -kMaximumUiCoordinate, kMaximumUiCoordinate);
}
namespace {
inline bool unicode_category(UiMediaPickerSectionVisual visual) noexcept {
    return visual >= UiMediaPickerSectionVisual::people && visual <= UiMediaPickerSectionVisual::flags;
}
inline bool server_section(UiMediaPickerSectionVisual visual) noexcept {
    return visual == UiMediaPickerSectionVisual::guild || visual == UiMediaPickerSectionVisual::media;
}
// Rail items: 32 DIP art plus 8 DIP spacing, or Discord's 34 DIP for Unicode
// categories; the last server entry carries the separator.
float rail_item_pitch(const UiMediaPickerMetrics& m, const ConstSlice<UiMediaPickerSection>& sections,
                      u32 index) noexcept {
    const auto visual = sections[index].visual;
    float pitch = unicode_category(visual) ? m.rail_pitch - kUiMenuPadding : m.rail_pitch;
    if (server_section(visual) && index + 1 < sections.count && !server_section(sections[index + 1].visual))
        pitch += kRailSeparator;
    return pitch;
}
// Server icons and category glyphs: 32 DIP squares centred in the rail card.
inline float rail_art_extent(const UiMediaPickerMetrics& m) noexcept {
    return minimum(m.rail_width - m.list_inset * 2, m.rail_pitch - m.list_inset);
}
// The rail's scrollable height: the list inset above the first item and the same
// inset below the last item's art, so scrolled to the end nothing touches the card edge.
float rail_content_extent(const UiMediaPickerMetrics& m, ConstSlice<UiMediaPickerSection> sections) noexcept {
    if (!sections.count)
        return 0;
    float extent = m.list_inset * 2 + rail_art_extent(m);
    for (u32 i = 0; i + 1 < sections.count; ++i)
        extent += rail_item_pitch(m, sections, i);
    return extent;
}
// Fixed cells: the column count follows the width. Emoji rows leave the
// remainder at the end, sticker rows spread it between columns and GIF
// columns share the width.
void grid_geometry(const UiMediaPickerStyle& style, UiMediaPickerKind kind, bool back, bool sections,
                   float panel_width, UiRect body, PickerLayout* out) noexcept {
    const auto& m = style.metrics;
    out->masonry = false;
    out->grid_left = body.left + m.list_inset;
    const float width = body.right - m.scrollbar_gutter - out->grid_left;
    if (kind == UiMediaPickerKind::emoji) {
        const float art = panel_width >= m.panel_width + m.emoji_extent ? m.emoji_large_extent : m.emoji_extent;
        const float cell = art + m.emoji_padding * 2;
        out->cell_width = out->cell_height = out->column_pitch = out->cell_pitch = cell;
        out->columns = static_cast<u16>(maximum(0.f, width) / cell);
        return;
    }
    if (kind == UiMediaPickerKind::sticker) {
        const bool compact = width <= kStickerCompactWidth;
        const float node = (compact ? m.sticker_extent * kStickerCompactScale : m.sticker_extent) +
                           kStickerNodePadding * 2;
        const float gap = compact ? m.sticker_gap / 3 : m.sticker_gap;
        const u32 columns = static_cast<u32>(maximum(0.f, width + gap) / (node + gap));
        const float gutter =
            columns > 1 ? static_cast<float>(static_cast<u32>(maximum(gap, (width - node * columns) / (columns - 1))))
                        : 0;
        out->cell_width = out->cell_height = node;
        out->column_pitch = node + gutter;
        out->cell_pitch = node + gap;
        out->columns = static_cast<u16>(columns);
        return;
    }
    // GIFs: Discord's masonry pads every side by its gutter.
    out->grid_left = body.left + m.gif_gutter;
    const float inner = body.right - m.gif_gutter - out->grid_left;
    const u32 cap = back ? kMasonryColumns : 6;
    const u32 columns =
        minimum(cap, maximum(1u, static_cast<u32>(maximum(0.f, ui_rect_width(body)) / m.gif_target_width)));
    out->columns = static_cast<u16>(columns);
    out->cell_width = (inner - m.gif_gutter * (columns - 1)) / columns;
    out->column_pitch = out->cell_width + m.gif_gutter;
    out->cell_height = m.gif_category_height;
    out->cell_pitch = m.gif_category_height + m.gif_gutter;
    // Results and favorites are masonry; landing tiles share one height.
    out->masonry = back && !sections;
}
// The least row pitch the projection must budget for.
inline float minimum_row_pitch(const UiMediaPickerStyle& style, const PickerLayout& layout) noexcept {
    return layout.masonry ? layout.cell_width * kGifMinimumAspect + style.metrics.gif_gutter : layout.cell_pitch;
}
} // namespace
bool picker_layout(const UiMediaPickerStyle& style, const UiMediaPickerSource* source,
                   UiMediaPickerKind kind, UiMediaPickerVariant variant, bool back, UiRect viewport,
                   UiRect anchor, float preferred_width, PickerLayout* output) noexcept {
    const auto& m = style.metrics;
    const bool reaction = variant == UiMediaPickerVariant::reaction;
    const bool sections = source && source->sections.count;
    const float available_width = ui_rect_width(viewport) - m.viewport_inset * 2;
    const float minimum_width = minimum(m.panel_width, available_width);
    float width = reaction || preferred_width == 0 ? minimum_width
                                                   : clamped(preferred_width, minimum_width, available_width);
    const float height = minimum(m.panel_height, ui_rect_height(viewport) - m.viewport_inset * 2);
    if (width < 240 || height < 200)
        return false;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        float left = clamped(anchor.right - width, viewport.left + m.viewport_inset,
                             viewport.right - width - m.viewport_inset);
        float top = clamped(anchor.top - height - m.anchor_gap, viewport.top + m.viewport_inset,
                            viewport.bottom - height - m.viewport_inset);
        if (reaction) {
            // The reaction popout opens beside its control, top aligned: to the
            // left when it fits there, else to the right, clamped into the pane.
            const float before = anchor.left - m.anchor_gap - width;
            left = clamped(before >= viewport.left + m.viewport_inset ? before : anchor.right + m.anchor_gap,
                           viewport.left + m.viewport_inset, viewport.right - width - m.viewport_inset);
            top = clamped(anchor.top, viewport.top + m.viewport_inset,
                          viewport.bottom - height - m.viewport_inset);
        }
        PickerLayout layout{};
        layout.panel = {left, top, left + width, top + height};
        // The reaction popout has no GIF/Sticker/Emoji selector row.
        layout.tabs = {left + m.padding, top + m.content_inset, left + width - m.padding,
                       top + m.content_inset + (reaction ? 0 : m.tab_height)};
        const float field_top = reaction ? layout.tabs.bottom : layout.tabs.bottom + m.header_gap;
        layout.field = {layout.tabs.left, field_top, layout.tabs.right, field_top + m.search_height};
        layout.text = {layout.field.left + m.icon_extent + m.gap * 2 + (back ? m.icon_extent : 0),
                       layout.field.top + m.text_inset, layout.field.right - m.gap,
                       layout.field.bottom - m.text_inset};
        // The rail and the inspector bar are cards inset by card_gap from the
        // panel; the grid starts a gap after the rail and ends a gap above the bar.
        const float body_top = layout.field.bottom + m.header_gap;
        const float rail_right = left + m.card_gap + m.rail_width;
        const float body_left = sections ? rail_right + m.card_gap : left;
        // Discord's GIF picker has no inspector; the rail runs beside the footer.
        const float footer_height = kind == UiMediaPickerKind::gif ? 0 : m.footer_height;
        const float footer_bottom = top + height - (footer_height ? m.card_gap : 0);
        layout.footer = {sections ? body_left : left + m.card_gap, footer_bottom - footer_height,
                         left + width - m.card_gap, footer_bottom};
        layout.body = {body_left, body_top, left + width, footer_height ? layout.footer.top - m.card_gap : footer_bottom};
        if (sections)
            layout.rail = {left + m.card_gap, body_top + m.card_gap, rail_right, top + height - m.card_gap};
        // Validate the actual available pane, not just the preferred panel width.
        if (!ui_rect_has_area(layout.text) ||
            (!reaction && (ui_rect_width(layout.tabs) - m.gap * 2) / 3 < 24) ||
            style.edit_radius * 2 > m.search_height)
            return false;
        const float body_height = ui_rect_height(layout.body);
        if (body_height <= m.section_height + 1)
            return false;
        const u32 headers = sections ? static_cast<u32>(body_height / m.section_height) + 2 : 0;
        const u32 rail_slots =
            sections ? static_cast<u32>(ui_rect_height(layout.rail) / (m.rail_pitch - kUiMenuPadding)) + 2 : 0;
        if (headers > kUiMediaPickerVisibleSections || rail_slots > kUiMediaPickerVisibleSections)
            return false;
        grid_geometry(style, kind, back, sections, width, layout.body, &layout);
        if (!layout.columns || layout.cell_width <= 1 || layout.cell_height <= 1 ||
            style.control_radius * 2 > minimum(layout.cell_width, layout.cell_height))
            return false;
        const u32 rows = static_cast<u32>(body_height / minimum_row_pitch(style, layout)) + 2;
        const u32 fit = kUiMediaPickerVisibleCells / rows;
        // Disabled previews use at most five quads; rail symbols eleven and
        // section arrows two.
        if (fit && layout.columns <= fit &&
            layout.columns * rows * 5 + headers * 2 + rail_slots * 11 + 24 <= kMaxPaintQuads) {
            *output = layout;
            return true;
        }
        // Too wide for the bounded projection: shrink to the columns that fit.
        if (!fit || attempt || reaction || kind == UiMediaPickerKind::gif)
            return false;
        width -= (layout.columns - fit) * layout.column_pitch;
        if (width < minimum_width)
            return false;
    }
    return false;
}
// Inline results sit the anchor's width directly above it: the heading, then
// the GIF strip or up to eight sticker rows, then the list's bottom padding.
// While the first results load only the spinner shows (8 DIP above it). The
// panel shrinks to the viewport, clipping the rows it cannot hold.
bool inline_layout(const UiMediaPickerStyle& style, const UiMediaPickerSource* source,
                   const UiMediaPickerContent& content, UiRect viewport, UiRect anchor,
                   PickerLayout* output) noexcept {
    const auto& m = style.metrics;
    const bool ready = content.phase == UiMediaPickerPhase::ready && source && source->count;
    const bool gif = content.kind == UiMediaPickerKind::gif;
    const float pad = kUiInlineResultsPadding;
    const float body = !ready ? pad + kUiInlineResultsArrowExtent
                       : gif  ? kUiInlineResultsGifExtent + pad * 2
                              : minimum(source->count, kUiInlineResultsStickerLimit) *
                                   (kUiInlineResultsStickerExtent + kUiComposerPadding * 2);
    const float heading = ready ? kUiInlineResultsHeadingHeight : 0;
    const float bottom = anchor.top - m.anchor_gap;
    const float top = maximum(viewport.top + m.viewport_inset, bottom - heading - body - pad);
    if (ui_rect_width(anchor) < kUiInlineResultsGifExtent || bottom - top < heading + pad * 2)
        return false;
    PickerLayout layout{};
    layout.panel = {anchor.left, top, anchor.right, bottom};
    // No tabs, search field, rail or footer: empty rectangles at the corner.
    layout.tabs = layout.field = layout.text = layout.rail = layout.footer = {anchor.left, top, anchor.left, top};
    layout.body = {anchor.left, top + heading, anchor.right, bottom - pad};
    layout.columns = 1;
    layout.cell_width = layout.cell_height = layout.cell_pitch = layout.column_pitch =
        gif ? kUiInlineResultsGifExtent : kUiInlineResultsStickerExtent;
    layout.grid_left = layout.body.left;
    *output = layout;
    return true;
}
bool label_valid(const Utf8View& text) noexcept {
    return text.bytes <= 256 && (!text.bytes || text.data) && utf8_validate(text).valid;
}
// The descriptor bounds (resolve, item and section counts) were admitted by
// ui_media_picker_scene_update before this runs.
bool source_valid(const UiMediaPickerSource* source, const UiMediaPickerContent& content) noexcept {
    if (!source)
        return true;
    if (source->account_scope != content.account_scope || source->query_generation != content.query_generation)
        return false;
    u32 first = 0;
    for (u32 i = 0; i < source->sections.count; ++i) {
        const auto& section = source->sections[i];
        const bool guild = section.visual == UiMediaPickerSectionVisual::guild;
        const bool media = section.visual == UiMediaPickerSectionVisual::media;
        if (!section.id || section.first != first || section.count > source->count - first ||
            !label_valid(section.label) || section.visual > UiMediaPickerSectionVisual::media ||
            (!guild && (section.guild_id || section.icon_animated)) || (guild && !section.guild_id) ||
            (!guild && !media && section.icon_variant) || (!media && (section.asset_id || section.asset_flags)) ||
            (media && section.asset_id && !section.icon_variant))
            return false;
        first += section.count;
        for (u32 j = 0; j < i; ++j)
            if (source->sections[j].id == section.id)
                return false;
    }
    return !source->sections.count || first == source->count;
}
namespace {
// Emoji sections keep Discord's 12 DIP bottom margin, except the last.
float section_margin(const UiMediaPickerScene& scene, u32 index) noexcept {
    const auto& section = scene.source->sections[index];
    return scene.kind == UiMediaPickerKind::emoji && section.expanded && section.count &&
                   index + 1 < scene.source->sections.count
               ? scene.style.metrics.header_gap
               : 0;
}
inline u32 rows_of(const UiMediaPickerScene& scene, u32 count) noexcept {
    return (count + scene.columns - 1) / scene.columns;
}
} // namespace
float section_extent(const UiMediaPickerScene& scene, u32 index) noexcept {
    const auto& section = scene.source->sections[index];
    return scene.style.metrics.section_height +
           (section.expanded ? rows_of(scene, section.count) * scene.cell_pitch : 0) +
           section_margin(scene, index);
}
namespace {
float content_extent(const UiMediaPickerScene& scene) noexcept {
    const auto& m = scene.style.metrics;
    if (!scene.source || !scene.content || scene.content->phase != UiMediaPickerPhase::ready ||
        !scene.source->count)
        return 0;
    if (scene.masonry)
        return scene.tile_count == scene.source->count ? scene.tile_extent : 0;
    float height = content_top(scene);
    if (!scene.source->sections.count)
        return height + rows_of(scene, scene.source->count) * scene.cell_pitch +
               (scene.kind == UiMediaPickerKind::gif ? 0 : m.list_inset);
    for (u32 i = 0; i < scene.source->sections.count; ++i)
        height += section_extent(scene, i);
    return height + m.list_inset;
}
} // namespace
bool item_valid(const UiMediaPickerItem& item) noexcept {
    if (item.glyph.bytes && (item.kind != UiMediaPickerItemKind::emoji || item.asset_id ||
                             item.glyph.bytes > 64 || !utf8_validate(item.glyph).valid))
        return false;
    if (item.regional_flag) {
        char target[kRegionalFlagSvgTargetBytes];
        if (item.kind != UiMediaPickerItemKind::emoji || item.asset_id || item.regional_flag > 0xffffu ||
            !regional_flag_svg_target(static_cast<u16>(item.regional_flag), {target, sizeof(target)}) ||
            (item.glyph.bytes && regional_flag_sequence(item.glyph) != item.regional_flag))
            return false;
    }
    return item.id && item.kind <= UiMediaPickerItemKind::emoji && label_valid(item.label) &&
           label_valid(item.detail) && label_valid(item.note) && item.width <= 65536 && item.height <= 65536 &&
           ((!item.width && !item.height) || (item.width && item.height));
}
namespace {
// GIF masonry: each tile goes to the currently shortest column. Aspects are
// resolved once per publication; geometry changes only re-place the tiles.
bool layout_tiles(UiMediaPickerScene& scene) noexcept {
    const auto& m = scene.style.metrics;
    const u32 count = scene.source->count;
    bool resolved = scene.tiles && scene.tile_source_revision == scene.source_revision &&
                    scene.tile_count == count;
    if (resolved && scene.tile_width == scene.cell_width && scene.tile_columns == scene.columns)
        return true;
    if (count > scene.tile_capacity) {
        const usize bytes = (static_cast<usize>(count) * sizeof(UiMediaPickerTile) + 4095) & ~usize{4095};
        auto* next = static_cast<UiMediaPickerTile*>(
            commit_pages(bytes));
        if (!next)
            return false;
        if (scene.tiles)
            pages_release(scene.tiles);
        scene.tiles = next;
        scene.tile_capacity = static_cast<u32>(bytes / sizeof(UiMediaPickerTile));
        scene.tile_count = 0;
        resolved = false;
        ++scene.counters.allocations;
    }
    if (!resolved) {
        scene.tile_count = 0;
        for (u32 i = 0; i < count; ++i) {
            UiMediaPickerItem item{};
            ++scene.counters.resolves;
            if (!scene.source->resolve(scene.source->context, i, &item) || !item_valid(item))
                return false;
            auto& tile = scene.tiles[i];
            tile.fixed = item.kind == UiMediaPickerItemKind::category;
            tile.aspect = item.width ? clamped(static_cast<float>(item.height) / item.width, kGifMinimumAspect,
                                               kGifMaximumAspect)
                                     : kGifDefaultAspect;
        }
        scene.tile_count = count;
        scene.tile_source_revision = scene.source_revision;
    }
    if (scene.variant == UiMediaPickerVariant::inline_results) {
        // The inline strip: tiles side by side at its height, each inside its
        // padding, after the strip's own leading padding. Never cached as a
        // column layout (tile_columns stays zero).
        float left = 0;
        for (u32 i = 0; i < count; ++i) {
            auto& tile = scene.tiles[i];
            tile.column = 0;
            tile.top = 0;
            tile.height = kUiInlineResultsGifExtent;
            tile.left = left;
            tile.width = kUiInlineResultsGifExtent / tile.aspect;
            left += tile.width + kUiInlineResultsPadding * 2;
        }
        scene.tile_extent = count ? left + kUiInlineResultsPadding * 2 : 0;
        scene.tile_tallest = kUiInlineResultsGifExtent;
        scene.tile_width = scene.cell_width;
        scene.tile_columns = 0;
        return true;
    }
    float heights[kMasonryColumns];
    for (u32 c = 0; c < scene.columns; ++c)
        heights[c] = m.gif_gutter;
    scene.tile_tallest = 0;
    for (u32 i = 0; i < count; ++i) {
        u32 shortest = 0;
        for (u32 c = 1; c < scene.columns; ++c)
            if (heights[c] < heights[shortest])
                shortest = c;
        auto& tile = scene.tiles[i];
        tile.column = static_cast<u16>(shortest);
        tile.top = heights[shortest];
        tile.height = tile.fixed ? m.gif_category_height : scene.cell_width * tile.aspect;
        heights[shortest] += tile.height + m.gif_gutter;
        scene.tile_tallest = maximum(scene.tile_tallest, tile.height);
    }
    float bottom = 0;
    for (u32 c = 0; c < scene.columns; ++c)
        bottom = maximum(bottom, heights[c]);
    scene.tile_extent = count ? bottom : 0;
    scene.tile_width = scene.cell_width;
    scene.tile_columns = scene.columns;
    ++scene.counters.masonry_layouts;
    return true;
}
} // namespace
UiMediaPickerTarget target(const UiMediaPickerScene& scene, UiWidgetHandle widget) noexcept {
    if (!ui_media_picker_scene_current(scene) || !ui_widget_manager_input_enabled(scene.widgets, widget) ||
        widget.slot >= kUiMediaPickerWidgetCapacity)
        return {};
    const auto mapping = scene.projection->targets[widget.slot];
    UiMediaPickerTarget result{scene.account_scope,
                               scene.query_generation,
                               0,
                               scene.source_revision,
                               mapping.source_index,
                               scene.generation,
                               mapping.action,
                               mapping.tab};
    if (mapping.action == UiMediaPickerAction::item || mapping.action == UiMediaPickerAction::category) {
        for (u32 i = 0; i < scene.cell_count; ++i)
            if (scene.projection->cells[i].source_index == mapping.source_index) {
                result.item_id = scene.projection->cells[i].item.id;
                return result;
            }
        return {};
    }
    if (mapping.action == UiMediaPickerAction::section ||
        mapping.action == UiMediaPickerAction::section_jump)
        result.item_id = scene.source && mapping.section_index < scene.source->sections.count
                             ? scene.source->sections[mapping.section_index].id
                             : 0;
    return result;
}
namespace {
UiWidgetHandle add(UiMediaPickerScene& scene, const UiRect& rectangle,
                   UiMediaPickerProjection::WidgetTarget mapping, const Utf8View& label, bool enabled = true,
                   UiWidgetHandle parent = ui_widget_invalid_handle()) noexcept {
    const auto origin =
        parent.generation ? ui_widget_manager_layout_node(scene.widgets, parent)->rectangle : scene.panel;
    const auto result = ui_widget_manager_add_placed(
        &scene.widgets, parent.generation ? parent : scene.root,
        mapping.action == UiMediaPickerAction::edit ? UiWidgetKind::text_field : UiWidgetKind::button,
        static_cast<u16>(enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                 : ui_widget_none),
        label, rectangle, origin);
    if (!result.ok() || result.handle.slot >= kUiMediaPickerWidgetCapacity)
        return ui_widget_invalid_handle();
    scene.projection->targets[result.handle.slot] = mapping;
    return result.handle;
}
inline UiRect grid_cell(const UiMediaPickerScene& scene, u32 column, float top) noexcept {
    const float left = scene.grid_left + column * scene.column_pitch;
    return {left, top, left + scene.cell_width, top + scene.cell_height};
}
// hit: the cell's widget area when it is larger than its art (inline rows
// and tiles keep their padding clickable); the art's clip stays the cell's.
bool cell(UiMediaPickerScene& scene, u32 index, const UiRect& rectangle, const UiMediaPickerCell* prior,
          u32 prior_count, const UiRect* hit = nullptr) noexcept {
    auto visible = scene.body;
    visible.top = maximum(visible.top, scene.pinned_bottom);
    const auto clip = ui_rect_intersection(rectangle, visible);
    const auto area = hit ? ui_rect_intersection(*hit, visible) : clip;
    if (!ui_rect_has_area(area))
        return true;
    if (scene.cell_count == kUiMediaPickerVisibleCells)
        return false;
    UiMediaPickerItem item{};
    bool reused = false;
    // Prior cells are source-index ordered. Borrows remain valid only within
    // the unchanged source/revision checked by safe_rebuild.
    u32 lo = 0, hi = prior_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        if (prior[mid].source_index < index)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < prior_count && prior[lo].source_index == index) {
        item = prior[lo].item;
        reused = true;
    }
    const UiMediaPickerItemKind expected = scene.kind == UiMediaPickerKind::gif ? UiMediaPickerItemKind::gif
                                           : scene.kind == UiMediaPickerKind::sticker
                                               ? UiMediaPickerItemKind::sticker
                                               : UiMediaPickerItemKind::emoji;
    if (!reused)
        ++scene.counters.resolves;
    if ((!reused && (!scene.source->resolve(scene.source->context, index, &item) || !item_valid(item))) ||
        (item.kind != expected &&
         !(scene.kind == UiMediaPickerKind::gif && item.kind == UiMediaPickerItemKind::category)))
        return false;
    for (u32 i = 0; i < scene.cell_count; ++i)
        if (scene.projection->cells[i].item.id == item.id)
            return false;
    const auto action = item.kind == UiMediaPickerItemKind::category ? UiMediaPickerAction::category
                                                                     : UiMediaPickerAction::item;
    const auto widget = add(scene, area, {index, 0, action, scene.content->kind}, item.label, item.enabled);
    if (!widget.generation)
        return false;
    scene.projection->cells[scene.cell_count++] = {item, rectangle, clip, widget, index};
    ++scene.counters.cells;
    return true;
}
bool project_items(UiMediaPickerScene& scene, u32 first, u32 count, float top,
                   const UiMediaPickerCell* prior, u32 prior_count) noexcept {
    const u32 rows = rows_of(scene, count);
    const float offset = maximum(0.f, scene.body.top - top);
    const u32 first_row = minimum(rows, static_cast<u32>(offset / scene.cell_pitch));
    const u32 last_row =
        minimum(rows, first_row + static_cast<u32>(ui_rect_height(scene.body) / scene.cell_pitch) + 2);
    for (u32 row = first_row; row < last_row; ++row)
        for (u32 column = 0; column < scene.columns && row * scene.columns + column < count; ++column)
            if (!cell(scene, first + row * scene.columns + column,
                      grid_cell(scene, column, top + row * scene.cell_pitch), prior, prior_count))
                return false;
    return true;
}
// Tile tops never decrease in index order (the shortest column only grows),
// so the visible tiles follow the first tile within one tallest tile above.
bool project_tiles(UiMediaPickerScene& scene, float top, const UiMediaPickerCell* prior,
                   u32 prior_count) noexcept {
    const float from = scene.body.top - top - scene.tile_tallest;
    u32 lo = 0, hi = scene.tile_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        if (scene.tiles[mid].top < from)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (u32 i = lo; i < scene.tile_count && top + scene.tiles[i].top < scene.body.bottom; ++i) {
        const auto& tile = scene.tiles[i];
        const float left = scene.grid_left + tile.column * scene.column_pitch;
        if (!cell(scene, i, {left, top + tile.top, left + scene.cell_width, top + tile.top + tile.height}, prior,
                  prior_count))
            return false;
    }
    return true;
}
} // namespace
void synchronize_focus(UiMediaPickerScene& scene) noexcept {
    ui_widget_manager_focus(&scene.widgets, ui_widget_invalid_handle());
    if (scene.content->edit.focused) {
        ui_widget_manager_focus(&scene.widgets, scene.edit);
        return;
    }
    for (u32 i = 0; i < scene.cell_count; ++i)
        if (scene.projection->cells[i].source_index == scene.selected_index) {
            ui_widget_manager_focus(&scene.widgets, scene.projection->cells[i].widget);
            return;
        }
}
namespace {
inline u32 append(char* output, u32 used, u32 capacity, const Utf8View& text) noexcept {
    return used + utf8_copy_truncated(output + used, capacity - used, text);
}
} // namespace
// Formats the footer for the inspected cell; unchanged cells keep their text.
void refresh_footer(UiMediaPickerScene& scene) noexcept {
    const auto* cell = ui_media_picker_scene_inspected(scene);
    const u32 index = cell ? cell->source_index : ~u32{0};
    if (index == scene.footer_index && (cell || !scene.footer_title_bytes))
        return;
    scene.footer_index = index;
    scene.footer_title_bytes = scene.footer_detail_bytes = 0;
    if (!cell)
        return;
    const auto& item = cell->item;
    const bool emoji = item.kind == UiMediaPickerItemKind::emoji;
    u32 title = 0;
    if (emoji) {
        char name[kUnicodeEmojiMaximumNameBytes + 8];
        const u32 bytes = item.glyph.bytes ? emoji_shortcode_key(item.glyph, {name, sizeof(name)}) : 0;
        title = append(scene.footer_title, 0, sizeof(scene.footer_title), utf8_literal(":"));
        title = append(scene.footer_title, title, sizeof(scene.footer_title) - 1,
                       bytes ? Utf8View{name, bytes} : item.label);
        title = append(scene.footer_title, title, sizeof(scene.footer_title), utf8_literal(":"));
    } else
        title = append(scene.footer_title, 0, sizeof(scene.footer_title), item.label);
    scene.footer_title_bytes = static_cast<u8>(title);
    if (item.detail.bytes && item.kind != UiMediaPickerItemKind::gif &&
        item.kind != UiMediaPickerItemKind::category) {
        const u32 detail = append(scene.footer_detail, 0, sizeof(scene.footer_detail), utf8_literal("from "));
        scene.footer_detail_bytes =
            static_cast<u8>(append(scene.footer_detail, detail, sizeof(scene.footer_detail), item.detail));
    }
}
namespace {
// Inline results: the sticker rows (art 4 DIP inside its row, 8 DIP from the
// row's start) or the GIF strip's visible tiles, then the strip's arrows.
bool project_inline(UiMediaPickerScene& scene, const UiMediaPickerCell* prior, u32 prior_count) noexcept {
    for (auto& tab : scene.tab_widgets)
        tab = ui_widget_invalid_handle();
    scene.edit = scene.back = scene.page_widgets[0] = scene.page_widgets[1] = ui_widget_invalid_handle();
    scene.content_height = 0;
    if (!scene.source || scene.content->phase != UiMediaPickerPhase::ready)
        return true;
    constexpr float pad = kUiInlineResultsPadding;
    if (scene.kind == UiMediaPickerKind::sticker) {
        const u32 count = minimum(scene.source->count, kUiInlineResultsStickerLimit);
        const float pitch = kUiInlineResultsStickerExtent + kUiComposerPadding * 2;
        scene.content_height = count * pitch;
        scene.scroll_dips =
            minimum(scene.scroll_dips, maximum(0.f, scene.content_height - ui_rect_height(scene.body)));
        for (u32 i = 0; i < count; ++i) {
            const float top = scene.body.top + i * pitch - scene.scroll_dips;
            const UiRect row{scene.body.left + pad, top, scene.body.right - pad, top + pitch};
            const float left = row.left + pad, art_top = top + kUiComposerPadding;
            const UiRect art{left, art_top, left + kUiInlineResultsStickerExtent,
                             art_top + kUiInlineResultsStickerExtent};
            if (!cell(scene, i, art, prior, prior_count, &row))
                return false;
        }
        return true;
    }
    if (!layout_tiles(scene))
        return false;
    scene.content_height = scene.tile_extent;
    const float last = maximum(0.f, scene.content_height - ui_rect_width(scene.body));
    scene.scroll_dips = minimum(scene.scroll_dips, last);
    for (u32 i = 0; i < scene.tile_count; ++i) {
        const auto& tile = scene.tiles[i];
        const float left = scene.body.left + pad + tile.left - scene.scroll_dips;
        if (left >= scene.body.right)
            break;
        const UiRect box{left, scene.body.top, left + tile.width + pad * 2,
                         scene.body.top + kUiInlineResultsGifExtent + pad * 2};
        const UiRect art{box.left + pad, box.top + pad, box.right - pad, box.bottom - pad};
        if (box.right > scene.body.left && !cell(scene, i, art, prior, prior_count, &box))
            return false;
    }
    // The arrows: centred on the strip, 8 DIP inside its ends, only
    // toward content past that end.
    const float middle = (scene.body.top + scene.body.bottom) * .5f, half = kUiInlineResultsArrowExtent * .5f;
    if (scene.scroll_dips > 0)
        scene.page_widgets[0] = add(scene,
                                    {scene.body.left + pad, middle - half,
                                     scene.body.left + pad + kUiInlineResultsArrowExtent, middle + half},
                                    {0, 0, UiMediaPickerAction::page_back, scene.kind}, utf8_literal("Scroll left"));
    if (scene.scroll_dips < last)
        scene.page_widgets[1] = add(scene,
                                    {scene.body.right - pad - kUiInlineResultsArrowExtent, middle - half,
                                     scene.body.right - pad, middle + half},
                                    {0, 0, UiMediaPickerAction::page_forward, scene.kind},
                                    utf8_literal("Scroll right"));
    return true;
}
} // namespace
bool rebuild(UiMediaPickerScene& scene, bool reuse) noexcept {
    const auto& m = scene.style.metrics;
    ++scene.counters.rebuilds;
    // The previous cells are the reuse source: snapshot them (the rebuild
    // overwrites the projection) into owned storage that grows with the visible
    // count. Without it the rebuild resolves every cell again.
    u32 prior_count = reuse ? scene.cell_count : 0;
    if (prior_count > scene.prior_capacity) {
        const usize bytes = (static_cast<usize>(prior_count) * sizeof(UiMediaPickerCell) + 4095) & ~usize{4095};
        auto* next = static_cast<UiMediaPickerCell*>(
            commit_pages(bytes));
        if (next) {
            if (scene.prior)
                pages_release(scene.prior);
            scene.prior = next;
            scene.prior_capacity = static_cast<u32>(bytes / sizeof(UiMediaPickerCell));
        } else {
            prior_count = 0;
        }
    }
    if (prior_count)
        copy_bytes(scene.prior, scene.projection->cells, prior_count * sizeof(UiMediaPickerCell));
    const UiMediaPickerCell* const prior = scene.prior;
    reset_value(&scene.pressed);
    scene.favorite_hovered = false;
    scene.footer_index = ~u32{0};
    scene.footer_title_bytes = 0;
    ui_widget_manager_reset(&scene.widgets);
    scene.cell_count = scene.header_count = scene.rail_count = 0;
    scene.pinned_bottom = scene.body.top;
    if (++scene.generation == 0)
        ++scene.generation;
    auto style = ui_layout_fill_style(ui_layout_node_visible | ui_layout_node_clip_children |
                                      ui_layout_node_allow_overlap);
    const auto root = ui_widget_manager_add_keyed(&scene.widgets, ui_widget_invalid_handle(), 1,
                                                  UiWidgetKind::container, ui_widget_none, "Media picker", style);
    if (!root.ok() || !ui_widget_manager_layout_rectangle(&scene.widgets, scene.panel).ok())
        return false;
    scene.root = root.handle;
    if (inline_results(scene)) {
        if (!project_inline(scene, prior, prior_count) ||
            !ui_widget_manager_layout_rectangle(&scene.widgets, scene.panel).ok())
            return false;
        synchronize_focus(scene);
        ++scene.revision;
        return true;
    }
    float x = scene.tabs.left;
    u32 tab_count = 0;
    for (u32 i = 0; i < 3; ++i)
        tab_count += (scene.content->available_tabs >> i) & 1u;
    const float tab_width =
        tab_count
            ? minimum(m.tab_max_width, (ui_rect_width(scene.tabs) - m.gap * (tab_count - 1)) / tab_count)
            : 0;
    const Utf8View names[]{utf8_literal("GIFs"), utf8_literal("Stickers"), utf8_literal("Emoji")};
    for (u32 i = 0; i < 3; ++i) {
        scene.tab_widgets[i] = ui_widget_invalid_handle();
        if (!(scene.content->available_tabs & (1u << i)) || scene.variant == UiMediaPickerVariant::reaction)
            continue;
        scene.tab_widgets[i] =
            add(scene, {x, scene.tabs.top, minimum(x + tab_width, scene.tabs.right), scene.tabs.bottom},
                {0, 0, UiMediaPickerAction::tab, static_cast<UiMediaPickerKind>(i)}, names[i]);
        x += tab_width + m.gap;
    }
    scene.edit = add(scene, scene.field, {0, 0, UiMediaPickerAction::edit, scene.content->kind},
                     utf8_literal("Search media"));
    scene.back = ui_widget_invalid_handle();
    if (scene.content->back)
        scene.back = add(
            scene,
            {scene.field.left, scene.field.top, scene.field.left + m.section_height, scene.field.bottom},
            {0, 0, UiMediaPickerAction::back, scene.content->kind}, utf8_literal("Back"));
    const bool ready = scene.source && scene.content->phase == UiMediaPickerPhase::ready;
    if (ready && scene.masonry && !layout_tiles(scene))
        return false;
    scene.content_height = content_extent(scene);
    scene.scroll_dips =
        minimum(scene.scroll_dips, maximum(0.f, scene.content_height - ui_rect_height(scene.body)));
    scene.rail_extent = ready ? rail_content_extent(m, scene.source->sections) : 0;
    scene.rail_scroll_dips =
        minimum(scene.rail_scroll_dips, maximum(0.f, scene.rail_extent - ui_rect_height(scene.rail)));
    if (ready) {
        float top = scene.body.top + content_top(scene) - scene.scroll_dips;
        if (scene.masonry) {
            if (!project_tiles(scene, scene.body.top - scene.scroll_dips, prior, prior_count))
                return false;
        } else if (!scene.source->sections.count) {
            if (!project_items(scene, 0, scene.source->count, top, prior, prior_count))
                return false;
        } else {
            for (u32 i = 0; i < scene.source->sections.count; ++i) {
                const auto& section = scene.source->sections[i];
                const float extent = section_extent(scene, i);
                if (top < scene.body.bottom && top + extent > scene.body.top) {
                    // Sticky headers: the section under the body's top pins its
                    // header there until the section's end pushes it up.
                    float header_top = top;
                    if (top < scene.body.top) {
                        header_top = minimum(scene.body.top,
                                             top + extent - section_margin(scene, i) - m.section_height);
                        scene.pinned_bottom = header_top + m.section_height;
                    }
                    const UiRect header{scene.body.left, header_top, scene.body.right,
                                        header_top + m.section_height};
                    const auto rectangle = ui_rect_intersection(header, scene.body);
                    if (ui_rect_has_area(rectangle)) {
                        if (scene.header_count == kUiMediaPickerVisibleSections)
                            return false;
                        const auto widget =
                            add(scene, rectangle,
                                {0, static_cast<u16>(i), UiMediaPickerAction::section, scene.content->kind},
                                section.label);
                        scene.projection->headers[scene.header_count++] = {header, widget,
                                                                           static_cast<u16>(i)};
                    }
                    if (section.expanded && !project_items(scene, section.first, section.count,
                                                           top + m.section_height, prior, prior_count))
                        return false;
                }
                top += extent;
            }
            // Rail items: 32 DIP art top-aligned in each item, below the list inset.
            float y = scene.rail.top + m.list_inset - scene.rail_scroll_dips;
            const float extent = rail_art_extent(m);
            for (u32 i = 0; i < scene.source->sections.count && y < scene.rail.bottom; ++i) {
                const float pitch = rail_item_pitch(m, scene.source->sections, i);
                const float left = (scene.rail.left + scene.rail.right - extent) * .5f;
                const UiRect artwork{left, y, left + extent, y + extent};
                y += pitch;
                const auto r = ui_rect_intersection(artwork, scene.rail);
                if (!ui_rect_has_area(r))
                    continue;
                if (scene.rail_count == kUiMediaPickerVisibleSections)
                    return false;
                const auto widget =
                    add(scene, r,
                        {0, static_cast<u16>(i), UiMediaPickerAction::section_jump, scene.content->kind},
                        scene.source->sections[i].label);
                scene.projection->rail[scene.rail_count++] = {artwork, widget, static_cast<u16>(i)};
            }
        }
    }
    if (!ui_widget_manager_layout_rectangle(&scene.widgets, scene.panel).ok())
        return false;
    synchronize_focus(scene);
    refresh_footer(scene);
    ++scene.revision;
    return true;
}
namespace {
inline u32 section_count(const UiMediaPickerScene& scene) noexcept {
    return scene.source->sections.count ? scene.source->sections.count : 1;
}
} // namespace
bool grid_locate(const UiMediaPickerScene& scene, u32 index, GridPosition* position) noexcept {
    for (u32 s = 0; s < section_count(scene); ++s) {
        const u32 first = section_first(scene, s), count = section_items(scene, s);
        if (index >= first && index - first < count) {
            *position = {s, (index - first) / scene.columns, (index - first) % scene.columns};
            return true;
        }
    }
    return false;
}
// The index `rows` visible rows away, column clamped to the target row; the
// nearest row when the grid ends first.
u32 grid_step(const UiMediaPickerScene& scene, u32 index, i32 rows) noexcept {
    GridPosition at{};
    if (!grid_locate(scene, index, &at))
        return index;
    for (; rows; rows += rows > 0 ? -1 : 1) {
        if (rows > 0) {
            if ((at.row + 1) * scene.columns < section_items(scene, at.section)) {
                ++at.row;
                continue;
            }
            u32 next = at.section + 1;
            while (next < section_count(scene) && !section_items(scene, next))
                ++next;
            if (next == section_count(scene))
                break;
            at = {next, 0, at.column};
        } else {
            if (at.row) {
                --at.row;
                continue;
            }
            u32 previous = at.section;
            while (previous && !section_items(scene, previous - 1))
                --previous;
            if (!previous)
                break;
            --previous;
            at = {previous, rows_of(scene, section_items(scene, previous)) - 1, at.column};
        }
    }
    const u32 first = section_first(scene, at.section) + at.row * scene.columns;
    const u32 length = minimum<u32>(scene.columns, section_items(scene, at.section) - at.row * scene.columns);
    return first + minimum(at.column, length - 1);
}
bool item_position(const UiMediaPickerScene& scene, u32 index, float* top, float* height) noexcept {
    if (!scene.source || !scene.content || scene.content->phase != UiMediaPickerPhase::ready ||
        index >= scene.source->count)
        return false;
    *height = scene.cell_pitch;
    if (inline_results(scene)) {
        // Offsets along the scroll axis: GIF tiles with their padding, or sticker rows.
        if (scene.kind == UiMediaPickerKind::sticker) {
            *height = kUiInlineResultsStickerExtent + kUiComposerPadding * 2;
            *top = index * *height;
            return index < kUiInlineResultsStickerLimit;
        }
        if (index >= scene.tile_count)
            return false;
        *top = kUiInlineResultsPadding + scene.tiles[index].left;
        *height = scene.tiles[index].width + kUiInlineResultsPadding * 2;
        return true;
    }
    if (scene.masonry) {
        if (index >= scene.tile_count)
            return false;
        *top = scene.tiles[index].top;
        *height = scene.tiles[index].height + scene.style.metrics.gif_gutter;
        return true;
    }
    *top = content_top(scene);
    if (!scene.source->sections.count) {
        *top += index / scene.columns * scene.cell_pitch;
        return true;
    }
    for (u32 i = 0; i < scene.source->sections.count; ++i) {
        const auto& section = scene.source->sections[i];
        if (index >= section.first && index - section.first < section.count) {
            if (!section.expanded)
                return false;
            *top += scene.style.metrics.section_height + (index - section.first) / scene.columns * scene.cell_pitch;
            return true;
        }
        *top += section_extent(scene, i);
    }
    return false;
}
bool safe_rebuild(UiMediaPickerScene& scene) noexcept {
    if (rebuild(scene, ui_media_picker_scene_current(scene)))
        return true;
    ui_media_picker_scene_close(&scene);
    return false;
}
bool select_index(UiMediaPickerScene& scene, u32 index, float top, float height) noexcept {
    const auto& m = scene.style.metrics;
    scene.selected_index = index;
    // Keep the row clear of a pinned section header and the list's leading inset.
    const float lead = inline_results(scene) ? 0
                       : (scene.source && scene.source->sections.count ? m.section_height : 0) + content_top(scene);
    const float extent = body_extent(scene);
    if (top < scene.scroll_dips + lead)
        scene.scroll_dips = maximum(0.f, top - lead);
    else if (top + height > scene.scroll_dips + extent)
        scene.scroll_dips = top + height - extent;
    return safe_rebuild(scene);
}
bool select_available(UiMediaPickerScene& scene, u32 first, i32 direction) noexcept {
    if (!scene.source || !scene.source->count || scene.source->count > kUiMediaPickerMaximumItems)
        return false;
    i64 candidate = first;
    // User input resolves only its candidate prefix. Collapsed metadata jumps
    // straight across hidden records without calling the catalog resolver.
    for (u32 scanned = 0;
         scanned < scene.source->count && candidate >= 0 && candidate < scene.source->count; ++scanned) {
        bool collapsed = false;
        for (u32 s = 0; s < scene.source->sections.count; ++s) {
            const auto& section = scene.source->sections[s];
            if (candidate >= section.first && candidate - section.first < section.count &&
                !section.expanded) {
                candidate = direction > 0 ? static_cast<i64>(section.first) + section.count
                                          : static_cast<i64>(section.first) - 1;
                collapsed = true;
                break;
            }
        }
        if (collapsed)
            continue;
        float top = 0, height = 0;
        UiMediaPickerItem item{};
        if (!item_position(scene, static_cast<u32>(candidate), &top, &height) ||
            !scene.source->resolve(scene.source->context, static_cast<u32>(candidate), &item) ||
            !item_valid(item))
            return false;
        if (item.enabled)
            return select_index(scene, static_cast<u32>(candidate), top, height);
        candidate += direction;
    }
    return false;
}
// Masonry: Up/Down follow the tile's column; Left/Right take the neighbouring
// column's tile nearest by top, wrapping between the outer columns a row on.
u32 masonry_step(const UiMediaPickerScene& scene, u32 index, i32 horizontal, i32 vertical) noexcept {
    const auto& from = scene.tiles[index];
    if (vertical) {
        for (i64 i = static_cast<i64>(index) + vertical; i >= 0 && i < scene.tile_count; i += vertical)
            if (scene.tiles[i].column == from.column)
                return static_cast<u32>(i);
        return index;
    }
    i32 column = static_cast<i32>(from.column) + horizontal;
    i32 wrap = 0;
    if (column < 0 || column >= static_cast<i32>(scene.columns)) {
        column = column < 0 ? scene.columns - 1 : 0;
        wrap = horizontal;
    }
    u32 best = index;
    float best_distance = kMaximumUiCoordinate;
    for (u32 i = 0; i < scene.tile_count; ++i) {
        const auto& tile = scene.tiles[i];
        if (tile.column != static_cast<u32>(column) || i == index || (wrap > 0 && tile.top <= from.top) ||
            (wrap < 0 && tile.top >= from.top))
            continue;
        const float distance = tile.top > from.top ? tile.top - from.top : from.top - tile.top;
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}
} // namespace ui_media_picker_detail

using namespace ui_media_picker_detail;

// Emoji stand-ins for the category rail icons.
Utf8View ui_media_picker_section_glyph(UiMediaPickerSectionVisual visual) noexcept {
    switch (visual) {
    case UiMediaPickerSectionVisual::recent: return utf8_literal("\xF0\x9F\x95\x92");     // clock three
    case UiMediaPickerSectionVisual::favorite: return utf8_literal("\xE2\xAD\x90");       // star
    case UiMediaPickerSectionVisual::people: return utf8_literal("\xF0\x9F\x98\x80");     // grinning face
    case UiMediaPickerSectionVisual::nature: return utf8_literal("\xF0\x9F\x8C\xBF");     // herb
    case UiMediaPickerSectionVisual::food: return utf8_literal("\xF0\x9F\x8D\x8E");       // red apple
    case UiMediaPickerSectionVisual::activity: return utf8_literal("\xF0\x9F\x8E\xAE");   // video game
    case UiMediaPickerSectionVisual::travel: return utf8_literal("\xF0\x9F\x9A\xB2");     // bicycle
    case UiMediaPickerSectionVisual::objects: return utf8_literal("\xF0\x9F\x92\xA1");    // light bulb
    case UiMediaPickerSectionVisual::symbols: return utf8_literal("\xE2\x9D\xA4\xEF\xB8\x8F"); // red heart
    case UiMediaPickerSectionVisual::flags: return utf8_literal("\xF0\x9F\x9A\xA9");      // triangular flag
    default: return {};
    }
}
u32 ui_media_picker_scene_current_section(const UiMediaPickerScene& scene) noexcept {
    if (!ui_media_picker_scene_current(scene) || !scene.source || !scene.source->sections.count ||
        scene.content->phase != UiMediaPickerPhase::ready)
        return ~u32{0};
    float bottom = content_top(scene);
    for (u32 i = 0; i < scene.source->sections.count; ++i) {
        bottom += section_extent(scene, i);
        if (bottom > scene.scroll_dips)
            return i;
    }
    return scene.source->sections.count - 1;
}
bool ui_media_picker_target_equal(UiMediaPickerTarget a, UiMediaPickerTarget b) noexcept {
    return a.account_scope == b.account_scope && a.query_generation == b.query_generation &&
           a.item_id == b.item_id && a.source_revision == b.source_revision &&
           a.source_index == b.source_index && a.scene_generation == b.scene_generation &&
           a.action == b.action && a.tab == b.tab;
}
bool ui_media_picker_scene_current(const UiMediaPickerScene& scene) noexcept {
    return scene.active && scene.projection && scene.content &&
           scene.content->account_scope == scene.account_scope &&
           scene.content->query_generation == scene.query_generation &&
           scene.content->revision == scene.content_revision &&
           (!scene.source || (scene.source->account_scope == scene.account_scope &&
                              scene.source->query_generation == scene.query_generation &&
                              scene.source->revision == scene.source_revision));
}
} // namespace maicord
