#pragma once
#include "probe_present.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/ui/message_author.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace forwarded_messages_probe_detail {
struct Fixture {
    MessageRecord records[2]{};
    u32 order[2]{}, heights[2]{};
    u64 fenwick[2]{};
    u8 text[4096]{}, rich_text[16384]{};
    u16 rich_order[2]{};
    UiWidget widgets[2 * kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[2 * kMessageSceneWidgetsPerRow + 1]{};
    MessageStore store{};
    MessageRichStore rich{};
    StableStorage rich_storage{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene scene{};
    MessageTextPresentation presentation{};
    ~Fixture() noexcept { message_scene_release(&scene); message_rich_store_release(&rich); }
};
inline bool upload(D3D11Renderer& renderer, const MessageRichStore& store,
                   const MessageRichRecord& rich, const MessageSceneRowWidget& row) noexcept {
    // Original opaque color bands make the independent media owners visible.
    u8 pixels[32 * 16 * 4]{};
    for (u32 y = 0; y < 16; ++y) for (u32 x = 0; x < 32; ++x) {
        const u32 at = (y * 32 + x) * 4;
        pixels[at] = static_cast<u8>((rich.forwarded_snapshot_child ? 175 : 35) + x * 2);
        pixels[at + 1] = static_cast<u8>(65 + y * 8);
        pixels[at + 2] = static_cast<u8>(rich.forwarded_snapshot_child ? 80 : 190);
        pixels[at + 3] = 255;
    }
    for (u32 i = 0; i < rich.attachment_count; ++i) {
        const auto& attachment = rich.attachments[i];
        const auto format = message_rich_attachment_format(store, attachment);
        float width{}, height{};
        if (!message_rich_attachment_preview_extent(store, attachment, row.measured_width, &width, &height)) continue;
        const auto tier = renderer.preview_media_atlas
            ? media_resolution_tier_for_display(width, height, renderer.dpi, media_preview_extent_cap(format))
            : MediaResolutionTier::original;
        if (!renderer_upload_media_straight_rgba8(&renderer, media_asset_attachment(attachment.id, format, tier),
            32, 16, 128, {pixels, sizeof(pixels)}).ok()) return false;
    }
    for (u32 i = 0; i < rich.embed_count; ++i) {
        MessageRichEmbedMediaSelection selected{};
        if (!message_rich_embed_media_select(store, rich.embeds[i], &selected)) continue;
        const auto tier = renderer.preview_media_atlas
            ? media_resolution_tier_for_display(row.rich_item_layout.embed_media_widths[i],
                row.rich_item_layout.embed_media_heights[i], renderer.dpi, media_preview_extent_cap(selected.format))
            : MediaResolutionTier::original;
        const auto key = media_asset_embed(rich.message_id, rich.message_revision, static_cast<u8>(i),
            selected.resource, selected.format, selected.animated, tier, rich.forwarded_snapshot_child);
        if (!renderer_upload_media_straight_rgba8(&renderer, key, 32, 16, 128,
            {pixels, sizeof(pixels)}).ok()) return false;
    }
    return true;
}
} // namespace forwarded_messages_probe_detail

inline bool forwarded_messages_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure,
    bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    if (!renderer.message_scene || !renderer.message_text || !renderer.chat_state ||
        !renderer.navigation_view || !renderer.ui_scene || !renderer.dpi) return false;
    using namespace forwarded_messages_probe_detail;
    Fixture fixture{};
    const u32 original_width = renderer.width, original_height = renderer.height, original_dpi = renderer.dpi;
    const auto* original_scene = renderer.message_scene;
    const auto* original_text = renderer.message_text;
    struct Restore {
        D3D11Renderer& renderer; const MessageScene* scene; const MessageTextPresentation* text;
        ~Restore() noexcept {
            renderer.message_scene = scene; renderer.message_text = text;
            renderer.retained_presentation.valid = false;
            content_text_retention_invalidate(&renderer.content_text_retention);
        }
    } restore{renderer, original_scene, original_text};
    u32 stage{};
    const auto require = [&](bool valid, u32 next) noexcept {
        if (!valid && !stage) { stage = next; log_u64(LogLevel::error, "probe", "forwarded_native_failure_stage", next); }
        return valid;
    };
    constexpr u64 id = 0x46574401;
    const u64 channel = renderer.navigation_view->selected_channel_id ? renderer.navigation_view->selected_channel_id : 7;
    if (!require(message_store_initialize(&fixture.store, {fixture.records, 2}, {fixture.order, 2},
            {fixture.text, sizeof(fixture.text)}) &&
        message_store_upsert(&fixture.store, {id, channel, 123, 1'725'021'300'000ull, 0,
            utf8_literal("**Current note:** [outer guide](https://outer.example/guide). This text belongs to the sender."), 0}).ok() &&
        message_rich_store_initialize_owned_sparse(&fixture.rich, &fixture.rich_storage, 2,
            {fixture.rich_order, 2}, {fixture.rich_text, sizeof(fixture.rich_text)}) &&
        ui_variable_list_initialize(&fixture.list, {fixture.heights, 2}, {fixture.fenwick, 2}, 1, ui_extent_units(80)) &&
        message_timeline_initialize(&fixture.timeline, &fixture.store, &fixture.list,
            ui_extent_units(80), ui_extent_units(900), false) &&
        message_timeline_bind_rich_store(&fixture.timeline, &fixture.rich) &&
        message_scene_initialize_owned(&fixture.scene, &fixture.timeline, 2, 1,
            {fixture.widgets, 2 * kMessageSceneWidgetsPerRow + 1},
            {fixture.nodes, 2 * kMessageSceneWidgetsPerRow + 1}), 1)) return false;
    fixture.presentation = *original_text;
    fixture.presentation.active = false;
    fixture.presentation.projection_signature = 0;
    fixture.presentation.command_count = fixture.presentation.measurement_count = 0;
    GatewayMessageAttachment outer_attachment{}, child_attachments[2]{};
    outer_attachment.id = 0x46574402;
    outer_attachment.filename = utf8_literal("outer-original-bands.png");
    outer_attachment.content_type = utf8_literal("image/png");
    outer_attachment.url = utf8_literal("https://cdn.discordapp.com/attachments/7/1180124162/outer.png");
    outer_attachment.width = 144; outer_attachment.height = 72;
    outer_attachment.has_width = outer_attachment.has_height = true;
    child_attachments[0] = outer_attachment;
    child_attachments[0].id = 0x46574403;
    child_attachments[0].filename = utf8_literal("snapshot-original-bands.png");
    child_attachments[0].url = utf8_literal("https://cdn.discordapp.com/attachments/8/1180124163/snapshot.png");
    child_attachments[1].id = 0x46574404;
    child_attachments[1].filename = utf8_literal("snapshot-origin-notes.pdf");
    child_attachments[1].content_type = utf8_literal("application/pdf");
    child_attachments[1].url = utf8_literal("https://cdn.discordapp.com/attachments/8/1180124164/notes.pdf");
    GatewayMessageEmbed outer_embed{}, child_embed{};
    outer_embed.type = child_embed.type = GatewayMessageEmbedType::rich;
    outer_embed.title = utf8_literal("Outer source card");
    outer_embed.description = utf8_literal("[Current card link](https://outer.example/card)");
    outer_embed.image.url = utf8_literal("https://cdn.discordapp.com/attachments/7/1180124165/card.png");
    outer_embed.image.width = 96; outer_embed.image.height = 48;
    outer_embed.image.has_width = outer_embed.image.has_height = true;
    child_embed = outer_embed;
    child_embed.title = utf8_literal("Snapshot source card");
    child_embed.description = utf8_literal("[Original card link](https://snapshot.example/card)");
    child_embed.image.url = utf8_literal("https://cdn.discordapp.com/attachments/8/1180124166/card.png");
    GatewayMessageEvent event{};
    event.id = id;
    event.fields = gateway_message_field_attachments | gateway_message_field_embeds;
    event.attachments = {&outer_attachment, 1}; event.embeds = {&outer_embed, 1};
    if (!require(message_rich_store_apply(&fixture.rich, id, fixture.records[0].revision, event).ok(), 2)) return false;
    const u32 ordinary_extent = fixture.rich.record_extent;
    const u32 ordinary_rows = fixture.scene.owned_rows.capacity;
    const u32 ordinary_metrics = fixture.scene.owned_measurements.capacity;
    Theme theme = kDefaultTheme;
    const auto project = [&]() noexcept {
        const UiRect viewport = ui_scene_content_rectangle(*renderer.ui_scene, UiSemantic::message_view);
        if (!message_scene_update(&fixture.scene, viewport).ok()) return false;
        renderer.message_scene = &fixture.scene; renderer.message_text = &fixture.presentation;
        const auto result = message_text_coordinator_converge({&fixture.scene, &fixture.presentation, &measure,
            renderer.chat_state, renderer.user_store, renderer.navigation_view, &theme,
            +[](void* owner, const Theme& current) noexcept {
                return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner), current);
            }, &renderer, 1'725'021'300ull, static_cast<u16>(renderer.dpi), nullptr});
        if (!result.ok()) {
            log_u64(LogLevel::error, "probe", "forwarded_coordinator_error", static_cast<u32>(result.error));
            log_u64(LogLevel::error, "probe", "forwarded_coordinator_passes", result.passes);
        }
        return result.ok();
    };
    const auto show = [&](const wchar_t* name) noexcept {
        if (!project() || !fixture.scene.count) return false;
        const auto* outer = message_rich_store_find(fixture.rich, id);
        const auto* child = message_rich_store_snapshot(fixture.rich, id);
        const auto* child_row = message_scene_snapshot_row(fixture.scene, fixture.scene.rows[0]);
        if (!outer || !upload(renderer, fixture.rich, *outer, fixture.scene.rows[0]) ||
            (child && (!child_row || !upload(renderer, fixture.rich, *child, *child_row)))) return false;
        return probe_present(&renderer, theme) && (!capture || capture(name, theme));
    };
    if (!require(show(L"forwarded-ordinary.bmp") && fixture.rich.record_extent == ordinary_extent &&
        fixture.scene.owned_rows.capacity == ordinary_rows &&
        fixture.scene.owned_measurements.capacity == ordinary_metrics, 3)) return false;
    // Header paint and profile hit testing must use the existing native cache
    // even when the member pane is absent. The old byte-count estimate clipped
    // the real "Mai" fixture to "M..." despite hundreds of available DIPs.
    const MessageTextCommand* author_command = nullptr;
    const auto& author_row = fixture.scene.rows[0];
    for (u32 i = 0; i < fixture.presentation.command_count; ++i) {
        const auto& command = fixture.presentation.commands[i];
        if (command.owner.handle.slot == author_row.author.slot &&
            command.owner.handle.generation == author_row.author.generation) author_command = &command;
    }
    MessageAuthorLayout author_header{};
    if (!require(!renderer.member_list_scene && renderer.message_measure_cache == &measure && author_command &&
        message_author_layout(fixture.scene, *author_command, *renderer.chat_state, *renderer.user_store,
                              renderer.message_measure_cache, theme, &author_header), 13)) return false;
    const auto* sender = user_store_find(*renderer.user_store, fixture.records[0].author_id);
    const auto sender_name = sender ? user_store_name(*renderer.user_store, *sender) : Utf8View{};
    const TextStyleSpan sender_style{0, sender_name.bytes, text_style_bold};
    UiTextMeasureValue native_sender{};
    if (!require(sender_name.bytes && author_header.name.text.bytes == sender_name.bytes &&
        renderer_text_measure_backend(&renderer,
            {{sender->id, hash64_counted_bytes(kHash64Seed, sender_name.data, sender_name.bytes), 0, 0x4657, 0},
             sender_name, {&sender_style, 1}, {ui_rect_width(author_command->rectangle), 64}, true,
             author_header.name.face}, &native_sender) &&
        ui_rect_width(author_header.name.rectangle) >= native_sender.size.width, 13)) return false;
    const auto name_hit = message_author_hit_test(fixture.scene, fixture.presentation, *renderer.chat_state,
        *renderer.user_store, renderer.message_measure_cache, theme,
        author_header.name.rectangle.right - 1, (author_header.name.rectangle.top + author_header.name.rectangle.bottom) * .5f);
    const auto timestamp_hit = message_author_hit_test(fixture.scene, fixture.presentation, *renderer.chat_state,
        *renderer.user_store, renderer.message_measure_cache, theme,
        author_header.timestamp.rectangle.left + 1, (author_header.name.rectangle.top + author_header.name.rectangle.bottom) * .5f);
    const u32 warm_author_calls = measure.backend_calls;
    for (u32 i = 0; i < 32; ++i)
        if (!message_author_layout(fixture.scene, *author_command, *renderer.chat_state, *renderer.user_store,
                                   renderer.message_measure_cache, theme, &author_header)) return false;
    if (!require(name_hit.user_id == sender->id && !timestamp_hit.user_id &&
                 measure.backend_calls == warm_author_calls, 13)) return false;
    log_write(LogLevel::info, "probe", "forwarded_sender_header_full_native_width_no_member_pane_zero_warm_backend_calls");
    event.fields |= gateway_message_field_message_snapshots | gateway_message_field_message_reference;
    event.reference.type = 1; event.reference.channel_id = 8; event.reference.message_id = 90;
    event.snapshot.present = true;
    event.snapshot.content = utf8_literal("**Frozen original:** [snapshot guide](https://snapshot.example/guide) and ||a private spoiler||. No original author is invented.");
    event.snapshot.fields = gateway_message_field_content | gateway_message_field_attachments | gateway_message_field_embeds;
    event.snapshot.attachments = {child_attachments, 2}; event.snapshot.embeds = {&child_embed, 1};
    if (!require(message_rich_store_apply(&fixture.rich, id, fixture.records[0].revision, event).ok() &&
        show(L"forwarded-coexistence.bmp") && fixture.rich.record_extent > ordinary_extent &&
        fixture.scene.owned_rows.capacity > ordinary_rows &&
        fixture.scene.owned_measurements.capacity > ordinary_metrics, 4)) return false;
    const auto* child_geometry = message_scene_snapshot_row(fixture.scene, fixture.scene.rows[0]);
    const auto* child_frame = child_geometry ? message_scene_layout_node(fixture.scene, child_geometry->widget) : nullptr;
    const auto* outer_media = message_scene_rich_item_layout_node(fixture.scene, fixture.scene.rows[0],
        MessageSceneRichItemKind::attachment, 0);
    if (!require(child_frame && outer_media &&
        child_frame->spec.style.margin.bottom == kDefaultMessageRichBlockGapDips &&
        outer_media->rectangle.top >= child_frame->rectangle.bottom + kDefaultMessageRichBlockGapDips,
        12)) return false;
    log_u64(LogLevel::info, "probe", "forwarded_rich_live_extent", fixture.rich.record_extent);
    log_u64(LogLevel::info, "probe", "forwarded_scene_admitted_slots", fixture.scene.owned_rows.capacity);
    log_u64(LogLevel::info, "probe", "forwarded_measurement_admitted_slots", fixture.scene.owned_measurements.capacity);
    MessageTextHit snapshot_link{};
    for (u32 i = 0; i < fixture.presentation.command_count; ++i) {
        const auto& command = fixture.presentation.commands[i];
        if (command.source_id != 0xC001) continue;
        for (u32 s = 0; s < command.semantics.count; ++s) {
            const auto& semantic = command.semantics[s];
            if (semantic.kind == MessageMarkdownSemanticKind::link)
                snapshot_link = {id, semantic.target, fixture.records[0].revision, semantic.source_offset,
                                 semantic.source_bytes, semantic.kind, true};
        }
    }
    if (!require(snapshot_link.valid && message_text_presentation_hit_source(fixture.store, &fixture.rich,
        snapshot_link).bytes != 0 && renderer.media_quad_count >= 4, 5)) return false;
    const u32 layout = fixture.scene.layout_passes, rebuilds = fixture.scene.rebuilds,
        prepare = fixture.scene.preparation_passes, shape = measure.backend_calls,
        text_builds = renderer.content_text->rebuilds, raster = renderer.glyph_text.cache_builds,
        paint_uploads = renderer.paint_instance_uploads, text_uploads = renderer.text_instance_uploads,
        media_uploads = renderer.media_instance_uploads, draws = renderer.painted_frames,
        skips = renderer.unchanged_present_skips;
    for (u32 frame = 0; frame < 64; ++frame)
        if (!message_scene_update(&fixture.scene, fixture.scene.viewport).ok() ||
            !renderer_rebuild_content_text(&renderer, theme) || !probe_present(&renderer, theme)) return false;
    if (!require(fixture.scene.layout_passes == layout && fixture.scene.rebuilds == rebuilds &&
        fixture.scene.preparation_passes == prepare && measure.backend_calls == shape &&
        renderer.content_text->rebuilds == text_builds && renderer.glyph_text.cache_builds == raster &&
        renderer.paint_instance_uploads == paint_uploads && renderer.text_instance_uploads == text_uploads &&
        renderer.media_instance_uploads == media_uploads && renderer.painted_frames == draws &&
        renderer.unchanged_present_skips - skips == 64, 6)) return false;
    log_write(LogLevel::info, "probe", "forwarded_64_zero_prepare_layout_shape_raster_upload_draw_passed");
    const float outside = renderer.width * 96.f / renderer.dpi - ui_rect_width(fixture.scene.viewport);
    if (!require(probe_resize_surface(static_cast<u32>((outside + 360.f) * renderer.dpi / 96.f),
        renderer.height) && show(L"forwarded-narrow.bmp"), 7)) return false;
    renderer_set_dpi(&renderer, 144);
    if (!require(probe_resize_surface(1800, 1500) && show(L"forwarded-dpi144.bmp"), 8)) return false;
    theme = kLightTheme;
    if (!require(show(L"forwarded-light.bmp"), 9)) return false;
    GatewayMessageEvent clear{}; clear.id = id; clear.fields = gateway_message_field_message_snapshots;
    if (!require(message_rich_store_apply(&fixture.rich, id, fixture.records[0].revision, clear).ok() &&
        message_text_presentation_hit_source(fixture.store, &fixture.rich, snapshot_link).bytes == 0 &&
        show(L"forwarded-child-removed.bmp") && fixture.scene.snapshot_count == 0 &&
        !message_scene_snapshot_row(fixture.scene, fixture.scene.rows[0]), 10)) return false;
    renderer_set_dpi(&renderer, original_dpi);
    if (!require(probe_resize_surface(original_width, original_height), 11)) return false;
    log_write(LogLevel::info, "probe", "forwarded_coexistence_origin_scope_narrow_light_dpi_removal_passed");
    return true;
}
} // namespace maicord
