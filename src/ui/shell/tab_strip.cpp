#include "maicord/ui/shell/tab_strip.hpp"

#include "maicord/core/hash.hpp"

namespace maicord {
namespace {

// A tab: padding, icon, label, close button, padding. Narrower than kNarrow the icon centres and the close button
// takes its place.
constexpr float kPad = 8.f, kGap = 6.f, kIcon = 16.f, kClose = 20.f, kNarrow = 56.f;
// The label's room beside the icon and the close button; a "+" or "..." fill is a square this far inside its box.
constexpr float kLabelLeft = kPad + kIcon + kGap, kLabelRight = kGap + kClose + kGap, kButtonInset = 2.f;
// Dimmed ink: an unreachable channel, a disabled "+".
constexpr float kDim = .5f;

float snap(float value, float scale) noexcept {
    const float pixels = value * scale;
    return static_cast<float>(static_cast<i32>(pixels + (pixels < 0 ? -.5f : .5f))) / scale;
}

Color mix(const Color& a, const Color& b, float t) noexcept {
    return {a.red + (b.red - a.red) * t, a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t,
            a.alpha + (b.alpha - a.alpha) * t};
}

bool inside(const UiRect& r, float x, float y) noexcept {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// A `size` square centred on `x` (centre) and the area's middle.
UiRect centred(const UiTabStripLayout& layout, float x, float size) noexcept {
    const float top = (layout.area.top + layout.area.bottom - size) * .5f;
    return {x - size * .5f, top, x + size * .5f, top + size};
}

bool narrow(const UiTabStripLayout& layout, u32 index) noexcept {
    return layout.width[index] < kNarrow;
}

bool closable(const UiTabStripLayout& layout, u32 index) noexcept {
    return (layout.closable >> index) & 1;
}

UiRect snapped(const UiRect& r, float scale) noexcept {
    return {snap(r.left, scale), snap(r.top, scale), snap(r.right, scale), snap(r.bottom, scale)};
}

} // namespace

UiTabStripLayout ui_tab_strip_layout(const UiRect& area, const UiTabStripView& view) noexcept {
    UiTabStripLayout layout{};
    layout.area = area;
    const float room = area.right - area.left;
    if (!(room >= kUiTabButton + kUiTabMinimum))
        return layout;
    const u32 count = minimum<u32>(view.count, kUiTabCapacity);
    // The live items (not closing) and the active one's rank among them.
    u32 live = 0, active = ~0u;
    for (u32 i = 0; i < count; ++i) {
        const u8 flags = view.items[i].flags;
        if (flags & kUiTabClosing)
            continue;
        if (flags & kUiTabActive)
            active = live;
        ++live;
    }
    // The active tab keeps its own minimum while the others shrink to theirs.
    const float wide = active != ~0u ? kUiTabActiveMinimum - kUiTabMinimum : 0.0f;
    layout.overflowed = static_cast<float>(live) * kUiTabMinimum + wide + kUiTabButton > room;
    const float space = room - kUiTabButton * (layout.overflowed ? 2 : 1);
    u32 fit = live;
    if (layout.overflowed)
        fit = minimum(live, maximum(1u, static_cast<u32>(maximum(0.0f, (space - wide) / kUiTabMinimum))));
    layout.shown = static_cast<u8>(fit);
    float width = fit ? minimum(kUiTabWidth, space / static_cast<float>(fit)) : 0.0f;
    float active_width = width;
    if (active != ~0u && fit > 1 && width < kUiTabActiveMinimum) {
        active_width = kUiTabActiveMinimum;
        width = (space - active_width) / static_cast<float>(fit - 1);
    }
    // The first items draw; when the active one lies beyond them it takes the last slot.
    const u32 beyond = active != ~0u && active >= fit ? 1u : 0u;
    float x = area.left;
    u32 rank = 0;
    for (u32 i = 0; i < count; ++i) {
        float slot = 0;
        if (!(view.items[i].flags & kUiTabClosing)) {
            const bool is_active = rank == active;
            if (is_active || rank + beyond < fit)
                slot = is_active ? active_width : width;
            ++rank;
        }
        layout.slot_left[i] = x;
        layout.slot_width[i] = slot;
        x += slot;
    }
    // The transition moves every tab from where it was; a dragging one follows the pointer within the tabs.
    const float remaining = minimum(maximum(view.remaining, 0.0f), 1.0f);
    float right = area.left;
    for (u32 i = 0; i < count; ++i) {
        const UiTabItem& item = view.items[i];
        const bool opening = item.flags & kUiTabOpening;
        float left = layout.slot_left[i], extent = layout.slot_width[i];
        left += ((opening ? left : item.from_left) - left) * remaining;
        extent += ((opening ? 0.0f : item.from_width) - extent) * remaining;
        if (item.flags & kUiTabDragging) {
            extent = layout.slot_width[i];
            left = minimum(maximum(view.drag_left, area.left), maximum(area.left, x - extent));
        }
        layout.left[i] = left;
        layout.width[i] = maximum(extent, 0.0f);
        layout.fade[i] = opening ? 1 - remaining : (item.flags & kUiTabClosing) ? remaining : 1.0f;
        right = maximum(right, left + layout.width[i]);
        const bool hovered = (view.hovered.kind == UiTabHitKind::tab || view.hovered.kind == UiTabHitKind::close) &&
                             view.hovered.index == i;
        // The active tab and the wide ones always show their close button; the others while the pointer is on them
        // (their hover fill fades it in and out).
        if (!(item.flags & kUiTabClosing) && layout.width[i] >= kUiTabMinimum &&
            ((item.flags & kUiTabActive) || layout.slot_width[i] >= kUiTabCloseAlways || hovered || item.hover > 0))
            layout.closable |= 1u << i;
    }
    // The buttons follow the tabs, the last at the strip's edge when everything does not fit.
    const float plus = minimum(right, area.right - kUiTabButton * (layout.overflowed ? 2 : 1));
    layout.plus = {plus, area.top, plus + kUiTabButton, area.bottom};
    if (layout.overflowed)
        layout.overflow = {layout.plus.right, area.top, layout.plus.right + kUiTabButton, area.bottom};
    return layout;
}

UiRect ui_tab_strip_tab(const UiTabStripLayout& layout, u32 index) noexcept {
    return layout.width[index] > 0
               ? UiRect{layout.left[index], layout.area.top, layout.left[index] + layout.width[index],
                        layout.area.bottom}
               : UiRect{};
}

UiRect ui_tab_strip_icon(const UiTabStripLayout& layout, u32 index) noexcept {
    const UiRect tab = ui_tab_strip_tab(layout, index);
    if (!ui_rect_has_area(tab) || (narrow(layout, index) && closable(layout, index)))
        return {};
    return centred(layout, narrow(layout, index) ? (tab.left + tab.right) * .5f : tab.left + kPad + kIcon * .5f,
                   kIcon);
}

UiRect ui_tab_strip_close(const UiTabStripLayout& layout, u32 index) noexcept {
    const UiRect tab = ui_tab_strip_tab(layout, index);
    if (!ui_rect_has_area(tab) || !closable(layout, index))
        return {};
    return centred(layout, narrow(layout, index) ? (tab.left + tab.right) * .5f : tab.right - kGap - kClose * .5f,
                   kClose);
}

UiTabLabel ui_tab_strip_label(const UiTabStripLayout& layout, u32 index) noexcept {
    if (layout.slot_width[index] < kUiTabLabelMinimum)
        return {};
    return {layout.left[index] + kLabelLeft,
            layout.slot_width[index] - kLabelLeft - (closable(layout, index) ? kLabelRight : kPad)};
}

Color ui_tab_strip_ink(const Theme& theme, const UiTabStripView& view, const UiTabStripLayout& layout, u32 index,
                       bool window_active) noexcept {
    const auto& tokens = theme.tokens.caption;
    const UiTabItem& item = view.items[index];
    Color ink = mix(window_active ? tokens.icon : tokens.icon_inactive,
                    window_active ? tokens.text : tokens.text_inactive, maximum(item.hover, item.select));
    ink.alpha *= layout.fade[index] * ((item.flags & kUiTabMissing) ? kDim : 1.0f);
    return ink;
}

UiTabHit ui_tab_strip_hit(const UiTabStripLayout& layout, float x, float y) noexcept {
    if (!inside(layout.area, x, y))
        return {};
    if (inside(layout.plus, x, y))
        return {UiTabHitKind::plus, 0};
    if (inside(layout.overflow, x, y))
        return {UiTabHitKind::overflow, 0};
    for (u32 i = kUiTabCapacity; i-- > 0;) {
        // Closing and hidden tabs take no input, whatever is left of them mid-transition.
        if (!(layout.slot_width[i] > 0) || !inside(ui_tab_strip_tab(layout, i), x, y))
            continue;
        const UiRect close = ui_tab_strip_close(layout, i);
        const bool on_close = ui_rect_has_area(close) && x >= close.left - kGap * .5f && x < close.right + kGap * .5f;
        return {on_close ? UiTabHitKind::close : UiTabHitKind::tab, static_cast<u8>(i)};
    }
    return {};
}

UiRect ui_tab_strip_hit_rect(const UiTabStripLayout& layout, UiTabHit hit) noexcept {
    switch (hit.kind) {
    case UiTabHitKind::tab:
        return ui_tab_strip_tab(layout, hit.index);
    case UiTabHitKind::close:
        return ui_tab_strip_close(layout, hit.index);
    case UiTabHitKind::plus:
        return layout.plus;
    case UiTabHitKind::overflow:
        return layout.overflow;
    default:
        return {};
    }
}

Utf8View ui_tab_strip_hit_name(const UiTabStripView& view, UiTabHit hit) noexcept {
    switch (hit.kind) {
    case UiTabHitKind::tab:
        return hit.index < view.count ? view.items[hit.index].label : Utf8View{};
    case UiTabHitKind::close:
        return utf8_literal("Close tab");
    case UiTabHitKind::plus:
        return utf8_literal("Pin this channel");
    case UiTabHitKind::overflow:
        return utf8_literal("All tabs");
    default:
        return {};
    }
}

u32 ui_tab_strip_drag_slot(const UiTabStripLayout& layout, u32 dragged, float x) noexcept {
    // The dragged tab lands before the first other tab whose midpoint lies right of its centre, else after the last.
    u32 last = dragged;
    for (u32 i = 0; i < kUiTabCapacity; ++i) {
        if (i == dragged || !(layout.slot_width[i] > 0))
            continue;
        if (layout.slot_left[i] + layout.slot_width[i] * .5f >= x)
            return i > dragged ? i - 1 : i;
        last = i;
    }
    return maximum(last, dragged);
}

u64 ui_tab_strip_stamp(const UiTabStripView& view) noexcept {
    const u32 count = minimum<u32>(view.count, kUiTabCapacity);
    const u32 state[]{count, view.plus_enabled, static_cast<u32>(view.hovered.kind) << 8 | view.hovered.index,
                      static_cast<u32>(view.pressed.kind) << 8 | view.pressed.index};
    const float motion[]{view.remaining, view.drag_left};
    u64 hash = hash64_bytes(kHash64Seed, state, sizeof(state));
    hash = hash64_bytes(hash, motion, sizeof(motion));
    for (u32 i = 0; i < count; ++i) {
        const UiTabItem& item = view.items[i];
        hash = hash64_counted_bytes(hash, item.label.data, item.label.bytes);
        hash = hash64_mix(hash, static_cast<u64>(item.icon) << 8 | item.flags);
        hash = hash64_bytes(hash, &item.from_left, 4 * sizeof(float));
    }
    return hash;
}

bool ui_tab_strip_paint(PaintStream* paint, const Theme& theme, const UiTabStripLayout& layout,
                        const UiTabStripView& view, bool window_active, float pixel_scale) noexcept {
    const auto& tokens = theme.tokens.caption;
    const float radius = theme.scale.radius_md, small = theme.scale.radius_sm;
    const Color idle = window_active ? tokens.icon : tokens.icon_inactive;
    // A tab's rounded shape reaches a corner radius below the bar, whose draw scissors it: the bottom corners that
    // meet the bar's edge are square.
    const auto fill = [&](const UiRect& r, Color color, float amount, float corner) noexcept {
        color.alpha *= amount;
        return !(color.alpha > 0) || paint_stream_push_rounded(paint, r, color, corner);
    };
    const auto icon = [&](UiIcon glyph, const UiRect& box, const Color& color) noexcept {
        return !ui_rect_has_area(box) || ui_icon_paint(paint, glyph, snapped(box, pixel_scale), color);
    };
    const auto tab = [&](u32 i) noexcept {
        const UiTabItem& item = view.items[i];
        const UiRect body = ui_tab_strip_tab(layout, i);
        if (!ui_rect_has_area(body))
            return true;
        const float fade = layout.fade[i];
        const bool pressed = view.pressed.kind == UiTabHitKind::tab && view.pressed.index == i;
        const Color ink = ui_tab_strip_ink(theme, view, layout, i, window_active);
        const UiRect shape{body.left, body.top, body.right, body.bottom + radius};
        if (!fill(shape, tokens.pressed, item.select * fade, radius) ||
            !fill(shape, pressed ? tokens.pressed : tokens.hover, (pressed ? 1.0f : item.hover) * fade, radius) ||
            !icon(item.icon, ui_tab_strip_icon(layout, i), ink))
            return false;
        const UiRect close = ui_tab_strip_close(layout, i);
        if (!ui_rect_has_area(close))
            return true;
        // The pressed close button wins over a merely hovered one.
        const bool close_pressed = view.pressed.kind == UiTabHitKind::close && view.pressed.index == i;
        const bool on_close = close_pressed || (view.pressed.kind == UiTabHitKind::none &&
                                                view.hovered.kind == UiTabHitKind::close && view.hovered.index == i);
        // Beyond the active and the wide tabs, the close button comes in with the hover fill.
        const float reveal = (item.flags & kUiTabActive) || layout.slot_width[i] >= kUiTabCloseAlways ? 1.0f : item.hover;
        Color lit = tokens.icon_hover, quiet = ink;
        lit.alpha *= fade * reveal;
        quiet.alpha *= reveal;
        return fill(close, close_pressed ? tokens.pressed : tokens.hover, on_close ? fade * reveal : 0.0f, small) &&
               icon(UiIcon::close,
                    {close.left + kButtonInset, close.top + kButtonInset, close.right - kButtonInset,
                     close.bottom - kButtonInset},
                    on_close ? lit : quiet);
    };
    u32 dragging = kUiTabCapacity;
    for (u32 i = 0; i < kUiTabCapacity; ++i) {
        if (view.items[i].flags & kUiTabDragging)
            dragging = i;
        else if (!tab(i))
            return false;
    }
    if (dragging < kUiTabCapacity && !tab(dragging))
        return false;
    const auto button = [&](const UiRect& box, UiIcon glyph, UiTabHitKind kind, bool enabled) noexcept {
        if (!ui_rect_has_area(box))
            return true;
        const bool lit = enabled && view.hovered.kind == kind;
        const bool pressed = lit && view.pressed.kind == kind;
        const UiRect square{box.left + kButtonInset, box.top + kButtonInset, box.right - kButtonInset,
                            box.bottom - kButtonInset};
        Color ink = lit ? tokens.icon_hover : idle;
        ink.alpha *= enabled ? 1.0f : kDim;
        return fill(square, pressed ? tokens.pressed : tokens.hover, lit ? 1.0f : 0.0f, small) &&
               icon(glyph, square, ink);
    };
    return button(layout.plus, UiIcon::plus, UiTabHitKind::plus, view.plus_enabled) &&
           button(layout.overflow, UiIcon::ellipsis, UiTabHitKind::overflow, true);
}

} // namespace maicord
