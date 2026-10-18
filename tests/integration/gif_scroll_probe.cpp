#include "probe_present.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/core/log.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u32 count = 4;
constexpr u32 widgets = count * kMessageSceneWidgetsPerRow + 1;
struct GifScrollFixture {
    MessageRecord records[count];
    u32 order[count];
    u8 text[128];
    MessageRichRecord rich_records[count];
    u16 rich_order[count];
    u8 rich_text[2048];
    u32 heights[count];
    u64 fenwick[count];
    UiWidget widget[widgets];
    UiLayoutNode node[widgets];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
    MediaAtlas primary, preview;
    MediaAtlasEntry entries[2][16];
    u32 buckets[2][32];
    GlyphAtlasEntry allocations[2][16];
    GlyphAtlasRow rows[2][16];
    u8 indices[640 * 360];
};
GifScrollFixture fixture;
}

bool gif_scroll_probe(D3D11Renderer& r, bool unpaced) noexcept {
    auto& f = fixture;
    bool ok = message_store_initialize(&f.store, {f.records, count}, {f.order, count}, {f.text, sizeof(f.text)}) &&
        message_rich_store_initialize(&f.rich, {f.rich_records, count}, {f.rich_order, count}, {f.rich_text, sizeof(f.rich_text)});
    for (u32 i = 0; ok && i < count; ++i) {
        ok = message_store_upsert(&f.store, {90'000 + i, 7, 20 + i, 0, 0, {}, 0}).ok();
        GatewayMessageAttachment attachment{};
        attachment.id = 91'000 + i;
        attachment.filename = utf8_literal("scroll.gif");
        attachment.url = utf8_literal("https://cdn.discordapp.com/attachments/7/91000/scroll.gif");
        attachment.content_type = utf8_literal("image/gif");
        attachment.width = 640;
        attachment.height = 360;
        attachment.has_width = attachment.has_height = true;
        GatewayMessageEvent event{};
        event.id = 90'000 + i;
        event.fields = gateway_message_field_attachments;
        event.attachments = {&attachment, 1};
        ok = ok && message_rich_store_apply(&f.rich, event.id, f.records[i].revision, event).ok();
    }
    const UiRect viewport = r.message_scene->viewport;
    ok = ok && ui_variable_list_initialize(&f.list, {f.heights, count}, {f.fenwick, count}, count, ui_extent_units(220)) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(220), ui_extent_units(690), false) &&
        message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
        message_scene_initialize_owned(&f.scene, &f.timeline, count, 1, {f.widget, widgets}, {f.node, widgets}) &&
        message_scene_update(&f.scene, viewport).ok();
    if (!ok) { message_scene_release(&f.scene); return false; }
    const auto* old_scene = r.message_scene;
    const auto* old_text = r.message_text;
    auto* old_primary = r.media_atlas;
    auto* old_preview = r.preview_media_atlas;
    renderer_unbind_media_atlas(&r);
    ok = media_atlas_initialize(&f.primary, 64, 64, 1, {f.entries[0], 16}, {f.buckets[0], 32}, {f.allocations[0], 16}, {f.rows[0], 16}) &&
        media_atlas_initialize(&f.preview, 2048, 2048, 1, {f.entries[1], 16}, {f.buckets[1], 32}, {f.allocations[1], 16}, {f.rows[1], 16}) &&
        renderer_bind_media_atlas(&r, &f.primary) && renderer_bind_preview_media_atlas(&r, &f.preview);
    r.message_scene = &f.scene;
    r.message_text = nullptr;
    MediaAssetKey keys[3]{};
    MessageMediaCommand commands[16]{};
    MessageMediaCommandStream media{};
    PaintStream paint{};
    MessagePresentationResult result{};
    paint_stream_reset(&paint, 900, 800);
    ok = ok && message_media_command_stream_initialize(&media, {commands, 16}) &&
        message_presentation_build(f.scene, kDefaultTheme, &paint, &media, &result);
    u32 found = 0;
    for (u32 i = 0; ok && i < media.count && found < 3; ++i) {
        if (commands[i].kind == MessageMediaCommandKind::attachment) {
            const auto tier = media_resolution_tier_for_display(ui_rect_width(commands[i].rectangle),
                ui_rect_height(commands[i].rectangle), r.dpi);
            keys[found++] = media_asset_attachment(commands[i].id,
                static_cast<MediaAttachmentFormat>(commands[i].format), tier);
        }
    }
    ok = ok && found == 3;
    GifFrame frame{};
    frame.width = 640;
    frame.height = 360;
    GifPalette palette{};
    palette.count = 256;
    for (u32 i = 0; i < 256; ++i) palette.rgba[i] = 0xff000000u | (i * 0x010101u);
    for (u32 i = 0; i < sizeof(f.indices); ++i) f.indices[i] = static_cast<u8>(i);
    for (u32 i = 0; ok && i < 3; ++i) ok = renderer_begin_gif(&r, keys[i], 640, 360, frame, palette, {f.indices, sizeof(f.indices)}).ok();
    ok = ok && (unpaced ? probe_present(&r, kDefaultTheme) : renderer_present(&r, kDefaultTheme));
    const u32 before = r.full_present_draws + r.partial_present_draws;
    const u32 gif_before = r.media_gpu.gif_frames;
    // Only the timeline scrolls: every build replays the shell segment and
    // no glyph is uploaded again.
    const u32 segment_replays = r.shell_segment.replays, segment_builds = r.shell_segment.builds;
    const u64 text_bytes = r.text_upload_bytes;
    LARGE_INTEGER frequency{}, start{}, finish{};
    QueryPerformanceFrequency(&frequency);
    u64 work_us = 0, max_work_us = 0;
    constexpr u32 steps = 48;
    float prior_top = 0;
    u32 moved = 0;
    for (u32 step = 0; ok && step < steps; ++step) {
        // Like the application loop: acquire the slot before sampling scroll
        // and collecting all three independent animation completions.
        ok = unpaced || d3d11_device_owner_wait(r.graphics, 1000) == WAIT_OBJECT_0;
        renderer_accept_begin_frame(&r);
        QueryPerformanceCounter(&start);
        bool changed = false;
        ok = ok && message_scene_scroll_by(&f.scene, step % 2 == 0 ? 64 : -64, 1, &changed) && changed &&
            message_scene_update(&f.scene, viewport).ok();
        for (u32 i = 0; ok && i < 3; ++i) {
            palette.rgba[0] = 0xff000000u | ((step + i) * 0x030303u);
            ok = renderer_advance_gif(&r, keys[i], frame, frame, palette, {f.indices, sizeof(f.indices)}).ok();
        }
        ok = ok && (unpaced ? probe_present(&r, kDefaultTheme) : renderer_present(&r, kDefaultTheme));
        QueryPerformanceCounter(&finish);
        const u64 elapsed = static_cast<u64>(finish.QuadPart - start.QuadPart) * 1'000'000 / frequency.QuadPart;
        work_us += elapsed;
        max_work_us = maximum(max_work_us, elapsed);
        u32 visible = 0;
        float top = 0;
        for (u32 q = 0; q < r.media_quad_count; ++q) {
            const auto handle = r.media_draw.handle_data[q];
            for (u32 i = 0; i < 3; ++i) {
                const auto allocation = media_atlas_peek(&f.preview, keys[i]);
                if (allocation.ok() && handle.slot == allocation.handle.slot && handle.generation == allocation.handle.generation &&
                    r.media_draw.quad_data[q].v0 < 0.0f) {
                    ++visible;
                    if (i == 0) top = r.media_draw.quad_data[q].top;
                }
            }
        }
        if (step != 0 && top != prior_top) ++moved;
        prior_top = top;
        ok = ok && visible == 3 && r.full_present_draws + r.partial_present_draws == before + step + 1;
    }
    ok = ok && moved == steps - 1 && r.media_gpu.gif_frames == gif_before + steps * 3;
    log_u64(LogLevel::info, "probe", "three_gif_scroll_shell_replays", r.shell_segment.replays - segment_replays);
    log_u64(LogLevel::info, "probe", "three_gif_scroll_shell_builds", r.shell_segment.builds - segment_builds);
    log_u64(LogLevel::info, "probe", "three_gif_scroll_text_upload_bytes", r.text_upload_bytes - text_bytes);
    ok = ok && r.shell_segment.replays - segment_replays == steps && r.shell_segment.builds == segment_builds &&
         r.text_upload_bytes == text_bytes;
    log_write(LogLevel::info, "probe", unpaced ? "three_gif_scroll_unpaced" : "three_gif_scroll_paced");
    log_u64(LogLevel::info, "probe", "three_gif_scroll_work_average_us", work_us / steps);
    log_u64(LogLevel::info, "probe", "three_gif_scroll_work_max_us", max_work_us);
    log_u64(LogLevel::info, "probe", "three_gif_scroll_presented_steps", r.full_present_draws + r.partial_present_draws - before);
    log_u64(LogLevel::info, "probe", "three_gif_scroll_moved_steps", moved);
    log_u64(LogLevel::info, "probe", "three_gif_scroll_advanced_frames", r.media_gpu.gif_frames - gif_before);
    log_write(ok ? LogLevel::info : LogLevel::error, "probe", ok ? "three_gif_scroll_pass" : "three_gif_scroll_failed");
    renderer_unbind_media_atlas(&r);
    r.media_atlas = old_primary;
    r.preview_media_atlas = old_preview;
    r.message_scene = old_scene;
    r.message_text = old_text;
    r.retained_presentation.valid = false;
    paint_stream_release(&paint);
    message_scene_release(&f.scene);
    return ok;
}
} // namespace maicord
