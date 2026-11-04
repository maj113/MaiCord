#include "store_internal.hpp"

namespace maicord {
using namespace store_detail;
namespace {

struct Range {
    u32 first, end;
};
Range active_range(const UiStoreScene& s) noexcept {
    if (s.dialog_open && s.dialog_root != kUiStoreNone)
        return s.dialog_bar ? Range{0, s.count} : Range{s.dialog_root, s.count};
    return {0, s.dialog_root != kUiStoreNone ? s.dialog_root : s.count};
}

bool visible(const UiStoreScene& s, u16 index, UiRect* rect) noexcept {
    const UiRect r = screen_rect(s, index);
    *rect = r;
    return ui_rect_has_area(ui_rect_intersection(r, clip_of(s, index)));
}

float centre_x(const UiRect& r) noexcept {
    return (r.left + r.right) * .5f;
}

// Reveals a page node inside the panel (keyboard focus).
void reveal(UiStoreScene* s, u16 index) noexcept {
    if (in_dialog(*s, index))
        return;
    const UiRect r = screen_rect(*s, index);
    const float top = s->column.top, bottom = s->panel.bottom - 16;
    if (r.top < top)
        ui_store_scene_scroll_to(s, s->scroll + r.top - top - 16);
    else if (r.bottom > bottom)
        ui_store_scene_scroll_to(s, s->scroll + r.bottom - bottom);
}

// Page Up/Down and Home/End outside a group move the page (or the open dialog's body), not the focus.
bool paging(UiStoreKey key) noexcept {
    return key == UiStoreKey::page_up || key == UiStoreKey::page_down || key == UiStoreKey::home || key == UiStoreKey::end;
}
bool page_scroll(UiStoreScene* s, UiStoreKey key) noexcept {
    const float page = ui_rect_height(s->panel) * .8f, far = s->content_height + s->dialog_height;
    return ui_store_scene_scroll(s, key == UiStoreKey::page_up     ? -page
                                    : key == UiStoreKey::page_down ? page
                                    : key == UiStoreKey::home      ? -far
                                                                   : far);
}

bool set_focus(UiStoreScene* s, u16 index, bool visible_ring) noexcept {
    const u32 action = s->nodes[index].action;
    const bool changed = s->focus_action != action || s->focus_visible != visible_ring;
    s->focus_action = action;
    s->focus_visible = visible_ring;
    reveal(s, index);
    if (changed)
        ++s->motion;
    return changed;
}

bool fill_event(const UiStoreScene& s, u16 index, UiStoreEventKind kind, u32 value, UiStoreEvent* event) noexcept {
    if (!event)
        return true;
    *event = {kind, s.nodes[index].action, value, screen_rect(s, index)};
    return true;
}

u32 slider_steps(const UiStoreNode& n) noexcept {
    return n.count > 1 ? n.count : 2;
}

u32 slider_value_at(const UiStoreNode& n, const UiRect& r, float x) noexcept {
    const float left = r.left + 8, right = r.right - 8;
    const u32 steps = slider_steps(n);
    const float t = right > left ? (x - left) / (right - left) : 0;
    const float clamped = t < 0 ? 0 : t > 1 ? 1 : t;
    return static_cast<u32>(clamped * (steps - 1) + .5f);
}

// The nearest node of a group in a direction, by rows then by distance across.
u16 neighbour(const UiStoreScene& s, u16 from, UiStoreKey key) noexcept {
    const auto& origin = s.nodes[from];
    const UiRect r = screen_rect(s, from);
    const Range range = active_range(s);
    if (key == UiStoreKey::left || key == UiStoreKey::right) {
        const bool forward = key == UiStoreKey::right;
        u16 best = kUiStoreNone;
        for (u32 i = range.first; i < range.end; ++i) {
            const auto& n = s.nodes[i];
            if (n.group != origin.group || !interactive(n) || i == from)
                continue;
            if (forward ? i > from && (best == kUiStoreNone) : i < from)
                best = static_cast<u16>(i);
            if (forward && best != kUiStoreNone)
                break;
        }
        return best;
    }
    const bool down = key == UiStoreKey::down;
    u16 best = kUiStoreNone;
    float best_gap = 1e9f, best_cross = 1e9f;
    for (u32 i = range.first; i < range.end; ++i) {
        const auto& n = s.nodes[i];
        if (n.group != origin.group || !interactive(n) || i == from)
            continue;
        const UiRect c = screen_rect(s, static_cast<u16>(i));
        const float gap = down ? c.top - r.top : r.top - c.top;
        // A row below (or above) the current one, not just a taller sibling on the same line.
        if (gap < (down ? r.bottom - r.top : c.bottom - c.top) * .5f || (down ? c.top < r.bottom - 1 : c.bottom > r.top + 1))
            continue;
        const float cross = centre_x(c) > centre_x(r) ? centre_x(c) - centre_x(r) : centre_x(r) - centre_x(c);
        if (gap < best_gap - .5f || (gap <= best_gap + .5f && cross < best_cross)) {
            best = static_cast<u16>(i);
            best_gap = gap;
            best_cross = cross;
        }
    }
    return best;
}

u16 group_edge(const UiStoreScene& s, u16 from, bool last, bool row_only) noexcept {
    const auto& origin = s.nodes[from];
    const UiRect r = screen_rect(s, from);
    const Range range = active_range(s);
    u16 found = kUiStoreNone;
    for (u32 i = range.first; i < range.end; ++i) {
        const auto& n = s.nodes[i];
        if (n.group != origin.group || !interactive(n))
            continue;
        if (row_only) {
            const UiRect c = screen_rect(s, static_cast<u16>(i));
            if (c.top > r.bottom - 1 || c.bottom < r.top + 1)
                continue;
        }
        if (!last)
            return static_cast<u16>(i);
        found = static_cast<u16>(i);
    }
    return found;
}

// The first interactive node of [first, end) going forward, or the last going back; kUiStoreNone: none.
u16 scan(const UiStoreScene& s, u32 first, u32 end, bool reverse) noexcept {
    if (!reverse) {
        for (u32 i = first; i < end; ++i)
            if (interactive(s.nodes[i]))
                return static_cast<u16>(i);
        return kUiStoreNone;
    }
    for (u32 i = end; i > first; --i)
        if (interactive(s.nodes[i - 1]))
            return static_cast<u16>(i - 1);
    return kUiStoreNone;
}
} // namespace

namespace store_detail {
u16 hit_node(const UiStoreScene& s, float x, float y) noexcept {
    const bool in_bar = s.dialog_bar && s.dialog_open && ui_rect_contains(s.dialog_rect, x, y);
    if (s.page_root == kUiStoreNone || (s.dialog_open && !s.dialog_bar && !ui_rect_contains(s.dialog_rect, x, y)) ||
        (!s.dialog_open && !ui_rect_contains(s.panel, x, y)) || (s.dialog_bar && !in_bar && !ui_rect_contains(s.panel, x, y)))
        return kUiStoreNone;
    const Range range = s.dialog_bar ? (in_bar ? Range{s.dialog_root, s.count} : Range{0, s.dialog_root}) : active_range(s);
    for (u32 i = range.end; i > range.first; --i) {
        const auto& n = s.nodes[i - 1];
        if (!interactive(n))
            continue;
        const UiRect r = screen_rect(s, static_cast<u16>(i - 1));
        if (ui_rect_contains(r, x, y) && ui_rect_contains(clip_of(s, static_cast<u16>(i - 1)), x, y))
            return static_cast<u16>(i - 1);
    }
    return kUiStoreNone;
}
} // namespace store_detail

bool ui_store_scene_hover(UiStoreScene* s, float x, float y) noexcept {
    if (!s || !s->active)
        return false;
    const u16 hit = hit_node(*s, x, y);
    const u32 action = hit == kUiStoreNone ? 0 : s->nodes[hit].action;
    const bool clear = hit != kUiStoreNone && s->nodes[hit].kind == UiStoreKind::search && s->nodes[hit].value != 0 &&
                       ui_rect_contains(search_clear_rect(screen_rect(*s, hit)), x, y);
    if (action == s->hover_action && clear == s->hover_clear)
        return false;
    s->hover_action = action;
    s->hover_clear = clear;
    ++s->motion;
    return true;
}

bool ui_store_scene_modal_hit(const UiStoreScene& s, float x, float y) noexcept {
    return s.dialog_open && ui_rect_contains(s.dialog_rect, x, y);
}

bool ui_store_scene_slider(UiStoreScene* s, u32 action, float x, UiStoreEvent* event) noexcept {
    const u16 index = s ? find_action(*s, action) : kUiStoreNone;
    if (index == kUiStoreNone || s->nodes[index].kind != UiStoreKind::slider)
        return false;
    const u32 value = slider_value_at(s->nodes[index], screen_rect(*s, index), x);
    if (value == s->nodes[index].value)
        return false;
    return fill_event(*s, index, UiStoreEventKind::value, value, event);
}

bool ui_store_scene_press(UiStoreScene* s, float x, float y, UiStoreEvent* event) noexcept {
    if (!s || !s->active)
        return false;
    const u16 hit = hit_node(*s, x, y);
    if (hit == kUiStoreNone) {
        s->press_action = 0;
        return false;
    }
    s->press_action = s->nodes[hit].action;
    s->focus_action = s->press_action;
    s->focus_visible = false;
    ++s->motion;
    if (s->nodes[hit].kind == UiStoreKind::slider) {
        const u32 value = slider_value_at(s->nodes[hit], screen_rect(*s, hit), x);
        if (value != s->nodes[hit].value)
            fill_event(*s, hit, UiStoreEventKind::value, value, event);
    }
    return true;
}

bool ui_store_scene_release_pointer(UiStoreScene* s, float x, float y, UiStoreEvent* event) noexcept {
    if (!s)
        return false;
    const u32 pressed = s->press_action;
    s->press_action = 0;
    if (!pressed)
        return false;
    ++s->motion;
    const u16 hit = hit_node(*s, x, y);
    if (hit == kUiStoreNone || s->nodes[hit].action != pressed || s->nodes[hit].kind == UiStoreKind::slider)
        return true;
    return fill_event(*s, hit, UiStoreEventKind::activate, 0, event);
}

bool ui_store_scene_focus(UiStoreScene* s, u32 action, bool visible_ring) noexcept {
    if (!s)
        return false;
    if (!action) {
        const bool changed = s->focus_action != 0;
        s->focus_action = 0;
        if (changed)
            ++s->motion;
        return changed;
    }
    const u16 index = find_action(*s, action);
    return index != kUiStoreNone && set_focus(s, index, visible_ring);
}

u32 ui_store_scene_focused(const UiStoreScene& s) noexcept {
    return s.focus_action;
}

bool ui_store_scene_rect(const UiStoreScene& s, u32 action, UiRect* rect) noexcept {
    const u16 index = find_action(s, action);
    if (index == kUiStoreNone)
        return false;
    *rect = screen_rect(s, index);
    return true;
}

bool ui_store_scene_press_is_slider(const UiStoreScene& s) noexcept {
    const u16 index = find_action(s, s.press_action);
    return index != kUiStoreNone && s.nodes[index].kind == UiStoreKind::slider;
}

u32 ui_store_scene_value(const UiStoreScene& s, u32 action) noexcept {
    const u16 index = find_action(s, action);
    return index == kUiStoreNone ? 0 : s.nodes[index].value;
}

bool ui_store_scene_key(UiStoreScene* s, UiStoreKey key, UiStoreEvent* event) noexcept {
    if (!s || !s->active)
        return false;
    u16 current = s->focus_action ? find_action(*s, s->focus_action) : kUiStoreNone;
    const Range range = active_range(*s);
    if (key == UiStoreKey::tab || key == UiStoreKey::back_tab) {
        const bool reverse = key == UiStoreKey::back_tab;
        u16 next = current == kUiStoreNone ? scan(*s, range.first, range.end, reverse)
                   : reverse               ? scan(*s, range.first, current, true)
                                           : scan(*s, current + 1u, range.end, false);
        if (next == kUiStoreNone && s->dialog_open && s->focus_action)
            next = scan(*s, range.first, range.end, reverse); // A dialog traps focus: wrap around.
        if (next == kUiStoreNone)
            return false;
        set_focus(s, next, true);
        return true;
    }
    if (current == kUiStoreNone) {
        if (paging(key))
            return page_scroll(s, key);
        if (key == UiStoreKey::down || key == UiStoreKey::right) {
            const u16 first = scan(*s, range.first, range.end, false);
            if (first != kUiStoreNone) {
                set_focus(s, first, true);
                return true;
            }
        }
        return false;
    }
    const auto& n = s->nodes[current];
    if (key == UiStoreKey::enter || key == UiStoreKey::space) {
        if (n.kind == UiStoreKind::search || n.kind == UiStoreKind::slider)
            return false;
        // Enter opens a card or presses a button; Space toggles and presses (the host decides by action).
        return fill_event(*s, current, UiStoreEventKind::activate, 0, event);
    }
    if (n.kind == UiStoreKind::slider) {
        const u32 steps = slider_steps(n), page = steps > 10 ? steps / 10 : 1;
        u32 value = n.value;
        switch (key) {
        case UiStoreKey::left:
        case UiStoreKey::down:
            value = value ? value - 1 : 0;
            break;
        case UiStoreKey::right:
        case UiStoreKey::up:
            value = value + 1 < steps ? value + 1 : steps - 1;
            break;
        case UiStoreKey::page_up:
            value = value + page < steps ? value + page : steps - 1;
            break;
        case UiStoreKey::page_down:
            value = value > page ? value - page : 0;
            break;
        case UiStoreKey::home:
            value = 0;
            break;
        case UiStoreKey::end:
            value = steps - 1;
            break;
        default:
            return false;
        }
        if (value != n.value)
            fill_event(*s, current, UiStoreEventKind::value, value, event);
        return true;
    }
    if (!n.group)
        return paging(key) && page_scroll(s, key);
    u16 target = kUiStoreNone;
    switch (key) {
    case UiStoreKey::left:
    case UiStoreKey::right:
    case UiStoreKey::up:
    case UiStoreKey::down:
        target = neighbour(*s, current, key);
        break;
    case UiStoreKey::home:
    case UiStoreKey::end:
        target = group_edge(*s, current, key == UiStoreKey::end, true);
        break;
    case UiStoreKey::first:
    case UiStoreKey::last:
        target = group_edge(*s, current, key == UiStoreKey::last, false);
        break;
    case UiStoreKey::page_up:
    case UiStoreKey::page_down: {
        const UiStoreKey step = key == UiStoreKey::page_up ? UiStoreKey::up : UiStoreKey::down;
        const float rows = maximum(1.f, ui_rect_height(s->panel) / maximum(40.f, screen_rect(*s, current).bottom - screen_rect(*s, current).top));
        u16 at = current;
        for (float k = 0; k < rows; ++k) {
            const u16 next = neighbour(*s, at, step);
            if (next == kUiStoreNone)
                break;
            at = next;
        }
        target = at == current ? kUiStoreNone : at;
        break;
    }
    default:
        return false;
    }
    if (target == kUiStoreNone)
        return true; // the key belongs to the group even at its edge
    set_focus(s, target, true);
    // Selection follows focus on tabs and segments.
    const auto kind = s->nodes[target].kind;
    if (kind == UiStoreKind::tab || kind == UiStoreKind::segment)
        fill_event(*s, target, UiStoreEventKind::activate, 0, event);
    return true;
}

bool ui_store_scene_tick(UiStoreScene* s, u64 now_ms, bool reduced_motion) noexcept {
    if (!s)
        return false;
    s->reduced_motion = reduced_motion;
    // A frame is needed only while a skeleton is on screen. A second tick in the same millisecond (the host
    // synchronizes more than once per frame) still reports it, so the frame chain never drops.
    if (!ui_store_scene_animating(*s))
        return false;
    if (s->clock_ms != now_ms) {
        s->clock_ms = now_ms;
        ++s->motion;
    }
    return true;
}

namespace {
bool any_visible(const UiStoreScene& s, bool (*match)(const UiStoreNode&) noexcept) noexcept {
    if (!s.active)
        return false;
    for (u32 i = 0; i < s.count; ++i) {
        UiRect r;
        if (match(s.nodes[i]) && visible(s, static_cast<u16>(i), &r))
            return true;
    }
    return false;
}
} // namespace

bool ui_store_scene_animating(const UiStoreScene& s) noexcept {
    return !s.reduced_motion && any_visible(s, [](const UiStoreNode& n) noexcept { return n.kind == UiStoreKind::bar; });
}

bool ui_store_scene_spinner_visible(const UiStoreScene& s) noexcept {
    return any_visible(s, [](const UiStoreNode& n) noexcept {
        return n.kind == UiStoreKind::spinner || (n.kind == UiStoreKind::button && (n.flags & ui_store_busy));
    });
}
} // namespace maicord
