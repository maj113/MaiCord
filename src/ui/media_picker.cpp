#include "maicord/ui/media_picker.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/regional_flag.hpp"

#include <Windows.h>

namespace maicord {
namespace {
static_assert(sizeof(UiMediaPickerScene) + sizeof(UiMediaPickerProjection) +
                  kUiMediaPickerWidgetCapacity * (sizeof(UiWidget) + sizeof(UiLayoutNode)) <=
              64 * 1024);
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
struct PickerLayout {
    UiRect panel, tabs, field, text, body, rail, footer;
    float cell_pitch;
    u8 columns;
};
bool picker_layout(const UiMediaPickerStyle& style, const UiMediaPickerSource* source,
                   UiMediaPickerKind kind, UiMediaPickerVariant variant, bool back, UiRect viewport,
                   UiRect anchor, float preferred_width, PickerLayout* output) noexcept {
    const auto& m = style.metrics;
    const bool reaction = variant == UiMediaPickerVariant::reaction;
    const float available_width = ui_rect_width(viewport) - m.viewport_inset * 2;
    const float minimum_width = minimum(m.panel_width, available_width);
    const float width = reaction || preferred_width == 0
                            ? minimum_width
                            : clamped(preferred_width, minimum_width, available_width);
    const float height = minimum(m.panel_height, ui_rect_height(viewport) - m.viewport_inset * 2);
    if (width < 240 || height < 200)
        return false;
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
        top = clamped(anchor.top, viewport.top + m.viewport_inset, viewport.bottom - height - m.viewport_inset);
    }
    PickerLayout layout{};
    layout.panel = {left, top, left + width, top + height};
    // The reaction popout has no GIF/Sticker/Emoji selector row.
    layout.tabs = {left + m.padding, top + m.content_inset, left + width - m.padding,
                   top + m.content_inset + (reaction ? 0 : m.tab_height)};
    const float field_top = reaction ? layout.tabs.bottom : layout.tabs.bottom + m.padding;
    layout.field = {layout.tabs.left, field_top, layout.tabs.right, field_top + m.search_height};
    layout.text = {layout.field.left + m.icon_extent + m.gap * 2 + (back ? m.icon_extent : 0),
                   layout.field.top + m.text_inset, layout.field.right - m.gap,
                   layout.field.bottom - m.text_inset};
    layout.footer = {left, top + height - m.footer_height, left + width, top + height};
    layout.body = {left, layout.field.bottom + m.padding, left + width, layout.footer.top};
    if (source && source->sections.count) {
        layout.rail = {left, layout.body.top, left + m.rail_width, layout.body.bottom};
        layout.body.left += m.rail_width;
    }
    layout.columns = kind == UiMediaPickerKind::gif ? 2 : kind == UiMediaPickerKind::sticker ? 4 : 8;
    const float cell_width =
        (ui_rect_width(layout.body) - m.padding * 2 - m.scrollbar_gutter - m.gap * (layout.columns - 1)) /
        layout.columns;
    layout.cell_pitch = kind == UiMediaPickerKind::gif
                            ? maximum(m.gif_min_height, cell_width * m.gif_aspect_ratio + m.gap)
                            : cell_width + m.gap;
    if (kind == UiMediaPickerKind::emoji)
        layout.cell_pitch = maximum(m.emoji_min_height, layout.cell_pitch);
    const float art_inset = kind == UiMediaPickerKind::sticker ? m.sticker_inset
                            : kind == UiMediaPickerKind::emoji ? m.emoji_inset
                                                               : 0;
    // Validate the actual available pane, not just the preferred panel width.
    // Partial rows, collapsed headers and the category rail have fixed budgets.
    if (!ui_rect_has_area(layout.text) || (!reaction && (ui_rect_width(layout.tabs) - m.gap * 2) / 3 < 24) ||
        cell_width <= art_inset * 2 + 1 || layout.cell_pitch - m.gap <= art_inset * 2 + 1 ||
        style.control_radius * 2 > minimum(cell_width, layout.cell_pitch - m.gap) ||
        style.edit_radius * 2 > m.search_height)
        return false;
    // Keep the common full body. Reject an incompatible override as a whole;
    // never hide its bottom rows or stretch compact Emoji cells to fit a bank.
    const float body_height = ui_rect_height(layout.body);
    if (body_height <= m.content_inset + 1)
        return false;
    const u32 visible_rows = static_cast<u32>(body_height / layout.cell_pitch) + 2;
    const u32 cells = visible_rows * layout.columns;
    const bool sections = source && source->sections.count;
    const u32 headers = sections ? static_cast<u32>(body_height / m.section_height) + 2 : 0;
    const u32 rail_slots = sections ? static_cast<u32>(body_height / m.rail_pitch) + 2 : 0;
    // Disabled previews use at most five quads; category rail symbols eleven,
    // section arrows two. Reserve native chrome/focus work within the shared
    // fixed paint budget, even when every visible source preview is unavailable.
    if (cells > kUiMediaPickerVisibleCells || headers > kUiMediaPickerVisibleSections ||
        rail_slots > kUiMediaPickerVisibleSections ||
        cells * 5 + headers * 2 + rail_slots * 11 + 24 > kMaxPaintQuads)
        return false;
    *output = layout;
    return true;
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
        if (!section.id || section.first != first || section.count > source->count - first ||
            !label_valid(section.label) || section.visual > UiMediaPickerSectionVisual::guild ||
            (section.visual != UiMediaPickerSectionVisual::guild &&
             (section.guild_id || section.icon_variant || section.icon_animated)) ||
            (section.visual == UiMediaPickerSectionVisual::guild && !section.guild_id))
            return false;
        first += section.count;
        for (u32 j = 0; j < i; ++j)
            if (source->sections[j].id == section.id)
                return false;
    }
    return !source->sections.count || first == source->count;
}
float section_extent(const UiMediaPickerScene& scene, const UiMediaPickerSection& section) noexcept {
    const auto& m = scene.style.metrics;
    return m.section_height +
           (section.expanded ? (section.count + scene.columns - 1) / scene.columns * scene.cell_pitch : 0);
}
float content_extent(const UiMediaPickerScene& scene) noexcept {
    const auto& m = scene.style.metrics;
    if (!scene.source || !scene.content || scene.content->phase != UiMediaPickerPhase::ready)
        return 0;
    if (!scene.source->sections.count)
        return scene.source->count ? m.content_inset + (scene.source->count + scene.columns - 1) /
                                                           scene.columns * scene.cell_pitch
                                   : 0;
    float height = m.content_inset;
    for (u32 i = 0; i < scene.source->sections.count; ++i)
        height += section_extent(scene, scene.source->sections[i]);
    return height;
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
bool cell(UiMediaPickerScene& scene, u32 index, float top, u32 column, const UiMediaPickerCell* prior,
          u32 prior_count) noexcept {
    const auto& m = scene.style.metrics;
    const float width =
        (ui_rect_width(scene.body) - m.padding * 2 - m.scrollbar_gutter - m.gap * (scene.columns - 1)) /
        scene.columns;
    const float left = scene.body.left + m.padding + column * (width + m.gap);
    const UiRect rectangle{left, top, left + width, top + scene.cell_pitch - m.gap};
    const auto clip = ui_rect_intersection(rectangle, scene.body);
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
    if ((!reused && !scene.source->resolve(scene.source->context, index, &item)) || !item_valid(item) ||
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
    return true;
}
bool project_items(UiMediaPickerScene& scene, u32 first, u32 count, float top,
                   const UiMediaPickerCell* prior, u32 prior_count) noexcept {
    const u32 rows = (count + scene.columns - 1) / scene.columns;
    const float offset = maximum(0.f, scene.body.top - top);
    const u32 first_row = minimum(rows, static_cast<u32>(offset / scene.cell_pitch));
    const u32 last_row =
        minimum(rows, first_row + static_cast<u32>(ui_rect_height(scene.body) / scene.cell_pitch) + 2);
    for (u32 row = first_row; row < last_row; ++row)
        for (u32 column = 0; column < scene.columns && row * scene.columns + column < count; ++column)
            if (!cell(scene, first + row * scene.columns + column, top + row * scene.cell_pitch, column,
                      prior, prior_count))
                return false;
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
bool rebuild(UiMediaPickerScene& scene, bool reuse = false) noexcept {
    const auto& m = scene.style.metrics;
    // Transient bounded stack snapshot; no second retained catalogue or allocation.
    UiMediaPickerCell prior[kUiMediaPickerVisibleCells];
    const u32 prior_count = reuse ? scene.cell_count : 0;
    if (prior_count)
        copy_bytes(prior, scene.projection->cells, prior_count * sizeof(*prior));
    scene.pressed = {};
    scene.favorite_hovered = false;
    ui_widget_manager_reset(&scene.widgets);
    ZeroMemory(scene.projection, sizeof(*scene.projection));
    scene.cell_count = scene.header_count = scene.rail_count = scene.media_count = 0;
    if (++scene.generation == 0)
        ++scene.generation;
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::overlay;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.flags |= ui_layout_node_clip_children | ui_layout_node_allow_overlap;
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
    scene.content_height = content_extent(scene);
    scene.scroll_dips =
        minimum(scene.scroll_dips, maximum(0.f, scene.content_height - ui_rect_height(scene.body)));
    const float rail_extent = scene.content->phase == UiMediaPickerPhase::ready && scene.source
                                  ? scene.source->sections.count * m.rail_pitch
                                  : 0;
    scene.rail_scroll_dips =
        minimum(scene.rail_scroll_dips, maximum(0.f, rail_extent - ui_rect_height(scene.rail)));
    if (!ui_scrollbar_geometry(scene.body, scene.scroll_dips, scene.content_height).visible)
        scene.body_scrollbar_state = UiScrollbarVisualState::normal;
    if (!ui_scrollbar_geometry(scene.rail, scene.rail_scroll_dips, rail_extent).visible)
        scene.rail_scrollbar_state = UiScrollbarVisualState::normal;
    if (scene.source && scene.content->phase == UiMediaPickerPhase::ready) {
        float top = scene.body.top + m.content_inset - scene.scroll_dips;
        if (!scene.source->sections.count) {
            if (!project_items(scene, 0, scene.source->count, top, prior, prior_count))
                return false;
        } else {
            for (u32 i = 0; i < scene.source->sections.count; ++i) {
                const auto& section = scene.source->sections[i];
                const float extent = section_extent(scene, section);
                if (top < scene.body.bottom && top + extent > scene.body.top) {
                    const auto rectangle = ui_rect_intersection({scene.body.left + m.padding, top,
                                                                 scene.body.right - m.scrollbar_gutter,
                                                                 top + m.section_height},
                                                                scene.body);
                    if (ui_rect_has_area(rectangle)) {
                        if (scene.header_count == kUiMediaPickerVisibleSections)
                            return false;
                        const auto widget =
                            add(scene, rectangle,
                                {0, static_cast<u16>(i), UiMediaPickerAction::section, scene.content->kind},
                                section.label);
                        scene.projection->headers[scene.header_count++] = {
                            {scene.body.left + m.padding, top, scene.body.right - m.scrollbar_gutter,
                             top + m.section_height},
                            widget,
                            static_cast<u16>(i)};
                    }
                    if (section.expanded && !project_items(scene, section.first, section.count,
                                                           top + m.section_height, prior, prior_count))
                        return false;
                }
                top += extent;
            }
            const u32 first_rail = minimum(scene.source->sections.count,
                                           static_cast<u32>(scene.rail_scroll_dips / m.rail_pitch));
            const u32 end =
                minimum(scene.source->sections.count, first_rail + kUiMediaPickerVisibleSections);
            for (u32 i = first_rail; i < end; ++i) {
                const float y = scene.rail.top + i * m.rail_pitch - scene.rail_scroll_dips;
                const float extent = minimum(m.rail_width - m.rail_inset * 2,
                                             m.rail_pitch - minimum(m.rail_inset, m.gap) * 2);
                const float left = (scene.rail.left + scene.rail.right - extent) * .5f;
                const float artwork_top = y + (m.rail_pitch - extent) * .5f;
                const UiRect artwork{left, artwork_top, left + extent, artwork_top + extent};
                const auto r = ui_rect_intersection(artwork, scene.rail);
                if (!ui_rect_has_area(r))
                    continue;
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
    ++scene.revision;
    return true;
}
bool item_position(const UiMediaPickerScene& scene, u32 index, float* top) noexcept {
    const auto& m = scene.style.metrics;
    if (!scene.source || !scene.content || scene.content->phase != UiMediaPickerPhase::ready ||
        index >= scene.source->count)
        return false;
    *top = m.content_inset;
    if (!scene.source->sections.count) {
        *top += index / scene.columns * scene.cell_pitch;
        return true;
    }
    for (u32 i = 0; i < scene.source->sections.count; ++i) {
        const auto& section = scene.source->sections[i];
        if (index >= section.first && index - section.first < section.count) {
            if (!section.expanded)
                return false;
            *top += m.section_height + (index - section.first) / scene.columns * scene.cell_pitch;
            return true;
        }
        *top += section_extent(scene, section);
    }
    return false;
}
bool safe_rebuild(UiMediaPickerScene& scene) noexcept {
    if (rebuild(scene, ui_media_picker_scene_current(scene)))
        return true;
    ui_media_picker_scene_close(&scene);
    return false;
}
bool select_index(UiMediaPickerScene& scene, u32 index, float top) noexcept {
    const auto& m = scene.style.metrics;
    scene.selected_index = index;
    if (top < scene.scroll_dips + m.content_inset)
        scene.scroll_dips = maximum(0.f, top - m.content_inset);
    else if (top + scene.cell_pitch > scene.scroll_dips + ui_rect_height(scene.body))
        scene.scroll_dips = top + scene.cell_pitch - ui_rect_height(scene.body);
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
        float top = 0;
        UiMediaPickerItem item{};
        if (!item_position(scene, static_cast<u32>(candidate), &top) ||
            !scene.source->resolve(scene.source->context, static_cast<u32>(candidate), &item) ||
            !item_valid(item))
            return false;
        if (item.enabled)
            return select_index(scene, static_cast<u32>(candidate), top);
        candidate += direction;
    }
    return false;
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
    float bottom = scene.style.metrics.content_inset;
    for (u32 i = 0; i < scene.source->sections.count; ++i) {
        bottom += section_extent(scene, scene.source->sections[i]);
        if (bottom > scene.scroll_dips + scene.style.metrics.content_inset)
            return i;
    }
    return scene.source->sections.count - 1;
}
bool ui_media_picker_style_valid(const UiMediaPickerStyle& style) noexcept {
    const auto& m = style.metrics;
    return bounded(m.panel_width, 240, 768) && bounded(m.panel_height, 200, 768) &&
           bounded(m.viewport_inset, 0, 24) && bounded(m.anchor_gap, 0, 24) && bounded(m.padding, 4, 24) &&
           bounded(m.gap, 2, 16) && bounded(m.content_inset, 4, 24) && bounded(m.tab_height, 24, 48) &&
           bounded(m.tab_max_width, 48, 128) && bounded(m.search_height, 32, 64) &&
           bounded(m.footer_height, 40, 80) && bounded(m.section_height, 24, 48) &&
           bounded(m.rail_width, 32, 64) && bounded(m.rail_pitch, 32, 64) &&
           bounded(m.scrollbar_gutter, 8, 24) && bounded(m.sticker_inset, 0, 16) &&
           bounded(m.emoji_inset, 0, 8) && bounded(m.gif_min_height, 48, 128) &&
           bounded(m.emoji_min_height, 32, 80) && bounded(m.gif_aspect_ratio, .35f, 1) &&
           bounded(m.icon_extent, 16, 28) && bounded(m.text_inset, 2, 8) && bounded(m.rail_inset, 2, 8) &&
           bounded(style.panel_radius, 0, 24) && bounded(style.control_radius, 0, 24) &&
           bounded(style.edit_radius, 0, 24) && bounded(style.selection_width, .5f, 3) &&
           bounded(style.category_opacity, 0, 1) && m.search_height >= m.icon_extent + m.text_inset * 2 &&
           m.footer_height >= m.icon_extent * 2 + m.text_inset * 2 &&
           m.rail_width >= m.icon_extent + m.rail_inset * 2 &&
           m.rail_pitch >= m.icon_extent + m.rail_inset * 2;
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
    if (!scene->storage.arena.base &&
        !ui_fixed_widget_storage_initialize(&scene->storage, kUiMediaPickerWidgetCapacity,
                                            sizeof(UiMediaPickerProjection),
                                            alignof(UiMediaPickerProjection)))
        return false;
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
    scene->pressed = {};
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
}
void ui_media_picker_scene_release(UiMediaPickerScene* scene) noexcept {
    if (!scene)
        return;
    ui_media_picker_scene_close(scene);
    ui_fixed_widget_storage_release(&scene->storage);
    *scene = {};
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
    const float maximum_scroll = rail ? maximum(0.f, (scene->source ? scene->source->sections.count : 0) *
                                                             scene->style.metrics.rail_pitch -
                                                         ui_rect_height(scene->rail))
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
    float offset = scene->style.metrics.content_inset;
    for (u32 i = 0; i < scene->source->sections.count; ++i) {
        if (scene->source->sections[i].id == id) {
            scene->scroll_dips = minimum(offset - scene->style.metrics.content_inset,
                                         maximum(0.f, scene->content_height - ui_rect_height(scene->body)));
            return safe_rebuild(*scene);
        }
        offset += section_extent(*scene, scene->source->sections[i]);
    }
    return false;
}
bool ui_media_picker_scene_select(UiMediaPickerScene* scene, u32 index) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene))
        return false;
    float top = 0;
    UiMediaPickerItem item{};
    if (!item_position(*scene, index, &top) ||
        !scene->source->resolve(scene->source->context, index, &item) || !item_valid(item) || !item.enabled)
        return false;
    return select_index(*scene, index, top);
}
bool ui_media_picker_scene_move_selection(UiMediaPickerScene* scene, i32 horizontal,
                                          i32 vertical) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || !scene->source || !scene->source->count ||
        horizontal < -1 || horizontal > 1 || vertical < -1 || vertical > 1 || (!horizontal && !vertical))
        return false;
    i64 index = scene->selected_index == ~u32{0}
                    ? 0
                    : static_cast<i64>(scene->selected_index) + horizontal + vertical * scene->columns;
    index = maximum(i64{0}, minimum(index, static_cast<i64>(scene->source->count - 1)));
    return select_available(
        *scene, static_cast<u32>(index),
        scene->selected_index == ~u32{0} || horizontal + vertical * scene->columns > 0 ? 1 : -1);
}
bool ui_media_picker_scene_page_selection(UiMediaPickerScene* scene, bool reverse) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || !scene->source || !scene->source->count)
        return false;
    const u32 rows = maximum(1u, static_cast<u32>(ui_rect_height(scene->body) / scene->cell_pitch));
    const u32 stride = rows * scene->columns;
    const u32 index = scene->selected_index == ~u32{0} ? 0 : scene->selected_index;
    return select_available(*scene,
                            reverse ? index - minimum(index, stride)
                                    : minimum(scene->source->count - 1, index + stride),
                            scene->selected_index == ~u32{0} || !reverse ? 1 : -1);
}
bool ui_media_picker_scene_edge_selection(UiMediaPickerScene* scene, bool last) noexcept {
    return scene && ui_media_picker_scene_current(*scene) && scene->source && scene->source->count &&
           select_available(*scene, last ? scene->source->count - 1 : 0, last ? -1 : 1);
}
UiMediaPickerTarget ui_media_picker_scene_selected_target(const UiMediaPickerScene& scene) noexcept {
    for (u32 i = 0; i < scene.cell_count; ++i)
        if (scene.projection->cells[i].source_index == scene.selected_index)
            return target(scene, scene.projection->cells[i].widget);
    return {};
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
    const float inset = maximum(4.f, m.gap);
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
    if (section.visual != UiMediaPickerSectionVisual::guild || !section.guild_id || !section.icon_variant)
        return false;
    const float inset = scene.style.metrics.padding * .5f;
    const UiRect rectangle{slot.rectangle.left + inset, slot.rectangle.top + inset,
                           slot.rectangle.right - inset, slot.rectangle.bottom - inset};
    *output = {target(scene, slot.widget),
               {&scene.widgets, slot.widget},
               section.guild_id,
               section.icon_variant,
               section.icon_animated,
               rectangle,
               ui_rect_intersection(rectangle, scene.rail),
               scene.style.control_radius};
    return ui_rect_has_area(output->clip);
}
bool ui_media_picker_scene_media(const UiMediaPickerScene& scene, u32 index,
                                 UiMediaPickerMediaSlot* output) noexcept {
    const auto& m = scene.style.metrics;
    if (!output || !ui_media_picker_scene_current(scene) || index >= scene.cell_count)
        return false;
    const auto& cell = scene.projection->cells[index];
    if (!cell.item.enabled || (!cell.item.asset_id && !cell.item.regional_flag))
        return false;
    UiRect rectangle = cell.rectangle;
    const bool contain =
        cell.item.kind == UiMediaPickerItemKind::sticker || cell.item.kind == UiMediaPickerItemKind::emoji;
    const float inset = cell.item.kind == UiMediaPickerItemKind::emoji ? m.emoji_inset : m.sticker_inset;
    if (contain)
        rectangle = {rectangle.left + inset, rectangle.top + inset, rectangle.right - inset,
                     rectangle.bottom - inset};
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
               ui_rect_intersection(rectangle, cell.clip),
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
UiScrollbarGeometry ui_media_picker_scene_scrollbar(const UiMediaPickerScene& scene, bool rail) noexcept {
    if (!ui_media_picker_scene_current(scene) || scene.content->phase != UiMediaPickerPhase::ready)
        return {};
    return rail ? ui_scrollbar_geometry(scene.rail, scene.rail_scroll_dips,
                                        (scene.source ? scene.source->sections.count : 0) *
                                            scene.style.metrics.rail_pitch)
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
Utf8View ui_media_picker_scene_tooltip(const UiMediaPickerScene& scene, u64 now_ms, u64 delay_ms) noexcept {
    if (!ui_media_picker_scene_current(scene) || !scene.widgets.hovered.generation ||
        now_ms < scene.widgets.hover_started_ms || now_ms - scene.widgets.hover_started_ms < delay_ms)
        return {};
    const auto hit = target(scene, scene.widgets.hovered);
    if (scene.favorite_hovered)
        for (u32 i = 0; i < scene.cell_count; ++i) {
            const auto& cell = scene.projection->cells[i];
            if (cell.item.id == hit.item_id) {
                if (cell.item.favorite_pending)
                    return utf8_literal("Updating favorite");
                if (cell.item.favorite_failed)
                    return utf8_literal("Couldn't update favorite. Try again");
                return cell.item.favorite ? utf8_literal("Remove from favorites")
                                          : utf8_literal("Add to favorites");
            }
        }
    if (hit.action == UiMediaPickerAction::section_jump || hit.action == UiMediaPickerAction::section)
        for (u32 i = 0; i < scene.source->sections.count; ++i)
            if (scene.source->sections[i].id == hit.item_id)
                return scene.source->sections[i].label;
    for (u32 i = 0; i < scene.cell_count; ++i)
        if (scene.projection->cells[i].item.id == hit.item_id)
            return scene.projection->cells[i].item.label;
    return {};
}
} // namespace maicord
