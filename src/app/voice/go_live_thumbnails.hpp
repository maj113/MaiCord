#pragma once
#include "maicord/core/base.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/ui/voice/go_live_scene.hpp"

namespace maicord::app_detail {
// The screen share dialog's source thumbnails, one picture per source that is replaced in place every refresh. A
// refresh uploads the new picture under a new key and the dialog's scene copies keys when it is synchronized, so the
// previous key stays resident until go_live_thumbnails_synchronized: whatever key the scene holds is always drawable,
// and a source never goes blank between two refreshes (a failed upload keeps the previous picture too).
inline constexpr u64 kGoLiveThumbnailKeyBase = 0x474c'4956'0000'0000ull; // "GLIV"

struct GoLiveThumbnail {
    u64 handle;                 // The source; 0: the slot is free.
    MediaAssetKey key, retired; // The latest picture; the one before it while the scene may still show it.
    u16 width, height;
};
struct GoLiveThumbnails {
    GoLiveThumbnail slots[kUiGoLiveMaximumSources];
    u32 generation;
};
// Where pictures go: upload takes RGBA8 rows of width * 4 bytes (false: refused), forget drops a key.
struct GoLiveThumbnailAtlas {
    void* context;
    bool (*upload)(void* context, const MediaAssetKey& key, u32 width, u32 height, const u8* rgba) noexcept;
    void (*forget)(void* context, const MediaAssetKey& key) noexcept;
};

inline void go_live_thumbnail_forget(const GoLiveThumbnailAtlas& atlas, const MediaAssetKey& key) noexcept {
    if (media_asset_key_valid(key))
        atlas.forget(atlas.context, key);
}
inline const GoLiveThumbnail* go_live_thumbnail_find(const GoLiveThumbnails& set, u64 handle) noexcept {
    for (const auto& slot : set.slots)
        if (handle && slot.handle == handle)
            return &slot;
    return nullptr;
}
// A finished picture of `handle`. False (the previous picture stays) when it did not fit a slot or the upload failed.
inline bool go_live_thumbnail_publish(GoLiveThumbnails* set, const GoLiveThumbnailAtlas& atlas, u64 handle, u32 width,
                                      u32 height, const u8* rgba) noexcept {
    if (!handle || !width || !height || width > 0xFFFFu || height > 0xFFFFu)
        return false;
    GoLiveThumbnail* slot = nullptr;
    for (auto& candidate : set->slots)
        if (candidate.handle == handle)
            slot = &candidate;
    for (auto& candidate : set->slots)
        if (!slot && !candidate.handle)
            slot = &candidate;
    if (!slot)
        return false;
    const auto key = media_asset_local_draft_preview(kGoLiveThumbnailKeyBase | static_cast<u64>(slot - set->slots),
                                                     ++set->generation, MediaAttachmentFormat::png);
    if (!media_asset_key_valid(key) || !atlas.upload(atlas.context, key, width, height, rgba))
        return false;
    // The scene shows the oldest key not yet retired; a picture that was never shown goes at once.
    if (media_asset_key_valid(slot->retired))
        go_live_thumbnail_forget(atlas, slot->key);
    else
        slot->retired = slot->key;
    *slot = {handle, key, slot->retired, static_cast<u16>(width), static_cast<u16>(height)};
    return true;
}
// The scene took the keys of the latest pictures: the ones they replaced can go.
inline void go_live_thumbnails_synchronized(GoLiveThumbnails* set, const GoLiveThumbnailAtlas& atlas) noexcept {
    for (auto& slot : set->slots) {
        go_live_thumbnail_forget(atlas, slot.retired);
        slot.retired = {};
    }
}
// Sources that left the list lose their pictures.
inline void go_live_thumbnails_prune(GoLiveThumbnails* set, const GoLiveThumbnailAtlas& atlas, const u64* listed,
                                     u32 count) noexcept {
    for (auto& slot : set->slots) {
        bool found = !slot.handle;
        for (u32 i = 0; !found && i < count; ++i)
            found = listed[i] == slot.handle;
        if (found)
            continue;
        go_live_thumbnail_forget(atlas, slot.key);
        go_live_thumbnail_forget(atlas, slot.retired);
        reset_value(&slot);
    }
}
inline void go_live_thumbnails_release(GoLiveThumbnails* set, const GoLiveThumbnailAtlas& atlas) noexcept {
    go_live_thumbnails_prune(set, atlas, nullptr, 0);
}
} // namespace maicord::app_detail
