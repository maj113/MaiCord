#include "maicord/ui/text.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/message_component_layout.hpp"

namespace maicord {
namespace {
constexpr u32 widgets = 8 * kMessageSceneWidgetsPerRow + 1;
struct Fixture {
    MessageRecord record[8];
    u32 order[8];
    u8 text[1024];
    MessageRichRecord rich_record[8];
    u16 rich_order[8];
    u8 rich_text[65536];
    u32 heights[8];
    u64 fenwick[8];
    UiWidget widget[widgets];
    UiLayoutNode node[widgets];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
    MessageTextCommand commands[256];
    char utf8[32768];
    TextStyleSpan styles[1024];
    MessageMarkdownSemantic semantics[256];
    MessageTextMeasurement measurements[8];
    MessageTextSpoilerReveal spoilers[8];
    u32 byte_styles[32768];
    MessageTextPresentation presentation;
    UiTextMeasureRecord cache_records[512];
    UiTextMeasureCache cache;
};
Fixture fixture;
TextSystem text_system;
wchar_t scratch_text[8192];
bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* output) noexcept {
    auto result = text_measure_utf8_styled(
        &text_system, request.text, request.styles, request.constraint.width, request.constraint.height,
        {scratch_text, 8192}, request.no_wrap ? TextWrapping::no_wrap : TextWrapping::wrap);
    *output = {{result.width_including_trailing_whitespace, result.height}, 0, result.line_count};
    return result.ok();
}
bool finish(void*, const Theme&) noexcept {
    return true;
}

} // namespace

void test_embed_convergence(void (*check)(bool, const char*) noexcept) noexcept {
    if (!text_system_initialize(&text_system, L"Segoe UI", 16, L"en-US").ok()) {
        check(false, "embed_native_text_system");
        return;
    }
    auto& f = fixture;
    const auto payload = utf8_literal(
        R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"1","channel_id":"7","author":{"id":"9","username":"Bot"},"content":"","timestamp":"2026-09-05T10:00:00Z","embeds":[{"type":"rich","title":"CALDERA <3","description":"Official **CALDERA** server","thumbnail":{"url":"https://example.com/thumb.png","width":128,"height":128},"fields":[{"name":"Owner","value":"Wren042","inline":true},{"name":"Server ID","value":"1111222233334444555","inline":true},{"name":"Member Count","value":"27","inline":true},{"name":"Visible Channels","value":"Text: 15\nVoice: 3","inline":true},{"name":"Roles","value":"10","inline":true},{"name":"Created At","value":"Thursday, May 25, 2023 at 11:20 AM"}],"footer":{"text":"Thanks for being a part of our server!"}}]}})");
    u32 structurals[1024]{};
    char packed_text[8192]{};
    u64 ids[100]{};
    GatewayEnvelope envelope{};
    GatewayClientEvent routed{};
    ConstSlice<u8> packed{};
    GatewayMessageComponent components[40]{};
    GatewayMessageEmbed embeds[1]{};
    GatewayMessageEmbedField fields[6]{};
    GatewayMessageRichScratch scratch{};
    scratch.components = {components, 40};
    scratch.embeds = {embeds, 1};
    scratch.embed_fields = {fields, 6};
    GatewayMessageEvent message{};
    bool ok =
        gateway_decode_envelope(payload, {structurals, 1024}, 32, &envelope).ok() &&
        gateway_client_route_dispatch(envelope, {structurals, 1024}, 32, {packed_text, sizeof(packed_text)},
                                      {ids, 100}, &routed, &packed)
            .ok() &&
        gateway_client_event_as_message(routed, packed, scratch, &message);
    ok = ok && message_store_initialize(&f.store, {f.record, 8}, {f.order, 8}, {f.text, sizeof(f.text)}) &&
         message_reconcile_create(&f.store, message).ok() &&
         message_rich_store_initialize(&f.rich, {f.rich_record, 8}, {f.rich_order, 8},
                                       {f.rich_text, sizeof(f.rich_text)}) &&
         message_rich_store_apply(&f.rich, 1, f.record[0].revision, message).ok() &&
         ui_variable_list_initialize(&f.list, {f.heights, 8}, {f.fenwick, 8}, 1, ui_extent_units(57)) &&
         message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57),
                                     ui_extent_units(1000), false) &&
         message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
         message_scene_initialize_owned(&f.scene, &f.timeline, 8, 8, {f.widget, widgets},
                                        {f.node, widgets}) &&
         message_text_presentation_initialize(&f.presentation, {f.commands, 256}, {f.utf8, sizeof(f.utf8)},
                                              {f.styles, 1024}, {f.semantics, 256}, {f.measurements, 8},
                                              {f.spoilers, 8}, {f.byte_styles, 32768}) &&
         ui_text_measure_cache_initialize(&f.cache, {f.cache_records, 512}, measure, nullptr);
    check(ok, "embed_convergence_fixture");
    if (!ok) {
        message_scene_release(&f.scene);
        text_system_release(&text_system);
        return;
    }
    ChatStateStore state{};
    NavigationView navigation{};
    // Real DirectWrite word wrapping: the old width feedback failed at 547.
    bool converged = true, steady = true;
    for (u32 width = 320; width < 900 && converged; ++width) {
        converged = message_scene_update(&f.scene, {0, 0, float(width), 1000}).ok();
        const MessageTextCoordinatorRequest request{
            &f.scene,       &f.presentation, &f.cache, &state, nullptr, &navigation,
            &kDefaultTheme, finish,          nullptr,  1,      1};
        const auto result = message_text_coordinator_converge(request);
        const auto* row = message_scene_find_row(f.scene, 0);
        converged = converged && result.ok() && result.finalized && row &&
                    row->rich_item_layout.embed_widths[0] > 0 && result.passes <= 5;
        if (converged) {
            const float chosen = row->rich_item_layout.embed_widths[0];
            const u32 height = f.heights[0];
            const auto repeated = message_text_coordinator_converge(request);
            row = message_scene_find_row(f.scene, 0);
            steady = steady && repeated.ok() && repeated.passes == 1 &&
                     row->rich_item_layout.embed_widths[0] == chosen && f.heights[0] == height;
        }
    }
    check(converged, "thumbnail_inline_embed_converges_across_580_pane_widths");
    check(steady, "embed_width_and_height_are_stable_after_wrapping");
    const MessageTextCoordinatorRequest request{
        &f.scene,       &f.presentation, &f.cache, &state, nullptr, &navigation,
        &kDefaultTheme, finish,          nullptr,  1,      1};
    bool restored =
        message_scene_update(&f.scene, {}).ok() && message_scene_update(&f.scene, {0, 0, 899, 240}).ok();
    restored = restored && message_text_coordinator_converge(request).ok();
    check(restored, "clipped_embed_rematerializes_with_retained_content_width");
    const GatewayMessageEmbed original_embed = embeds[0];
    // A new rich-content identity must choose a new width, not keep the old one.
    embeds[0].title = utf8_literal("Pong!");
    embeds[0].description = utf8_literal("32ms");
    embeds[0].thumbnail = {};
    embeds[0].field_count = 0;
    embeds[0].footer_text = {};
    message.embed_fields = {};
    bool changed = message_rich_store_apply(&f.rich, 1, f.record[0].revision, message).ok();
    changed = changed && message_text_coordinator_converge(request).ok();
    const auto* compact = message_scene_find_row(f.scene, 0);
    check(changed && compact && compact->rich_item_layout.embed_widths[0] > 0 &&
              compact->rich_item_layout.embed_widths[0] < 200,
          "edited_embed_recomputes_its_natural_width");
    embeds[0] = original_embed;
    message.embed_fields = {fields, 6};
    bool scrolling = true;
    for (u64 id = 2; id <= 8; ++id) {
        message.id = id;
        scrolling = scrolling && message_timeline_apply_created(&f.timeline, message).ok();
    }
    const float viewport_widths[]{547, 899, 320, 700, 546, 548};
    for (float width : viewport_widths) {
        scrolling = scrolling && message_scene_update(&f.scene, {0, 0, width, 240}).ok();
        bool moved = false;
        scrolling = scrolling && message_timeline_scroll_to_bottom(&f.timeline, &moved) &&
                    message_text_coordinator_converge(request).ok();
        scrolling = scrolling && message_timeline_scroll_to_start(&f.timeline, &moved) &&
                    message_text_coordinator_converge(request).ok();
    }
    check(scrolling, "embed_resize_converges_while_scrolling_rematerializes_rows");
    message_scene_release(&f.scene);
    text_system_release(&text_system);
}
} // namespace maicord
