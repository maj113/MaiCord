// The typing indicator draws inside a strip that is always reserved above the composer surface: showing or
// hiding it moves no message, scrolls nothing and lays nothing out again, it only repaints the strip.
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "../../../support/composer_height_model.hpp"
#include "maicord/ui/chat/messages/scene.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/shell/presentation.hpp"

namespace maicord {
namespace {

constexpr u32 kRows = 14;
constexpr u32 kWidgets = kRows * kMessageSceneWidgetsPerRow + 1;
constexpr u64 kStart = 1'700'000'000'000ull;

struct Fixture {
    MessageRecord records[kRows];
    u32 order[kRows];
    u8 text[512];
    u32 heights[kRows];
    u64 fenwick[kRows];
    UiWidget widget_storage[kWidgets];
    UiLayoutNode nodes[kWidgets];
    MessageStore store;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
    UiScene shell;
    char draft[128];
    MessageComposer model;
    MessageComposerScene composer;
};
Fixture fixture;

// The rectangle of every control the strip could disturb, in a fixed order.
constexpr MessageComposerControl kControls[]{
    MessageComposerControl::typing_indicator, MessageComposerControl::edit,  MessageComposerControl::add_attachment,
    MessageComposerControl::gif,              MessageComposerControl::sticker, MessageComposerControl::emoji,
    MessageComposerControl::send};
struct Geometry {
    UiRect viewport, controls[sizeof(kControls) / sizeof(kControls[0])], surface;
    u32 layout_passes;
    float measured;
};

bool capture(const MessageComposerScene& scene, Geometry* geometry) noexcept {
    const UiLayoutNode* surface = ui_widget_manager_layout_node(scene.widgets, scene.surface);
    if (!surface)
        return false;
    geometry->viewport = scene.viewport;
    geometry->surface = surface->rectangle;
    geometry->layout_passes = scene.layout_passes;
    geometry->measured = message_composer_scene_measured_height(scene, 60.f);
    for (u32 i = 0; i < sizeof(kControls) / sizeof(kControls[0]); ++i) {
        const UiLayoutNode* node = message_composer_scene_layout_node(scene, kControls[i]);
        if (!node)
            return false;
        geometry->controls[i] = node->rectangle;
    }
    return true;
}

bool same(const Geometry& a, const Geometry& b) noexcept {
    bool equal = ui_rect_equal(a.viewport, b.viewport) && ui_rect_equal(a.surface, b.surface) &&
                 a.layout_passes == b.layout_passes && a.measured == b.measured;
    for (u32 i = 0; i < sizeof(kControls) / sizeof(kControls[0]); ++i)
        equal = equal && ui_rect_equal(a.controls[i], b.controls[i]);
    return equal;
}

struct Rows {
    UiRect rectangles[kRows];
    u32 resident;
};

Rows rows_of(const MessageScene& scene) noexcept {
    Rows rows{};
    for (u32 i = 0; i < kRows; ++i) {
        const auto* row = message_scene_find_row(scene, i);
        rows.rectangles[i] = row ? message_scene_message_rectangle(scene, *row) : UiRect{};
        rows.resident += row != nullptr;
    }
    return rows;
}

bool same(const Rows& a, const Rows& b) noexcept {
    bool equal = a.resident == b.resident;
    for (u32 i = 0; i < kRows; ++i)
        equal = equal && ui_rect_equal(a.rectangles[i], b.rectangles[i]);
    return equal;
}

} // namespace

void test_typing_strip(void (*check)(bool, const char*) noexcept) noexcept {
    Fixture& f = fixture;
    bool ready = message_store_initialize(&f.store, {f.records, kRows}, {f.order, kRows}, {f.text, sizeof(f.text)});
    for (u32 i = 0; ready && i < kRows; ++i)
        ready = message_store_upsert(&f.store, {100 + i, 7, 9, kStart + i * 1'000ull, 0, utf8_literal("message"), 0})
                    .ok();
    ready = ready &&
            ui_variable_list_initialize(&f.list, {f.heights, kRows}, {f.fenwick, kRows}, kRows, ui_extent_units(57)) &&
            message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(1000),
                                        true) &&
            message_scene_initialize_owned(&f.scene, &f.timeline, kRows, kRows, {f.widget_storage, kWidgets},
                                           {f.nodes, kWidgets}) &&
            message_composer_initialize(&f.model, {f.draft, sizeof(f.draft)}) &&
            message_composer_scene_initialize_owned(&f.composer) && ui_scene_initialize_bootstrap(&f.shell);
    check(ready, "typing_strip_fixture");
    if (!ready) {
        message_scene_release(&f.scene);
        message_composer_scene_release(&f.composer);
        ui_scene_release(&f.shell);
        return;
    }

    // The shell as the application lays it out: the composer's height and its strip's overlap over the list's
    // bottom space are the only inputs, and neither names typers.
    constexpr float kWidth = 1280.f, kHeight = 420.f;
    UiShellLayoutMetrics metrics = kGuildChannelShellLayoutMetrics;
    const auto layout_shell = [&]() noexcept {
        metrics.composer_height =
            message_composer_scene_preferred_height(f.model, ui_scene_composer_width(metrics, kWidth), false);
        metrics.composer_overlap = message_composer_scene_chrome_height(false, 0);
        return ui_scene_layout_with_metrics(&f.shell, UiShellView::guild_channel, metrics, kWidth, kHeight);
    };
    const auto rectangle_of = [&](UiSemantic semantic) noexcept {
        const UiLayoutNode* node = ui_scene_node(f.shell, semantic);
        return node ? node->rectangle : UiRect{};
    };
    const bool laid_out = layout_shell() && ui_rect_has_area(rectangle_of(UiSemantic::message_view)) &&
                          ui_rect_has_area(rectangle_of(UiSemantic::composer));
    check(laid_out, "typing_strip_shell_layout");
    if (!laid_out) {
        message_scene_release(&f.scene);
        message_composer_scene_release(&f.composer);
        ui_scene_release(&f.shell);
        return;
    }
    const UiRect body = rectangle_of(UiSemantic::message_view);
    const UiRect composer_rectangle = rectangle_of(UiSemantic::composer);

    const Utf8View typers = utf8_literal("Zupko is typing...");
    const auto update_composer = [&](Utf8View label) noexcept {
        return message_composer_scene_update(&f.composer, &f.model, composer_rectangle, true, 0, {}, nullptr, label);
    };
    bool at_bottom = update_composer({}).ok() && message_scene_update(&f.scene, body).ok();
    bool moved = false;
    at_bottom = at_bottom && message_scene_scroll_to_bottom(&f.scene, &moved) &&
                message_scene_update(&f.scene, body).ok() && f.timeline.follow_bottom &&
                message_timeline_is_at_bottom(f.timeline, 0);
    check(at_bottom, "typing_strip_messages_start_scrolled_to_the_bottom");

    Geometry quiet{};
    const bool quiet_captured = capture(f.composer, &quiet);
    const Rows quiet_rows = rows_of(f.scene);
    UiScrollAnchor quiet_anchor{};
    const auto quiet_scroll = f.timeline.scroll_units;
    const bool anchored = ui_variable_list_capture_anchor(f.list, f.timeline.scroll_units, &quiet_anchor);
    check(quiet_captured && anchored && quiet_rows.resident != 0,
          "typing_strip_message_rectangles_captured_without_typers");

    // The strip is part of the composer even when nobody types: the indicator's row sits above its surface and
    // draws over the bottom of the message list, inside the list's own bottom space. The list keeps the gap it
    // had before the strip existed (the list's bottom margin plus the composer's top margin): the strip adds none.
    const float kListGap = kGuildChannelShellLayoutMetrics.message_bottom_margin +
                           kGuildChannelShellLayoutMetrics.composer_margin.top;
    const auto last_row_gap = [&](const Geometry& g) noexcept {
        const auto* row = message_scene_find_row(f.scene, kRows - 1);
        return row ? g.surface.top - message_scene_message_rectangle(f.scene, *row).bottom : -1.f;
    };
    const float quiet_row_gap = quiet_captured ? last_row_gap(quiet) : -1.f;
    const UiRect strip{composer_rectangle.left, composer_rectangle.top, composer_rectangle.right, quiet.surface.top};
    check(quiet_captured && ui_rect_height(strip) == 24.f &&
              ui_rect_equal(quiet.controls[0], {strip.left, strip.top, strip.right, strip.bottom - 4.f}) &&
              quiet.surface.top - body.bottom == kListGap && quiet.controls[0].top < body.bottom &&
              quiet.controls[0].top >= body.bottom - 24.f && quiet_row_gap >= kListGap,
          "typing_strip_draws_over_the_message_list_bottom_space_while_nobody_types");

    // Someone starts typing: only the strip repaints.
    const auto started = update_composer(typers);
    Geometry typing{};
    const bool typing_captured = capture(f.composer, &typing);
    check(started.ok() && started.changed && typing_captured && same(quiet, typing),
          "typing_strip_typing_starts_without_relayout_or_moving_any_control");
    check(typing_captured && utf8_equal(message_composer_scene_typing_label(f.composer), typers) &&
              ui_rect_equal(ui_rect_intersection(typing.controls[0], strip), typing.controls[0]) &&
              !ui_rect_has_area(ui_rect_intersection(typing.controls[0], typing.surface)),
          "typing_strip_indicator_draws_inside_the_reserved_strip");

    // The application derives the composer's height from the draft, never from the typers, so the shell and
    // the message list see the same rectangles with typers present.
    const bool relaid = layout_shell() && ui_rect_equal(rectangle_of(UiSemantic::message_view), body) &&
                        ui_rect_equal(rectangle_of(UiSemantic::composer), composer_rectangle);
    check(relaid && typing_captured && last_row_gap(typing) == quiet_row_gap &&
              typing.surface.top - rectangle_of(UiSemantic::message_view).bottom == kListGap,
          "typing_strip_last_message_to_composer_distance_is_the_same_with_typers");
    const auto message_update = message_scene_update(&f.scene, rectangle_of(UiSemantic::message_view));
    UiScrollAnchor typing_anchor{};
    const bool typing_anchored = ui_variable_list_capture_anchor(f.list, f.timeline.scroll_units, &typing_anchor);
    check(message_update.ok() && !message_update.changed && typing_anchored && same(quiet_rows, rows_of(f.scene)),
          "typing_strip_message_rectangles_identical_with_and_without_typers");
    check(typing_anchor.index == quiet_anchor.index &&
              typing_anchor.within_item_units == quiet_anchor.within_item_units &&
              f.timeline.scroll_units == quiet_scroll && f.timeline.follow_bottom &&
              message_timeline_is_at_bottom(f.timeline, 0),
          "typing_strip_scroll_anchor_unchanged_when_typing_starts_at_the_bottom");

    // The label changes to several typers, and then everyone stops: still no relayout, no movement.
    const auto several = update_composer(utf8_literal("Zupko and Mai are typing..."));
    Geometry crowd{};
    check(several.ok() && several.changed && capture(f.composer, &crowd) && same(quiet, crowd),
          "typing_strip_label_change_repaints_without_relayout");
    const auto stopped = update_composer({});
    Geometry after{};
    check(stopped.ok() && stopped.changed && capture(f.composer, &after) && same(quiet, after) &&
              message_composer_scene_typing_label(f.composer).bytes == 0,
          "typing_strip_typing_stops_without_relayout_or_moving_any_control");
    const bool stop_laid_out = layout_shell() && ui_rect_equal(rectangle_of(UiSemantic::message_view), body) &&
                               last_row_gap(after) == quiet_row_gap;
    const auto message_stop = message_scene_update(&f.scene, rectangle_of(UiSemantic::message_view));
    UiScrollAnchor stopped_anchor{};
    check(stop_laid_out && message_stop.ok() && !message_stop.changed &&
              ui_variable_list_capture_anchor(f.list, f.timeline.scroll_units, &stopped_anchor) &&
              stopped_anchor.index == quiet_anchor.index &&
              stopped_anchor.within_item_units == quiet_anchor.within_item_units &&
              f.timeline.scroll_units == quiet_scroll && same(quiet_rows, rows_of(f.scene)) &&
              f.timeline.follow_bottom,
          "typing_strip_scroll_anchor_unchanged_when_typing_stops_at_the_bottom");

    // The fade behind the strip, painted in the composer overlay while the list is scrolled away from the newest
    // message: transparent chat background to the chat background, then (while someone types) a solid band of it
    // down to the list's bottom; it keeps the scrollbar's 16 DIP clear, and never touches the label's line.
    PaintStream paint{};
    const Theme& theme = kDefaultTheme;
    Color clear = theme.base_low;
    clear.alpha = 0;
    const MessageComposerPresentationInput no_input{};
    const auto overlay = [&](bool away) noexcept {
        const ShellTypingFade fade{rectangle_of(UiSemantic::message_view), away};
        return paint_stream_reset(&paint, kWidth, kHeight) &&
               shell_presentation_build_composer_overlay(&f.composer, no_input, theme, &paint, 96, nullptr, &fade);
    };
    const auto count_of = [&](float border_width) noexcept {
        u32 found = 0;
        for (u32 i = 0; i < paint.count; ++i)
            found += paint.quads[i].border_width == border_width;
        return found;
    };
    const auto has = [&](const UiRect& rectangle, const Color& fill) noexcept {
        for (u32 i = 0; i < paint.count; ++i) {
            const PaintQuad& q = paint.quads[i];
            if (ui_rect_equal({q.left, q.top, q.right, q.bottom}, rectangle) && bytes_equal(&q.fill, &fill, sizeof(Color)))
                return true;
        }
        return false;
    };
    const UiRect list = rectangle_of(UiSemantic::message_view);
    const float scrollbar_edge = list.right - kUiTypingFadeRightInset;
    bool faded = paint_stream_initialize(&paint) && update_composer(typers).ok();
    f.composer.typing_dots = true;
    const float strip_top = message_composer_scene_layout_node(f.composer, MessageComposerControl::typing_indicator)
                                ->rectangle.top;
    // Someone types and the list is scrolled away: a 72 DIP gradient up to the strip's top, a solid band below it.
    faded = faded && overlay(true) && count_of(-2) == 1 && strip_top < list.bottom &&
            has({list.left, strip_top - kUiTypingFadeTyping, scrollbar_edge, strip_top}, clear) &&
            has({list.left, strip_top, scrollbar_edge, list.bottom}, theme.base_low);
    bool gradient_ends_opaque = false;
    for (u32 i = 0; i < paint.count; ++i)
        gradient_ends_opaque |= paint.quads[i].border_width == -2 &&
                                bytes_equal(&paint.quads[i].border, &theme.base_low, sizeof(Color));
    check(faded && gradient_ends_opaque,
          "typing_strip_fade_while_typing_is_a_72_dip_gradient_into_the_chat_background_then_a_solid_band");
    // At the bottom nothing is under the strip: no fade, only the three dots remain.
    check(faded && overlay(false) && count_of(-2) == 0 && paint.count == kUiTypingDotCount,
          "typing_strip_no_fade_at_the_bottom_of_the_list");
    // Nobody types and the list is scrolled away: the plain 16 DIP fade at the list's bottom, no band, no dots.
    f.composer.typing_dots = false;
    check(update_composer({}).ok() && overlay(true) && count_of(-2) == 1 && paint.count == 1 &&
              has({list.left, list.bottom - kUiTypingFadeIdle, scrollbar_edge, list.bottom}, clear),
          "typing_strip_idle_fade_is_16_dips_at_the_list_bottom");
    // The label's line (the row's middle plus the drop) is centred 12 DIP above the surface, like the official
    // client's 24 DIP wrapper.
    const UiLayoutNode* label = message_composer_scene_layout_node(f.composer, MessageComposerControl::typing_indicator);
    check(label &&
              (label->content_rectangle.top + label->content_rectangle.bottom) * .5f + kUiTypingLabelDrop ==
                  quiet.surface.top - 12.f,
          "typing_strip_label_line_is_centred_12_dips_above_the_composer_like_the_wrapper");
    paint_stream_release(&paint);

    message_scene_release(&f.scene);
    message_composer_scene_release(&f.composer);
    ui_scene_release(&f.shell);
}

} // namespace maicord
