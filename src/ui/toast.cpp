#include "maicord/ui/toast.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
constexpr u64 kMeasureSource = 0x54534f4154ull; // "TOAST"
// Fallback metrics without a measure cache (tests, a missing account UI).
constexpr float kEstimatedAdvance = 8, kEstimatedLine = 20;

bool same_entry(const UiToastEntry& a, const UiToastEntry& b) noexcept {
    return a.kind == b.kind && a.bytes == b.bytes && bytes_equal(a.text, b.text, a.bytes);
}
void pop(UiToastScene* s) noexcept {
    for (u32 i = 1; i < s->count; ++i)
        s->entries[i - 1] = s->entries[i];
    reset_value(&s->entries[--s->count]);
}
bool active(UiToastPhase phase) noexcept {
    return phase == UiToastPhase::entering || phase == UiToastPhase::shown ||
           phase == UiToastPhase::leaving;
}
// Phase deadlines in order; false when the phase is still running at now.
bool advance(UiToastScene* s, u64 now, bool animate) noexcept {
    const u64 start = s->phase_started_ms;
    const u64 elapsed = now > start ? now - start : 0;
    const u32 motion = animate ? kUiToastMotionMs : 0;
    switch (s->phase) {
    case UiToastPhase::entering:
        if (elapsed < motion)
            return false;
        s->phase = UiToastPhase::shown;
        s->phase_started_ms = start + motion;
        return true;
    case UiToastPhase::shown:
        if (elapsed < kUiToastHoldMs)
            return false;
        s->phase = UiToastPhase::leaving;
        s->phase_started_ms = start + kUiToastHoldMs;
        return true;
    case UiToastPhase::leaving:
        if (elapsed < motion)
            return false;
        pop(s);
        s->phase = s->count ? UiToastPhase::waiting : UiToastPhase::idle;
        s->phase_started_ms = s->count ? start + motion : 0;
        return s->count != 0;
    case UiToastPhase::waiting: {
        const u32 gap = animate ? kUiToastHandoverMs - kUiToastMotionMs : 0;
        if (elapsed < gap)
            return false;
        s->phase = UiToastPhase::entering;
        s->phase_started_ms = start + gap;
        ++s->serial;
        return true;
    }
    default:
        return false;
    }
}
float phase_progress(const UiToastScene& s, u64 now, bool animate) noexcept {
    const u64 elapsed = now > s.phase_started_ms ? now - s.phase_started_ms : 0;
    const float t = animate ? minimum(1.f, static_cast<float>(elapsed) / kUiToastMotionMs) : 1.f;
    switch (s.phase) {
    case UiToastPhase::entering:
        return t;
    case UiToastPhase::shown:
        return 1;
    case UiToastPhase::leaving:
        return 1 - t;
    default:
        return 0;
    }
}
struct Metrics {
    float padding, gap, margin, lead;
};
Metrics metrics(const Theme& theme, UiToastKind kind) noexcept {
    const auto& s = theme.scale;
    const float gap = s.space_4;
    return {s.space_4 + s.space_2, gap, s.space_4,
            kind == UiToastKind::message ? 0 : kUiToastIconSize + gap};
}
void measure(UiToastScene* s, const UiToastEnvironment& env) noexcept {
    const auto& e = s->entries[0];
    const auto m = metrics(*env.theme, e.kind);
    const float available =
        maximum(1.f, minimum(kUiToastMaximumWidth, ui_rect_width(env.viewport) - m.margin * 2) -
                         m.padding * 2 - m.lead);
    const Utf8View text{e.text, e.bytes};
    UiTextMeasureValue value{};
    const UiTextMeasureRequest request{
        {kMeasureSource, hash64_bytes(kHash64Seed, text.data, text.bytes), 0, 0, 0},
        text,
        {},
        {available, 1024},
        false};
    if (env.measure && ui_text_measure_cache_measure(env.measure, request, &value).ok() &&
        value.size.width > 0 && value.size.height > 0 && value.line_count) {
        const float line = value.size.height / static_cast<float>(value.line_count);
        s->text_size = {minimum(value.size.width, available),
                        line * static_cast<float>(minimum(value.line_count, kUiToastMaximumLines))};
    } else {
        const float width = kEstimatedAdvance * static_cast<float>(e.bytes);
        const u32 lines = width <= available ? 1 : kUiToastMaximumLines;
        s->text_size = {minimum(width, available), kEstimatedLine * static_cast<float>(lines)};
    }
    s->measured_serial = s->serial;
}
void place(UiToastScene* s, const UiToastEnvironment& env) noexcept {
    const auto& e = s->entries[0];
    const auto m = metrics(*env.theme, e.kind);
    const auto v = env.viewport;
    const float content =
        maximum(s->text_size.height, e.kind == UiToastKind::message ? 0.f : kUiToastIconSize);
    const float width = m.padding * 2 + m.lead + s->text_size.width, height = content + m.padding * 2;
    const float center = (v.left + v.right) * .5f;
    // Full motion slides in from one toast height above, as it fades.
    const float slide = env.animate && !env.reduced_motion ? height * (1 - s->progress) : 0;
    const float top = v.top + m.margin - slide;
    s->rectangle = {center - width * .5f, top, center + width * .5f, top + height};
    const float middle = top + height * .5f, left = s->rectangle.left + m.padding;
    s->icon = e.kind == UiToastKind::message
                  ? UiRect{}
                  : UiRect{left, middle - kUiToastIconSize * .5f, left + kUiToastIconSize,
                           middle + kUiToastIconSize * .5f};
    s->text = {left + m.lead, top + m.padding, s->rectangle.right - m.padding,
               s->rectangle.bottom - m.padding};
}
} // namespace

bool ui_toast_push(UiToastScene* s, UiToastKind kind, Utf8View text) noexcept {
    if (!s || !text.data || !text.bytes || !utf8_validate(text).valid ||
        static_cast<u8>(kind) > static_cast<u8>(UiToastKind::failure))
        return false;
    UiToastEntry entry{};
    entry.bytes = static_cast<u8>(utf8_copy_truncated(entry.text, sizeof(entry.text), text));
    entry.kind = kind;
    if (!entry.bytes)
        return false;
    if (s->count && same_entry(s->entries[s->count - 1], entry)) {
        // The newest toast again: a showing one starts its time over; an
        // entering or pending one already covers it. A leaving one queues anew.
        if (s->count == 1 && s->phase == UiToastPhase::shown) {
            s->phase_started_ms = 0;
            return true;
        }
        if (s->count > 1 || s->phase != UiToastPhase::leaving)
            return false;
    }
    if (s->count == kUiToastQueueCapacity) {
        for (u32 i = 2; i < s->count; ++i)
            s->entries[i - 1] = s->entries[i];
        --s->count;
        ++s->dropped;
    }
    s->entries[s->count++] = entry;
    return true;
}

bool ui_toast_bind_text_owner(UiToastScene* s, UiWidgetReference owner) noexcept {
    if (!s || !owner.manager || !ui_widget_manager_resolve(*owner.manager, owner.handle))
        return false;
    s->text_owner = owner;
    return true;
}

bool ui_toast_update(UiToastScene* s, u64 now, const UiToastEnvironment& env) noexcept {
    if (!s || !env.theme)
        return false;
    now = now ? now : 1;
    const float old_progress = s->progress;
    const u32 old_revision = s->revision;
    if (s->phase == UiToastPhase::idle && s->count) {
        s->phase = UiToastPhase::entering;
        s->phase_started_ms = 0;
        ++s->serial;
    }
    if (s->phase != UiToastPhase::idle && !s->phase_started_ms)
        s->phase_started_ms = now;
    // Bounded catch-up after a long sleep: every queued toast runs its phases.
    for (u32 step = 0; step < kUiToastQueueCapacity * 4 + 4 && advance(s, now, env.animate); ++step) {
    }
    s->progress = phase_progress(*s, now, env.animate);
    if (!active(s->phase) || !ui_rect_has_area(env.viewport)) {
        if (ui_rect_has_area(s->rectangle))
            ++s->revision;
        s->rectangle = s->icon = s->text = {};
        s->viewport = env.viewport;
        return s->revision != old_revision || s->progress != old_progress;
    }
    if (s->measured_serial != s->serial || ui_rect_width(s->viewport) != ui_rect_width(env.viewport)) {
        measure(s, env);
        ++s->revision;
    }
    const UiRect previous = s->rectangle;
    s->viewport = env.viewport;
    place(s, env);
    if (!ui_rect_equal(previous, s->rectangle))
        ++s->revision;
    return s->revision != old_revision || s->progress != old_progress;
}

u64 ui_toast_next_wake(const UiToastScene& s, u64 now, bool animate) noexcept {
    if (s.phase == UiToastPhase::idle)
        return s.count ? now : 0;
    if (!s.phase_started_ms)
        return now;
    const u64 start = s.phase_started_ms;
    switch (s.phase) {
    case UiToastPhase::entering:
    case UiToastPhase::leaving: {
        if (!animate)
            return now;
        const u64 end = start + kUiToastMotionMs;
        return now >= end ? now : minimum(end, now + kUiTransitionFrameMs);
    }
    case UiToastPhase::shown:
        return start + kUiToastHoldMs;
    case UiToastPhase::waiting:
        return start + (animate ? kUiToastHandoverMs - kUiToastMotionMs : 0);
    default:
        return 0;
    }
}

bool ui_toast_visible(const UiToastScene& s) noexcept {
    return s.count && active(s.phase) && ui_rect_has_area(s.rectangle) && s.text_owner.manager &&
           ui_widget_manager_resolve(*s.text_owner.manager, s.text_owner.handle);
}

Utf8View ui_toast_current_text(const UiToastScene& s) noexcept {
    return s.count && s.phase != UiToastPhase::idle && s.phase != UiToastPhase::waiting
               ? Utf8View{s.entries[0].text, s.entries[0].bytes}
               : Utf8View{};
}
UiToastKind ui_toast_current_kind(const UiToastScene& s) noexcept {
    return s.count ? s.entries[0].kind : UiToastKind::message;
}

bool ui_toast_paint(const UiToastScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!ui_toast_visible(s))
        return true;
    const auto v = s.viewport, r = s.rectangle;
    if (!paint || !paint_stream_push_clip(paint, v))
        return false;
    const float radius = theme.scale.radius_md;
    const auto kind = s.entries[0].kind;
    bool ok = ui_surface_shadow_paint(paint, r, theme, UiElevation::popover, radius);
    if (kind == UiToastKind::message) {
        ok = ok && ui_surface_paint_plate(paint, r, theme, UiElevation::tooltip, radius);
    } else {
        // Feedback toasts: the base plate under a tint and an accent edge.
        const bool success = kind == UiToastKind::success;
        const Color accent = success ? theme.tokens.accent.success : theme.tokens.accent.danger;
        const Color base = theme.tokens.background.primary;
        const float edge = ui_surface_style(theme, UiElevation::tooltip).border_width;
        // The tint sits inside the edge, its corners concentric with the plate's.
        ok = ok && ui_surface_paint_plate(paint, r, theme, UiElevation::tooltip, radius, &base, &accent) &&
             paint_stream_push_rounded(paint, r.left + edge, r.top + edge, r.right - edge, r.bottom - edge,
                                       ui_theme_color_alpha(accent, .12f), maximum(0.f, radius - edge)) &&
             ui_icon_paint(paint, success ? UiIcon::circle_check : UiIcon::circle_x, s.icon,
                           ui_icon_role_color(theme, success ? UiIconRole::success : UiIconRole::danger));
    }
    return paint_stream_pop_clip(paint) && ok;
}

bool ui_toast_text_output(const UiToastScene& s, const Theme& theme, UiTextOutput* output) noexcept {
    if (!ui_toast_visible(s))
        return true;
    return ui_text_output_push(output, {{s.entries[0].text, s.entries[0].bytes},
                                        s.text_owner,
                                        s.text,
                                        s.viewport,
                                        theme.tokens.text.normal,
                                        text_style_none,
                                        UiTextAlignment::start,
                                        UiTextAlignment::center,
                                        UiTextWrapping::wrap,
                                        UiTextOverflow::ellipsis});
}

void ui_toast_clear(UiToastScene* s) noexcept {
    if (!s)
        return;
    const auto owner = s->text_owner;
    const u32 serial = s->serial, revision = s->revision + 1;
    reset_value(s);
    s->text_owner = owner;
    s->serial = serial;
    s->revision = revision;
}

} // namespace maicord
