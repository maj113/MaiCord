#include "maicord/ui/tooltip.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool valid_style(const UiTooltipStyle& style) noexcept {
    return style.pointer_delay_ms <= 5000 && style.keyboard_delay_ms <= 5000 &&
           ui_coordinate_valid(style.padding_x) && style.padding_x >= 0 && style.padding_x <= 32 &&
           ui_coordinate_valid(style.padding_y) && style.padding_y >= 0 && style.padding_y <= 32 &&
           ui_coordinate_valid(style.margin) && style.margin >= 0 && style.margin <= 32 &&
           ui_coordinate_valid(style.gap) && style.gap >= style.arrow_size && style.gap <= 32 &&
           ui_coordinate_valid(style.maximum_width) && style.maximum_width >= 32 &&
           style.maximum_width <= 512 && ui_coordinate_valid(style.maximum_height) &&
           style.maximum_height >= 16 && style.maximum_height <= 256 && ui_coordinate_valid(style.radius) &&
           style.radius >= 0 && style.radius <= 16 && ui_coordinate_valid(style.arrow_size) &&
           style.arrow_size >= 0 && style.arrow_size <= 12 &&
           style.maximum_width > style.padding_x * 2 + 12 && style.maximum_height > style.padding_y * 2;
}
bool source_current(const UiTooltipKey& key) noexcept {
    if (!key.owner.manager || !key.scope || !key.identity || !key.action)
        return false;
    const auto* widget = ui_widget_manager_resolve(*key.owner.manager, key.owner.handle);
    const auto* node = ui_widget_manager_layout_node(*key.owner.manager, key.owner.handle);
    if (!widget || !node || !node->visible || (widget->flags & ui_widget_block_input) ||
        !ui_rect_has_area(ui_rect_intersection(node->rectangle, node->clip_rectangle)))
        return false;
    if (widget->kind == UiWidgetKind::button || (widget->flags & ui_widget_hit_test))
        return ui_widget_manager_input_enabled(*key.owner.manager, key.owner.handle);
    // Text/virtual controls may use a noninteractive container as their text
    // owner. Its visible ancestry still blocks tooltip emission when hidden.
    auto handle = key.owner.handle;
    for (u32 depth = 0; depth < kMaximumUiLayoutDepth; ++depth) {
        const auto* ancestor = ui_widget_manager_resolve(*key.owner.manager, handle);
        const auto* layout = ui_widget_manager_layout_node(*key.owner.manager, handle);
        if (!ancestor || !layout || !layout->visible || (ancestor->flags & ui_widget_block_input))
            return false;
        if (!ancestor->parent.generation)
            return true;
        handle = ancestor->parent;
    }
    return false;
}
bool valid_label(Utf8View label) noexcept {
    if (!label.data || !label.bytes || label.bytes > kUiTooltipMaximumTextBytes ||
        !utf8_validate(label).valid)
        return false;
    for (u32 i = 0; i < label.bytes; ++i)
        if (static_cast<u8>(label.data[i]) < 0x20 && label.data[i] != '\n')
            return false;
    return true;
}
UiRect placed(const UiTooltipScene& scene, UiTooltipPlacement placement) noexcept {
    const float width = scene.text_size.width + scene.style.padding_x * 2,
                height = scene.text_size.height + scene.style.padding_y * 2;
    const float gap = scene.style.gap;
    const float x = (scene.anchor.left + scene.anchor.right) * .5f;
    const float y = (scene.anchor.top + scene.anchor.bottom) * .5f;
    switch (placement) {
    case UiTooltipPlacement::above:
        return {x - width * .5f, scene.anchor.top - gap - height, x + width * .5f, scene.anchor.top - gap};
    case UiTooltipPlacement::below:
        return {x - width * .5f, scene.anchor.bottom + gap, x + width * .5f,
                scene.anchor.bottom + gap + height};
    case UiTooltipPlacement::left:
        return {scene.anchor.left - gap - width, y - height * .5f, scene.anchor.left - gap,
                y + height * .5f};
    case UiTooltipPlacement::right:
        return {scene.anchor.right + gap, y - height * .5f, scene.anchor.right + gap + width,
                y + height * .5f};
    }
    return {};
}
bool fits(UiRect rectangle, UiRect viewport, float margin) noexcept {
    return rectangle.left >= viewport.left + margin && rectangle.right <= viewport.right - margin &&
           rectangle.top >= viewport.top + margin && rectangle.bottom <= viewport.bottom - margin;
}
void place(UiTooltipScene* scene) noexcept {
    const float margin = scene->style.margin;
    auto direction = scene->preferred;
    auto rectangle = placed(*scene, direction);
    if (!fits(rectangle, scene->viewport, margin)) {
        direction = direction == UiTooltipPlacement::above   ? UiTooltipPlacement::below
                    : direction == UiTooltipPlacement::below ? UiTooltipPlacement::above
                    : direction == UiTooltipPlacement::left  ? UiTooltipPlacement::right
                                                             : UiTooltipPlacement::left;
        rectangle = placed(*scene, direction);
    }
    const float width = ui_rect_width(rectangle), height = ui_rect_height(rectangle);
    const float left = maximum(scene->viewport.left + margin,
                               minimum(rectangle.left, scene->viewport.right - margin - width));
    const float top = maximum(scene->viewport.top + margin,
                              minimum(rectangle.top, scene->viewport.bottom - margin - height));
    scene->rectangle = {left, top, left + width, top + height};
    scene->placement = direction;
}
bool arrow(const UiTooltipScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    const auto r = scene.rectangle;
    const bool vertical =
        scene.placement == UiTooltipPlacement::above || scene.placement == UiTooltipPlacement::below;
    const float middle =
        vertical
            ? maximum(r.left + 12, minimum((scene.anchor.left + scene.anchor.right) * .5f, r.right - 12))
            : maximum(r.top + 12, minimum((scene.anchor.top + scene.anchor.bottom) * .5f, r.bottom - 12));
    // At most twelve bounded strips form the small caret for every placement,
    // drawn as ordinary quads so the viewport clip applies.
    const u32 strips = static_cast<u32>(scene.style.arrow_size);
    for (u32 i = 0; i < strips; ++i) {
        const float distance = static_cast<float>(i), half = scene.style.arrow_size - distance;
        UiRect row{};
        if (scene.placement == UiTooltipPlacement::above)
            row = {middle - half, r.bottom + distance, middle + half, r.bottom + distance + 1};
        else if (scene.placement == UiTooltipPlacement::below)
            row = {middle - half, r.top - distance - 1, middle + half, r.top - distance};
        else if (scene.placement == UiTooltipPlacement::left)
            row = {r.right + distance, middle - half, r.right + distance + 1, middle + half};
        else
            row = {r.left - distance - 1, middle - half, r.left - distance, middle + half};
        if (!ui_surface_paint_material(paint, row, theme, UiElevation::tooltip, 0,
                                       ui_surface_style(theme, UiElevation::tooltip).fill, {}, 0))
            return false;
    }
    return true;
}
} // namespace

bool ui_tooltip_key_equal(const UiTooltipKey& a, const UiTooltipKey& b) noexcept {
    return a.owner.manager == b.owner.manager && a.owner.handle.slot == b.owner.handle.slot &&
           a.owner.handle.generation == b.owner.handle.generation && a.scope == b.scope &&
           a.identity == b.identity && a.source_revision == b.source_revision && a.action == b.action;
}
bool ui_tooltip_clear(UiTooltipScene* scene) noexcept {
    if (!scene)
        return false;
    const bool changed = scene->pending || scene->visible;
    const u32 revision = scene->revision + (scene->visible ? 1u : 0u);
    const auto text_owner = scene->text_owner;
    *scene = {};
    scene->revision = revision;
    scene->text_owner = text_owner;
    return changed;
}
bool ui_tooltip_offer(UiTooltipScene* scene, const UiTooltipTarget& target, UiRect viewport,
                      UiTooltipTrigger trigger, u64 now_ms, const UiTooltipStyle& style) noexcept {
    if (!scene)
        return false;
    // Reject raw descriptor coordinates before intersection can clamp them.
    if (!ui_rect_valid(target.anchor))
        return ui_tooltip_clear(scene);
    const auto anchor = ui_rect_intersection(target.anchor, viewport);
    if (!source_current(target.key) || !valid_label(target.label) || !valid_style(style) ||
        !ui_rect_valid(viewport) || ui_rect_width(viewport) < 64 || ui_rect_height(viewport) < 48 ||
        !ui_rect_has_area(anchor) ||
        ui_rect_width(viewport) < style.margin * 2 + style.padding_x * 2 + 12 ||
        ui_rect_height(viewport) <= style.margin * 2 + style.padding_y * 2 ||
        target.placement > UiTooltipPlacement::right || trigger > UiTooltipTrigger::keyboard)
        return ui_tooltip_clear(scene);
    const Utf8View current{scene->text, scene->text_bytes};
    const bool same = (scene->pending || scene->visible) && ui_tooltip_key_equal(scene->key, target.key) &&
                      scene->trigger == trigger && utf8_equal(current, target.label);
    if (same) {
        const bool changed_style = !bytes_equal(&scene->style, &style, sizeof(style));
        if (ui_rect_equal(scene->anchor, anchor) && ui_rect_equal(scene->viewport, viewport) &&
            scene->preferred == target.placement)
            if (!changed_style)
                return false;
        scene->anchor = anchor;
        scene->viewport = viewport;
        scene->preferred = target.placement;
        scene->style = style;
        if (changed_style) {
            if (scene->visible) {
                scene->visible = false;
                ++scene->revision;
                scene->show_at_ms = maximum<u64>(1, now_ms);
            } else
                scene->show_at_ms =
                    deadline_after(now_ms, trigger == UiTooltipTrigger::pointer ? style.pointer_delay_ms
                                                                                : style.keyboard_delay_ms);
            scene->pending = true;
            return true;
        }
        if (scene->visible) {
            place(scene);
            ++scene->revision;
        }
        return true;
    }
    // Copy first so a caller can re-offer the scene's current label with
    // a new semantic identity. No borrowed descriptor survives this call.
    char copied[kUiTooltipMaximumTextBytes];
    const UiTooltipStyle offered_style = style;
    copy_bytes(copied, target.label.data, target.label.bytes);
    ui_tooltip_clear(scene);
    scene->key = target.key;
    scene->anchor = anchor;
    scene->viewport = viewport;
    scene->text_bytes = static_cast<u16>(target.label.bytes);
    copy_bytes(scene->text, copied, target.label.bytes);
    scene->trigger = trigger;
    scene->preferred = target.placement;
    scene->style = offered_style;
    scene->show_at_ms = maximum<u64>(1, deadline_after(now_ms, trigger == UiTooltipTrigger::pointer
                                                                   ? offered_style.pointer_delay_ms
                                                                   : offered_style.keyboard_delay_ms));
    scene->pending = true;
    return true;
}
bool ui_tooltip_bind_text_owner(UiTooltipScene* scene, UiWidgetReference owner) noexcept {
    if (!scene || !owner.manager || !ui_widget_manager_resolve(*owner.manager, owner.handle))
        return false;
    scene->text_owner = owner;
    return true;
}
bool ui_tooltip_tick(UiTooltipScene* scene, u64 now_ms, UiTextMeasureCache* measure) noexcept {
    if (!scene || (!scene->pending && !scene->visible))
        return false;
    if (!source_current(scene->key))
        return ui_tooltip_clear(scene);
    if (!scene->pending || !deadline_due(now_ms, scene->show_at_ms))
        return false;
    const float width =
        minimum(scene->style.maximum_width, ui_rect_width(scene->viewport) - scene->style.margin * 2) -
        scene->style.padding_x * 2;
    const float height =
        minimum(scene->style.maximum_height, ui_rect_height(scene->viewport) - scene->style.margin * 2) -
        scene->style.padding_y * 2;
    const Utf8View label{scene->text, scene->text_bytes};
    const TextStyleSpan style{0, label.bytes, text_style_subtext};
    UiTextMeasureValue value{};
    const UiTextMeasureRequest request{
        {0x544f4f4c544950ull, hash64_bytes(kHash64Seed, label.data, label.bytes), 0, 0, 0},
        label,
        {&style, 1},
        {width, height},
        false};
    if (!measure || !ui_text_measure_cache_measure(measure, request, &value).ok() ||
        !ui_coordinate_valid(value.size.width) || !ui_coordinate_valid(value.size.height) ||
        value.size.width <= 0 || value.size.height <= 0)
        return ui_tooltip_clear(scene);
    scene->text_size = {maximum(12.f, minimum(value.size.width, width)),
                        minimum(value.size.height, height)};
    place(scene);
    scene->pending = false;
    scene->visible = true;
    scene->show_at_ms = 0;
    ++scene->revision;
    return true;
}
u64 ui_tooltip_next_wake(const UiTooltipScene& scene) noexcept {
    return scene.pending ? scene.show_at_ms : 0;
}
bool ui_tooltip_visible(const UiTooltipScene& scene) noexcept {
    if (!scene.visible || !source_current(scene.key) || !scene.text_owner.manager)
        return false;
    const auto* node = ui_widget_manager_layout_node(*scene.text_owner.manager, scene.text_owner.handle);
    return node && node->visible && node->rectangle.left <= scene.rectangle.left &&
           node->rectangle.top <= scene.rectangle.top && node->rectangle.right >= scene.rectangle.right &&
           node->rectangle.bottom >= scene.rectangle.bottom &&
           node->clip_rectangle.left <= scene.rectangle.left &&
           node->clip_rectangle.top <= scene.rectangle.top &&
           node->clip_rectangle.right >= scene.rectangle.right &&
           node->clip_rectangle.bottom >= scene.rectangle.bottom;
}
bool ui_tooltip_paint(const UiTooltipScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    if (!ui_tooltip_visible(scene))
        return true;
    if (!paint || !paint_stream_push_clip(paint, scene.viewport.left, scene.viewport.top,
                                          scene.viewport.right, scene.viewport.bottom))
        return false;
    const auto r = scene.rectangle;
    if (!paint_stream_record_content(paint, scene.text_owner, r, UiLayoutContentKind::decoration,
                                     ui_layout_content_bounds_allow_overlap)) {
        paint_stream_pop_clip(paint);
        return false;
    }
    const bool painted = paint_stream_push_rounded(paint, r.left + 1, r.top + 2, r.right + 1, r.bottom + 2,
                                                   theme.tokens.overlay.shadow, scene.style.radius) &&
                         arrow(scene, theme, paint) &&
                         ui_surface_paint(paint, r, theme, UiElevation::tooltip, scene.style.radius);
    return paint_stream_pop_clip(paint) && painted;
}
bool ui_tooltip_text_output(const UiTooltipScene& scene, const Theme& theme,
                            UiTextOutput* output) noexcept {
    if (!ui_tooltip_visible(scene))
        return true;
    const auto r = scene.rectangle;
    return ui_text_output_push(output, {{scene.text, scene.text_bytes},
                                        scene.text_owner,
                                        {r.left + scene.style.padding_x, r.top + scene.style.padding_y,
                                         r.right - scene.style.padding_x, r.bottom - scene.style.padding_y},
                                        scene.viewport,
                                        theme.text_primary,
                                        text_style_subtext,
                                        UiTextAlignment::center,
                                        UiTextAlignment::center,
                                        UiTextWrapping::wrap,
                                        UiTextOverflow::ellipsis});
}
} // namespace maicord
