#include "maicord/ui/tooltip.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/asset.hpp"

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
           style.arrow_size >= 0 && style.arrow_size <= 12 && ui_coordinate_valid(style.emoji_size) &&
           style.emoji_size >= 0 && style.emoji_size <= 64 && ui_coordinate_valid(style.emoji_gap) &&
           style.emoji_gap >= 0 && style.emoji_gap <= 32 &&
           style.maximum_width > style.padding_x * 2 + 12 && style.maximum_height > style.padding_y * 2;
}
bool source_current(const UiTooltipKey& key) noexcept {
    if (!key.owner.manager || !key.scope || !key.identity || !key.action)
        return false;
    const auto* widget = ui_widget_manager_resolve(*key.owner.manager, key.owner.handle);
    const auto* node = ui_widget_manager_layout_node(*key.owner.manager, key.owner.handle);
    if (!widget || !node || !node->visible || (widget->flags & ui_widget_block_input) ||
        !ui_rect_intersects(node->rectangle, node->clip_rectangle))
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
        handle = ui_widget_manager_parent(*key.owner.manager, handle);
        if (!handle.generation)
            return true;
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
// Bold ranges lie inside the label, ascending and disjoint.
bool valid_bold(ConstSlice<TextStyleSpan> bold, u32 bytes) noexcept {
    if (bold.count > kUiTooltipMaximumBold || (bold.count && !bold.data))
        return false;
    u32 at = 0;
    for (u32 i = 0; i < bold.count; ++i) {
        if (!bold[i].byte_count || bold[i].byte_offset < at || bold[i].byte_offset > bytes ||
            bold[i].byte_count > bytes - bold[i].byte_offset)
            return false;
        at = bold[i].byte_offset + bold[i].byte_count;
    }
    return true;
}
bool valid_emoji(const UiTooltipEmoji& emoji) noexcept {
    return emoji.icon <= UiIcon::count &&
           (!emoji.glyph.bytes || (emoji.glyph.data && emoji.glyph.bytes <= kUiTooltipMaximumEmojiBytes &&
                                   utf8_validate(emoji.glyph).valid));
}
bool valid_detail(Utf8View detail) noexcept {
    return !detail.bytes ||
           (detail.data && detail.bytes <= kUiTooltipMaximumDetailBytes && valid_label(detail));
}
u8 icon_slot(UiIcon icon) noexcept {
    return icon < UiIcon::count ? static_cast<u8>(static_cast<u32>(icon) + 1) : u8{0};
}
bool has_emoji(const UiTooltipScene& scene) noexcept {
    return media_asset_key_valid(scene.emoji_media) || scene.emoji_glyph_bytes || scene.emoji_icon;
}
// An icon with a detail line leads the label on its own line instead of
// standing in a column beside both lines.
bool inline_icon(const UiTooltipScene& scene) noexcept {
    return scene.emoji_icon && scene.detail_bytes && !media_asset_key_valid(scene.emoji_media) &&
           !scene.emoji_glyph_bytes;
}
// The emoji column before the label: the square and its gap.
float leading(const UiTooltipScene& scene) noexcept {
    return has_emoji(scene) && !inline_icon(scene) ? scene.style.emoji_size + scene.style.emoji_gap : 0;
}
// Plain and bold runs covering the whole label.
u8 label_runs(u32 bytes, ConstSlice<TextStyleSpan> bold, TextStyleSpan* runs) noexcept {
    u8 count = 0;
    u32 at = 0;
    for (u32 i = 0; i < bold.count; ++i) {
        if (bold[i].byte_offset > at)
            runs[count++] = {at, bold[i].byte_offset - at, text_style_subtext};
        runs[count++] = {bold[i].byte_offset, bold[i].byte_count, text_style_subtext | text_style_bold};
        at = bold[i].byte_offset + bold[i].byte_count;
    }
    if (at < bytes || !count)
        runs[count++] = {at, bytes - at, text_style_subtext};
    return count;
}
bool same_content(const UiTooltipScene& scene, const UiTooltipTarget& target) noexcept {
    TextStyleSpan runs[kUiTooltipMaximumBold * 2 + 1]{};
    const u8 count = label_runs(target.label.bytes, target.bold, runs);
    return utf8_equal({scene.text, scene.text_bytes}, target.label) && count == scene.style_count &&
           bytes_equal(runs, scene.styles, count * static_cast<u32>(sizeof(TextStyleSpan))) &&
           media_asset_key_equal(scene.emoji_media, target.emoji.media) &&
           utf8_equal({scene.emoji_glyph, scene.emoji_glyph_bytes}, target.emoji.glyph) &&
           scene.emoji_icon == icon_slot(target.emoji.icon) &&
           utf8_equal({scene.detail, scene.detail_bytes}, target.detail);
}
// Copies the target's label, runs and emoji; the target may borrow the
// scene's own text.
void set_content(UiTooltipScene* scene, const UiTooltipTarget& target) noexcept {
    char label[kUiTooltipMaximumTextBytes];
    char glyph[kUiTooltipMaximumEmojiBytes];
    TextStyleSpan runs[kUiTooltipMaximumBold * 2 + 1]{};
    copy_bytes(label, target.label.data, target.label.bytes);
    copy_bytes(glyph, target.emoji.glyph.data, target.emoji.glyph.bytes);
    const u8 count = label_runs(target.label.bytes, target.bold, runs);
    copy_bytes(scene->text, label, target.label.bytes);
    scene->text_bytes = static_cast<u16>(target.label.bytes);
    copy_bytes(scene->styles, runs, sizeof(runs));
    scene->style_count = count;
    scene->emoji_media = target.emoji.media;
    copy_bytes(scene->emoji_glyph, glyph, target.emoji.glyph.bytes);
    scene->emoji_glyph_bytes = static_cast<u8>(target.emoji.glyph.bytes);
    scene->emoji_icon = icon_slot(target.emoji.icon);
    char detail[kUiTooltipMaximumDetailBytes];
    copy_bytes(detail, target.detail.data, target.detail.bytes);
    copy_bytes(scene->detail, detail, target.detail.bytes);
    scene->detail_bytes = static_cast<u8>(target.detail.bytes);
}
// The top of the label line: the text block centres in the box.
float text_top(const UiTooltipScene& scene) noexcept {
    const auto r = scene.rectangle;
    const float inner = ui_rect_height(r) - scene.style.padding_y * 2;
    return r.top + scene.style.padding_y + (inner - scene.text_size.height) * .5f;
}
// The label line's left edge when an icon leads it, centred in the box.
float inline_left(const UiTooltipScene& scene) noexcept {
    const auto r = scene.rectangle;
    const float line = scene.style.emoji_size + scene.style.emoji_gap + scene.label_size.width;
    return (r.left + r.right - line) * .5f;
}
UiRect emoji_square(const UiTooltipScene& scene) noexcept {
    const auto r = scene.rectangle;
    const float size = scene.style.emoji_size;
    if (inline_icon(scene)) {
        const float left = inline_left(scene),
                    top = text_top(scene) + (scene.label_size.height - size) * .5f;
        return {left, top, left + size, top + size};
    }
    const float left = r.left + scene.style.padding_x, top = (r.top + r.bottom - size) * .5f;
    return {left, top, left + size, top + size};
}
UiRect placed(const UiTooltipScene& scene, UiTooltipPlacement placement) noexcept {
    const float width = scene.text_size.width + scene.style.padding_x * 2 + leading(scene),
                height = maximum(scene.text_size.height,
                                 has_emoji(scene) && !inline_icon(scene) ? scene.style.emoji_size : 0.f) +
                         scene.style.padding_y * 2;
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
    return a.owner.manager == b.owner.manager && ui_widget_handle_equal(a.owner.handle, b.owner.handle) &&
           a.scope == b.scope &&
           a.identity == b.identity && a.source_revision == b.source_revision && a.action == b.action;
}
bool ui_tooltip_clear(UiTooltipScene* scene) noexcept {
    if (!scene)
        return false;
    const bool changed = scene->pending || scene->visible;
    const u32 revision = scene->revision + (scene->visible ? 1u : 0u);
    const auto text_owner = scene->text_owner;
    reset_value(scene);
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
        !valid_bold(target.bold, target.label.bytes) || !valid_emoji(target.emoji) || !valid_detail(target.detail) ||
        !ui_rect_valid(viewport) || ui_rect_width(viewport) < 64 || ui_rect_height(viewport) < 48 ||
        !ui_rect_has_area(anchor) ||
        ui_rect_width(viewport) < style.margin * 2 + style.padding_x * 2 + 12 ||
        ui_rect_height(viewport) <= style.margin * 2 + style.padding_y * 2 ||
        target.placement > UiTooltipPlacement::right || trigger > UiTooltipTrigger::keyboard)
        return ui_tooltip_clear(scene);
    const bool same = (scene->pending || scene->visible) && ui_tooltip_key_equal(scene->key, target.key) &&
                      scene->trigger == trigger;
    if (same && !same_content(*scene, target)) {
        // The same source with new content (reactors that finished loading):
        // a shown tooltip is re-measured at the next tick, a pending one keeps
        // its deadline.
        set_content(scene, target);
        scene->anchor = anchor;
        scene->viewport = viewport;
        scene->preferred = target.placement;
        scene->style = style;
        if (scene->visible)
            scene->show_at_ms = maximum<u64>(1, now_ms);
        scene->pending = true;
        return true;
    }
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
    UiTooltipScene copied{};
    const UiTooltipStyle offered_style = style;
    set_content(&copied, target);
    ui_tooltip_clear(scene);
    scene->key = target.key;
    scene->anchor = anchor;
    scene->viewport = viewport;
    scene->text_bytes = copied.text_bytes;
    copy_bytes(scene->text, copied.text, copied.text_bytes);
    copy_bytes(scene->styles, copied.styles, sizeof(copied.styles));
    scene->style_count = copied.style_count;
    scene->emoji_media = copied.emoji_media;
    copy_bytes(scene->emoji_glyph, copied.emoji_glyph, copied.emoji_glyph_bytes);
    scene->emoji_glyph_bytes = copied.emoji_glyph_bytes;
    scene->emoji_icon = copied.emoji_icon;
    copy_bytes(scene->detail, copied.detail, copied.detail_bytes);
    scene->detail_bytes = copied.detail_bytes;
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
        scene->style.padding_x * 2 - leading(*scene);
    const float height =
        minimum(scene->style.maximum_height, ui_rect_height(scene->viewport) - scene->style.margin * 2) -
        scene->style.padding_y * 2;
    const bool detail = scene->detail_bytes != 0;
    // An icon leading the label on its line narrows that line.
    const float label_width = width - (inline_icon(*scene) ? scene->style.emoji_size + scene->style.emoji_gap : 0);
    const Utf8View label{scene->text, scene->text_bytes};
    UiTextMeasureValue value{};
    const UiTextMeasureRequest request{
        {0x544f4f4c544950ull,
         hash64_bytes(hash64_bytes(kHash64Seed, label.data, label.bytes), scene->styles,
                      scene->style_count * static_cast<u32>(sizeof(TextStyleSpan))),
         0, static_cast<u16>(detail ? 1 : 0), 0},
        label,
        {scene->styles, scene->style_count},
        {label_width, height},
        detail};
    if (!(label_width >= 12) || !measure || !ui_text_measure_cache_measure(measure, request, &value).ok() ||
        !ui_coordinate_valid(value.size.width) || !ui_coordinate_valid(value.size.height) ||
        value.size.width <= 0 || value.size.height <= 0)
        return ui_tooltip_clear(scene);
    scene->label_size = {maximum(12.f, minimum(value.size.width, label_width)), minimum(value.size.height, height)};
    scene->text_size = scene->label_size;
    reset_value(&scene->detail_size);
    if (detail) {
        const TextStyleSpan muted{0, scene->detail_bytes, text_style_subtext};
        const UiTextMeasureRequest second{
            {0x544f4f4c444554ull, hash64_bytes(kHash64Seed, scene->detail, scene->detail_bytes), 0, 0, 0},
            {scene->detail, scene->detail_bytes},
            {&muted, 1},
            {width, height},
            true};
        UiTextMeasureValue line{};
        if (!ui_text_measure_cache_measure(measure, second, &line).ok() || !ui_coordinate_valid(line.size.width) ||
            !ui_coordinate_valid(line.size.height) || line.size.width <= 0 || line.size.height <= 0)
            return ui_tooltip_clear(scene);
        scene->detail_size = {minimum(line.size.width, width), line.size.height};
        const float first = scene->label_size.width +
                            (inline_icon(*scene) ? scene->style.emoji_size + scene->style.emoji_gap : 0);
        scene->text_size = {maximum(first, scene->detail_size.width),
                            minimum(scene->label_size.height + scene->detail_size.height, height)};
    }
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
namespace {
bool paint_box(const UiTooltipScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    const auto r = scene.rectangle;
    return paint_stream_push_rounded(paint, r.left + 1, r.top + 2, r.right + 1, r.bottom + 2,
                                     theme.tokens.overlay.shadow, scene.style.radius) &&
           arrow(scene, theme, paint) && ui_surface_paint(paint, r, theme, UiElevation::tooltip, scene.style.radius);
}
} // namespace
bool ui_tooltip_paint_box(PaintStream* paint, const Theme& theme, UiRect rectangle, UiRect anchor,
                          UiTooltipPlacement placement, const UiTooltipStyle& style) noexcept {
    if (!paint || !ui_rect_has_area(rectangle))
        return false;
    UiTooltipScene scene{};
    scene.rectangle = rectangle;
    scene.anchor = anchor;
    scene.placement = placement;
    scene.style = style;
    return paint_box(scene, theme, paint);
}
bool ui_tooltip_paint(const UiTooltipScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    if (!ui_tooltip_visible(scene))
        return true;
    if (!paint || !paint_stream_push_clip(paint, scene.viewport))
        return false;
    // An icon is paint, drawn in the label colour; images and glyphs are not.
    bool painted = paint_box(scene, theme, paint);
    if (painted && scene.emoji_icon && !media_asset_key_valid(scene.emoji_media) && !scene.emoji_glyph_bytes)
        painted = ui_icon_paint(paint, static_cast<UiIcon>(scene.emoji_icon - 1), emoji_square(scene),
                                theme.text_primary);
    return paint_stream_pop_clip(paint) && painted;
}
bool ui_tooltip_text_output(const UiTooltipScene& scene, const Theme& theme,
                            UiTextOutput* output) noexcept {
    if (!ui_tooltip_visible(scene))
        return true;
    const auto r = scene.rectangle;
    // A Unicode emoji is jumbo text in its square; images are the host's.
    if (scene.emoji_glyph_bytes && !media_asset_key_valid(scene.emoji_media) &&
        !ui_text_output_push(output, {{scene.emoji_glyph, scene.emoji_glyph_bytes},
                                      scene.text_owner,
                                      emoji_square(scene),
                                      scene.viewport,
                                      theme.text_primary,
                                      text_style_jumbo_emoji,
                                      UiTextAlignment::center,
                                      UiTextAlignment::center,
                                      TextWrapping::no_wrap,
                                      TextOverflow::clip}))
        return false;
    const float lead = leading(scene);
    // Plain labels keep the single subtext style; bold runs travel with the text.
    const bool runs = scene.style_count > 1 || (scene.style_count == 1 && (scene.styles[0].flags & text_style_bold));
    const ConstSlice<TextStyleSpan> styles =
        runs ? ConstSlice<TextStyleSpan>{scene.styles, scene.style_count} : ConstSlice<TextStyleSpan>{};
    const float left = r.left + scene.style.padding_x + lead, right = r.right - scene.style.padding_x;
    if (!scene.detail_bytes)
        return ui_text_output_push(output, {{scene.text, scene.text_bytes},
                                            scene.text_owner,
                                            {left, r.top + scene.style.padding_y, right,
                                             r.bottom - scene.style.padding_y},
                                            scene.viewport,
                                            theme.text_primary,
                                            text_style_subtext,
                                            lead > 0 ? UiTextAlignment::start : UiTextAlignment::center,
                                            UiTextAlignment::center,
                                            TextWrapping::wrap,
                                            TextOverflow::ellipsis,
                                            UiTextFace::normal,
                                            {},
                                            styles});
    // Two lines: the label on one line, the muted detail below it. An icon
    // leads the label and both lines centre; beside an emoji they start.
    const float top = text_top(scene), middle = top + scene.label_size.height;
    const bool inline_lead = inline_icon(scene);
    const float label_left =
        inline_lead ? inline_left(scene) + scene.style.emoji_size + scene.style.emoji_gap : left;
    const float label_right = inline_lead ? minimum(right, label_left + scene.label_size.width + 1) : right;
    const auto align = lead > 0 ? UiTextAlignment::start : UiTextAlignment::center;
    return ui_text_output_push(output, {{scene.text, scene.text_bytes},
                                        scene.text_owner,
                                        {label_left, top, label_right, middle},
                                        scene.viewport,
                                        theme.text_primary,
                                        text_style_subtext,
                                        inline_lead ? UiTextAlignment::start : align,
                                        UiTextAlignment::center,
                                        TextWrapping::no_wrap,
                                        TextOverflow::ellipsis,
                                        UiTextFace::normal,
                                        {},
                                        styles}) &&
           ui_text_output_push(output, {{scene.detail, scene.detail_bytes},
                                        scene.text_owner,
                                        {left, middle, right, middle + scene.detail_size.height},
                                        scene.viewport,
                                        theme.tokens.text.muted,
                                        text_style_subtext,
                                        align,
                                        UiTextAlignment::center,
                                        TextWrapping::no_wrap,
                                        TextOverflow::ellipsis});
}
bool ui_tooltip_emoji(const UiTooltipScene& scene, UiRect* square) noexcept {
    if (!square || !has_emoji(scene) || !ui_tooltip_visible(scene))
        return false;
    *square = emoji_square(scene);
    return true;
}
} // namespace maicord
