#include "maicord/ui/poll_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {
bool owner_current(UiWidgetReference owner) noexcept {
    const auto* widget = owner.manager ? ui_widget_manager_resolve(*owner.manager, owner.handle) : nullptr;
    return widget && (widget->flags & ui_widget_enabled) && !(widget->flags & ui_widget_block_input);
}
bool rounded(PaintStream* stream, UiRect bounds, Color color, float radius) noexcept {
    return !ui_rect_has_area(bounds) ||
           paint_stream_push_rounded(stream, bounds.left, bounds.top, bounds.right, bounds.bottom, color,
                                     radius);
}
bool border(PaintStream* stream, UiRect bounds, Color fill, Color line, float radius,
            float width = 1) noexcept {
    return !ui_rect_has_area(bounds) ||
           paint_stream_push_rounded_border(stream, bounds.left, bounds.top, bounds.right, bounds.bottom,
                                            fill, line, radius, width);
}
UiRect inset(UiRect bounds, float value) noexcept {
    return {bounds.left + value, bounds.top + value, bounds.right - value, bounds.bottom - value};
}
UiRect centered(UiRect bounds, float extent) noexcept {
    const float x = (bounds.left + bounds.right) * .5f, y = (bounds.top + bounds.bottom) * .5f;
    return {x - extent * .5f, y - extent * .5f, x + extent * .5f, y + extent * .5f};
}
bool label(UiTextOutput* output, Utf8View text, UiRect bounds, UiRect clip, UiWidgetReference owner,
           Color color, u32 flags, bool wrap = true, UiTextAlignment horizontal = UiTextAlignment::start,
           UiTextAlignment vertical = UiTextAlignment::start) noexcept {
    if (!text.bytes || !ui_rect_has_area(ui_rect_intersection(bounds, clip)))
        return true;
    return ui_text_output_push_owned(output, {text, owner, bounds, clip, color, flags, horizontal, vertical,
                                              wrap ? UiTextWrapping::wrap : UiTextWrapping::no_wrap,
                                              UiTextOverflow::ellipsis});
}
bool closed(const UiPollState& state) noexcept {
    return (state.finalized_known && state.finalized) ||
           (state.expiry_known && state.expiry_ms && state.now_ms >= state.expiry_ms);
}
// Bars, counts and the own-vote check show once voted, closed, or after
// "Show results".
bool counts_visible(const UiPollState& state) noexcept {
    return state.results || closed(state);
}
// The prompt line: a status replaces it; the prompt is hidden when closed.
Utf8View instruction(const UiPollProjection& projection, const UiPollStyle& style) noexcept {
    const auto& state = projection.state;
    return !state.supported ? style.unavailable
           : closed(state)  ? Utf8View{}
           : state.multiselect ? style.multi
                               : style.single;
}
Utf8View status(const UiPollProjection& projection, const UiPollStyle& style) noexcept {
    const auto& state = projection.state;
    return state.status.bytes                      ? state.status
           : state.loading                         ? Utf8View{}
           : state.error                           ? style.failed
           : state.results && !state.results_known ? style.unknown_results
                                                   : Utf8View{};
}
// Radio (single select) or checkbox (multiple), sized as fractions of the
// 24-DIP marker box; a selected checkbox fills with the accent and shows a check.
bool selection(PaintStream* paint, UiRect marker, bool selected, bool multiple, const UiPollProjection& projection,
               Color ring) noexcept {
    const float extent = ui_rect_width(marker);
    if (!multiple) {
        const UiRect circle = centered(marker, extent * (20.f / 24.f));
        if (!border(paint, circle, {}, ring, ui_rect_width(circle) * .5f, extent / 12.f))
            return false;
        if (!selected)
            return true;
        const UiRect dot = centered(marker, extent * (10.f / 24.f));
        return rounded(paint, dot, projection.accent, ui_rect_width(dot) * .5f);
    }
    const UiRect box = centered(marker, extent * (20.f / 24.f));
    const float radius = ui_rect_width(box) * .15f;
    if (!selected)
        return border(paint, box, {}, ring, radius, extent / 12.f);
    return rounded(paint, box, projection.accent, radius) &&
           ui_icon_paint(paint, UiIcon::check, centered(box, ui_rect_width(box) * .85f),
                         projection.primary_foreground);
}
// The own-vote circle check in results: accent while open, victor color on a
// winning answer once closed, muted on the others.
bool own_check(PaintStream* paint, UiRect marker, Color fill, Color ink) noexcept {
    const UiRect circle = centered(marker, ui_rect_width(marker) * (20.f / 24.f));
    return rounded(paint, circle, fill, ui_rect_width(circle) * .5f) &&
           ui_icon_paint(paint, UiIcon::check, centered(circle, ui_rect_width(circle) * .7f), ink);
}
u64 geometry_hash(u64 hash, const UiPollGeometry& geometry) noexcept {
    const UiRect rectangles[]{
        geometry.bounds, geometry.question, geometry.instruction, geometry.status,  geometry.summary,
        geometry.expiry, geometry.vote,     geometry.remove_vote, geometry.results, geometry.retry};
    hash = hash64_bytes(hash, rectangles, sizeof(rectangles));
    for (const auto& answer : geometry.answers) {
        const UiRect parts[]{answer.bounds, answer.marker, answer.emoji, answer.text, answer.result, answer.percent};
        hash = hash64_bytes(hash, parts, sizeof(parts));
    }
    return hash;
}
} // namespace

bool ui_poll_controls_paint(const UiPollProjection& projection, const UiPollStyle& style, UiRect clip,
                            const Theme& theme, UiWidgetReference owner, PaintStream* paint,
                            UiPollTarget focus, UiPollTarget hover, bool keyboard) noexcept {
    if (!paint || !owner_current(owner) || !ui_rect_valid_or_empty(clip))
        return false;
    const auto visible = ui_rect_intersection(clip, projection.geometry.bounds);
    if (!ui_rect_has_area(visible))
        return true;
    if (!paint_stream_push_clip(paint, visible.left, visible.top, visible.right, visible.bottom))
        return false;
    const auto& state = projection.state;
    const bool results = counts_visible(state);
    const bool ended = closed(state);
    const auto draw = [&]() noexcept {
        if (!ui_background_paint_surface(paint, projection.geometry.bounds, theme, projection.surface,
                                         projection.border, style.radius, 1, 1,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high,
                                         UiBackgroundBorderTone::palette) ||
            !paint_stream_record_content(paint, owner, projection.geometry.bounds,
                                         UiLayoutContentKind::control,
                                         ui_layout_content_bounds_allow_overlap))
            return false;
        for (u32 i = 0; i < state.answers.count; ++i) {
            const auto& answer = state.answers[i];
            const auto& row = projection.geometry.answers[i];
            const UiPollTarget target{UiPollControl::answer, answer.id};
            const bool voted = answer.me_known && answer.me_voted;
            const bool victor = projection.victor_answers[i];
            const bool hovered =
                ui_poll_target_equal(target, hover) && ui_poll_target_enabled(projection, target);
            if (!rounded(paint, row.bounds, projection.answer_fill, style.radius))
                return false;
            // The vote percentage bar sits behind the row content.
            if (results && answer.count_known && state.total_votes_known && projection.percentages[i]) {
                auto bar = row.bounds;
                bar.right = bar.left + ui_rect_width(bar) * static_cast<float>(projection.percentages[i]) / 100;
                const Color fill = victor ? projection.bar_victor : voted && !ended ? projection.bar_voted
                                                                                     : projection.bar;
                if (!rounded(paint, bar, fill, minimum(style.radius, ui_rect_width(bar) * .5f)))
                    return false;
            }
            // Outline: accent while selected (voting) or voted, victor color
            // for a winner, the hover outline on hover while voting.
            Color outline{};
            if (!results)
                outline = answer.selected ? projection.accent : hovered ? projection.outline_hover : Color{};
            else
                outline = victor ? projection.victor : voted && !ended ? projection.accent : Color{};
            if (outline.alpha > 0 && !border(paint, row.bounds, {}, outline, style.radius))
                return false;
            if (!results) {
                if (!selection(paint, row.marker, answer.selected, state.multiselect, projection,
                               projection.foreground))
                    return false;
            } else if (voted) {
                const Color fill = !ended ? projection.accent : victor ? projection.victor : projection.selected;
                const Color ink = !ended || victor ? projection.primary_foreground : projection.foreground;
                if (!own_check(paint, row.marker, fill, ink))
                    return false;
            }
            if (keyboard && ui_poll_target_equal(focus, target) &&
                ui_poll_target_enabled(projection, target) &&
                !border(paint, inset(row.bounds, -2), {}, projection.accent, style.radius + 2, 2))
                return false;
        }
        const UiPollTarget targets[]{{UiPollControl::vote},
                                     {UiPollControl::remove_vote},
                                     {UiPollControl::results},
                                     {UiPollControl::retry}};
        for (auto target : targets) {
            if (!ui_poll_target_visible(projection, target))
                continue;
            const auto bounds = ui_poll_target_rectangle(projection, target);
            if (!ui_rect_has_area(bounds))
                continue;
            const bool enabled = ui_poll_target_enabled(projection, target);
            const bool hovered = enabled && ui_poll_target_equal(target, hover);
            // "Show results" is a text action without a surface.
            const bool text_action = target.control == UiPollControl::results && !projection.results_primary;
            if (!text_action) {
                Color fill = projection.selected;
                if (target.control == UiPollControl::vote) {
                    fill = projection.accent;
                    if (!enabled)
                        fill.alpha *= .5f;
                } else if (hovered)
                    fill = theme.tokens.state.pressed;
                if (!rounded(paint, bounds, fill, style.radius))
                    return false;
            }
            if (keyboard && enabled && ui_poll_target_equal(focus, target) &&
                !border(paint, inset(bounds, -2), {}, projection.accent, style.radius + 2, 2))
                return false;
        }
        return true;
    };
    const bool drawn = draw();
    const bool popped = paint_stream_pop_clip(paint);
    return drawn && popped;
}

bool ui_poll_controls_text_output(const UiPollProjection& projection, const UiPollStyle& style, UiRect clip,
                                  UiWidgetReference owner, UiTextOutput* output, UiPollTarget hover) noexcept {
    if (!output || !owner_current(owner) || !ui_rect_valid_or_empty(clip))
        return false;
    const auto visible = ui_rect_intersection(clip, projection.geometry.bounds);
    const auto& geometry = projection.geometry;
    const auto& state = projection.state;
    const bool results = counts_visible(state);
    const auto status_text = status(projection, style);
    if (!label(output, state.question.bytes ? state.question : style.question_fallback, geometry.question,
               visible, owner, projection.foreground, style.question_style) ||
        !label(output, status_text.bytes ? Utf8View{} : instruction(projection, style), geometry.instruction,
               visible, owner, projection.secondary, style.detail_style) ||
        !label(output, status_text, geometry.status, visible, owner,
               state.error ? projection.error : projection.secondary, style.detail_style) ||
        !label(output, ui_poll_summary_label(projection), geometry.summary, visible, owner, projection.secondary,
               style.detail_style, false))
        return false;
    for (u32 i = 0; i < state.answers.count; ++i) {
        const auto& answer = state.answers[i];
        const auto& row = geometry.answers[i];
        const auto text = answer.text.bytes                            ? answer.text
                          : answer.emoji_id || answer.emoji_name.bytes ? Utf8View{}
                                                                       : style.answer_fallback;
        if (!label(output, text, row.text, visible, owner, projection.foreground, style.answer_style, true,
                   UiTextAlignment::start, UiTextAlignment::center))
            return false;
        if (!answer.emoji_id && answer.emoji_name.bytes &&
            !label(output, answer.emoji_name, row.emoji, visible, owner, projection.foreground, text_style_none,
                   false, UiTextAlignment::center, UiTextAlignment::center))
            return false;
        if (!results)
            continue;
        if (!label(output, ui_poll_count_label(projection, i), row.result, visible, owner, projection.foreground,
                   style.count_style, false, UiTextAlignment::end, UiTextAlignment::center) ||
            (answer.count_known &&
             !label(output, {projection.percent_text[i], projection.percent_bytes[i]}, row.percent, visible, owner,
                    projection.foreground, style.percent_style, false, UiTextAlignment::end,
                    UiTextAlignment::center)))
            return false;
    }
    const UiPollTarget targets[]{{UiPollControl::vote},
                                 {UiPollControl::remove_vote},
                                 {UiPollControl::results},
                                 {UiPollControl::retry}};
    const Utf8View texts[]{style.vote, style.remove_vote,
                           projection.results_primary ? style.back : style.view_results, style.retry};
    for (u32 i = 0; i < 4; ++i) {
        if (!ui_poll_target_visible(projection, targets[i]))
            continue;
        const auto bounds = ui_poll_target_rectangle(projection, targets[i]);
        const bool enabled = ui_poll_target_enabled(projection, targets[i]);
        const bool text_action = targets[i].control == UiPollControl::results && !projection.results_primary;
        Color color = targets[i].control == UiPollControl::vote ? projection.primary_foreground
                                                                : projection.foreground;
        if (!enabled)
            color.alpha *= .5f;
        // The tertiary action underlines on hover.
        const u32 flags = style.button_style |
                          (text_action && enabled && ui_poll_target_equal(targets[i], hover) ? text_style_underline
                                                                                             : 0u);
        if (!label(output, texts[i], bounds, visible, owner, color, flags, false,
                   text_action ? UiTextAlignment::end : UiTextAlignment::center, UiTextAlignment::center))
            return false;
    }
    return true;
}
bool ui_poll_scene_paint(const UiPollScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    return !scene.active ||
           ui_poll_controls_paint(scene.projection, scene.style, scene.clip, theme, scene.owner, paint,
                                  scene.focus, scene.hover, scene.keyboard_focus);
}
bool ui_poll_scene_text_output(const UiPollScene& scene, UiTextOutput* output) noexcept {
    return !scene.active ||
           ui_poll_controls_text_output(scene.projection, scene.style, scene.clip, scene.owner, output, scene.hover);
}
u64 ui_poll_scene_paint_revision(const UiPollScene& scene) noexcept {
    if (!scene.active || !owner_current(scene.owner))
        return 0;
    u64 hash = geometry_hash(kHash64Seed, scene.projection.geometry);
    hash = hash64_bytes(hash, &scene.clip, sizeof(scene.clip));
    hash = hash64_mix(hash, reinterpret_cast<usize>(scene.owner.manager));
    hash = hash64_mix(hash, scene.owner.handle.slot);
    hash = hash64_mix(hash, scene.owner.handle.generation);
    const Color colors[]{scene.projection.surface,     scene.projection.foreground,
                         scene.projection.secondary,   scene.projection.accent,
                         scene.projection.primary_foreground, scene.projection.border,
                         scene.projection.selected,    scene.projection.answer_fill,
                         scene.projection.outline_hover, scene.projection.bar,
                         scene.projection.bar_voted,   scene.projection.bar_victor,
                         scene.projection.victor};
    hash = hash64_bytes(hash, colors, sizeof(colors));
    for (u32 i = 0; i < scene.projection.state.answers.count; ++i) {
        const auto& answer = scene.projection.state.answers[i];
        hash = hash64_mix(hash, answer.id);
        hash = hash64_mix(hash, answer.selected);
        hash = hash64_mix(hash, answer.me_known);
        hash = hash64_mix(hash, answer.me_voted);
        hash = hash64_mix(hash, answer.emoji_id);
        hash = hash64_mix(hash, scene.projection.percentages[i]);
        hash = hash64_mix(hash, scene.projection.victor_answers[i]);
    }
    const bool flags[]{scene.projection.state.results,   scene.projection.state.multiselect,
                       scene.projection.vote_enabled,    scene.projection.remove_enabled,
                       scene.projection.results_enabled, scene.projection.retry_enabled,
                       scene.projection.answers_enabled, scene.projection.results_primary,
                       scene.keyboard_focus,             closed(scene.projection.state)};
    for (bool flag : flags)
        hash = hash64_mix(hash, flag);
    const UiPollControl controls[]{UiPollControl::vote, UiPollControl::remove_vote, UiPollControl::results,
                                   UiPollControl::retry};
    for (auto control : controls)
        hash = hash64_mix(hash, ui_poll_target_visible(scene.projection, {control}));
    hash = hash64_mix(hash, static_cast<u8>(scene.focus.control));
    hash = hash64_mix(hash, scene.focus.answer_id);
    hash = hash64_mix(hash, static_cast<u8>(scene.hover.control));
    hash = hash64_mix(hash, scene.hover.answer_id);
    return hash64_bytes(hash, &scene.style.radius, sizeof(scene.style.radius));
}
u64 ui_poll_scene_text_revision(const UiPollScene& scene) noexcept {
    if (!scene.active || !owner_current(scene.owner))
        return 0;
    UiTextEmitter emitter{};
    UiTextOutput output{nullptr, &emitter};
    return ui_text_emitter_initialize(&emitter) && ui_poll_scene_text_output(scene, &output)
               ? emitter.signature
               : 0;
}

bool ui_poll_voters_scene_paint(const UiPollVotersScene& scene, const Theme& theme,
                                PaintStream* paint) noexcept {
    if (!scene.active)
        return true;
    if (!paint || !owner_current(scene.owner))
        return false;
    const auto& geometry = scene.geometry;
    Color dim = theme.base_lowest;
    dim.alpha = scene.style.backdrop_opacity;
    if (!rounded(paint, geometry.viewport, dim, 0) ||
        !paint_stream_push_clip(paint, geometry.panel.left, geometry.panel.top, geometry.panel.right,
                                geometry.panel.bottom))
        return false;
    const auto draw = [&]() noexcept {
        if (!ui_background_paint_surface(paint, geometry.panel, theme, scene.surface, theme.interactive,
                                         scene.style.radius, 1, 1, UiBackgroundSurfaceMode::gradient,
                                         UiBackgroundTier::high, UiBackgroundBorderTone::palette) ||
            !paint_stream_record_content(paint, scene.owner, geometry.panel, UiLayoutContentKind::control,
                                         ui_layout_content_bounds_allow_overlap) ||
            !ui_icon_paint(paint, UiIcon::close, inset(geometry.close, 6), scene.foreground))
            return false;
        for (u32 i = 0; i < scene.state.answers.count; ++i) {
            const bool selected = scene.state.answers[i].id == scene.state.answer_id;
            if (!border(paint, geometry.answers[i], selected ? scene.accent : scene.surface,
                        theme.interactive, scene.style.radius))
                return false;
        }
        if (!paint_stream_push_clip(paint, geometry.body.left, geometry.body.top, geometry.body.right,
                                    geometry.body.bottom))
            return false;
        bool rows_ok = true;
        for (u32 i = scene.first; rows_ok && i < scene.first + scene.visible; ++i) {
            const auto row = ui_poll_voters_row_rectangle(scene, i);
            const bool hovered = scene.hover.control == UiPollVotersControl::user &&
                                 scene.hover.id == scene.state.voters[i].id;
            rows_ok = !hovered || rounded(paint, row, theme.surface_high, scene.style.radius);
            const float top = row.top + (scene.style.row_height - scene.style.avatar_extent) * .5f;
            const UiRect avatar{row.left, top, row.left + scene.style.avatar_extent,
                                top + scene.style.avatar_extent};
            rows_ok = rows_ok &&
                      rounded(paint, avatar, theme.avatar_placeholder, scene.style.avatar_extent * .5f);
            if (scene.keyboard_focus && scene.focus.control == UiPollVotersControl::user &&
                scene.focus.id == scene.state.voters[i].id)
                rows_ok = rows_ok && border(paint, row, {}, scene.accent, scene.style.radius, 2);
        }
        const bool popped = paint_stream_pop_clip(paint);
        if (!rows_ok || !popped)
            return false;
        const UiPollVotersTarget buttons[]{
            {UiPollVotersControl::previous}, {UiPollVotersControl::next}, {UiPollVotersControl::retry}};
        for (auto button : buttons) {
            const auto bounds = ui_poll_voters_target_rectangle(scene, button);
            if (!ui_rect_has_area(bounds))
                continue;
            const bool enabled = ui_poll_voters_target_enabled(scene, button);
            if (!border(paint, bounds,
                        enabled && button.control == UiPollVotersControl::retry ? scene.accent
                                                                                : scene.surface,
                        theme.interactive, scene.style.radius))
                return false;
        }
        if (scene.keyboard_focus && scene.focus.control != UiPollVotersControl::user &&
            ui_poll_voters_target_enabled(scene, scene.focus) &&
            !border(paint, ui_poll_voters_target_rectangle(scene, scene.focus), {}, scene.accent,
                    scene.style.radius, 2))
            return false;
        return true;
    };
    const bool drawn = draw();
    const bool popped = paint_stream_pop_clip(paint);
    return drawn && popped;
}
bool ui_poll_voters_scene_text_output(const UiPollVotersScene& scene, UiTextOutput* output) noexcept {
    if (!scene.active)
        return true;
    if (!output || !owner_current(scene.owner))
        return false;
    const auto& geometry = scene.geometry;
    if (!label(output, scene.style.title, geometry.title, geometry.panel, scene.owner, scene.foreground,
               text_style_bold, false, UiTextAlignment::start, UiTextAlignment::center) ||
        !label(output, scene.state.question, geometry.question, geometry.panel, scene.owner,
               scene.secondary, text_style_subtext))
        return false;
    for (u32 i = 0; i < scene.state.answers.count; ++i) {
        const auto& answer = scene.state.answers[i];
        if (answer.id == scene.state.answer_id &&
            !label(output,
                   answer.text.bytes         ? answer.text
                   : answer.emoji_name.bytes ? answer.emoji_name
                                             : utf8_literal("Answer"),
                   geometry.answer, geometry.panel, scene.owner, scene.foreground, text_style_bold))
            return false;
        char text[2]{};
        u32 bytes{};
        if (i + 1 >= 10)
            text[bytes++] = '1';
        text[bytes++] = static_cast<char>('0' + (i + 1) % 10);
        const auto color = answer.id == scene.state.answer_id ? scene.accent_foreground : scene.foreground;
        if (!label(output, {text, bytes}, geometry.answers[i], geometry.panel, scene.owner, color,
                   text_style_none, false, UiTextAlignment::center, UiTextAlignment::center))
            return false;
    }
    const auto status_text = scene.state.status.bytes    ? scene.state.status
                             : scene.state.loading       ? scene.style.loading
                             : scene.state.error         ? scene.style.failed
                             : !scene.state.voters.count ? scene.style.empty
                                                         : Utf8View{};
    if (!label(output, status_text, geometry.status, geometry.panel, scene.owner,
               scene.state.error ? scene.error : scene.secondary, text_style_subtext) ||
        !label(output, {scene.page_text, scene.page_bytes}, geometry.page, geometry.panel, scene.owner,
               scene.secondary, text_style_subtext))
        return false;
    for (u32 i = scene.first; i < scene.first + scene.visible; ++i) {
        const auto row = ui_poll_voters_row_rectangle(scene, i);
        const auto text_bounds = UiRect{row.left + scene.style.avatar_extent + scene.style.gap, row.top + 4,
                                        row.right, row.bottom - 4};
        const auto name =
            scene.state.voters[i].name.bytes ? scene.state.voters[i].name : utf8_literal("User");
        if (!label(output, name, text_bounds, geometry.body, scene.owner,
                   scene.state.loading ? scene.secondary : scene.foreground, text_style_none, true,
                   UiTextAlignment::start, UiTextAlignment::center))
            return false;
    }
    const UiPollVotersTarget buttons[]{
        {UiPollVotersControl::previous}, {UiPollVotersControl::next}, {UiPollVotersControl::retry}};
    const Utf8View texts[]{scene.style.previous, scene.style.next, scene.style.retry};
    for (u32 i = 0; i < 3; ++i) {
        const bool enabled = ui_poll_voters_target_enabled(scene, buttons[i]);
        const auto color = enabled && buttons[i].control == UiPollVotersControl::retry
                               ? scene.accent_foreground
                           : enabled ? scene.foreground
                                     : scene.secondary;
        if (!label(output, texts[i], inset(ui_poll_voters_target_rectangle(scene, buttons[i]), 8),
                   geometry.panel, scene.owner, color, text_style_none, false, UiTextAlignment::center,
                   UiTextAlignment::center))
            return false;
    }
    return true;
}
u64 ui_poll_voters_scene_text_revision(const UiPollVotersScene& scene) noexcept {
    if (!scene.active || !owner_current(scene.owner))
        return 0;
    UiTextEmitter emitter{};
    UiTextOutput output{nullptr, &emitter};
    return ui_text_emitter_initialize(&emitter) && ui_poll_voters_scene_text_output(scene, &output)
               ? emitter.signature
               : 0;
}
u64 ui_poll_voters_scene_paint_revision(const UiPollVotersScene& scene) noexcept {
    if (!scene.active || !owner_current(scene.owner))
        return 0;
    const auto& g = scene.geometry;
    const UiRect bounds[]{g.viewport, g.panel, g.title,    g.close, g.question, g.answer,
                          g.status,   g.body,  g.previous, g.next,  g.retry,    g.page};
    u64 hash = hash64_bytes(kHash64Seed, bounds, sizeof(bounds));
    hash = hash64_bytes(hash, g.answers, sizeof(g.answers));
    hash = hash64_mix(hash, reinterpret_cast<usize>(scene.owner.manager));
    hash = hash64_mix(hash, scene.owner.handle.slot);
    hash = hash64_mix(hash, scene.owner.handle.generation);
    hash = hash64_mix(hash, scene.state.answer_id);
    hash = hash64_mix(hash, scene.state.loading);
    hash = hash64_mix(hash, scene.state.has_next);
    hash = hash64_mix(hash, scene.state.has_previous);
    hash = hash64_mix(hash, scene.state.can_retry);
    hash = hash64_mix(hash, scene.first);
    hash = hash64_mix(hash, scene.visible);
    hash = hash64_bytes(hash, &scene.scroll, sizeof(scene.scroll));
    const Color colors[]{scene.surface, scene.foreground, scene.secondary, scene.accent};
    hash = hash64_bytes(hash, colors, sizeof(colors));
    const float metrics[]{scene.style.radius, scene.style.row_height, scene.style.avatar_extent,
                          scene.style.backdrop_opacity};
    hash = hash64_bytes(hash, metrics, sizeof(metrics));
    hash = hash64_mix(hash, scene.keyboard_focus);
    hash = hash64_mix(hash, static_cast<u8>(scene.focus.control));
    hash = hash64_mix(hash, scene.focus.id);
    hash = hash64_mix(hash, static_cast<u8>(scene.hover.control));
    return hash64_mix(hash, scene.hover.id);
}
} // namespace maicord
