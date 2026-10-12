#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool positive(float value) noexcept {
    return ui_coordinate_valid(value) && value > 0;
}
bool text_valid(Utf8View text, u32 bytes, u32 scalars = ~0u) noexcept {
    if (text.bytes > bytes || (!text.data && text.bytes))
        return false;
    u32 offset{}, count{};
    while (offset < text.bytes) {
        const auto decoded = utf8_decode_one(text.data + offset, text.bytes - offset);
        if (!decoded.valid || ++count > scalars)
            return false;
        offset += decoded.width;
    }
    return true;
}
bool state_valid(const UiPollState& state) noexcept {
    if (!state.owner || !state.generation || !state.revision || !state.content_revision ||
        !text_valid(state.question, kUiPollQuestionMaximumBytes, 300) || !text_valid(state.status, 256) ||
        !state.answers.data || !state.answers.count || state.answers.count > kUiPollMaximumAnswers)
        return false;
    if (state.total_votes_known && state.total_votes > static_cast<u64>(~0u) * kUiPollMaximumAnswers)
        return false;
    u32 selected{};
    for (u32 i = 0; i < state.answers.count; ++i) {
        const auto& answer = state.answers[i];
        if (!answer.id || !text_valid(answer.text, kUiPollAnswerMaximumBytes, 55) ||
            !text_valid(answer.emoji_name, kUiPollEmojiMaximumBytes))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (state.answers[j].id == answer.id)
                return false;
        selected += answer.selected ? 1u : 0u;
    }
    return state.multiselect || selected <= 1;
}
bool style_valid(const UiPollStyle& style) noexcept {
    const float lengths[]{style.padding,
                          style.gap,
                          style.answer_padding,
                          style.answer_gap,
                          style.minimum_width,
                          style.maximum_width,
                          style.target,
                          style.marker,
                          style.emoji_extent,
                          style.question_minimum_height,
                          style.answer_minimum_height,
                          style.label_minimum_height,
                          style.radius};
    for (float value : lengths)
        if (!ui_coordinate_valid(value) || value < 0)
            return false;
    if (style.minimum_width < 128 || style.maximum_width < style.minimum_width || style.target < 32 ||
        style.marker < 12 || style.emoji_extent < 16 || style.label_minimum_height < 12)
        return false;
    const Utf8View labels[]{style.question_fallback,
                            style.answer_fallback,
                            style.vote,
                            style.remove_vote,
                            style.view_results,
                            style.back,
                            style.view_voters,
                            style.retry,
                            style.single,
                            style.multi,
                            style.closed,
                            style.unavailable,
                            style.unknown_results,
                            style.loading,
                            style.failed};
    for (auto label : labels)
        if (!label.bytes || !text_valid(label, 256))
            return false;
    return true;
}
bool closed(const UiPollState& state) noexcept {
    return (state.finalized_known && state.finalized) ||
           (state.expiry_known && state.expiry_ms && state.now_ms >= state.expiry_ms);
}
u32 selected_count(const UiPollState& state) noexcept {
    u32 count{};
    for (u32 i = 0; i < state.answers.count; ++i)
        count += state.answers[i].selected ? 1u : 0u;
    return count;
}
bool own_vote(const UiPollState& state) noexcept {
    for (u32 i = 0; i < state.answers.count; ++i)
        if (state.answers[i].me_known && state.answers[i].me_voted)
            return true;
    return false;
}
// The primary action, first match: none when closed; "Remove Vote" once
// voted; "Go back to vote" while results are shown without a vote; "Vote".
UiPollControl primary(const UiPollState& state) noexcept {
    if (state.can_retry)
        return UiPollControl::retry;
    if (!state.supported || closed(state))
        return UiPollControl::none;
    if (own_vote(state))
        return state.can_remove_vote ? UiPollControl::remove_vote : UiPollControl::none;
    if (state.results)
        return state.can_view_results ? UiPollControl::results : UiPollControl::none;
    return UiPollControl::vote;
}
// The tertiary "Show results" text action: open, not voted, results hidden.
bool show_results(const UiPollState& state) noexcept {
    return state.supported && !closed(state) && !own_vote(state) && !state.results && state.can_view_results &&
           !state.can_retry;
}
// Results view (vote counts shown): voted, closed or "Show results".
bool counts_visible(const UiPollState& state) noexcept {
    return state.results || closed(state);
}
Utf8View question(const UiPollState& state, const UiPollStyle& style) noexcept {
    return state.question.bytes ? state.question : style.question_fallback;
}
Utf8View answer_text(const UiPollAnswer& answer, const UiPollStyle& style) noexcept {
    return answer.text.bytes ? answer.text : style.answer_fallback;
}
u64 content_signature(const UiPollState& state, const UiPollStyle& style) noexcept {
    u64 hash = hash64_counted_bytes(kHash64Seed, state.question.data, state.question.bytes);
    hash = hash64_mix(hash, state.answers.count);
    hash = hash64_mix(hash, state.multiselect);
    for (u32 i = 0; i < state.answers.count; ++i) {
        const auto& answer = state.answers[i];
        hash = hash64_mix(hash, answer.id);
        hash = hash64_counted_bytes(hash, answer.text.data, answer.text.bytes);
        hash = hash64_counted_bytes(hash, answer.emoji_name.data, answer.emoji_name.bytes);
        hash = hash64_mix(hash, answer.emoji_id);
    }
    const float metrics[]{style.padding,
                          style.gap,
                          style.answer_padding,
                          style.answer_gap,
                          style.minimum_width,
                          style.maximum_width,
                          style.target,
                          style.marker,
                          style.emoji_extent,
                          style.question_minimum_height,
                          style.answer_minimum_height,
                          style.label_minimum_height,
                          style.radius};
    hash = hash64_bytes(hash, metrics, sizeof(metrics));
    const Utf8View labels[]{style.question_fallback,
                            style.answer_fallback,
                            style.vote,
                            style.remove_vote,
                            style.view_results,
                            style.back,
                            style.retry,
                            style.single,
                            style.multi,
                            style.closed,
                            style.unavailable,
                            style.loading,
                            style.failed,
                            style.unknown_results};
    for (auto label : labels)
        hash = hash64_counted_bytes(hash, label.data, label.bytes);
    const u32 flags[]{style.question_style, style.answer_style, style.detail_style,
                      style.count_style,    style.percent_style, style.button_style};
    return hash64_bytes(hash, flags, sizeof(flags));
}
bool measure(const UiPollState& state, Utf8View text, u16 item, u32 flags, float width, bool no_wrap,
             UiTextMeasureCache* cache, u16 backend, UiTextMeasureValue* value) noexcept {
    if (!positive(width))
        return false;
    const TextStyleSpan span{0, text.bytes, flags};
    u64 signature = hash64_counted_bytes(kHash64Seed, text.data, text.bytes);
    signature = hash64_mix(signature, flags);
    signature = hash64_mix(signature, no_wrap);
    return ui_text_measure_cache_measure(cache,
                                         {{state.owner, signature, state.content_revision, item, backend},
                                          text,
                                          flags == text_style_none ? ConstSlice<TextStyleSpan>{}
                                                                   : ConstSlice<TextStyleSpan>{&span, 1},
                                          {width, kMaximumUiCoordinate},
                                          no_wrap},
                                         value)
               .ok() &&
           positive(value->size.height) && ui_coordinate_valid(value->size.width);
}
template <u32 N> void number(char (&text)[N], u8& count, u64 value, bool grouped = false) noexcept {
    char reverse[32]{};
    u32 bytes{}, digits{};
    do {
        if (grouped && digits && digits % 3 == 0)
            reverse[bytes++] = ',';
        reverse[bytes++] = static_cast<char>('0' + value % 10);
        value /= 10;
        ++digits;
    } while (value);
    while (bytes && count < N)
        text[count++] = reverse[--bytes];
}
template <u32 N> void literal(char (&text)[N], u8& count, const char* value) noexcept {
    while (*value && count < N)
        text[count++] = *value++;
}
// "{n} vote(s)" with grouped digits.
template <u32 N> void votes_label(char (&text)[N], u8& count, u64 votes) noexcept {
    number(text, count, votes, true);
    literal(text, count, votes == 1 ? " vote" : " votes");
}
void labels(UiPollProjection& projection, const UiPollStyle& style) noexcept {
    const auto& state = projection.state;
    u32 top{};
    for (u32 i = 0; i < state.answers.count; ++i)
        if (state.answers[i].count_known)
            top = maximum(top, state.answers[i].count);
    const bool ended = closed(state);
    for (u32 i = 0; i < state.answers.count; ++i) {
        const auto& answer = state.answers[i];
        if (!answer.count_known) {
            literal(projection.count_text[i], projection.count_bytes[i], "--");
            continue;
        }
        votes_label(projection.count_text[i], projection.count_bytes[i], answer.count);
        // 100 * votes / total rounded half up; total sums every answer's votes.
        u32 percent = 0;
        if (state.total_votes_known && state.total_votes && answer.count <= state.total_votes)
            percent = minimum(static_cast<u32>((static_cast<u64>(answer.count) * 200u + state.total_votes) /
                                               (2u * state.total_votes)),
                              100u);
        projection.percentages[i] = percent;
        number(projection.percent_text[i], projection.percent_bytes[i], percent);
        literal(projection.percent_text[i], projection.percent_bytes[i], "%");
        // Winners once closed: the most votes, at least one; ties share it.
        projection.victor_answers[i] = ended && top && answer.count == top;
    }
    if (ended)
        literal(projection.expiry_text, projection.expiry_bytes, "Poll closed");
    else if (state.expiry_known && state.expiry_ms) {
        const u64 remaining = state.expiry_ms - state.now_ms;
        constexpr u64 minute = 60'000, hour = 60 * minute, day = 24 * hour;
        if (remaining > day) {
            number(projection.expiry_text, projection.expiry_bytes, remaining / day);
            literal(projection.expiry_text, projection.expiry_bytes, "d left");
        } else if (remaining > hour) {
            number(projection.expiry_text, projection.expiry_bytes, remaining / hour);
            literal(projection.expiry_text, projection.expiry_bytes, "h left");
        } else if (remaining >= minute) {
            number(projection.expiry_text, projection.expiry_bytes, remaining / minute);
            literal(projection.expiry_text, projection.expiry_bytes, "m left");
        } else
            literal(projection.expiry_text, projection.expiry_bytes, "< 1m left");
    }
    if (state.total_votes_known)
        votes_label(projection.summary_text, projection.summary_bytes, state.total_votes);
    if (projection.expiry_bytes) {
        // Details after the first are preceded by a U+2219 bullet.
        if (projection.summary_bytes)
            literal(projection.summary_text, projection.summary_bytes, " \xE2\x88\x99 ");
        for (u32 i = 0; i < projection.expiry_bytes && projection.summary_bytes < sizeof(projection.summary_text);
             ++i)
            projection.summary_text[projection.summary_bytes++] = projection.expiry_text[i];
    }
    (void)style;
}
const UiPollAnswer* find_answer(const UiPollState& state, u32 id, u32* index = nullptr) noexcept {
    for (u32 i = 0; i < state.answers.count; ++i)
        if (state.answers[i].id == id) {
            if (index)
                *index = i;
            return &state.answers[i];
        }
    return nullptr;
}
void offset(UiRect& rectangle, UiPoint point) noexcept {
    if (ui_rect_has_area(rectangle)) {
        rectangle.left += point.x;
        rectangle.right += point.x;
        rectangle.top += point.y;
        rectangle.bottom += point.y;
    }
}
bool owner_current(UiWidgetReference owner) noexcept {
    const auto* widget = owner.manager ? ui_widget_manager_resolve(*owner.manager, owner.handle) : nullptr;
    return widget && (widget->flags & ui_widget_enabled) && !(widget->flags & ui_widget_block_input);
}
bool contains(UiRect rectangle, UiPoint point) noexcept {
    return ui_rect_has_area(rectangle) && point.x >= rectangle.left && point.x < rectangle.right &&
           point.y >= rectangle.top && point.y < rectangle.bottom;
}
bool scene_current(const UiPollScene& scene) noexcept {
    return scene.active && owner_current(scene.owner);
}
bool eligible(const UiPollScene& scene, UiPollTarget target) noexcept {
    return scene_current(scene) && ui_poll_target_enabled(scene.projection, target) &&
           ui_rect_has_area(
               ui_rect_intersection(ui_poll_target_rectangle(scene.projection, target), scene.clip));
}
u32 targets(const UiPollProjection& projection, UiPollTarget* output) noexcept {
    u32 count{};
    for (u32 i = 0; i < projection.state.answers.count; ++i)
        output[count++] = {UiPollControl::answer, projection.state.answers[i].id};
    output[count++] = {UiPollControl::results};
    output[count++] = {UiPollControl::vote};
    output[count++] = {UiPollControl::remove_vote};
    output[count++] = {UiPollControl::retry};
    return count;
}
} // namespace

bool ui_poll_measure_layout(const UiPollState& state, float width, const UiPollStyle& style,
                            UiTextMeasureCache* cache, u16 backend, UiPollLayout* out) noexcept {
    if (!out || !cache || !state_valid(state) || !style_valid(style) || !positive(width))
        return false;
    width = minimum(width, style.maximum_width);
    if (width < style.minimum_width)
        return false;
    UiPollLayout layout{};
    layout.width = width;
    layout.content_revision = state.content_revision;
    layout.content_signature = content_signature(state, style);
    auto& geometry = layout.geometry;
    const float left = style.padding, right = width - style.padding;
    // Answer rows: vertical padding is half the inline padding; item_gap
    // separates label, votes and icon.
    const float row_vertical = style.answer_padding * .5f, item_gap = style.gap;
    if (right - left < 2 * style.answer_padding + style.marker + item_gap)
        return false;
    UiTextMeasureValue measured{};
    // Header grid: question/prompt beside a reserved info column.
    const float info_column = style.marker + style.gap;
    const float header_right = right - left > info_column + style.marker ? right - info_column : right;
    if (!measure(state, question(state, style), 0, style.question_style, header_right - left, false, cache,
                 backend, &measured))
        return false;
    float top = style.padding;
    geometry.question = {left, top, header_right,
                         top + maximum(style.question_minimum_height, measured.size.height)};
    top = geometry.question.bottom + style.gap * .5f;
    const Utf8View helper_labels[]{style.single,  style.multi,  style.closed,         style.unavailable,
                                   style.loading, style.failed, style.unknown_results};
    float helper_height = style.label_minimum_height;
    for (u32 i = 0; i < 7; ++i) {
        if (!measure(state, helper_labels[i], static_cast<u16>(70 + i), style.detail_style, header_right - left,
                     false, cache, backend, &measured))
            return false;
        helper_height = maximum(helper_height, measured.size.height);
    }
    geometry.instruction = {left, top, header_right, top + helper_height};
    geometry.status = geometry.instruction;
    top = geometry.instruction.bottom + style.gap;
    // The widest vote data ("N votes" + gap + "NN%") is reserved while
    // wrapping labels so a row keeps one height in every view.
    UiTextMeasureValue count_size{}, percent_size{};
    if (!measure(state, utf8_literal("99,999 votes"), 2, style.count_style, kMaximumUiCoordinate, true, cache,
                 backend, &count_size) ||
        !measure(state, utf8_literal("100%"), 3, style.percent_style, kMaximumUiCoordinate, true, cache, backend,
                 &percent_size))
        return false;
    // In narrow cards the vote count is dropped first, then the percentage, so
    // a label always keeps room to wrap.
    const float label_room = right - left - 2 * style.answer_padding - style.marker - item_gap -
                             (style.emoji_extent + 2 + item_gap);
    const float full_votes = count_size.size.width + item_gap + percent_size.size.width;
    geometry.compact = label_room - full_votes - item_gap < 2 * style.marker;
    const bool percent_visible = label_room - percent_size.size.width - item_gap >= 2 * style.marker;
    const float votes_width = !geometry.compact ? full_votes : percent_visible ? percent_size.size.width : 0.0f;
    const float votes_reserve = votes_width ? votes_width + item_gap : 0.0f;
    float row_text_height = 0;
    float text_heights[kUiPollMaximumAnswers]{};
    for (u32 i = 0; i < state.answers.count; ++i) {
        const auto& answer = state.answers[i];
        const bool emoji = answer.emoji_id || answer.emoji_name.bytes;
        const float text_left =
            left + style.answer_padding + (emoji ? style.emoji_extent + 2 + item_gap : 0.0f);
        const float text_right =
            maximum(text_left + 1, right - style.answer_padding - style.marker - item_gap - votes_reserve);
        float text_height = emoji ? style.emoji_extent : style.label_minimum_height;
        if (answer.text.bytes || !emoji) {
            if (!measure(state, answer_text(answer, style), static_cast<u16>(10 + i), style.answer_style,
                         text_right - text_left, false, cache, backend, &measured))
                return false;
            text_height = maximum(text_height, measured.size.height);
        }
        text_heights[i] = text_height;
        row_text_height = maximum(row_text_height, text_height);
    }
    // Every answer row is as tall as the tallest.
    const float row_height = maximum(style.answer_minimum_height, 2 * row_vertical + row_text_height);
    for (u32 i = 0; i < state.answers.count; ++i) {
        const auto& answer = state.answers[i];
        auto& row = geometry.answers[i];
        const bool emoji = answer.emoji_id || answer.emoji_name.bytes;
        const float middle = top + row_height * .5f;
        row.bounds = {left, top, right, top + row_height};
        float cursor = left + style.answer_padding;
        if (emoji) {
            row.emoji = {cursor, middle - style.emoji_extent * .5f, cursor + style.emoji_extent,
                         middle + style.emoji_extent * .5f};
            cursor = row.emoji.right + 2 + item_gap;
        }
        const float icon_right = right - style.answer_padding;
        row.marker = {icon_right - style.marker, middle - style.marker * .5f, icon_right,
                      middle + style.marker * .5f};
        if (percent_visible)
            row.percent = {row.marker.left - item_gap - percent_size.size.width,
                           middle - percent_size.size.height * .5f, row.marker.left - item_gap,
                           middle + percent_size.size.height * .5f};
        if (!geometry.compact)
            row.result = {row.percent.left - item_gap - count_size.size.width, middle - count_size.size.height * .5f,
                          row.percent.left - item_gap, middle + count_size.size.height * .5f};
        const float text_right = maximum(cursor + 1, row.marker.left - item_gap - votes_reserve);
        row.text = {cursor, middle - text_heights[i] * .5f, text_right, middle + text_heights[i] * .5f};
        top = row.bounds.bottom + (i + 1 < state.answers.count ? style.answer_gap : 0.0f);
    }
    top += 2 * style.gap;
    // Footer: details text, then "Show results" and the primary
    // button on the right, 16 DIPs apart; it wraps to a second row when narrow.
    const Utf8View button_labels[]{style.vote, style.remove_vote, style.back, style.view_results, style.retry};
    float widths[5]{};
    for (u32 i = 0; i < 5; ++i) {
        if (!measure(state, button_labels[i], static_cast<u16>(30 + i), style.button_style, kMaximumUiCoordinate,
                     true, cache, backend, &measured))
            return false;
        // Buttons pad 16 DIPs a side; the "Show results" text action has none.
        widths[i] = minimum(right - left, i == 3 ? measured.size.width
                                                 : maximum(style.target * 1.5f, measured.size.width + 2 * style.padding));
    }
    layout.vote_width = widths[0];
    layout.remove_width = widths[1];
    layout.back_width = widths[2];
    layout.show_width = widths[3];
    layout.retry_width = widths[4];
    if (!measure(state, utf8_literal("99,999 votes \xE2\x88\x99 Poll closed"), 4, style.detail_style,
                 kMaximumUiCoordinate, true, cache, backend, &measured))
        return false;
    const float details_width = measured.size.width;
    const float actions_width = widths[3] + 2 * style.padding + maximum(maximum(widths[0], widths[1]),
                                                                        maximum(widths[2], widths[4]));
    const bool stacked = details_width + 2 * style.padding * .5f + actions_width > right - left;
    geometry.footer_rows = stacked ? 2 : 1;
    const float details_height = maximum(style.label_minimum_height, measured.size.height);
    geometry.summary = {left, top + (style.target - details_height) * .5f, right,
                        top + (style.target + details_height) * .5f};
    if (stacked)
        top += style.target + style.gap;
    // Buttons are placed per state in projection; this row is their band.
    geometry.vote = geometry.remove_vote = geometry.results = geometry.retry = {left, top, right, top + style.target};
    top += style.target;
    layout.height = top + style.padding;
    geometry.bounds = {0, 0, width, layout.height};
    if (!positive(layout.height) || layout.height > kMaximumUiCoordinate)
        return false;
    *out = layout;
    return true;
}

bool ui_poll_controls_project_layout(const UiPollState& state, UiPoint origin, const UiPollLayout& layout,
                                     const UiPollStyle& style, const Theme& theme,
                                     UiPollProjection* out) noexcept {
    if (!out || !state_valid(state) || !style_valid(style) || !ui_coordinate_valid(origin.x) ||
        !ui_coordinate_valid(origin.y) || layout.content_revision != state.content_revision ||
        layout.content_signature != content_signature(state, style) || !positive(layout.height) ||
        !positive(layout.width) || layout.width < style.minimum_width ||
        layout.width > style.maximum_width ||
        !ui_rect_equal(layout.geometry.bounds, {0, 0, layout.width, layout.height}))
        return false;
    UiPollProjection projection{};
    projection.state = state;
    projection.geometry = layout.geometry;
    auto& geometry = projection.geometry;
    // Footer actions, right-aligned in their band: [Show results] [primary].
    const UiRect band = layout.geometry.vote;
    const auto primary_control = primary(state);
    const float primary_width = primary_control == UiPollControl::vote          ? layout.vote_width
                                : primary_control == UiPollControl::remove_vote ? layout.remove_width
                                : primary_control == UiPollControl::results     ? layout.back_width
                                : primary_control == UiPollControl::retry       ? layout.retry_width
                                                                                : 0.0f;
    const UiRect primary_rect{band.right - primary_width, band.top, band.right, band.bottom};
    geometry.vote = primary_control == UiPollControl::vote ? primary_rect : UiRect{};
    geometry.remove_vote = primary_control == UiPollControl::remove_vote ? primary_rect : UiRect{};
    geometry.retry = primary_control == UiPollControl::retry ? primary_rect : UiRect{};
    projection.results_primary = primary_control == UiPollControl::results;
    if (projection.results_primary)
        geometry.results = primary_rect;
    else if (show_results(state)) {
        const float right = primary_width ? primary_rect.left - 2 * style.padding : band.right;
        geometry.results = {right - layout.show_width, band.top, right, band.bottom};
    } else
        geometry.results = {};
    if (layout.geometry.footer_rows == 1) {
        // The details text ends before the actions.
        const float actions_left = ui_rect_has_area(geometry.results) ? geometry.results.left
                                   : primary_width                    ? primary_rect.left
                                                                      : band.right;
        geometry.summary.right = maximum(geometry.summary.left + 1, actions_left - 2 * style.padding * .5f);
    }
    UiRect* rectangles[]{
        &geometry.bounds, &geometry.question, &geometry.instruction, &geometry.status,  &geometry.summary,
        &geometry.expiry, &geometry.vote,     &geometry.remove_vote, &geometry.results, &geometry.retry};
    for (auto* rectangle : rectangles)
        offset(*rectangle, origin);
    for (auto& answer : geometry.answers) {
        UiRect* parts[]{&answer.bounds, &answer.marker, &answer.emoji, &answer.text, &answer.result,
                        &answer.percent};
        for (auto* part : parts)
            offset(*part, origin);
    }
    if (!ui_rect_valid_or_empty(geometry.bounds))
        return false;
    projection.surface = theme.surface_high;
    projection.foreground = ui_foreground_color(theme, theme.text_primary);
    projection.secondary = ui_foreground_color(theme, theme.tokens.text.muted);
    projection.accent = theme.tokens.accent.brand;
    projection.primary_foreground = ui_surface_foreground(theme, theme.tokens.accent.brand);
    projection.border = theme.tokens.border.subtle;
    projection.selected = theme.tokens.fill.muted;
    projection.error = ui_foreground_color(theme, theme.tokens.text.danger);
    projection.answer_fill = theme.tokens.fill.subtle;
    projection.outline_hover = theme.tokens.border.strong;
    projection.bar = theme.tokens.state.pressed;
    projection.bar_voted = ui_theme_color_alpha(theme.tokens.accent.brand, .2f);
    projection.victor = theme.tokens.accent.success;
    projection.bar_victor = ui_theme_color_alpha(theme.tokens.accent.success, .2f);
    const bool mutable_poll = state.supported && !state.loading && !closed(state);
    const bool results = counts_visible(state);
    projection.answers_enabled =
        results ? state.can_view_voters && !state.loading : mutable_poll && state.can_vote;
    projection.vote_enabled = mutable_poll && state.can_vote && selected_count(state);
    projection.remove_enabled = mutable_poll && state.can_remove_vote && own_vote(state);
    projection.results_enabled = state.can_view_results && !state.loading;
    projection.retry_enabled = state.can_retry && !state.loading;
    labels(projection, style);
    *out = projection;
    return true;
}
bool ui_poll_controls_project(const UiPollState& state, UiRect bounds, const UiPollStyle& style,
                              const Theme& theme, UiTextMeasureCache* cache, u16 backend,
                              UiPollProjection* out) noexcept {
    UiPollLayout layout{};
    return ui_rect_valid_or_empty(bounds) &&
           ui_poll_measure_layout(state, ui_rect_width(bounds), style, cache, backend, &layout) &&
           ui_poll_controls_project_layout(state, {bounds.left, bounds.top}, layout, style, theme, out);
}
Utf8View ui_poll_count_label(const UiPollProjection& projection, u32 index) noexcept {
    return index < projection.state.answers.count
               ? Utf8View{projection.count_text[index], projection.count_bytes[index]}
               : Utf8View{};
}
Utf8View ui_poll_summary_label(const UiPollProjection& projection) noexcept {
    return {projection.summary_text, projection.summary_bytes};
}
Utf8View ui_poll_expiry_label(const UiPollProjection& projection) noexcept {
    return {projection.expiry_text, projection.expiry_bytes};
}
UiRect ui_poll_target_rectangle(const UiPollProjection& projection, UiPollTarget target) noexcept {
    u32 index{};
    switch (target.control) {
    case UiPollControl::answer:
        return find_answer(projection.state, target.answer_id, &index)
                   ? projection.geometry.answers[index].bounds
                   : UiRect{};
    case UiPollControl::vote:
        return projection.geometry.vote;
    case UiPollControl::remove_vote:
        return projection.geometry.remove_vote;
    case UiPollControl::results:
        return projection.geometry.results;
    case UiPollControl::retry:
        return projection.geometry.retry;
    default:
        return {};
    }
}
bool ui_poll_target_enabled(const UiPollProjection& projection, UiPollTarget target) noexcept {
    if (target.control != UiPollControl::answer && target.answer_id)
        return false;
    if (!ui_poll_target_visible(projection, target))
        return false;
    switch (target.control) {
    case UiPollControl::answer:
        return projection.answers_enabled;
    case UiPollControl::vote:
        return projection.vote_enabled;
    case UiPollControl::remove_vote:
        return projection.remove_enabled;
    case UiPollControl::results:
        return projection.results_enabled;
    case UiPollControl::retry:
        return projection.retry_enabled;
    default:
        return false;
    }
}
bool ui_poll_target_visible(const UiPollProjection& projection, UiPollTarget target) noexcept {
    if ((target.control != UiPollControl::answer && target.answer_id) ||
        !ui_rect_has_area(ui_poll_target_rectangle(projection, target)))
        return false;
    const auto& state = projection.state;
    switch (target.control) {
    case UiPollControl::answer:
        return true;
    case UiPollControl::results:
        return projection.results_primary ? primary(state) == UiPollControl::results : show_results(state);
    default:
        return target.control == primary(state);
    }
}
UiPollTarget ui_poll_controls_hit(const UiPollProjection& projection, UiPoint point) noexcept {
    UiPollTarget candidates[kUiPollMaximumAnswers + 4]{};
    const auto count = targets(projection, candidates);
    for (u32 i = 0; i < count; ++i)
        if (ui_poll_target_enabled(projection, candidates[i]) &&
            contains(ui_poll_target_rectangle(projection, candidates[i]), point))
            return candidates[i];
    return {};
}
bool ui_poll_controls_action(const UiPollProjection& projection, UiPollTarget target,
                             UiPollAction* out) noexcept {
    if (!out || !ui_poll_target_enabled(projection, target))
        return false;
    UiPollActionKind kind{};
    switch (target.control) {
    case UiPollControl::answer:
        kind = counts_visible(projection.state) ? UiPollActionKind::voters : UiPollActionKind::toggle_answer;
        break;
    case UiPollControl::vote:
        kind = UiPollActionKind::vote;
        break;
    case UiPollControl::remove_vote:
        kind = UiPollActionKind::remove_vote;
        break;
    case UiPollControl::results:
        kind = UiPollActionKind::results;
        break;
    case UiPollControl::retry:
        kind = UiPollActionKind::retry;
        break;
    default:
        return false;
    }
    *out = {projection.state.owner, projection.state.generation, projection.state.revision, kind,
            target.answer_id};
    return true;
}
bool ui_poll_action_current(const UiPollState& state, const UiPollAction& action) noexcept {
    if (!state_valid(state) || action.owner != state.owner || action.generation != state.generation ||
        action.revision != state.revision)
        return false;
    if (action.kind != UiPollActionKind::toggle_answer && action.kind != UiPollActionKind::voters &&
        action.answer_id)
        return false;
    const bool mutate = state.supported && !state.loading && !closed(state) && state.can_vote;
    switch (action.kind) {
    case UiPollActionKind::toggle_answer:
        return mutate && !counts_visible(state) && find_answer(state, action.answer_id);
    case UiPollActionKind::vote:
        return mutate && !state.results && selected_count(state) && primary(state) == UiPollControl::vote;
    case UiPollActionKind::remove_vote:
        return state.supported && !state.loading && !closed(state) && state.can_remove_vote &&
               own_vote(state) && primary(state) == UiPollControl::remove_vote;
    case UiPollActionKind::results:
        return state.can_view_results && !state.loading &&
               (primary(state) == UiPollControl::results || show_results(state));
    case UiPollActionKind::voters:
        return counts_visible(state) && state.can_view_voters && !state.loading &&
               find_answer(state, action.answer_id);
    case UiPollActionKind::retry:
        return state.can_retry && !state.loading;
    default:
        return false;
    }
}
bool ui_poll_scene_project(UiPollScene* scene, const UiPollState& state, UiRect bounds, UiRect clip,
                           UiWidgetReference owner, UiPollSourceSurface surface, const UiPollStyle& style,
                           const Theme& theme, UiTextMeasureCache* cache, u16 backend) noexcept {
    if (!scene)
        return false;
    UiPollProjection projection{};
    if (!owner_current(owner) || !ui_rect_valid_or_empty(clip) ||
        static_cast<u32>(surface) > static_cast<u32>(UiPollSourceSurface::forward) ||
        !ui_poll_controls_project(state, bounds, style, theme, cache, backend, &projection)) {
        *scene = {};
        return false;
    }
    const auto effective = ui_rect_intersection(projection.geometry.bounds, clip);
    if (!ui_rect_has_area(effective)) {
        *scene = {};
        return false;
    }
    const bool same = scene->active && scene->owner.manager == owner.manager &&
                      scene->owner.handle.slot == owner.handle.slot &&
                      scene->owner.handle.generation == owner.handle.generation &&
                      scene->surface == surface && scene->projection.state.owner == state.owner &&
                      scene->projection.state.generation == state.generation;
    const bool capture_current = same && scene->projection.state.revision == state.revision &&
                                 ui_rect_equal(scene->clip, effective) &&
                                 ui_rect_equal(ui_poll_target_rectangle(scene->projection, scene->capture),
                                               ui_poll_target_rectangle(projection, scene->capture));
    const auto focus = scene->focus, hover = scene->hover, capture = scene->capture;
    const bool keyboard = scene->keyboard_focus;
    *scene = {};
    scene->projection = projection;
    scene->style = style;
    scene->owner = owner;
    scene->surface = surface;
    scene->clip = effective;
    scene->active = true;
    if (same && eligible(*scene, focus)) {
        scene->focus = focus;
        scene->keyboard_focus = keyboard;
    }
    if (same && eligible(*scene, hover))
        scene->hover = hover;
    if (capture_current && eligible(*scene, capture))
        scene->capture = capture;
    return true;
}
UiPollTarget ui_poll_scene_hit(const UiPollScene& scene, UiPoint point) noexcept {
    return scene_current(scene) && contains(scene.clip, point)
               ? ui_poll_controls_hit(scene.projection, point)
               : UiPollTarget{};
}
void ui_poll_scene_blur(UiPollScene* scene) noexcept {
    if (scene) {
        scene->focus = {};
        scene->capture = {};
        scene->keyboard_focus = false;
    }
}
UiPollSceneResult ui_poll_scene_pointer(UiPollScene* scene, UiPollPointerEvent event,
                                        UiPoint point) noexcept {
    UiPollSceneResult result{};
    if (!scene)
        return result;
    if (event == UiPollPointerEvent::cancel || event == UiPollPointerEvent::leave ||
        !scene_current(*scene)) {
        result.release_capture = scene->capture.control != UiPollControl::none;
        scene->capture = {};
        scene->hover = {};
        return result;
    }
    const auto hit = ui_poll_scene_hit(*scene, point);
    scene->hover = hit;
    if (event == UiPollPointerEvent::down && hit.control != UiPollControl::none) {
        scene->capture = hit;
        scene->focus = hit;
        scene->keyboard_focus = false;
        result.handled = result.request_capture = true;
    } else if (event == UiPollPointerEvent::up && scene->capture.control != UiPollControl::none) {
        result.handled = result.release_capture = true;
        if (ui_poll_target_equal(hit, scene->capture))
            result.action_requested = ui_poll_controls_action(scene->projection, hit, &result.action);
        scene->capture = {};
    } else
        result.handled =
            hit.control != UiPollControl::none || scene->capture.control != UiPollControl::none;
    return result;
}
UiPollSceneResult ui_poll_scene_key(UiPollScene* scene, UiPollKey key) noexcept {
    UiPollSceneResult result{};
    if (!scene || !scene_current(*scene))
        return result;
    if (key == UiPollKey::activate) {
        result.handled = eligible(*scene, scene->focus);
        result.action_requested =
            result.handled && ui_poll_controls_action(scene->projection, scene->focus, &result.action);
        return result;
    }
    UiPollTarget all[kUiPollMaximumAnswers + 4]{}, enabled[kUiPollMaximumAnswers + 4]{};
    const auto count = targets(scene->projection, all);
    u32 available{}, current = ~0u;
    for (u32 i = 0; i < count; ++i)
        if (eligible(*scene, all[i])) {
            if (ui_poll_target_equal(all[i], scene->focus))
                current = available;
            enabled[available++] = all[i];
        }
    if (!available)
        return result;
    u32 next = key == UiPollKey::last || key == UiPollKey::previous ? available - 1 : 0;
    if (current < available && (key == UiPollKey::next || key == UiPollKey::previous))
        next = key == UiPollKey::next ? (current + 1) % available : (current + available - 1) % available;
    scene->focus = enabled[next];
    scene->keyboard_focus = true;
    result.handled = true;
    return result;
}
} // namespace maicord
