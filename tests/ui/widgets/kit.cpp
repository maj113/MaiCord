#include "maicord/ui/widgets/kit.hpp"
#include "maicord/ui/widgets/kit_markdown.hpp"
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

void test_segmented(Check check, const UiSettingsThemeTokens& t) noexcept {
    PaintStream paint{};
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
    check(ui_segmented_fits(180, 400) && !ui_segmented_fits(300, 400),
          "segmented: a track over 60% of its row falls back to sitting under the text");
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_segmented_paint(&paint, t, track, 2, 0, 1, {true, false, false, true}) &&
              paint.count == 4 && paint_stream_reset(&paint, 1000, 1000) &&
              ui_segmented_paint(&paint, t, track, 2, ~0u, ~0u, {false, false, false, false}) && paint.count == 1,
          "segmented: the track, the selected plate, a hovered plate and the focus ring; no selection paints the track only");
    paint_stream_release(&paint);
}

void test_dropdown(Check check, const UiSettingsThemeTokens& t) noexcept {
    check(near(ui_dropdown_width(t, 0, 100, 320), t.metric.select_width) &&
              near(ui_dropdown_width(t, 0, 300, 320), 320) && near(ui_dropdown_width(t, 0, 300, 100), t.metric.select_width) &&
              ui_dropdown_width(t, 40, 200, 500) > ui_dropdown_width(t, 0, 200, 500),
          "dropdown: width follows the value within the select width and the row's share");
}

void test_chips(Check check, const UiSettingsThemeTokens& t) noexcept {
    const UiRect chip{0, 0, 100, 24};
    const auto label = ui_chip_label_rect(chip);
    PaintStream paint{};
    check(near(ui_chip_width(30), 46) && near(label.left, 8) && near(label.right, 92) &&
              paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_chip_paint(&paint, t, chip, UiChipTone::positive) && paint.count == 1 &&
              paint_stream_reset(&paint, 1000, 1000) && ui_card_hairline_paint(&paint, t, {0, 0, 300, 56}) && paint.count == 1 &&
              near(paint.quads[0].left, t.metric.card_inset) && near(paint.quads[0].right, 300 - t.metric.card_inset) &&
              near(paint.quads[0].bottom, 56),
          "chips: a pill whose label starts after its padding; cards get an inset hairline between rows");
    check(ui_chip_ink(t, UiChipTone::positive).green == t.color.positive.green &&
              ui_chip_ink(t, UiChipTone::brand).red == t.color.on_brand.red && ui_chip_ink(t, UiChipTone::dev).red == t.color.critical_text.red,
          "chips: tones take their ink from the settings tokens");
    paint_stream_release(&paint);
}

void test_text_ramp(Check check) noexcept {
    check(near(ui_kit_text_height(nullptr, {}, kUiKitTextSmall, 100), 0) && near(ui_kit_line_height(kUiKitTextTitle), 30) &&
              near(ui_kit_line_height(kUiKitTextSmall), 18) && near(ui_kit_line_height(kUiKitTextBody), 20) &&
              near(ui_kit_line_height(kUiKitTextEyebrow), 16) &&
              ui_kit_text_height(nullptr, utf8_literal("one two three four five six seven eight nine ten eleven twelve"),
                                 kUiKitTextSmall, 60) > 18 * 2,
          "text ramp: line heights follow the ramp and long text wraps in the estimate");
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
    constexpr u8 body = text_size_sm;
    check(ui_markdown_parse(&block, source, body, {scratch, kUiMarkdownSourceMaximum}) && block.parsed && block.span_count > 3 &&
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
    check(ui_markdown_parse(&block, source, body, {scratch, kUiMarkdownSourceMaximum}) && block.source_signature == signature &&
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
    check(!ui_markdown_parse(&block, source, body, {scratch, 8}) && !block.parsed,
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
    check(ui_markdown_parse(blocks[0], source, body, ui_markdown_bank_scratch(bank)) && blocks[0]->parsed,
          "markdown bank: blocks parse into their own storage");
    ui_markdown_bank_release(&bank);
    check(!bank && !ui_markdown_bank_block(&bank, 0), "markdown bank: release frees it and an empty owner gets no block");
}
} // namespace

void test_ui_kit(Check check) noexcept {
    const UiSettingsThemeTokens tokens(kDefaultTheme);
    test_segmented(check, tokens);
    test_dropdown(check, tokens);
    test_chips(check, tokens);
    test_text_ramp(check);
    test_markdown(check);
}
} // namespace maicord
