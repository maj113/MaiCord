#pragma once

#include "maicord/core/memory.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/layout/rect_edges.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/widgets/spinner.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/core/widget_storage.hpp"
#include "maicord/ui/theme/background_presentation.hpp"

// Building blocks for retained scenes: a scene describes each control's look
// once (a UiLook of theme tokens) and paints and emits text through two pens
// that skip everything after the first failure, so paint and text passes read
// as straight lists without `ok &&` chains.
namespace maicord {

// ---------------------------------------------------------------------------
// Theme tokens: a Theme color named by its byte offset, so a description that
// names colors is a compile-time constant every theme resolves.
using UiToken = u16;
inline constexpr UiToken kUiTokenNone = 0xffff;
static_assert(sizeof(Theme) < kUiTokenNone);
#define UI_TOKEN(member) static_cast<::maicord::UiToken>(__builtin_offsetof(::maicord::Theme, member))
inline const Color& ui_token(const Theme& theme, UiToken token) noexcept {
    return *reinterpret_cast<const Color*>(reinterpret_cast<const char*>(&theme) + token);
}

// ---------------------------------------------------------------------------
// Plate state bits of one handle (zero-generation handles match nothing).
enum UiStateBit : u8 {
    ui_state_hovered = 1,
    ui_state_focused = 2,
    ui_state_selected = 4,
};
inline u8 ui_widget_state(const UiWidgetManager& widgets, UiWidgetHandle handle) noexcept {
    return static_cast<u8>((ui_widget_handle_matches(handle, widgets.hovered) ? ui_state_hovered : 0) |
                           (ui_widget_handle_matches(handle, widgets.focused) ? ui_state_focused : 0));
}
// Scene shorthands; a scene keeps its UiWidgetManager in `widgets`.
template <class Scene> UiRect ui_bounds(const Scene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_manager_rectangle(s.widgets, handle);
}
template <class Scene> bool ui_hovered(const Scene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_handle_matches(handle, s.widgets.hovered);
}
template <class Scene> bool ui_focused(const Scene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_handle_matches(handle, s.widgets.focused);
}

// ---------------------------------------------------------------------------
// How a control's plate looks in each state, by theme token:
//   fill   = selected ? selected : hovered ? hover : fill
//   border = focused ? focus : selected ? selected_border : border
// kUiTokenNone falls back: hover to fill, selected to hover, border to fill,
// selected_border to border.
enum UiLookFlag : u8 {
    ui_look_hot_only = 1,           // no plate unless hovered, focused or selected
    ui_look_pill = 2,               // radius is half the plate's height
    ui_look_dialog = 4,             // dialog material (full palette share, highest tier); else popover
    ui_look_radius_sm = 8,          // radius is theme.scale.radius_sm
    ui_look_radius_md = 16,         // radius is theme.scale.radius_md
    ui_look_selected_emphasis = 32, // the selected fill is the popover's emphasis selection tint
};
struct UiLook {
    UiToken fill;
    UiToken hover = kUiTokenNone;
    UiToken selected = kUiTokenNone;
    UiToken border = kUiTokenNone;
    UiToken focus = UI_TOKEN(tokens.border.focus);
    UiToken selected_border = kUiTokenNone;
    float radius = 8;
    u8 flags = 0;
};
// The quiet icon button of popout headers: a plate only while hot.
inline constexpr UiLook kUiGhostButton{.fill = UI_TOKEN(surface_high), .radius = 6, .flags = ui_look_hot_only};
// A tab of a row of tabs (Inbox, Friends): a pill shown while selected or hot.
inline constexpr UiLook kUiTab{.fill = UI_TOKEN(surface), .selected = UI_TOKEN(surface_high),
                               .border = UI_TOKEN(surface_high), .flags = ui_look_hot_only | ui_look_pill};

// ---------------------------------------------------------------------------
// Paint pen: every call is skipped after the first failure; clips it pushed
// are popped by unclip()/end() only when they were pushed. Pushes, the clip
// stack and constant looks are force-inlined, so the pen stays in registers
// and a call site compiles to the `ok && push(...)` it replaces; the frames and
// run-time looks are out of line, one copy for all scenes.
class UiPaint {
  public:
    UiPaint(PaintStream* out, const Theme& theme) noexcept : out_(out), theme_(&theme), ok_(out != nullptr) {}

    const Theme& theme() const noexcept { return *theme_; }
    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    __forceinline UiPaint& fill(const UiRect& r, const Color& color) noexcept {
        ok_ = ok_ && paint_stream_push(out_, r, color);
        return *this;
    }
    __forceinline UiPaint& rounded(const UiRect& r, const Color& color, float radius) noexcept {
        ok_ = ok_ && paint_stream_push_rounded(out_, r, color, radius);
        return *this;
    }
    __forceinline UiPaint& rounded_border(const UiRect& r, const Color& fill, const Color& border, float radius,
                                          float width) noexcept {
        ok_ = ok_ && paint_stream_push_rounded_border(out_, r, fill, border, radius, width);
        return *this;
    }
    // Paint another function draws on the same stream (`Paint(stream, args...)`, returning success); skipped after
    // a failure, a failure stops the pen.
    template <auto Paint, class... Args> __forceinline UiPaint& use(Args&&... args) noexcept {
        ok_ = ok_ && Paint(out_, static_cast<Args&&>(args)...);
        return *this;
    }
    __forceinline UiPaint& icon(UiIcon icon, const UiRect& r, const Color& color) noexcept {
        ok_ = ok_ && ui_icon_paint(out_, icon, r, color);
        return *this;
    }
    // A raised plate: popover material (.9 palette share, high tier) or, with dialog, the dialog material.
    __forceinline UiPaint& surface(const UiRect& r, const Color& fill, const Color& border, float radius = 8,
                                   bool dialog = false) noexcept {
        ok_ = ok_ && plate(out_, *theme_, r, fill, border, radius, dialog);
        return *this;
    }
    // A control plate for `state` (UiStateBit bits). The template forms take a constant look, resolve its tokens
    // at compile time and inline to the plate call; the forms taking a UiLook object serve looks built at run
    // time (a style's radius) and the selected-emphasis tint.
    template <UiLook L> __forceinline UiPaint& look(const UiRect& r, u8 state) noexcept {
        static_assert(!(L.flags & ui_look_selected_emphasis), "selected emphasis needs the run-time look()");
        constexpr UiToken hover = L.hover == kUiTokenNone ? L.fill : L.hover;
        constexpr UiToken selected = L.selected == kUiTokenNone ? hover : L.selected;
        constexpr UiToken border = L.border == kUiTokenNone ? L.fill : L.border;
        constexpr UiToken selected_border = L.selected_border == kUiTokenNone ? border : L.selected_border;
        if (!ok_ || ((L.flags & ui_look_hot_only) && !state))
            return *this;
        const UiToken fill = (state & ui_state_selected) ? selected : (state & ui_state_hovered) ? hover : L.fill;
        const UiToken edge = (state & ui_state_focused)    ? L.focus
                             : (state & ui_state_selected) ? selected_border
                                                           : border;
        float radius = L.radius;
        if constexpr ((L.flags & ui_look_pill) != 0)
            radius = ui_rect_height(r) * .5f;
        else if constexpr ((L.flags & ui_look_radius_sm) != 0)
            radius = theme_->scale.radius_sm;
        else if constexpr ((L.flags & ui_look_radius_md) != 0)
            radius = theme_->scale.radius_md;
        return surface(r, ui_token(*theme_, fill), ui_token(*theme_, edge), radius, (L.flags & ui_look_dialog) != 0);
    }
    template <UiLook L>
    __forceinline UiPaint& control(const UiWidgetManager& widgets, UiWidgetHandle handle, bool selected = false) noexcept {
        if (!handle.generation)
            return *this;
        return look<L>(ui_widget_manager_rectangle(widgets, handle),
                       static_cast<u8>(ui_widget_state(widgets, handle) | (selected ? ui_state_selected : 0)));
    }
    // control() with an icon inset `inset` DIPs inside the widget.
    template <UiLook L>
    __forceinline UiPaint& icon_control(const UiWidgetManager& widgets, UiWidgetHandle handle, UiIcon icon, float inset,
                                        const Color& color) noexcept {
        return control<L>(widgets, handle).icon(icon, ui_rect_shrink(ui_widget_manager_rectangle(widgets, handle), inset),
                                                color);
    }
    UiPaint& look(const UiRect& r, const UiLook& look, u8 state) noexcept;
    UiPaint& control(const UiWidgetManager& widgets, UiWidgetHandle handle, const UiLook& look,
                     bool selected = false) noexcept;
    // A rectangle in the theme's separator color; rule_below/rule_above: the one-DIP line inside the bottom
    // (top) edge of a band.
    __forceinline UiPaint& separator(const UiRect& r) noexcept { return fill(r, ui_separator_color(*theme_)); }
    __forceinline UiPaint& rule_below(const UiRect& band) noexcept {
        return separator(ui_rect_edges(band, ui_start(0), ui_end(-1), ui_end(0), ui_end(0)));
    }
    __forceinline UiPaint& rule_above(const UiRect& band) noexcept {
        return separator(ui_rect_edges(band, ui_start(0), ui_start(0), ui_end(0), ui_start(1)));
    }
    __forceinline UiPaint& spinner(const UiRect& r, const Color& color, u8 phase) noexcept {
        ok_ = ok_ && ui_spinner_paint(out_, r, color, phase);
        return *this;
    }
    __forceinline UiPaint& scrollbar(const UiScrollbarGeometry& geometry) noexcept {
        ok_ = ok_ && ui_scrollbar_paint(out_, geometry, *theme_);
        return *this;
    }
    // A popover panel: isolated shadow, the panel clip, then the plate (with the surface_high border unless
    // `bordered` is false).
    __forceinline UiPaint& popover(const UiRect& panel, float radius, bool bordered = true) noexcept {
        return frame(ok_ && popover_frame(out_, *theme_, panel, radius, bordered));
    }
    // A modal dialog: the viewport clip and scrim, then the dialog surface with its shadow.
    __forceinline UiPaint& dialog(const UiRect& viewport, const UiRect& panel) noexcept {
        return frame(ok_ && dialog_frame(out_, *theme_, viewport, panel));
    }
    __forceinline UiPaint& clip(const UiRect& r) noexcept { return frame(ok_ && paint_stream_push_clip(out_, r)); }
    // Pops the innermost clip this pen pushed, even after a failure.
    __forceinline UiPaint& unclip() noexcept {
        if (clips_) {
            --clips_;
            ok_ = paint_stream_pop_clip(out_) && ok_;
        }
        return *this;
    }
    // Records the failure of paint pushed outside the pen, stopping later primitives.
    void fail() noexcept { ok_ = false; }
    // Pops every clip this pen still holds; the pass result.
    __forceinline bool end() noexcept {
        for (; clips_; --clips_)
            if (!paint_stream_pop_clip(out_)) {
                clips_ = 0;
                return ok_ = false;
            }
        return ok_;
    }

  private:
    // Out-of-line pieces take no pen, so a pen whose methods all inline stays in registers. The frames push
    // their clip and pop it again when a later push fails.
    static bool plate(PaintStream* out, const Theme& theme, const UiRect& r, const Color& fill, const Color& border,
                      float radius, bool dialog) noexcept;
    static bool popover_frame(PaintStream* out, const Theme& theme, const UiRect& panel, float radius,
                              bool bordered) noexcept;
    static bool dialog_frame(PaintStream* out, const Theme& theme, const UiRect& viewport, const UiRect& panel) noexcept;
    __forceinline UiPaint& frame(bool pushed) noexcept {
        if (pushed)
            ++clips_;
        else
            ok_ = false;
        return *this;
    }

    PaintStream* out_;
    const Theme* theme_;
    bool ok_;
    u8 clips_ = 0;
};

// ---------------------------------------------------------------------------
// Text placement bits: wrapped lines (top aligned unless ui_text_middle), the
// pen's body region instead of its panel, and the horizontal alignment
// (UiTextAlignment) in bits 2-3.
inline constexpr u32 ui_text_wrap = 1;
inline constexpr u32 ui_text_in_body = 2;
inline constexpr u32 ui_text_center = 1u << 2;
inline constexpr u32 ui_text_end = 2u << 2;
inline constexpr u32 ui_text_middle = 1u << 4;
inline constexpr u32 ui_text_clip = 1u << 5; // overflow clips instead of ellipsizing
inline constexpr u32 ui_text_chat = 1u << 6; // sized by Text size in chat (the chat face)

// How a region clips a label (a UiTextPen template argument):
//   intersect: the clip is rectangle ∩ region; nothing when that is empty.
//   region:    the clip is the region; nothing when the rectangle is empty.
//   label:     the clip is the region; nothing unless the rectangle meets it.
enum class UiTextClip : u8 { intersect, region, label };
inline constexpr UiRect kUiNoRect{};

// Text pen: one label per call, owned by a widget, clipped to the pen's panel
// (or body, with ui_text_in_body) or to an explicit region through in(). The
// panel and body are borrowed; empty text pushes nothing. Every call is
// skipped after the first failure. A call passes the pen and the label, so a
// call site is one direct call with no branch. The clip policy is a member, so one out-of-line body serves every
// pen type.
class UiTextPenBase {
  protected:
    UiTextPenBase(UiTextOutput* out, const UiWidgetManager& widgets, const UiRect& panel, const UiRect& body,
                  UiTextClip policy) noexcept
        : out_(out), widgets_(&widgets), panel_(&panel), body_(&body), ok_(ui_text_output_valid(out)),
          policy_(policy) {}
    // A label clipped to `region` instead of the pen's panel/body. Every label goes through here: kept free of
    // machine-outlined fragments (calls the size build would otherwise add to each label).
    [[clang::no_outline]] void push_region(const UiRect& region, UiWidgetHandle owner, const Utf8View& value,
                                           const UiRect& r, const Color& color, u32 style, u32 mode) noexcept;

    UiTextOutput* out_;
    const UiWidgetManager* widgets_;
    const UiRect* panel_;
    const UiRect* body_;
    bool ok_;
    UiTextClip policy_;
};
template <UiTextClip Policy = UiTextClip::intersect> class UiTextPen : public UiTextPenBase {
  public:
    UiTextPen(UiTextOutput* out, const UiWidgetManager& widgets, const UiRect& panel,
              const UiRect& body = kUiNoRect) noexcept
        : UiTextPenBase(out, widgets, panel, body, Policy) {}

    bool ok() const noexcept { return ok_; }
    // Records the failure of output pushed outside the pen (an edit field's text), stopping later labels.
    void fail() noexcept { ok_ = false; }
    // A command the pen cannot describe (its own clip, text overflow or face): pushed as is, skipped after a
    // failure, a failure stops the pen.
    UiTextPen& emit(const UiTextCommand& command) noexcept {
        ok_ = ok_ && ui_text_output_push(out_, command);
        return *this;
    }

    __forceinline UiTextPen& operator()(UiWidgetHandle owner, const Utf8View& value, const UiRect& r,
                                        const Color& color, u32 style = 0, u32 mode = 0) noexcept {
        return push((mode & ui_text_in_body) ? *body_ : *panel_, owner, value, r, color, style, mode);
    }
    __forceinline UiTextPen& push(const UiRect& region, UiWidgetHandle owner, const Utf8View& value, const UiRect& r,
                                  const Color& color, u32 style, u32 mode) noexcept {
        push_region(region, owner, value, r, color, style, mode);
        return *this;
    }
    struct Clipped {
        UiTextPen* pen;
        UiRect region;
        __forceinline Clipped& operator()(UiWidgetHandle owner, const Utf8View& value, const UiRect& r,
                                          const Color& color, u32 style = 0, u32 mode = 0) noexcept {
            pen->push(region, owner, value, r, color, style, mode);
            return *this;
        }
    };
    __forceinline Clipped in(const UiRect& region) noexcept { return {this, region}; }
    // A label clipped to the layout clip of the owner's node; the pen fails when the owner has no node.
    __forceinline Clipped at(UiWidgetHandle owner) noexcept {
        const auto* node = ui_widget_manager_layout_node(*widgets_, owner);
        if (!node)
            ok_ = false;
        return {this, node ? node->clip_rectangle : UiRect{}};
    }
};

// Builds a scene-owned label in a fixed buffer and its u8 length: `label << utf8_literal("Page ") << n`.
// A piece that does not fit is dropped whole.
class UiLabelWriter {
  public:
    template <usize Capacity>
    UiLabelWriter(char (&data)[Capacity], u8* bytes) noexcept : data_(data), bytes_(bytes), capacity_(Capacity) {
        static_assert(Capacity <= 255);
        *bytes = 0;
    }
    UiLabelWriter& operator<<(Utf8View text) noexcept;
    UiLabelWriter& operator<<(u64 value) noexcept;

  private:
    char* data_;
    u8* bytes_;
    u32 capacity_;
};

// ---------------------------------------------------------------------------
// Scene lifecycle.

// Allocates the fixed widget storage (with an optional sidecar) and binds the
// manager to it; on failure nothing stays allocated.
bool ui_scene_open(UiFixedWidgetStorageOwner* storage, UiWidgetManager* widgets, u32 capacity,
                   u32 sidecar_bytes = 0, u32 sidecar_alignment = 1) noexcept;
// Releases a scene that owns `storage` and resets it to its empty state.
template <class Scene> void ui_scene_release(Scene* s) noexcept {
    if (!s)
        return;
    ui_fixed_widget_storage_release(&s->storage);
    reset_value(s);
}
// Starts a rebuild: resets the tree and adds the root container that fills the
// viewport (clips, allows overlap); zero-generation handle on failure.
UiWidgetHandle ui_scene_root(UiWidgetManager* widgets, const Utf8View& name) noexcept;
// The root of a part-based scene (ui_widget_manager_add_part), filling the viewport.
UiWidgetHandle ui_scene_part_root(UiWidgetManager* widgets, const Utf8View& name) noexcept;
// Adds a part under `parent`; zero-generation handle on failure.
UiWidgetHandle ui_scene_part(UiWidgetManager* widgets, UiWidgetHandle parent, UiWidgetKind kind,
                             const Utf8View& name) noexcept;

// Places a control (button, or container for a target without action) under
// `parent` and records its target. Children of `s.body_owner` are placed
// against `s.body`, everything else against `s.panel`; a rectangle without
// area adds nothing. Needs `widgets`, `panel`, `body`, `body_owner` and a
// `targets` array indexed by widget slot.
template <class T> struct UiArrayElement;
template <class T, usize N> struct UiArrayElement<T[N]> {
    using Type = T;
};
template <class Scene> using UiSceneTarget = typename UiArrayElement<decltype(Scene::targets)>::Type;
template <class Scene>
UiWidgetHandle ui_scene_add(Scene& s, const UiRect& r, const UiSceneTarget<Scene>& target, const Utf8View& name,
                            bool enabled, UiWidgetHandle parent) noexcept {
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
// The target of an input-enabled widget; none for a stale, disabled or unknown handle.
template <class Scene> UiSceneTarget<Scene> ui_scene_target(const Scene& s, UiWidgetHandle handle) noexcept {
    return handle.slot < sizeof(s.targets) / sizeof(s.targets[0]) && ui_widget_manager_input_enabled(s.widgets, handle)
               ? s.targets[handle.slot]
               : UiSceneTarget<Scene>{};
}
// Whether a scene's row source is still the one it accepted: absent, or at the accepted revision and row count and
// still live by its own probe (when it has one).
template <class Source> __forceinline bool ui_source_current(const Source* source, u32 revision, u32 count) noexcept {
    return !source || (source->revision == revision && source->count == count &&
                       (!source->current || source->current(source->context)));
}
// A row scene (quick switcher, forward, downloads) still shows what it accepted: active, at its content's revision and
// on a current row source.
template <class Scene> __forceinline bool ui_row_scene_current(const Scene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision &&
           ui_source_current(s.source, s.source_revision, s.accepted_count);
}
// Ends a rebuild: lays the tree out in `viewport`, focuses every widget whose
// target equals `focused` and bumps the paint revision.
template <class Target, class Equal>
bool ui_scene_commit(UiWidgetManager* widgets, const UiRect& viewport, const Target* targets, const Target& focused,
                     Equal equal, u32* revision) noexcept {
    if (!ui_widget_manager_layout_rectangle(widgets, viewport).ok())
        return false;
    for (u32 i = 1; i < widgets->count; ++i)
        if (equal(targets[i], focused))
            ui_widget_manager_focus(widgets, {static_cast<u16>(i), widgets->generation});
    ++*revision;
    return true;
}
// The first widget slot (1..count) whose target equals `wanted` and accepts focus; 0 when none does.
template <class Target, class Equal>
u32 ui_scene_focus_target(UiWidgetManager* widgets, const Target* targets, const Target& wanted, Equal equal) noexcept {
    for (u32 i = 1; i < widgets->count; ++i)
        if (equal(targets[i], wanted) && ui_widget_manager_focus(widgets, {static_cast<u16>(i), widgets->generation}))
            return i;
    return 0;
}
// Pointer hover; `revision` is the scene's paint revision, bumped on a change.
// False without `changed` or when the manager refuses the hover.
bool ui_scene_hover(UiWidgetManager* widgets, u32* revision, float x, float y, bool* changed) noexcept;

// Keyboard traversal over an ordered, wrapping candidate list. `enumerate(emit)` calls emit(target) for each
// candidate in order and returns false when the source failed; it runs twice (count and selection, then pick),
// so the list never needs storage. `selected` is the current target, if any is in the list. Scene focus-move
// enums start with next, previous, first, last and convert with ui_ring().
enum class Ring : u8 { next, previous, first, last };
template <class Move> constexpr Ring ui_ring(Move move) noexcept {
    return static_cast<Ring>(move);
}
template <class Target, class Enumerate, class Equal>
bool ui_ring_pick(Enumerate enumerate, Equal equal, const Target& selected, Ring move, Target* out) noexcept {
    u32 count = 0, found = ~0u, index = 0;
    // One call of `enumerate` in a loop: the enumerator is expanded once, whatever its size.
    for (bool pick = false;; pick = true) {
        u32 at = 0;
        if (!enumerate([&](const Target& candidate) noexcept {
                if (pick) {
                    if (at++ == index)
                        *out = candidate;
                } else {
                    if (equal(candidate, selected))
                        found = count;
                    ++count;
                }
            }))
            return false;
        if (pick)
            return true;
        if (!count)
            return false;
        index = move == Ring::first ? 0
                : move == Ring::last ? count - 1
                : move == Ring::previous ? (found >= count || !found ? count - 1 : found - 1)
                                         : (found >= count || found + 1 == count ? 0 : found + 1);
    }
}

} // namespace maicord
