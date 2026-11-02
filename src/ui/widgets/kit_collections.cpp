#include "maicord/ui/widgets/kit.hpp"

namespace maicord {
namespace {
constexpr float kIconSmall = 14, kIconGap = 6, kDot = 8, kDotGap = 6, kGroupGap = 17, kDividerHeight = 20;
float pill_radius(const UiRect& r) noexcept {
    return ui_rect_height(r) * .5f;
}
UiRect centered(const UiRect& r, float width, float height) noexcept {
    const float cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f;
    return {cx - width * .5f, cy - height * .5f, cx + width * .5f, cy + height * .5f};
}
bool risk(UiChipTone tone) noexcept {
    return tone == UiChipTone::risk_low || tone == UiChipTone::risk_medium || tone == UiChipTone::risk_high;
}
UiRect rect_at(float left, float top, float width, float height) noexcept {
    return {left, top, left + width, top + height};
}
} // namespace

// ---------------------------------------------------------------------------
bool ui_card_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& card, UiControlState state) noexcept {
    const auto& c = t.color;
    const float alpha = state.enabled ? 1.f : t.disabled_alpha, radius = t.metric.card_radius;
    const Color fill = state.pressed ? c.mod_strong : state.hovered ? c.mod_normal : c.card_fill;
    bool ok = paint_stream_push_rounded_border(paint, card, ui_settings_color_alpha(fill, alpha),
                                               ui_settings_color_alpha(state.hovered ? c.border_normal : c.card_border, alpha),
                                               radius, 1);
    if (ok && state.focused)
        ok = ui_settings_focus_paint(paint, t, card, radius);
    return ok;
}
bool ui_card_hairline_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& row) noexcept {
    return paint_stream_push(paint, row.left + t.metric.card_inset, row.bottom - 1, row.right - t.metric.card_inset,
                             row.bottom, t.color.hairline);
}
UiRect ui_card_content(const UiSettingsLayoutTokens& t, const UiRect& card) noexcept {
    return {card.left + t.metric.card_inset, card.top + t.metric.card_inset, card.right - t.metric.card_inset,
            card.bottom - t.metric.card_inset};
}

UiCardGrid ui_card_grid(const UiSettingsLayoutTokens& t, float width, u32 count, float card_height) noexcept {
    UiCardGrid g{};
    g.gap = t.metric.card_gap;
    g.count = count;
    g.card_height = card_height;
    const float fit = (width + g.gap) / (kUiKitCardMinimum + g.gap);
    g.columns = fit < 1 ? 1 : fit > kUiKitColumnsMaximum ? static_cast<u32>(kUiKitColumnsMaximum) : static_cast<u32>(fit);
    g.card_width = (width - g.gap * static_cast<float>(g.columns - 1)) / static_cast<float>(g.columns);
    g.rows = (count + g.columns - 1) / g.columns;
    g.height = g.rows ? static_cast<float>(g.rows) * (card_height + g.gap) - g.gap : 0;
    return g;
}
UiRect ui_card_grid_rect(const UiCardGrid& g, float left, float top, u32 index) noexcept {
    if (index >= g.count)
        return {};
    const u32 row = index / g.columns, column = index % g.columns;
    return rect_at(left + static_cast<float>(column) * (g.card_width + g.gap),
                   top + static_cast<float>(row) * (g.card_height + g.gap), g.card_width, g.card_height);
}
void ui_card_grid_visible(const UiCardGrid& g, float view_top, float view_height, u32* first, u32* end) noexcept {
    const float pitch = g.card_height + g.gap;
    const float top = view_top / pitch, bottom = (view_top + view_height) / pitch;
    const u32 first_row = top < 1 ? 0 : static_cast<u32>(top) - 1;
    const u32 end_row = bottom < 0 ? 1 : minimum(g.rows, static_cast<u32>(bottom) + 2);
    *first = minimum(g.count, first_row * g.columns);
    *end = minimum(g.count, end_row * g.columns);
}
u32 ui_card_grid_hit(const UiCardGrid& g, float x, float y) noexcept {
    if (x < 0 || y < 0 || !g.columns)
        return ~0u;
    const float pitch_x = g.card_width + g.gap, pitch_y = g.card_height + g.gap;
    const u32 column = static_cast<u32>(x / pitch_x), row = static_cast<u32>(y / pitch_y);
    if (column >= g.columns || x - static_cast<float>(column) * pitch_x >= g.card_width ||
        y - static_cast<float>(row) * pitch_y >= g.card_height)
        return ~0u;
    const u32 index = row * g.columns + column;
    return index < g.count ? index : ~0u;
}
u32 ui_card_grid_move(const UiCardGrid& g, u32 index, UiKitKey key, u32 page_rows) noexcept {
    if (!g.count)
        return index;
    if (index >= g.count)
        return 0;
    const u32 row_start = index - index % g.columns, last = g.count - 1;
    switch (key) {
    case UiKitKey::left:
        return index ? index - 1 : 0;
    case UiKitKey::right:
        return minimum(index + 1, last);
    case UiKitKey::up:
        return index >= g.columns ? index - g.columns : index;
    case UiKitKey::down:
        return index + g.columns <= last ? index + g.columns : last - last % g.columns > row_start ? last : index;
    case UiKitKey::home:
        return row_start;
    case UiKitKey::end:
        return minimum(row_start + g.columns - 1, last);
    case UiKitKey::ctrl_home:
        return 0;
    case UiKitKey::ctrl_end:
        return last;
    case UiKitKey::page_up:
        return index >= page_rows * g.columns ? index - page_rows * g.columns : index % g.columns;
    case UiKitKey::page_down: {
        const u32 next = index + page_rows * g.columns;
        return next <= last ? next : last;
    }
    default:
        return index;
    }
}

// ---------------------------------------------------------------------------
float ui_chip_width(const UiSettingsLayoutTokens&, float label_width, UiChipTone tone, bool icon) noexcept {
    return kUiKitChipPadding * 2 + (risk(tone) ? kDot + kDotGap : 0) + (icon ? kIconSmall + kIconGap : 0) + label_width;
}
UiRect ui_chip_label_rect(const UiSettingsLayoutTokens&, const UiRect& chip, UiChipTone tone, bool icon) noexcept {
    const float left = chip.left + kUiKitChipPadding + (risk(tone) ? kDot + kDotGap : 0) + (icon ? kIconSmall + kIconGap : 0);
    return {left, chip.top, maximum(left, chip.right - kUiKitChipPadding), chip.bottom};
}
Color ui_chip_ink(const UiSettingsTokens& t, UiChipTone tone) noexcept {
    const auto& c = t.color;
    switch (tone) {
    case UiChipTone::neutral:
    case UiChipTone::subtle:
        return c.text_subtle;
    case UiChipTone::positive:
        return c.positive;
    case UiChipTone::warning:
        return c.warning;
    case UiChipTone::dev:
        return c.critical_text;
    case UiChipTone::brand:
        return c.on_brand;
    default:
        return c.text_default;
    }
}
bool ui_chip_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& chip, UiChipTone tone,
                   UiIcon icon) noexcept {
    const auto& c = t.color;
    const Color fill = tone == UiChipTone::positive ? c.positive_tint
                       : tone == UiChipTone::warning ? c.warning_tint
                       : tone == UiChipTone::dev     ? c.critical_tint
                       : tone == UiChipTone::brand   ? c.brand
                                                     : c.mod_subtle;
    bool ok = tone == UiChipTone::mono ? paint_stream_push_rounded_border(paint, chip, fill, c.border_subtle, pill_radius(chip), 1)
                                       : paint_stream_push_rounded(paint, chip, fill, pill_radius(chip));
    const float cy = (chip.top + chip.bottom) * .5f;
    float x = chip.left + kUiKitChipPadding;
    if (ok && risk(tone)) {
        const Color dot = tone == UiChipTone::risk_low ? c.risk_low : tone == UiChipTone::risk_medium ? c.risk_medium : c.risk_high;
        ok = paint_stream_push_rounded(paint, {x, cy - kDot * .5f, x + kDot, cy + kDot * .5f}, dot, kDot * .5f);
        x += kDot + kDotGap;
    }
    if (ok && icon != UiIcon::count)
        ok = ui_icon_paint(paint, icon, {x, cy - kIconSmall * .5f, x + kIconSmall, cy + kIconSmall * .5f}, ui_chip_ink(t, tone));
    return ok;
}
float ui_filter_chip_width(const UiSettingsLayoutTokens&, float label_width, float count_width, bool icon,
                           bool selected) noexcept {
    return kUiKitFilterChipPadding * 2 + (icon ? kIconSmall + kIconGap : 0) + (selected ? kIconSmall + kIconGap : 0) +
           label_width + (count_width > 0 ? 4 + count_width : 0);
}
UiRect ui_filter_chip_label_rect(const UiSettingsLayoutTokens&, const UiRect& chip, bool icon, bool selected) noexcept {
    const float left = chip.left + kUiKitFilterChipPadding + (icon ? kIconSmall + kIconGap : 0) +
                       (selected ? kIconSmall + kIconGap : 0);
    return {left, chip.top, maximum(left, chip.right - kUiKitFilterChipPadding), chip.bottom};
}
bool ui_filter_chip_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& chip, bool selected,
                          UiControlState state) noexcept {
    const auto& c = t.color;
    const float alpha = state.enabled ? 1.f : t.disabled_alpha, radius = pill_radius(chip);
    const Color fill = selected ? c.emphasis_selected : state.pressed ? c.mod_strong : state.hovered ? c.mod_normal : c.mod_subtle;
    bool ok = paint_stream_push_rounded_border(paint, chip, ui_settings_color_alpha(fill, alpha),
                                               ui_settings_color_alpha(selected ? c.emphasis_outline : c.border_subtle, alpha),
                                               radius, 1);
    if (ok && selected) {
        const float cy = (chip.top + chip.bottom) * .5f, x = chip.left + kUiKitFilterChipPadding;
        ok = ui_icon_paint(paint, UiIcon::check, {x, cy - kIconSmall * .5f, x + kIconSmall, cy + kIconSmall * .5f},
                           ui_settings_color_alpha(c.text_strong, alpha));
    }
    if (ok && state.focused)
        ok = ui_settings_focus_paint(paint, t, chip, radius);
    return ok;
}
float ui_chip_strip_place(const UiSettingsLayoutTokens&, const float* widths, const u8* groups, u32 count, float width,
                          bool wrap, float scroll, UiRect* out, UiRect* dividers) noexcept {
    float x = 0, y = 0;
    for (u32 i = 0; i < count; ++i) {
        const bool grouped = i && groups && groups[i] != groups[i - 1];
        float lead = i ? (grouped ? kGroupGap : kUiKitChipGap) : 0;
        bool divider = grouped;
        if (wrap && i && x + lead + widths[i] > width) {
            x = 0;
            y += kUiKitFilterChipHeight + kUiKitChipGap;
            lead = 0;
            divider = false;
        }
        x += lead;
        out[i] = rect_at(x - scroll, y, widths[i], kUiKitFilterChipHeight);
        if (dividers)
            dividers[i] = divider ? rect_at(x - scroll - kGroupGap * .5f - .5f, y + (kUiKitFilterChipHeight - kDividerHeight) * .5f, 1,
                                            kDividerHeight)
                                  : UiRect{};
        x += widths[i];
    }
    return count ? y + kUiKitFilterChipHeight : 0;
}
bool ui_chip_divider_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& divider) noexcept {
    return !ui_rect_has_area(divider) || paint_stream_push(paint, divider, t.color.hairline);
}
u32 ui_chip_strip_hit(const UiRect* rects, u32 count, float x, float y) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (ui_rect_contains(rects[i], x, y))
            return i;
    return ~0u;
}
u32 ui_chip_strip_move(const UiRect* rects, u32 count, u32 index, UiKitKey key) noexcept {
    if (!count)
        return index;
    if (index >= count)
        return 0;
    switch (key) {
    case UiKitKey::left:
        return index ? index - 1 : 0;
    case UiKitKey::right:
        return minimum(index + 1, count - 1);
    case UiKitKey::home:
    case UiKitKey::ctrl_home:
        return 0;
    case UiKitKey::end:
    case UiKitKey::ctrl_end:
        return count - 1;
    case UiKitKey::up:
    case UiKitKey::down: {
        // The nearest row above or below, then the chip whose centre is nearest to this one's.
        const bool up = key == UiKitKey::up;
        const float top = rects[index].top, middle = (rects[index].left + rects[index].right) * .5f;
        float row = up ? -kMaximumUiCoordinate : kMaximumUiCoordinate;
        for (u32 i = 0; i < count; ++i)
            if (up ? rects[i].top < top && rects[i].top > row : rects[i].top > top && rects[i].top < row)
                row = rects[i].top;
        if (row == kMaximumUiCoordinate || row == -kMaximumUiCoordinate)
            return index;
        u32 best = index;
        float distance = kMaximumUiCoordinate;
        for (u32 i = 0; i < count; ++i) {
            if (rects[i].top != row)
                continue;
            const float d = (rects[i].left + rects[i].right) * .5f - middle, a = d < 0 ? -d : d;
            if (a < distance)
                distance = a, best = i;
        }
        return best;
    }
    default:
        return index;
    }
}
float ui_chip_strip_reveal(const UiRect* rects, u32 index, float width, float scroll) noexcept {
    const auto& r = rects[index];
    return r.left < 0 ? scroll + r.left : r.right > width ? scroll + r.right - width : scroll;
}
u64 ui_chip_toggle(u64 mask, const u8* groups, u32 count, u32 index, bool is_all) noexcept {
    if (index >= count || count > 64)
        return mask;
    if (!is_all)
        return mask ^ (u64{1} << index);
    for (u32 i = 0; i < count; ++i)
        if (!groups || groups[i] == groups[index])
            mask &= ~(u64{1} << i);
    return mask;
}
bool ui_chip_filter_matches(u64 selected, u64 entry, const u8* groups, u32 count) noexcept {
    for (u32 i = 0; i < count && i < 64; ++i) {
        bool first = true;
        for (u32 j = 0; j < i && first; ++j)
            first = !groups || groups[j] != groups[i];
        if (!first)
            continue;
        u64 members = 0;
        for (u32 j = i; j < count && j < 64; ++j)
            if (!groups || groups[j] == groups[i])
                members |= u64{1} << j;
        const u64 chosen = selected & members;
        if (chosen && !(entry & chosen))
            return false;
    }
    return true;
}
float ui_count_badge_width(const UiSettingsLayoutTokens&, float digits_width) noexcept {
    return maximum(kUiKitBadgeHeight, digits_width + 10);
}
bool ui_count_badge_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& badge, bool attention) noexcept {
    return paint_stream_push_rounded(paint, badge, attention ? t.color.brand : t.color.mod_strong, pill_radius(badge));
}

// ---------------------------------------------------------------------------
float ui_tab_width(const UiSettingsLayoutTokens&, float label_width, float badge_width) noexcept {
    return label_width + (badge_width > 0 ? 6 + badge_width : 0);
}
bool ui_tab_bar_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& bar, const UiRect& selected_tab,
                      bool focused) noexcept {
    const auto& c = t.color;
    bool ok = paint_stream_push(paint, bar.left, bar.bottom - 1, bar.right, bar.bottom, c.separator);
    if (ok && ui_rect_has_area(selected_tab))
        ok = paint_stream_push_rounded(paint, selected_tab.left, bar.bottom - 2, selected_tab.right, bar.bottom, c.brand,
                                       t.metric.control_radius * .125f);
    if (ok && focused && ui_rect_has_area(selected_tab))
        ok = ui_settings_focus_paint(paint, t, {selected_tab.left - 4, selected_tab.top + 8, selected_tab.right + 4, selected_tab.bottom - 8},
                                     t.metric.control_radius * .5f);
    return ok;
}

// ---------------------------------------------------------------------------
bool ui_glyph_tile_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& tile, UiIcon icon,
                         UiTileTone tone) noexcept {
    const auto& c = t.color;
    const float size = ui_rect_width(tile), radius = size >= 56 ? t.metric.card_radius : t.metric.control_radius;
    const Color fill = tone == UiTileTone::selected ? c.emphasis_selected : tone == UiTileTone::dev ? c.critical_tint : c.mod_normal;
    const Color ink = tone == UiTileTone::selected ? c.emphasis_outline : tone == UiTileTone::dev ? c.critical_text : c.text_default;
    bool ok = tone == UiTileTone::selected
                  ? paint_stream_push_rounded_border(paint, tile, fill, ui_settings_color_alpha(c.emphasis_outline, .25f), radius, 1)
                  : paint_stream_push_rounded(paint, tile, fill, radius);
    const float glyph = maximum(16.f, size * .5f);
    return ok && ui_icon_paint(paint, icon, centered(tile, glyph, glyph), ink);
}
UiRect ui_preview_rect(const UiRect& area, UiPreviewAspect aspect) noexcept {
    const float width = ui_rect_width(area), height = width * (aspect == UiPreviewAspect::r16x10 ? 10.f / 16.f : 9.f / 16.f);
    return {area.left, area.top, area.right, area.top + height};
}
bool ui_preview_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& tile, float top_radius,
                      UiPreviewState state) noexcept {
    if (state == UiPreviewState::ready)
        return true;
    const auto& c = t.color;
    // Only the top corners round: the plate overshoots below and the clip trims it.
    bool ok = paint_stream_push_clip(paint, tile);
    ok = ok && paint_stream_push_rounded(paint, tile.left, tile.top, tile.right, tile.bottom + top_radius, c.placeholder, top_radius);
    ok = paint_stream_pop_clip(paint) && ok;
    ok = ok && paint_stream_push(paint, tile.left, tile.bottom - 1, tile.right, tile.bottom, c.border_subtle);
    if (ok && state == UiPreviewState::failed)
        ok = ui_icon_paint(paint, UiIcon::image_off, centered(tile, 24, 24), c.text_subtle);
    return ok;
}
bool ui_pill_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& pill) noexcept {
    return paint_stream_push_rounded(paint, pill, t.color.pill, pill_radius(pill));
}

void ui_preview_plan(UiPreviewResidency* residency, const u64* visible, u32 visible_count, const u64* ahead,
                     u32 ahead_count, UiPreviewPlan* plan) noexcept {
    *plan = {};
    ++residency->clock;
    const auto find = [&](u64 key) noexcept -> u32 {
        for (u32 i = 0; i < residency->count; ++i)
            if (residency->keys[i] == key)
                return i;
        return ~0u;
    };
    const auto wanted = [&](u64 key) noexcept {
        for (u32 i = 0; i < visible_count; ++i)
            if (visible[i] == key)
                return true;
        for (u32 i = 0; i < ahead_count; ++i)
            if (ahead[i] == key)
                return true;
        return false;
    };
    for (u32 pass = 0; pass < 2; ++pass) {
        const u64* keys = pass ? ahead : visible;
        const u32 n = pass ? ahead_count : visible_count;
        for (u32 i = 0; i < n; ++i) {
            if (!keys[i])
                continue;
            const u32 at = find(keys[i]);
            if (at != ~0u) {
                residency->stamps[at] = residency->clock;
                continue;
            }
            bool queued = false;
            for (u32 j = 0; j < plan->request_count; ++j)
                queued = queued || plan->request[j] == keys[i];
            if (!queued && plan->request_count < kUiPreviewRequests)
                plan->request[plan->request_count++] = keys[i];
        }
    }
    // Make room for the requests: unwanted residents go, least recently seen first.
    while (residency->count + plan->request_count > kUiPreviewResident) {
        u32 oldest = ~0u;
        for (u32 i = 0; i < residency->count; ++i)
            if (!wanted(residency->keys[i]) && (oldest == ~0u || residency->stamps[i] < residency->stamps[oldest]))
                oldest = i;
        if (oldest == ~0u)
            break;
        plan->evict[plan->evict_count++] = residency->keys[oldest];
        --residency->count;
        residency->keys[oldest] = residency->keys[residency->count];
        residency->stamps[oldest] = residency->stamps[residency->count];
    }
}
void ui_preview_resident(UiPreviewResidency* residency, u64 key, bool ready) noexcept {
    u32 at = ~0u;
    for (u32 i = 0; i < residency->count; ++i)
        if (residency->keys[i] == key)
            at = i;
    if (!ready) {
        if (at != ~0u) {
            --residency->count;
            residency->keys[at] = residency->keys[residency->count];
            residency->stamps[at] = residency->stamps[residency->count];
        }
        return;
    }
    if (at == ~0u) {
        if (residency->count == kUiPreviewResident) {
            // A host that ignored the plan: the least recently seen slot is reused.
            at = 0;
            for (u32 i = 1; i < residency->count; ++i)
                if (residency->stamps[i] < residency->stamps[at])
                    at = i;
        } else
            at = residency->count++;
        residency->keys[at] = key;
    }
    residency->stamps[at] = residency->clock;
}
bool ui_preview_loop_update(UiPreviewLoop* loop, u64 hovered_key, u64 focused_key, bool reduced_motion,
                            u64 now_ms) noexcept {
    const u64 key = focused_key ? focused_key : hovered_key;
    loop->reduced = reduced_motion;
    if (key != loop->key) {
        loop->key = key;
        loop->since_ms = now_ms;
        loop->manual = false;
    }
    loop->playing = key && (reduced_motion ? loop->manual
                                           : focused_key == key || now_ms - loop->since_ms >= kUiPreviewLoopDelayMs);
    return loop->playing;
}
u64 ui_preview_loop_next_wake(const UiPreviewLoop& loop) noexcept {
    return loop.key && !loop.playing && !loop.reduced ? loop.since_ms + kUiPreviewLoopDelayMs : 0;
}
void ui_preview_loop_toggle(UiPreviewLoop* loop, u64 key) noexcept {
    if (key != loop->key) {
        loop->key = key;
        loop->manual = true;
    } else
        loop->manual = !loop->manual;
    loop->playing = loop->manual || loop->playing;
}

// ---------------------------------------------------------------------------
float ui_skeleton_pulse(u64 now_ms, bool reduced_motion) noexcept {
    if (reduced_motion)
        return 0;
    const float phase = static_cast<float>(now_ms % kUiSkeletonPeriodMs) / static_cast<float>(kUiSkeletonPeriodMs);
    const float tri = phase < .5f ? phase * 2 : 2 - phase * 2;
    return tri * tri * (3 - 2 * tri);
}
u64 ui_skeleton_next_wake(u64 now_ms, bool reduced_motion) noexcept {
    return reduced_motion ? 0 : now_ms + 33;
}
bool ui_skeleton_bar_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& bar, float pulse,
                           bool strong) noexcept {
    const auto& c = t.color;
    return paint_stream_push_rounded(
        paint, bar, strong ? ui_settings_color_mix(c.mod_strong, c.mod_normal, pulse) : ui_settings_color_mix(c.mod_normal, c.mod_strong, pulse),
        t.metric.control_radius * .5f);
}
float ui_skeleton_card_height(const UiSettingsLayoutTokens&, UiSkeletonCard kind, float card_width) noexcept {
    return kind == UiSkeletonCard::plugin ? 168.f
           : kind == UiSkeletonCard::theme ? card_width * 10.f / 16.f + 136.f
                                           : card_width * 9.f / 16.f + 136.f;
}
bool ui_skeleton_card_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& card, UiSkeletonCard kind,
                            float pulse) noexcept {
    const float pad = t.metric.card_inset, width = ui_rect_width(card) - pad * 2;
    bool ok = ui_card_paint(paint, t, card, {});
    float top = card.top + pad;
    const float left = card.left + pad;
    const auto bar = [&](float x, float y, float w, float h, bool strong = false) noexcept {
        ok = ok && ui_skeleton_bar_paint(paint, t, rect_at(x, y, w, h), pulse, strong);
    };
    if (kind == UiSkeletonCard::plugin) {
        const float tile = 40, text = left + tile + t.metric.lead_gap, text_width = width - tile - t.metric.lead_gap;
        bar(left, top, tile, tile);
        bar(text, top + 4, text_width * .55f, 16);
        bar(text, top + 26, text_width * .35f, 12);
        top += tile + t.metric.lead_gap;
    } else {
        const auto preview = ui_preview_rect(card, kind == UiSkeletonCard::theme ? UiPreviewAspect::r16x10 : UiPreviewAspect::r16x9);
        ok = ok && ui_preview_paint(paint, t, preview, t.metric.card_radius, UiPreviewState::loading);
        top = preview.bottom + pad;
        bar(left, top, width * .55f, 16);
        bar(left, top + 22, width * .35f, 12);
        top += 22 + 12 + t.metric.lead_gap;
    }
    bar(left, top, width * .92f, 12);
    bar(left, top + 18, width * .7f, 12);
    const float footer = card.bottom - pad - kUiKitFilterChipHeight;
    bar(left, footer + 4, 72, kUiKitChipHeight);
    bar(left + 80, footer + 4, 56, kUiKitChipHeight);
    bar(card.right - pad - 64, footer, 64, kUiKitFilterChipHeight, true);
    return ok;
}

} // namespace maicord
