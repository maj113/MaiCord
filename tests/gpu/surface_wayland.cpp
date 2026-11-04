// A Wayland window showing the ui_quads scene through the GPU API's surface: the EGL window surface on the window's
// wl_egl_window, the offscreen frame the scene is drawn into, the damage blit and the compositor's frame callbacks
// as the frame clock. Every frame redraws the scene and a moving square but declares only the square's old and new
// rectangle as damaged, so what is on screen is right only when the blit and the buffer-age bookkeeping are.
//
// Runs against a compositor (opt-in like the window tests: tools/test_gpu_surface.sh starts headless sway, takes a
// grim screenshot and checks it against the scene). MAICORD_SURFACE_HOLD_MS (default 2500) is how long it presents.
// Exit status: 0 pass, 1 fail, 77 skipped (no compositor), as ctest's SKIP_RETURN_CODE expects.
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/platform/window/pump.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "maicord/platform/window/window.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../support/test_output.hpp"
#include "ui_quads_scene.hpp"

namespace maicord {
namespace {

constexpr u32 kClientWidthDips = 480;
constexpr u32 kClientHeightDips = 320;
constexpr float kSquare = 20.0f;

Window g_window;
GpuSurface g_surface;
u32 g_failures;
u32 g_frame_callbacks;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

void sink(void*, const WindowEvent&, WindowEventResult*) noexcept {}

void frame_listener(void*, u32) noexcept {
    ++g_frame_callbacks;
    if (g_surface.impl)
        gpu_surface_frame_callback(&g_surface);
}

u32 parse_milliseconds(const char* text, u32 fallback) noexcept {
    if (!text || !text[0])
        return fallback;
    u32 value = 0;
    for (; *text >= '0' && *text <= '9'; ++text)
        value = value * 10 + static_cast<u32>(*text - '0');
    return value;
}

[[noreturn]] void skip(const char* reason) noexcept {
    test_write_text("SKIP ");
    test_write_line(reason);
    test_process_exit(77);
}

struct Scene {
    GpuBuffer* instances;
    GpuBuffer* frame;
    GpuBuffer* background;
    GpuVertexShader* vertex_shader;
    GpuInputLayout* layout;
    GpuPixelShader* pixel_shader;
    GpuBlend* blend;
    GpuRaster* raster;
};

bool create_scene(GpuDevice* device, Scene* scene) noexcept {
    const GpuInputElement elements[] = {{"RECT", 0, 0}, {"COLOR", 0, 16}, {"COLOR", 1, 32}, {"TEXCOORD", 0, 48}};
    bool layout_failed = false;
    return gpu_ok(gpu_buffer_create(device,
                                    {(ui_quads::kQuadCount + 1) * sizeof(ui_quads::Quad), GpuUsage::dynamic,
                                     gpu_bind_vertex, false},
                                    nullptr, &scene->instances)) &&
           gpu_ok(gpu_buffer_create(device, {sizeof(ui_quads::FrameConstants), GpuUsage::default_, gpu_bind_constant, false},
                                    nullptr, &scene->frame)) &&
           gpu_ok(gpu_buffer_create(device,
                                    {sizeof(ui_quads::kBackground), GpuUsage::default_, gpu_bind_constant, false},
                                    &ui_quads::kBackground, &scene->background)) &&
           gpu_ok(gpu_vertex_shader_create(device, ShaderId::ui_vertex, {elements, 4}, &scene->vertex_shader,
                                           &scene->layout, &layout_failed)) &&
           gpu_ok(gpu_pixel_shader_create(device, ShaderId::ui_pixel, &scene->pixel_shader)) &&
           gpu_ok(gpu_blend_create(device,
                                   {true, GpuBlendFactor::one, GpuBlendFactor::inv_src_alpha, GpuBlendFactor::one,
                                    GpuBlendFactor::inv_src_alpha},
                                   &scene->blend)) &&
           gpu_ok(gpu_raster_create(device, &scene->raster));
}

void release_scene(Scene* scene) noexcept {
    gpu_release(scene->raster);
    gpu_release(scene->blend);
    gpu_release(scene->pixel_shader);
    gpu_release(scene->layout);
    gpu_release(scene->vertex_shader);
    gpu_release(scene->background);
    gpu_release(scene->frame);
    gpu_release(scene->instances);
}

// The scene plus the square at `square_x` DIPs, drawn into the surface's frame target.
void draw_frame(GpuContext* context, const Scene& scene, u32 width, u32 height, u32 dpi, float square_x) noexcept {
    ui_quads::Quad quads[ui_quads::kQuadCount + 1];
    copy_bytes(quads, ui_quads::kQuads, sizeof(ui_quads::kQuads));
    quads[ui_quads::kQuadCount] = {{square_x, 140, square_x + kSquare, 140 + kSquare}, {1, 1, 1, 1}, {0, 0, 0, 0}, {4, 0, 0, 0}};
    void* mapped = gpu_map(context, scene.instances, GpuMap::discard, nullptr);
    if (!mapped)
        return;
    copy_bytes(mapped, quads, sizeof(quads));
    gpu_unmap(context, scene.instances);
    const ui_quads::FrameConstants constants = ui_quads::frame_constants(width, height, dpi);
    gpu_update_buffer(context, scene.frame, &constants, sizeof(constants));

    GpuRtv* target = gpu_surface_target(&g_surface);
    gpu_clear_rtv(context, target, ui_quads::kClear);
    gpu_set_render_target(context, target);
    gpu_set_viewport(context, 0, 0, static_cast<float>(width), static_cast<float>(height));
    gpu_set_scissor(context, {0, 0, static_cast<i32>(width), static_cast<i32>(height)});
    gpu_set_raster(context, scene.raster);
    const float no_factor[4] = {0, 0, 0, 0};
    gpu_set_blend(context, scene.blend, no_factor);
    GpuBuffer* vertex_constants[1] = {scene.frame};
    GpuBuffer* pixel_constants[2] = {scene.frame, scene.background};
    gpu_set_constants(context, GpuStage::vertex, 0, 1, vertex_constants);
    gpu_set_constants(context, GpuStage::pixel, 0, 2, pixel_constants);
    gpu_set_input_layout(context, scene.layout);
    gpu_set_vertex_buffer(context, scene.instances, sizeof(ui_quads::Quad), 0);
    gpu_set_shader(context, scene.vertex_shader);
    gpu_set_shader(context, scene.pixel_shader);
    gpu_draw_instanced(context, 4, ui_quads::kQuadCount + 1, 0, 0);
}

void run() noexcept {
    const char* display_name = environment_get("WAYLAND_DISPLAY");
    if (!display_name || !display_name[0])
        skip("WAYLAND_DISPLAY is not set: no compositor to show the surface on");
    const u32 hold_ms = parse_milliseconds(environment_get("MAICORD_SURFACE_HOLD_MS"), 2500);

    WindowCreateInfo info{};
    info.title = Utf8View{"maicord gpu surface", 19};
    info.app_id = Utf8View{"maicord", 7};
    info.client_width_dips = kClientWidthDips;
    info.client_height_dips = kClientHeightDips;
    info.visible = true;
    if (!window_create(info, sink, nullptr, &g_window)) {
        skip("the window cannot be created (no usable compositor)");
    }
    const WindowPumpHooks hooks{};
    // Until the compositor has configured the window.
    WaylandSurfaceInfo surface_info{};
    for (u32 attempt = 0; attempt < 50; ++attempt) {
        window_wait(nullptr, 0, 100, 0);
        window_pump(256, 2000, hooks);
        if (window_wayland_info(g_window, &surface_info) && surface_info.width_px > 0)
            break;
    }
    check(window_wayland_info(g_window, &surface_info) && surface_info.display && surface_info.surface &&
              surface_info.width_px > 0 && surface_info.height_px > 0,
          "the window reports its surface");
    if (g_failures) {
        window_destroy(g_window);
        return;
    }
    window_wayland_use_external_buffers(g_window, true);
    gpu_device_set_native_display(surface_info.display);
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    check(gpu_ok(gpu_device_create(&device, &context)), "device creation on the compositor's display");
    Scene scene{};
    if (device) {
        test_write_text("device: ");
        test_write_line(gpu_device_description(device));
        test_write_metric("caps buffer_age=", gpu_caps(device).buffer_age);
        test_write_metric("caps surface_damage=", gpu_caps(device).surface_damage);
        check(create_scene(device, &scene), "scene resources");
        const GpuSurfaceDesc description{surface_info.surface, surface_info.width_px, surface_info.height_px, window_metrics(g_window).dpi};
        check(gpu_ok(gpu_surface_create(device, description, &g_surface)), "surface creation");
    }
    u32 width = surface_info.width_px, height = surface_info.height_px;
    u32 presented = 0, failed_presents = 0, resizes = 0;
    if (g_failures == 0) {
        window_set_frame_listener(g_window, frame_listener, nullptr);
        const Waitable clock = gpu_frame_clock_waitable(&g_surface);
        const u64 start = clock_monotonic_milliseconds();
        float previous_x = -1.0f;
        while (clock_monotonic_milliseconds() - start < hold_ms) {
            // A frame permit, a window event or a 50 ms watchdog (a hidden window gets no frame callbacks).
            const WindowWaitResult waited = window_wait(&clock, 1, 50, 0);
            window_pump(256, 2000, hooks);
            if (window_wayland_info(g_window, &surface_info) &&
                (surface_info.width_px != width || surface_info.height_px != height) && surface_info.width_px > 0) {
                width = surface_info.width_px;
                height = surface_info.height_px;
                check(gpu_ok(gpu_surface_resize(&g_surface, width, height)), "surface resize");
                previous_x = -1.0f;
                ++resizes;
            }
            if (!(waited.ready_mask & 1) && presented > 0)
                continue;
            const u32 dpi = window_metrics(g_window).dpi;
            const float x = 8.0f + static_cast<float>((presented * 7) % 380);
            draw_frame(context, scene, width, height, dpi, x);
            const float pixels = static_cast<float>(dpi) / 96.0f;
            GpuRect damage[2];
            u32 count = 0;
            if (previous_x >= 0.0f)
                damage[count++] = {static_cast<i32>(previous_x * pixels), static_cast<i32>(140 * pixels),
                                   static_cast<i32>((previous_x + kSquare) * pixels + 1),
                                   static_cast<i32>((140 + kSquare) * pixels + 1)};
            damage[count++] = {static_cast<i32>(x * pixels), static_cast<i32>(140 * pixels),
                               static_cast<i32>((x + kSquare) * pixels + 1), static_cast<i32>((140 + kSquare) * pixels + 1)};
            // The first frame, and the first after a resize, show everything.
            window_frame_callback_request(g_window);
            const GpuStatus status = gpu_surface_present(&g_surface, 0, previous_x >= 0.0f ? damage : nullptr,
                                                         previous_x >= 0.0f ? count : 0);
            if (gpu_ok(status))
                ++presented;
            else
                ++failed_presents;
            previous_x = x;
        }
    }
    test_write_metric("frames presented=", presented);
    test_write_metric("frame callbacks=", g_frame_callbacks);
    test_write_metric("resizes=", resizes);
    check(failed_presents == 0, "every present succeeds");
    check(presented >= 5, "frames are presented");
    check(g_frame_callbacks >= 2, "the compositor answers frame callbacks");
    gpu_surface_release(&g_surface);
    release_scene(&scene);
    gpu_device_destroy(&device, &context);
    window_destroy(g_window);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_surface_wayland_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_gpu_surface_wayland");
}
