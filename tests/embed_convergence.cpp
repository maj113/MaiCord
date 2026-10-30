#include "maicord/ui/text.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/text_flow_layout.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/core/memory.hpp"

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
    auto result = text_measure_utf8(
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
    const float intrinsic_height = 550.0f * 457.0f / 1003.0f + 6.0f;
    const float origins[]{0.0f, 98.015625f, 401.984375f, 907.984375f, -201.015625f};
    bool translated_extent = true;
    for (float top : origins) {
        UiTextFlowLayout flow{};
        const UiRect bounds{0, top, 550, top + 400};
        translated_extent = translated_extent && ui_text_flow_initialize(&flow, bounds, bounds) &&
                            ui_text_flow_advance(&flow, intrinsic_height) &&
                            ui_text_flow_required_height(flow) == intrinsic_height;
    }
    check(translated_extent, "media_flow_measurement_is_independent_of_viewport_origin");
    if (!text_system_initialize(&text_system, L"Segoe UI", 16).ok()) {
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
    // Real DirectWrite word wrapping: the old width feedback failed at 547, so
    // 545..556 is swept pixel by pixel and the rest of 320..898 every 9 px.
    bool converged = true, steady = true;
    for (u32 width = 320; width < 900 && converged; width += width >= 540 && width < 556 ? 1u : 9u) {
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
    check(converged, "thumbnail_inline_embed_converges_across_pane_widths");
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
    GatewayMessageAttachment attachment{};
    attachment.id = 91;
    attachment.filename = utf8_literal("preview.png");
    attachment.content_type = utf8_literal("image/png");
    attachment.url = utf8_literal("https://example.com/preview.png");
    attachment.width = 1003;
    attachment.height = 457;
    attachment.has_width = attachment.has_height = true;
    GatewayMessageReaction reaction{};
    reaction.emoji_id = 92;
    reaction.name = utf8_literal("sample");
    reaction.count = reaction.normal_count = 3;
    message.embeds = {};
    message.embed_fields = {};
    message.attachments = {&attachment, 1};
    message.reactions = {&reaction, 1};
    message.fields |= gateway_message_field_attachments | gateway_message_field_reactions |
                      gateway_message_field_referenced_message;
    message.reference.state = GatewayMessageReferenceState::available;
    message.reference.message_id = 90;
    message.reference.author_name = utf8_literal("Sample author");
    message.reference.content = utf8_literal("An image reply");
    bool image_scrolling = true;
    for (u64 id = 1; id <= 8; ++id) {
        message.id = id;
        const auto& previous = f.record[f.order[id - 1]];
        const MessageInput body{id, previous.channel_id, previous.author_id, previous.timestamp_ms,
                                0, utf8_literal("A sample body above the attachment.")};
        image_scrolling = image_scrolling && message_store_upsert(&f.store, body).ok();
        image_scrolling = image_scrolling && message_rich_store_apply(
            &f.rich, id, f.record[f.order[id - 1]].revision, message).ok();
    }
    bool separated = true;
    bool image_steady = true;
    UiResolvedThemeStyles theme_styles{};
    for (u32 step = 0; step < 8 && image_scrolling; ++step) {
        if (step == 2 || step == 4) {
            for (u64 id = 1; id <= 8; ++id) {
                bool dimensions_changed = false;
                image_scrolling = image_scrolling && message_rich_store_set_attachment_dimensions(
                    &f.rich, id, f.record[f.order[id - 1]].revision, attachment.id, attachment.width,
                    step == 2 ? 617 : attachment.height, &dimensions_changed) && dimensions_changed;
            }
        }
        if (step == 6) {
            UiThemeMetrics metrics{};
            metrics.message_left = 22.25f;
            metrics.message_rich_gap = 9.5f;
            image_scrolling = image_scrolling &&
                              ui_theme_styles_apply(&theme_styles, {}, metrics).ok() &&
                              message_scene_bind_theme_styles(&f.scene, &theme_styles);
        }
        const float left = 351.0f + static_cast<float>(step) * 27.0f;
        image_scrolling = image_scrolling && message_scene_update(&f.scene, {left, 98, 979, 908}).ok();
        bool moved = false;
        image_scrolling = image_scrolling && message_timeline_scroll_to_start(&f.timeline, &moved) &&
                          message_timeline_scroll_by(&f.timeline, 60'000 + step * 31, 0, &moved) &&
                          message_text_coordinator_converge(request).ok();
        if (image_scrolling) {
            const auto repeated = message_text_coordinator_converge(request);
            image_steady = image_steady && repeated.ok() && repeated.passes == 1;
        }
        for (u32 index = 0; index < f.presentation.command_count && image_scrolling; ++index) {
            const auto& command = f.commands[index];
            if (command.measurement_kind != MessageTextMeasurementKind::content &&
                command.measurement_kind != MessageTextMeasurementKind::reply)
                continue;
            const auto* row = message_scene_find_row(f.scene,
                f.measurements[command.measurement_index].source_index);
            const auto* media = row ? message_scene_rich_item_layout_node(
                f.scene, *row, MessageSceneRichItemKind::attachment, 0) : nullptr;
            separated = separated && media && command.rectangle.top + command.measured_height <=
                                                   media->rectangle.top;
        }
    }
    check(image_scrolling, "image_reply_reaction_text_converges_at_fractional_scroll_origins");
    check(image_scrolling && separated, "image_reply_body_text_stays_above_the_attachment");
    check(image_scrolling && image_steady, "image_metrics_stay_stable_after_resize_theme_and_metadata_changes");
    // A scroll step over unchanged rows moves the projection instead of
    // rebuilding it, and the moved commands equal a rebuild at that position.
    MessageTextCoordinatorRequest step = request;
    step.scroll_refresh = step.scroll_moving = true;
    const auto close_to = [](float a, float b) noexcept { return a - b < 0.001f && b - a < 0.001f; };
    bool moved = false;
    bool shifted = image_scrolling && message_timeline_scroll_to_start(&f.timeline, &moved) &&
                   message_timeline_scroll_by(&f.timeline, 60'000, 0, &moved) &&
                   message_text_coordinator_converge(request).ok();
    const u32 builds = f.presentation.builds, rows = f.scene.rebuilds;
    shifted = shifted && message_scene_scroll_by(&f.scene, 1'237, 0, &moved) && moved;
    const auto stepped = message_text_coordinator_converge(step);
    shifted = shifted && stepped.ok() && stepped.finalized && stepped.passes == 1 &&
              f.presentation.builds == builds && f.scene.rebuilds == rows && f.presentation.scroll_moving &&
              close_to(f.presentation.scroll_shift_dips, -ui_signed_extent_dips(1'237));
    static MessageTextCommand moved_commands[256];
    const u32 moved_count = f.presentation.command_count;
    for (u32 i = 0; i < moved_count; ++i)
        moved_commands[i] = f.commands[i];
    f.presentation.shift_layout_passes = 0;
    shifted = shifted && message_text_coordinator_converge(request).ok() && f.presentation.builds != builds &&
              f.presentation.command_count == moved_count;
    const auto same = [&](const UiRect& a, const UiRect& b) noexcept {
        return close_to(a.left, b.left) && close_to(a.top, b.top) && close_to(a.right, b.right) && close_to(a.bottom, b.bottom);
    };
    for (u32 i = 0; shifted && i < moved_count; ++i) {
        const auto& a = moved_commands[i];
        const auto& b = f.commands[i];
        shifted = a.text.bytes == b.text.bytes && bytes_equal(a.text.data, b.text.data, a.text.bytes) &&
                  same(a.rectangle, b.rectangle) && same(a.clip, b.clip);
    }
    check(shifted, "scroll_step_moves_the_projection_exactly_as_a_rebuild_places_it");
    message_scene_release(&f.scene);
    text_system_release(&text_system);
}
} // namespace maicord
