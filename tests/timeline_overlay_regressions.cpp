// Regressions for the timeline overlays reported in live use: loading spinner
// frames, inbox scrolling, the hover toolbar under the unread banner and while
// scrolling, and Add Reaction opening the picker for its message.
#include "maicord/app/message_timeline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/reaction_preferences.hpp"
#include "maicord/ui/inbox_scene.hpp"
#include "maicord/ui/message_action_bar.hpp"
#include "maicord/ui/message_action_bar_presentation.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/scroll_host.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/ui/unread_banner_scene.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

u64 paint_hash(const PaintStream& paint) noexcept {
    return hash64_bytes(kHash64Seed, paint.quads, paint.count * sizeof(PaintQuad));
}

void test_spinner_clock(Check check) noexcept {
    UiSpinnerClock clock{};
    // A visible spinner samples a phase and arms the next 32 ms step.
    const bool first = ui_spinner_clock_tick(&clock, 1'000, true, false);
    check(first && clock.visible && clock.phase == ui_spinner_phase(1'000, false) &&
              clock.wake_ms == ui_spinner_next_wake(1'000, true, false) && clock.wake_ms > 1'000 &&
              !ui_spinner_clock_due(clock, clock.wake_ms - 1) && ui_spinner_clock_due(clock, clock.wake_ms),
          "spinner clock: a visible loading spinner arms its next frame and becomes due on time");
    const u64 wake = clock.wake_ms;
    const u8 phase = clock.phase;
    check(ui_spinner_clock_tick(&clock, wake, true, false) && clock.phase != phase && clock.wake_ms > wake,
          "spinner clock: each due frame advances the painted phase (the spinner animates)");
    check(!ui_spinner_clock_tick(&clock, clock.wake_ms - 1, true, false) || clock.wake_ms > wake,
          "spinner clock: an early repaint keeps a monotonic deadline");
    check(!ui_spinner_clock_tick(&clock, clock.wake_ms + 5, false, false) && !clock.visible && !clock.wake_ms &&
              !ui_spinner_clock_due(clock, ~u64{0} >> 1),
          "spinner clock: hiding the spinner disarms the clock so nothing polls");
    check(ui_spinner_clock_tick(&clock, 5'000, true, true) && !clock.wake_ms && clock.phase == 0 &&
              !ui_spinner_clock_due(clock, 6'000),
          "spinner clock: reduced motion paints a still spinner without scheduling frames");
}

void test_scene_spinners(Check check) noexcept {
    static PaintStream paint{};
    if (!paint_stream_initialize(&paint)) {
        check(false, "scene spinners: paint stream initialization");
        return;
    }
    UiInboxContent loading{};
    loading.revision = 1;
    loading.phase = UiInboxPhase::loading;
    UiInboxScene inbox{};
    const UiRect viewport{240, 48, 1280, 800};
    bool ok = ui_inbox_scene_update(&inbox, nullptr, &loading, viewport) && ui_inbox_scene_spinner_visible(inbox);
    inbox.spinner_phase = 0;
    ok = ok && paint_stream_reset(&paint) && ui_inbox_scene_paint(inbox, kDefaultTheme, &paint);
    const u64 inbox_still = paint_hash(paint);
    inbox.spinner_phase = 9;
    ok = ok && paint_stream_reset(&paint) && ui_inbox_scene_paint(inbox, kDefaultTheme, &paint);
    check(ok && inbox_still != paint_hash(paint),
          "scene spinners: the inbox loading spinner paints the host-sampled phase, not a constant");
    UiInboxContent ready = loading;
    ready.revision = 2;
    ready.phase = UiInboxPhase::ready;
    check(ui_inbox_scene_update(&inbox, nullptr, &ready, viewport) && !ui_inbox_scene_spinner_visible(inbox),
          "scene spinners: a loaded inbox stops requesting spinner frames");
    ui_inbox_scene_release(&inbox);

    UiPinnedMessagesContent pins_loading{};
    pins_loading.channel = utf8_literal("# notes");
    pins_loading.channel_id = 71;
    pins_loading.revision = 1;
    pins_loading.phase = UiPinnedMessagesPhase::loading;
    UiPinnedMessagesScene pins{};
    ok = ui_pinned_messages_scene_update(&pins, nullptr, &pins_loading, {100, 48, 1300, 780}) &&
         ui_pinned_messages_scene_spinner_visible(pins);
    pins.spinner_phase = 0;
    ok = ok && paint_stream_reset(&paint) && ui_pinned_messages_scene_paint(pins, kDefaultTheme, &paint);
    const u64 pins_still = paint_hash(paint);
    pins.spinner_phase = 17;
    ok = ok && paint_stream_reset(&paint) && ui_pinned_messages_scene_paint(pins, kDefaultTheme, &paint);
    check(ok && pins_still != paint_hash(paint),
          "scene spinners: the Loading pins spinner rotates with the shared phase");
    UiPinnedMessagesContent pins_ready = pins_loading;
    pins_ready.revision = 2;
    pins_ready.phase = UiPinnedMessagesPhase::ready;
    check(ui_pinned_messages_scene_update(&pins, nullptr, &pins_ready, pins.viewport) &&
              !ui_pinned_messages_scene_spinner_visible(pins),
          "scene spinners: an empty, loaded pin list shows no spinner and schedules no frames");
    ui_pinned_messages_scene_release(&pins);
    paint_stream_release(&paint);
}

bool inbox_resolve(void*, u32 index, UiInboxRow* row) noexcept {
    *row = {UiInboxRowKind::mention, 900 + index, 70, 9, utf8_literal("Ana"), utf8_literal("#general"),
            utf8_literal("@you a long mention history"), utf8_literal("now")};
    return true;
}
bool inbox_scroll_read(void* context, u64, UiScrollRange* range) noexcept {
    auto& scene = *static_cast<UiInboxScene*>(context);
    *range = {scene.body, scene.scroll_dips, ui_inbox_scene_scroll_maximum(scene), 40., 1., 0., 1};
    return true;
}
bool inbox_scroll_write(void* context, u64, double offset, bool* changed) noexcept {
    auto& scene = *static_cast<UiInboxScene*>(context);
    const float before = scene.scroll_dips;
    const bool ok = ui_inbox_scene_scroll(&scene, static_cast<float>(offset));
    *changed = before != scene.scroll_dips;
    return ok;
}

void test_inbox_scrolling(Check check) noexcept {
    UiInboxSource source{nullptr, 60, 1, inbox_resolve, nullptr};
    UiInboxContent content{};
    content.revision = 1;
    content.phase = UiInboxPhase::ready;
    content.has_more = true;
    UiInboxScene scene{};
    const bool updated = ui_inbox_scene_update(&scene, &source, &content, {240, 48, 1280, 800});
    const auto top = ui_inbox_scene_scrollbar(scene);
    check(updated && top.visible && top.thumb.top <= scene.body.top + 4 && top.track.right == scene.body.right &&
              ui_rect_has_area(scene.footer),
          "inbox scrolling: a long mention history shows an overlay scrollbar beside Load more");
    // The popout takes the same animated wheel curve as the timeline.
    UiScrollHost host{};
    const UiScrollBinding binding{&scene, 1, inbox_scroll_read, inbox_scroll_write};
    const UiPoint inside{(scene.body.left + scene.body.right) * .5f, scene.body.top + 40};
    const bool queued = ui_scroll_host_wheel(&host, binding, inside, -120, 3, false, 1'000) ==
                        UiScrollDispatch::handled;
    const float before_tick = scene.scroll_dips;
    bool changed = false;
    const bool ticked = ui_scroll_host_tick(&host, 1'000 + 16'000, &changed);
    const float partial = scene.scroll_dips;
    for (u64 t = 32'000; t <= 600'000 && ui_scroll_host_active(host); t += 16'000)
        ui_scroll_host_tick(&host, 1'000 + t, &changed);
    const float settled = scene.scroll_dips;
    check(queued && before_tick == 0 && ticked && partial > 0 && partial < settled && settled > 119.5f && settled < 120.5f &&
              !ui_scroll_host_active(host),
          "inbox scrolling: wheel input animates through intermediate offsets to the target");
    const auto moved = ui_inbox_scene_scrollbar(scene);
    check(moved.visible && moved.thumb.top > top.thumb.top &&
              ui_scrollbar_scroll_from_pointer(moved, moved.thumb.top + 3, 3) > 119.f &&
              ui_scrollbar_scroll_from_pointer(moved, moved.thumb.top + 3, 3) < 121.f,
          "inbox scrolling: the thumb reports the position and maps back for dragging");
    const u32 revision = scene.revision;
    check(ui_inbox_scene_set_scrollbar_state(&scene, UiScrollbarVisualState::pressed) && scene.revision != revision &&
              !ui_inbox_scene_set_scrollbar_state(&scene, UiScrollbarVisualState::pressed),
          "inbox scrolling: thumb hover/press state repaints only on change");
    // Load more stays reachable at the end of the list.
    ui_inbox_scene_scroll(&scene, 1e6f);
    const auto more = ui_inbox_scene_hit_test(scene, (scene.footer.left + scene.footer.right) * .5f,
                                              (scene.footer.top + scene.footer.bottom) * .5f);
    check(scene.scroll_dips == ui_inbox_scene_scroll_maximum(scene) && more.action == UiInboxAction::load_more,
          "inbox scrolling: Load more still answers at the bottom of the history");
    ui_inbox_scene_release(&scene);
}

struct TimelineFixture {
    MessageRecord records[2];
    u32 order[2];
    u8 source_text[128];
    MessageStore store;
    u32 item_units[2];
    u64 fenwick[2];
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
    MessageSceneRowWidget rows[2];
    UiWidget widgets[2 * kMessageSceneWidgetsPerRow + 1];
    UiLayoutNode layout_nodes[2 * kMessageSceneWidgetsPerRow + 1];
    ReactionPreferenceStore preferences;
};

bool timeline_ready(TimelineFixture& f, UiRect viewport) noexcept {
    const u32 units = ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height);
    return message_store_initialize(&f.store, Slice<MessageRecord>{f.records, 2}, Slice<u32>{f.order, 2},
                                    Slice<u8>{f.source_text, 128}) &&
           message_store_upsert(&f.store, MessageInput{71, 7, 9, 1'700'000'000'000ull, 0, utf8_literal("first"), 0})
               .ok() &&
           message_store_upsert(&f.store, MessageInput{72, 7, 10, 1'700'000'500'000ull, 0, utf8_literal("second"), 0})
               .ok() &&
           ui_variable_list_initialize(&f.list, Slice<u32>{f.item_units, 2}, Slice<u64>{f.fenwick, 2}, 2, units) &&
           message_timeline_initialize(&f.timeline, &f.store, &f.list, units, ui_extent_units(360.0f), true) &&
           message_scene_initialize(&f.scene, &f.timeline, Slice<MessageSceneRowWidget>{f.rows, 2},
                                    Slice<UiWidget>{f.widgets, 2 * kMessageSceneWidgetsPerRow + 1},
                                    Slice<UiLayoutNode>{f.layout_nodes, 2 * kMessageSceneWidgetsPerRow + 1}) &&
           message_scene_update(&f.scene, viewport).ok() && f.scene.count == 2 &&
           reaction_preferences_initialize(&f.preferences);
}

bool open_bar(TimelineFixture& f, MessageActionBar* bar, u32 row) noexcept {
    const auto& widget = f.scene.rows[row];
    const auto found = message_store_find(f.store, widget.message_id);
    const auto* record = found.ok() ? message_store_resolve(f.store, found.handle) : nullptr;
    return record && message_action_bar_open(bar, f.scene, {widget.message_id, widget.widget, widget.source_index},
                                             *record, &f.preferences)
                         .ok();
}

void test_action_bar_occlusion(Check check) noexcept {
    static TimelineFixture f{};
    const UiRect viewport{100.0f, 40.0f, 700.0f, 400.0f};
    static MessageActionBar bar{};
    if (!timeline_ready(f, viewport) || !open_bar(f, &bar, 0)) {
        check(false, "action bar occlusion: timeline fixture");
        return;
    }
    const UiRect rectangle = bar.rectangle;
    check(ui_rect_equal(bar.clip, viewport) && ui_rect_equal(message_action_bar_visible_rectangle(bar), rectangle),
          "action bar occlusion: an uncovered toolbar is clipped to the timeline viewport only");

    // A banner over the toolbar's top half: the toolbar slides under it.
    const UiRect banner{viewport.left + 16, viewport.top, viewport.right - 16, rectangle.top + 12};
    const UiRect occluders[2]{banner, {}};
    bool changed = false;
    const bool clipped = message_action_bar_set_occluders(&bar, occluders, 2, &changed);
    const UiRect visible = message_action_bar_visible_rectangle(bar);
    check(clipped && changed && bar.active && visible.top == banner.bottom && visible.bottom == rectangle.bottom &&
              !ui_rect_has_area(ui_rect_intersection(visible, banner)),
          "action bar occlusion: the unread banner clips the toolbar instead of being drawn over");
    const float x = (bar.items[3].rectangle.left + bar.items[3].rectangle.right) * .5f;
    check(!message_action_bar_hit_test(bar, x, rectangle.top + 4).inside &&
              message_action_bar_hit_test(bar, x, rectangle.bottom - 4).action == MessageActionBarAction::add_reaction,
          "action bar occlusion: the covered strip belongs to the banner; the visible strip still hits");

    static PaintStream paint{};
    UiTextCommand commands[kMessageActionBarQuickReactionCount]{};
    UiTextCommandStream text{};
    bool presented = paint_stream_initialize(&paint) && paint_stream_reset(&paint) &&
                     ui_text_command_stream_initialize(
                         &text, Slice<UiTextCommand>{commands, kMessageActionBarQuickReactionCount}) &&
                     message_action_bar_presentation_build(bar, kDefaultTheme, &paint) &&
                     message_action_bar_text_presentation_build(bar, kDefaultTheme, &text);
    for (u32 i = 0; presented && i < paint.batch_count; ++i) {
        const auto& clip = paint.clips[paint.batches[i].clip_index];
        presented = clip.top >= banner.bottom && clip.bottom <= rectangle.bottom;
    }
    for (u32 i = 0; presented && i < text.count; ++i)
        presented = text.storage[i].clip.top >= banner.bottom;
    check(presented && paint.batch_count && text.count == kMessageActionBarQuickReactionCount,
          "action bar occlusion: toolbar paint and quick-reaction glyphs never reach the banner");

    // A reopen on the same row (pointer motion) keeps the published overlays.
    MessageActionBar reopened = bar;
    message_action_bar_close(&reopened);
    check(open_bar(f, &reopened, 0) && reopened.clip.top == banner.bottom,
          "action bar occlusion: overlays persist across hover reopen");

    const UiRect cover[2]{{viewport.left, viewport.top, viewport.right, rectangle.bottom + 4}, {}};
    check(message_action_bar_set_occluders(&bar, cover, 2, &changed) && changed && !bar.active &&
              !message_action_bar_hit_test(bar, x, rectangle.bottom - 4).inside,
          "action bar occlusion: a toolbar entirely under an overlay closes");
    check(message_action_bar_set_occluders(&bar, nullptr, 0, &changed) && open_bar(f, &bar, 0) &&
              ui_rect_equal(bar.clip, viewport),
          "action bar occlusion: retired overlays restore the full timeline clip");

    // The real unread banner publishes its painted surfaces as occluders.
    static UiUnreadBannerScene unread{};
    char label[kUiUnreadBannerLabelBytes];
    const u32 label_bytes = ui_unread_banner_format_label(3, false, {}, {label, sizeof(label)});
    UiUnreadBannerView view{7, 0, 3, false, true, true};
    UiRect published[2]{};
    const bool banner_ready =
        ui_unread_banner_update(&unread, view, {label, label_bytes}, viewport,
                                {&f.scene.widgets, f.scene.root}, 1'000, false);
    ui_unread_banner_occluders(unread, published);
    check(banner_ready && ui_rect_equal(published[0], ui_rect_intersection(unread.banner, viewport)) &&
              published[0].top == viewport.top && ui_rect_equal(published[1], unread.pill) &&
              published[1].bottom <= viewport.bottom,
          "action bar occlusion: the unread banner and jump pill are the published occluders");
    view.banner = view.jump = false;
    ui_unread_banner_update(&unread, view, {}, viewport, {&f.scene.widgets, f.scene.root}, 2'000, false);
    ui_unread_banner_occluders(unread, published);
    check(!ui_rect_has_area(published[0]) && !ui_rect_has_area(published[1]),
          "action bar occlusion: hidden banners occlude nothing");
    paint_stream_release(&paint);
}

void test_hover_scroll_gate(Check check) noexcept {
    MessageHoverScrollGate gate{};
    check(message_hover_scroll_gate_pointer(&gate, 300, 200) && !message_hover_scroll_gate_observe(&gate, 1, 0) &&
              !message_hover_scroll_gate_observe(&gate, 1, 0),
          "hover scroll gate: a still timeline never suppresses hover");
    check(message_hover_scroll_gate_observe(&gate, 1, 640) && gate.suppressed &&
              !message_hover_scroll_gate_observe(&gate, 1, 1280),
          "hover scroll gate: the first scrolled frame suppresses hover exactly once");
    check(!message_hover_scroll_gate_pointer(&gate, 300, 200) && gate.suppressed,
          "hover scroll gate: a repeated WM_MOUSEMOVE at the same point keeps the toolbar hidden");
    check(message_hover_scroll_gate_pointer(&gate, 301, 200) && !gate.suppressed,
          "hover scroll gate: real pointer movement restores hover");
    check(message_hover_scroll_gate_observe(&gate, 2, 1280) && !message_hover_scroll_gate_pointer(&gate, 301, 200),
          "hover scroll gate: a programmatic jump (timeline reset) hides hover like a scroll");

    // Host order: scroll observed -> toolbar closed -> no reopen until motion.
    static TimelineFixture f{};
    static MessageActionBar bar{};
    if (!timeline_ready(f, {100.0f, 40.0f, 700.0f, 400.0f}) || !open_bar(f, &bar, 1)) {
        check(false, "hover scroll gate: timeline fixture");
        return;
    }
    MessageHoverScrollGate host{};
    const float x = (bar.rectangle.left + bar.rectangle.right) * .5f, y = bar.rectangle.bottom + 6;
    message_hover_scroll_gate_pointer(&host, x, y);
    message_hover_scroll_gate_observe(&host, f.timeline.reset_generation, 0);
    bool closed = false;
    if (message_hover_scroll_gate_observe(&host, f.timeline.reset_generation, 96))
        message_action_bar_close(&bar, &closed);
    const bool hidden = !bar.active;
    const bool gated = !message_hover_scroll_gate_pointer(&host, x, y);
    bool reopened = false;
    if (message_hover_scroll_gate_pointer(&host, x, y + 1))
        reopened = message_action_bar_track_pointer(&bar, &f.scene, x, y + 1, &f.preferences).ok() && bar.active;
    check(closed && hidden && gated && reopened,
          "hover scroll gate: scrolling hides the toolbar until the pointer moves, then hover resumes");
}

void test_add_reaction_picker(Check check) noexcept {
    static TimelineFixture f{};
    static MessageActionBar bar{};
    const UiRect viewport{312.0f, 48.0f, 1040.0f, 700.0f};
    if (!timeline_ready(f, viewport) || !open_bar(f, &bar, 1)) {
        check(false, "add reaction: timeline fixture");
        return;
    }
    u32 add = kMessageActionBarNoItem;
    for (u32 i = 0; i < bar.item_count; ++i)
        if (bar.items[i].action == MessageActionBarAction::add_reaction)
            add = i;
    const auto cell = add < bar.item_count ? bar.items[add].rectangle : UiRect{};
    const auto hit = message_action_bar_hit_test(bar, (cell.left + cell.right) * .5f, (cell.top + cell.bottom) * .5f);
    check(hit.action == MessageActionBarAction::add_reaction && hit.message_id == f.scene.rows[1].message_id &&
              ui_rect_equal(hit.rectangle, cell),
          "add reaction: the toolbar button targets its own message and anchors at the button");

    // The popout is the shared emoji picker: no tab selector, beside the
    // lower-half Add Reaction button, inside the client area.
    struct Emoji {
        static bool resolve(void*, u32 index, UiMediaPickerItem* item) noexcept {
            *item = {};
            item->id = 100 + index;
            item->kind = UiMediaPickerItemKind::emoji;
            item->enabled = true;
            item->label = utf8_literal("grinning_face");
            item->glyph = utf8_literal("\xF0\x9F\x98\x80");
            return true;
        }
    };
    const UiMediaPickerSection sections[]{{1, 0, 40, utf8_literal("Frequently Used"), true,
                                           UiMediaPickerSectionVisual::recent}};
    UiMediaPickerSource source{nullptr, 3, 7, 40, 1, {sections, 1}, Emoji::resolve};
    UiMediaPickerContent content{};
    content.account_scope = 3;
    content.query_generation = 7;
    content.revision = 1;
    content.kind = UiMediaPickerKind::emoji;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::emoji);
    content.variant = UiMediaPickerVariant::reaction;
    content.edit.active = true;
    static UiMediaPickerScene popout{};
    const UiRect client{0, 32, 1280, 800};
    const UiRect anchor = hit.rectangle;
    const bool opened = ui_media_picker_scene_update(&popout, &source, &content, client, anchor);
    check(opened && !popout.tab_widgets[0].generation && !popout.tab_widgets[1].generation &&
              !popout.tab_widgets[2].generation && popout.field.top < popout.panel.top + 16 &&
              popout.panel.right <= anchor.left && popout.panel.top >= client.top &&
              popout.panel.bottom <= client.bottom && ui_rect_width(popout.panel) == kUiMediaPickerReactionWidth,
          "add reaction: the popout opens for that message beside its button, inside the client area, "
          "without the GIF/Sticker/Emoji selector");
    ui_media_picker_scene_release(&popout);
}
} // namespace

void test_timeline_overlay_regressions(void (*check)(bool, const char*) noexcept) noexcept {
    test_spinner_clock(check);
    test_scene_spinners(check);
    test_inbox_scrolling(check);
    test_action_bar_occlusion(check);
    test_hover_scroll_gate(check);
    test_add_reaction_picker(check);
}
} // namespace maicord
