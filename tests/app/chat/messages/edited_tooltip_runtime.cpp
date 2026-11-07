// The tooltip of a message's "(edited)" label: pointing at the label (the renderer's hit test names it; the real one
// runs against shaped text in the message link UI probe) shows the edit time in full after the usual delay, for the
// current revision of a message of the open channel only, and leaves with the pointer.
#include "shell/state.hpp"
#include "shell/tooltip.hpp"
#include "account/account_ui_runtime.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/ui/widgets/tooltip.hpp"

#include "../../../support/test_output.hpp"
#include "../../../support/window_fixture.hpp"

namespace maicord {
namespace edited_tooltip_test {
// The renderer's label under the pointer.
RendererEditedLabelHit hit;
} // namespace edited_tooltip_test
RendererEditedLabelHit renderer_edited_label_hit_test(const D3D11Renderer&, float x, float y) noexcept {
    const auto& hit = edited_tooltip_test::hit;
    return hit.valid && ui_rect_contains(hit.rectangle, x, y) ? hit : RendererEditedLabelHit{};
}

namespace app_detail {
namespace {
// The pointer rests on the label until the tooltip is due; the text shown then, or nothing.
Utf8View hover(UiRect at, u64* deadline) noexcept {
    application_tooltip_input(window_test::pointer_move(static_cast<short>((at.left + at.right) * .5f),
                                                        static_cast<short>((at.top + at.bottom) * .5f)));
    *deadline = application_tooltip_next_wake();
    const auto& tip = *g_app.renderer.tooltip_scene;
    if (!*deadline || !application_tooltip_synchronize(*deadline) || !tip.visible)
        return {};
    return {tip.text, tip.text_bytes};
}
} // namespace

void test_edited_tooltip_owner(void (*check)(bool, const char*) noexcept) noexcept {
    using edited_tooltip_test::hit;
    auto& ui = *g_app.account_ui;
    struct Restore {
        MessageStore store = g_app.message_view.store;
        UiWidgetManager widgets = g_app.message_scene.widgets;
        UiWidgetHandle root = g_app.message_scene.root;
        ~Restore() noexcept {
            application_tooltip_release();
            edited_tooltip_test::hit = {};
            g_app.message_view.store = store;
            g_app.message_scene.widgets = widgets;
            g_app.message_scene.root = root;
        }
    } restore;
    constexpr u64 kEditedSeconds = 1'700'000'100;
    MessageRecord messages[1]{};
    u32 message_order[1]{};
    u8 text[64]{};
    const bool seeded =
        message_store_initialize(&g_app.message_view.store, {messages, 1}, {message_order, 1}, {text, sizeof(text)}) &&
        message_store_upsert(&g_app.message_view.store,
                             {44, 8, 9, 1'700'000'000'000ull, kEditedSeconds * 1'000, utf8_literal("hello"), 0})
            .ok();
    const auto* record = application_message_record(44);
    // The message view: a visible container the label's owner is.
    UiWidget widget_storage[1]{};
    UiLayoutNode node_storage[1]{};
    UiWidgetReference owner{};
    const bool owned =
        ui_widget_manager_initialize_fill_root(&g_app.message_scene.widgets, {widget_storage, 1}, {node_storage, 1},
                                               utf8_literal("Messages"), ui_layout_node_visible, &owner) &&
        ui_widget_manager_layout_rectangle(&g_app.message_scene.widgets, {0, 0, 1000, 700}).ok();
    g_app.message_scene.root = owner.handle;
    ui.active_channel_id = 8;
    check(seeded && record && owned, "edited tooltip: message and message view initialize");
    if (!seeded || !record || !owned)
        return;

    const auto label_at = [&](float left) noexcept {
        return RendererEditedLabelHit{44, kEditedSeconds, {left, 100, left + 52, 116}, record->revision, false, true};
    };
    char expected[kMessageMarkdownTimestampMaximumBytes];
    const auto formatted = format_timestamp_local(kEditedSeconds, 'F', clock_unix_milliseconds() / 1'000,
                                                  {expected, sizeof(expected)});
    const Utf8View time{expected, formatted.ok ? formatted.bytes : 0u};
    // The first eligible pointer creates the host's single tooltip owner.
    application_tooltip_input(window_test::pointer_move(990, 690));
    if (!g_app.renderer.tooltip_scene) {
        check(false, "edited tooltip: the pointer creates the tooltip owner");
        return;
    }
    const auto& tip = *g_app.renderer.tooltip_scene;
    u64 deadline = 0;
    hit = label_at(420);
    const u64 pointed = clock_monotonic_milliseconds();
    const Utf8View shown = hover(hit.rectangle, &deadline);
    check(formatted.ok && time.bytes && utf8_equal(shown, time) && test_contains(shown, utf8_literal("2023")) &&
              deadline >= pointed + kUiTooltipPointerDelayMs,
          "edited tooltip: the label shows its edit time in the long timestamp format after the delay");
    check(tip.placement == UiTooltipPlacement::above && ui_rect_equal(tip.anchor, hit.rectangle) &&
              tip.key.identity == 44 && tip.key.source_revision == record->revision,
          "edited tooltip: it sits above the label and is keyed by the message and its revision");
    check(application_tooltip_synchronize(clock_monotonic_milliseconds()) && tip.visible &&
              !application_tooltip_next_wake(),
          "edited tooltip: a resting pointer keeps it without another wake");
    application_tooltip_input(window_test::pointer_move(701, 400));
    check(!tip.visible && !application_tooltip_next_wake(), "edited tooltip: leaving the label hides it at once");

    // A later edit time shows its own time; a revision that moved on, or another channel, shows nothing.
    hit = label_at(420);
    hit.edited_unix_seconds = kEditedSeconds + 90'000;
    char later[kMessageMarkdownTimestampMaximumBytes];
    const auto later_format = format_timestamp_local(hit.edited_unix_seconds, 'F', clock_unix_milliseconds() / 1'000,
                                                     {later, sizeof(later)});
    check(later_format.ok && utf8_equal(hover(hit.rectangle, &deadline), Utf8View{later, later_format.bytes}),
          "edited tooltip: the time shown is the label's own");
    application_tooltip_clear();
    hit = label_at(460);
    ++hit.message_revision;
    check(!hover(hit.rectangle, &deadline).bytes && !deadline,
          "edited tooltip: a label of a message's old text cannot show");
    hit = label_at(500);
    ui.active_channel_id = 9;
    check(!hover(hit.rectangle, &deadline).bytes && !deadline,
          "edited tooltip: a message outside the open channel cannot show");
    ui.active_channel_id = 8;
    hit = label_at(540);
    check(utf8_equal(hover(hit.rectangle, &deadline), time), "edited tooltip: shown before a click");
    application_tooltip_input(window_test::pointer_down(560, 108));
    check(!tip.visible && !application_tooltip_next_wake(), "edited tooltip: a click hides it");
}
} // namespace app_detail
} // namespace maicord
