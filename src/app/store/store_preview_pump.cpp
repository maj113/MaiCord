#include "store_preview.hpp"

#include "../plugins/catalog_service.hpp"
#include "../shell/state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/media/image/apng.hpp"
#include "maicord/render/d3d11/renderer.hpp"

namespace maicord::app_detail {

// A playing effect loop: the asset bytes stay while it plays, frames decode one at a time into a canvas-sized
// atlas entry. One loop at a time; it is gone the moment hover, focus or the page leave it.
struct StorePreviewLoop {
    u32 slot;
    MediaAssetKey key;
    u8 sha256[32];
    CatalogAsset asset;
    bool taken, begun;
    ApngImage image;
    ApngWorkspace* workspace;
    DeflateWorkspace* inflate;
    u8* scanlines;
    u32 scanline_bytes, frame;
    ApngFrame previous;
    u64 next_ms;
};

namespace {

constexpr u32 kLoopCanvasMaximumPixels = 1280u * 720u;
constexpr u64 kRetryMs = 20'000;

MediaAtlas* atlas() noexcept {
    return g_app.renderer.preview_media_atlas;
}

void forget(const MediaAssetKey& key) noexcept {
    if (atlas() && media_asset_key_valid(key))
        media_atlas_forget(atlas(), key);
}

void free_entry(StorePreviewEntry* e) noexcept {
    if (e->phase == 2)
        forget(e->key);
    reset_value(e);
}

void loop_stop(StorePreviews* p) noexcept {
    StorePreviewLoop* loop = p->loop;
    if (!loop)
        return;
    p->loop = nullptr;
    p->changes += loop->begun;
    forget(loop->key);
    if (loop->taken)
        application_catalog_asset_release(&loop->asset);
    if (loop->scanlines)
        VirtualFree(loop->scanlines, 0, MEM_RELEASE);
    if (loop->workspace)
        VirtualFree(loop->workspace, 0, MEM_RELEASE);
    deflate_workspace_free(loop->inflate);
    VirtualFree(loop, 0, MEM_RELEASE);
}

StorePreviewEntry* find(StorePreviews* p, const MediaAssetKey& key) noexcept {
    for (auto& e : p->entries)
        if (e.phase != 0 || e.retry_ms) {
            if (media_asset_key_equal(e.key, key))
                return &e;
        }
    return nullptr;
}

// A free slot, else the least recently seen one that is not on screen now.
StorePreviewEntry* acquire(StorePreviews* p) noexcept {
    StorePreviewEntry* oldest = nullptr;
    for (auto& e : p->entries) {
        if (e.phase == 0 && !e.retry_ms)
            return &e;
        if (e.touched != p->epoch && (!oldest || e.touched < oldest->touched))
            oldest = &e;
    }
    if (oldest)
        free_entry(oldest);
    return oldest;
}

// The still: upload what arrived. True when the entry became resident.
bool upload_still(StorePreviewEntry* e, ConstSlice<u8> png) noexcept {
    StorePreviewImage image{};
    if (!store_preview_decode(png, e->key.variant & 0x1fffffffu, &image))
        return false;
    const auto uploaded = renderer_upload_media_straight_rgba8(&g_app.renderer, e->key, static_cast<u16>(image.width),
                                                               static_cast<u16>(image.height), image.width * 4u,
                                                               {image.pixels, image.width * image.height * 4u});
    store_preview_free(&image);
    return uploaded.ok();
}

void pump_stills(StorePreviews* p, const StorePreviewPump& in, bool* more) noexcept {
    bool decoded = false;
    UiStoreMedia media{};
    u32 cursor = 0;
    while (ui_store_scene_next_media(*in.scene, &cursor, &media)) {
        CatalogBlob blob{};
        if (!store_preview_blob(media.slot, false, &blob))
            continue;
        const MediaAssetKey key = store_preview_key(blob.sha256, store_preview_pixels(ui_rect_width(media.rect), in.dpi), false);
        StorePreviewEntry* e = find(p, key);
        if (!e) {
            e = acquire(p);
            if (!e)
                continue;
            e->key = key;
            copy_bytes(e->sha256, blob.sha256, 32);
        }
        e->touched = p->epoch;
        if (e->phase == 2 && !media_atlas_peek(atlas(), e->key).ok()) {
            // Evicted under atlas pressure: the verified bytes are still on disk.
            e->phase = 0;
            e->retry_ms = 0;
        }
        if (e->phase == 0 && in.now_ms >= e->retry_ms) {
            e->retry_ms = 0;
            if (application_catalog_asset_request(CatalogAssetKind::preview, blob)) {
                e->phase = 1;
                e->retry_ms = in.now_ms + kRetryMs;
            } else
                e->retry_ms = in.now_ms + 1'000;
        } else if (e->phase == 1) {
            CatalogAsset asset{};
            if (decoded) {
                *more = true;
            } else if (application_catalog_asset_take(e->sha256, &asset)) {
                decoded = true;
                const bool ok = upload_still(e, {asset.bytes, asset.count});
                application_catalog_asset_release(&asset);
                e->phase = ok ? 2 : 3;
                p->changes += ok;
                e->retry_ms = ok ? 0 : in.now_ms + 60'000;
            } else if (in.now_ms >= e->retry_ms) {
                e->phase = 0; // the request was lost: ask again
                e->retry_ms = 0;
            } else
                *more = true; // on its way: look again shortly
        } else if (e->phase == 3 && in.now_ms >= e->retry_ms) {
            e->phase = 0;
            e->retry_ms = 0;
        }
    }
}

bool loop_fail(StorePreviews* p, u32 slot) noexcept {
    loop_stop(p);
    p->failed_loop_slot = slot;
    return false;
}

// Starts, fetches or advances the loop of `in.loop_slot`. Returns when its next frame is due, 0 for none.
u64 pump_loop(StorePreviews* p, const StorePreviewPump& in) noexcept {
    const u32 wanted = in.loop_slot;
    if (p->loop && p->loop->slot != wanted)
        loop_stop(p);
    if (!wanted || !atlas()) {
        p->failed_loop_slot = 0;
        return 0;
    }
    if (p->failed_loop_slot == wanted)
        return 0;
    if (!p->loop) {
        CatalogBlob blob{};
        if (!store_preview_blob(wanted, true, &blob) ||
            !application_catalog_asset_request(CatalogAssetKind::preview, blob)) {
            p->failed_loop_slot = wanted;
            return 0;
        }
        auto* loop = static_cast<StorePreviewLoop*>(commit_pages(sizeof(StorePreviewLoop)));
        if (!loop) {
            p->failed_loop_slot = wanted;
            return 0;
        }
        loop->slot = wanted;
        copy_bytes(loop->sha256, blob.sha256, 32);
        p->loop = loop;
        return in.now_ms + 50;
    }
    StorePreviewLoop& loop = *p->loop;
    if (!loop.taken) {
        if (!application_catalog_asset_take(loop.sha256, &loop.asset))
            return in.now_ms + 100;
        loop.taken = true;
        loop.workspace = static_cast<ApngWorkspace*>(commit_pages(sizeof(ApngWorkspace)));
        loop.inflate = deflate_workspace_allocate();
        const ConstSlice<u8> bytes{loop.asset.bytes, loop.asset.count};
        if (!loop.workspace || !loop.inflate || !apng_parse(bytes, &loop.image, loop.workspace).ok() ||
            loop.image.frame_count < 2 || !loop.image.canvas.width || !loop.image.canvas.height ||
            loop.image.canvas.width > 0xffffu || loop.image.canvas.height > 0xffffu ||
            loop.image.canvas.width * loop.image.canvas.height > kLoopCanvasMaximumPixels)
            return loop_fail(p, wanted);
        loop.scanline_bytes = png_decode_buffer_bytes(loop.image.canvas);
        loop.scanlines = loop.scanline_bytes ? static_cast<u8*>(commit_pages(loop.scanline_bytes)) : nullptr;
        if (!loop.scanlines)
            return loop_fail(p, wanted);
        // One identity for the loop's canvas: its width in pixels.
        loop.key = store_preview_key(loop.sha256, loop.image.canvas.width, true);
        loop.next_ms = in.now_ms;
    }
    if (in.now_ms < loop.next_ms)
        return loop.next_ms;
    const ConstSlice<u8> encoded{loop.asset.bytes, loop.asset.count};
    PngImage frame_image{};
    const ApngDecodeResult decoded = apng_decode_parsed_frame(encoded, loop.image, loop.frame, loop.inflate, &frame_image,
                                                              {loop.scanlines, loop.scanline_bytes});
    if (!decoded.ok())
        return loop_fail(p, wanted);
    const ConstSlice<u8> lines{loop.scanlines, frame_image.inflated_bytes};
    const u16 width = static_cast<u16>(loop.image.canvas.width), height = static_cast<u16>(loop.image.canvas.height);
    const auto uploaded = loop.frame == 0 ? renderer_begin_apng(&g_app.renderer, loop.key, width, height, decoded.frame, frame_image, lines)
                                          : renderer_advance_apng(&g_app.renderer, loop.key, loop.previous, decoded.frame, frame_image, lines);
    if (!uploaded.ok())
        return loop_fail(p, wanted);
    loop.previous = decoded.frame;
    p->changes += !loop.begun;
    loop.begun = true;
    const u64 duration = decoded.duration_us / 1000 ? decoded.duration_us / 1000 : 10;
    loop.next_ms = in.now_ms + duration;
    loop.frame = loop.frame + 1 < loop.image.frame_count ? loop.frame + 1 : 0;
    return loop.next_ms;
}

} // namespace

u64 store_previews_pump(StorePreviews* p, const StorePreviewPump& in) noexcept {
    if (!atlas() || !g_app.renderer.graphics.device)
        return 0;
    ++p->epoch;
    bool more = false;
    pump_stills(p, in, &more);
    const u64 due = pump_loop(p, in);
    if (more)
        return in.now_ms + 1;
    return due;
}

bool store_previews_key(const StorePreviews& p, u32 slot, u32 pixels, MediaAssetKey* key) noexcept {
    if (p.loop && p.loop->slot == slot && p.loop->begun) {
        *key = p.loop->key;
        return true;
    }
    CatalogBlob blob{};
    if (!store_preview_blob(slot, false, &blob))
        return false;
    *key = store_preview_key(blob.sha256, pixels, false);
    return true;
}

void store_previews_release(StorePreviews* p) noexcept {
    loop_stop(p);
    for (auto& e : p->entries)
        free_entry(&e);
    p->failed_loop_slot = 0;
}

} // namespace maicord::app_detail
