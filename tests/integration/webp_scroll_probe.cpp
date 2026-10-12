#include "probe_present.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/core/log.hpp"
#include "maicord/media/presentation_policy.hpp"
#include "maicord/core/hash.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"

#include <d3d11_1.h>

namespace maicord {
bool probe_resize_surface(u32 width, u32 height) noexcept;
namespace {
constexpr u32 count = 6;
constexpr u32 widgets = count * kMessageSceneWidgetsPerRow + 1;
struct WebpScrollFixture {
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
    AnimatedWebpWorkspace animation;
    LossyWebpWorkspace lossy;
    Vp8lWorkspace lossless;
    u8 scratch[512 * 512 * 4];
    u8 alpha[512 * 512];
};
WebpScrollFixture fixture;
}

bool webp_scroll_probe(D3D11Renderer& r, ConstSlice<u8> encoded) noexcept {
    auto& f = fixture;
    AnimatedWebpImage animation{};
    if (!animated_webp_parse(encoded, &animation, &f.animation).ok() || animation.frame_count > 256) return false;
    const u32 original_width = r.width, original_height = r.height;
    if (!probe_resize_surface(original_width, 1500)) return false;
    const u64 fingerprint = hash64_counted_bytes(kHash64Seed, encoded.data, encoded.count);
    u64 expected[256]{};
    bool ok = message_store_initialize(&f.store, {f.records, count}, {f.order, count}, {f.text, sizeof(f.text)}) &&
        message_rich_store_initialize(&f.rich, {f.rich_records, count}, {f.rich_order, count}, {f.rich_text, sizeof(f.rich_text)});
    for (u32 i = 0; ok && i < count; ++i) {
        ok = message_store_upsert(&f.store, {90'000 + i, 7, 20, 0, 0, {}, 0}).ok();
        GatewayMessageAttachment attachment{};
        attachment.id = 91'000 + i;
        attachment.filename = utf8_literal("scroll.webp");
        attachment.url = utf8_literal("https://cdn.discordapp.com/attachments/7/91000/scroll.webp");
        attachment.content_type = utf8_literal("image/webp");
        attachment.width = 512;
        attachment.height = 288;
        attachment.has_width = attachment.has_height = true;
        GatewayMessageEvent event{};
        event.id = 90'000 + i;
        event.fields = gateway_message_field_attachments;
        event.attachments = {&attachment, 1};
        ok = ok && message_rich_store_apply(&f.rich, event.id, f.records[i].revision, event).ok();
    }
    UiRect viewport = r.message_scene->viewport;
    ok = ok && ui_variable_list_initialize(&f.list, {f.heights, count}, {f.fenwick, count}, count, ui_extent_units(220)) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(220), ui_extent_units(690), false) &&
        message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
        message_scene_initialize_owned(&f.scene, &f.timeline, count, 1, {f.widget, widgets}, {f.node, widgets}) &&
        message_scene_update(&f.scene, viewport).ok();
    if (!ok) { message_scene_release(&f.scene); probe_resize_surface(original_width, original_height); return false; }
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
    MediaAssetKey keys[5]{};
    MessageMediaCommand commands[16]{};
    MessageMediaCommandStream media{};
    PaintStream paint{};
    MessagePresentationResult result{};
    paint_stream_reset(&paint, static_cast<float>(r.width), static_cast<float>(r.height));
    ok = ok && message_media_command_stream_initialize(&media, {commands, 16}) &&
        message_presentation_build(f.scene, kDefaultTheme, &paint, &media, &result);
    u32 found = 0;
    for (u32 i = 0; ok && i < media.count && found < 5; ++i) {
        if (commands[i].kind == MessageMediaCommandKind::attachment) {
            const auto tier = media_resolution_tier_for_display(ui_rect_width(commands[i].rectangle),
                ui_rect_height(commands[i].rectangle), r.dpi);
            keys[found++] = media_asset_attachment(commands[i].id,
                static_cast<MediaAttachmentFormat>(commands[i].format), tier);
        }
    }
    ok = ok && found == 5;
    ID3D11Texture2D* readback = nullptr;
    auto read_frame_hashes = [&](bool save, u32 frame_index) noexcept {
        D3D11_TEXTURE2D_DESC desc{};
        r.media_gpu.preview_texture->GetDesc(&desc);
        if (!readback) {
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(r.graphics.device->CreateTexture2D(&desc, nullptr, &readback))) return false;
        }
        r.graphics.context1->CopyResource(readback, r.media_gpu.preview_texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(r.graphics.context1->Map(readback, 0, D3D11_MAP_READ, 0, &mapped))) return false;
        bool exact = true;
        for (u32 i = 0; i < (save ? 1u : 5u); ++i) {
            const auto tile = media_atlas_peek(&f.preview, keys[i]);
            u64 hash = kHash64Seed;
            exact = exact && tile.ok();
            for (u32 y = 0; tile.ok() && y < tile.height; ++y)
                hash = hash64_counted_bytes(hash, static_cast<const u8*>(mapped.pData) +
                    (tile.y + y) * mapped.RowPitch + tile.x * 4u, tile.width * 4u);
            if (save) expected[frame_index] = hash;
            else exact = exact && hash == expected[frame_index];
        }
        r.graphics.context1->Unmap(readback, 0);
        return exact;
    };
    AnimatedWebpFrame previous{};
    for (u32 index = 0; ok && index < animation.frame_count; ++index) {
        Vp8Keyframe vp8{}; Vp8ControlHeader header{};
        const auto decoded = animated_webp_decode_parsed_frame(encoded, animation, index,
            &f.lossless, &f.lossy, &vp8, &header, {}, {f.scratch, sizeof(f.scratch)}, {f.alpha, sizeof(f.alpha)});
        const auto& frame = decoded.frame;
        const ConstSlice<u8> alpha = frame.has_alpha ? ConstSlice<u8>{f.alpha, static_cast<u32>(frame.width) * frame.height} : ConstSlice<u8>{};
        ok = decoded.ok() && frame.codec == AnimatedWebpCodec::vp8_lossy;
        if (index == 0) {
            for (u32 i = 0; ok && i < 5; ++i)
                ok = renderer_begin_animated_webp_vp8(&r, keys[i], animation.width, animation.height,
                    animation.background_rgba, frame, vp8, header, f.lossy.modes, f.lossy.coefficients, alpha).ok();
        } else if (ok) {
            ok = renderer_advance_animated_webp_vp8(&r, keys[0], animation.background_rgba, previous,
                frame, vp8, header, f.lossy.modes, f.lossy.coefficients, alpha).ok();
        }
        previous = frame;
        AnimationFrameCacheDescriptor descriptor{};
        descriptor.key = {keys[0], index, 1, 0, fingerprint};
        descriptor.width = animation.width; descriptor.height = animation.height;
        descriptor.duration_us = frame.duration_ms * 1000ull;
        descriptor.background_rgba = animation.background_rgba;
        descriptor.kind = AnimationFrameCacheKind::animated_webp; descriptor.control.animated_webp = frame;
        ok = ok && renderer_capture_animation_frame(&r, descriptor) && read_frame_hashes(true, index);
    }
    ok = ok && probe_present(&r, kDefaultTheme);
    const u32 before = r.full_present_draws + r.partial_present_draws;
    const u32 decoded_before = r.media_gpu.vp8_reconstructions;
    const u64 restored_before = r.preview_animation_gpu.restores;
    LARGE_INTEGER frequency{}, start{}, finish{};
    QueryPerformanceFrequency(&frequency);
    u64 work_us = 0, max_work_us = 0;
    ID3D11Query* disjoint = nullptr; ID3D11Query* gpu_start = nullptr; ID3D11Query* gpu_end = nullptr;
    D3D11_QUERY_DESC query_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ok = ok && SUCCEEDED(r.graphics.device->CreateQuery(&query_desc, &disjoint));
    query_desc.Query = D3D11_QUERY_TIMESTAMP;
    ok = ok && SUCCEEDED(r.graphics.device->CreateQuery(&query_desc, &gpu_start)) &&
        SUCCEEDED(r.graphics.device->CreateQuery(&query_desc, &gpu_end));
    u64 gpu_us = 0, max_gpu_us = 0;
    constexpr u32 steps = 48;
    float prior_top = 0;
    u32 moved = 0;
    for (u32 step = 0; ok && step < steps; ++step) {
        // Acquire the presentation slot before sampling scroll and restoring
        // all five animations. Readback verification is outside work timing.
        ok = d3d11_device_owner_wait(r.graphics, 1000) == WAIT_OBJECT_0;
        renderer_accept_begin_frame(&r);
        QueryPerformanceCounter(&start);
        bool changed = false;
        ok = ok && message_scene_scroll_by(&f.scene, step % 2 == 0 ? 8 : -8, 1, &changed) && changed &&
            message_scene_update(&f.scene, viewport).ok();
        r.graphics.context1->Begin(disjoint);
        r.graphics.context1->End(gpu_start);
        for (u32 i = 0; ok && i < 5; ++i) {
            AnimationFrameCacheResult restored{};
            ok = renderer_restore_animation_frame(&r, {keys[i], step % animation.frame_count,
                static_cast<u16>(i + 1), 0, fingerprint}, &restored);
        }
        ok = ok && probe_present(&r, kDefaultTheme);
        r.graphics.context1->End(gpu_end);
        r.graphics.context1->End(disjoint);
        QueryPerformanceCounter(&finish);
        const u64 elapsed = static_cast<u64>(finish.QuadPart - start.QuadPart) * 1'000'000 / frequency.QuadPart;
        work_us += elapsed;
        max_work_us = maximum(max_work_us, elapsed);
        u32 visible = 0;
        float top = 0;
        for (u32 q = 0; q < r.media_quad_count; ++q) {
            const auto handle = r.media_draw.handle_data[q];
            for (u32 i = 0; i < 5; ++i) {
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
        ok = ok && read_frame_hashes(false, step % animation.frame_count);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{}; UINT64 first_tick = 0, last_tick = 0;
        ok = ok && r.graphics.context1->GetData(disjoint, &timing, sizeof(timing), 0) == S_OK &&
            !timing.Disjoint && timing.Frequency != 0 &&
            r.graphics.context1->GetData(gpu_start, &first_tick, sizeof(first_tick), 0) == S_OK &&
            r.graphics.context1->GetData(gpu_end, &last_tick, sizeof(last_tick), 0) == S_OK;
        const u64 elapsed_gpu = timing.Frequency ? (last_tick - first_tick) * 1000000 / timing.Frequency : 0;
        gpu_us += elapsed_gpu; max_gpu_us = maximum(max_gpu_us, elapsed_gpu);
        ok = ok && visible == 5 && r.full_present_draws + r.partial_present_draws == before + step + 1;
    }
    ok = ok && moved == steps - 1 && r.media_gpu.vp8_reconstructions == decoded_before &&
        r.preview_animation_gpu.restores == restored_before + steps * 5 &&
        r.preview_animation_gpu.cache.entry_count == animation.frame_count;
    log_u64(LogLevel::info, "probe", "five_webp_scroll_work_average_us", work_us / steps);
    log_u64(LogLevel::info, "probe", "five_webp_scroll_work_max_us", max_work_us);
    log_u64(LogLevel::info, "probe", "five_webp_scroll_gpu_interval_average_us", gpu_us / steps);
    log_u64(LogLevel::info, "probe", "five_webp_scroll_gpu_interval_max_us", max_gpu_us);
    log_u64(LogLevel::info, "probe", "five_webp_scroll_presented_steps", r.full_present_draws + r.partial_present_draws - before);
    log_u64(LogLevel::info, "probe", "five_webp_scroll_moved_steps", moved);
    log_u64(LogLevel::info, "probe", "five_webp_scroll_cached_frames", r.preview_animation_gpu.restores - restored_before);
    log_u64(LogLevel::info, "probe", "five_webp_scroll_new_decodes", r.media_gpu.vp8_reconstructions - decoded_before);
    log_u64(LogLevel::info, "probe", "five_webp_unique_cache_frames", r.preview_animation_gpu.cache.entry_count);
    log_u64(LogLevel::info, "probe", "five_webp_scroll_verified_pixels", 5ull * steps * animation.width * animation.height);
    u32 stationary_frames = 0;
    for (u32 index = 0; ok && index < animation.frame_count; ++index) {
        ok = d3d11_device_owner_wait(r.graphics, 1000) == WAIT_OBJECT_0;
        renderer_accept_begin_frame(&r);
        for (u32 i = 0; ok && i < 5; ++i) {
            AnimationFrameCacheResult restored{};
            ok = renderer_restore_animation_frame(&r, {keys[i], index, 1, 0, fingerprint}, &restored);
        }
        ok = ok && probe_present(&r, kDefaultTheme) && read_frame_hashes(false, index);
        if (ok) ++stationary_frames;
    }
    ok = ok && stationary_frames == animation.frame_count && r.media_gpu.vp8_reconstructions == decoded_before;
    log_u64(LogLevel::info, "probe", "five_webp_stationary_verified_frames", stationary_frames);
    if (readback) readback->Release();
    if (gpu_end) gpu_end->Release(); if (gpu_start) gpu_start->Release(); if (disjoint) disjoint->Release();
    log_write(ok ? LogLevel::info : LogLevel::error, "probe", ok ? "five_webp_scroll_pass" : "five_webp_scroll_failed");
    renderer_unbind_media_atlas(&r);
    r.media_atlas = old_primary;
    r.preview_media_atlas = old_preview;
    r.message_scene = old_scene;
    r.message_text = old_text;
    r.retained_presentation.valid = false;
    paint_stream_release(&paint);
    message_scene_release(&f.scene);
    const bool restored_size = probe_resize_surface(original_width, original_height);
    return ok && restored_size;
}
} // namespace maicord
