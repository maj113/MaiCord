// The renderer (src/render/d3d11, the D3D11Renderer logic over the gpu_* API) driven through scenes the way the UI probes
// drive it: on Linux over the OpenGL ES backend against an offscreen GpuSurface, on Windows (scenes_windows.cpp, same
// scenes) over a window. The frame is read back through gpu_readback from the surface's target texture.
//
// MAICORD_RENDER_DUMP=<prefix> writes every captured frame to <prefix>.<scene> as raw RGBA bytes.
#include "maicord/core/log.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/theme/theme.hpp"
#include "render/d3d11/renderer_draw.hpp"
#include "../gpu/capture_io.hpp"
#include "../integration/standalone_title_geometry.hpp"
#include "render/d3d11/renderer_caption.hpp"
#include "maicord/ui/shell/caption_bar.hpp"
#include "../support/test_output.hpp"

#if defined(_WIN32)
#include "../integration/probe_surface.hpp"
#endif
namespace maicord {
namespace {

constexpr u32 kWidth = 640, kHeight = 360;

u32 g_failures = 0;
char g_dump[512];
D3D11Renderer g_renderer;
UiScene g_shell_scene;
u8 g_pixels[kWidth * kHeight * 4];

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

bool read_frame(D3D11Renderer& renderer, u8* pixels) noexcept {
    GpuTexture* frame = gpu_surface_target_texture(&renderer.graphics.surface);
    return frame && gpu_ok(gpu_readback(renderer.graphics.device, renderer.graphics.context, frame,
                                        {0, 0, renderer.width, renderer.height + renderer.caption_height}, pixels,
                                        renderer.width * 4));
}

void dump(const char* scene) noexcept {
    if (!g_dump[0])
        return;
    char path[640];
    u32 at = 0;
    for (const char* c = g_dump; *c; ++c)
        path[at++] = *c;
    path[at++] = '.';
    for (const char* c = scene; *c; ++c)
        path[at++] = *c;
    path[at] = 0;
    check(capture_io::write_file(path, g_pixels, g_renderer.width * (g_renderer.height + g_renderer.caption_height) * 4),
          "a dump file is written");
}

// Pixels that differ from the first one: the scene drew something.
u32 ink_pixels(const u8* pixels, u32 count) noexcept {
    u32 ink = 0;
    for (u32 i = 1; i < count; ++i)
        ink += pixels[i * 4] != pixels[0] || pixels[i * 4 + 1] != pixels[1] || pixels[i * 4 + 2] != pixels[2];
    return ink;
}

bool same_pixels(const u8* a, const u8* b, u32 bytes) noexcept {
    return bytes_equal(a, b, bytes);
}

// The bootstrap title (the wordmark the shell shows before an account binds): drawn at three DPIs through the standalone
// path, as ui_probe's probe_bootstrap_title does.
void test_bootstrap_title() noexcept {
    auto& renderer = g_renderer;
    check(ui_scene_initialize_bootstrap(&g_shell_scene), "the bootstrap scene is made");
    const auto draw = [&]() noexcept {
        if (!standalone_title_layout(&g_shell_scene, static_cast<float>(renderer.width) * 96 / renderer.dpi,
                                     static_cast<float>(renderer.height) * 96 / renderer.dpi) ||
            !renderer_bind_ui_scene(&renderer, &g_shell_scene))
            return false;
        const float clear[]{0, 0, 0, 1};
        gpu_clear_rtv(renderer.graphics.context, gpu_surface_target(&renderer.graphics.surface), clear);
        return d3d11_renderer_draw_stream(&renderer, kDefaultTheme, true, true, nullptr, 10000);
    };
    static constexpr struct {
        u32 dpi;
        const char* name;
    } kDpis[]{{144, "bootstrap-144"}, {192, "bootstrap-192"}, {96, "bootstrap-96"}};
    for (const auto& step : kDpis) {
        renderer_set_dpi(&renderer, step.dpi);
        check(renderer.glyph_text.ready && draw() && renderer.glyph_text.title_quad_count, step.name);
        const u32 total = renderer.width * (renderer.height + renderer.caption_height);
        check(read_frame(renderer, g_pixels), "the frame is read back");
        check(ink_pixels(g_pixels, total) > total / 400, "the wordmark inks the frame");
        dump(step.name);
        // A second draw of the same scene builds nothing new and paints the same pixels.
        const u32 builds = renderer.glyph_text.cache_builds;
        static u8 again[kWidth * kHeight * 4];
        check(draw() && renderer.glyph_text.cache_builds == builds && read_frame(renderer, again) &&
                  same_pixels(g_pixels, again, total * 4),
              "a repeated draw is the same picture without cache builds");
    }
}

struct Pixel {
    u8 r, g, b;
};

Pixel pixel_at(const D3D11Renderer& renderer, const u8* pixels, u32 x, u32 y) noexcept {
    const u8* at = pixels + (static_cast<usize>(y) * renderer.width + x) * 4;
    // The frame is in the surface's own texel order.
    return gpu_surface_format(&renderer.graphics.surface) == GpuFormat::bgra8 ? Pixel{at[2], at[1], at[0]}
                                                                             : Pixel{at[0], at[1], at[2]};
}

// The colour a source-over blend of `top` onto opaque `base` leaves.
Color over(Color top, Color base) noexcept {
    const float a = top.alpha;
    return {top.red * a + base.red * (1 - a), top.green * a + base.green * (1 - a), top.blue * a + base.blue * (1 - a),
            1};
}

bool matches(Pixel pixel, Color color, i32 tolerance = 3) noexcept {
    const auto channel = [&](u8 value, float expected) noexcept {
        const i32 delta = static_cast<i32>(value) - static_cast<i32>(expected * 255.0f + .5f);
        return delta <= tolerance && -delta <= tolerance;
    };
    return channel(pixel.r, color.red) && channel(pixel.g, color.green) && channel(pixel.b, color.blue);
}

// The window caption through the real renderer, as caption_ui_probe checks it: the band above the content, the minimize
// stroke, the close and maximize fills (hover and pressed), the update badge's progress track and ready fill, and the
// title's ink. Every colour is the theme's own, so the checks hold on any font.
void test_caption() noexcept {
    auto& r = g_renderer;
    const Theme& theme = kDefaultTheme;
    const auto& tokens = theme.tokens.caption;
    renderer_set_dpi(&r, 96);
    const float scale = 1.0f;
    const u32 client_height = r.height + r.caption_height;
    const u32 caption = static_cast<u32>(ui_caption_bar_height(theme) * scale + .5f);
    renderer_set_caption_height(&r, caption);
    check(renderer_resize(&r, r.width, client_height - caption) && r.target_origin_y == caption &&
              r.height == client_height - caption,
          "the content moves down by the caption height");
    const auto draw = [&]() noexcept {
        if (!standalone_title_layout(&g_shell_scene, static_cast<float>(r.width), static_cast<float>(r.height)) ||
            !renderer_bind_ui_scene(&r, &g_shell_scene))
            return false;
        const float black[4]{0, 0, 0, 1};
        gpu_clear_rtv(r.graphics.context, gpu_surface_target(&r.graphics.surface), black);
        d3d11_renderer_caption_invalidate(&r);
        if (!d3d11_renderer_draw_stream(&r, theme, true, true, nullptr))
            return false;
        r.present_full = true;
        d3d11_renderer_caption_pending(&r, theme);
        const bool drawn = d3d11_renderer_caption_draw(&r, theme);
        r.present_full = false;
        return drawn;
    };
    const float width = static_cast<float>(r.width) / scale, height = static_cast<float>(caption) / scale;
    const UiCaptionBarLayout layout = ui_caption_bar_layout(theme, width, {}, height, scale);
    const auto px = [&](float dips) noexcept { return static_cast<u32>(dips * scale); };
    const UiRect minimize = layout.buttons[0];
    const u32 fill_y = caption / 2;
    const u32 glyph_x = px((minimize.left + minimize.right) * .5f);
    const Color background = over(tokens.background, theme.base_lowest);
    check(renderer_set_caption(&r, {}, utf8_literal("Native Lab")), "the caption title is set");
    struct Shot {
        const char* name;
        UiCaptionBarState state;
    };
    constexpr auto none = UiCaptionPart::none;
    const Shot shots[]{
        {"caption-focused", {none, none, true, false, UiCaptionUpdate::hidden, false, 0}},
        {"caption-unfocused", {none, none, false, false, UiCaptionUpdate::hidden, false, 0}},
        {"caption-close-hover", {UiCaptionPart::close, none, true, false, UiCaptionUpdate::hidden, false, 0}},
        {"caption-maximized-pressed",
         {UiCaptionPart::maximize, UiCaptionPart::maximize, true, true, UiCaptionUpdate::hidden, false, 0}},
        {"caption-update-downloading", {none, none, true, false, UiCaptionUpdate::downloading, false, .42f}},
        {"caption-update-ready-hover", {UiCaptionPart::update, none, true, false, UiCaptionUpdate::ready, false, 1}},
    };
    u32 title_ink[2]{};
    for (u32 shot = 0; shot < sizeof(shots) / sizeof(shots[0]); ++shot) {
        const UiCaptionBarState& state = shots[shot].state;
        check(renderer_set_caption(&r, state, utf8_literal("Native Lab")) && draw() && read_frame(r, g_pixels),
              shots[shot].name);
        dump(shots[shot].name);
        // The band's leading edge, clear of the title.
        check(matches(pixel_at(r, g_pixels, 1, caption - 1), background), "the band has the caption colour");
        // The minimize stroke: the strongest ink on its button's centre column.
        Pixel stroke = pixel_at(r, g_pixels, glyph_x, 0);
        i32 contrast = -1;
        for (u32 y = 0; y < caption; ++y) {
            const Pixel p = pixel_at(r, g_pixels, glyph_x, y);
            const i32 c = static_cast<i32>(p.r) + p.g + p.b;
            if (c > contrast)
                contrast = c, stroke = p;
        }
        const Color ink = state.active ? tokens.icon : over(tokens.icon_inactive, background);
        check(matches(stroke, ink, 8), "the minimize glyph has the icon colour");
        if (shot == 2)
            check(matches(pixel_at(r, g_pixels, px(layout.fills[2].left) + 2, fill_y), over(tokens.close_hover, background)),
                  "the close hover fill");
        if (shot == 3)
            check(matches(pixel_at(r, g_pixels, px(layout.fills[1].left) + 2, fill_y), over(tokens.pressed, background)),
                  "the maximize pressed fill");
        if (shot < 2) {
            for (u32 y = caption / 4; y < caption * 3 / 4; ++y)
                for (u32 x = px(layout.title.left); x < px(layout.title.right); ++x)
                    title_ink[shot] += matches(pixel_at(r, g_pixels, x, y), background, 12) ? 0u : 1u;
        }
        const UiCaptionBarLayout items = ui_caption_bar_layout(theme, width, state, height, scale);
        if (state.update == UiCaptionUpdate::downloading) {
            // Progress fill at 20% of the track, the bare track at 90%.
            const float icon_px = theme.scale.caption_icon;
            const float left = (items.update.left + items.update.right - icon_px) * .5f;
            const float bar = (items.update.top + items.update.bottom + icon_px) * .5f + theme.scale.space_1 + .5f;
            check(matches(pixel_at(r, g_pixels, px(left + icon_px * .2f), px(bar)), tokens.badge_progress, 12) &&
                      matches(pixel_at(r, g_pixels, px(left + icon_px * .9f), px(bar)),
                              over(tokens.badge_track, background), 12),
                  "the update progress track");
        }
        if (state.update == UiCaptionUpdate::ready)
            check(matches(pixel_at(r, g_pixels, px(items.update_fill.left) + 2, fill_y),
                          over(tokens.hover, over(tokens.badge_ready_fill, background)), 6),
                  "the update ready fill");
    }
    check(title_ink[0] >= 20 && title_ink[1] >= 20, "the title is drawn focused and unfocused");
    // Redrawing an unchanged caption builds nothing new.
    const u32 builds = d3d11_renderer_caption_builds(r);
    check(renderer_set_caption(&r, shots[0].state, utf8_literal("Native Lab")) && draw() &&
              renderer_set_caption(&r, shots[0].state, utf8_literal("Native Lab")) && draw() &&
              d3d11_renderer_caption_builds(r) <= builds + 2,
          "an unchanged caption is reused");
    // The caption goes away with its height.
    renderer_set_caption_height(&r, 0);
    check(renderer_resize(&r, r.width, client_height) && r.target_origin_y == 0, "the caption can be removed");
}

// The surface the scenes draw into: offscreen on Linux (the GLES backend draws to a frame texture of its own), a hidden
// popup window of the same client size on Windows (the swap chain needs one).
bool host_surface(GpuSurfaceDesc* desc) noexcept {
#if defined(_WIN32)
    const HWND window = CreateWindowExW(0, L"STATIC", L"maicord_render_scenes", WS_POPUP, 0, 0, static_cast<int>(kWidth),
                                        static_cast<int>(kHeight), nullptr, nullptr, nullptr, nullptr);
    if (!window)
        return false;
    *desc = probe_window_surface(window);
    return true;
#else
    *desc = GpuSurfaceDesc{nullptr, kWidth, kHeight, 96};
    return true;
#endif
}

void run() noexcept {
    capture_io::environment("MAICORD_RENDER_DUMP", g_dump, sizeof(g_dump));
    GpuSurfaceDesc surface{};
    check(host_surface(&surface), "the host surface is made");
    const bool initialized = renderer_initialize(&g_renderer, surface, true);
    check(initialized, "the renderer initializes on an offscreen surface");
    if (!initialized)
        return;
    check(g_renderer.width == kWidth && g_renderer.height == kHeight, "the renderer takes the surface size");
    test_bootstrap_title();
    test_caption();
    renderer_shutdown(&g_renderer);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_render_scenes_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_render_scenes");
}
