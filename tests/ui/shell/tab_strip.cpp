// The title bar's channel tabs: widths and overflow across the strip's room, the active tab's minimum, hits on tabs,
// close buttons and the "+", drag slots, opening, closing, sliding and dragging motion, the stamp, and paint inside
// the strip. Synthetic views only.
#include "maicord/core/win32_kernel.hpp"
#include "maicord/ui/shell/caption_bar.hpp"
#include "maicord/ui/shell/tab_strip.hpp"
#include "../../support/test_output.hpp"

namespace maicord::tab_strip_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}

bool near(float a, float b) noexcept {
    return a - b < .01f && b - a < .01f;
}

constexpr float kLeft = 30.f, kTop = 4.f, kBottom = 32.f;

UiRect area(float width) noexcept {
    return {kLeft, kTop, kLeft + width, kBottom};
}

UiTabStripView view(u32 count, u32 active) noexcept {
    UiTabStripView v{};
    v.count = static_cast<u8>(count);
    v.plus_enabled = true;
    for (u32 i = 0; i < count; ++i) {
        v.items[i].label = utf8_literal("general");
        v.items[i].icon = UiIcon::channel;
        v.items[i].flags = i == active ? kUiTabActive : u8{0};
    }
    return v;
}

float centre(const UiRect& r) noexcept {
    return (r.left + r.right) * .5f;
}

void widths_and_overflow() noexcept {
    {
        const UiTabStripView v = view(1, 0);
        const UiTabStripLayout l = ui_tab_strip_layout(area(600), v);
        check(near(l.width[0], kUiTabWidth) && near(l.left[0], kLeft) && !l.overflowed && l.shown == 1,
              "one tab keeps its preferred width at the strip's start");
        check(near(l.plus.left, kLeft + kUiTabWidth) && near(l.plus.right - l.plus.left, kUiTabButton) &&
                  !ui_rect_has_area(l.overflow),
              "the plus follows the last tab, no overflow button");
    }
    {
        const UiTabStripView v = view(4, 0);
        const UiTabStripLayout l = ui_tab_strip_layout(area(600), v);
        check(near(l.width[0], 143.f) && near(l.width[3], 143.f) && near(l.slot_left[1], kLeft + 143.f),
              "tabs share the room evenly once they no longer fit at rest");
        check(near(l.plus.right, kLeft + 600.f), "the plus ends at the strip's edge when the tabs fill it");
    }
    {
        // Nine tabs at 41 DIP would put the active one below its minimum: it keeps 96, the rest shrink further.
        const UiTabStripView v = view(9, 3);
        const UiTabStripLayout l = ui_tab_strip_layout(area(400), v);
        check(!l.overflowed && l.shown == 9 && near(l.width[3], kUiTabActiveMinimum) &&
                  near(l.width[0], (400.f - kUiTabButton - kUiTabActiveMinimum) / 8.f) && l.width[0] >= kUiTabMinimum,
              "the active tab keeps its minimum while the others shrink");
        check(near(l.plus.right, kLeft + 400.f), "the strip is filled exactly");
    }
    {
        // 16 tabs in 300 DIP: the first four and the active one (item 12, taking the last slot), then "..." at the end.
        const UiTabStripView v = view(16, 12);
        const UiTabStripLayout l = ui_tab_strip_layout(area(300), v);
        check(l.overflowed && l.shown == 5, "sixteen tabs overflow in 300 DIP");
        check(near(l.slot_width[12], kUiTabActiveMinimum) && near(l.slot_left[12], l.slot_left[3] + l.slot_width[3]) &&
                  l.slot_width[4] == 0 && l.slot_width[13] == 0,
              "the active tab takes the last visible slot, the others beyond stay hidden");
        check(near(l.overflow.right, kLeft + 300.f) && near(l.plus.right, l.overflow.left) &&
                  near(l.plus.left, l.slot_left[12] + l.slot_width[12]),
              "the buttons close the strip");
        check(ui_tab_strip_hit(l, centre(ui_tab_strip_tab(l, 4)), 20).kind != UiTabHitKind::tab,
              "a hidden tab takes no input");
        const UiTabStripView first = view(16, 0);
        const UiTabStripLayout f = ui_tab_strip_layout(area(300), first);
        check(f.shown == 5 && f.slot_width[4] > 0 && f.slot_width[5] == 0 && near(f.slot_width[0], 96.f),
              "an active first tab is among the first ones");
    }
    {
        // The window's smallest strips (624 DIP wide).
        const UiTabStripView v = view(16, 15);
        const UiTabStripLayout narrow = ui_tab_strip_layout(area(257), v);
        check(narrow.overflowed && narrow.shown >= 4 && ui_rect_has_area(narrow.overflow) &&
                  near(narrow.overflow.right, kLeft + 257.f),
              "a 624 DIP window's strip keeps the active tab, the plus and the list button");
        const UiTabStripLayout wide = ui_tab_strip_layout(area(1538), v);
        check(!wide.overflowed && wide.shown == 16 && wide.width[0] >= kUiTabLabelMinimum,
              "all sixteen tabs fit a 1920 DIP window with labels");
    }
    {
        const UiTabStripView v = view(3, 0);
        const UiTabStripLayout l = ui_tab_strip_layout(area(50), v);
        check(l.shown == 0 && !ui_rect_has_area(l.plus) && ui_tab_strip_hit(l, kLeft + 10, 20).kind == UiTabHitKind::none,
              "a strip too narrow for the plus and one tab draws and takes nothing");
    }
}

void hits_and_labels() noexcept {
    UiTabStripView v = view(3, 1);
    UiTabStripLayout l = ui_tab_strip_layout(area(700), v);
    const UiRect second = ui_tab_strip_tab(l, 1), close = ui_tab_strip_close(l, 1);
    check(ui_rect_has_area(close) && close.right <= second.right && close.left > second.left,
          "wide tabs show their close button");
    UiTabHit hit = ui_tab_strip_hit(l, second.left + 40, 20);
    check(hit.kind == UiTabHitKind::tab && hit.index == 1, "a press inside a tab hits it");
    hit = ui_tab_strip_hit(l, centre(close), 20);
    check(hit.kind == UiTabHitKind::close && hit.index == 1, "the close button hits");
    check(ui_tab_strip_hit(l, second.left + 40, kTop - .5f).kind == UiTabHitKind::none &&
              ui_tab_strip_hit(l, second.left + 40, kBottom).kind == UiTabHitKind::none,
          "above and below the tab is the drag band");
    hit = ui_tab_strip_hit(l, centre(l.plus), 20);
    check(hit.kind == UiTabHitKind::plus, "the plus hits");
    check(ui_tab_strip_hit(l, l.plus.right + 1, 20).kind == UiTabHitKind::none &&
              ui_tab_strip_hit(l, kLeft - 1, 20).kind == UiTabHitKind::none,
          "the empty strip and what lies outside it are the drag band");
    check(ui_tab_strip_hit_rect(l, {UiTabHitKind::tab, 1}).left == second.left &&
              ui_tab_strip_hit_rect(l, {UiTabHitKind::plus, 0}).left == l.plus.left &&
              !ui_rect_has_area(ui_tab_strip_hit_rect(l, {})),
          "hit rectangles for tooltips");
    check(utf8_equal(ui_tab_strip_hit_name(v, {UiTabHitKind::tab, 2}), utf8_literal("general")) &&
              ui_tab_strip_hit_name(v, {UiTabHitKind::close, 2}).bytes && ui_tab_strip_hit_name(v, {UiTabHitKind::plus, 0}).bytes &&
              ui_tab_strip_hit_name(v, {UiTabHitKind::overflow, 0}).bytes && !ui_tab_strip_hit_name(v, {}).bytes,
          "tooltip texts");
    // A tab that says more in its tooltip (its server) shows that text; the others keep their label.
    UiTabStripView named = v;
    named.items[1].tooltip = utf8_literal("general \xC2\xB7 Server");
    check(utf8_equal(ui_tab_strip_hit_name(named, {UiTabHitKind::tab, 1}), named.items[1].tooltip) &&
              utf8_equal(ui_tab_strip_hit_name(named, {UiTabHitKind::tab, 2}), utf8_literal("general")) &&
              utf8_equal(ui_tab_strip_hit_name(named, {UiTabHitKind::close, 1}), utf8_literal("Close tab")),
          "a tooltip text replaces the tab's label in its tooltip only");
    const UiTabLabel label = ui_tab_strip_label(l, 0);
    check(label.limit > 0 && label.limit < l.slot_width[0] && label.left > l.left[0],
          "a wide tab's label sits between its icon and its close button");

    // Narrow tabs: icon only; the close button shows on the active and the hovered tab, in the icon's place.
    v = view(9, 3);
    l = ui_tab_strip_layout(area(400), v);
    check(ui_tab_strip_label(l, 0).limit == 0 && ui_rect_has_area(ui_tab_strip_icon(l, 0)) &&
              !ui_rect_has_area(ui_tab_strip_close(l, 0)),
          "a narrow tab is its icon alone");
    check(ui_rect_has_area(ui_tab_strip_close(l, 3)), "the active tab keeps its close button");
    v.hovered = {UiTabHitKind::tab, 0};
    l = ui_tab_strip_layout(area(400), v);
    check(ui_rect_has_area(ui_tab_strip_close(l, 0)) && !ui_rect_has_area(ui_tab_strip_icon(l, 0)) &&
              near(centre(ui_tab_strip_close(l, 0)), centre(ui_tab_strip_tab(l, 0))),
          "hovering a narrow tab swaps its icon for the close button");
    hit = ui_tab_strip_hit(l, centre(ui_tab_strip_close(l, 0)), 20);
    check(hit.kind == UiTabHitKind::close && hit.index == 0, "and the close button hits");

    // A closing tab takes no input.
    v = view(3, 0);
    v.items[1].flags = kUiTabClosing;
    v.items[1].from_left = kLeft + 168.f;
    v.items[1].from_width = 168.f;
    v.remaining = .5f;
    l = ui_tab_strip_layout(area(700), v);
    check(l.slot_width[1] == 0 && ui_tab_strip_hit(l, l.left[1] + 10, 20).kind != UiTabHitKind::close &&
              !ui_rect_has_area(ui_tab_strip_close(l, 1)),
          "a closing tab keeps no close button");
}

// Inactive tabs narrower than kUiTabCloseAlways show no close button until the pointer is on them, and their label
// takes the button's room (24 DIP) back meanwhile.
void close_on_hover() noexcept {
    // Sixteen tabs in the 1920 DIP strip: 94 DIP each, the active one (8) 96.
    UiTabStripView v = view(16, 8);
    UiTabStripLayout l = ui_tab_strip_layout(area(1538), v);
    check(l.slot_width[3] < kUiTabCloseAlways && l.slot_width[3] >= kUiTabLabelMinimum,
          "the inactive tabs are narrow but still show labels");
    check(!ui_rect_has_area(ui_tab_strip_close(l, 3)) && ui_rect_has_area(ui_tab_strip_close(l, 8)),
          "only the active tab shows its close button");
    const UiTabLabel idle = ui_tab_strip_label(l, 3);
    const UiRect body = ui_tab_strip_tab(l, 3);
    UiTabHit hit = ui_tab_strip_hit(l, body.right - 16, 20);
    check(hit.kind == UiTabHitKind::tab && hit.index == 3, "where no button shows, the press is on the tab");
    v.hovered = {UiTabHitKind::tab, 3};
    l = ui_tab_strip_layout(area(1538), v);
    const UiRect close = ui_tab_strip_close(l, 3);
    check(ui_rect_has_area(close), "hovering a narrow tab reveals its close button");
    hit = ui_tab_strip_hit(l, centre(close), 20);
    check(hit.kind == UiTabHitKind::close && hit.index == 3, "and the button hits");
    check(near(idle.limit - ui_tab_strip_label(l, 3).limit, 24.f), "the label gives that room up only while it shows");
    // The hover fill fading out keeps the button until it is gone.
    v.hovered = {};
    v.items[3].hover = .4f;
    l = ui_tab_strip_layout(area(1538), v);
    check(ui_rect_has_area(ui_tab_strip_close(l, 3)), "a fading hover keeps the close button");
    v.items[3].hover = 0;
    l = ui_tab_strip_layout(area(1538), v);
    check(!ui_rect_has_area(ui_tab_strip_close(l, 3)), "and it goes with the hover");
    // Wide tabs always show it.
    v = view(4, 0);
    l = ui_tab_strip_layout(area(720), v);
    hit = ui_tab_strip_hit(l, centre(ui_tab_strip_close(l, 2)), 20);
    check(hit.kind == UiTabHitKind::close && hit.index == 2, "a wide inactive tab keeps its close button");
}

void drag_slots() noexcept {
    const UiTabStripView v = view(4, 0);
    const UiTabStripLayout l = ui_tab_strip_layout(area(720), v);
    const auto mid = [&](u32 i) noexcept { return l.slot_left[i] + l.slot_width[i] * .5f; };
    check(ui_tab_strip_drag_slot(l, 1, mid(1)) == 1, "a dragged tab over its own slot stays");
    check(ui_tab_strip_drag_slot(l, 1, mid(2) - 1) == 1 && ui_tab_strip_drag_slot(l, 1, mid(2) + 1) == 2,
          "it passes a neighbour at the neighbour's midpoint");
    check(ui_tab_strip_drag_slot(l, 1, mid(0) + 1) == 1 && ui_tab_strip_drag_slot(l, 1, mid(0) - 1) == 0,
          "in both directions");
    check(ui_tab_strip_drag_slot(l, 0, 5000) == 3 && ui_tab_strip_drag_slot(l, 3, -5000) == 0 &&
              ui_tab_strip_drag_slot(l, 3, 5000) == 3,
          "the ends hold the tabs in range");
}

void motion() noexcept {
    UiTabStripView v = view(3, 0);
    const UiTabStripLayout rest = ui_tab_strip_layout(area(700), v);
    // An opening tab grows from nothing in its slot while the label fades in.
    v.items[2].flags = kUiTabOpening;
    v.remaining = 1;
    UiTabStripLayout l = ui_tab_strip_layout(area(700), v);
    check(l.width[2] == 0 && l.fade[2] == 0 && near(l.left[2], rest.slot_left[2]) && l.fade[0] == 1,
          "an opening tab starts as nothing");
    v.remaining = .5f;
    l = ui_tab_strip_layout(area(700), v);
    check(near(l.width[2], rest.slot_width[2] * .5f) && near(l.fade[2], .5f), "halfway it is half grown");
    v.remaining = 0;
    l = ui_tab_strip_layout(area(700), v);
    check(near(l.width[2], rest.slot_width[2]) && l.fade[2] == 1, "and settles on its slot");

    // Neighbours slide from where they were.
    v = view(3, 0);
    v.items[1].from_left = rest.slot_left[1] + 60;
    v.items[1].from_width = rest.slot_width[1];
    v.remaining = 1;
    l = ui_tab_strip_layout(area(700), v);
    check(near(l.left[1], rest.slot_left[1] + 60) && near(l.slot_left[1], rest.slot_left[1]), "a sliding tab starts where it was");
    v.remaining = .25f;
    l = ui_tab_strip_layout(area(700), v);
    check(near(l.left[1], rest.slot_left[1] + 15), "and eases toward its slot");

    // A closing tab shrinks to nothing and its neighbours' slots close over it.
    v = view(3, 0);
    v.items[1].flags = kUiTabClosing;
    v.items[1].from_left = rest.slot_left[1];
    v.items[1].from_width = rest.slot_width[1];
    v.remaining = 1;
    l = ui_tab_strip_layout(area(700), v);
    check(near(l.width[1], rest.slot_width[1]) && l.fade[1] == 1 && near(l.slot_left[2], rest.slot_left[1]),
          "a closing tab starts whole while the next one's slot is already beside the first");
    v.remaining = 0;
    l = ui_tab_strip_layout(area(700), v);
    check(l.width[1] == 0 && l.fade[1] == 0, "and ends as nothing");

    // A dragged tab follows the pointer inside the tabs.
    v = view(3, 0);
    v.items[1].flags = kUiTabDragging;
    v.drag_left = -500;
    l = ui_tab_strip_layout(area(700), v);
    check(near(l.left[1], kLeft) && near(l.width[1], rest.slot_width[1]), "a dragged tab stops at the strip's start");
    v.drag_left = 5000;
    l = ui_tab_strip_layout(area(700), v);
    check(near(l.left[1] + l.width[1], rest.slot_left[2] + rest.slot_width[2]), "and at the last tab's end");
    v.drag_left = 100;
    l = ui_tab_strip_layout(area(700), v);
    check(near(l.left[1], 100) && near(l.slot_left[1], rest.slot_left[1]), "in between it is where the pointer holds it");
}

void stamp() noexcept {
    UiTabStripView v = view(3, 1);
    const u64 base = ui_tab_strip_stamp(v);
    check(base == ui_tab_strip_stamp(view(3, 1)), "equal views share a stamp");
    UiTabStripView w = v;
    w.items[0].label = utf8_literal("random");
    check(ui_tab_strip_stamp(w) != base, "a label changes it");
    w = v;
    w.items[2].hover = .5f;
    check(ui_tab_strip_stamp(w) != base, "a hover amount changes it");
    w = v;
    w.hovered = {UiTabHitKind::plus, 0};
    check(ui_tab_strip_stamp(w) != base, "the hovered button changes it");
    w = v;
    w.remaining = .3f;
    check(ui_tab_strip_stamp(w) != base, "the transition changes it");
    w = v;
    w.count = 2;
    check(ui_tab_strip_stamp(w) != base, "the count changes it");
    w = v;
    w.items[1].flags |= kUiTabUnread;
    check(ui_tab_strip_stamp(w) != base, "the unread state changes it");
    w = v;
    w.items[1].tooltip = utf8_literal("general \xC2\xB7 Server");
    check(ui_tab_strip_stamp(w) == base, "a tooltip text draws nothing, so it leaves it");
}

void painting() noexcept {
    const Theme& theme = kDefaultTheme;
    PaintStream paint{};
    check(paint_stream_initialize(&paint, 128), "paint stream");
    UiTabStripView v = view(5, 2);
    v.items[2].select = 1;
    v.items[0].hover = .5f;
    v.hovered = {UiTabHitKind::close, 2};
    const UiRect strip = area(600);
    const UiTabStripLayout l = ui_tab_strip_layout(strip, v);
    check(paint_stream_reset(&paint, 1000, kBottom) && ui_tab_strip_paint(&paint, theme, l, v, true, 1) && !paint.rejected,
          "the strip paints");
    // Per tab: its icon; the close button of the active tab and of a hovered one; fills for the active, the hovered
    // and the close hover; the plus and the list button add their icons.
    check(paint.count >= 5 + 2 && paint.count <= 5 * 2 + 5 + 4, "the strip's quad count stays small");
    bool inside = true;
    for (u32 i = 0; i < paint.count; ++i)
        inside = inside && paint.quads[i].left >= strip.left - .01f && paint.quads[i].right <= strip.right + .01f &&
                 paint.quads[i].top >= kTop - .01f && paint.quads[i].bottom <= kBottom + theme.scale.radius_md + .01f;
    check(inside, "every quad stays in the strip, reaching below the bar only by a corner radius");
    // The active fill's corners are rounded on top and reach below the bar to square the bottom.
    const UiRect body = ui_tab_strip_tab(l, 2);
    bool shape = false;
    for (u32 i = 0; i < paint.count; ++i)
        shape = shape || (near(paint.quads[i].left, body.left) && near(paint.quads[i].right, body.right) &&
                          near(paint.quads[i].top, kTop) && near(paint.quads[i].bottom, kBottom + theme.scale.radius_md) &&
                          near(paint.quads[i].radius, theme.scale.radius_md));
    check(shape, "the active tab is a top-rounded shape whose bottom lies on the bar's edge");
    const Color idle = ui_tab_strip_ink(theme, v, l, 0, true), active = ui_tab_strip_ink(theme, v, l, 2, true);
    check(active.alpha >= idle.alpha && near(active.red, theme.tokens.caption.text.red),
          "the active tab's ink is the caption text");
    UiTabStripView faded = v;
    faded.items[3].flags = kUiTabMissing;
    check(ui_tab_strip_ink(theme, faded, l, 3, true).alpha < ui_tab_strip_ink(theme, v, l, 3, true).alpha,
          "an unreachable channel is dimmed");
    // Unread: one 6 DIP dot on the icon's upper right.
    const u32 plain = paint.count;
    UiTabStripView unread = v;
    unread.items[1].flags |= kUiTabUnread;
    const UiRect glyph = ui_tab_strip_icon(l, 1);
    check(paint_stream_reset(&paint, 1000, kBottom) &&
              ui_tab_strip_paint(&paint, theme, ui_tab_strip_layout(strip, unread), unread, true, 1) &&
              paint.count == plain + 1,
          "an unread tab adds one quad");
    // The dot is the only 6 x 6 quad: the icons are 16 and the buttons 24 or more.
    bool dotted = false;
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& dot = paint.quads[i];
        dotted = dotted || (near(dot.right - dot.left, 6) && near(dot.bottom - dot.top, 6) &&
                            near((dot.left + dot.right) * .5f, glyph.right) && near((dot.top + dot.bottom) * .5f, glyph.top));
    }
    check(dotted, "the dot sits on the icon's upper right corner");
    paint_stream_release(&paint);
}

void caption_layout() noexcept {
    const Theme& theme = kDefaultTheme;
    const UiTabStripView v = view(3, 0);
    UiCaptionBarState state{};
    const UiCaptionBarLayout off = ui_caption_bar_layout(theme, 1000, state);
    check(!ui_rect_has_area(off.tabs.area) && !off.title_leading &&
              near(off.title.left, off.icon.right + theme.scale.caption_padding) &&
              near(off.title.right, off.buttons[0].left - theme.scale.caption_padding),
          "without tabs the title spans from the icon to the buttons, centred");
    state.tabs = &v;
    const UiCaptionBarLayout on = ui_caption_bar_layout(theme, 1000, state);
    const float region = minimum(maximum(1000 * .2f, kUiCaptionServerMinimum), kUiCaptionServerMaximum);
    check(on.title_leading && near(on.title.left, on.buttons[0].left - region) &&
              near(on.title.right, on.buttons[0].left - theme.scale.caption_padding),
          "with tabs the title is a fixed region beside the window buttons");
    check(near(on.tabs.area.left, on.icon_target.right) &&
              near(on.tabs.area.right, on.title.left - kUiCaptionDragGap) && near(on.tabs.area.top, theme.scale.space_2) &&
              near(on.tabs.area.bottom, on.height),
          "the strip fills the room between the icon and the server region less the drag gap");
    const float x = centre(ui_tab_strip_tab(on.tabs, 1));
    check(ui_caption_bar_part(on, x, 20) == UiCaptionPart::tab && ui_caption_bar_part(on, x, 1) == UiCaptionPart::caption &&
              ui_caption_bar_part(on, on.tabs.area.right, 20) == UiCaptionPart::caption &&
              ui_caption_bar_part(on, on.title.left + 4, 20) == UiCaptionPart::caption &&
              ui_caption_bar_part(on, on.icon.left + 2, 20) == UiCaptionPart::icon,
          "tabs are client parts; the empty strip, the gap and the server region still drag");
    check(ui_caption_bar_native(UiCaptionPart::tab) == UiCaptionNative::client &&
              ui_caption_bar_part_tooltip(UiCaptionPart::tab),
          "the tab part is client and shows tooltips");

    // The region depends on the bar's width alone: another title moves nothing.
    check(ui_caption_title_place(on, 50, false, 0).text_left == on.title.left &&
              ui_caption_title_place(on, 90, true, kUiCaptionTitleImage).icon.left == on.title.left,
          "the server name starts at the region's left, whatever its length");
    // The update badge sits left of the server region and narrows the strip by its own width.
    state.update = UiCaptionUpdate::ready;
    const UiCaptionBarLayout badge = ui_caption_bar_layout(theme, 1000, state);
    check(near(badge.title.left, on.title.left) && near(badge.update.right, badge.title.left - theme.scale.space_2) &&
              near(badge.tabs.area.right, badge.update.left - kUiCaptionDragGap),
          "the update badge sits before the server region");
    // The region and the strip at the window's smallest and a wide window.
    const float widths[]{624, 864, 1000, 1920};
    for (const float width : widths) {
        state.update = UiCaptionUpdate::hidden;
        const UiCaptionBarLayout bar = ui_caption_bar_layout(theme, width, state);
        const float expected = minimum(maximum(width * .2f, kUiCaptionServerMinimum), kUiCaptionServerMaximum);
        check(near(bar.buttons[0].left - bar.title.left, expected) && bar.tabs.area.right - bar.tabs.area.left > 250.f,
              "the server region and the strip at each width");
    }
    // The strip's tooltip anchors to the hovered tab.
    state.update = UiCaptionUpdate::hidden;
    UiTabStripView hovered = v;
    hovered.hovered = {UiTabHitKind::tab, 2};
    state.tabs = &hovered;
    const UiCaptionBarLayout tip = ui_caption_bar_layout(theme, 1000, state);
    check(ui_caption_bar_part_rect(tip, UiCaptionPart::tab).left == ui_tab_strip_tab(tip.tabs, 2).left &&
              utf8_equal(ui_caption_bar_part_name(UiCaptionPart::tab, state), utf8_literal("general")),
          "the tab part's tooltip is the hovered tab's label and rectangle");
}

void run() noexcept {
    widths_and_overflow();
    hits_and_labels();
    close_on_hover();
    drag_slots();
    motion();
    stamp();
    painting();
    caption_layout();
}
} // namespace maicord::tab_strip_test

extern "C" __declspec(noreturn) void WINAPI maicord_tab_strip_tests_entry() noexcept {
    maicord::tab_strip_test::run();
    ExitProcess(maicord::tab_strip_test::failures ? 1 : 0);
}
