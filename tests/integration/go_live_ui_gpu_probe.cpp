// The self-preview's GPU cost in the call view while streaming (MAICORD_UI_GO_LIVE_GPU_PROBE=1 on the UI probe, run by
// hand; never in the suite): the renderer at 2560x1377 and 125 % (a maximized window on a 1440p screen) shows the call
// with your own 1440p stream focused, docked beside the sidebar and then in the call's full screen, and takes a new
// encoder picture 60 times a second the way the call view does (renderer_present_video_frame into the 2576-wide
// preview atlas, then a present of the damage). D3D11 timestamp queries time the conversion and the paint with its
// Present; the presents' kinds, draw calls and repainted pixels are counted. Prints numbers only.
#include "probe_present.hpp"
#include "maicord/app/voice/call_video_presenter.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/render/media/atlas.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/ui/voice/call_scene.hpp"
#include "nv12_test_picture.hpp"

namespace maicord {
namespace {
constexpr u32 kWidth = 2560, kHeight = 1440, kEntries = 64, kRows = 64;
constexpr u64 kSelf = (1ull << 63) | 77;
constexpr u32 kSsrc = 0x7fff'fff1u;

struct Atlases {
    MediaAtlas identity, preview;
    MediaAtlasEntry identity_entries[kEntries], preview_entries[kEntries];
    u32 identity_buckets[kEntries * 2], preview_buckets[kEntries * 2];
    GlyphAtlasEntry identity_allocations[kEntries], preview_allocations[kEntries];
    GlyphAtlasRow identity_rows[kRows], preview_rows[kRows];
};

// The client's two media surfaces (ui_runtime.cpp) in place of the probe's small ones.
bool bind_client_atlases(D3D11Renderer& r, Atlases& a) noexcept {
    renderer_unbind_media_atlas(&r);
    return media_atlas_initialize(&a.identity, kMediaAtlasExtent, kMediaAtlasPageRows, kMediaAtlasPadding,
                                  {a.identity_entries, kEntries}, {a.identity_buckets, kEntries * 2},
                                  {a.identity_allocations, kEntries}, {a.identity_rows, kRows}) &&
           media_atlas_configure_surface(&a.identity, MediaAtlasSurface::identity) &&
           media_atlas_initialize(&a.preview, kPreviewMediaAtlasExtent, kPreviewMediaAtlasPageRows, kMediaAtlasPadding,
                                  {a.preview_entries, kEntries}, {a.preview_buckets, kEntries * 2},
                                  {a.preview_allocations, kEntries}, {a.preview_rows, kRows}) &&
           media_atlas_configure_surface(&a.preview, MediaAtlasSurface::preview) &&
           renderer_bind_media_atlas(&r, &a.identity) && renderer_bind_preview_media_atlas(&r, &a.preview);
}

// Two NV12 pictures (a luma ramp each way under one tint), so every picture differs from the one before.
bool make_pictures(ID3D11Device* device, Nv12TestPicture (&pictures)[2]) noexcept {
    auto* pixels = static_cast<u8*>(pages_allocate(kWidth * kHeight * 3 / 2));
    bool ok = pixels != nullptr;
    for (u32 n = 0; ok && n < 2; ++n) {
        for (u32 y = 0; y < kHeight; ++y)
            for (u32 x = 0; x < kWidth; ++x)
                pixels[y * kWidth + x] = static_cast<u8>(40 + (n ? x : kWidth - 1 - x) * 160 / kWidth);
        for (u32 i = 0; i < kWidth * kHeight / 2; i += 2) {
            pixels[kWidth * kHeight + i] = 110;
            pixels[kWidth * kHeight + i + 1] = 150;
        }
        ok = pictures[n].create(device, kWidth, kHeight, pixels);
    }
    if (pixels)
        pages_release(pixels);
    return ok;
}

struct Timing {
    u64 convert_us, paint_us, maximum_paint_us;
    u32 frames, timed;
};

// GPU microseconds between two timestamps of a finished disjoint query (spins until the GPU is done).
bool wait_times(ID3D11DeviceContext* c, ID3D11Query* disjoint, ID3D11Query* const (&stamps)[3], u64* convert,
                u64* paint) noexcept {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
    u64 t[3]{};
    while (c->GetData(disjoint, &d, sizeof(d), 0) == S_FALSE) {
    }
    for (u32 i = 0; i < 3; ++i)
        while (c->GetData(stamps[i], &t[i], 8, 0) == S_FALSE) {
        }
    if (d.Disjoint || !d.Frequency)
        return false;
    *convert = (t[1] - t[0]) * 1'000'000 / d.Frequency;
    *paint = (t[2] - t[1]) * 1'000'000 / d.Frequency;
    return true;
}

bool run_layout(D3D11Renderer& r, const Theme& theme, UiCallScene& scene, const Nv12TestPicture (&pictures)[2],
                u32 count, const char* label) noexcept {
    ID3D11Device* device = r.graphics.device;
    ID3D11DeviceContext* c = r.graphics.context;
    D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp_desc{D3D11_QUERY_TIMESTAMP, 0};
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* stamps[3]{};
    bool ok = SUCCEEDED(device->CreateQuery(&disjoint_desc, &disjoint));
    for (auto*& q : stamps)
        ok = ok && SUCCEEDED(device->CreateQuery(&stamp_desc, &q));
    const auto* tile = ui_call_scene_find_tile(scene, kSelf);
    const float scale = static_cast<float>(r.dpi) / 96.f;
    const u32 tile_pixels =
        tile ? static_cast<u32>(maximum(ui_rect_width(tile->target), ui_rect_height(tile->target)) * scale + .5f) : 0;
    const u32 extent = call_video_reduction_extent(kWidth, kHeight, tile_pixels);
    const u32 full_before = r.full_present_draws, partial_before = r.partial_present_draws;
    const u64 pixels_before = r.partial_present_pixels;
    Timing timing{};
    u64 draw_calls = 0;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const Clock clock = clock_initialize();
    const i64 start = clock_now();
    for (u32 i = 0; ok && i < count; ++i) {
        const u64 due = u64{i} * 1'000'000 / 60, now = clock_microseconds(clock, start, clock_now());
        if (due > now) {
            LARGE_INTEGER wait{};
            wait.QuadPart = -static_cast<LONGLONG>((due - now) * 10);
            SetWaitableTimer(timer, &wait, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
        c->Begin(disjoint);
        c->End(stamps[0]);
        ok = renderer_present_video_frame(&r, media_asset_call_video(kSelf, kSsrc), pictures[i & 1].frame(kWidth, kHeight),
                                          extent)
                 .ok();
        c->End(stamps[1]);
        ok = ok && probe_present(&r, theme);
        c->End(stamps[2]);
        c->End(disjoint);
        draw_calls += r.draw_calls_last_frame;
        u64 convert = 0, paint = 0;
        if (ok && wait_times(c, disjoint, stamps, &convert, &paint)) {
            timing.convert_us += convert;
            timing.paint_us += paint;
            timing.maximum_paint_us = maximum(timing.maximum_paint_us, paint);
            ++timing.timed;
        }
        ++timing.frames;
    }
    CloseHandle(timer);
    com_release(disjoint);
    for (auto* q : stamps)
        com_release(q);
    const u32 timed = maximum(timing.timed, 1u);
    log_write(LogLevel::info, "probe", label);
    log_u64(LogLevel::info, "probe", "tile_pixels", tile_pixels);
    log_u64(LogLevel::info, "probe", "reduction_extent", extent);
    log_u64(LogLevel::info, "probe", "pictures", timing.frames);
    log_u64(LogLevel::info, "probe", "convert_gpu_us_mean", timing.convert_us / timed);
    log_u64(LogLevel::info, "probe", "paint_present_gpu_us_mean", timing.paint_us / timed);
    log_u64(LogLevel::info, "probe", "paint_present_gpu_us_max", timing.maximum_paint_us);
    log_u64(LogLevel::info, "probe", "draw_calls_per_picture", draw_calls / maximum(timing.frames, 1u));
    log_u64(LogLevel::info, "probe", "full_presents", r.full_present_draws - full_before);
    log_u64(LogLevel::info, "probe", "partial_presents", r.partial_present_draws - partial_before);
    log_u64(LogLevel::info, "probe", "partial_pixels_per_picture",
            (r.partial_present_pixels - pixels_before) / maximum(timing.frames, 1u));
    return ok;
}
} // namespace

bool go_live_ui_gpu_probe(D3D11Renderer& renderer, u32 count) noexcept {
    static Atlases atlases;
    static UiCallScene scene;
    Nv12TestPicture pictures[2];
    renderer_set_dpi(&renderer, 120);
    bool ok = probe_resize_surface(2560, 1377) && bind_client_atlases(renderer, atlases) &&
              make_pictures(renderer.graphics.device, pictures);
    Theme theme = kDefaultTheme;
    const UiRect window{0, 0, renderer.width * 96.f / renderer.dpi, renderer.height * 96.f / renderer.dpi};
    const UiRect docked{minimum(312.f, window.right * .3f), 0, window.right, window.bottom};
    UiCallParticipant people[2]{};
    people[0].user_id = kSelf;
    people[0].name = utf8_literal("Mai");
    people[0].self = true;
    people[0].stream = UiCallStream::live;
    people[0].viewers = 1;
    people[0].video_ssrc = kSsrc;
    people[0].video_width = kWidth;
    people[0].video_height = kHeight;
    people[1].user_id = 12;
    people[1].name = utf8_literal("Bo");
    UiCallContent content{};
    content.title = utf8_literal("Lounge");
    content.participants = {people, 2};
    content.connected = true;
    content.share = UiCallShare::live;
    content.gap = theme.scale.space_4;
    content.focus_user = kSelf;
    const auto show = [&](const UiRect& viewport) noexcept {
        renderer.call_scene = &scene;
        renderer.retained_presentation.valid = false;
        return ui_call_scene_update(&scene, content, viewport, 0, true) && renderer_rebuild_content_text(&renderer, theme) &&
               renderer_present_video_frame(&renderer, media_asset_call_video(kSelf, kSsrc),
                                            pictures[0].frame(kWidth, kHeight), 0)
                   .ok() &&
               probe_present(&renderer, theme);
    };
    ok = ok && show(docked) && run_layout(renderer, theme, scene, pictures, count, "go_live_ui_docked_focus");
    content.fullscreen = true;
    ok = ok && show(window) && run_layout(renderer, theme, scene, pictures, count, "go_live_ui_call_fullscreen");
    renderer.call_scene = nullptr;
    ui_call_scene_release(&scene);
    for (auto& picture : pictures)
        picture.release();
    log_write(ok ? LogLevel::info : LogLevel::error, "probe", ok ? "go_live_ui_gpu_passed" : "go_live_ui_gpu_failed");
    return ok;
}
} // namespace maicord
