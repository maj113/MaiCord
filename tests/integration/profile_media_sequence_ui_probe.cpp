// Profile cards opened one after another from a guild member list, through the client's own path: the media pump
// scans the open card and requests its images from the account media module (here a fake that answers as the CDN
// and the decode worker would, at the size each URL asks for), uploads every frame into the identity atlas with its
// profile pools, and the renderer draws the card. For more cards than any pool has slots, mixing still, animated and
// per-server banners, every card's banner, effect layers and decoration must be requested, become resident with
// their pixels, and be drawn (the renderer finds the entry for the card's key and its pixels reach the screen).
// Nameplates scroll past in a 30-row member list window by window. Each card opens in the frame that uploads a burst
// of member-list rows (the sequence that hung an AMD GPU when the atlas's tile pool shrank under queued writes), and
// while it is open another burst backs and returns bands of the identity surface and the card's text is laid out
// again before the next frame: the frame presents and the banner's texels, read back whole, are unchanged.
#include "probe_present.hpp"
#include "probe_capture.hpp"
#include "../support/account_module_fake.hpp"
#include "../../src/app/media/pump.hpp"
#include "../../src/app/account/account_ui_runtime.hpp"
#include "../../src/app/account/profile.hpp"
#include "../../src/render/d3d11/renderer_draw.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/media/image/png.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {
using namespace app_detail;

constexpr u32 kCards = 12;      // more than the banner pool's 8 slots
constexpr u32 kEffectCards = 6; // 12 layers: more than the effect pool's 8 slots
constexpr u32 kRequestLog = 512;
constexpr u32 kTimelineEntries = 64;
constexpr u32 kLanes = 4;

// One decode lane of the fake account media module: the request it runs and the turn its frame is done.
struct Lane {
    MediaAssetKey key;
    char url[384];
    u16 url_bytes;
    u32 due;
    bool active;
};

struct SequenceFixture {
    MediaAtlas primary, preview;
    MediaAtlasEntry primary_entries[kMediaAtlasEntryCapacity];
    u32 primary_buckets[kMediaAtlasEntryCapacity * 2];
    GlyphAtlasEntry primary_allocations[kMediaAtlasEntryCapacity];
    GlyphAtlasRow primary_rows[kMediaAtlasRowCapacity];
    MediaAtlasEntry preview_entries[kPreviewMediaAtlasEntryCapacity];
    u32 preview_buckets[kPreviewMediaAtlasEntryCapacity * 2];
    GlyphAtlasEntry preview_allocations[kPreviewMediaAtlasEntryCapacity];
    GlyphAtlasRow preview_rows[kPreviewMediaAtlasRowCapacity];
    AnimationTimelineEntry timeline[kTimelineEntries];
    u32 timeline_buckets[kTimelineEntries * 2];
    AnimationVisualState visuals[kTimelineEntries];
    // One decoded picture at a time, as a worker lane holds it.
    u8 pixels[512 * 512 * 4];
    u8 scanlines[(450 * 4 + 1) * 880];
    PngImage png;
    AccountMediaFrame frame;
    bool frame_ready;
    MediaAssetKey requested[kRequestLog];
    u32 request_count;
    u32 failure_line;
    u64 skew_ms;
    Lane lanes[kLanes];
    u32 preferred, turn;
};
SequenceFixture* g_sequence;

// The colour every picture of `key` is drawn in: opaque, distinct per key.
u32 key_rgb(const MediaAssetKey& key) noexcept {
    const u64 mixed = (key.id ^ (key.id >> 29)) * 0x9E3779B97F4A7C15ull;
    return 0x404040u | static_cast<u32>(mixed >> 40);
}

bool contains(Utf8View text, const char* part) noexcept {
    u32 bytes = 0;
    while (part[bytes])
        ++bytes;
    for (u32 i = 0; i + bytes <= text.bytes; ++i)
        if (bytes_equal(text.data + i, part, bytes))
            return true;
    return false;
}

// The CDN's `size` (0 when the URL has none).
u32 url_size(Utf8View url) noexcept {
    for (u32 i = 0; i + 5 <= url.bytes; ++i)
        if (bytes_equal(url.data + i, "size=", 5)) {
            u32 value = 0;
            for (u32 j = i + 5; j < url.bytes && url.data[j] >= '0' && url.data[j] <= '9'; ++j)
                value = value * 10 + static_cast<u32>(url.data[j] - '0');
            return value;
        }
    return 0;
}

void fill_rgba(u32 rgb, u32 width, u32 height) noexcept {
    for (u32 i = 0; i < width * height; ++i) {
        g_sequence->pixels[i * 4] = static_cast<u8>(rgb >> 16);
        g_sequence->pixels[i * 4 + 1] = static_cast<u8>(rgb >> 8);
        g_sequence->pixels[i * 4 + 2] = static_cast<u8>(rgb);
        g_sequence->pixels[i * 4 + 3] = 255;
    }
}

// What the decode worker publishes for `url`: the CDN serves a banner `size` wide (5:2 at most, an animated one at
// most 512 wide), avatars, decorations and badges `size` square, and effect layers (no size) at 450x880.
void stage_frame(Utf8View url, const MediaAssetKey& key) noexcept {
    auto& f = *g_sequence;
    AccountMediaFrame& frame = f.frame;
    frame = {};
    frame.key = key;
    const u32 rgb = key_rgb(key);
    const u32 size = url_size(url);
    const bool animated = (key.flags & media_asset_flag_animated) != 0;
    // Avatars, decorations and badges `size` square; straight RGBA stays within 512 like the worker's.
    u32 width = minimum<u32>(size ? size : 64, 512), height = width;
    bool png = false;
    if (contains(url, "/banners/")) {
        // Banners as 1ba6a30 fetched them (600 still, 512 animated) whatever the URL asks: the slot pools then
        // hold their largest entries.
        width = animated ? 512u : 600u;
        height = width * 211 / 600;
        png = !animated;
    } else if (contains(url, "profile_effects")) {
        width = 450;
        height = 880;
        png = true;
    } else if (contains(url, "nameplates")) {
        width = 448;
        height = 84;
        png = true;
    }
    if (png) {
        PngImage& image = f.png;
        image = PngImage{};
        png_image_configure(&image, width, height, 8, 6);
        for (u32 y = 0; y < height; ++y) {
            u8* row = f.scanlines + y * image.row_stride;
            row[0] = 0;
            for (u32 x = 0; x < width; ++x) {
                row[1 + x * 4] = static_cast<u8>(rgb >> 16);
                row[2 + x * 4] = static_cast<u8>(rgb >> 8);
                row[3 + x * 4] = static_cast<u8>(rgb);
                row[4 + x * 4] = 255;
            }
        }
        frame.kind = AccountMediaFrameKind::png_scanlines;
        frame.image = &image;
        frame.bytes = {f.scanlines, image.row_stride * height};
        frame.row_pitch = image.row_stride;
    } else if (animated && contains(url, "/banners/")) {
        // The first frame of a two-frame animated WebP, composited by the worker.
        fill_rgba(rgb, width, height);
        frame.kind = AccountMediaFrameKind::animated_webp_rgba8;
        frame.bytes = {f.pixels, width * height * 4};
        frame.row_pitch = width * 4;
        frame.frame_count = 2;
        frame.duration_us = 200'000;
        frame.cache_generation = static_cast<u16>(1 + (key.id & 0x7FFF));
        frame.canvas_width = static_cast<u16>(width);
        frame.canvas_height = static_cast<u16>(height);
        frame.animated_webp_frame = {0, width * height * 4, 100, 0, 0, static_cast<u16>(width),
                                     static_cast<u16>(height), AnimatedWebpBlend::source,
                                     AnimatedWebpDispose::none, false, AnimatedWebpCodec::vp8l_lossless};
    } else {
        fill_rgba(rgb, width, height);
        frame.kind = AccountMediaFrameKind::straight_rgba8;
        frame.bytes = {f.pixels, width * height * 4};
        frame.row_pitch = width * 4;
    }
    frame.width = width;
    frame.height = height;
    f.frame_ready = true;
}

// The decode worker's lanes, as the account media module runs them: a request starts a fetch and decode on a free
// lane (queued, then pending), the lane holds its finished frame until the pump takes it (the next frame drain, or
// the request that finds it ready), and a lane holding another key's frame is busy for everyone else.
Lane* lane_of(const MediaAssetKey& key) noexcept {
    for (Lane& lane : g_sequence->lanes)
        if (lane.active && media_asset_key_equal(lane.key, key))
            return &lane;
    return nullptr;
}

AccountMediaRequestResult WINAPI fetch(void*, const AccountMediaAttachmentRequest* request, u64, u64) noexcept {
    auto& f = *g_sequence;
    AccountMediaRequestResult result{};
    if (Lane* lane = lane_of(request->key)) {
        result.state = lane->due > f.turn ? AccountMediaRequestState::pending : AccountMediaRequestState::ready;
        if (result.state == AccountMediaRequestState::ready)
            f.preferred = static_cast<u32>(lane - f.lanes);
        return result;
    }
    Lane* free = nullptr;
    for (Lane& lane : f.lanes)
        free = !free && !lane.active ? &lane : free;
    if (!free || request->signed_url.bytes > sizeof(free->url)) {
        result.state = AccountMediaRequestState::busy;
        return result;
    }
    if (f.request_count < kRequestLog)
        f.requested[f.request_count++] = request->key;
    *free = Lane{request->key, {}, static_cast<u16>(request->signed_url.bytes), f.turn + 1 + (f.request_count % 3), true};
    copy_bytes(free->url, request->signed_url.data, request->signed_url.bytes);
    result.state = AccountMediaRequestState::queued;
    return result;
}

AccountMediaRequestResult WINAPI busy(void*, MediaAssetKey, u64, u64) noexcept {
    AccountMediaRequestResult result{};
    result.state = AccountMediaRequestState::busy;
    return result;
}

AccountMediaRequestResult WINAPI busy_icon(void*, const AccountMediaGuildIconRequest*, u64, u64) noexcept {
    AccountMediaRequestResult result{};
    result.state = AccountMediaRequestState::busy;
    return result;
}

// A finished lane's frame, the preferred lane (the one a ready request named) first.
bool WINAPI take(void*, AccountMediaFrame* frame) noexcept {
    auto& f = *g_sequence;
    const u32 first = f.preferred < kLanes ? f.preferred : 0;
    f.preferred = kLanes;
    for (u32 offset = 0; offset < kLanes; ++offset) {
        Lane& lane = f.lanes[(first + offset) % kLanes];
        if (!lane.active || lane.due > f.turn)
            continue;
        stage_frame({lane.url, lane.url_bytes}, lane.key);
        lane.active = false;
        f.frame_ready = false;
        *frame = f.frame;
        return true;
    }
    return false;
}

bool requested(const MediaAssetKey& key) noexcept {
    for (u32 i = 0; i < g_sequence->request_count; ++i)
        if (media_asset_key_equal(g_sequence->requested[i], key))
            return true;
    return false;
}

// The client's identity and preview surfaces, empty, and both animation frame caches.
bool bind_client_atlases(D3D11Renderer& r) noexcept {
    auto& f = *g_sequence;
    renderer_unbind_media_atlas(&r);
    animation_frame_cache_gpu_release(&r.animation_gpu);
    animation_frame_cache_gpu_release(&r.preview_animation_gpu);
    return animation_frame_cache_gpu_initialize(&r.animation_gpu) &&
           animation_frame_cache_gpu_initialize(&r.preview_animation_gpu, true) &&
           media_atlas_initialize(&f.primary, kMediaAtlasExtent, kMediaAtlasPageRows, kMediaAtlasPadding,
                                  {f.primary_entries, kMediaAtlasEntryCapacity},
                                  {f.primary_buckets, kMediaAtlasEntryCapacity * 2},
                                  {f.primary_allocations, kMediaAtlasEntryCapacity},
                                  {f.primary_rows, kMediaAtlasRowCapacity}) &&
           media_atlas_configure_surface(&f.primary, MediaAtlasSurface::identity) &&
           media_atlas_initialize(&f.preview, kPreviewMediaAtlasExtent, kPreviewMediaAtlasPageRows, kMediaAtlasPadding,
                                  {f.preview_entries, kPreviewMediaAtlasEntryCapacity},
                                  {f.preview_buckets, kPreviewMediaAtlasEntryCapacity * 2},
                                  {f.preview_allocations, kPreviewMediaAtlasEntryCapacity},
                                  {f.preview_rows, kPreviewMediaAtlasRowCapacity}) &&
           media_atlas_configure_surface(&f.preview, MediaAtlasSurface::preview) &&
           renderer_bind_media_atlas(&r, &f.primary) && renderer_bind_preview_media_atlas(&r, &f.preview);
}

void rebase_renderer_views(D3D11Renderer& renderer) noexcept {
    renderer.composer_layout.selections = renderer.edit_selections;
    renderer.login_edit.selections = renderer.edit_selections;
    renderer.edit_selection_owner = &renderer.composer_layout;
    if (renderer.animation_gpu.cache.allocations.rows)
        renderer.animation_gpu.cache.allocations.rows = renderer.animation_gpu.rows;
    if (renderer.preview_animation_gpu.cache.allocations.rows)
        renderer.preview_animation_gpu.cache.allocations.rows = renderer.preview_animation_gpu.rows;
}

void take_renderer(Application& application, D3D11Renderer& renderer) noexcept {
    application.renderer = renderer;
    renderer = {};
    rebase_renderer_views(application.renderer);
}

void return_renderer(Application& application, D3D11Renderer& renderer) noexcept {
    renderer = application.renderer;
    application.renderer = {};
    rebase_renderer_views(renderer);
}

// The atlas texels of `key`'s whole entry, read back from the identity surface through a staging copy, are its colour.
bool resident_pixels(D3D11Renderer& r, const MediaAssetKey& key) noexcept {
    const MediaAtlasResult a = media_atlas_peek(r.media_atlas, key);
    ID3D11Texture2D* texture = r.media_gpu.surface ? r.media_gpu.surface->texture : nullptr;
    if (!a.ok() || !texture)
        return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Width = a.width;
    description.Height = a.height;
    description.BindFlags = description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(r.graphics.device->CreateTexture2D(&description, nullptr, &staging)))
        return false;
    const D3D11_BOX box{a.x, a.y, 0, u32{a.x} + a.width, u32{a.y} + a.height, 1};
    r.graphics.context1->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const bool mapped_ok = SUCCEEDED(r.graphics.context1->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    bool ok = mapped_ok;
    const u32 rgb = key_rgb(key);
    for (u32 y = 0; ok && y < a.height; ++y) {
        const u8* p = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch;
        for (u32 x = 0; ok && x < a.width; ++x, p += 4)
            ok = p[0] == static_cast<u8>(rgb >> 16) && p[1] == static_cast<u8>(rgb >> 8) &&
                 p[2] == static_cast<u8>(rgb) && p[3] == 255;
    }
    if (mapped_ok)
        r.graphics.context1->Unmap(staging, 0);
    staging->Release();
    return ok;
}

// The renderer queued a quad for `key`'s current entry in the frame it just built.
bool drawn(const D3D11Renderer& r, const MediaAssetKey& key) noexcept {
    const MediaAtlasResult a = media_atlas_peek(r.media_atlas, key);
    if (!a.ok())
        return false;
    for (u32 i = 0; i < r.media_quad_count; ++i)
        if (r.media_draw.handle_data[i].slot == a.handle.slot &&
            r.media_draw.handle_data[i].generation == a.handle.generation)
            return true;
    return false;
}

// The screen pixel at DIP point `at` shows `rgb` (within rounding and blending).
bool on_screen_rgb(D3D11Renderer& r, UiPoint at, u32 rgb) noexcept {
    if (!d3d11_renderer_draw_stream(&r, kDefaultTheme, true, true, nullptr))
        return false;
    ProbeTargetReadback readback;
    if (probe_target_readback(r, &readback) != 0)
        return false;
    const float scale = static_cast<float>(r.dpi) / 96.0f;
    const u32 x = static_cast<u32>(at.x * scale), y = r.caption_height + static_cast<u32>(at.y * scale);
    if (x >= readback.width || y >= readback.height)
        return false;
    const u8* p = static_cast<const u8*>(readback.mapped.pData) + y * readback.mapped.RowPitch + x * 4;
    // The target is BGRA.
    const float shown[3]{static_cast<float>(p[2]), static_cast<float>(p[1]), static_cast<float>(p[0])};
    const float colour[3]{static_cast<float>((rgb >> 16) & 255), static_cast<float>((rgb >> 8) & 255),
                          static_cast<float>(rgb & 255)};
    u32 brightest = 0;
    for (u32 c = 1; c < 3; ++c)
        brightest = colour[c] > colour[brightest] ? c : brightest;
    const float dim = shown[brightest] / colour[brightest];
    bool match = dim >= .95f && dim <= 1.02f;
    for (u32 c = 0; c < 3; ++c)
        match = match && shown[c] - dim * colour[c] <= 5.f && dim * colour[c] - shown[c] <= 5.f;
    return match;
}

// The screen pixel at DIP point `at` shows `key`'s colour, never the banner's fallback fill.
bool on_screen(D3D11Renderer& r, UiPoint at, const MediaAssetKey& key) noexcept {
    return on_screen_rgb(r, at, key_rgb(key));
}

// The first colour image_palette finds in a picture of one colour: the centre of its 5-bit cell.
u32 cell_centre(u32 rgb) noexcept {
    return (rgb & 0xF8F8F8u) | 0x040404u;
}

template <u32 N>
void set(ProfileString<N>* text, const char* value, u32 number) noexcept {
    text->bytes = 0;
    while (*value)
        text->data[text->bytes++] = *value++;
    char digits[8];
    u32 count = 0;
    do
        digits[count++] = static_cast<char>('0' + number % 10);
    while (number /= 10);
    while (count)
        text->data[text->bytes++] = digits[--count];
}

template <u32 N>
void set_url(ProfileString<N>* text, const char* value, u32 number, u32 layer) noexcept {
    text->bytes = 0;
    while (*value)
        text->data[text->bytes++] = *value++;
    text->data[text->bytes++] = static_cast<char>('a' + number % 26);
    text->data[text->bytes++] = static_cast<char>('a' + number / 26);
    text->data[text->bytes++] = static_cast<char>('0' + layer);
    copy_bytes(text->data + text->bytes, ".png", 4);
    text->bytes += 4;
}

// A banner picture (still, animated, per-server), or none: the accent colour alone, or neither (the avatar's colour).
enum class BannerKind : u8 { still, animated, server, accent, avatar };

struct CardResult {
    MediaAssetKey banner, avatar, decoration, effects[2];
    bool banner_requested, banner_resident, banner_drawn, banner_visible;
    // Without a picture: the banner shows the accent, or the avatar's colour.
    bool fill_visible;
    bool decoration_ok, effects_ok;
    // The burst under the open card backed bands of the identity surface, and returned bands it had backed.
    bool mapped, released;
};

// The client idles past the media trim delay: the renderer's maintenance trims every surface as the frame loop
// does once its deadline passes (the clock skips ahead; nothing else waits).
void idle(D3D11Renderer& renderer) noexcept {
    g_sequence->skew_ms += kMediaAtlasIdleTrimMs + 2'000;
    renderer_media_maintain(&renderer, clock_monotonic_milliseconds() + g_sequence->skew_ms);
}

MediaAssetKey profile_image(u64 id) noexcept {
    auto key = media_asset_embed(id, 1, 0, MediaEmbedResource::image, MediaAttachmentFormat::png, false,
                                 MediaResolutionTier::px512);
    key.kind = MediaAssetKind::profile_image;
    return key;
}

// The member list scrolled to new rows between cards: their avatars, decorations and nameplates arrive and take
// general rows and nameplate slots (nothing of them is drawn here, so all of it may give way).
bool member_list_rows(D3D11Renderer& renderer, u32 k) noexcept {
    bool ok = true;
    for (u32 i = 0; ok && i < 16; ++i) {
        const MediaAssetKey avatar = media_asset_user_avatar(200'000 + k * 16 + i, 1, false);
        fill_rgba(key_rgb(avatar), 56, 56);
        ok = renderer_upload_media_straight_rgba8(&renderer, avatar, 56, 56, 56 * 4,
                                                  {g_sequence->pixels, 56 * 56 * 4})
                 .ok();
        const MediaAssetKey decoration = profile_image(0x44454300'00000000ull + k * 16 + i);
        fill_rgba(key_rgb(decoration), 64, 64);
        ok = ok && renderer_upload_media_straight_rgba8(&renderer, decoration, 64, 64, 64 * 4,
                                                        {g_sequence->pixels, 64 * 64 * 4})
                       .ok();
    }
    for (u32 i = 0; ok && i < 12; ++i) {
        const MediaAssetKey nameplate = profile_image(0x4E504C00'00000000ull + k * 12 + i);
        ProfileString<384> url{};
        set_url(&url, "https://cdn.discordapp.com/assets/collectibles/nameplates/list", k * 12 + i, 0);
        stage_frame(url.view(), nameplate);
        g_sequence->frame_ready = false;
        ok = renderer_upload_png_scanlines(&renderer, nameplate, g_sequence->png,
                                           {g_sequence->scanlines, g_sequence->png.row_stride * 84})
                 .ok();
    }
    // Rows hovered on the way to a card: their animated decorations at the CDN's native 288 px, and the chat's
    // emoji and avatars at their many sizes.
    for (u32 i = 0; ok && i < 4; ++i) {
        const MediaAssetKey hovered = profile_image(0x48565200'00000000ull + k * 4 + i);
        fill_rgba(key_rgb(hovered), 288, 288);
        ok = renderer_upload_media_straight_rgba8(&renderer, hovered, 288, 288, 288 * 4,
                                                  {g_sequence->pixels, 288 * 288 * 4})
                 .ok();
    }
    static constexpr u16 kChatSizes[] = {22, 24, 32, 40, 48, 64, 80, 96, 128, 160};
    for (u32 i = 0; ok && i < 40; ++i) {
        const u16 size = kChatSizes[(k + i) % 10];
        const MediaAssetKey emoji = media_asset_custom_emoji(0x454D4F00'00000000ull + k * 40 + i, false);
        fill_rgba(key_rgb(emoji), size, size);
        ok = renderer_upload_media_straight_rgba8(&renderer, emoji, size, size, size * 4u,
                                                  {g_sequence->pixels, u32{size} * size * 4u})
                 .ok();
    }
    return ok;
}

// The client's loop turn (shell/main.cpp): the media pump (finished frames, then the visibility scan and its
// requests), the renderer's media maintenance, then the frame. The fake worker's lanes finish their decodes as
// turns pass.
bool run_frames(D3D11Renderer& renderer, Application& application, u32 frames) noexcept {
    bool ok = true;
    for (u32 frame = 0; ok && frame < frames; ++frame) {
        ++g_sequence->turn;
        take_renderer(application, renderer);
        ApplicationMediaPump pump{application};
        bool repaint = false;
        ok = pump.pump_visible_static_media(&repaint);
        return_renderer(application, renderer);
        renderer_media_maintain(&renderer, clock_monotonic_milliseconds() + g_sequence->skew_ms);
        ok = ok && probe_present(&renderer, kDefaultTheme);
    }
    return ok;
}

// One card as application_profile_open shows it: opened over the member list with what the member list knows
// (loading), its profile fetched and laid out again (banner, theme, effect), its pictures arriving over the
// following frames; drawn, then closed.
bool open_card(D3D11Renderer& renderer, Application& application, UiTextMeasureCache* cache,
               ChatStateStore* state, UserStore* users, u32 k, BannerKind kind, bool effect,
               CardResult* out) noexcept {
    // The member list's rows arrive in one burst and the card opens before the next frame: its text is laid out and
    // drawn in the frame that uploads them. The uploads back new bands of the identity surface, and bands go back
    // before that frame while their writes are still queued (shrinking the pool then hung an AMD GPU).
    if (!member_list_rows(renderer, k))
        return false;
    ApplicationProfile& profile = *application.profile;
    UserProfile& data = profile.data;
    reset_value(&data);
    reset_value(&profile.effect);
    data.user_id = 70'000 + k;
    data.guild_id = 100;
    set(&data.username, "member", k);
    set(&data.avatar, "avatar", k);
    set(&data.user_appearance.decoration, "decoration", k);
    ProfileScene& scene = profile.scene;
    scene.phase = ProfilePhase::loading;
    scene.measure = cache;
    scene.effect = &profile.effect;
    scene.effect_started_ms = 0;
    scene.media_dpi = static_cast<u16>(renderer.dpi);
    const UiRect viewport{0, 0, static_cast<float>(renderer.width) * 96 / renderer.dpi,
                          static_cast<float>(renderer.height) * 96 / renderer.dpi};
    // Rows of the member list down the right edge: each card opens beside its row.
    const float row = 60 + static_cast<float>(k % 8) * 44;
    if (!profile_scene_open(&scene, &data, state, users, 1, {viewport.right - 240, row, viewport.right, row + 44},
                            viewport))
        return false;
    // Settled: the entrance motion is over before the pump and the renderer look at it.
    scene.opened_ms = clock_monotonic_milliseconds() - 10'000;
    renderer.profile_scene = &scene;
    bool ok = profile_scene_layout(&scene, viewport, true) && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
              probe_present(&renderer, kDefaultTheme) && run_frames(renderer, application, 2);
    // The profile arrives: an accent colour (but for the avatar-coloured card) and the banner picture, if any.
    data.has_accent = kind != BannerKind::avatar;
    data.accent_color = 0x2040C0;
    if (kind == BannerKind::server)
        set(&data.guild_banner, "server", k);
    else if (kind == BannerKind::still || kind == BannerKind::animated)
        set(&data.banner, kind == BannerKind::animated ? "a_animated" : "still", k);
    if (effect) {
        data.effect_sku_id = 900 + k;
        profile.effect.sku = 900 + k;
        profile.effect.count = 2;
        for (u32 layer = 0; layer < 2; ++layer) {
            auto& l = profile.effect.layers[layer];
            set_url(&l.source, "https://cdn.discordapp.com/assets/profile_effects/layer", k, layer);
            l.duration_ms = 600'000;
            l.loop = true;
        }
    }
    scene.phase = ProfilePhase::ready;
    ok = ok && profile_scene_layout(&scene, viewport, true);
    for (u32 i = 0; i < scene.part_count; ++i) {
        ProfileMedia media{};
        if (!profile_media_resolve(scene, i, &media))
            continue;
        if (scene.parts[i].media == ProfileMediaKind::banner)
            out->banner = media.key;
        else if (scene.parts[i].media == ProfileMediaKind::avatar)
            out->avatar = media.key;
        else if (scene.parts[i].media == ProfileMediaKind::decoration)
            out->decoration = media.key;
    }
    ok = ok && run_frames(renderer, application, 8);
    // The effect starts once its layers are resident; draw it as it plays.
    if (effect && scene.effect_started_ms) {
        // As if its 500 ms start delay had passed: the host lays the playing card out again.
        scene.effect_started_ms = clock_monotonic_milliseconds() - 1;
        ++scene.revision;
        for (u32 layer = 0; layer < 2; ++layer) {
            ProfileMedia media{};
            if (profile_effect_media_resolve(scene, layer, clock_monotonic_milliseconds(), &media))
                out->effects[layer] = media.key;
        }
        ok = ok && probe_present(&renderer, kDefaultTheme);
    }
    // A card left open past the idle delay: the trim keeps what it shows.
    if (k % 4 == 3) {
        idle(renderer);
        ok = ok && probe_present(&renderer, kDefaultTheme);
    }
    // Rows scroll in under the open card: a burst of uploads backs and writes new bands of the identity surface, the
    // idle trim returns the bands of what nothing draws, and the card's text is laid out again before the frame. The
    // banner keeps its texels (below).
    const GpuPagedTexture* surface = renderer.media_gpu.surface;
    u8 bands[sizeof(surface->band_slot)];
    copy_bytes(bands, surface->band_slot, sizeof(bands));
    ok = ok && member_list_rows(renderer, kCards + kEffectCards + k);
    for (u32 band = 0; band < sizeof(bands); ++band)
        out->mapped |= surface->band_slot[band] && surface->band_slot[band] != bands[band];
    copy_bytes(bands, surface->band_slot, sizeof(bands));
    idle(renderer);
    for (u32 band = 0; band < sizeof(bands); ++band)
        out->released |= bands[band] && surface->band_slot[band] != bands[band];
    ok = ok && renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    out->banner_requested = media_asset_key_valid(out->banner) && requested(out->banner);
    out->banner_resident = ok && resident_pixels(renderer, out->banner);
    out->banner_drawn = ok && drawn(renderer, out->banner);
    const UiRect banner = scene.banner;
    // Right of the avatar, below the header buttons.
    const UiPoint inside{banner.left + ui_rect_width(banner) * .55f, banner.top + ui_rect_height(banner) * .7f};
    out->banner_visible = ok && on_screen(renderer, inside, out->banner);
    // Without a picture: the accent, else the colour of the still avatar picture the pump reads.
    ProfileMedia still{};
    const bool still_ok = profile_avatar_still_media(scene, ui_rect_width(scene.avatar), &still);
    const u32 fill = kind == BannerKind::accent ? data.accent_color : cell_centre(key_rgb(still.key));
    out->fill_visible = ok && (kind == BannerKind::accent || (kind == BannerKind::avatar && still_ok)) &&
                        on_screen_rgb(renderer, inside, fill);
    out->decoration_ok = media_asset_key_valid(out->decoration) && requested(out->decoration) &&
                         resident_pixels(renderer, out->decoration) && drawn(renderer, out->decoration);
    out->effects_ok = true;
    for (u32 layer = 0; effect && layer < 2; ++layer)
        out->effects_ok = out->effects_ok && media_asset_key_valid(out->effects[layer]) &&
                          requested(out->effects[layer]) && resident_pixels(renderer, out->effects[layer]) &&
                          drawn(renderer, out->effects[layer]);
    // Closed before the next card opens: one frame without it, and now and then a pause between cards.
    renderer.profile_scene = nullptr;
    profile_scene_release(&scene);
    ok = ok && renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    if (k % 3 == 2)
        idle(renderer);
    return ok;
}

void fail(u32 line) noexcept {
    if (!g_sequence->failure_line)
        g_sequence->failure_line = line;
}

// Nameplates of a 30-row member list, 12 rows on screen at a time, scrolled a row at a time: each window's rows
// are drawn (pinned) every frame and every row that scrolls in becomes resident in the nameplate pool.
bool nameplate_rows(D3D11Renderer& renderer) noexcept {
    constexpr u32 kRowCount = 30, kWindow = 12;
    MediaAssetKey keys[kRowCount]{};
    for (u32 i = 0; i < kRowCount; ++i) {
        ProfileString<384> url{};
        set_url(&url, "https://cdn.discordapp.com/assets/collectibles/nameplates/row", i, 0);
        keys[i] = media_asset_embed(0x4E414D45'00000000ull + i, 1, 0, MediaEmbedResource::image,
                                    MediaAttachmentFormat::png, false, MediaResolutionTier::px512);
        keys[i].kind = MediaAssetKind::profile_image;
    }
    bool ok = true;
    for (u32 top = 0; ok && top + kWindow <= kRowCount; ++top) {
        media_atlas_begin_visibility(renderer.media_atlas);
        for (u32 i = top; ok && i < top + kWindow; ++i) {
            if (media_atlas_find(renderer.media_atlas, keys[i]).ok())
                continue;
            ProfileString<384> url{};
            set_url(&url, "https://cdn.discordapp.com/assets/collectibles/nameplates/row", i, 0);
            stage_frame(url.view(), keys[i]);
            g_sequence->frame_ready = false;
            ok = renderer_upload_png_scanlines(&renderer, keys[i], g_sequence->png,
                                               {g_sequence->scanlines, g_sequence->png.row_stride * 84})
                     .ok();
        }
        for (u32 i = top; ok && i < top + kWindow; ++i)
            ok = media_atlas_find(renderer.media_atlas, keys[i]).ok() && resident_pixels(renderer, keys[i]);
    }
    return ok;
}

} // namespace

bool profile_media_sequence_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache* cache, ChatStateStore* state,
                                     UserStore* users) noexcept {
    g_sequence = static_cast<SequenceFixture*>(
        VirtualAlloc(nullptr, sizeof(SequenceFixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* application = static_cast<Application*>(
        VirtualAlloc(nullptr, sizeof(Application), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* profile = static_cast<ApplicationProfile*>(
        VirtualAlloc(nullptr, sizeof(ApplicationProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const auto release = [&]() noexcept {
        void* const blocks[]{g_sequence, application, ui, profile};
        for (void* block : blocks)
            if (block)
                VirtualFree(block, 0, MEM_RELEASE);
        g_sequence = nullptr;
    };
    if (!g_sequence || !application || !ui || !profile || !cache || !state || !users) {
        release();
        return false;
    }
    MediaAtlas* const old_primary = renderer.media_atlas;
    MediaAtlas* const old_preview = renderer.preview_media_atlas;
    const ProfileScene* const old_profile = renderer.profile_scene;
    auto& f = *g_sequence;
    static int instance;
    application->clock = clock_initialize();
    application->process_start = clock_now();
    application->account.instance = &instance;
    g_account_fake.media_request = busy;
    g_account_fake.media_guild_icon_request = busy_icon;
    g_account_fake.media_attachment_request = fetch;
    g_account_fake.media_take = take;
    application->account_ui = ui;
    application->profile = profile;
    application->animations.visuals = f.visuals;
    bool ok = animation_timeline_initialize(&application->animations.timeline, {f.timeline, kTimelineEntries},
                                            {f.timeline_buckets, kTimelineEntries * 2}) &&
              bind_client_atlases(renderer);
    if (!ok)
        fail(__LINE__);
    // Banners: still, animated and per-server in turn, more cards than the banner pool has slots.
    u32 banners_drawn = 0, mapped = 0, released = 0;
    for (u32 k = 0; ok && k < kCards; ++k) {
        CardResult card{};
        const auto kind = static_cast<BannerKind>(k % 3);
        ok = open_card(renderer, *application, cache, state, users, k, kind, false, &card);
        if (!ok)
            fail(__LINE__);
        const bool banner = card.banner_requested && card.banner_resident && card.banner_drawn && card.banner_visible;
        log_u64(banner ? LogLevel::info : LogLevel::error, "probe",
                banner ? "profile_sequence_banner_shown" : "profile_sequence_banner_missing",
                (u64{k} << 32) | (u64{card.banner_requested} << 3) | (u64{card.banner_resident} << 2) |
                    (u64{card.banner_drawn} << 1) | u64{card.banner_visible});
        banners_drawn += banner;
        mapped += card.mapped;
        released += card.released;
        if (!card.decoration_ok)
            fail(__LINE__);
    }
    // Bursts under open cards must have backed and returned bands, or the banners proved nothing about remaps.
    if (banners_drawn != kCards || mapped == 0 || released == 0)
        fail(__LINE__);
    log_u64(LogLevel::info, "probe", "profile_sequence_bursts_mapped_released", (u64{mapped} << 32) | released);
    // Effect layers and decorations: cards with a two-layer effect each, more layers than the effect pool's slots.
    for (u32 k = 0; ok && k < kEffectCards; ++k) {
        CardResult card{};
        ok = open_card(renderer, *application, cache, state, users, kCards + k, BannerKind::still, true, &card);
        if (!ok)
            fail(__LINE__);
        if (!card.effects_ok || !card.decoration_ok) {
            log_u64(LogLevel::error, "probe", "profile_sequence_effect_missing", kCards + k);
            fail(__LINE__);
        }
    }
    // Cards without a banner picture: the accent colour fills the banner, and without one the avatar's colour (its
    // dominant colour, read back once the avatar is resident). Their member-list rows follow every earlier burst's.
    for (u32 k = 0; ok && k < 2; ++k) {
        CardResult card{};
        const auto kind = k ? BannerKind::avatar : BannerKind::accent;
        ok = open_card(renderer, *application, cache, state, users, 2 * (kCards + kEffectCards) + k, kind, false,
                       &card);
        if (!ok)
            fail(__LINE__);
        log_u64(card.fill_visible ? LogLevel::info : LogLevel::error, "probe",
                card.fill_visible ? "profile_sequence_banner_fill_shown" : "profile_sequence_banner_fill_missing",
                static_cast<u64>(kind));
        if (!card.fill_visible || !card.decoration_ok)
            fail(__LINE__);
    }
    if (ok && !nameplate_rows(renderer))
        fail(__LINE__);
    const u32 failure = f.failure_line;
    if (failure)
        log_u64(LogLevel::error, "probe", "profile_sequence_failure_line", failure);
    renderer.profile_scene = const_cast<ProfileScene*>(old_profile);
    renderer_unbind_media_atlas(&renderer);
    if (old_primary)
        renderer_bind_media_atlas(&renderer, old_primary);
    if (old_preview)
        renderer_bind_preview_media_atlas(&renderer, old_preview);
    release();
    return ok && failure == 0;
}

} // namespace maicord
