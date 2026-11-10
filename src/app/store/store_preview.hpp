#pragma once

#include "maicord/app/plugins/catalog_index.hpp"
#include "maicord/media/core/asset_key.hpp"
#include "maicord/ui/store/store_scene.hpp"

// The pictures of the store tiles (store-ui.md 4.10 and 8.4): a catalog `previews/<sha>.png` reduced by an
// integer box factor to the tile's size on screen and uploaded as straight RGBA into the media atlas. The
// decode and the identity are pure; the host's pump (store_preview_pump.cpp) fetches, decodes lazily for the
// visible tiles only and forgets them when the page closes.
namespace maicord::app_detail {

// The pixel width a tile of `width_dip` needs at `dpi`, rounded up to 32 so a small resize keeps its picture.
u32 store_preview_pixels(float width_dip, u32 dpi) noexcept;
// The atlas identity of one picture at one size: the first eight bytes of its SHA-256 and the size. Loops (the
// canvas of a playing APNG) are a different identity from the still.
MediaAssetKey store_preview_key(const u8 (&sha256)[32], u32 pixels, bool loop) noexcept;

struct StorePreviewImage {
    u8* pixels; // width * height * 4, straight RGBA8, owned (store_preview_free)
    u32 width, height;
};
// Streams the rows of a (non-interlaced) PNG through an alpha-weighted box filter of the largest integer factor
// that keeps the picture at least `pixels` wide. False: not a PNG this client decodes, or out of memory.
bool store_preview_decode(ConstSlice<u8> png, u32 pixels, StorePreviewImage* out) noexcept;
void store_preview_free(StorePreviewImage*) noexcept;

} // namespace maicord::app_detail

// ---- The host's pump (store_preview_pump.cpp) ----------------------------------------------------------------
namespace maicord::app_detail {

inline constexpr u32 kStorePreviewEntries = 24;

struct StorePreviewEntry {
    MediaAssetKey key;
    u8 sha256[32];
    u8 phase;      // 0 free, 1 asked for, 2 resident, 3 failed
    u32 touched;   // the pump epoch it was last visible in
    u64 retry_ms;  // when a free or failed entry may ask again
};
struct StorePreviewLoop;
struct StorePreviews {
    StorePreviewEntry entries[kStorePreviewEntries];
    u32 epoch;
    u32 changes; // pictures that appeared or went away: the scene lays out again (the renderer queues what is resident)
    StorePreviewLoop* loop;
    u32 failed_loop_slot; // a loop that could not play is not asked for again until another slot is wanted
};

struct StorePreviewPump {
    const UiStoreScene* scene;
    u32 loop_slot; // the slot whose loop plays, 0 none (the host decides: hover, focus, an uninstalled effect)
    u32 dpi;
    u64 now_ms;
};
// The catalog blob behind a preview slot: its still, or (loop) the effect's APNG loop. False: none. The host's.
bool store_preview_blob(u32 slot, bool loop, CatalogBlob* out) noexcept;
// One host frame while a store page is shown: asks the catalog for the pictures of the visible tiles, decodes at
// most one that arrived, uploads it, and advances the playing loop. Returns the time the next frame is due (0:
// nothing animates), so the host schedules one frame, never a timer while nothing plays.
u64 store_previews_pump(StorePreviews*, const StorePreviewPump&) noexcept;
// The media identity to draw for `slot` at `pixels` wide: the loop's canvas while it plays, else the still.
bool store_previews_key(const StorePreviews&, u32 slot, u32 pixels, MediaAssetKey* key) noexcept;
// The page closed: every picture leaves the atlas and the loop's memory is freed.
void store_previews_release(StorePreviews*) noexcept;

} // namespace maicord::app_detail
