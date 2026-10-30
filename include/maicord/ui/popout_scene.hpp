#pragma once

#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/scene_text.hpp"
#include "maicord/ui/widget_manager.hpp"

// Helpers the popout scenes (pins, inbox, thread browser, forum cards) share.
// A scene needs `widgets`, `panel` and `body` members; the .cpp files bring the
// names in with `using namespace popout;` inside their anonymous namespace.
namespace maicord::popout {

template <class Scene> UiRect bounds(const Scene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_manager_rectangle(s.widgets, handle);
}
template <class Scene> bool hovered(const Scene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_handle_matches(handle, s.widgets.hovered);
}
template <class Scene> bool focused(const Scene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_handle_matches(handle, s.widgets.focused);
}

// The raised card / button plate.
inline bool surface(PaintStream* out, const UiRect& r, const Theme& theme, const Color& fill, const Color& border,
                    float radius = 8) noexcept {
    return ui_background_paint_surface(out, r, theme, fill, border, radius, 1, .9f, UiBackgroundSurfaceMode::gradient,
                                       UiBackgroundTier::high);
}

// Text clipped to the body (ui_text_in_body) or to the whole panel.
template <class Scene>
bool text(UiTextOutput* out, const Scene& s, UiWidgetHandle owner, const Utf8View& value, const UiRect& r,
          const Color& color, u32 flags = 0, u32 mode = 0) noexcept {
    return ui_scene_text(out, s.widgets, owner, value, r, (mode & ui_text_in_body) ? s.body : s.panel, color, flags,
                         mode);
}

// Places a widget under `parent` and records its target. Children of the body viewport are placed against the
// body, everything else against the panel; during tree construction the parent has not been laid out yet.
template <class Scene, class Target>
UiWidgetHandle add(Scene& s, const UiRect& r, const Target& target, const Utf8View& name, bool enabled,
                   UiWidgetHandle parent) noexcept {
    if (!ui_rect_has_area(r))
        return {};
    const auto origin = s.body_owner.generation && parent.slot == s.body_owner.slot ? s.body : s.panel;
    const auto result = ui_widget_manager_add_placed(
        &s.widgets, parent, target.action == decltype(target.action){} ? UiWidgetKind::container : UiWidgetKind::button,
        static_cast<u16>(enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : 0), name, r, origin);
    if (!result.ok())
        return {};
    s.targets[result.handle.slot] = target;
    return result.handle;
}

// Pointer hover; `revision` is the scene's paint revision, bumped on a change.
inline bool set_hover(UiWidgetManager* widgets, u32* revision, float x, float y, bool* changed) noexcept {
    if (!changed || !ui_widget_manager_hover_at(widgets, x, y, changed))
        return false;
    if (*changed)
        ++*revision;
    return true;
}

// The first widget slot (1..count) whose target equals `wanted` and accepts focus; 0 when none does.
template <class Target, class Equal>
u32 focus_target(UiWidgetManager* widgets, const Target* targets, const Target& wanted, Equal equal) noexcept {
    for (u32 i = 1; i < widgets->count; ++i)
        if (equal(targets[i], wanted) && ui_widget_manager_focus(widgets, {static_cast<u16>(i), widgets->generation}))
            return i;
    return 0;
}

// Keyboard traversal over an ordered, wrapping candidate list. `enumerate(emit)` calls emit(target) for each
// candidate in order and returns false when the source failed; it runs twice (count and selection, then pick),
// so the list never needs storage. `selected` is the current target, if any is in the list.
enum class Ring : u8 { next, previous, first, last };
template <class Target, class Enumerate, class Equal>
bool ring_pick(Enumerate enumerate, Equal equal, const Target& selected, Ring move, Target* out) noexcept {
    u32 count = 0, found = ~0u;
    if (!enumerate([&](const Target& candidate) noexcept {
            if (equal(candidate, selected))
                found = count;
            ++count;
        }) ||
        !count)
        return false;
    u32 index = move == Ring::first ? 0
                : move == Ring::last ? count - 1
                : move == Ring::previous ? (found >= count || !found ? count - 1 : found - 1)
                                         : (found >= count || found + 1 == count ? 0 : found + 1);
    u32 at = 0;
    return enumerate([&](const Target& candidate) noexcept {
        if (at++ == index)
            *out = candidate;
    });
}

} // namespace maicord::popout
