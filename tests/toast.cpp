// Toast queue: FIFO order, merge and overflow, phase timing by update and
// wake counts (never wall clock), zero wakes once drained, reduced motion,
// placement, paint and text output.
#include "maicord/ui/toast.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::toast_test {
namespace {
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
constexpr UiRect kViewport{0, 32, 1000, 732};
constexpr u64 kStart = 10'000;

struct Fixture {
    UiWidget widgets[1]{};
    UiLayoutNode nodes[1]{};
    UiWidgetManager manager{};
    UiWidgetReference owner{};
    UiToastScene scene{};
    bool ready{};
    Fixture() noexcept {
        ready = ui_widget_manager_initialize_fill_root(
                    &manager, {widgets, 1}, {nodes, 1}, utf8_literal("Toast"),
                    ui_layout_node_visible | ui_layout_node_allow_overlap | ui_layout_node_clip_children,
                    &owner) &&
                ui_widget_manager_layout_rectangle(&manager, kViewport).ok() &&
                ui_toast_bind_text_owner(&scene, owner);
    }
};
UiToastEnvironment environment(bool animate = true, bool reduced = false,
                               UiTextMeasureCache* measure = nullptr) noexcept {
    return {&kDefaultTheme, measure, kViewport, animate, reduced};
}
bool current_is(const UiToastScene& s, Utf8View text) noexcept {
    return utf8_equal(ui_toast_current_text(s), text);
}
// Runs the host loop from now: update, then sleep until the next wake.
// Returns the updates made; *end is the time of the last one.
struct Run {
    u32 updates, entering, leaving, moved;
    u64 end;
};
Run drain(UiToastScene* s, u64 now, const UiToastEnvironment& env, u32 limit = 400) noexcept {
    Run run{};
    for (; run.updates < limit; ++run.updates) {
        const auto phase = s->phase;
        const float top = s->rectangle.top;
        ui_toast_update(s, now, env);
        run.entering += s->phase == UiToastPhase::entering || phase == UiToastPhase::entering;
        run.leaving += s->phase == UiToastPhase::leaving;
        run.moved += s->rectangle.top != top;
        run.end = now;
        const u64 wake = ui_toast_next_wake(*s, now, env.animate);
        if (!wake)
            return ++run.updates, run;
        now = wake > now ? wake : now + 1;
    }
    return run;
}

void test_queue() noexcept {
    UiToastScene s{};
    check(!ui_toast_next_wake(s, kStart, true) && !ui_toast_visible(s) && !ui_toast_current_text(s).bytes,
          "toast: an empty queue never wakes");
    check(ui_toast_push(&s, UiToastKind::success, utf8_literal("Link Copied!")) &&
              ui_toast_push(&s, UiToastKind::success, utf8_literal("Image Copied!")) && s.count == 2,
          "toast: later toasts queue behind the current one");
    check(!ui_toast_push(&s, UiToastKind::success, utf8_literal("Image Copied!")) && s.count == 2,
          "toast: a repeat of a pending toast is merged");
    check(ui_toast_push(&s, UiToastKind::failure, utf8_literal("Image Copied!")) && s.count == 3,
          "toast: the same text with another kind queues");
    check(ui_toast_push(&s, UiToastKind::message, utf8_literal("Fourth")) &&
              ui_toast_push(&s, UiToastKind::message, utf8_literal("Fifth")) &&
              s.count == kUiToastQueueCapacity && s.dropped == 1 &&
              utf8_equal({s.entries[0].text, s.entries[0].bytes}, utf8_literal("Link Copied!")) &&
              s.entries[1].kind == UiToastKind::failure &&
              utf8_equal({s.entries[3].text, s.entries[3].bytes}, utf8_literal("Fifth")),
          "toast: a full queue keeps the current toast and drops its oldest pending one");
    check(!ui_toast_push(&s, UiToastKind::message, {}) &&
              !ui_toast_push(nullptr, UiToastKind::message, utf8_literal("x")) &&
              !ui_toast_push(&s, UiToastKind::message, utf8_literal("\xff")),
          "toast: empty, null and invalid UTF-8 pushes are refused");
    UiToastScene long_text{};
    char text[201];
    text[0] = 'a';
    for (u32 i = 1; i < 201; i += 2)
        text[i] = static_cast<char>(0xC3), text[i + 1] = static_cast<char>(0xA9);
    check(ui_toast_push(&long_text, UiToastKind::message, {text, 201}) &&
              long_text.entries[0].bytes == 95 &&
              utf8_validate({long_text.entries[0].text, long_text.entries[0].bytes}).valid,
          "toast: long text is truncated at a code point");
}

void test_timing() noexcept {
    Fixture f;
    check(f.ready, "toast: fixture text owner binds");
    ui_toast_push(&f.scene, UiToastKind::success, utf8_literal("Link Copied!"));
    check(ui_toast_next_wake(f.scene, kStart, true) == kStart, "toast: a pushed toast is due at once");
    const auto run = drain(&f.scene, kStart, environment());
    // 300 ms in 16 ms frames each way (19 after the first), one wake for the hold.
    check(run.updates == 40 && run.entering == 20 && run.leaving == 19 &&
              run.end == kStart + kUiToastMotionMs * 2 + kUiToastHoldMs,
          "toast: enter and exit run 300 ms at frame pace around one 3 s hold wake");
    check(!f.scene.count && f.scene.phase == UiToastPhase::idle && !ui_toast_visible(f.scene) &&
              !ui_toast_next_wake(f.scene, run.end, true) && !ui_rect_has_area(f.scene.rectangle),
          "toast: a drained queue hides and stops waking");
    check(run.moved >= 36, "toast: full motion slides the pill in and out");

    ui_toast_push(&f.scene, UiToastKind::success, utf8_literal("Image Saved"));
    const auto hidden = drain(&f.scene, kStart, environment(false));
    check(hidden.updates == 2 && hidden.end == kStart + kUiToastHoldMs,
          "toast: a hidden window wakes only for the hold, without animation frames");

    // Reduced motion fades in place: progress changes, geometry does not.
    ui_toast_push(&f.scene, UiToastKind::success, utf8_literal("Copied ID"));
    ui_toast_update(&f.scene, kStart, environment(true, true));
    const UiRect settled = f.scene.rectangle;
    const u32 revision = f.scene.revision;
    ui_toast_update(&f.scene, kStart + 150, environment(true, true));
    check(f.scene.progress > .49f && f.scene.progress < .51f && ui_rect_equal(settled, f.scene.rectangle) &&
              f.scene.revision == revision && settled.top == kViewport.top + kDefaultTheme.scale.space_4,
          "toast: reduced motion fades without moving or rebuilding");
    ui_toast_clear(&f.scene);
}

void test_order() noexcept {
    Fixture f;
    ui_toast_push(&f.scene, UiToastKind::success, utf8_literal("First"));
    ui_toast_update(&f.scene, kStart, environment());
    ui_toast_push(&f.scene, UiToastKind::failure, utf8_literal("Second"));
    const u32 serial = f.scene.serial;
    ui_toast_update(&f.scene, kStart + kUiToastMotionMs + kUiToastHoldMs, environment());
    check(current_is(f.scene, utf8_literal("First")) && f.scene.phase == UiToastPhase::leaving &&
              f.scene.serial == serial,
          "toast: a queued toast never replaces the showing one");
    const u64 left = kStart + kUiToastMotionMs * 2 + kUiToastHoldMs;
    ui_toast_update(&f.scene, left, environment());
    check(f.scene.phase == UiToastPhase::waiting && !ui_toast_visible(f.scene) &&
              ui_toast_next_wake(f.scene, left, true) == left + kUiToastHandoverMs - kUiToastMotionMs,
          "toast: the next toast enters 400 ms after the previous one started leaving");
    ui_toast_update(&f.scene, left + 100, environment());
    check(current_is(f.scene, utf8_literal("Second")) &&
              ui_toast_current_kind(f.scene) == UiToastKind::failure &&
              f.scene.phase == UiToastPhase::entering && f.scene.serial == serial + 1,
          "toast: queued toasts show in order, each with a new serial");

    // The same toast while it shows starts its time over.
    UiToastScene& s = f.scene;
    ui_toast_clear(&s);
    ui_toast_push(&s, UiToastKind::success, utf8_literal("Link Copied!"));
    ui_toast_update(&s, kStart, environment());
    ui_toast_update(&s, kStart + 1'000, environment());
    check(ui_toast_push(&s, UiToastKind::success, utf8_literal("Link Copied!")) && s.count == 1,
          "toast: repeating the showing toast keeps one");
    ui_toast_update(&s, kStart + 1'500, environment());
    check(ui_toast_next_wake(s, kStart + 1'500, true) == kStart + 1'500 + kUiToastHoldMs,
          "toast: repeating the showing toast restarts its hold");

    // A long sleep catches up through every queued phase in one update.
    ui_toast_clear(&s);
    ui_toast_push(&s, UiToastKind::success, utf8_literal("A"));
    ui_toast_push(&s, UiToastKind::success, utf8_literal("B"));
    ui_toast_update(&s, kStart, environment());
    ui_toast_update(&s, kStart + 60'000, environment());
    check(!s.count && s.phase == UiToastPhase::idle && !ui_toast_next_wake(s, kStart + 60'000, true),
          "toast: an update after a long sleep retires every expired toast");
}

bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    const float width = request.text.bytes * 7.f;
    const u32 lines = width <= request.constraint.width ? 1 : 3;
    *value = {{minimum(width, request.constraint.width), 20.f * lines}, 15, lines};
    return true;
}

struct Captured {
    UiTextCommand commands[4];
    u32 count;
};
bool capture(void* context, const UiTextCommand& command, u32) noexcept {
    auto& c = *static_cast<Captured*>(context);
    if (c.count < 4)
        c.commands[c.count] = command;
    ++c.count;
    return true;
}

void test_presentation() noexcept {
    Fixture f;
    UiTextMeasureRecord records[4]{};
    UiTextMeasureCache cache{};
    check(ui_text_measure_cache_initialize(&cache, {records, 4}, measure, nullptr), "toast: measure cache");
    const auto& theme = kDefaultTheme;
    ui_toast_push(&f.scene, UiToastKind::success, utf8_literal("Link Copied!"));
    ui_toast_update(&f.scene, kStart, environment(true, false, &cache));
    ui_toast_update(&f.scene, kStart + kUiToastMotionMs, environment(true, false, &cache));
    const auto r = f.scene.rectangle;
    const float padding = theme.scale.space_4 + theme.scale.space_2;
    check(ui_toast_visible(f.scene) && f.scene.progress == 1 &&
              r.top == kViewport.top + theme.scale.space_4 &&
              (r.left + r.right) * .5f == (kViewport.left + kViewport.right) * .5f &&
              ui_rect_width(r) == padding * 2 + kUiToastIconSize + theme.scale.space_4 + 12 * 7.f &&
              ui_rect_height(r) == padding * 2 + 20,
          "toast: the settled pill sits at the top centre, sized to its icon and text");

    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint), "toast: paint stream");
    check(ui_toast_paint(f.scene, theme, &paint) && paint.count > 0, "toast: paints a visible toast");
    const float radius = theme.scale.radius_md;
    bool plate = false, tint = false, icon = false;
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        plate |= q.left == r.left && q.top == r.top && q.right == r.right && q.bottom == r.bottom &&
                 q.radius == radius;
        tint |= q.left == r.left + 1 && q.top == r.top + 1 && q.right == r.right - 1 &&
                q.bottom == r.bottom - 1 && q.radius == radius - 1;
        icon |= ui_icon_quad(q) && ui_icon_quad_data(q).icon == static_cast<u32>(UiIcon::circle_check);
    }
    check(plate && tint && icon,
          "toast: a success toast has its plate, a concentric inset tint and the check icon");

    Captured captured{};
    UiTextEmitter emitter{};
    UiTextOutput output{nullptr, &emitter};
    check(ui_text_emitter_initialize(&emitter, capture, &captured) &&
              ui_toast_text_output(f.scene, theme, &output) && captured.count == 1 &&
              utf8_equal(captured.commands[0].text, utf8_literal("Link Copied!")) &&
              ui_rect_equal(captured.commands[0].clip, kViewport) &&
              captured.commands[0].color.red == theme.tokens.text.normal.red &&
              captured.commands[0].owner.manager == &f.manager,
          "toast: emits its text once, clipped to the window, in the normal text colour");

    // Two lines at most: longer text is clamped to the maximum lines.
    ui_toast_clear(&f.scene);
    ui_toast_push(&f.scene, UiToastKind::message,
                  utf8_literal("A message toast with enough words to wrap past the widest pill size"));
    ui_toast_update(&f.scene, kStart, environment(true, false, &cache));
    ui_toast_update(&f.scene, kStart + kUiToastMotionMs, environment(true, false, &cache));
    paint_stream_reset(&paint);
    bool message_icon = false;
    check(ui_toast_paint(f.scene, theme, &paint), "toast: paints a message toast");
    for (u32 i = 0; i < paint.count; ++i)
        message_icon |= ui_icon_quad(paint.quads[i]);
    check(ui_rect_width(f.scene.rectangle) == kUiToastMaximumWidth &&
              ui_rect_height(f.scene.rectangle) == padding * 2 + 20 * kUiToastMaximumLines && !message_icon,
          "toast: message toasts have no icon, cap at 280 DIP and two lines");
    paint_stream_release(&paint);

    // Without a text owner nothing is presented.
    UiToastScene unbound{};
    ui_toast_push(&unbound, UiToastKind::success, utf8_literal("x"));
    ui_toast_update(&unbound, kStart, environment());
    check(!ui_toast_visible(unbound), "toast: an unbound scene stays hidden");
}
} // namespace
} // namespace maicord::toast_test

extern "C" __declspec(noreturn) void WINAPI maicord_toast_tests_entry() noexcept {
    using namespace maicord::toast_test;
    test_queue();
    test_timing();
    test_order();
    test_presentation();
    const bool failed = failures != 0;
    constexpr char pass[] = "PASS toast\n", fail[] = "FAIL toast\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
