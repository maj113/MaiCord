#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/message_component_layout.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/typing_indicator.hpp"

namespace maicord {
namespace {
constexpr u32 widgets = kMessageSceneWidgetsPerRow + 1;
struct Fixture {
    MessageRecord record[1]; u32 order[1]; u8 text[64];
    MessageRichRecord rich_record[1]; u16 rich_order[1]; u8 rich_text[8192];
    u32 heights[1]; u64 fenwick[1];
    UiWidget widget[widgets]; UiLayoutNode node[widgets];
    MessageStore store; MessageRichStore rich; UiVariableList list;
    MessageTimeline timeline; MessageScene scene;
    MessageTextCommand commands[32]; char utf8[4096]; TextStyleSpan styles[128];
    MessageMarkdownSemantic semantics[32]; MessageTextMeasurement measurements[1];
    MessageTextSpoilerReveal spoilers[1]; u32 byte_styles[4096];
    MessageTextPresentation presentation;
    UiTextMeasureRecord cache_records[64]; UiTextMeasureCache cache;
    PaintStream paint;
};
Fixture fixture;

bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* output) noexcept {
    const float width = minimum(request.constraint.width, static_cast<float>(request.text.bytes) * 7);
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 7));
    const u32 lines = request.no_wrap ? 1 : (request.text.bytes + columns - 1) / columns;
    *output = {{width, static_cast<float>(lines) * 18}, 14, lines};
    return true;
}
bool finish(void*, const Theme&) noexcept { return true; }
bool contains(UiRect a, UiRect b) noexcept {
    return a.left <= b.left && a.top <= b.top && a.right >= b.right && a.bottom >= b.bottom;
}
}

void test_message_components(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = fixture;
    const auto payload = utf8_literal(R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{
        "id":"1","channel_id":"7","author":{"id":"9","username":"Bot"},"content":"",
        "timestamp":"2026-09-05T10:00:00Z","flags":32768,"type":20,
        "interaction":{"id":"20","type":2,"name":"vote","name_localized":"vote localized",
          "user":{"id":"11","username":"User","global_name":"Display","avatar":"0123456789abcdef0123456789abcdef"}},
        "components":[{"id":1,"type":17,"accent_color":16724838,"components":[
          {"id":2,"type":10,"content":"This server isn't listed in guides yet."},
          {"id":3,"type":1,"components":[
            {"id":4,"type":2,"style":5,"label":"Finish your submission","url":"https://example.com/submit"},
            {"id":5,"type":2,"style":2,"label":"Help","custom_id":"help"}]}]}]}})");
    u32 structurals[1024]{}; char packed_text[8192]{}; u64 ids[100]{};
    GatewayEnvelope envelope{}; GatewayClientEvent routed{}; ConstSlice<u8> packed{};
    GatewayMessageComponent components[40]{};
    GatewayMessageRichScratch scratch{}; scratch.components = {components, 40};
    GatewayMessageEvent message{};
    bool ok = gateway_decode_envelope(payload, {structurals, 1024}, 32, &envelope).ok() &&
        gateway_client_route_dispatch(envelope, {structurals, 1024}, 32, {packed_text, sizeof(packed_text)},
            {ids, 100}, &routed, &packed).ok() &&
        gateway_client_event_as_message(routed, packed, scratch, &message);
    check(ok && message.interaction.user_id == 11 && message.interaction.id == 20 &&
        utf8_equal(message.interaction.command_name, utf8_literal("vote localized")) &&
        utf8_equal(message.interaction.user_name, utf8_literal("Display")) &&
        message.components.count == 5 && components[0].accent_color == 16724838 &&
        components[0].has_accent_color && components[3].parent == 2 && components[4].parent == 2,
        "interaction_and_container_roundtrip_preserves_attribution_rgb_and_parentage");
    ok = ok && message_store_initialize(&f.store, {f.record, 1}, {f.order, 1}, {f.text, sizeof(f.text)}) &&
        message_reconcile_create(&f.store, message).ok() &&
        message_rich_store_initialize(&f.rich, {f.rich_record, 1}, {f.rich_order, 1}, {f.rich_text, sizeof(f.rich_text)}) &&
        message_rich_store_apply(&f.rich, 1, f.record[0].revision, message).ok() &&
        ui_variable_list_initialize(&f.list, {f.heights, 1}, {f.fenwick, 1}, 1, ui_extent_units(57)) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(1000), false) &&
        message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
        message_scene_initialize_owned(&f.scene, &f.timeline, 1, 1, {f.widget, widgets}, {f.node, widgets}) &&
        message_text_presentation_initialize(&f.presentation, {f.commands, 32}, {f.utf8, sizeof(f.utf8)},
            {f.styles, 128}, {f.semantics, 32}, {f.measurements, 1}, {f.spoilers, 1}, {f.byte_styles, 4096}) &&
        ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 64}, measure, nullptr);
    ChatStateStore state{}; NavigationView navigation{};
    const float widths[]{740.0f, 460.0f, 900.0f};
    for (float width : widths) {
        ok = ok && message_scene_update(&f.scene, {0, 0, width, 1000}).ok();
        const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr,
            &navigation, &kDefaultTheme, finish, nullptr, 1, 1};
        const auto result = ok ? message_text_coordinator_converge(request) : MessageTextCoordinatorResult{};
        ok = ok && result.ok();
        const auto* row = message_scene_find_row(f.scene, 0);
        const auto* container = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 0) : nullptr;
        const auto* text = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 1) : nullptr;
        const auto* action = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 2) : nullptr;
        const auto* first = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 3) : nullptr;
        const auto* second = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 4) : nullptr;
        check(ok && container && text && action && first && second &&
            contains(container->content_rectangle, text->rectangle) && contains(container->content_rectangle, action->rectangle) &&
            contains(action->content_rectangle, first->rectangle) && contains(action->content_rectangle, second->rectangle) &&
            first->rectangle.right <= second->rectangle.left && first->rectangle.top == second->rectangle.top &&
            text->rectangle.bottom <= action->rectangle.top &&
            row->rich_layout.components == ui_rect_height(container->rectangle),
            "component_tree_measures_and_owns_text_and_horizontal_buttons_during_resize");
        bool header = false;
        for (u32 i = 0; i < f.presentation.command_count; ++i)
            if (f.commands[i].measurement_kind == MessageTextMeasurementKind::reply)
                header = utf8_equal(f.commands[i].text, utf8_literal("Display used vote localized")) &&
                    f.commands[i].styles.count == 2 && f.commands[i].wrapping == TextWrapping::no_wrap;
        check(ok && header && row && !row->grouped && row->measured_reply_height > 0,
              "interaction_header_is_retained_single_line_styled_and_measured");
        if (!ok) break;
    }
    // A narrow timeline shrinks the container to the row and wraps the
    // action row's buttons onto a second line inside it.
    {
        ok = ok && message_scene_update(&f.scene, {0, 0, 300, 1000}).ok();
        const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr,
            &navigation, &kDefaultTheme, finish, nullptr, 1, 1};
        ok = ok && message_text_coordinator_converge(request).ok();
        const auto* row = message_scene_find_row(f.scene, 0);
        const auto* region = row ? message_scene_layout_node(f.scene, row->components) : nullptr;
        const auto* container = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 0) : nullptr;
        const auto* action = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 2) : nullptr;
        const auto* first = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 3) : nullptr;
        const auto* second = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 4) : nullptr;
        check(ok && region && container && action && first && second &&
            container->rectangle.right <= region->content_rectangle.right + .01f &&
            second->rectangle.top >= first->rectangle.bottom && contains(action->rectangle, second->rectangle) &&
            contains(container->rectangle, action->rectangle) &&
            row->rich_layout.components == ui_rect_height(container->rectangle),
            "component_container_fits_a_narrow_row_and_wraps_its_buttons");
    }
    GatewayMessageEvent update{}; update.id = 1; update.fields = gateway_message_field_components;
    update.components = {components, 5}; components[0].accent_color = 0;
    ok = ok && message_rich_store_apply(&f.rich, 1, f.record[0].revision, update).ok();
    const auto* stored = message_rich_store_find(f.rich, 1);
    check(ok && stored && message_rich_has_interaction(*stored) && stored->components[0].has_accent_color &&
          stored->components[0].accent_color == 0, "component_update_preserves_interaction_and_explicit_black");
    update = {}; update.id = 1; update.fields = gateway_message_field_interaction;
    ok = ok && message_rich_store_apply(&f.rich, 1, f.record[0].revision, update).ok();
    stored = message_rich_store_find(f.rich, 1);
    check(ok && stored && !message_rich_has_header(*stored) && stored->component_count == 5,
          "interaction_clear_does_not_clear_container");
    message_scene_release(&f.scene);
}

// An application-command reply with two secondary buttons: a Unicode emoji
// leads one label as text, a custom emoji leads the other as media. Both
// draw the translucent neutral chrome at the official size.
void test_message_component_buttons(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = fixture;
    reset_value(&f);
    const auto payload = utf8_literal(R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{
        "id":"1","channel_id":"7","author":{"id":"9","username":"BlueprintBot","bot":true},"content":"",
        "timestamp":"2026-09-05T10:00:00Z","type":20,
        "components":[{"type":1,"components":[
          {"type":2,"style":2,"label":"Zoom In","custom_id":"zoom","emoji":{"name":"🔎"}},
          {"type":2,"style":2,"label":"Download","custom_id":"download","emoji":{"id":"123","name":"blueprint"}}]}]}})");
    u32 structurals[1024]{}; char packed_text[8192]{}; u64 ids[100]{};
    GatewayEnvelope envelope{}; GatewayClientEvent routed{}; ConstSlice<u8> packed{};
    GatewayMessageComponent components[40]{};
    GatewayMessageRichScratch scratch{}; scratch.components = {components, 40};
    GatewayMessageEvent message{};
    bool ok = gateway_decode_envelope(payload, {structurals, 1024}, 32, &envelope).ok() &&
        gateway_client_route_dispatch(envelope, {structurals, 1024}, 32, {packed_text, sizeof(packed_text)},
            {ids, 100}, &routed, &packed).ok() &&
        gateway_client_event_as_message(routed, packed, scratch, &message) && message.components.count == 3 &&
        message_store_initialize(&f.store, {f.record, 1}, {f.order, 1}, {f.text, sizeof(f.text)}) &&
        message_reconcile_create(&f.store, message).ok() &&
        message_rich_store_initialize(&f.rich, {f.rich_record, 1}, {f.rich_order, 1}, {f.rich_text, sizeof(f.rich_text)}) &&
        message_rich_store_apply(&f.rich, 1, f.record[0].revision, message).ok() &&
        ui_variable_list_initialize(&f.list, {f.heights, 1}, {f.fenwick, 1}, 1, ui_extent_units(57)) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(1000), false) &&
        message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
        message_scene_initialize_owned(&f.scene, &f.timeline, 1, 1, {f.widget, widgets}, {f.node, widgets}) &&
        message_text_presentation_initialize(&f.presentation, {f.commands, 32}, {f.utf8, sizeof(f.utf8)},
            {f.styles, 128}, {f.semantics, 32}, {f.measurements, 1}, {f.spoilers, 1}, {f.byte_styles, 4096}) &&
        ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 64}, measure, nullptr) &&
        message_scene_update(&f.scene, {0, 0, 740, 1000}).ok();
    ChatStateStore state{}; NavigationView navigation{};
    const MessageTextCoordinatorRequest request{&f.scene, &f.presentation, &f.cache, &state, nullptr,
        &navigation, &kDefaultTheme, finish, nullptr, 1, 1};
    ok = ok && message_text_coordinator_converge(request).ok();
    const auto* row = ok ? message_scene_find_row(f.scene, 0) : nullptr;
    const auto* zoom = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 1) : nullptr;
    const auto* download = row ? message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::component, 2) : nullptr;
    // The fake measure gives 7 DIP per byte: "🔎 Zoom In" is 12 bytes.
    check(zoom && download && ui_rect_height(zoom->rectangle) == kMessageComponentButtonHeight &&
              ui_rect_width(zoom->rectangle) == 84.0f + 2 * kMessageComponentButtonPadding &&
              ui_rect_width(download->rectangle) ==
                  56.0f + 2 * kMessageComponentButtonPadding + kMessageComponentEmojiExtent + kMessageComponentEmojiGap &&
              download->rectangle.left == zoom->rectangle.right + kMessageComponentGap &&
              download->rectangle.top == zoom->rectangle.top,
          "component_buttons_hug_emoji_and_label_with_official_padding");
    const MessageTextCommand* zoom_label = nullptr;
    for (u32 i = 0; i < f.presentation.command_count; ++i)
        if (utf8_equal(f.commands[i].text, utf8_literal("🔎 Zoom In")))
            zoom_label = &f.commands[i];
    check(zoom_label && zoom_label->styles.count == 2 && zoom_label->styles[0].byte_count == 4 &&
              zoom_label->styles[0].size == text_size_md && zoom_label->styles[1].size == text_size_sm &&
              zoom_label->horizontal_alignment == UiTextAlignment::center,
          "component_button_label_is_reduced_after_a_body_size_unicode_emoji");
    MessageMediaCommand media_commands[8]{};
    MessageMediaCommandStream media{};
    MessagePresentationResult result{};
    ok = ok && paint_stream_initialize(&f.paint) && paint_stream_reset(&f.paint) &&
         message_media_command_stream_initialize(&media, {media_commands, 8}) &&
         message_presentation_build(f.scene, kDefaultTheme, &f.paint, &media, &result);
    u32 chrome = 0;
    for (u32 i = 0; ok && zoom && download && i < f.paint.count; ++i) {
        const auto& q = f.paint.quads[i];
        const UiRect r{q.left, q.top, q.right, q.bottom};
        if ((ui_rect_equal(r, zoom->rectangle) || ui_rect_equal(r, download->rectangle)) &&
            bytes_equal(&q.fill, &kDefaultTheme.tokens.fill.muted, sizeof(Color)) &&
            bytes_equal(&q.border, &kDefaultTheme.tokens.border.subtle, sizeof(Color)) &&
            q.radius == kDefaultTheme.scale.radius_md && q.border_width == 1.0f)
            ++chrome;
    }
    bool emoji = false;
    for (u32 i = 0; ok && download && i < media.count; ++i)
        emoji |= media_commands[i].kind == MessageMediaCommandKind::custom_emoji && media_commands[i].id == 123 &&
                 media_commands[i].rectangle.left == download->rectangle.left + kMessageComponentButtonPadding &&
                 ui_rect_width(media_commands[i].rectangle) == kMessageComponentEmojiExtent;
    check(ok && chrome == 2 && emoji, "component_buttons_draw_neutral_chrome_and_custom_emoji");
    // A failed click: the error line sits under the action row, not in the
    // message notice; the row grows by its height.
    const float row_height = row ? row->rich_layout.components : 0.0f;
    ok = ok && message_scene_set_interaction(&f.scene, {1, 1, false, true}) &&
         message_scene_update(&f.scene, {0, 0, 740, 1000}).ok() && message_text_coordinator_converge(request).ok();
    const auto* failed_row = ok ? message_scene_find_row(f.scene, 0) : nullptr;
    const auto* action = failed_row ? message_scene_rich_item_layout_node(f.scene, *failed_row,
                                                                          MessageSceneRichItemKind::component, 0)
                                    : nullptr;
    u32 error_lines = 0;
    bool under_row = false;
    for (u32 i = 0; action && i < f.presentation.command_count; ++i)
        if (utf8_equal(f.commands[i].text, utf8_literal("This interaction failed"))) {
            ++error_lines;
            under_row = f.commands[i].rectangle.bottom == action->rectangle.bottom - 4.0f &&
                        f.commands[i].rectangle.left ==
                            action->rectangle.left + kMessageComponentErrorIconExtent + kMessageComponentErrorIconGap;
        }
    check(ok && action && failed_row->rich_layout.components == row_height + kMessageComponentErrorHeight &&
              error_lines == 1 && under_row,
          "component_failure_shows_one_error_line_under_its_action_row");
    // Loading: dots replace the content at the scene's phase.
    ok = ok && message_scene_set_interaction(&f.scene, {1, 2, true, false}) &&
         message_scene_update(&f.scene, {0, 0, 740, 1000}).ok() && message_text_coordinator_converge(request).ok() &&
         paint_stream_reset(&f.paint) && message_media_command_stream_initialize(&media, {media_commands, 8}) &&
         message_presentation_build(f.scene, kDefaultTheme, &f.paint, &media, &result);
    const auto* loading = ok ? message_scene_rich_item_layout_node(f.scene, *message_scene_find_row(f.scene, 0),
                                                                   MessageSceneRichItemKind::component, 2)
                             : nullptr;
    u32 dots = 0, custom = 0;
    for (u32 i = 0; loading && i < f.paint.count; ++i) {
        const auto& q = f.paint.quads[i];
        dots += q.right - q.left <= kUiTypingDotSize + .01f && q.left >= loading->rectangle.left &&
                q.right <= loading->rectangle.right && q.top >= loading->rectangle.top &&
                q.bottom <= loading->rectangle.bottom;
    }
    for (u32 i = 0; i < media.count; ++i)
        custom += media_commands[i].kind == MessageMediaCommandKind::custom_emoji;
    check(ok && dots == kUiTypingDotCount && custom == 0, "component_loading_shows_dots_in_place_of_content");
    paint_stream_release(&f.paint);
    message_scene_release(&f.scene);
}
} // namespace maicord
