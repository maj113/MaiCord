#include "maicord/ui/media_picker.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/regional_flag.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
static_assert(sizeof(UiMediaPickerScene) + sizeof(UiMediaPickerProjection) +
                  kUiMediaPickerWidgetCapacity * (sizeof(UiWidget) + sizeof(UiLayoutNode)) <=
              128 * 1024);
// Discord's sticker grid: at most 360 DIP wide it uses 80 DIP previews 4 DIP
// apart; every preview node adds 1 DIP padding.
constexpr float kStickerCompactWidth = 360, kStickerCompactScale = 80.f / 96, kStickerNodePadding = 1;
// The rail's separator after the last server: 12 + 1 + 12 DIP.
constexpr float kRailSeparator = 25;
// Masonry columns are capped at 8; extreme GIF aspect ratios are clamped.
constexpr u32 kMasonryColumns = 8;
constexpr float kGifMinimumAspect = .25f, kGifMaximumAspect = 4, kGifDefaultAspect = .5625f;
float clamped(float value, float low, float high) noexcept {
    return maximum(low, minimum(value, high));
}
bool bounded(float value, float low, float high) noexcept {
    u32 bits = 0;
    CopyMemory(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u && value >= low && value <= high;
}
bool finite_rectangle(UiRect r) noexcept {
    return bounded(r.left, -kMaximumUiCoordinate, kMaximumUiCoordinate) &&
           bounded(r.top, -kMaximumUiCoordinate, kMaximumUiCoordinate) &&
           bounded(r.right, -kMaximumUiCoordinate, kMaximumUiCoordinate) &&
           bounded(r.bottom, -kMaximumUiCoordinate, kMaximumUiCoordinate);
}
bool unicode_category(UiMediaPickerSectionVisual visual) noexcept {
    return visual >= UiMediaPickerSectionVisual::people && visual <= UiMediaPickerSectionVisual::flags;
}
bool server_section(UiMediaPickerSectionVisual visual) noexcept {
    return visual == UiMediaPickerSectionVisual::guild || visual == UiMediaPickerSectionVisual::media;
}
// Rail items: 32 DIP art plus 8 DIP spacing, or Discord's 34 DIP for Unicode
// categories; the last server entry carries the separator.
float rail_item_pitch(const UiMediaPickerMetrics& m, ConstSlice<UiMediaPickerSection> sections,
                      u32 index) noexcept {
    const auto visual = sections[index].visual;
    float pitch = unicode_category(visual) ? m.rail_pitch - kUiMenuPadding : m.rail_pitch;
    if (server_section(visual) && index + 1 < sections.count && !server_section(sections[index + 1].visual))
        pitch += kRailSeparator;
    return pitch;
}
float rail_content_extent(const UiMediaPickerMetrics& m, ConstSlice<UiMediaPickerSection> sections) noexcept {
    float extent = sections.count ? m.list_inset : 0;
    for (u32 i = 0; i < sections.count; ++i)
        extent += rail_item_pitch(m, sections, i);
    return extent;
}
struct PickerLayout {
    UiRect panel, tabs, field, text, body, rail, footer;
    float cell_pitch, cell_width, cell_height, column_pitch, grid_left;
    u16 columns;
    bool masonry;
};
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
float minimum_row_pitch(const UiMediaPickerStyle& style, const PickerLayout& layout) noexcept {
    return layout.masonry ? layout.cell_width * kGifMinimumAspect + style.metrics.gif_gutter : layout.cell_pitch;
}
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
        const float body_top = layout.field.bottom + m.header_gap;
        const float body_left = sections ? left + m.rail_width : left;
        // Discord's GIF picker has no inspector; the rail runs beside the footer.
        const float footer_height = kind == UiMediaPickerKind::gif ? 0 : m.footer_height;
        layout.footer = {body_left, top + height - footer_height, left + width, top + height};
        layout.body = {body_left, body_top, left + width, layout.footer.top};
        if (sections)
            layout.rail = {left, body_top, body_left, top + height};
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
bool label_valid(Utf8View text) noexcept {
    return text.bytes <= 256 && (!text.bytes || text.data) && utf8_validate(text).valid;
}
bool source_valid(const UiMediaPickerSource* source, const UiMediaPickerContent& content) noexcept {
    if (!source)
        return true;
    if (!source->resolve || source->count > kUiMediaPickerMaximumItems ||
        source->account_scope != content.account_scope ||
        source->query_generation != content.query_generation ||
        source->sections.count > kUiMediaPickerMaximumSections ||
        (source->sections.count && !source->sections.data))
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
// Leading list padding: none above sticky headers (Discord), the list inset
// above plain results and the gutter above GIFs.
float content_top(const UiMediaPickerScene& scene) noexcept {
    const auto& m = scene.style.metrics;
    return scene.kind == UiMediaPickerKind::gif               ? m.gif_gutter
           : scene.source && scene.source->sections.count ? 0
                                                          : m.list_inset;
}
// Emoji sections keep Discord's 12 DIP bottom margin, except the last.
float section_margin(const UiMediaPickerScene& scene, u32 index) noexcept {
    const auto& section = scene.source->sections[index];
    return scene.kind == UiMediaPickerKind::emoji && section.expanded && section.count &&
                   index + 1 < scene.source->sections.count
               ? scene.style.metrics.header_gap
               : 0;
}
u32 rows_of(const UiMediaPickerScene& scene, u32 count) noexcept {
    return (count + scene.columns - 1) / scene.columns;
}
float section_extent(const UiMediaPickerScene& scene, u32 index) noexcept {
    const auto& section = scene.source->sections[index];
    return scene.style.metrics.section_height +
           (section.expanded ? rows_of(scene, section.count) * scene.cell_pitch : 0) +
           section_margin(scene, index);
}
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
           label_valid(item.detail) && item.width <= 65536 && item.height <= 65536 &&
           ((!item.width && !item.height) || (item.width && item.height));
}
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
            VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!next)
            return false;
        if (scene.tiles)
            VirtualFree(scene.tiles, 0, MEM_RELEASE);
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
UiWidgetHandle add(UiMediaPickerScene& scene, UiRect rectangle,
                   UiMediaPickerProjection::WidgetTarget mapping, Utf8View label, bool enabled = true,
                   UiWidgetHandle parent = ui_widget_invalid_handle()) noexcept {
    const auto origin =
        parent.generation ? ui_widget_manager_layout_node(scene.widgets, parent)->rectangle : scene.panel;
    auto style = ui_layout_style();
    style.width = ui_layout_fixed(ui_rect_width(rectangle));
    style.height = ui_layout_fixed(ui_rect_height(rectangle));
    style.margin.left = rectangle.left - origin.left;
    style.margin.top = rectangle.top - origin.top;
    style.flags |= ui_layout_node_clip_children | ui_layout_node_allow_overlap;
    const u32 id = scene.widgets.count + 1;
    const auto result = ui_widget_manager_add_prevalidated(
        &scene.widgets, parent.generation ? parent : scene.root,
        {id,
         mapping.action == UiMediaPickerAction::edit ? UiWidgetKind::text_field : UiWidgetKind::button,
         static_cast<u16>(enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                  : ui_widget_none),
         {label, style, {}, 0, id}});
    if (!result.ok() || result.handle.slot >= kUiMediaPickerWidgetCapacity)
        return ui_widget_invalid_handle();
    scene.projection->targets[result.handle.slot] = mapping;
    return result.handle;
}
UiRect grid_cell(const UiMediaPickerScene& scene, u32 column, float top) noexcept {
    const float left = scene.grid_left + column * scene.column_pitch;
    return {left, top, left + scene.cell_width, top + scene.cell_height};
}
bool cell(UiMediaPickerScene& scene, u32 index, UiRect rectangle, const UiMediaPickerCell* prior,
          u32 prior_count) noexcept {
    auto visible = scene.body;
    visible.top = maximum(visible.top, scene.pinned_bottom);
    const auto clip = ui_rect_intersection(rectangle, visible);
    if (!ui_rect_has_area(clip))
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
    const auto widget = add(scene, clip, {index, 0, action, scene.content->kind}, item.label, item.enabled);
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
u32 append(char* output, u32 used, u32 capacity, Utf8View text) noexcept {
    return used + utf8_copy_truncated(output + used, capacity - used, text);
}
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
bool rebuild(UiMediaPickerScene& scene, bool reuse = false) noexcept {
    const auto& m = scene.style.metrics;
    ++scene.counters.rebuilds;
    // Transient bounded stack snapshot; no second retained catalogue or allocation.
    UiMediaPickerCell prior[kUiMediaPickerVisibleCells];
    const u32 prior_count = reuse ? scene.cell_count : 0;
    if (prior_count)
        copy_bytes(prior, scene.projection->cells, prior_count * sizeof(*prior));
    reset_value(&scene.pressed);
    scene.favorite_hovered = false;
    scene.footer_index = ~u32{0};
    scene.footer_title_bytes = 0;
    ui_widget_manager_reset(&scene.widgets);
    ZeroMemory(scene.projection, sizeof(*scene.projection));
    scene.cell_count = scene.header_count = scene.rail_count = scene.media_count = 0;
    scene.pinned_bottom = scene.body.top;
    if (++scene.generation == 0)
        ++scene.generation;
    auto style = ui_layout_fill_style(ui_layout_node_visible | ui_layout_node_clip_children |
                                      ui_layout_node_allow_overlap);
    const auto root = ui_widget_manager_add_prevalidated(
        &scene.widgets, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Media picker"), style, {}, 0, 1}});
    if (!root.ok() || !ui_widget_manager_layout_rectangle(&scene.widgets, scene.panel).ok())
        return false;
    scene.root = root.handle;
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
    if (!ui_scrollbar_geometry(scene.body, scene.scroll_dips, scene.content_height).visible)
        scene.body_scrollbar_state = UiScrollbarVisualState::normal;
    if (!ui_scrollbar_geometry(scene.rail, scene.rail_scroll_dips, scene.rail_extent).visible)
        scene.rail_scrollbar_state = UiScrollbarVisualState::normal;
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
            const float extent = minimum(m.rail_width - m.list_inset * 2, m.rail_pitch - m.list_inset);
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
// Where an item sits: its section, its row within the section and its column.
struct GridPosition {
    u32 section, row, column;
};
u32 section_count(const UiMediaPickerScene& scene) noexcept {
    return scene.source->sections.count ? scene.source->sections.count : 1;
}
u32 section_first(const UiMediaPickerScene& scene, u32 section) noexcept {
    return scene.source->sections.count ? scene.source->sections[section].first : 0;
}
// Items shown in a section: collapsed sections show none.
u32 section_items(const UiMediaPickerScene& scene, u32 section) noexcept {
    if (!scene.source->sections.count)
        return scene.source->count;
    const auto& s = scene.source->sections[section];
    return s.expanded ? s.count : 0;
}
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
    const float lead = (scene.source && scene.source->sections.count ? m.section_height : 0) + content_top(scene);
    if (top < scene.scroll_dips + lead)
        scene.scroll_dips = maximum(0.f, top - lead);
    else if (top + height > scene.scroll_dips + ui_rect_height(scene.body))
        scene.scroll_dips = top + height - ui_rect_height(scene.body);
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
} // namespace
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
bool ui_media_picker_style_valid(const UiMediaPickerStyle& style) noexcept {
    const auto& m = style.metrics;
    return bounded(m.panel_width, 240, 768) && bounded(m.panel_height, 200, 768) &&
           bounded(m.viewport_inset, 0, 24) && bounded(m.anchor_gap, 0, 24) && bounded(m.padding, 4, 24) &&
           bounded(m.gap, 2, 16) && bounded(m.content_inset, 4, 24) && bounded(m.tab_height, 24, 48) &&
           bounded(m.tab_max_width, 48, 128) && bounded(m.header_gap, 4, 24) &&
           bounded(m.search_height, 32, 64) && bounded(m.footer_height, 40, 80) &&
           bounded(m.section_height, 24, 48) && bounded(m.rail_width, 32, 64) &&
           bounded(m.rail_pitch, 32, 64) && bounded(m.scrollbar_gutter, 4, 24) &&
           bounded(m.list_inset, 0, 16) && bounded(m.emoji_extent, 24, 64) &&
           bounded(m.emoji_large_extent, 24, 64) && m.emoji_large_extent >= m.emoji_extent &&
           bounded(m.emoji_padding, 0, 8) && bounded(m.sticker_extent, 48, 160) &&
           bounded(m.sticker_gap, 3, 24) && bounded(m.gif_target_width, 96, 400) &&
           bounded(m.gif_gutter, 2, 24) && bounded(m.gif_category_height, 48, 160) &&
           bounded(m.icon_extent, 16, 28) && bounded(m.text_inset, 2, 8) && bounded(m.rail_inset, 2, 8) &&
           bounded(m.footer_art, 16, 40) && bounded(style.panel_radius, 0, 24) &&
           bounded(style.control_radius, 0, 24) && bounded(style.edit_radius, 0, 24) &&
           bounded(style.selection_width, .5f, 3) && bounded(style.category_opacity, 0, 1) &&
           m.search_height >= m.icon_extent + m.text_inset * 2 && m.footer_height >= m.footer_art + 4 &&
           m.footer_height >= m.icon_extent * 2 + m.text_inset * 2 &&
           m.rail_width >= m.icon_extent + m.rail_inset * 2 && m.rail_pitch >= m.icon_extent + m.rail_inset * 2 &&
           m.rail_width >= m.list_inset * 2 + m.icon_extent && m.rail_pitch >= m.list_inset + m.icon_extent;
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
bool ui_media_picker_scene_update(UiMediaPickerScene* scene, const UiMediaPickerSource* source,
                                  const UiMediaPickerContent* content, UiRect viewport, UiRect anchor,
                                  const UiMediaPickerStyle* override_style, float preferred_width) noexcept {
    if (!scene)
        return false;
    if (!finite_rectangle(viewport) || !finite_rectangle(anchor) ||
        !bounded(preferred_width, 0, kMaximumUiCoordinate))
        return false;
    // Keep basic descriptor admission unconditional even when the publication
    // revision permits reuse of already validated section metadata.
    if (source && (!source->resolve || source->count > kUiMediaPickerMaximumItems ||
                   source->sections.count > kUiMediaPickerMaximumSections ||
                   (source->sections.count && !source->sections.data)))
        return false;
    // The publication revision owns section metadata and its borrowed labels.
    // Avoid repeating quadratic uniqueness admission during unchanged scroll frames.
    const bool admitted_source = source && scene->active && scene->source == source &&
                                 scene->source_revision == source->revision &&
                                 scene->account_scope == source->account_scope &&
                                 scene->query_generation == source->query_generation && content &&
                                 content->account_scope == source->account_scope &&
                                 content->query_generation == source->query_generation;
    if (!content || !content->account_scope || !content->query_generation ||
        content->kind > UiMediaPickerKind::emoji || content->phase > UiMediaPickerPhase::failed ||
        !(content->available_tabs & ui_media_picker_tab_bit(content->kind)) ||
        (content->available_tabs & ~7u) || content->variant > UiMediaPickerVariant::reaction ||
        (content->variant == UiMediaPickerVariant::reaction &&
         (content->kind != UiMediaPickerKind::emoji ||
          content->available_tabs != ui_media_picker_tab_bit(UiMediaPickerKind::emoji))) ||
        !content->edit.active || !label_valid(content->placeholder) ||
        !label_valid(content->status) || !ui_rect_valid(viewport) || !ui_rect_valid(anchor) ||
        (!admitted_source && !source_valid(source, *content)))
        return false;
    const UiMediaPickerStyle defaults = content->variant == UiMediaPickerVariant::reaction
                                            ? ui_media_picker_reaction_style()
                                            : ui_media_picker_style();
    UiMediaPickerStyle effective =
        override_style && ui_media_picker_style_valid(*override_style) ? *override_style : defaults;
    PickerLayout layout{};
    if (!picker_layout(effective, source, content->kind, content->variant, content->back, viewport, anchor,
                       preferred_width, &layout)) {
        effective = defaults;
        if (!picker_layout(effective, source, content->kind, content->variant, content->back, viewport,
                           anchor, preferred_width, &layout))
            return false;
    }
    const bool same_style = bytes_equal(&scene->style, &effective, sizeof(effective));
    const u16 flags = static_cast<u16>(content->available_tabs | (content->back ? 8 : 0) |
                                       (static_cast<u16>(content->phase) << 4) |
                                       (static_cast<u16>(content->variant) << 8));
    if (ui_media_picker_scene_current(*scene) && same_style && scene->source == source &&
        scene->content == content && ui_rect_equal(scene->viewport, viewport) &&
        ui_rect_equal(scene->panel, layout.panel))
        return true;
    if (scene->active && same_style && scene->source == source &&
        scene->source_revision == (source ? source->revision : 0) &&
        scene->account_scope == content->account_scope &&
        scene->query_generation == content->query_generation && scene->kind == content->kind &&
        scene->layout_flags == flags && ui_rect_equal(scene->viewport, viewport) &&
        ui_rect_equal(scene->panel, layout.panel)) {
        scene->content = content;
        scene->content_revision = content->revision;
        synchronize_focus(*scene);
        ++scene->revision;
        return true; // Native caret/search text changes need no gallery re-resolve.
    }
    if (!scene->storage.arena.base) {
        if (!ui_fixed_widget_storage_initialize(&scene->storage, kUiMediaPickerWidgetCapacity,
                                                sizeof(UiMediaPickerProjection),
                                                alignof(UiMediaPickerProjection)))
            return false;
        ++scene->counters.allocations;
    }
    if (!scene->widgets.storage.data &&
        !ui_widget_manager_initialize(&scene->widgets, scene->storage.widgets, scene->storage.layout_nodes))
        return false;
    const bool replacement = !scene->active || scene->account_scope != content->account_scope ||
                             scene->query_generation != content->query_generation ||
                             scene->kind != content->kind || scene->variant != content->variant;
    scene->projection = static_cast<UiMediaPickerProjection*>(scene->storage.sidecar);
    scene->source = source;
    scene->content = content;
    scene->style = effective;
    scene->viewport = viewport;
    scene->panel = layout.panel;
    scene->tabs = layout.tabs;
    scene->field = layout.field;
    scene->text = layout.text;
    scene->body = layout.body;
    scene->rail = layout.rail;
    scene->footer = layout.footer;
    scene->columns = layout.columns;
    scene->cell_pitch = layout.cell_pitch;
    scene->cell_width = layout.cell_width;
    scene->cell_height = layout.cell_height;
    scene->column_pitch = layout.column_pitch;
    scene->grid_left = layout.grid_left;
    scene->masonry = layout.masonry;
    scene->account_scope = content->account_scope;
    scene->query_generation = content->query_generation;
    scene->source_revision = source ? source->revision : 0;
    scene->content_revision = content->revision;
    scene->kind = content->kind;
    scene->variant = content->variant;
    scene->layout_flags = flags;
    if (replacement) {
        scene->scroll_dips = scene->rail_scroll_dips = 0;
        scene->body_scrollbar_state = scene->rail_scrollbar_state = UiScrollbarVisualState::normal;
        scene->selected_index = ~u32{0};
    }
    scene->active = true;
    if (!rebuild(*scene)) {
        ui_media_picker_scene_close(scene);
        return false;
    }
    return true;
}
void ui_media_picker_scene_cancel_capture(UiMediaPickerScene* scene) noexcept {
    if (!scene)
        return;
    reset_value(&scene->pressed);
    ui_widget_manager_capture(&scene->widgets, ui_widget_invalid_handle());
}
void ui_media_picker_scene_close(UiMediaPickerScene* scene) noexcept {
    if (!scene)
        return;
    ui_media_picker_scene_cancel_capture(scene);
    ui_widget_manager_reset(&scene->widgets);
    scene->active = false;
    scene->body_scrollbar_state = scene->rail_scrollbar_state = UiScrollbarVisualState::normal;
    scene->favorite_hovered = false;
    scene->cell_count = scene->header_count = scene->rail_count = scene->media_count = 0;
    scene->source = nullptr;
    scene->content = nullptr;
    scene->tile_count = 0;
    scene->footer_title_bytes = scene->footer_detail_bytes = 0;
}
void ui_media_picker_scene_release(UiMediaPickerScene* scene) noexcept {
    if (!scene)
        return;
    ui_media_picker_scene_close(scene);
    ui_fixed_widget_storage_release(&scene->storage);
    if (scene->tiles)
        VirtualFree(scene->tiles, 0, MEM_RELEASE);
    // Generations keep counting so stale targets from before stay fenced.
    const u32 generation = scene->generation;
    reset_value(scene);
    scene->generation = generation;
}
UiMediaPickerTarget ui_media_picker_scene_hit_test(const UiMediaPickerScene& scene, float x,
                                                   float y) noexcept {
    auto hit = target(scene, ui_widget_manager_hit_test(scene.widgets, x, y));
    if (hit.action == UiMediaPickerAction::item) {
        for (u32 i = 0; i < scene.cell_count; ++i) {
            const auto& cell = scene.projection->cells[i];
            UiRect button{};
            if (cell.source_index == hit.source_index &&
                ui_media_picker_scene_favorite(scene, i, &button) && ui_rect_contains(button, x, y)) {
                if (cell.item.favorite_pending)
                    return {};
                hit.action = UiMediaPickerAction::favorite;
                break;
            }
        }
    }
    return hit;
}
bool ui_media_picker_scene_hover(UiMediaPickerScene* scene, float x, float y, u64 now_ms) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene))
        return false;
    const auto widget = ui_widget_manager_hit_test(scene->widgets, x, y);
    const bool widget_changed = scene->widgets.hovered.slot != widget.slot ||
                                scene->widgets.hovered.generation != widget.generation;
    if (widget_changed && !ui_widget_manager_hover(&scene->widgets, widget))
        return false;
    bool favorite_hovered = false;
    for (u32 i = 0; i < scene->cell_count; ++i) {
        const auto& cell = scene->projection->cells[i];
        UiRect button{};
        if (cell.widget.slot == widget.slot && cell.widget.generation == widget.generation &&
            ui_media_picker_scene_favorite(*scene, i, &button)) {
            favorite_hovered = ui_rect_contains(button, x, y);
            break;
        }
    }
    if (!widget_changed && scene->favorite_hovered == favorite_hovered)
        return false;
    scene->favorite_hovered = favorite_hovered;
    scene->widgets.hover_started_ms = now_ms;
    refresh_footer(*scene);
    ++scene->revision;
    return true;
}
bool ui_media_picker_scene_pointer_down(UiMediaPickerScene* scene, float x, float y) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene))
        return false;
    const auto widget = ui_widget_manager_hit_test(scene->widgets, x, y);
    scene->pressed = ui_media_picker_scene_hit_test(*scene, x, y);
    return scene->pressed.action != UiMediaPickerAction::none &&
           ui_widget_manager_capture(&scene->widgets, widget);
}
UiMediaPickerTarget ui_media_picker_scene_pointer_up(UiMediaPickerScene* scene, float x, float y) noexcept {
    if (!scene)
        return {};
    const auto pressed = scene->pressed;
    const auto hit = ui_media_picker_scene_hit_test(*scene, x, y);
    const bool valid = scene->widgets.captured.generation && ui_media_picker_target_equal(pressed, hit);
    ui_media_picker_scene_cancel_capture(scene);
    return valid ? hit : UiMediaPickerTarget{};
}
bool ui_media_picker_scene_scroll(UiMediaPickerScene* scene, float delta, bool rail) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || !scene->content ||
        scene->content->phase != UiMediaPickerPhase::ready ||
        !bounded(delta, -kMaximumPaintCoordinate, kMaximumPaintCoordinate))
        return false;
    const float maximum_scroll = rail ? maximum(0.f, scene->rail_extent - ui_rect_height(scene->rail))
                                      : maximum(0.f, scene->content_height - ui_rect_height(scene->body));
    float& scroll = rail ? scene->rail_scroll_dips : scene->scroll_dips;
    const float next = clamped(scroll + delta, 0.f, maximum_scroll);
    if (next == scroll)
        return false;
    const float previous = scroll;
    scroll = next;
    if (rebuild(*scene, true))
        return true;
    // A failed newly exposed row must not revoke the whole picker. Rebuild
    // the last usable viewport without borrowing the failed projection.
    scroll = previous;
    if (!rebuild(*scene))
        ui_media_picker_scene_close(scene);
    return false;
}
bool ui_media_picker_scene_scroll_section(UiMediaPickerScene* scene, u64 id) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || !scene->source ||
        scene->content->phase != UiMediaPickerPhase::ready)
        return false;
    float offset = content_top(*scene);
    for (u32 i = 0; i < scene->source->sections.count; ++i) {
        if (scene->source->sections[i].id == id) {
            scene->scroll_dips =
                minimum(offset, maximum(0.f, scene->content_height - ui_rect_height(scene->body)));
            return safe_rebuild(*scene);
        }
        offset += section_extent(*scene, i);
    }
    return false;
}
bool ui_media_picker_scene_select(UiMediaPickerScene* scene, u32 index) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene))
        return false;
    float top = 0, height = 0;
    UiMediaPickerItem item{};
    if (!item_position(*scene, index, &top, &height) ||
        !scene->source->resolve(scene->source->context, index, &item) || !item_valid(item) || !item.enabled)
        return false;
    return select_index(*scene, index, top, height);
}
bool ui_media_picker_scene_move_selection(UiMediaPickerScene* scene, i32 horizontal,
                                          i32 vertical) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || !scene->source || !scene->source->count ||
        scene->content->phase != UiMediaPickerPhase::ready || horizontal < -1 || horizontal > 1 ||
        vertical < -1 || vertical > 1 || (!horizontal && !vertical) || (horizontal && vertical))
        return false;
    const u32 selected = scene->selected_index;
    if (selected >= scene->source->count)
        return select_available(*scene, 0, 1);
    u32 index = selected;
    if (scene->masonry)
        index = selected < scene->tile_count ? masonry_step(*scene, selected, horizontal, vertical) : selected;
    else if (vertical)
        index = grid_step(*scene, selected, vertical);
    else if (horizontal > 0 || selected)
        index = minimum(scene->source->count - 1, selected + horizontal);
    if (index == selected)
        return false;
    return select_available(*scene, index, index > selected ? 1 : -1);
}
bool ui_media_picker_scene_page_selection(UiMediaPickerScene* scene, bool reverse) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || !scene->source || !scene->source->count ||
        scene->content->phase != UiMediaPickerPhase::ready)
        return false;
    const u32 selected = scene->selected_index;
    if (selected >= scene->source->count)
        return select_available(*scene, 0, 1);
    const i32 rows = maximum(1, static_cast<i32>(ui_rect_height(scene->body) / scene->cell_pitch));
    u32 index = selected;
    if (scene->masonry)
        for (i32 i = 0; i < rows; ++i)
            index = masonry_step(*scene, index, 0, reverse ? -1 : 1);
    else
        index = grid_step(*scene, selected, reverse ? -rows : rows);
    return index != selected && select_available(*scene, index, index > selected ? 1 : -1);
}
bool ui_media_picker_scene_edge_selection(UiMediaPickerScene* scene, bool last) noexcept {
    return scene && ui_media_picker_scene_current(*scene) && scene->source && scene->source->count &&
           select_available(*scene, last ? scene->source->count - 1 : 0, last ? -1 : 1);
}
bool ui_media_picker_scene_row_edge_selection(UiMediaPickerScene* scene, bool last) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || !scene->source || !scene->source->count)
        return false;
    GridPosition at{};
    if (scene->masonry || scene->selected_index >= scene->source->count ||
        !grid_locate(*scene, scene->selected_index, &at))
        return ui_media_picker_scene_edge_selection(scene, last);
    const u32 first = section_first(*scene, at.section) + at.row * scene->columns;
    const u32 length =
        minimum<u32>(scene->columns, section_items(*scene, at.section) - at.row * scene->columns);
    const u32 index = last ? first + length - 1 : first;
    return index != scene->selected_index && select_available(*scene, index, last ? -1 : 1);
}
bool ui_media_picker_scene_favorite(const UiMediaPickerScene& scene, u32 index, UiRect* rectangle,
                                    UiRect* full_rectangle) noexcept {
    if (!rectangle || !ui_media_picker_scene_current(scene) || index >= scene.cell_count)
        return false;
    const auto& cell = scene.projection->cells[index];
    if (!cell.item.enabled || !cell.item.favoritable || cell.item.kind != UiMediaPickerItemKind::gif ||
        !ui_widget_manager_input_enabled(scene.widgets, cell.widget))
        return false;
    const auto same = [&](UiWidgetHandle widget) noexcept {
        return widget.slot == cell.widget.slot && widget.generation == cell.widget.generation;
    };
    if (!same(scene.widgets.hovered) && !same(scene.widgets.focused) && !same(scene.widgets.captured))
        return false;
    const auto& m = scene.style.metrics;
    // Discord: 4 DIP from the tile's top-right corner.
    const float inset = m.emoji_padding;
    const auto r = cell.rectangle;
    const float extent =
        minimum(m.icon_extent + m.text_inset * 2, minimum(ui_rect_width(r), ui_rect_height(r)) - inset * 2);
    if (extent <= 0)
        return false;
    const UiRect button{r.right - inset - extent, r.top + inset, r.right - inset, r.top + inset + extent};
    *rectangle = ui_rect_intersection(button, cell.clip);
    if (full_rectangle)
        *full_rectangle = button;
    return ui_rect_has_area(*rectangle);
}
UiMediaPickerTarget ui_media_picker_scene_selected_favorite_target(
    const UiMediaPickerScene& scene) noexcept {
    if (!ui_media_picker_scene_current(scene) || scene.content->edit.focused)
        return {};
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        UiRect button{};
        if (cell.source_index == scene.selected_index && scene.widgets.focused.slot == cell.widget.slot &&
            scene.widgets.focused.generation == cell.widget.generation && !cell.item.favorite_pending &&
            ui_media_picker_scene_favorite(scene, i, &button)) {
            auto result = target(scene, cell.widget);
            result.action = UiMediaPickerAction::favorite;
            return result;
        }
    }
    return {};
}
ConstSlice<UiMediaPickerCell> ui_media_picker_scene_cells(const UiMediaPickerScene& scene) noexcept {
    return ui_media_picker_scene_current(scene)
               ? ConstSlice<UiMediaPickerCell>{scene.projection->cells, scene.cell_count}
               : ConstSlice<UiMediaPickerCell>{};
}
bool ui_media_picker_scene_rail_media(const UiMediaPickerScene& scene, u32 index,
                                      UiMediaPickerRailMediaSlot* output) noexcept {
    if (!output || !ui_media_picker_scene_current(scene) || index >= scene.rail_count)
        return false;
    const auto& slot = scene.projection->rail[index];
    const auto& section = scene.source->sections[slot.section_index];
    const bool guild = section.visual == UiMediaPickerSectionVisual::guild && section.guild_id;
    const bool media = section.visual == UiMediaPickerSectionVisual::media && section.asset_id;
    if ((!guild && !media) || !section.icon_variant)
        return false;
    // Server icons fill the 32 DIP item; pack covers keep a small inset.
    const float inset = media ? scene.style.metrics.rail_inset : 0;
    const UiRect rectangle{slot.rectangle.left + inset, slot.rectangle.top + inset,
                           slot.rectangle.right - inset, slot.rectangle.bottom - inset};
    *output = {target(scene, slot.widget),
               {&scene.widgets, slot.widget},
               guild ? section.guild_id : 0,
               section.icon_variant,
               guild && section.icon_animated,
               rectangle,
               ui_rect_intersection(rectangle, scene.rail),
               scene.style.control_radius,
               media ? section.asset_id : 0,
               media ? section.asset_flags : static_cast<u16>(0)};
    return ui_rect_has_area(output->clip);
}
namespace {
bool media_slot(const UiMediaPickerScene& scene, const UiMediaPickerCell& cell, UiRect rectangle, UiRect clip,
                UiMediaPickerMediaSlot* output) noexcept {
    if (!cell.item.enabled || (!cell.item.asset_id && !cell.item.regional_flag))
        return false;
    const bool contain =
        cell.item.kind == UiMediaPickerItemKind::sticker || cell.item.kind == UiMediaPickerItemKind::emoji;
    const UiMediaPickerTarget media_target{scene.account_scope,
                                           scene.query_generation,
                                           cell.item.id,
                                           scene.source_revision,
                                           cell.source_index,
                                           scene.generation,
                                           cell.item.kind == UiMediaPickerItemKind::category
                                               ? UiMediaPickerAction::category
                                               : UiMediaPickerAction::item,
                                           scene.kind};
    *output = {media_target,
               {&scene.widgets, cell.widget},
               cell.item.asset_id,
               rectangle,
               ui_rect_intersection(rectangle, clip),
               scene.style.control_radius,
               cell.item.kind == UiMediaPickerItemKind::category ? scene.style.category_opacity : 1.f,
               cell.item.variant,
               cell.item.width,
               cell.item.height,
               contain ? UiMediaPickerFit::contain : UiMediaPickerFit::cover,
               cell.item.asset_flags,
               cell.item.kind,
               cell.item.regional_flag};
    return true;
}
} // namespace
bool ui_media_picker_scene_media(const UiMediaPickerScene& scene, u32 index,
                                 UiMediaPickerMediaSlot* output) noexcept {
    if (!output || !ui_media_picker_scene_current(scene) || index >= scene.cell_count)
        return false;
    const auto& cell = scene.projection->cells[index];
    UiRect rectangle = cell.rectangle;
    // Emoji art sits inside its padding; sticker previews inside their node.
    const float inset = cell.item.kind == UiMediaPickerItemKind::emoji     ? scene.style.metrics.emoji_padding
                        : cell.item.kind == UiMediaPickerItemKind::sticker ? kStickerNodePadding
                                                                           : 0;
    rectangle = {rectangle.left + inset, rectangle.top + inset, rectangle.right - inset, rectangle.bottom - inset};
    return media_slot(scene, cell, rectangle, cell.clip, output);
}
const UiMediaPickerCell* ui_media_picker_scene_inspected(const UiMediaPickerScene& scene) noexcept {
    if (!ui_media_picker_scene_current(scene) || !ui_rect_has_area(scene.footer) || !scene.cell_count)
        return nullptr;
    // Discord inspects grid position (0, 0) by default: the first shown item,
    // while it is on screen.
    u32 first_index = scene.source && !scene.source->sections.count ? 0 : ~u32{0};
    for (u32 s = 0; scene.source && s < scene.source->sections.count && first_index == ~u32{0}; ++s)
        if (scene.source->sections[s].expanded && scene.source->sections[s].count)
            first_index = scene.source->sections[s].first;
    const UiMediaPickerCell* selected = nullptr;
    const UiMediaPickerCell* first = nullptr;
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        if (scene.widgets.hovered.slot == cell.widget.slot &&
            scene.widgets.hovered.generation == cell.widget.generation)
            return &cell;
        if (cell.source_index == scene.selected_index)
            selected = &cell;
        if (cell.source_index == first_index && cell.item.enabled)
            first = &cell;
    }
    return selected ? selected : first;
}
bool ui_media_picker_scene_footer_media(const UiMediaPickerScene& scene, UiMediaPickerMediaSlot* output) noexcept {
    const auto* cell = output ? ui_media_picker_scene_inspected(scene) : nullptr;
    if (!cell)
        return false;
    const auto& m = scene.style.metrics;
    const float left = scene.footer.left + m.padding;
    const float top = (scene.footer.top + scene.footer.bottom - m.footer_art) * .5f;
    const UiRect art{left, top, left + m.footer_art, top + m.footer_art};
    return media_slot(scene, *cell, art, scene.footer, output);
}
Utf8View ui_media_picker_scene_footer_title(const UiMediaPickerScene& scene) noexcept {
    return ui_media_picker_scene_current(scene) ? Utf8View{scene.footer_title, scene.footer_title_bytes}
                                                : Utf8View{};
}
Utf8View ui_media_picker_scene_footer_detail(const UiMediaPickerScene& scene) noexcept {
    return ui_media_picker_scene_current(scene) ? Utf8View{scene.footer_detail, scene.footer_detail_bytes}
                                                : Utf8View{};
}
UiScrollbarGeometry ui_media_picker_scene_scrollbar(const UiMediaPickerScene& scene, bool rail) noexcept {
    if (!ui_media_picker_scene_current(scene) || scene.content->phase != UiMediaPickerPhase::ready)
        return {};
    return rail ? ui_scrollbar_geometry(scene.rail, scene.rail_scroll_dips, scene.rail_extent)
                : ui_scrollbar_geometry(scene.body, scene.scroll_dips, scene.content_height);
}
bool ui_media_picker_scene_set_scrollbar_state(UiMediaPickerScene* scene, bool rail,
                                               UiScrollbarVisualState state) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || state > UiScrollbarVisualState::pressed)
        return false;
    if (!ui_media_picker_scene_scrollbar(*scene, rail).visible)
        state = UiScrollbarVisualState::normal;
    auto& current_state = rail ? scene->rail_scrollbar_state : scene->body_scrollbar_state;
    if (current_state == state)
        return false;
    current_state = state;
    ++scene->revision;
    return true;
}
} // namespace maicord
