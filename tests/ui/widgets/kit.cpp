#include "maicord/ui/widgets/kit.hpp"
#include "maicord/ui/widgets/kit_markdown.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool near(float a, float b) noexcept {
    return a - b < .01f && b - a < .01f;
}
bool inside(const UiRect& inner, const UiRect& outer) noexcept {
    return inner.left >= outer.left - .01f && inner.top >= outer.top - .01f && inner.right <= outer.right + .01f &&
           inner.bottom <= outer.bottom + .01f;
}
bool disjoint(const UiRect& a, const UiRect& b) noexcept {
    return a.right <= b.left + .01f || b.right <= a.left + .01f || a.bottom <= b.top + .01f || b.bottom <= a.top + .01f;
}
bool contains_bytes(const char* text, u32 bytes, const char* needle) noexcept {
    const u32 length = c_string_length(needle);
    for (u32 i = 0; i + length <= bytes; ++i)
        if (bytes_equal(text + i, needle, length))
            return true;
    return false;
}

void test_search_and_segmented(Check check, const UiSettingsThemeTokens& t) noexcept {
    const UiRect field{0, 0, 400, 40};
    const auto empty = ui_search_field_geometry(t, field, false, 44);
    const auto typed = ui_search_field_geometry(t, field, true, 44);
    check(inside(empty.icon, field) && inside(empty.hint, field) && !ui_rect_has_area(empty.clear) &&
              empty.text.left >= empty.icon.right && empty.text.right <= empty.hint.left && empty.hint.right <= field.right,
          "search field: an empty field shows the glyph and the key hint, text between them");
    check(inside(typed.clear, field) && !ui_rect_has_area(typed.hint) && typed.text.right <= typed.clear.left &&
              ui_search_field_hit_clear(typed, (typed.clear.left + typed.clear.right) * .5f, (typed.clear.top + typed.clear.bottom) * .5f) &&
              !ui_search_field_hit_clear(typed, 5, 5) && !ui_search_field_hit_clear(empty, 390, 20),
          "search field: typed text shows a clear button that is the only clear hit target");
    check(ui_search_field_key('F', true, false, false, false) == UiSearchKey::focus &&
              ui_search_field_key('F', true, false, true, true) == UiSearchKey::focus &&
              ui_search_field_key(0xBF, false, false, false, false) == UiSearchKey::focus &&
              ui_search_field_key(0xBF, false, false, true, true) == UiSearchKey::none &&
              ui_search_field_key(0x1B, false, false, true, true) == UiSearchKey::clear &&
              ui_search_field_key(0x1B, false, false, true, false) == UiSearchKey::leave &&
              ui_search_field_key(0x1B, false, false, false, false) == UiSearchKey::none &&
              ui_search_field_key('F', false, false, false, false) == UiSearchKey::none,
          "search field: Ctrl+F and / focus, Esc clears then leaves, other keys pass");
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_search_field_paint(&paint, t, empty, false, {true, false, false, false}, {}) && paint.count >= 3 &&
              paint_stream_reset(&paint, 1000, 1000) &&
              ui_search_field_paint(&paint, t, typed, true, {true, false, false, true}, {true, true, false, false}) &&
              paint.count >= 5,
          "search field: paints the shell, glyph, hint or clear button, and the focus ring");

    const float labels[]{40, 60};
    const float item = ui_segmented_item_width(t, labels, 2);
    const float width = ui_segmented_width(t, item, 2);
    check(near(item, 88) && near(width, 184) && near(ui_segmented_item_width(t, labels, 0), 72) &&
              near(ui_segmented_height(t, false), 40) && near(ui_segmented_height(t, true), 32),
          "segmented: items are as wide as the widest label plus padding, at least 72; the track adds its padding");
    const UiRect track{100, 50, 100 + width, 90};
    const auto first = ui_segmented_option(t, track, 2, 0), second = ui_segmented_option(t, track, 2, 1);
    check(inside(first, track) && inside(second, track) && disjoint(first, second) && near(ui_rect_width(first), item) &&
              !ui_rect_has_area(ui_segmented_option(t, track, 2, 2)),
          "segmented: options partition the track inside its padding");
    check(ui_segmented_hit(t, track, 2, 110, 70) == 0 && ui_segmented_hit(t, track, 2, 250, 70) == 1 &&
              ui_segmented_hit(t, track, 2, 101, 52) == 0 && ui_segmented_hit(t, track, 2, 99, 70) == ~0u &&
              ui_segmented_hit(t, track, 2, 250, 95) == ~0u && ui_segmented_hit(t, track, 0, 110, 70) == ~0u,
          "segmented: every point of the track resolves to an option, none outside");
    check(ui_segmented_key(3, 1, UiKitKey::right) == 2 && ui_segmented_key(3, 2, UiKitKey::right) == 2 &&
              ui_segmented_key(3, 1, UiKitKey::left) == 0 && ui_segmented_key(3, 0, UiKitKey::up) == 0 &&
              ui_segmented_key(3, 1, UiKitKey::home) == 0 && ui_segmented_key(3, 1, UiKitKey::end) == 2 &&
              ui_segmented_key(3, ~0u, UiKitKey::left) == 2 &&
              ui_segmented_key(3, ~0u, UiKitKey::right) == 0 && ui_segmented_key(3, 1, UiKitKey::enter) == 1 &&
              ui_segmented_key(0, 0, UiKitKey::right) == 0,
          "segmented: arrows step with radio semantics, Home and End jump, other keys keep the selection");
    check(ui_segmented_fits(180, 400) && !ui_segmented_fits(300, 400),
          "segmented: a track over 60% of its row falls back to sitting under the text");
    check(paint_stream_reset(&paint, 1000, 1000) && ui_segmented_paint(&paint, t, track, 2, 0, 1, {true, false, false, true}) &&
              paint.count == 4 && paint_stream_reset(&paint, 1000, 1000) &&
              ui_segmented_paint(&paint, t, track, 2, ~0u, ~0u, {false, false, false, false}) && paint.count == 1,
          "segmented: the track, the selected plate, a hovered plate and the focus ring; no selection paints the track only");
    paint_stream_release(&paint);
}

void test_dropdown(Check check, const UiSettingsThemeTokens& t) noexcept {
    const UiRect field{0, 0, 200, 40};
    const auto plain = ui_dropdown_geometry(t, field, 0), sort = ui_dropdown_geometry(t, field, 40);
    check(near(plain.value.left, t.metric.field_padding) && near(plain.chevron.right, field.right) &&
              plain.value.right <= plain.chevron.left && !ui_rect_has_area(plain.label) &&
              inside(sort.label, field) && sort.value.left >= sort.label.right && sort.value.right <= sort.chevron.left,
          "dropdown: the value sits between the padding and the chevron, a label leads it");
    check(near(ui_dropdown_width(t, 0, 100, 320), t.metric.select_width) &&
              near(ui_dropdown_width(t, 0, 300, 320), 320) && near(ui_dropdown_width(t, 0, 300, 100), t.metric.select_width) &&
              ui_dropdown_width(t, 40, 200, 500) > ui_dropdown_width(t, 0, 200, 500),
          "dropdown: width follows the value within the select width and the row's share");
    const Utf8View labels[]{utf8_literal("Name"), utf8_literal("Newest"), utf8_literal("Least access")};
    PopupItem items[3]{};
    check(ui_dropdown_menu_items(labels, 3, 1, 700, {items, 3}) == 3 && items[1].action == 701 &&
              (items[1].flags & popup_item_checked) && !(items[0].flags & popup_item_checked) &&
              (items[2].flags & popup_item_enabled) && ui_dropdown_menu_items(labels, 3, 1, 700, {items, 2}) == 0 &&
              ui_dropdown_menu_items(nullptr, 3, 1, 700, {items, 3}) == 0,
          "dropdown: the menu is one themed row per option with the current one checked");
    u32 selected = 0;
    check(ui_dropdown_menu_choose(3, 700, 702, &selected) && selected == 2 && !ui_dropdown_menu_choose(3, 700, 703, &selected) &&
              !ui_dropdown_menu_choose(3, 700, 699, &selected) && selected == 2,
          "dropdown: a menu action maps back to its option, others are rejected");
    bool open = false;
    selected = 1;
    check(ui_dropdown_key(3, &selected, UiKitKey::down, &open) && selected == 2 && !open &&
              !ui_dropdown_key(3, &selected, UiKitKey::down, &open) && ui_dropdown_key(3, &selected, UiKitKey::up, &open) &&
              selected == 1 && ui_dropdown_key(3, &selected, UiKitKey::alt_down, &open) && open && selected == 1,
          "dropdown: arrows change the value, Alt+Down, Enter and Space ask for the menu");
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_dropdown_paint(&paint, t, plain, true, {true, true, false, false}) && paint.count >= 2,
          "dropdown: paints the select shell and chevron");
    paint_stream_release(&paint);
}

void test_chips(Check check, const UiSettingsThemeTokens& t) noexcept {
    const float widths[]{60, 60, 60, 60};
    const u8 groups[]{0, 0, 1, 1};
    UiRect rects[4]{}, dividers[4]{};
    const float wrapped = ui_chip_strip_place(t, widths, groups, 4, 200, true, 0, rects, dividers);
    bool whole = true, inside_width = true;
    for (u32 i = 0; i < 4; ++i) {
        whole = whole && near(ui_rect_width(rects[i]), 60) && near(ui_rect_height(rects[i]), kUiKitFilterChipHeight);
        inside_width = inside_width && rects[i].left >= 0 && rects[i].right <= 200;
    }
    check(near(wrapped, 72) && whole && inside_width && near(rects[0].top, 0) && near(rects[2].top, 40) &&
              near(rects[2].left, 0) && near(rects[3].left, 68) && !ui_rect_has_area(dividers[2]),
          "chip strip: chips wrap onto new rows, keep their measured width and never overflow");
    const float flat = ui_chip_strip_place(t, widths, groups, 4, 400, true, 0, rects, dividers);
    check(near(flat, 32) && near(rects[1].left, 68) && near(rects[2].left, 145) && ui_rect_has_area(dividers[2]) &&
              dividers[2].left > rects[1].right && dividers[2].right < rects[2].left && !ui_rect_has_area(dividers[1]),
          "chip strip: a group boundary on one row gets a divider between the chips");
    const float scrolled = ui_chip_strip_place(t, widths, groups, 4, 100, false, 10, rects, dividers);
    check(near(scrolled, 32) && near(rects[0].left, -10) && rects[3].right > 100 && near(rects[0].top, rects[3].top) &&
              near(ui_chip_strip_reveal(rects, 3, 100, 10), 10 + rects[3].right - 100) &&
              near(ui_chip_strip_reveal(rects, 0, 100, 10), 0) && near(ui_chip_strip_reveal(rects, 1, 100, 10), 28),
          "chip strip: one-row mode scrolls and can reveal a chip without squeezing any");
    ui_chip_strip_place(t, widths, groups, 4, 200, true, 0, rects, nullptr);
    check(ui_chip_strip_hit(rects, 4, 30, 10) == 0 && ui_chip_strip_hit(rects, 4, 30, 50) == 2 &&
              ui_chip_strip_hit(rects, 4, 64, 10) == ~0u && ui_chip_strip_hit(rects, 4, 190, 10) == ~0u,
          "chip strip: hit testing resolves a chip or nothing");
    check(ui_chip_strip_move(rects, 4, 1, UiKitKey::right) == 2 && ui_chip_strip_move(rects, 4, 2, UiKitKey::left) == 1 &&
              ui_chip_strip_move(rects, 4, 3, UiKitKey::right) == 3 && ui_chip_strip_move(rects, 4, 0, UiKitKey::down) == 2 &&
              ui_chip_strip_move(rects, 4, 3, UiKitKey::up) == 1 && ui_chip_strip_move(rects, 4, 3, UiKitKey::down) == 3 &&
              ui_chip_strip_move(rects, 4, 2, UiKitKey::home) == 0 && ui_chip_strip_move(rects, 4, 0, UiKitKey::end) == 3,
          "chip strip: arrows walk wrapped rows in reading order, up and down find the nearest chip");
    u64 mask = ui_chip_toggle(0, groups, 4, 1, false);
    check(mask == 2 && ui_chip_toggle(mask, groups, 4, 1, false) == 0 &&
              ui_chip_toggle(7, groups, 4, 0, true) == 4 && ui_chip_toggle(7, groups, 4, 9, false) == 7,
          "chip filters: toggling flips one bit, choosing All clears only its group");
    check(ui_chip_filter_matches(0, 0, groups, 4) && ui_chip_filter_matches(3, 2, groups, 4) &&
              !ui_chip_filter_matches(3, 4, groups, 4) && ui_chip_filter_matches(7, 6, groups, 4) &&
              !ui_chip_filter_matches(7, 2, groups, 4) && ui_chip_filter_matches(4, 4, groups, 4),
          "chip filters: chips of a group are alternatives, groups all apply");
    check(near(ui_chip_width(t, 30, UiChipTone::neutral, false), 46) && near(ui_chip_width(t, 30, UiChipTone::risk_high, false), 60) &&
              near(ui_chip_width(t, 30, UiChipTone::brand, true), 66) &&
              near(ui_filter_chip_width(t, 30, 10, false, false), 68) && near(ui_filter_chip_width(t, 30, 0, false, true), 74) &&
              near(ui_count_badge_width(t, 4), 18) && near(ui_count_badge_width(t, 20), 30),
          "chips: widths add the padding, the risk dot, icons, counts and the check");
    const UiRect chip{0, 0, 100, 24};
    const auto label = ui_chip_label_rect(t, chip, UiChipTone::risk_low, false);
    PaintStream paint{};
    check(near(label.left, 8 + 8 + 6) && near(label.right, 92) &&
              near(ui_filter_chip_label_rect(t, {0, 0, 100, 32}, false, true).left, 12 + 14 + 6) &&
              paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_chip_paint(&paint, t, chip, UiChipTone::risk_medium, UiIcon::count) && paint.count == 2 &&
              paint_stream_reset(&paint, 1000, 1000) && ui_chip_paint(&paint, t, chip, UiChipTone::positive, UiIcon::check) &&
              paint.count == 2 && paint_stream_reset(&paint, 1000, 1000) &&
              ui_filter_chip_paint(&paint, t, {0, 0, 100, 32}, true, {true, false, false, true}) && paint.count == 3 &&
              paint_stream_reset(&paint, 1000, 1000) && ui_filter_chip_paint(&paint, t, {0, 0, 100, 32}, false, {}) &&
              paint.count == 1,
          "chips: labels start after the dot, icon and check; selected filter chips draw the check and focus ring");
    check(ui_chip_ink(t, UiChipTone::positive).green == t.color.positive.green &&
              ui_chip_ink(t, UiChipTone::brand).red == t.color.on_brand.red && ui_chip_ink(t, UiChipTone::dev).red == t.color.critical_text.red,
          "chips: tones take their ink from the settings tokens");
    paint_stream_release(&paint);
}

void test_grid(Check check, const UiSettingsThemeTokens& t) noexcept {
    const auto wide = ui_card_grid(t, 792, 5, 168), middle = ui_card_grid(t, 700, 5, 168), narrow = ui_card_grid(t, 200, 5, 168);
    check(wide.columns == 3 && middle.columns == 2 && narrow.columns == 1 && near(wide.card_width, (792 - 32) / 3.f) &&
              wide.rows == 2 && near(wide.height, 2 * (168 + 16) - 16) && near(narrow.card_width, 200) && narrow.rows == 5,
          "card grid: one to three columns by width, gap 16, cards share the width");
    const auto empty = ui_card_grid(t, 792, 0, 168);
    check(empty.rows == 0 && near(empty.height, 0) && !ui_rect_has_area(ui_card_grid_rect(empty, 0, 0, 0)) &&
              ui_card_grid_hit(empty, 10, 10) == ~0u,
          "card grid: an empty grid has no rows, rectangles or hits");
    const auto fifth = ui_card_grid_rect(wide, 10, 20, 4);
    check(near(fifth.left, 10 + wide.card_width + 16) && near(fifth.top, 20 + 168 + 16) && near(ui_rect_height(fifth), 168) &&
              !ui_rect_has_area(ui_card_grid_rect(wide, 0, 0, 5)),
          "card grid: cards fill rows left to right");
    check(ui_card_grid_hit(wide, wide.card_width + 16 + 30, 168 + 16 + 20) == 4 && ui_card_grid_hit(wide, wide.card_width + 8, 20) == ~0u &&
              ui_card_grid_hit(wide, 30, 168 + 8) == ~0u && ui_card_grid_hit(wide, 2 * (wide.card_width + 16) + 10, 168 + 16 + 20) == ~0u &&
              ui_card_grid_hit(wide, -1, 5) == ~0u,
          "card grid: gaps and the empty slots after the last card hit nothing");
    check(ui_card_grid_move(wide, 1, UiKitKey::down, 2) == 4 && ui_card_grid_move(wide, 2, UiKitKey::down, 2) == 4 &&
              ui_card_grid_move(wide, 4, UiKitKey::up, 2) == 1 && ui_card_grid_move(wide, 0, UiKitKey::up, 2) == 0 &&
              ui_card_grid_move(wide, 4, UiKitKey::home, 2) == 3 && ui_card_grid_move(wide, 3, UiKitKey::end, 2) == 4 &&
              ui_card_grid_move(wide, 1, UiKitKey::ctrl_end, 2) == 4 && ui_card_grid_move(wide, 3, UiKitKey::ctrl_home, 2) == 0 &&
              ui_card_grid_move(wide, 4, UiKitKey::right, 2) == 4 && ui_card_grid_move(wide, 0, UiKitKey::left, 2) == 0 &&
              ui_card_grid_move(wide, 0, UiKitKey::page_down, 1) == 3 && ui_card_grid_move(wide, 4, UiKitKey::page_up, 1) == 1 &&
              ui_card_grid_move(wide, 99, UiKitKey::left, 1) == 0,
          "card grid: arrows move by one and by the column count, Home and End within the row, Ctrl and Page across rows");
    const auto tall = ui_card_grid(t, 792, 10, 168);
    u32 first = 0, end = 0;
    ui_card_grid_visible(tall, 0, 100, &first, &end);
    const u32 top_end = end;
    ui_card_grid_visible(tall, 400, 100, &first, &end);
    check(top_end == 6 && first == 3 && end == 10, "card grid: only the rows in view and one either side are materialized");
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) && ui_card_paint(&paint, t, fifth, {}) &&
              paint.count == 1 && paint_stream_reset(&paint, 1000, 1000) &&
              ui_card_paint(&paint, t, fifth, {true, true, false, true}) && paint.count == 2 &&
              paint_stream_reset(&paint, 1000, 1000) && ui_card_hairline_paint(&paint, t, {0, 0, 300, 56}) && paint.count == 1 &&
              near(paint.quads[0].left, t.metric.card_inset) && near(paint.quads[0].right, 300 - t.metric.card_inset) &&
              near(paint.quads[0].bottom, 56) && near(ui_card_content(t, {0, 0, 100, 100}).left, t.metric.card_inset),
          "card: the plate, its hover and focus ring, and the inset hairline");
    paint_stream_release(&paint);
}

void test_previews(Check check, const UiSettingsThemeTokens& t) noexcept {
    const auto tile = ui_preview_rect({10, 20, 330, 400}, UiPreviewAspect::r16x10);
    check(near(ui_rect_width(tile), 320) && near(ui_rect_height(tile), 200) && near(tile.top, 20) &&
              near(ui_rect_height(ui_preview_rect({0, 0, 320, 400}, UiPreviewAspect::r16x9)), 180),
          "preview tile: 16:10 and 16:9 at the area's width");
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_preview_paint(&paint, t, tile, 12, UiPreviewState::ready) && paint.count == 0 &&
              paint_stream_reset(&paint, 1000, 1000) && ui_preview_paint(&paint, t, tile, 12, UiPreviewState::loading) &&
              paint.count == 2 && paint_stream_reset(&paint, 1000, 1000) &&
              ui_preview_paint(&paint, t, tile, 12, UiPreviewState::failed) && paint.count == 3 &&
              paint_stream_reset(&paint, 1000, 1000) && ui_pill_paint(&paint, t, {0, 0, 80, 28}) && paint.count == 1 &&
              paint_stream_reset(&paint, 1000, 1000) &&
              ui_glyph_tile_paint(&paint, t, {0, 0, 40, 40}, UiIcon::puzzle, UiTileTone::selected) && paint.count == 2,
          "preview tile: a decoded image leaves the plate to the renderer, a pending or failed one keeps it");
    paint_stream_release(&paint);

    UiPreviewResidency residency{};
    UiPreviewPlan plan{};
    const u64 visible[]{1, 2, 3, 4}, ahead[]{5, 6, 7, 8};
    ui_preview_plan(&residency, visible, 4, ahead, 4, &plan);
    bool in_order = plan.request_count == 8 && plan.evict_count == 0;
    for (u32 i = 0; in_order && i < 8; ++i)
        in_order = plan.request[i] == i + 1;
    check(in_order, "preview plan: visible tiles are requested first, then the row ahead");
    for (u64 key = 1; key <= 8; ++key)
        ui_preview_resident(&residency, key, true);
    const u64 now_visible[]{3, 4, 5}, now_ahead[]{6};
    ui_preview_plan(&residency, now_visible, 3, now_ahead, 1, &plan);
    check(plan.request_count == 0 && plan.evict_count == 0 && residency.count == 8, "preview plan: resident tiles need nothing");
    for (u64 key = 100; key < 116; ++key)
        ui_preview_resident(&residency, key, true);
    check(residency.count == kUiPreviewResident, "preview plan: the resident set fills to its cap");
    const u64 far_visible[]{3, 4, 200};
    ui_preview_plan(&residency, far_visible, 3, nullptr, 0, &plan);
    bool safe = plan.evict_count == 1 && plan.request_count == 1 && plan.request[0] == 200;
    for (u32 i = 0; i < plan.evict_count; ++i)
        safe = safe && plan.evict[i] != 3 && plan.evict[i] != 4 && plan.evict[i] != 200;
    check(safe && residency.count == kUiPreviewResident - 1,
          "preview plan: a new tile evicts the least recently seen one, never a visible tile");
    ui_preview_resident(&residency, 200, true);
    ui_preview_resident(&residency, 3, false);
    check(residency.count == kUiPreviewResident - 1, "preview plan: a failed decode drops the tile from the resident set");

    UiPreviewLoop loop{};
    check(!ui_preview_loop_update(&loop, 7, 0, false, 1000) && ui_preview_loop_next_wake(loop) == 1000 + kUiPreviewLoopDelayMs &&
              !ui_preview_loop_update(&loop, 7, 0, false, 1200) && ui_preview_loop_update(&loop, 7, 0, false, 1300) &&
              ui_preview_loop_next_wake(loop) == 0,
          "preview loop: hover starts the loop after the delay, with the deadline reported until then");
    check(ui_preview_loop_update(&loop, 7, 9, false, 2000) && loop.key == 9 && !ui_preview_loop_update(&loop, 0, 0, false, 2100) &&
              loop.key == 0,
          "preview loop: keyboard focus plays at once and leaving stops it");
    UiPreviewLoop reduced{};
    check(!ui_preview_loop_update(&reduced, 7, 0, true, 5000) && ui_preview_loop_next_wake(reduced) == 0 &&
              !ui_preview_loop_update(&reduced, 7, 7, true, 9000) && (ui_preview_loop_toggle(&reduced, 7), reduced.playing) &&
              ui_preview_loop_update(&reduced, 7, 0, true, 9100) && !ui_preview_loop_update(&reduced, 8, 0, true, 9200),
          "preview loop: reduced motion never autoplays; the play pill starts one preview that stops when the key changes");

    check(near(ui_skeleton_pulse(0, false), 0) && near(ui_skeleton_pulse(kUiSkeletonPeriodMs / 2, false), 1) &&
              near(ui_skeleton_pulse(kUiSkeletonPeriodMs, false), 0) && ui_skeleton_pulse(400, false) > 0 &&
              ui_skeleton_pulse(400, false) < 1 && ui_skeleton_pulse(400, true) == 0 && ui_skeleton_next_wake(1000, false) > 1000 &&
              ui_skeleton_next_wake(1000, true) == 0,
          "skeleton: the pulse breathes over 1.6 s and stops, with no wake-ups, under reduced motion");
    check(near(ui_skeleton_card_height(t, UiSkeletonCard::plugin, 253), 168) &&
              near(ui_skeleton_card_height(t, UiSkeletonCard::theme, 320), 200 + 136) &&
              near(ui_skeleton_card_height(t, UiSkeletonCard::effect, 320), 180 + 136),
          "skeleton: card heights follow the cards they stand in for");
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_skeleton_card_paint(&paint, t, {0, 0, 253, 168}, UiSkeletonCard::plugin, .5f) && paint.count >= 7 &&
              paint_stream_reset(&paint, 1000, 1000) && ui_skeleton_card_paint(&paint, t, {0, 0, 253, 294}, UiSkeletonCard::theme, 0) &&
              paint.count >= 8 && paint_stream_reset(&paint, 1000, 1000) &&
              ui_skeleton_bar_paint(&paint, t, {0, 0, 50, 12}, .25f, true) && paint.count == 1,
          "skeleton: cards draw their bars on their own geometry");
    paint_stream_release(&paint);
}

void test_panels(Check check, const UiSettingsThemeTokens& t) noexcept {
    const UiRect area{0, 100, 800, 600};
    const auto g = ui_empty_geometry(t, area, utf8_literal("Title"), utf8_literal("A short body."), 40, true, nullptr);
    check(near((g.disc.left + g.disc.right) * .5f, 400) && near(g.disc.top, 100) && near(ui_rect_width(g.disc), 64) &&
              g.title.top > g.disc.bottom && g.body.top > g.title.bottom && g.actions.top > g.body.bottom &&
              near(ui_rect_height(g.actions), 40) && g.detail.top > g.actions.bottom && near(g.height, g.detail.bottom - 100) &&
              inside(g.glyph, g.disc) && near(ui_rect_width(g.glyph), 40),
          "empty state: disc, title, body, actions and detail stack, centred");
    const auto bare = ui_empty_geometry(t, area, utf8_literal("Title"), {}, 0, false, nullptr);
    check(!ui_rect_has_area(bare.actions) && !ui_rect_has_area(bare.detail) && bare.height < g.height,
          "empty state: parts that are absent take no room");
    const float widths[]{100, 80};
    UiRect actions[2]{};
    ui_empty_place_actions(t, {0, 0, 800, 40}, widths, 2, actions);
    check(near(actions[0].left, 400 - 94) && near(actions[1].right, 400 + 94) && near(actions[1].left - actions[0].right, 8),
          "empty state: buttons centre with an 8 DIP gap");
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_empty_paint(&paint, t, g, UiIcon::cloud_off, true) && paint.count == 2,
          "empty state: the disc and glyph paint");
    paint_stream_release(&paint);

    UiPermissionRow row{utf8_literal("Reads the messages you receive"), utf8_literal("read_messages"), false};
    const auto plain = ui_permission_geometry(t, row, 0, 0, 600, 0, nullptr);
    const auto hosts = ui_permission_geometry(t, row, 0, 0, 600, 2, nullptr);
    check(plain.height >= 48 && near(ui_rect_width(plain.tile), 28) && inside(plain.tile, plain.row) &&
              plain.sentence.left > plain.tile.right && plain.name.left > plain.sentence.left && plain.name.right <= 600 - t.metric.card_inset + .01f &&
              !ui_rect_has_area(plain.hosts) && hosts.height > plain.height + 2 * kUiKitChipHeight &&
              hosts.hosts.top >= hosts.sentence.bottom && near(hosts.hosts.left, hosts.sentence.left),
          "permission row: tile, sentence and code name in a row of at least 48; host chips wrap under the sentence");
    check(ui_risk_color(t, UiRisk::low).green == t.color.risk_low.green && ui_risk_color(t, UiRisk::high).red == t.color.risk_high.red &&
              paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_permission_paint(&paint, t, plain, UiIcon::eye, false) && paint.count == 3 &&
              paint_stream_reset(&paint, 1000, 1000) && ui_permission_paint(&paint, t, plain, UiIcon::eye, true) && paint.count == 2,
          "permission row: risk colours and the hairline between rows of a group");

    UiKnob knob{UiKnobKind::slider, utf8_literal("Drift speed"), utf8_literal("Global default: 1x."), utf8_literal("1.5x"), 31, 15, 10,
                false, false, true, {}};
    const auto base = ui_knob_geometry(t, knob, 0, 0, 380, 0, nullptr);
    knob.overridden = true;
    const auto changed = ui_knob_geometry(t, knob, 0, 0, 380, 0, nullptr);
    check(base.height > 60 && ui_rect_has_area(base.control) && ui_rect_has_area(base.hint) && !ui_rect_has_area(base.reset) &&
              !ui_rect_has_area(base.dot),
          "knob row: a resting slider has a label, control and hint but no dot or reset");
    check(ui_rect_has_area(changed.reset) && ui_rect_has_area(changed.dot) && changed.dot.left > changed.label.left &&
              changed.reset.right <= 380 - t.metric.card_inset + .01f,
          "knob row: an override adds the dot after the label and the reset button at the end");
    check(changed.value.right <= changed.reset.left && near(changed.height, base.height),
          "knob row: the value sits before the reset and an override does not change the height");
    UiKnob toggle{UiKnobKind::toggle, utf8_literal("React to pointer"), {}, {}, 2, 1, 0, true, false, true, {}};
    const auto t_g = ui_knob_geometry(t, toggle, 0, 0, 380, 0, nullptr);
    check(near(ui_rect_width(t_g.control), t.metric.switch_width) && near(t_g.control.right, 380 - t.metric.card_inset) &&
              t_g.label.right <= t_g.control.left && t_g.height >= t.metric.switch_height + 24,
          "knob row: a switch knob sits at the end of its row");
    check(paint_stream_reset(&paint, 1000, 1000) &&
              ui_knob_paint(&paint, t, toggle, t_g, {true, false, false, false}, {}, ~0u) && paint.count >= 2,
          "knob row: a switch knob paints its track and thumb");
    check(paint_stream_reset(&paint, 1000, 1000) && ui_knob_paint(&paint, t, knob, changed, {true, false, false, false}, {}, ~0u) &&
              paint.count >= 5,
          "knob row: an overridden slider paints its dot, reset, track and default tick");
    check(ui_knob_beside(700) && !ui_knob_beside(639) && near(ui_knob_column_width(t, 700), (700 - 16) * .5f) &&
              near(ui_knob_column_width(t, 500), 500),
          "knob panel: two columns from 640 DIP, stacked below");
    paint_stream_release(&paint);

    const UiSettingsLayoutTokens& lt = t;
    UiRowSpec spec{utf8_literal("Title"), utf8_literal("A description."), 200, 40, false, false};
    const auto side = ui_row_geometry(lt, spec, 0, 0, 600, nullptr);
    spec.stacked = true;
    const auto stacked = ui_row_geometry(lt, spec, 0, 0, 600, nullptr);
    check(side.height >= t.metric.row_min_height && near(side.control.right, 600 - t.metric.card_inset) &&
              near(ui_rect_width(side.control), 200) && side.title.right <= side.control.left - t.metric.control_gap + .01f &&
              near((side.control.top + side.control.bottom) * .5f, side.height * .5f) && stacked.control.top >= stacked.description.bottom &&
              near(stacked.control.right, 600 - t.metric.card_inset) && stacked.height > side.height - 1,
          "row anatomy: text at the start, one trailing control centred on it, or under the text when stacked");
    const auto cell = ui_slider_value_geometry(lt, {0, 0, 300, 24}, 11, 5);
    check(near(cell.value.right, 300) && near(ui_rect_width(cell.value), t.metric.value_width) &&
              cell.slider.track_right <= cell.value.left && cell.slider.bar.left >= 0,
          "slider with value: the track and a value column share the cell");
    check(near(ui_kit_text_height(nullptr, {}, kUiKitTextSmall, 100), 0) && near(ui_kit_line_height(kUiKitTextTitle), 30) &&
              near(ui_kit_line_height(kUiKitTextSmall), 18) && near(ui_kit_line_height(kUiKitTextBody), 20) &&
              near(ui_kit_line_height(kUiKitTextEyebrow), 16) &&
              ui_kit_text_height(nullptr, utf8_literal("one two three four five six seven eight nine ten eleven twelve"),
                                 kUiKitTextSmall, 60) > 18 * 2,
          "text ramp: line heights follow the ramp and long text wraps in the estimate");
    const UiRect tab{10, 0, 70, 40};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_tab_bar_paint(&paint, t, {0, 0, 400, 40}, tab, false) && paint.count == 2 &&
              paint_stream_reset(&paint, 1000, 1000) && ui_tab_bar_paint(&paint, t, {0, 0, 400, 40}, tab, true) && paint.count == 3 &&
              near(ui_tab_width(lt, 50, 18), 74) && near(ui_tab_width(lt, 50, 0), 50) &&
              paint_stream_reset(&paint, 1000, 1000) && ui_count_badge_paint(&paint, t, {0, 0, 18, 18}, true) && paint.count == 1,
          "tabs: the rule and underline, the focus ring and the count badge");
    paint_stream_release(&paint);
}

void test_markdown(Check check) noexcept {
    static char text[kUiMarkdownSourceMaximum];
    static TextStyleSpan spans[128];
    static UiMarkdownPart parts[16];
    static u32 scratch[kUiMarkdownSourceMaximum];
    UiMarkdown block{};
    block.text = {text, sizeof(text)};
    block.spans = {spans, 128};
    block.parts = {parts, 16};
    const auto source = utf8_literal("### What's new\n\nFirst **bold** line with `code`.\n- one\n  - nested\n- two\n");
    const UiMarkdownOptions options{true, text_size_sm};
    check(ui_markdown_parse(&block, source, options, {scratch, kUiMarkdownSourceMaximum}) && block.parsed && block.span_count > 3 &&
              contains_bytes(text, block.text_bytes, "WHAT'S NEW") && !contains_bytes(text, block.text_bytes, "###") &&
              !contains_bytes(text, block.text_bytes, "**") && block.text_bytes <= source.bytes,
          "markdown: the chat parser's output with the ### heading as an uppercase eyebrow");
    constexpr u32 kListFlagsMask = text_style_list | text_style_list_bullet | kTextStyleListDepthMask;
    bool folded = true, covers = true, plain = true;
    for (u32 p = 0; p < block.part_count; ++p) {
        u32 offset = 0;
        for (u32 i = parts[p].span_first; i < parts[p].span_first + parts[p].span_count; ++i) {
            folded = folded && spans[i].size == 0 && text_style_size_code_ratio(spans[i].flags) > 0;
            covers = covers && spans[i].byte_offset == offset;
            plain = plain && (p == 0 || !(spans[i].flags & kListFlagsMask));
            offset += spans[i].byte_count;
        }
        covers = covers && offset == parts[p].bytes;
    }
    check(block.part_count == 4 && folded && covers && plain &&
              (spans[0].flags & kTextStyleHeadingMask) == kTextStyleSizeCodeXs,
          "markdown: the text is cut into a run and its items; spans are part-relative, folded to shaping flags and "
          "cover each part; the eyebrow uses the 12 pt code; items carry no list flags");
    check(parts[0].marker == UiMarkdownMarker::none && parts[1].marker == UiMarkdownMarker::bullet && parts[1].level == 0 &&
              parts[2].marker == UiMarkdownMarker::dash && parts[2].level == 1 && parts[2].bytes == 6 &&
              parts[3].marker == UiMarkdownMarker::bullet,
          "markdown: a first-level item has a bullet and a nested item a dash");
    const u64 signature = block.source_signature;
    check(ui_markdown_parse(&block, source, options, {scratch, kUiMarkdownSourceMaximum}) && block.source_signature == signature &&
              block.parsed,
          "markdown: an unchanged source keeps its parse");
    const float wide = ui_markdown_height(&block, 600, nullptr);
    const float cached = ui_markdown_height(&block, 600.4f, nullptr);
    const float narrow = ui_markdown_height(&block, 120, nullptr);
    check(wide >= 5 * 20 && near(wide, cached) && block.measured_width == 120 && narrow > wide &&
              ui_markdown_height(&block, 600, nullptr) == wide,
          "markdown: heights come per part, are cached per whole-DIP width and grow when narrower");
    const Color ink{1, 1, 1, 1}, quiet{.5f, .5f, .5f, 1};
    const UiTextCommand run = ui_markdown_command(&block, 0, false, nullptr, {}, {0, 0, 300, 400}, {0, 0, 300, 50}, ink, quiet);
    const UiTextCommand item = ui_markdown_command(&block, 2, false, nullptr, {}, {0, 0, 300, 400}, {0, 0, 300, 400}, ink, quiet);
    const UiTextCommand dash = ui_markdown_command(&block, 2, true, nullptr, {}, {0, 0, 300, 400}, {0, 0, 300, 400}, ink, quiet);
    const UiTextCommand bullet = ui_markdown_command(&block, 1, true, nullptr, {}, {0, 0, 300, 400}, {0, 0, 300, 400}, ink, quiet);
    const UiTextCommand outside = ui_markdown_command(&block, 3, false, nullptr, {}, {0, 0, 300, 400}, {0, 0, 300, 10}, ink, quiet);
    const UiRect shown{0, 0, 300, parts[3].top + parts[3].height};
    const UiTextCommand faint = ui_markdown_command(&block, 3, false, nullptr, {}, shown, shown, ink, quiet, 48);
    const UiTextCommand clear = ui_markdown_command(&block, 0, false, nullptr, {}, shown, shown, ink, quiet, 48);
    check(run.text.bytes == parts[0].bytes && run.styles.count == parts[0].span_count && run.wrapping == TextWrapping::wrap &&
              run.effect.themed_styles && run.clip.bottom == 50 && near(run.rectangle.left, 0),
          "markdown: a run is one wrapped, themed text command over the block's own storage");
    check(item.text.bytes == 6 && near(item.rectangle.left, 40) && near(item.rectangle.right, 300) &&
              item.rectangle.top == parts[2].top && dash.text.bytes == 3 && static_cast<u8>(dash.text.data[2]) == 0x93 &&
              bullet.text.bytes == 3 && static_cast<u8>(bullet.text.data[2]) == 0xA2 &&
              dash.horizontal_alignment == UiTextAlignment::end && dash.wrapping == TextWrapping::no_wrap &&
              dash.rectangle.right < item.rectangle.left && ui_markdown_part_count(block) == 4,
          "markdown: an item is inset by its level with its marker hanging in front, a dash from the second level");
    check(near(item.color.red, .5f) && near(dash.color.red, .5f) && near(bullet.color.red, 1) && near(run.color.red, 1) &&
              near(faint.color.red, 1) && faint.color.alpha < .5f && faint.color.alpha >= .2f && near(clear.color.alpha, 1),
          "markdown: nested items are muted and a part ending in the fade band is fainter");
    check(outside.text.bytes == 0 && ui_markdown_command(&block, 9, false, nullptr, {}, {0, 0, 300, 400}, {0, 0, 300, 400}, {}, {}).text.bytes == 0 &&
              ui_markdown_command(&block, 0, true, nullptr, {}, {0, 0, 300, 400}, {0, 0, 300, 400}, {}, {}).text.bytes == 0,
          "markdown: a part outside the clip, an unknown part and the marker of a plain run give no command");
    check(!ui_markdown_parse(&block, source, options, {scratch, 8}) && !block.parsed,
          "markdown: scratch smaller than the source is rejected and leaves the block unparsed");

    UiMarkdownBank* bank = nullptr;
    UiMarkdown* blocks[kUiMarkdownBankBlocks + 1]{};
    bool distinct = true;
    for (u32 i = 0; i < kUiMarkdownBankBlocks; ++i) {
        blocks[i] = ui_markdown_bank_block(&bank, i + 1);
        distinct = distinct && blocks[i];
        for (u32 j = 0; j < i; ++j)
            distinct = distinct && blocks[j] != blocks[i];
    }
    check(distinct && bank && ui_markdown_bank_scratch(bank).count == kUiMarkdownSourceMaximum &&
              ui_markdown_bank_block(&bank, 3) == blocks[2],
          "markdown bank: one block per row, the same row finds its block");
    blocks[kUiMarkdownBankBlocks] = ui_markdown_bank_block(&bank, 99);
    check(blocks[kUiMarkdownBankBlocks] && blocks[kUiMarkdownBankBlocks] != blocks[2] && ui_markdown_bank_block(&bank, 3) == blocks[2],
          "markdown bank: a new row evicts the least recently used block");
    check(ui_markdown_parse(blocks[0], source, options, ui_markdown_bank_scratch(bank)) && blocks[0]->parsed,
          "markdown bank: blocks parse into their own storage");
    ui_markdown_bank_release(&bank);
    check(!bank && !ui_markdown_bank_block(&bank, 0), "markdown bank: release frees it and an empty owner gets no block");
}
} // namespace

void test_ui_kit(Check check) noexcept {
    const UiSettingsThemeTokens tokens(kDefaultTheme);
    test_search_and_segmented(check, tokens);
    test_dropdown(check, tokens);
    test_chips(check, tokens);
    test_grid(check, tokens);
    test_previews(check, tokens);
    test_panels(check, tokens);
    test_markdown(check);
}
} // namespace maicord
