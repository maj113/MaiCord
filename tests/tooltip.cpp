#include "maicord/ui/tooltip.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/message_reaction_presentation.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    value->size = {minimum(request.constraint.width, request.text.bytes * 7.f), 18};
    value->baseline = 14;
    value->line_count = 1;
    return true;
}
bool failed_measure(void*, const UiTextMeasureRequest&, UiTextMeasureValue*) noexcept {
    return false;
}
} // namespace
void test_tooltip(void (*check)(bool, const char*) noexcept) noexcept {
    UiWidget widgets[2]{};
    UiLayoutNode nodes[2]{};
    UiWidgetManager manager{};
    auto root_style = ui_layout_style();
    root_style.width = ui_layout_fill();
    root_style.height = ui_layout_fill();
    auto button_style = ui_layout_style();
    button_style.width = ui_layout_fixed(32);
    button_style.height = ui_layout_fixed(32);
    const bool initialized = ui_widget_manager_initialize(&manager, {widgets, 2}, {nodes, 2});
    const auto root =
        initialized
            ? ui_widget_manager_add(
                  &manager, ui_widget_invalid_handle(),
                  {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Overlay"), root_style}})
            : UiWidgetResult{};
    const auto button =
        root.ok() ? ui_widget_manager_add(&manager, root.handle,
                                          {2,
                                           UiWidgetKind::button,
                                           ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable,
                                           {utf8_literal("Button"), button_style}})
                  : UiWidgetResult{};
    UiTooltipScene scene{};
    UiTextMeasureRecord records[4]{};
    UiTextMeasureCache cache{};
    const UiRect viewport{0, 0, 320, 180};
    check(button.ok() && ui_widget_manager_layout_rectangle(&manager, viewport).ok() &&
              ui_tooltip_bind_text_owner(&scene, {&manager, root.handle}) &&
              ui_text_measure_cache_initialize(&cache, {records, 4}, measure, nullptr),
          "tooltip: bounded source and independent overlay owner initialize");
    if (!button.ok())
        return;
    char label[]{'S', 'e', 't', 't', 'i', 'n', 'g', 's'};
    UiTooltipTarget target{
        {{&manager, button.handle}, 7, 1, 2, 3}, {12, 12, 44, 44}, {label, sizeof(label)}};
    check(ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::pointer, 100) &&
              ui_tooltip_next_wake(scene) == 600 && !ui_tooltip_tick(&scene, 599, &cache) && !scene.visible,
          "tooltip: pointer delay has exact boundary and no early visible frame");
    label[0] = 'X';
    check(!ui_tooltip_offer(&scene, {target.key, target.anchor, utf8_literal("Settings")}, viewport,
                            UiTooltipTrigger::pointer, 200) &&
              ui_tooltip_next_wake(scene) == 600 && ui_tooltip_tick(&scene, 600, &cache) &&
              ui_tooltip_visible(scene) && !ui_tooltip_next_wake(scene) && scene.text[0] == 'S' &&
              scene.placement == UiTooltipPlacement::below,
          "tooltip: copied text and stable hover preserve deadline, fit below upper edge and leave steady "
          "display asleep");
    UiTextCommand command{};
    UiTextCommandStream text{};
    UiTextOutput output{};
    check(ui_text_command_stream_initialize(&text, {&command, 1}),
          "tooltip: single native text command initializes");
    output.stream = &text;
    check(ui_tooltip_text_output(scene, kDefaultTheme, &output) && text.count == 1 &&
              command.owner.handle.slot == root.handle.slot && command.rectangle.left >= viewport.left &&
              command.rectangle.right <= viewport.right,
          "tooltip: text owns viewport root rather than smaller source control clip");
    const auto key = scene.key;
    UiTooltipStyle style{};
    style.padding_x = 14;
    style.radius = 8;
    check(ui_tooltip_offer(&scene, {target.key, target.anchor, utf8_literal("Settings")}, viewport,
                           UiTooltipTrigger::pointer, 700, style) &&
              ui_tooltip_key_equal(scene.key, key) && ui_tooltip_tick(&scene, 700, &cache) &&
              ui_tooltip_visible(scene) && scene.style.radius == 8,
          "tooltip: theme metrics remeasure visible presentation without replacing source authority");
    manager.storage[button.handle.slot].flags &= static_cast<u16>(~ui_widget_enabled);
    check(ui_tooltip_tick(&scene, 701, &cache) && !scene.visible && !ui_tooltip_next_wake(scene),
          "tooltip: disabled generation-checked source hides immediately and cancels wake");
    manager.storage[button.handle.slot].flags |= ui_widget_enabled;
    target.label = utf8_literal("Settings");
    ++target.key.source_revision;
    check(ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::keyboard, 1000) &&
              ui_tooltip_next_wake(scene) == 1350 && ui_tooltip_tick(&scene, 1350, &cache) &&
              ui_tooltip_clear(&scene) && !ui_tooltip_next_wake(scene),
          "tooltip: keyboard focus uses bounded delay and cancellation releases all timing");
    auto stale = target;
    ++stale.key.owner.handle.generation;
    check(!ui_tooltip_offer(&scene, stale, viewport, UiTooltipTrigger::pointer, 1500) &&
              !ui_tooltip_next_wake(scene),
          "tooltip: stale widget generation cannot admit borrowed source");
    target.anchor = {321, 0, 350, 32};
    check(!ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::pointer, 1500),
          "tooltip: fully clipped target is inert");
    target.anchor = {270, 140, 300, 172};
    target.placement = UiTooltipPlacement::right;
    const u32 invalid_coordinate_bits = 0x7fc00000u;
    auto invalid_anchor = target;
    copy_bytes(&invalid_anchor.anchor.left, &invalid_coordinate_bits, sizeof(float));
    check(!ui_tooltip_offer(&scene, invalid_anchor, viewport, UiTooltipTrigger::pointer, 1700) &&
              !ui_tooltip_next_wake(scene),
          "tooltip: raw NaN anchor cannot become valid through viewport intersection");
    check(!ui_tooltip_offer(&scene, target, viewport, static_cast<UiTooltipTrigger>(2), 1700),
          "tooltip: invalid trigger cannot schedule a wake");
    UiTooltipStyle invalid_height{};
    invalid_height.maximum_height = 16;
    invalid_height.padding_y = 8;
    check(!ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::pointer, 1700, invalid_height),
          "tooltip: padding must leave positive measured height");
    UiTooltipStyle extreme{};
    extreme.padding_x = 0;
    extreme.margin = 32;
    check(!ui_tooltip_offer(&scene, target, {250, 100, 315, 180}, UiTooltipTrigger::pointer, 1700, extreme),
          "tooltip: viewport inset must leave the minimum measured label width");
    check(ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::pointer, 1700) &&
              ui_tooltip_tick(&scene, 2200, &cache),
          "tooltip: edge presentation prepares clip guard fixture");
    const auto root_clip = nodes[widgets[root.handle.slot].layout.slot].clip_rectangle;
    nodes[widgets[root.handle.slot].layout.slot].clip_rectangle = {0, 0, 1, 1};
    check(!ui_tooltip_visible(scene),
          "tooltip: clipped emission owner cannot silently hide only its label");
    nodes[widgets[root.handle.slot].layout.slot].clip_rectangle = root_clip;
    ui_tooltip_clear(&scene);
    ui_text_measure_cache_invalidate(&cache);
    cache.backend = failed_measure;
    check(ui_tooltip_offer(&scene, target, viewport, UiTooltipTrigger::pointer, 2000) &&
              ui_tooltip_tick(&scene, 2500, &cache) && !scene.visible && !ui_tooltip_next_wake(scene),
          "tooltip: failed native measurement cancels deadline rather than polling");
    // Reaction tooltips: an emoji square leads a start-aligned label whose
    // reactor names are bold; names that finish loading replace the shown
    // label in place.
    cache.backend = measure;
    ui_text_measure_cache_invalidate(&cache);
    const float padding_x = UiTooltipStyle{}.padding_x, padding_y = UiTooltipStyle{}.padding_y;
    const TextStyleSpan names[]{{18, 3, 0}};
    UiTooltipTarget reaction{{target.key.owner, 7, 5, 1, 0x300},
                             {12, 12, 44, 44},
                             utf8_literal(":fire:"),
                             UiTooltipPlacement::below,
                             {},
                             {{}, utf8_literal("\xF0\x9F\x94\xA5")}};
    UiRect square{};
    check(ui_tooltip_offer(&scene, reaction, viewport, UiTooltipTrigger::pointer, 3000) &&
              ui_tooltip_tick(&scene, 3500, &cache) && ui_tooltip_emoji(scene, &square) &&
              ui_rect_width(square) == 48 && square.left == scene.rectangle.left + padding_x &&
              ui_rect_height(scene.rectangle) == 48 + padding_y * 2 &&
              ui_rect_width(scene.rectangle) == 6 * 7 + 48 + 12 + padding_x * 2,
          "tooltip reaction: the emoji square leads the label and sets the height");
    reaction.label = utf8_literal(":fire: reacted by Ada");
    reaction.bold = {names, 1};
    check(ui_tooltip_offer(&scene, reaction, viewport, UiTooltipTrigger::pointer, 3600) &&
              ui_tooltip_visible(scene) && ui_tooltip_next_wake(scene) == 3600 &&
              ui_tooltip_tick(&scene, 3600, &cache) && ui_tooltip_visible(scene) &&
              ui_rect_width(scene.rectangle) == 21 * 7 + 48 + 12 + padding_x * 2 && ui_tooltip_emoji(scene, &square),
          "tooltip reaction: loaded names replace the shown label without a fresh delay");
    UiTextCommand commands[2]{};
    UiTextCommandStream reaction_text{};
    UiTextOutput reaction_output{&reaction_text, nullptr};
    check(ui_text_command_stream_initialize(&reaction_text, {commands, 2}) &&
              ui_tooltip_text_output(scene, kDefaultTheme, &reaction_output) && reaction_text.count == 2 &&
              commands[0].style_flags == text_style_jumbo_emoji && ui_rect_equal(commands[0].rectangle, square) &&
              commands[1].horizontal_alignment == UiTextAlignment::start &&
              commands[1].rectangle.left == square.right + 12 && commands[1].styles.count == 2 &&
              commands[1].styles[1].byte_offset == 18 && commands[1].styles[1].byte_count == 3 &&
              (commands[1].styles[1].flags & text_style_bold) && !(commands[1].styles[0].flags & text_style_bold),
          "tooltip reaction: a Unicode emoji is jumbo text and reactor names are bold runs");
    reaction.emoji = {media_asset_custom_emoji(42, false), {}};
    check(ui_tooltip_offer(&scene, reaction, viewport, UiTooltipTrigger::pointer, 3700) &&
              ui_tooltip_tick(&scene, 3700, &cache) && ui_tooltip_emoji(scene, &square) &&
              ui_text_command_stream_initialize(&reaction_text, {commands, 2}) &&
              ui_tooltip_text_output(scene, kDefaultTheme, &reaction_output) && reaction_text.count == 1 &&
              commands[0].rectangle.left == square.right + 12,
          "tooltip reaction: a custom emoji leaves its square to the host's image");
    const TextStyleSpan overlapping[]{{0, 5, 0}, {3, 4, 0}};
    reaction.bold = {overlapping, 2};
    ui_tooltip_offer(&scene, reaction, viewport, UiTooltipTrigger::pointer, 3800);
    check(!scene.visible && !scene.pending && !ui_tooltip_emoji(scene, &square),
          "tooltip reaction: overlapping bold ranges are rejected");
    // The reaction tooltip's wording: the emoji's name, then up to three bold
    // names joined like Discord's, and the rest counted.
    char wording[kUiTooltipMaximumTextBytes];
    TextStyleSpan bold[3]{};
    const Utf8View people[]{utf8_literal("Ada"), utf8_literal("Grace"), utf8_literal("Alan")};
    const auto words = [&](u64 emoji_id, Utf8View emoji, u32 shown, u32 count, Utf8View expected,
                           u32 output_bytes = kUiTooltipMaximumTextBytes) noexcept {
        const auto result = message_reaction_tooltip_label(emoji_id, emoji, {people, shown}, count,
                                                           {wording, output_bytes}, {bold, 3});
        return utf8_equal({wording, result.bytes}, expected) && result.bold_count == (result.bytes > 7 ? shown : 0);
    };
    check(words(5, utf8_literal("party"), 0, 4, utf8_literal(":party:")) &&
              words(5, utf8_literal("party"), 1, 1, utf8_literal(":party: reacted by Ada")) &&
              bold[0].byte_offset == 19 && bold[0].byte_count == 3 &&
              words(5, utf8_literal("party"), 1, 2, utf8_literal(":party: reacted by Ada and 1 other")) &&
              words(5, utf8_literal("party"), 2, 2, utf8_literal(":party: reacted by Ada and Grace")) &&
              words(5, utf8_literal("party"), 2, 4, utf8_literal(":party: reacted by Ada, Grace, and 2 others")) &&
              words(5, utf8_literal("party"), 3, 3, utf8_literal(":party: reacted by Ada, Grace, and Alan")) &&
              bold[2].byte_offset == 35 && bold[2].byte_count == 4 &&
              words(5, utf8_literal("party"), 3, 15,
                    utf8_literal(":party: reacted by Ada, Grace, Alan, and 12 others")),
          "tooltip reaction label: names join with Oxford commas and the rest are counted");
    check(words(0, utf8_literal("\xF0\x9F\x92\x94"), 1, 1, utf8_literal(":broken_heart: reacted by Ada")) &&
              bold[0].byte_offset == 26 &&
              words(5, utf8_literal("party"), 3, 3, utf8_literal(":party:"), 24),
          "tooltip reaction label: Unicode emoji use their shortcode and names that do not fit are dropped");
}
} // namespace maicord
