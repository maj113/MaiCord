#include "maicord/ui/poll_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord {
namespace {
bool owner_current(UiWidgetReference owner) noexcept {
    const auto* widget = owner.manager ? ui_widget_manager_resolve(*owner.manager, owner.handle) : nullptr;
    return widget && (widget->flags & ui_widget_enabled) && !(widget->flags & ui_widget_block_input);
}
bool rounded(PaintStream* stream, const UiRect& bounds, const Color& color, float radius) noexcept {
    return !ui_rect_has_area(bounds) ||
           paint_stream_push_rounded(stream, bounds, color, radius);
}
bool border(PaintStream* stream, const UiRect& bounds, const Color& fill, const Color& line, float radius,
            float width = 1) noexcept {
    return !ui_rect_has_area(bounds) ||
           paint_stream_push_rounded_border(stream, bounds, fill, line, radius, width);
}
UiRect centered(UiRect bounds, float extent) noexcept {
    const float x = (bounds.left + bounds.right) * .5f, y = (bounds.top + bounds.bottom) * .5f;
    return {x - extent * .5f, y - extent * .5f, x + extent * .5f, y + extent * .5f};
}
// Label placement bits: unwrapped, then the horizontal alignment in bits 2-3 and the vertical in bits 4-5
// (UiTextAlignment values).
constexpr u32 label_nowrap = 1, label_center = 1u << 2, label_end = 2u << 2, label_middle = 1u << 4;
bool label(UiTextOutput* output, const Utf8View& text, const UiRect& bounds, const UiRect& clip,
           const UiWidgetReference& owner, const Color& color, u32 flags, u32 placement = 0) noexcept {
    if (!text.bytes || !ui_rect_intersects(bounds, clip))
        return true;
    return ui_text_output_push_owned(output, {text, owner, bounds, clip, color, flags,
                                              static_cast<UiTextAlignment>((placement >> 2) & 3u),
                                              static_cast<UiTextAlignment>((placement >> 4) & 3u),
                                              placement & label_nowrap ? TextWrapping::no_wrap
                                                                       : TextWrapping::wrap,
                                              TextOverflow::ellipsis});
}
// The prompt line: a status replaces it; the prompt is hidden when closed.
Utf8View instruction(const UiPollProjection& projection, const UiPollStyle& style) noexcept {
    const auto& state = projection.state;
    return !state.supported ? style.unavailable
           : ui_poll_closed(state)  ? Utf8View{}
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
    // The rectangles are consecutive members: six, the answers (six rectangles each), then four.
    static_assert(__builtin_offsetof(UiPollGeometry, answers) == 6 * sizeof(UiRect));
    static_assert(sizeof(UiPollAnswerGeometry) == 6 * sizeof(UiRect));
    static_assert(__builtin_offsetof(UiPollGeometry, vote) ==
                  __builtin_offsetof(UiPollGeometry, answers) + sizeof(geometry.answers));
    static_assert(__builtin_offsetof(UiPollGeometry, retry) == __builtin_offsetof(UiPollGeometry, vote) + 3 * sizeof(UiRect));
    hash = hash64_bytes(hash, &geometry.bounds, 6 * sizeof(UiRect));
    hash = hash64_bytes(hash, &geometry.vote, 4 * sizeof(UiRect));
    return hash64_bytes(hash, geometry.answers, sizeof(geometry.answers));
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
    if (!paint_stream_push_clip(paint, visible))
        return false;
    const auto& state = projection.state;
    const bool results = ui_poll_counts_visible(state);
    const bool ended = ui_poll_closed(state);
    const auto draw = [&]() noexcept {
        if (!ui_background_paint_surface(paint, projection.geometry.bounds, theme, projection.surface,
                                         projection.border, style.radius, 1, 1,
                                         UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high,
                                         UiBackgroundBorderTone::palette))
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
                // The row's own rounded shape cut at the share: concentric with the
                // row, with a square leading edge (the row clips it in Discord).
                const float right =
                    row.bounds.left + ui_rect_width(row.bounds) * static_cast<float>(projection.percentages[i]) / 100;
                const Color fill = victor ? projection.bar_victor : voted && !ended ? projection.bar_voted
                                                                                     : projection.bar;
                if (!paint_stream_push_clip(paint, row.bounds.left, row.bounds.top, right, row.bounds.bottom))
                    return false;
                const bool filled = rounded(paint, row.bounds, fill, style.radius);
                if (!paint_stream_pop_clip(paint) || !filled)
                    return false;
            }
            // Outline: accent while selected (voting) or voted, victor color
            // for a winner, the hover outline on hover while voting.
            Color outline{};
            if (!results)
                outline = answer.selected ? projection.outline : hovered ? projection.outline_hover : Color{};
            else
                outline = victor ? projection.victor : voted && !ended ? projection.outline : Color{};
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
                !border(paint, ui_rect_shrink(row.bounds, -2), {}, projection.outline, style.radius + 2, 2))
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
                !border(paint, ui_rect_shrink(bounds, -2), {}, projection.outline, style.radius + 2, 2))
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
    const bool results = ui_poll_counts_visible(state);
    const auto status_text = status(projection, style);
    if (!label(output, state.question.bytes ? state.question : style.question_fallback, geometry.question,
               visible, owner, projection.foreground, style.question_style) ||
        !label(output, status_text.bytes ? Utf8View{} : instruction(projection, style), geometry.instruction,
               visible, owner, projection.secondary, style.detail_style, label_nowrap) ||
        !label(output, status_text, geometry.status, visible, owner,
               state.error ? projection.error : projection.secondary, style.detail_style, label_nowrap) ||
        !label(output, ui_poll_summary_label(projection), geometry.summary, visible, owner, projection.secondary,
               style.detail_style, label_nowrap))
        return false;
    for (u32 i = 0; i < state.answers.count; ++i) {
        const auto& answer = state.answers[i];
        const auto& row = geometry.answers[i];
        const auto text = answer.text.bytes                            ? answer.text
                          : answer.emoji_id || answer.emoji_name.bytes ? Utf8View{}
                                                                       : style.answer_fallback;
        if (!label(output, text, row.text, visible, owner, projection.foreground, style.answer_style,
                   label_middle))
            return false;
        if (!answer.emoji_id && answer.emoji_name.bytes &&
            !label(output, answer.emoji_name, row.emoji, visible, owner, projection.foreground, text_style_none,
                   label_nowrap | label_center | label_middle))
            return false;
        if (!results)
            continue;
        if (!label(output, ui_poll_count_label(projection, i), row.result, visible, owner, projection.foreground,
                   style.count_style, label_nowrap | label_end | label_middle) ||
            (answer.count_known &&
             !label(output, {projection.percent_text[i], projection.percent_bytes[i]}, row.percent, visible, owner,
                    projection.foreground, style.percent_style, label_nowrap | label_end | label_middle)))
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
        if (!label(output, texts[i], bounds, visible, owner, color, flags,
                   label_nowrap | (text_action ? label_end : label_center) | label_middle))
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
    static_assert(__builtin_offsetof(UiPollProjection, selected) ==
                  __builtin_offsetof(UiPollProjection, surface) + 6 * sizeof(Color));
    static_assert(__builtin_offsetof(UiPollProjection, victor) ==
                  __builtin_offsetof(UiPollProjection, answer_fill) + 6 * sizeof(Color));
    hash = hash64_bytes(hash, &scene.projection.surface, 7 * sizeof(Color));
    hash = hash64_bytes(hash, &scene.projection.answer_fill, 7 * sizeof(Color));
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
                       scene.keyboard_focus,             ui_poll_closed(scene.projection.state)};
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
    return scene.active && owner_current(scene.owner)
               ? ui_text_output_signature<UiPollScene, ui_poll_scene_text_output>(scene)
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
        !paint_stream_push_clip(paint, geometry.panel))
        return false;
    const auto draw = [&]() noexcept {
        if (!ui_background_paint_surface(paint, geometry.panel, theme, scene.surface, theme.interactive,
                                         scene.style.radius, 1, 1, UiBackgroundSurfaceMode::gradient,
                                         UiBackgroundTier::high, UiBackgroundBorderTone::palette) ||
            !ui_icon_paint(paint, UiIcon::close, ui_rect_shrink(geometry.close, 6), scene.foreground))
            return false;
        for (u32 i = 0; i < scene.state.answers.count; ++i) {
            const bool selected = scene.state.answers[i].id == scene.state.answer_id;
            const bool hovered = scene.hover.control == UiPollVotersControl::answer &&
                                 scene.hover.id == scene.state.answers[i].id;
            // The answer list marks the open answer and the hovered row only.
            if (geometry.columns ? (selected || hovered) &&
                                       !rounded(paint, geometry.answers[i],
                                                selected ? theme.tokens.fill.muted : theme.surface_high,
                                                scene.style.radius)
                                 : !border(paint, geometry.answers[i], selected ? scene.accent : scene.surface,
                                           theme.interactive, scene.style.radius))
                return false;
        }
        if (!paint_stream_push_clip(paint, geometry.body))
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
               text_style_bold, label_nowrap | label_middle) ||
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
        if (geometry.columns) {
            // "Answer text ... N": the label ellipsizes before the vote count.
            char count[16]{};
            const u32 count_bytes = answer.count_known ? decimal_write_grouped(count, answer.count) : 0u;
            const float pad = scene.style.gap;
            const float count_width = count_bytes ? scene.style.avatar_extent * 1.5f + pad : 0.f;
            const auto& row = geometry.answers[i];
            const UiRect text_box{row.left + pad, row.top, row.right - pad - count_width, row.bottom};
            const UiRect count_box{row.right - pad - count_width, row.top, row.right - pad, row.bottom};
            const Utf8View text = answer.text.bytes         ? answer.text
                                  : answer.emoji_name.bytes ? answer.emoji_name
                                                            : utf8_literal("Answer");
            if (!label(output, text, text_box, geometry.panel, scene.owner, scene.foreground,
                       answer.id == scene.state.answer_id ? text_style_bold : text_style_none,
                       label_nowrap | label_middle) ||
                (count_bytes && !label(output, {count, count_bytes}, count_box, geometry.panel, scene.owner,
                                       scene.secondary, text_style_subtext,
                                       label_nowrap | label_end | label_middle)))
                return false;
            continue;
        }
        char text[2]{};
        u32 bytes{};
        if (i + 1 >= 10)
            text[bytes++] = '1';
        text[bytes++] = static_cast<char>('0' + (i + 1) % 10);
        const auto color = answer.id == scene.state.answer_id ? scene.accent_foreground : scene.foreground;
        if (!label(output, {text, bytes}, geometry.answers[i], geometry.panel, scene.owner, color,
                   text_style_none, label_nowrap | label_center | label_middle))
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
                   scene.state.loading ? scene.secondary : scene.foreground, text_style_none, label_middle))
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
        if (!label(output, texts[i], ui_rect_shrink(ui_poll_voters_target_rectangle(scene, buttons[i]), 8),
                   geometry.panel, scene.owner, color, text_style_none,
                   label_nowrap | label_center | label_middle))
            return false;
    }
    return true;
}
u64 ui_poll_voters_scene_text_revision(const UiPollVotersScene& scene) noexcept {
    return scene.active && owner_current(scene.owner)
               ? ui_text_output_signature<UiPollVotersScene, ui_poll_voters_scene_text_output>(scene)
               : 0;
}
u64 ui_poll_voters_scene_paint_revision(const UiPollVotersScene& scene) noexcept {
    if (!scene.active || !owner_current(scene.owner))
        return 0;
    const auto& g = scene.geometry;
    static_assert(__builtin_offsetof(UiPollVotersGeometry, body) == 7 * sizeof(UiRect));
    static_assert(__builtin_offsetof(UiPollVotersGeometry, page) ==
                  __builtin_offsetof(UiPollVotersGeometry, previous) + 3 * sizeof(UiRect));
    u64 hash = hash64_bytes(kHash64Seed, &g.viewport, 8 * sizeof(UiRect));
    hash = hash64_bytes(hash, &g.previous, 4 * sizeof(UiRect));
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
    static_assert(__builtin_offsetof(UiPollVotersScene, accent) == __builtin_offsetof(UiPollVotersScene, surface) + 3 * sizeof(Color));
    hash = hash64_bytes(hash, &scene.surface, 4 * sizeof(Color));
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
