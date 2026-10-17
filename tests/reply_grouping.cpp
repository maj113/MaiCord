#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/message_action_bar.hpp"
#include "maicord/ui/navigation_view.hpp"

namespace maicord {
namespace {
constexpr u32 count = 8;
constexpr u32 widgets = count * kMessageSceneWidgetsPerRow + 1;
struct Fixture {
    MessageRecord records[count];
    u32 order[count];
    u8 text[256];
    MessageRichRecord rich_records[count];
    u16 rich_order[count];
    u8 rich_text[1024];
    u32 heights[count];
    u64 fenwick[count];
    UiWidget widget_storage[widgets];
    UiLayoutNode nodes[widgets];
    MessageSceneRowWidget rows[count];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
};
Fixture fixture;
struct TextFixture {
    MessageTextCommand commands[128]; char text[4096]; TextStyleSpan styles[128];
    MessageMarkdownSemantic semantics[32]; MessageTextMeasurement measurements[count];
    MessageTextSpoilerReveal reveals[8]; u32 scratch[4096];
} text_fixture;

bool set_reply(Fixture& f, u64 id, GatewayMessageReferenceState state) noexcept {
    const auto found = message_store_find(f.store, id);
    const MessageRecord* record = found.ok() ? message_store_resolve(f.store, found.handle) : nullptr;
    if (!record) return false;
    GatewayMessageEvent event{};
    event.id = id;
    event.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message;
    event.reference.message_id = 999;
    event.reference.channel_id = 7;
    event.reference.author_id = 9;
    event.reference.state = state;
    if (state == GatewayMessageReferenceState::available) {
        event.reference.author_name = utf8_literal("Someone");
        event.reference.content = utf8_literal("hmmm");
    }
    return message_rich_store_apply(&f.rich, id, record->revision, event).ok();
}
}

void test_reply_grouping(void (*check)(bool, const char*) noexcept) noexcept {
    for (u32 owned = 0; owned < 2; ++owned) {
        Fixture& f = fixture;
        constexpr u64 start = 1'700'000'000'000ull;
        bool ready = message_store_initialize(&f.store, {f.records, count}, {f.order, count}, {f.text, sizeof(f.text)}) &&
            message_rich_store_initialize(&f.rich, {f.rich_records, count}, {f.rich_order, count}, {f.rich_text, sizeof(f.rich_text)});
        for (u32 i = 0; ready && i < count; ++i) {
            const u64 timestamp = start + (i < 6 ? i * 1'000ull : 424'000ull + i - 6);
            ready = message_store_upsert(&f.store, {100 + i, 7, 9, timestamp, 0, utf8_literal("message"), 0}).ok();
        }
        ready = ready && set_reply(f, 101, GatewayMessageReferenceState::available) &&
            set_reply(f, 104, GatewayMessageReferenceState::deleted) &&
            ui_variable_list_initialize(&f.list, {f.heights, count}, {f.fenwick, count}, count, ui_extent_units(57)) &&
            message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(1000), false) &&
            message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
            (owned ? message_scene_initialize_owned(&f.scene, &f.timeline, count, count,
                {f.widget_storage, widgets}, {f.nodes, widgets}) :
                message_scene_initialize(&f.scene, &f.timeline, {f.rows, count}, {f.widget_storage, widgets}, {f.nodes, widgets}));
        const UiRect viewport{0, 0, 740, 1000};
        ready = ready && message_scene_update(&f.scene, viewport).ok();
        const bool grouped[]{false, false, true, true, false, true, false, true};
        for (u32 i = 0; ready && i < count; ++i) {
            const auto* row = message_scene_find_row(f.scene, i);
            const auto* avatar = row ? message_scene_layout_node(f.scene, row->avatar) : nullptr;
            ready = row && row->grouped == grouped[i] && avatar &&
                (ui_rect_height(avatar->rectangle) == 0.0f) == grouped[i];
        }
        check(ready, owned ? "reply_group_owned_start_continue_and_seven_minute_boundary" :
                             "reply_group_borrowed_start_continue_and_seven_minute_boundary");
        const bool mutated = ready && set_reply(f, 102, GatewayMessageReferenceState::unknown) &&
            message_scene_update(&f.scene, viewport).ok();
        const auto* reply = message_scene_find_row(f.scene, 2);
        const auto* next = message_scene_find_row(f.scene, 3);
        check(mutated && reply && !reply->grouped && next && next->grouped,
              "reply_group_rich_mutation_invalidates_measurement_without_splitting_followup");
        bool measured = mutated;
        for (u32 i = 0; measured && i < count; ++i) {
            bool changed = false;
            measured = message_scene_set_text_metrics(&f.scene, i, 18, 30, true, nullptr, nullptr, {}, &changed);
        }
        measured = measured && message_scene_update(&f.scene, viewport).ok();
        const u32 before_height = ui_variable_list_item_units(f.list, 3);
        const u32 before_widgets = f.scene.widgets.count;
        bool changed = false;
        const bool marked = measured && message_scene_set_unread_cursor(&f.scene, 7, 102, true, &changed) && changed &&
            message_scene_update(&f.scene, viewport).ok();
        const auto* boundary = message_scene_find_row(f.scene, 3);
        const auto* following = message_scene_find_row(f.scene, 5);
        const auto divider = boundary ? message_scene_unread_divider_rectangle(f.scene, *boundary) : UiRect{};
        check(marked && boundary && !boundary->grouped && boundary->text_measured &&
            boundary->measured_content_height == 35 && following && following->grouped &&
            ui_rect_height(divider) == 24 && ui_variable_list_item_units(f.list, 3) == before_height + ui_extent_units(59) &&
            f.scene.widgets.count == before_widgets && f.scene.unread_first_message_id == 103,
            "unread_divider_breaks_author_group_preserves_body_measurement_and_uses_existing_widgets");
        MessageActionBar bar{};
        const float x = (divider.left + divider.right) * .5f, y = (divider.top + divider.bottom) * .5f;
        const auto tracked = marked ? message_action_bar_track_pointer(&bar, &f.scene, x, y) : MessageActionBarPointerResult{};
        const auto message_rectangle = boundary ? message_scene_message_rectangle(f.scene, *boundary) : UiRect{};
        const auto* record = boundary ? message_store_resolve(f.store, boundary->message) : nullptr;
        const bool opened = record && message_action_bar_open(&bar, f.scene,
            {record->id, boundary->widget, boundary->source_index}, *record).ok();
        check(marked && !message_scene_hit_test(f.scene, x, y).message_id && !tracked.hovered_message_id &&
            !f.scene.widgets.hovered.generation && opened && bar.rectangle.top >= divider.bottom &&
            message_scene_hit_test(f.scene, x, message_rectangle.top + 1).message_id == 103,
            "unread_divider_strip_is_not_a_message_hit_or_hover_and_action_bar_stays_below_it");
        MessageTextPresentation presentation{};
        auto& t = text_fixture;
        ChatStateStore state{}; NavigationView navigation{};
        const bool text_ready = message_text_presentation_initialize(&presentation, {t.commands, 128},
            {t.text, 4096}, {t.styles, 128}, {t.semantics, 32}, {t.measurements, count}, {t.reveals, 8}, {t.scratch, 4096}) &&
            message_text_presentation_build(&presentation, f.scene, state, nullptr, navigation,
                kDefaultTheme, start / 1000).ok();
        u32 divider_commands = 0;
        bool decoration_only = text_ready;
        for (u32 i = 0; text_ready && i < presentation.command_count; ++i) {
            const auto& command = presentation.commands[i];
            if (!utf8_equal(command.text, kUiUnreadDividerLabel)) continue;
            ++divider_commands;
            decoration_only &= command.message_id == 0 && command.measurement_kind == MessageTextMeasurementKind::none &&
                !command.semantics.count && !message_text_presentation_command_source(f.store, &f.rich, command).bytes &&
                ui_rect_equal(command.rectangle, ui_unread_divider_geometry(divider).label);
        }
        check(text_ready && divider_commands == 1 && decoration_only && presentation.measurement_count == count,
            "unread_divider_label_is_a_single_nonselectable_decoration_not_a_body_measurement");
        PaintStream paint{}; MessageMediaCommand media_storage[1]{}; MessageMediaCommandStream media{};
        MessagePresentationResult result{};
        bool painted = paint_stream_initialize(&paint) && message_media_command_stream_initialize(&media, {media_storage, 1});
        const Theme themes[]{kDefaultTheme, kLightTheme, kAmoledTheme};
        for (const auto& theme : themes) {
            painted &= paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                message_presentation_build(f.scene, theme, &paint, &media, &result);
            const auto line = ui_unread_divider_geometry(divider).line;
            bool found_line = false;
            for (u32 q = 0; q < paint.count; ++q) {
                const auto& quad = paint.quads[q];
                found_line |= quad.left == line.left && quad.right == line.right && quad.top == line.top &&
                    quad.bottom == line.bottom && quad.fill.red == theme.notification.red && quad.fill.green == theme.notification.green;
            }
            painted &= found_line;
        }
        check(painted, "unread_divider_paint_uses_semantic_theme_notification_in_existing_clipped_stream");
        paint_stream_release(&paint);
        const u32 layouts = f.scene.layout_passes, rebuilds = f.scene.rebuilds, preparations = f.scene.preparation_passes;
        bool warm = marked;
        for (u32 i = 0; i < 2; ++i) { // the second frame repeats the first's already-clean state
            changed = true;
            const bool same = message_scene_set_unread_cursor(&f.scene, 7, 102, true, &changed);
            const auto update = message_scene_update(&f.scene, viewport);
            warm &= same && !changed && update.ok() && !update.changed;
        }
        check(warm && layouts == f.scene.layout_passes && rebuilds == f.scene.rebuilds &&
            preparations == f.scene.preparation_passes,
            "unread_divider_unchanged_cursor_frames_do_not_reprepare_or_relayout");
        changed = false;
        check(message_scene_set_unread_cursor(&f.scene, 7, 0, false, &changed) && changed &&
            message_scene_update(&f.scene, viewport).ok() && !f.scene.unread_first_message_id &&
            ui_variable_list_item_units(f.list, 3) == before_height,
            "unread_divider_unknown_cursor_removes_reserved_height_without_changing_body_metrics");
        const UiRect compact{0, 0, 740, 100};
        bool anchored = message_scene_update(&f.scene, compact).ok() &&
            message_timeline_set_scroll(&f.timeline, ui_variable_list_prefix_units(f.list, 5) + ui_extent_units(3), false) &&
            message_scene_update(&f.scene, compact).ok();
        UiScrollAnchor before{}, after{};
        u32 retained_heights[count]{};
        for (u32 i = 0; i < count; ++i) retained_heights[i] = ui_variable_list_item_units(f.list, i);
        anchored &= ui_variable_list_capture_anchor(f.list, f.timeline.scroll_units, &before) &&
            message_scene_set_unread_cursor(&f.scene, 7, 102, true) && message_scene_update(&f.scene, compact).ok() &&
            ui_variable_list_capture_anchor(f.list, f.timeline.scroll_units, &after);
        check(anchored && before.index == after.index && before.within_item_units == after.within_item_units &&
            !f.timeline.follow_bottom,
            "unread_divider_height_change_above_viewport_preserves_existing_scroll_anchor");
        bool resident_heights_preserved = anchored;
        for (u32 i = 0; i < count; ++i) {
            resident_heights_preserved &= ui_variable_list_item_units(f.list, i) == retained_heights[i] +
                (i == 3 ? ui_extent_units(59) : 0);
        }
        check(resident_heights_preserved,
            "unread_cursor_change_preserves_virtualized_body_heights_and_only_changes_boundary_chrome");
        auto taller_style = kDefaultUiUnreadDividerStyle;
        taller_style.height = 36;
        const bool restyled = anchored && message_scene_set_unread_cursor(&f.scene, 7, 102, true, nullptr, &taller_style) &&
            message_scene_update(&f.scene, compact).ok() &&
            ui_variable_list_capture_anchor(f.list, f.timeline.scroll_units, &after);
        check(restyled && before.index == after.index && before.within_item_units == after.within_item_units &&
            ui_variable_list_item_units(f.list, 3) == retained_heights[3] + ui_extent_units(71),
            "unread_divider_style_height_change_preserves_virtualized_body_height_and_scroll_anchor");
        bool bottom = false;
        const bool followed = message_scene_scroll_to_bottom(&f.scene, &bottom) &&
            message_scene_update(&f.scene, compact).ok() && message_scene_set_unread_cursor(&f.scene, 7, 107, true) &&
            message_scene_update(&f.scene, compact).ok();
        check(followed && !f.scene.unread_first_message_id && f.timeline.follow_bottom &&
            message_timeline_is_at_bottom(f.timeline, 0),
            "unread_divider_clear_at_last_cursor_retains_follow_bottom");
        check(message_scene_set_unread_cursor(&f.scene, 99, 0, true) &&
            message_scene_update(&f.scene, compact).ok() && !f.scene.unread_first_message_id,
            "unread_divider_cannot_project_another_channel_into_the_selected_timeline");
        message_scene_release(&f.scene);
    }
}
} // namespace maicord
