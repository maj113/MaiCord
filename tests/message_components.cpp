#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/navigation_view.hpp"

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
                header = utf8_equal(f.commands[i].text, utf8_literal("Display used /vote localized")) &&
                    f.commands[i].styles.count == 2 && f.commands[i].wrapping == UiTextWrapping::no_wrap;
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
} // namespace maicord
