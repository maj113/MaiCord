#pragma once
#include "probe_present.hpp"
#include "ui_surface_motion_measurement.hpp"
#include "search_filter_ui_probe.hpp"
#include "maicord/core/log.hpp"
#include "maicord/protocol/message_search.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/media/presentation_policy.hpp"

namespace maicord {
namespace rich_search_probe_detail {
inline constexpr u32 kRows = 4;
// The bootstrap main timeline uses this same ID and initial revision. Search
// carries different content, exercising real owner routing despite collisions.
inline constexpr u64 kMixedId = 1'270'000'000'000'000'000ull;
// The rich decoder reuses these pools for each result. There is one descriptor
// scratch bank, not one worker/descriptor tree for every card.
struct DecodeScratch {
    u32 page_structurals[2048]{},message_structurals[1024]{};
    char text[16384]{};
    u64 mention_roles[100]{},member_roles[kGatewayMessageMemberRoleMaximumIds]{};
    GatewayMessageNamedUser mention_users[kGatewayMessageMentionUserMaximum]{};
    GatewayMessageReaction reactions[kGatewayMessageReactionMaximum]{};
    GatewayMessageAttachment attachments[kGatewayMessageAttachmentMaximum]{},snapshot_attachments[kGatewayMessageAttachmentMaximum]{};
    GatewayMessageEmbed embeds[kGatewayMessageEmbedMaximum]{},snapshot_embeds[kGatewayMessageEmbedMaximum]{};
    GatewayMessageEmbedField fields[kGatewayMessageEmbedFieldMaximum]{},snapshot_fields[kGatewayMessageEmbedFieldMaximum]{};
    GatewayMessageSticker stickers[kGatewayMessageStickerMaximum]{},snapshot_stickers[kGatewayMessageStickerMaximum]{};
    GatewayMessageComponent components[kGatewayMessageComponentMaximum]{},snapshot_components[kGatewayMessageComponentMaximum]{};
    GatewayMessageNamedUser snapshot_mentions[kGatewayMessageMentionUserMaximum]{};
    MessageSearchRichScratch view() noexcept {
        return {{mention_roles,100},{member_roles,kGatewayMessageMemberRoleMaximumIds},{mention_users,kGatewayMessageMentionUserMaximum},
            {{reactions,kGatewayMessageReactionMaximum},{attachments,kGatewayMessageAttachmentMaximum},
             {embeds,kGatewayMessageEmbedMaximum},{fields,kGatewayMessageEmbedFieldMaximum},
             {stickers,kGatewayMessageStickerMaximum},{components,kGatewayMessageComponentMaximum},
             {{snapshot_attachments,kGatewayMessageAttachmentMaximum},{snapshot_embeds,kGatewayMessageEmbedMaximum},
              {snapshot_fields,kGatewayMessageEmbedFieldMaximum},{snapshot_stickers,kGatewayMessageStickerMaximum},
              {snapshot_components,kGatewayMessageComponentMaximum},{snapshot_mentions,kGatewayMessageMentionUserMaximum}}}};
    }
};
struct Fixture {
    MessageStore store{},published{};
    MessageRecord records[kRows]{}; u32 order[kRows]{},heights[kRows]{}; MessageHandle result_order[kRows]{};
    u64 fenwick[kRows]{}; u8 message_bytes[16384]{},rich_bytes[32768]{}; u16 rich_order[kRows]{};
    MessageRichStore rich{}; StableStorage rich_storage{}; UiVariableList list{}; MessageTimeline timeline{};
    UserStore users{}; UserRecord user_records[2]{}; u32 user_order[2]{}; u8 user_names[1024]{};
    MessageSearchGroup groups[kRows]{}; MessageSearchMessage summaries[kRows]{};
    MessageSearchPage page{}; DecodeScratch decode{};
    MessageTextPresentation presentation{}; MessageTextCommand commands[192]{};
    char text_bytes[16384]{}; TextStyleSpan styles[256]{}; MessageMarkdownSemantic semantics[128]{};
    MessageTextMeasurement measurements[16]{}; MessageTextSpoilerReveal reveals[16]{}; u32 markdown_scratch[16384]{};
    UiSearchScene scene{}; UiSearchSource source{}; UiSearchContent content{};
    u32 calls{},accepted{},publication_revision{1}; bool current{true};
    ~Fixture() noexcept { ui_search_scene_release(&scene); message_rich_store_release(&rich); }
};
inline bool current(void* context) noexcept { return static_cast<Fixture*>(context)->current; }
inline bool resolve(void* context,u32 index,UiSearchRow* row) noexcept {
    auto& f = *static_cast<Fixture*>(context); ++f.calls;
    if (index >= f.source.count) return false;
    const auto handle = f.result_order[index];
    const auto* message = message_store_resolve(f.published,handle);
    if (!row || !message) return false;
    const auto* author = user_store_find(f.users,message->author_id);
    *row = {message->channel_id,message->id,
        author ? user_store_name(f.users,*author) : utf8_literal("Unknown author"),
        utf8_literal("#general"),message_store_content(f.published,handle),utf8_literal("10/05 12:30"),
        true,true,0,message->author_id,author ? user_store_avatar_hash(*author) : Utf8View{}};
    return true;
}
inline bool accept(void* context,u32 index,const MessageSearchMessage& result) noexcept {
    auto& f = *static_cast<Fixture*>(context); const auto& message = result.message;
    if (index >= kRows || index != f.accepted) return false;
    const auto inserted = message_store_upsert(&f.store,{message.id,message.channel_id,message.author_id,
        message.timestamp_ms,message.edited_timestamp_ms,message.content,message.flags});
    if (!inserted.ok()) {
        log_u64(LogLevel::error,"probe","rich_search_sink_message_store_error",static_cast<u64>(inserted.error));
        return false;
    }
    const auto rich = message_rich_store_apply(&f.rich,message.id,f.records[inserted.handle.slot].revision,message);
    if (!rich.ok()) {
        log_u64(LogLevel::error,"probe","rich_search_sink_rich_store_error",static_cast<u64>(rich.error));
        return false;
    }
    const auto author = user_store_upsert(&f.users,message.author_id,message.author_name);
    if (!author.ok()) {
        log_u64(LogLevel::error,"probe","rich_search_sink_user_store_error",static_cast<u64>(author.error));
        return false;
    }
    if (message.author_avatar_hash.bytes && !user_store_set_avatar(&f.users,message.author_id,message.author_avatar_hash)) return false;
    f.result_order[index] = inserted.handle; ++f.accepted; return true;
}
inline bool initialize(Fixture& f) noexcept {
    if (!message_store_initialize(&f.store,{f.records,kRows},{f.order,kRows},{f.message_bytes,sizeof(f.message_bytes)}) ||
        !message_rich_store_initialize_owned_sparse(&f.rich,&f.rich_storage,kRows,{f.rich_order,kRows},{f.rich_bytes,sizeof(f.rich_bytes)}) ||
        !user_store_initialize(&f.users,{f.user_records,2},{f.user_order,2},{f.user_names,sizeof(f.user_names)}) ||
        !message_text_presentation_initialize(&f.presentation,{f.commands,192},{f.text_bytes,sizeof(f.text_bytes)},
            {f.styles,256},{f.semantics,128},{f.measurements,16},{f.reveals,16},{f.markdown_scratch,16384})) return false;
    constexpr auto body = utf8_literal(R"({"total_results":29,"messages":[
        [{"id":"1270000000000000000","channel_id":"111","guild_id":"100","hit":true,"author":{"id":"123","username":"Aster"},"timestamp":"2026-10-05T12:30:00Z","content":"**Current note:** [review guide](https://original.example/review) and ||[private draft](https://original.example/private)||.","attachments":[{"id":"9100","filename":"outer-review-notes.pdf","content_type":"application/pdf","size":131072,"url":"https://cdn.discordapp.com/attachments/111/9100/outer-review-notes.pdf"}],"message_reference":{"type":1,"message_id":"8001","channel_id":"112"},"message_snapshots":[{"message":{"content":"**Frozen original:** [snapshot guide](https://original.example/snapshot). No original author is invented.","timestamp":"2026-10-04T12:30:00Z","attachments":[{"id":"9201","filename":"snapshot-original-bands.png","content_type":"image/png","size":8192,"url":"https://cdn.discordapp.com/attachments/112/9201/snapshot-original-bands.png","width":128,"height":64}]}}]}],
        [{"id":"9002","channel_id":"111","guild_id":"100","hit":true,"author":{"id":"124","username":"Mira"},"timestamp":"2026-10-05T12:31:00Z","content":"","attachments":[{"id":"9101","filename":"original-interface-bands.png","content_type":"image/png","size":16384,"url":"https://cdn.discordapp.com/attachments/111/9101/original-interface-bands.png","width":128,"height":64}]}],
        [{"id":"9003","channel_id":"111","guild_id":"100","hit":true,"author":{"id":"123","username":"Aster"},"timestamp":"2026-10-05T12:32:00Z","content":"","attachments":[{"id":"9102","filename":"native-layout-checklist.pdf","content_type":"application/pdf","size":65536,"url":"https://cdn.discordapp.com/attachments/111/9102/native-layout-checklist.pdf"}]}],
        [{"id":"9004","channel_id":"111","guild_id":"100","hit":true,"author":{"id":"124","username":"Mira"},"timestamp":"2026-10-05T12:33:00Z","content":"","attachments":[{"id":"9103","filename":"original-motion-study.mp4","content_type":"video/mp4","size":262144,"url":"https://cdn.discordapp.com/attachments/111/9103/original-motion-study.mp4","width":128,"height":72}]}]
    ]})");
    MessageSearchQuery query{}; query.scope = MessageSearchScope::guild; query.scope_id = 100; query.limit = 25;
    // The production decoder admits a real nonempty search, not an unfiltered
    // request. Every synthetic result owns an outer attachment.
    query.has = search_has_file;
    const auto decoded = decode_message_search_page(body,query,{f.decode.page_structurals,2048},
        {f.decode.message_structurals,1024},64,{f.decode.text,sizeof(f.decode.text)},
        {f.groups,kRows},{f.summaries,kRows},f.decode.view(),{&f,accept});
    if (!decoded.ok()) {
        log_u64(LogLevel::error,"probe","rich_search_decode_error",static_cast<u64>(decoded.error));
        log_u64(LogLevel::error,"probe","rich_search_decode_message_error",static_cast<u64>(decoded.message_error));
        log_u64(LogLevel::error,"probe","rich_search_decode_error_offset",decoded.error_offset);
        log_u64(LogLevel::error,"probe","rich_search_decode_accepted_rows",f.accepted);
        return false;
    }
    if (f.accepted != kRows || !message_rich_store_snapshot(f.rich,kMixedId)) {
        log_u64(LogLevel::error,"probe","rich_search_initialize_accepted_rows",f.accepted);
        return false;
    }
    f.page = decoded.page; f.published = f.store;
    if (!ui_variable_list_initialize(&f.list,{f.heights,kRows},{f.fenwick,kRows},kRows,ui_extent_units(80)) ||
        !message_timeline_initialize(&f.timeline,&f.published,&f.list,ui_extent_units(80),ui_extent_units(700),false) ||
        !message_timeline_bind_rich_store(&f.timeline,&f.rich)) return false;
    f.source = {&f,kRows,1,resolve,current,&f.timeline,{f.result_order,kRows}};
    f.content.edit = {utf8_literal("has:file"),8,8,true,false,false};
    f.content.heading = utf8_literal("Search MaiCord Lab"); f.content.placeholder = f.content.heading;
    f.content.phase = UiSearchPhase::ready; f.content.panel = true; f.content.total = f.page.total_results;
    f.content.next = true; f.content.revision = 1; return true;
}
inline UiSearchTarget attachment_target(const Fixture& f,u32 index,bool snapshot = false) noexcept {
    const auto* record = message_store_resolve(f.published,f.result_order[index]);
    UiSearchTarget target{UiSearchAction::rich_item,record ? record->channel_id : 0,record ? record->id : 0};
    target.source_revision = f.source.revision; target.source_index = index;
    target.rich_kind = MessageSceneRichItemKind::attachment; target.snapshot = snapshot; return target;
}
inline Color media_color(u64 id,u32 publication) noexcept {
    Color color = id == 9201 ? Color{.75f,.32f,.55f,1} : id == 9103 ? Color{.85f,.5f,.15f,1} : Color{.15f,.65f,.8f,1};
    if (!(publication & 1u)) { const auto red = color.red; color.red = color.blue; color.blue = red; }
    return color;
}
inline bool republish(Fixture& f) noexcept {
    // Model a newly accepted immutable bank with the same result identities.
    // Source/layout revisions do not change the publication's media epoch.
    const u32 next = f.publication_revision + 1;
    for (u32 i = 0; i < kRows; ++i) {
        const auto handle = f.result_order[i];
        const auto* current = message_store_resolve(f.store,handle);
        if (!current || current->revision != f.publication_revision) return false;
        const auto old = *current;
        const auto updated = message_store_upsert(&f.store,{old.id,old.channel_id,old.author_id,
            old.timestamp_ms,old.edited_timestamp_ms,message_store_content(f.store,handle),old.flags,
            old.content_availability,old.delivery,old.presentation_flags,old.type});
        if (!updated.ok() || f.records[handle.slot].revision != next ||
            !message_rich_store_advance_revision(&f.rich,old.id,old.revision,next)) return false;
        const auto* outer = message_rich_store_find(f.rich,old.id);
        const auto* child = message_rich_store_snapshot(f.rich,old.id);
        if (!outer || outer->message_revision != next || (child && child->message_revision != next)) return false;
    }
    f.publication_revision = next; f.published = f.store; return true;
}
inline bool upload_media(D3D11Renderer& renderer,Fixture& f) noexcept {
    const auto* scene = ui_search_scene_message_scene(f.scene);
    if (!scene) return false;
    for (u32 i = 0; i < f.source.count; ++i) for (u32 child = 0; child < 2; ++child) {
        const auto target = attachment_target(f,i,child != 0);
        const auto* owner = message_store_resolve(f.published,f.result_order[i]);
        const auto* rich = child ? message_rich_store_snapshot(f.rich,target.message_id) : message_rich_store_find(f.rich,target.message_id);
        if (!owner || !rich || !rich->attachment_count) continue;
        UiRect rectangle{},clip{};
        if (!ui_search_scene_rich_preview_geometry(f.scene,target,&rectangle,&clip)) continue;
        const auto& attachment = rich->attachments[0];
        const auto format = message_rich_attachment_format(f.rich,attachment);
        const auto tier = renderer.preview_media_atlas ? media_resolution_tier_for_display(
            ui_rect_width(rectangle),ui_rect_height(rectangle),renderer.dpi,media_preview_extent_cap(format)) : MediaResolutionTier::original;
        const auto key = media_asset_search_attachment(attachment.id,format,tier,owner->revision,target.snapshot);
        const auto color = media_color(attachment.id,owner->revision);
        if (format == MediaAttachmentFormat::mp4) {
            log_u64(LogLevel::info,"probe","rich_search_video_seed_format",static_cast<u64>(format));
            log_u64(LogLevel::info,"probe","rich_search_video_seed_tier",static_cast<u64>(media_asset_resolution_tier(key)));
            log_u64(LogLevel::info,"probe","rich_search_video_seed_publication",media_asset_search_revision(key));
        }
        u8 pixels[16 * 16 * 4]{};
        for (u32 y = 0; y < 16; ++y) for (u32 x = 0; x < 16; ++x) {
            const float shade = x < 8 ? 1.f : .65f; const u32 at = (y * 16 + x) * 4;
            pixels[at] = static_cast<u8>(color.red * shade * 255.f + .5f);
            pixels[at + 1] = static_cast<u8>(color.green * shade * 255.f + .5f);
            pixels[at + 2] = static_cast<u8>(color.blue * shade * 255.f + .5f); pixels[at + 3] = 255;
        }
        if (!media_asset_key_valid(key) || !renderer_upload_media_straight_rgba8(&renderer,key,16,16,64,{pixels,sizeof(pixels)}).ok()) return false;
    }
    return true;
}
inline bool read(D3D11Renderer& renderer,const Theme& theme,surface_motion_probe_detail::Readback& rb,u32 image) noexcept {
    if (!d3d11_renderer_draw_stream(&renderer,theme,false,false,nullptr)) return false;
    if (!rb.images && !rb.initialize(renderer)) return false;
    return rb.read(renderer,image);
}
inline bool ink(const surface_motion_probe_detail::Readback& rb,u32 image,UiRect rectangle,float scale,Color color,u32 minimum_pixels = 6,u32* matching = nullptr,u32 tolerance = 24) noexcept {
    const auto clip = raster_clip_rectangle(rectangle,scale,rb.width,rb.height);
    const u8 expected[]{static_cast<u8>(color.blue * 255.f + .5f),static_cast<u8>(color.green * 255.f + .5f),static_cast<u8>(color.red * 255.f + .5f)};
    u32 count = 0;
    for (i32 y = maximum(clip.top,0); y < clip.bottom; ++y) for (i32 x = maximum(clip.left,0); x < clip.right; ++x)
        if (surface_motion_probe_detail::distance(rb.images + image * rb.bytes + (y * rb.width + x) * 4,expected) <= tolerance && ++count >= minimum_pixels) {
            if (matching) *matching = count;
            return true;
        }
    if (matching) *matching = count;
    return false;
}
inline bool pixel_failure(u32 component,u64 owner,UiRect rectangle,u32 matching) noexcept {
    // Component 1=media, 2=Jump, 3=card fill, 4=file label, 5=coverage, 6=main text.
    log_u64(LogLevel::error,"probe","rich_search_pixel_component",component);
    log_u64(LogLevel::error,"probe","rich_search_pixel_owner",owner);
    log_u64(LogLevel::error,"probe","rich_search_pixel_matching",matching);
    log_u64(LogLevel::error,"probe","rich_search_pixel_left_millidip",static_cast<u64>(maximum(0.f,rectangle.left) * 1000));
    log_u64(LogLevel::error,"probe","rich_search_pixel_top_millidip",static_cast<u64>(maximum(0.f,rectangle.top) * 1000));
    log_u64(LogLevel::error,"probe","rich_search_pixel_right_millidip",static_cast<u64>(maximum(0.f,rectangle.right) * 1000));
    log_u64(LogLevel::error,"probe","rich_search_pixel_bottom_millidip",static_cast<u64>(maximum(0.f,rectangle.bottom) * 1000));
    return false;
}
inline bool pixels(const Fixture& f,const D3D11Renderer& renderer,const Theme& theme,
    const surface_motion_probe_detail::Readback& rb,u32 image,u32 minimum_media) noexcept {
    u32 media = 0,jumps = 0,frames = 0,files = 0; const float scale = renderer.dpi / 96.f;
    for (u32 i = 0; i < f.source.count; ++i) for (u32 child = 0; child < 2; ++child) {
        const auto target = attachment_target(f,i,child != 0);
        const auto* owner = message_store_resolve(f.published,f.result_order[i]);
        const auto* rich = child ? message_rich_store_snapshot(f.rich,target.message_id) : message_rich_store_find(f.rich,target.message_id);
        UiRect rectangle{},clip{};
        if (!owner || !rich || !rich->attachment_count || !ui_search_scene_rich_preview_geometry(f.scene,target,&rectangle,&clip)) continue;
        const UiRect patch{rectangle.left + ui_rect_width(rectangle) * .2f,rectangle.top + ui_rect_height(rectangle) * .4f,
            rectangle.left + ui_rect_width(rectangle) * .35f,rectangle.top + ui_rect_height(rectangle) * .6f};
        const auto visible = ui_rect_intersection(patch,clip);
        if (ui_rect_width(visible) < 3 || ui_rect_height(visible) < 3) continue;
        u32 matching{};
        if (!ink(rb,image,visible,scale,media_color(rich->attachments[0].id,owner->revision),12,&matching))
            return pixel_failure(1,rich->attachments[0].id,visible,matching);
        ++media;
    }
    UiTextCommand commands[96]{}; UiTextCommandStream stream{}; UiTextOutput output{&stream,nullptr};
    if (!ui_text_command_stream_initialize(&stream,{commands,96}) || !ui_search_scene_text_output(f.scene,theme,&output,&renderer.search_edit_geometry)) return false;
    for (u32 i = 0; i < stream.count; ++i) if (utf8_equal(commands[i].text,utf8_literal("Jump"))) {
        const auto visible = ui_rect_intersection(commands[i].rectangle,commands[i].clip);
        if (!ui_rect_has_area(visible)) continue;
        u32 matching{};
        // The small native label has antialiased stems: the 96-DPI capture
        // has 5 pixels within RGB-sum24 and 25 within36 of its foreground.
        // Sum48 still excludes its button fill (foreground distance629),
        // while requiring six actual foreground-like raster pixels.
        if (!ink(rb,image,visible,scale,commands[i].color,6,&matching,48)) return pixel_failure(2,i,visible,matching);
        ++jumps;
    }
    for (u32 i = 0; i < f.scene.count; ++i) {
        const auto card = f.scene.row_rectangles[i];
        const auto patch = ui_rect_intersection({card.right - 8,card.top + 31,card.right - 4,card.top + 34},f.scene.body);
        if (!ui_rect_has_area(patch)) continue;
        u32 matching{};
        if (!ink(rb,image,patch,scale,theme.surface,6,&matching)) return pixel_failure(3,f.scene.visible[i].message_id,patch,matching);
        ++frames;
    }
    for (u32 i = 0; i < f.presentation.command_count; ++i) {
        const auto& command = f.presentation.commands[i];
        if (command.measurement_kind != MessageTextMeasurementKind::attachment) continue;
        const auto visible = ui_search_scene_content_clip(f.scene,ui_rect_intersection(command.rectangle,command.clip));
        if (!ui_rect_has_area(visible)) continue;
        u32 matching{};
        if (!ink(rb,image,visible,scale,command.color,6,&matching)) return pixel_failure(4,command.message_id,visible,matching);
        ++files;
    }
    if (media < minimum_media || !jumps || !frames || !files) {
        log_u64(LogLevel::error,"probe","rich_search_pixel_media_count",media);
        log_u64(LogLevel::error,"probe","rich_search_pixel_jump_count",jumps);
        log_u64(LogLevel::error,"probe","rich_search_pixel_frame_count",frames);
        log_u64(LogLevel::error,"probe","rich_search_pixel_file_count",files);
        return pixel_failure(5,0,{},0);
    }
    return true;
}
struct SemanticPoint { MessageTextHit hit{}; UiPoint point{}; };
inline SemanticPoint semantic(const D3D11Renderer& renderer,const Fixture& f,
    MessageMarkdownSemanticKind kind,Utf8View expected = {}) noexcept {
    if (!renderer.content_text || !renderer.search_scene) return {};
    const auto& text = *renderer.content_text;
    for (u32 i = 0; i < text.content_hit_region_count; ++i) {
        const auto& region = text.content_hit_regions[i];
        if (region.semantic_index >= text.content_semantic_count) continue;
        const auto& value = text.content_semantics[region.semantic_index];
        const UiPoint point{(region.left + region.right) * .5f,(region.top + region.bottom) * .5f};
        if (!(value.reserved & kRendererSearchSemanticFlag) || value.kind != kind || value.message_id != kMixedId ||
            (kind != MessageMarkdownSemanticKind::spoiler && (value.reserved & 2u)) ||
            !ui_rect_has_area(ui_search_scene_content_clip(*renderer.search_scene,
                {point.x,point.y,point.x + .01f,point.y + .01f}))) continue;
        const MessageTextHit hit{value.message_id,value.target,value.message_revision,value.source_offset,
            value.source_bytes,value.kind,true};
        if (expected.bytes && !utf8_equal(message_text_presentation_hit_source(f.published,&f.rich,hit),expected)) continue;
        return {hit,point};
    }
    return {};
}
} // namespace rich_search_probe_detail
// Synthetic accepted results use the production rich decoder, shared native
// MessageScene/coordinator, retained Search text bank and native media renderer.
// The injected MP4 tile proves preview UI/ownership, not decoding or playback.
inline bool rich_search_ui_probe(D3D11Renderer& renderer,UiTextMeasureCache& measure,
    bool (*capture)(const wchar_t*,const Theme&) noexcept) noexcept {
    using namespace rich_search_probe_detail;
    if (!renderer.ui_scene || !renderer.message_scene || !renderer.message_text ||
        !renderer.chat_state || !renderer.navigation_view || !renderer.content_text || !renderer.dpi) return false;
    Fixture f{};
    const u32 width = renderer.width,height = renderer.height,dpi = renderer.dpi;
    struct Restore {
        D3D11Renderer& r; const UiSearchScene* scene; const MessageTextPresentation* text;
        UiTextMeasureCache* cache; const UserStore* users; const UiWidgetManager* focus;
        const UiMediaPickerScene* picker; const PopupScene* popup;
        ~Restore() noexcept {
            r.search_scene = scene; r.search_message_text = text; r.search_message_measure_cache = cache;
            r.search_message_users = users; r.focus_owner = focus; r.media_picker_scene = picker;
            renderer_bind_popup_scene(&r,popup); r.retained_presentation.valid = false;
            content_text_retention_invalidate(&r.content_text_retention);
            message_hit_test_cache_invalidate(&r.message_hit_test);
        }
    } restore{renderer,renderer.search_scene,renderer.search_message_text,renderer.search_message_measure_cache,
        renderer.search_message_users,renderer.focus_owner,renderer.media_picker_scene,renderer.popup_scene};
    const auto require = [&](bool ok,u32 stage) noexcept {
        if (!ok) log_u64(LogLevel::error,"probe","rich_search_native_failure_stage",stage);
        return ok;
    };
    if (!require(initialize(f),1)) return false;
    renderer.media_picker_scene = nullptr; renderer_bind_popup_scene(&renderer,nullptr); renderer.focus_owner = nullptr;
    Theme theme = kDefaultTheme;
    const auto project = [&]() noexcept {
        const auto* title = ui_scene_resolve(*renderer.ui_scene,ui_scene_find(*renderer.ui_scene,UiSemantic::title_bar));
        const auto header = ui_scene_header_geometry(*renderer.ui_scene);
        auto viewport = ui_scene_content_rectangle(*renderer.ui_scene,UiSemantic::content);
        viewport.right = renderer.width * 96.f / renderer.dpi;
        if (!require(title && ui_search_scene_update(&f.scene,&f.source,&f.content,viewport,title->rectangle,&header),701)) return false;
        renderer.search_scene = &f.scene;
        if (!require(renderer_bind_search_message_text_presentation(&renderer,&f.presentation,&f.users,&measure),702)) return false;
        auto* scene = ui_search_scene_message_scene(&f.scene);
        if (!require(scene != nullptr,703)) return false;
        const auto result = message_text_coordinator_converge({scene,&f.presentation,&measure,renderer.chat_state,&f.users,
            renderer.navigation_view,&theme,+[](void* owner,const Theme& current_theme) noexcept {
                auto& r = *static_cast<D3D11Renderer*>(owner);
                if (!ui_search_scene_refresh_rich(const_cast<UiSearchScene*>(r.search_scene))) {
                    log_write(LogLevel::error,"probe","rich_search_finalize_refresh_failed");
                    return false;
                }
                if (!renderer_rebuild_content_text(&r,current_theme)) {
                    log_write(LogLevel::error,"probe","rich_search_finalize_text_failed");
                    log_u64(LogLevel::error,"probe","rich_search_text_error",static_cast<u32>(r.content_text->last_error));
                    log_u64(LogLevel::error,"probe","rich_search_shape_error",static_cast<u32>(r.content_text->last_shape_error));
                    return false;
                }
                return true;
            },&renderer,1'759'667'400ull,static_cast<u16>(renderer.dpi),nullptr});
        if (!result.ok()) {
            log_u64(LogLevel::error,"probe","rich_search_coordinator_error",static_cast<u32>(result.error));
            log_u64(LogLevel::error,"probe","rich_search_scene_error",static_cast<u32>(result.scene_error));
            log_u64(LogLevel::error,"probe","rich_search_presentation_error",static_cast<u32>(result.presentation_error));
        }
        return result.ok();
    };
    const auto reproject_main = [&]() noexcept {
        return message_text_coordinator_converge({const_cast<MessageScene*>(renderer.message_scene),
            const_cast<MessageTextPresentation*>(renderer.message_text),
            renderer.message_measure_cache ? renderer.message_measure_cache : &measure,
            renderer.chat_state,renderer.user_store,renderer.navigation_view,&theme,
            +[](void* owner,const Theme& current_theme) noexcept {
                return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner),current_theme);
            },&renderer,1'759'667'400ull,static_cast<u16>(renderer.dpi),nullptr}).ok();
    };
    const auto show = [&](const wchar_t* name) noexcept {
        return require(project(),601) && require(upload_media(renderer,f),602) &&
            require(renderer_rebuild_content_text(&renderer,theme),603) &&
            require(probe_present(&renderer,theme),604) && require(!capture || capture(name,theme),605);
    };
    const auto warm = [&]() noexcept {
        const auto* scene = ui_search_scene_message_scene(f.scene);
        if (!scene) return false;
        const u32 calls = f.calls,layout = f.scene.widgets.layout.revision,native_layout = scene->layout_passes,
            prepare = scene->preparation_passes,resolve_count = scene->rebuilds,shape = measure.backend_calls,
            builds = renderer.content_text->rebuilds,raster = renderer.glyph_text.cache_builds,
            paint_upload = renderer.paint_instance_uploads,text_upload = renderer.text_instance_uploads,
            media_upload = renderer.media_instance_uploads,draws = renderer.painted_frames,skips = renderer.unchanged_present_skips;
        for (u32 i = 0; i < 64; ++i) if (!project() || !probe_present(&renderer,theme)) return false;
        return f.calls == calls && f.scene.widgets.layout.revision == layout && scene->layout_passes == native_layout &&
            scene->preparation_passes == prepare && scene->rebuilds == resolve_count && measure.backend_calls == shape &&
            renderer.content_text->rebuilds == builds && renderer.glyph_text.cache_builds == raster &&
            renderer.paint_instance_uploads == paint_upload && renderer.text_instance_uploads == text_upload &&
            renderer.media_instance_uploads == media_upload && renderer.painted_frames == draws &&
            renderer.unchanged_present_skips - skips == 64;
    };
    renderer_set_dpi(&renderer,96);
    if (!require(probe_resize_surface(1000,800) && show(L"rich-search-mixed-masked.bmp"),2)) return false;
    auto* native = ui_search_scene_message_scene(&f.scene);
    const auto* mixed = native ? message_scene_find_row(*native,0) : nullptr;
    const auto* snapshot = mixed ? message_scene_snapshot_row(*native,*mixed) : nullptr;
    const auto* main = renderer.message_scene->count ? message_store_resolve(*renderer.message_scene->timeline->store,
        renderer.message_scene->rows[0].message) : nullptr;
    if (!require(mixed && snapshot && main && main->id == kMixedId && main->revision == mixed->message_revision &&
        native != renderer.message_scene && renderer.search_message_text != renderer.message_text &&
        renderer.search_message_users == &f.users && f.order[0] != f.result_order[0].slot,3)) return false;
    const auto link = semantic(renderer,f,MessageMarkdownSemanticKind::link,utf8_literal("https://original.example/review"));
    const auto frozen = semantic(renderer,f,MessageMarkdownSemanticKind::link,utf8_literal("https://original.example/snapshot"));
    const auto spoiler = semantic(renderer,f,MessageMarkdownSemanticKind::spoiler);
    if (!require(link.hit.valid && frozen.hit.valid && spoiler.hit.valid &&
        !semantic(renderer,f,MessageMarkdownSemanticKind::link,utf8_literal("https://original.example/private")).hit.valid,4)) return false;
    const auto px = static_cast<i32>(link.point.x),py = static_cast<i32>(link.point.y);
    if (!require(!renderer_content_hit_test(renderer,px,py).valid &&
        ui_search_scene_hit_test(f.scene,link.point.x,link.point.y).action != UiSearchAction::result,5)) return false;
    bool main_hit = false;
    for (u32 i = 0; i < renderer.message_text->command_count; ++i) {
        const auto& command = renderer.message_text->commands[i];
        if (command.message_id != kMixedId || command.measurement_kind != MessageTextMeasurementKind::content) continue;
        const auto x = static_cast<i32>(command.rectangle.left + 4),y = static_cast<i32>(command.rectangle.top + 8);
        if (ui_rect_contains(f.scene.panel,static_cast<float>(x),static_cast<float>(y))) continue;
        const auto selected = renderer_message_text_position_hit_test(renderer,x,y);
        main_hit = selected.valid && selected.message_id == kMixedId && selected.command_identity == command.measurement_identity &&
            selected.text_bytes == command.text.bytes;
        if (main_hit) break;
    }
    if (!require(main_hit,6)) return false;
    UiSearchTarget jump{};
    for (u32 i = 0; i < f.scene.count; ++i) if (f.scene.visible[i].message_id == kMixedId) {
        const auto* node = ui_widget_manager_layout_node(f.scene.widgets,f.scene.rows[i]);
        if (!node || !ui_rect_has_area(node->rectangle)) continue;
        jump = ui_search_scene_hit_test(f.scene,(node->rectangle.left + node->rectangle.right) * .5f,
            (node->rectangle.top + node->rectangle.bottom) * .5f);
    }
    const auto snapshot_target = attachment_target(f,0,true);
    UiRect snapshot_rectangle{},snapshot_clip{};
    if (!require(jump.action == UiSearchAction::result && jump.message_id == kMixedId && jump.source_revision == f.source.revision &&
        ui_search_scene_rich_preview_geometry(f.scene,snapshot_target,&snapshot_rectangle,&snapshot_clip) &&
        ui_search_scene_hit_test(f.scene,(snapshot_rectangle.left + snapshot_rectangle.right) * .5f,
            (snapshot_rectangle.top + snapshot_rectangle.bottom) * .5f).snapshot,7)) return false;
    {
        surface_motion_probe_detail::Readback rb{};
        if (!require(read(renderer,theme,rb,0),80) || !require(pixels(f,renderer,theme,rb,0,1),8)) return false;
        bool changed{};
        if (!require(message_text_presentation_toggle_spoiler(&f.presentation,f.published,spoiler.hit,&changed) && changed &&
            show(L"rich-search-mixed-revealed.bmp") && read(renderer,theme,rb,1) &&
            semantic(renderer,f,MessageMarkdownSemanticKind::link,utf8_literal("https://original.example/private")).hit.valid,9)) return false;
        const UiRect area{spoiler.point.x - 8,spoiler.point.y - 6,spoiler.point.x + 8,spoiler.point.y + 6};
        const auto region = raster_clip_rectangle(area,1,rb.width,rb.height); u32 difference = 0;
        for (i32 y = region.top; y < region.bottom; ++y) for (i32 x = region.left; x < region.right; ++x) {
            const u32 at = (y * rb.width + x) * 4;
            if (surface_motion_probe_detail::distance(rb.images + at,rb.images + rb.bytes + at) > 8) ++difference;
        }
        if (!require(difference >= 8,10)) return false;
    }
    if (!require(warm(),11)) return false;
    log_write(LogLevel::info,"probe","rich_search_64_zero_resolve_layout_shape_raster_upload_draw_passed");
    const auto first_handle = f.result_order[0];
    if (!require(republish(f),28)) return false;
    for (u32 i = 0; i + 1 < kRows; ++i) f.result_order[i] = f.result_order[i + 1];
    f.result_order[kRows - 1] = first_handle; ++f.source.revision; ++f.content.revision;
    f.content.offset = 25; f.content.previous = true; f.content.next = false;
    if (!require(!ui_search_scene_target_current(f.scene,jump),1201) ||
        !require(!ui_search_scene_target_current(f.scene,snapshot_target),1202) ||
        !require(show(L"rich-search-media-only-page.bmp"),12)) return false;
    u32 clipped_commands = 0;
    for (u32 i = 0; i < f.presentation.command_count; ++i) {
        const auto& command = f.presentation.commands[i];
        const UiRect clip = ui_search_scene_content_clip(f.scene,command.clip);
        if (command.text.bytes && ui_rect_has_area(command.rectangle) && ui_rect_has_area(clip) &&
            !ui_rect_has_area(ui_rect_intersection(command.rectangle,clip)))
            ++clipped_commands;
    }
    if (!require(clipped_commands != 0,1204)) return false;
    {
        surface_motion_probe_detail::Readback rb{};
        if (!require(read(renderer,theme,rb,0) && pixels(f,renderer,theme,rb,0,2),13)) return false;
    }
    bool save_reachable = false;
    for (u32 i = 0; i < f.scene.widgets.count; ++i) {
        const auto target = f.scene.targets[i];
        if (!target.save || target.message_id != 9003 || target.source_index != 1) continue;
        const UiWidgetHandle handle{static_cast<u16>(i),f.scene.widgets.generation};
        save_reachable = ui_widget_manager_focus(&f.scene.widgets,handle) && ui_search_scene_target_current(f.scene,target);
    }
    if (!require(save_reachable,26)) return false;
    ui_widget_manager_focus(&f.scene.widgets,ui_widget_invalid_handle());
    if (!require(project() && probe_present(&renderer,theme) && warm(),27)) return false;
    log_write(LogLevel::info,"probe","rich_search_media_page_64_zero_work_and_separate_file_save_keyboard_passed");
    const float outside = renderer.width * 96.f / renderer.dpi - ui_rect_width(ui_scene_content_rectangle(*renderer.ui_scene,UiSemantic::content));
    if (!require(probe_resize_surface(static_cast<u32>(outside + 320),640) && show(L"rich-search-narrow.bmp") && f.scene.overlay,14)) return false;
    if (!require(ui_search_scene_scroll(&f.scene,ui_search_scene_scroll_maximum(f.scene)) && project(),15)) return false;
    const auto last_target = attachment_target(f,kRows - 1,true);
    UiRect clipped_rectangle{},clipped{};
    if (!require(ui_search_scene_rich_preview_geometry(f.scene,last_target,&clipped_rectangle,&clipped) &&
        clipped.left >= f.scene.body.left && clipped.right <= f.scene.body.right &&
        clipped.top >= f.scene.body.top && clipped.bottom <= f.scene.body.bottom,16)) return false;
    if (!require(ui_search_scene_scroll(&f.scene,0) && project(),17)) return false;
    renderer_set_dpi(&renderer,144); theme = kLightTheme;
    // The resize helper seeds the borrowed main projector in the default
    // theme. Reproject it once at this actual Light/DPI handoff; Search keeps
    // its separate native bank and the normal retained rendering path.
    if (!require(probe_resize_surface(1440,1200) && project() && reproject_main() &&
        show(L"rich-search-light-dpi144.bmp"),18)) return false;
    surface_motion_probe_detail::Readback chooser_readback{};
    {
        auto& rb = chooser_readback;
        if (!require(read(renderer,theme,rb,0) && pixels(f,renderer,theme,rb,0,2),19)) return false;
        bool main_ink = false;
        for (u32 i = 0; i < renderer.message_text->command_count; ++i) {
            const auto& command = renderer.message_text->commands[i];
            if (command.message_id != kMixedId || command.measurement_kind != MessageTextMeasurementKind::content) continue;
            auto visible = ui_rect_intersection(command.rectangle,command.clip);
            visible.right = minimum(visible.right,f.scene.panel.left);
            if (!ui_rect_has_area(visible)) continue;
            u32 matching{};
            if (command.color.red != theme.text_primary.red || command.color.green != theme.text_primary.green ||
                command.color.blue != theme.text_primary.blue ||
                !ink(rb,0,visible,renderer.dpi / 96.f,theme.text_primary,12,&matching,48))
                return pixel_failure(6,command.message_id,visible,matching);
            main_ink = true;
        }
        if (!require(main_ink,29)) return false;
    }
    {
        // Official-client parity tabs: the strip with counts, then each item layout over the same bank.
        f.content.tabs = true; f.content.tab_known = 0b11111;
        const u64 totals[] = {29,3,4,2,1};
        for (u32 i = 0; i < 5; ++i) f.content.tab_totals[i] = totals[i];
        const wchar_t* names[] = {L"rich-search-tab-messages.bmp",L"rich-search-tab-media.bmp",
            L"rich-search-tab-files.bmp",L"rich-search-tab-links.bmp"};
        for (u8 tab = 0; tab < 4; ++tab) {
            f.content.tab = f.content.result_tab = tab; ++f.content.revision;
            const bool shown = show(names[tab]);
            if (!require(shown && (tab != 1 || ui_search_scene_visible_tile_count(f.scene) == 3),40 + tab))
                return false;
        }
        const Utf8View recent[] = {utf8_literal("release notes"),utf8_literal("from:Mira has:image"),
            utf8_literal("fileType:pdf linkFrom:original.example")};
        f.content.tab = f.content.result_tab = 0; f.content.edit = {{},0,0,true,true,true};
        f.content.suggestions = true; f.content.recent = recent; f.content.recent_count = 3;
        ++f.content.revision;
        if (!require(show(L"rich-search-recent.bmp"),44)) return false;
        f.content.filters = true; ++f.content.revision;
        if (!require(show(L"rich-search-filters.bmp"),45)) return false;
        f.content.filters = f.content.suggestions = f.content.tabs = false; f.content.recent = nullptr;
        f.content.recent_count = 0; f.content.edit = {utf8_literal("has:file"),8,8,true,false,false};
        ++f.content.revision;
        if (!require(project() && ui_search_scene_scroll(&f.scene,0) && project(),46)) return false;
    }
    const auto saved = attachment_target(f,0);
    ++f.source.revision; ++f.content.revision;
    if (!require(!ui_search_scene_target_current(f.scene,saved) && !ui_search_scene_target_current(f.scene,jump),20)) return false;
    const auto* old = message_store_resolve(f.published,first_handle);
    if (!require(old != nullptr,21)) return false;
    const auto mutation = message_store_upsert(&f.store,{old->id,old->channel_id,old->author_id,old->timestamp_ms,0,
        utf8_literal("Updated source: [new guide](https://original.example/updated)."),0});
    GatewayMessageEvent update{}; update.id = kMixedId; update.fields = gateway_message_field_content;
    update.content = utf8_literal("Updated source: [new guide](https://original.example/updated).");
    if (!require(mutation.ok() && message_rich_store_apply(&f.rich,kMixedId,f.records[first_handle.slot].revision,update).ok(),21)) return false;
    f.published = f.store;
    if (!require(project() && message_text_presentation_hit_source(f.published,&f.rich,link.hit).bytes == 0,22)) return false;
    f.current = false;
    if (!require(!ui_search_scene_current(f.scene) && !ui_search_scene_target_current(f.scene,saved),23)) return false;
    UiSearchSource empty{}; empty.revision = f.source.revision + 1;
    empty.resolve = +[](void*,u32,UiSearchRow*) noexcept { return false; };
    f.content.total = 0; f.content.offset = 0; f.content.previous = f.content.next = false;
    f.content.status = utf8_literal("No results in this search scope."); ++f.content.revision;
    renderer_unbind_search_message_text_presentation(&renderer);
    const auto* title = ui_scene_resolve(*renderer.ui_scene,ui_scene_find(*renderer.ui_scene,UiSemantic::title_bar));
    const auto header = ui_scene_header_geometry(*renderer.ui_scene);
    auto viewport = ui_scene_content_rectangle(*renderer.ui_scene,UiSemantic::content); viewport.right = renderer.width * 96.f / renderer.dpi;
    if (!require(title && ui_search_scene_update(&f.scene,&empty,&f.content,viewport,title->rectangle,&header) && !f.scene.count &&
        renderer_rebuild_content_text(&renderer,theme) && probe_present(&renderer,theme) &&
        (!capture || capture(L"rich-search-retired-empty.bmp",theme)),24)) return false;
    if (!search_filter_ui_probe(renderer,chooser_readback)) return false;
    renderer_set_dpi(&renderer,dpi);
    if (!require(probe_resize_surface(width,height),25)) return false;
    log_write(LogLevel::info,"probe","rich_search_decoder_snapshot_media_spoiler_links_jump_owner_collision_scope_dpi_light_passed");
    return true;
}
} // namespace maicord
