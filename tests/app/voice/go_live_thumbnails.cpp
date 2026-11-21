#include "../../../src/app/voice/go_live_thumbnails.hpp"

// The screen share dialog's thumbnails across refreshes: the key the dialog's scene holds stays resident until the
// scene took the next one, so a tile is never drawn without its picture between two refreshes.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
using namespace app_detail;

struct FakeAtlas {
    MediaAssetKey resident[2 * kUiGoLiveMaximumSources + 4];
    u32 count, uploads, forgets;
    bool refuse;
};
bool has(const FakeAtlas& atlas, const MediaAssetKey& key) noexcept {
    for (u32 i = 0; i < atlas.count; ++i)
        if (media_asset_key_equal(atlas.resident[i], key))
            return true;
    return false;
}
FakeAtlas g_fake;
} // namespace

// The atlas side of go_live_thumbnails.hpp, which go_live.cpp defines for the client.
bool app_detail::go_live_thumbnail_upload(const MediaAssetKey& key, u32, u32, const u8*) noexcept {
    if (g_fake.refuse || g_fake.count == sizeof(g_fake.resident) / sizeof(g_fake.resident[0]) || has(g_fake, key))
        return false;
    g_fake.resident[g_fake.count++] = key;
    ++g_fake.uploads;
    return true;
}
void app_detail::go_live_thumbnail_drop(const MediaAssetKey& key) noexcept {
    for (u32 i = 0; i < g_fake.count; ++i)
        if (media_asset_key_equal(g_fake.resident[i], key)) {
            g_fake.resident[i] = g_fake.resident[--g_fake.count];
            ++g_fake.forgets;
            return;
        }
}

namespace {
// What the dialog's scene does with them: copies the keys on update, draws the copies every frame.
MediaAssetKey scene_key(const GoLiveThumbnails& set, u64 handle) noexcept {
    const auto* thumbnail = go_live_thumbnail_find(set, handle);
    return thumbnail ? thumbnail->key : MediaAssetKey{};
}
const u8 kPixels[4 * 4 * 4]{};
} // namespace

void test_go_live_thumbnails(Check check) noexcept {
    static GoLiveThumbnails set;
    auto& fake = g_fake;
    reset_value(&set);
    reset_value(&fake);

    check(go_live_thumbnail_publish(&set, 7, 4, 4, kPixels) && go_live_thumbnail_find(set, 7) &&
              go_live_thumbnail_find(set, 7)->width == 4 && has(fake, scene_key(set, 7)) && !go_live_thumbnail_find(set, 8),
          "go live thumbnails: a picture is uploaded under its source's key");
    MediaAssetKey shown = scene_key(set, 7);
    go_live_thumbnails_synchronized(&set);

    // The reported flicker: the refresh's new picture lands, the scene still shows the old key until it is synchronized.
    bool never_empty = true;
    for (u32 refresh = 0; refresh < 200; ++refresh) {
        const bool published = go_live_thumbnail_publish(&set, 7, 4, 4, kPixels);
        never_empty = never_empty && published && has(fake, shown); // before the scene took the new key
        shown = scene_key(set, 7);
        never_empty = never_empty && has(fake, shown);
        go_live_thumbnails_synchronized(&set);
        never_empty = never_empty && has(fake, shown) && fake.count == 1;
    }
    check(never_empty, "go live thumbnails: the key the scene holds stays resident across 200 refreshes, one picture per source");

    // Two refreshes before the scene synchronized: what it shows survives, what it never saw goes.
    const MediaAssetKey seen = shown;
    go_live_thumbnail_publish(&set, 7, 4, 4, kPixels);
    go_live_thumbnail_publish(&set, 7, 4, 4, kPixels);
    check(has(fake, seen) && fake.count == 2 && has(fake, scene_key(set, 7)),
          "go live thumbnails: two refreshes between synchronizations keep the shown picture and the latest only");
    shown = scene_key(set, 7);
    go_live_thumbnails_synchronized(&set);
    check(fake.count == 1 && has(fake, shown) && !has(fake, seen), "go live thumbnails: the shown picture is replaced once the scene synchronized");

    // A refused upload keeps the previous picture.
    fake.refuse = true;
    check(!go_live_thumbnail_publish(&set, 7, 4, 4, kPixels) && media_asset_key_equal(scene_key(set, 7), shown) &&
              has(fake, shown),
          "go live thumbnails: a failed upload keeps the previous picture");
    fake.refuse = false;

    // Sources come and go.
    go_live_thumbnail_publish(&set, 8, 4, 4, kPixels);
    go_live_thumbnail_publish(&set, 9, 4, 4, kPixels);
    go_live_thumbnails_synchronized(&set);
    const MediaAssetKey kept = scene_key(set, 8), left = scene_key(set, 9);
    const u64 listed[]{7, 8};
    go_live_thumbnails_prune(&set, listed, 2);
    check(!go_live_thumbnail_find(set, 9) && !has(fake, left) && has(fake, kept) && has(fake, scene_key(set, 7)) &&
              fake.count == 2,
          "go live thumbnails: a source that left the list loses its picture, the others keep theirs");
    check(!go_live_thumbnail_publish(&set, 0, 4, 4, kPixels) && !go_live_thumbnail_publish(&set, 5, 0, 4, kPixels),
          "go live thumbnails: no source or no pixels is refused");

    // Every slot in use: sources 7 and 8 hold two of them, the other thirty are free, and a further source has none.
    u32 stored = 0;
    for (u64 handle = 100; handle < 100 + kUiGoLiveMaximumSources; ++handle)
        stored += go_live_thumbnail_publish(&set, handle, 4, 4, kPixels);
    check(stored == kUiGoLiveMaximumSources - 2 && !go_live_thumbnail_publish(&set, 999, 4, 4, kPixels),
          "go live thumbnails: a full set refuses another source");

    go_live_thumbnails_release(&set);
    check(fake.count == 0 && !go_live_thumbnail_find(set, 7),
          "go live thumbnails: closing the dialog forgets every picture");
}
} // namespace maicord
