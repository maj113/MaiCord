#include "maicord/ui/media/picker.hpp"
#include "picker_internal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/text/emoji_catalog.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/text/regional_flag.hpp"
#include "maicord/sys/simd.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
using namespace ui_media_picker_detail;
bool ui_media_picker_scene_update(UiMediaPickerScene* scene, const UiMediaPickerSource* source,
                                  const UiMediaPickerContent* content, UiRect viewport, UiRect anchor,
                                  float preferred_width) noexcept {
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
        (content->available_tabs & ~7u) || content->variant > UiMediaPickerVariant::inline_results ||
        (content->variant == UiMediaPickerVariant::reaction &&
         (content->kind != UiMediaPickerKind::emoji ||
          content->available_tabs != ui_media_picker_tab_bit(UiMediaPickerKind::emoji))) ||
        // Inline results type into the composer: no search field, tabs or Back.
        (content->variant == UiMediaPickerVariant::inline_results &&
         (content->kind == UiMediaPickerKind::emoji || content->edit.active || content->back ||
          content->available_tabs != ui_media_picker_tab_bit(content->kind) || !label_valid(content->heading) ||
          content->heading_plain > content->heading.bytes)) ||
        (content->variant != UiMediaPickerVariant::inline_results && !content->edit.active) ||
        !label_valid(content->placeholder) ||
        !label_valid(content->status) || !ui_rect_valid(viewport) || !ui_rect_valid(anchor) ||
        (!admitted_source && !source_valid(source, *content)))
        return false;
    const bool inline_variant = content->variant == UiMediaPickerVariant::inline_results;
    const UiMediaPickerStyle effective = content->variant == UiMediaPickerVariant::reaction ? ui_media_picker_reaction_style()
                                         : inline_variant ? ui_media_picker_inline_style()
                                                          : ui_media_picker_style();
    PickerLayout layout{};
    if (inline_variant ? !inline_layout(effective, source, *content, viewport, anchor, &layout)
                       : !picker_layout(effective, source, content->kind, content->variant, content->back, viewport,
                                        anchor, preferred_width, &layout))
        return false;
    const u32 plain = minimum(content->heading_plain, content->heading.bytes);
    scene->heading_span = {plain, content->heading.bytes - plain, text_style_bold, 0, {}};
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
    scene->page_duration_ms = 0;
    ui_widget_manager_reset(&scene->widgets);
    scene->active = false;
    scene->body_scrollbar_state = scene->rail_scrollbar_state = UiScrollbarVisualState::normal;
    scene->favorite_hovered = false;
    scene->cell_count = scene->header_count = scene->rail_count = 0;
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
    if (scene->prior)
        VirtualFree(scene->prior, 0, MEM_RELEASE);
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
    const bool widget_changed = !ui_widget_handle_equal(scene->widgets.hovered, widget);
    if (widget_changed && !ui_widget_manager_hover(&scene->widgets, widget))
        return false;
    bool favorite_hovered = false;
    for (u32 i = 0; i < scene->cell_count; ++i) {
        const auto& cell = scene->projection->cells[i];
        UiRect button{};
        if (ui_widget_handle_equal(cell.widget, widget) &&
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
    if (scene)
        scene->page_duration_ms = 0;
    if (!scene || !ui_media_picker_scene_current(*scene) ||
        scene->content->phase != UiMediaPickerPhase::ready ||
        !bounded(delta, -kMaximumPaintCoordinate, kMaximumPaintCoordinate))
        return false;
    const float maximum_scroll = rail ? maximum(0.f, scene->rail_extent - ui_rect_height(scene->rail))
                                      : maximum(0.f, scene->content_height - body_extent(*scene));
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
bool ui_media_picker_scene_step_selection(UiMediaPickerScene* scene, i32 direction) noexcept {
    if (scene)
        scene->page_duration_ms = 0;
    if (!scene || !ui_media_picker_scene_current(*scene) || !inline_results(*scene) || !scene->source ||
        scene->content->phase != UiMediaPickerPhase::ready || (direction != 1 && direction != -1))
        return false;
    const u32 count = scene->kind == UiMediaPickerKind::sticker
                          ? minimum(scene->source->count, kUiInlineResultsStickerLimit)
                          : scene->source->count;
    if (!count)
        return false;
    const u32 selected = scene->selected_index;
    const u32 index = selected >= count ? (direction > 0 ? 0 : count - 1)
                      : direction > 0   ? (selected + 1 == count ? 0 : selected + 1)
                                        : (selected ? selected - 1 : count - 1);
    float top = 0, height = 0;
    return item_position(*scene, index, &top, &height) && select_index(*scene, index, top, height);
}
bool ui_media_picker_scene_clear_selection(UiMediaPickerScene* scene) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || scene->selected_index == ~u32{0})
        return false;
    scene->selected_index = ~u32{0};
    ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
    ++scene->revision;
    return true;
}
namespace {
// Chromium's smooth scroll curve: cubic-bezier(.4, 0, 0, 1), so x(t) = 1.2(1-t)^2 t + t^3 and
// y(t) = 3t^2 - 2t^3; x is solved by bisection before sampling y.
float page_eased(float progress) noexcept {
    float low = 0, high = 1;
    for (u32 i = 0; i < 16; ++i) {
        const float t = (low + high) * .5f;
        const float x = 1.2f * (1 - t) * (1 - t) * t + t * t * t;
        if (x < progress)
            low = t;
        else
            high = t;
    }
    const float t = (low + high) * .5f;
    return t * t * (3 - 2 * t);
}
} // namespace
bool ui_media_picker_scene_page(UiMediaPickerScene* scene, bool forward, u64 now_ms, float pixels_per_dip,
                                bool reduced_motion) noexcept {
    if (!scene || !ui_media_picker_scene_current(*scene) || !inline_results(*scene) ||
        scene->kind != UiMediaPickerKind::gif || scene->content->phase != UiMediaPickerPhase::ready ||
        !bounded(pixels_per_dip, .25f, 8.f))
        return false;
    const float step = ui_rect_width(scene->body) * .8f;
    const float from = scene->scroll_dips;
    const float to = clamped(from + (forward ? step : -step), 0.f,
                             maximum(0.f, scene->content_height - body_extent(*scene)));
    if (to == from)
        return false;
    // min(sqrt(|delta px|), kMaxAnimationDuration 1.5 s * 60) / 60 seconds.
    const double pixels = static_cast<double>((to > from ? to - from : from - to) * pixels_per_dip);
    const double frames = minimum(_mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(pixels))), 90.0);
    const u32 duration = static_cast<u32>(frames * 1000.0 / 60.0);
    if (reduced_motion || !duration)
        return ui_media_picker_scene_scroll(scene, to - from);
    scene->page_from = from;
    scene->page_to = to;
    scene->page_started_ms = now_ms;
    scene->page_duration_ms = duration;
    scene->page_source_revision = scene->source_revision;
    return true;
}
bool ui_media_picker_scene_page_tick(UiMediaPickerScene* scene, u64 now_ms) noexcept {
    if (!scene || !scene->page_duration_ms)
        return false;
    const u32 duration = scene->page_duration_ms;
    if (!ui_media_picker_scene_current(*scene) || scene->source_revision != scene->page_source_revision) {
        scene->page_duration_ms = 0;
        return false;
    }
    const u64 elapsed = now_ms > scene->page_started_ms ? now_ms - scene->page_started_ms : 0;
    const bool done = elapsed >= duration;
    const float value = done ? scene->page_to
                             : scene->page_from + (scene->page_to - scene->page_from) *
                                                      page_eased(static_cast<float>(elapsed) / duration);
    // Scrolling ends a page animation, so this frame's own step restores it.
    const bool moved = ui_media_picker_scene_scroll(scene, value - scene->scroll_dips);
    scene->page_duration_ms = done ? 0 : duration;
    return moved;
}
bool ui_media_picker_scene_paging(const UiMediaPickerScene& scene) noexcept {
    return scene.page_duration_ms != 0;
}
UiMediaPickerTarget ui_media_picker_scene_selected_target(const UiMediaPickerScene& scene) noexcept {
    for (u32 i = 0; ui_media_picker_scene_current(scene) && i < scene.cell_count; ++i)
        if (scene.projection->cells[i].source_index == scene.selected_index)
            return target(scene, scene.projection->cells[i].widget);
    return {};
}
bool ui_media_picker_scene_spinner_visible(const UiMediaPickerScene& scene) noexcept {
    return ui_media_picker_scene_current(scene) && inline_results(scene) &&
           scene.content->phase == UiMediaPickerPhase::loading;
}
bool ui_media_picker_scene_favorite(const UiMediaPickerScene& scene, u32 index, UiRect* rectangle,
                                    UiRect* full_rectangle) noexcept {
    if (!rectangle || !ui_media_picker_scene_current(scene) || index >= scene.cell_count)
        return false;
    const auto& cell = scene.projection->cells[index];
    if (!cell.item.enabled || !cell.item.favoritable || cell.item.kind != UiMediaPickerItemKind::gif ||
        !ui_widget_manager_input_enabled(scene.widgets, cell.widget))
        return false;
    if (!ui_widget_handle_equal(scene.widgets.hovered, cell.widget) &&
        !ui_widget_handle_equal(scene.widgets.focused, cell.widget) &&
        !ui_widget_handle_equal(scene.widgets.captured, cell.widget))
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
        if (cell.source_index == scene.selected_index && ui_widget_handle_equal(scene.widgets.focused, cell.widget) &&
            !cell.item.favorite_pending &&
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
               // Inline tiles and stickers are square-cornered.
               inline_results(scene) ? 0.f : scene.style.control_radius,
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
        if (ui_widget_handle_equal(scene.widgets.hovered, cell.widget))
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
    // Inline results page with their arrows (GIFs) or fit (stickers): no bar.
    if (!ui_media_picker_scene_current(scene) || scene.content->phase != UiMediaPickerPhase::ready ||
        inline_results(scene))
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
