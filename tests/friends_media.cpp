#include "maicord/ui/friends_media.hpp"
#include "maicord/media/animation_timeline.hpp"
#include "maicord/render/media_atlas.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {
namespace {
struct Source {u32 count = 16384, calls = 0;};
bool resolve_row(void* opaque, u32 index, UiFriendsRow* output) noexcept {
    auto& source = *static_cast<Source*>(opaque);
    ++source.calls;
    if (index >= source.count) return false;
    *output = {index + 1ull, utf8_literal("Friend"), utf8_literal("friend.user"), {},
        UiPresence::online, UiFriendsKind::friend_user, false};
    return true;
}
struct Probe {
    MediaAtlas* atlas;
    AnimationTimeline* timeline;
    u32 requests = 0, force_requests = 0, stop_after = 0;
    u64 now = 1000;
    bool fail = false;
};
FriendsAvatarMediaCallbacks callbacks(Probe* probe) noexcept {
    return {probe,
        +[](void* opaque, MediaAssetKey key) noexcept {return media_atlas_find(static_cast<Probe*>(opaque)->atlas, key).ok();},
        +[](void* opaque, MediaAssetKey key) noexcept {auto& p = *static_cast<Probe*>(opaque);return animation_timeline_observe_visible(p.timeline, key, p.now);},
        +[](void* opaque, const FriendsAvatarMedia&, bool force, bool* stop) noexcept {
            auto& p = *static_cast<Probe*>(opaque);++p.requests;if (force) ++p.force_requests;
            *stop = p.stop_after && p.requests >= p.stop_after;return !p.fail;
        }};
}
}
void test_friends_media(void (*check)(bool, const char*) noexcept) noexcept {
    UiFriendsScene scene{};
    Source logical{};
    UiFriendsSource source{&logical, logical.count, 1, resolve_row};
    UserStore users{};
    UserRecord records[160]{};u32 order[160]{};u8 names[8192]{};
    check(user_store_initialize(&users, {records, 160}, {order, 160}, {names, sizeof(names)}) &&
        ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, {0, 0, 800, 600}),
        "friends media: real retained viewport and existing user store initialize without an image bank");
    struct Cleanup {UiFriendsScene* scene;~Cleanup() noexcept {ui_friends_scene_release(scene);}} cleanup{&scene};
    if (!scene.active) return;
    constexpr auto still = utf8_literal("0123456789abcdef0123456789abcdef");
    constexpr auto animated = utf8_literal("a_0123456789abcdef0123456789abcdef");
    for (u64 i = 1; i <= 160; ++i) {
        user_store_upsert(&users, i, utf8_literal("Friend"), utf8_literal("friend.user"));
        if (i != 3) user_store_set_avatar(&users, i, i == 2 ? animated : still);
    }
    auto* malformed = const_cast<UserRecord*>(user_store_find(users, 4));
    malformed->avatar_hash_bytes = 3;malformed->avatar_hash[0] = 'b';malformed->avatar_hash[1] = 'a';malformed->avatar_hash[2] = 'd';
    FriendsAvatarMedia first{}, second{};
    const u32 source_calls = logical.calls;
    check(friends_avatar_media_resolve(scene, users, 0, &first) && friends_avatar_media_resolve(scene, users, 1, &second) &&
        ui_rect_width(first.rectangle) == 32 && ui_rect_height(first.rectangle) == 32 &&
        first.key.id == 1 && (second.key.flags & media_asset_flag_animated) &&
        first.hash.data == user_store_find(users, 1)->avatar_hash && logical.calls == source_calls,
        "friends media: 32-DIP static/animated candidates borrow current hashes and retained rows without source re-resolution");
    FriendsAvatarMedia skipped{};
    check(!friends_avatar_media_resolve(scene, users, 2, &skipped) && !friends_avatar_media_resolve(scene, users, 3, &skipped),
        "friends media: default and malformed avatars keep the native placeholder and never request a CDN asset");

    MediaAtlas atlas{};MediaAtlasEntry entries[4]{};u32 buckets[8]{};GlyphAtlasEntry allocations[4]{};GlyphAtlasRow rows[1]{};
    AnimationTimeline timeline{};AnimationTimelineEntry animations[8]{};u32 animation_buckets[16]{};
    check(media_atlas_initialize(&atlas, 32, 32, 0, {entries, 4}, {buckets, 8}, {allocations, 4}, {rows, 1}) &&
        animation_timeline_initialize(&timeline, {animations, 8}, {animation_buckets, 16}), "friends media: bounded real atlas and animation timeline initialize");
    Probe probe{&atlas, &timeline};
    auto visited = friends_avatar_media_visit(scene, users, callbacks(&probe));
    check(visited.ok && visited.requests == probe.requests && probe.requests > 1 && probe.force_requests == 1 &&
        visited.visited <= kUiFriendsMaximumVisibleRows && logical.calls == source_calls,
        "friends media: only intersecting current viewport candidates enter the existing request/animation seam");
    const u32 full_requests = probe.requests;
    probe.requests = probe.force_requests = 0;probe.stop_after = 2;
    visited = friends_avatar_media_visit(scene, users, callbacks(&probe));
    check(visited.ok && visited.stopped && probe.requests == 2 && visited.visited < scene.count,
        "friends media: shared upload/admission budget stops the viewport scan before later candidates");
    probe.requests = probe.force_requests = 0;probe.stop_after = 0;probe.fail = true;
    check(!friends_avatar_media_visit(scene, users, callbacks(&probe)).ok && probe.requests == 1,
        "friends media: request callback failure propagates without visiting later rows");
    probe.fail = false;
    const auto resident = media_atlas_allocate(&atlas, first.key, 32, 32);
    probe.requests = probe.force_requests = 0;
    visited = friends_avatar_media_visit(scene, users, callbacks(&probe));
    check(resident.ok() && visited.ok && probe.requests + 1 == full_requests,
        "friends media: resident static avatar skips redundant decode while viewport peers continue");
    media_atlas_begin_visibility(&atlas);
    const auto replacement = media_atlas_allocate(&atlas, second.key, 32, 32);
    probe.requests = probe.force_requests = 0;
    visited = friends_avatar_media_visit(scene, users, callbacks(&probe));
    check(replacement.ok() && !media_atlas_validate(atlas, resident.handle) && visited.ok && probe.force_requests == 1,
        "friends media: real atlas eviction invalidates stale handles and a resident animation without a timeline requests restart");
    const auto published = animation_timeline_publish(&timeline, {second.key, 1000, 7, 2, 0, true}, 1000);
    probe.requests = probe.force_requests = 0;
    visited = friends_avatar_media_visit(scene, users, callbacks(&probe));
    check(published.ok() && visited.ok && !probe.force_requests && probe.requests + 1 == full_requests,
        "friends media: resident animation with current playback ownership avoids redundant decode");
    animation_timeline_suspend(&timeline, 1000);
    AnimationFrameAdvance advance[2]{};
    check(!animation_timeline_collect_due(&timeline, 100000, {advance, 2}).advances &&
        friends_avatar_media_visit(scene, users, callbacks(&probe)).ok,
        "friends media: existing reduced-motion suspension keeps resident avatar without scheduling a new animation frame");

    check(friends_avatar_media_current(scene, users, first.key), "friends media: current visible completion identity is admitted");
    user_store_set_avatar(&users, 1, utf8_literal("fedcba9876543210fedcba9876543210"));
    check(!friends_avatar_media_current(scene, users, first.key),
        "friends media: hash change rejects a late old-avatar completion before upload");
    ++source.revision;
    probe.requests = 0;
    check(!friends_avatar_media_current(scene, users, second.key) &&
        !friends_avatar_media_visit(scene, users, callbacks(&probe)).requests && !probe.requests,
        "friends media: stale source revision suppresses completion admission and requests until scene synchronization");
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, {0, 0, 800, 600}) &&
        ui_friends_scene_scroll(&scene, 5000) && !friends_avatar_media_current(scene, users, second.key),
        "friends media: current viewport scroll rejects late completion from a formerly visible friend");
    const auto bank = scene.storage.arena.base;
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, {0, 0, 800, 8192}) &&
        ui_friends_scene_visible_count(scene) <= kUiFriendsMaximumVisibleRows && sizeof(FriendsAvatarMedia) <= 80 &&
        scene.capacity < 800 && scene.storage.arena.base && bank,
        "friends media: maximum native viewport keeps retained state bounded independently of 16384 logical rows");
    scene.active = false;probe.requests = 0;
    check(!friends_avatar_media_current(scene, users, second.key) &&
        !friends_avatar_media_visit(scene, users, callbacks(&probe)).requests && !probe.requests,
        "friends media: hidden Friends has no requests and cannot admit a late avatar upload");
    scene.active = true;
    check(user_store_initialize(&users, {records, 160}, {order, 160}, {names, sizeof(names)}) &&
        !friends_avatar_media_current(scene, users, second.key),
        "friends media: user-cache eviction cannot resurrect a stale avatar identity");
}
} // namespace maicord
