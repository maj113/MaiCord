// Draws the ui_quads scene (ui_quads_scene.hpp) with the GLSL translation of the ui shaders through the GPU API and
// compares it with tests/gpu/golden/ui_quads.png, which the Direct3D 11 side (ui_quads_windows.cpp) captured on
// Windows. MAICORD_GPU_QUAD_DUMP=<file> also writes this render as raw RGBA bytes (tools/update_gpu_goldens.py
// converts them), for inspecting a difference.
#include "maicord/core/log.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../support/test_output.hpp"
#include "ui_quads_scene.hpp"

namespace maicord {
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
alignas(16) constexpr u8 kGolden[] = {
#embed "golden/ui_quads.png"
};
#pragma clang diagnostic pop

u8 g_pixels[ui_quads::kWidth * ui_quads::kHeight * 4];
u8 g_golden[ui_quads::kWidth * ui_quads::kHeight * 4];

u32 g_failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

void dump(const char* path) noexcept {
    const long file = sys_open(path, kOpenWriteOnly | kOpenCreate | kOpenTruncate, 0644);
    if (file < 0)
        return;
    sys_write(static_cast<int>(file), g_pixels, sizeof(g_pixels));
    sys_close(static_cast<int>(file));
}

void run() noexcept {
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        check(false, "device creation");
        return;
    }
    test_write_text("device: ");
    test_write_line(gpu_device_description(device));

    GpuTexture* target = nullptr;
    GpuRtv* view = nullptr;
    check(gpu_ok(gpu_texture_create(device,
                                    {ui_quads::kWidth, ui_quads::kHeight, GpuFormat::rgba8, GpuUsage::default_,
                                     gpu_bind_srv | gpu_bind_rtv},
                                    nullptr, &target)) &&
              gpu_ok(gpu_rtv_create(device, target, &view)),
          "render target");

    GpuBuffer* instances = nullptr;
    check(gpu_ok(gpu_buffer_create(device, {sizeof(ui_quads::kQuads), GpuUsage::dynamic, gpu_bind_vertex, false}, nullptr,
                                   &instances)),
          "instance buffer");
    GpuBuffer* frame = nullptr;
    const ui_quads::FrameConstants frame_constants = ui_quads::frame_constants();
    check(gpu_ok(gpu_buffer_create(device, {sizeof(frame_constants), GpuUsage::default_, gpu_bind_constant, false},
                                   &frame_constants, &frame)),
          "frame constants");
    GpuBuffer* background = nullptr;
    check(gpu_ok(gpu_buffer_create(device,
                                   {sizeof(ui_quads::kBackground), GpuUsage::default_, gpu_bind_constant, false},
                                   &ui_quads::kBackground, &background)),
          "background constants");

    // The elements of D3D11DrawPipeline's UI layout (draw_pipeline.cpp).
    const GpuInputElement elements[] = {{"RECT", 0, 0}, {"COLOR", 0, 16}, {"COLOR", 1, 32}, {"TEXCOORD", 0, 48}};
    GpuVertexShader* vertex_shader = nullptr;
    GpuInputLayout* layout = nullptr;
    bool layout_failed = false;
    check(gpu_ok(gpu_vertex_shader_create(device, ShaderId::ui_vertex, {elements, 4}, &vertex_shader, &layout,
                                          &layout_failed)),
          "ui vertex shader");
    GpuPixelShader* pixel_shader = nullptr;
    check(gpu_ok(gpu_pixel_shader_create(device, ShaderId::ui_pixel, &pixel_shader)), "ui pixel shader");
    GpuBlend* blend = nullptr;
    check(gpu_ok(gpu_blend_create(device,
                                  {true, GpuBlendFactor::one, GpuBlendFactor::inv_src_alpha, GpuBlendFactor::one,
                                   GpuBlendFactor::inv_src_alpha},
                                  &blend)),
          "premultiplied blend");
    GpuRaster* raster = nullptr;
    check(gpu_ok(gpu_raster_create(device, &raster)), "rasterizer");

    if (g_failures == 0) {
        void* mapped = gpu_map(context, instances, GpuMap::discard, nullptr);
        check(mapped != nullptr, "instance map");
        if (mapped) {
            copy_bytes(mapped, ui_quads::kQuads, sizeof(ui_quads::kQuads));
            gpu_unmap(context, instances);
        }
        gpu_clear_rtv(context, view, ui_quads::kClear);
        gpu_set_render_target(context, view);
        gpu_set_viewport(context, 0, 0, static_cast<float>(ui_quads::kWidth), static_cast<float>(ui_quads::kHeight));
        gpu_set_scissor(context, {0, 0, static_cast<i32>(ui_quads::kWidth), static_cast<i32>(ui_quads::kHeight)});
        gpu_set_raster(context, raster);
        const float no_factor[4] = {0, 0, 0, 0};
        gpu_set_blend(context, blend, no_factor);
        GpuBuffer* vertex_constants[1] = {frame};
        GpuBuffer* pixel_constants[2] = {frame, background};
        gpu_set_constants(context, GpuStage::vertex, 0, 1, vertex_constants);
        gpu_set_constants(context, GpuStage::pixel, 0, 2, pixel_constants);
        gpu_set_input_layout(context, layout);
        gpu_set_vertex_buffer(context, instances, sizeof(ui_quads::Quad), 0);
        gpu_set_shader(context, vertex_shader);
        gpu_set_shader(context, pixel_shader);
        gpu_draw_instanced(context, 4, ui_quads::kQuadCount, 0, 0);

        check(gpu_ok(gpu_readback(device, context, target, {0, 0, ui_quads::kWidth, ui_quads::kHeight}, g_pixels,
                                  ui_quads::kWidth * 4)),
              "readback");
        const char* dump_path = environment_get("MAICORD_GPU_QUAD_DUMP");
        if (dump_path && dump_path[0])
            dump(dump_path);
        const bool decoded = ui_quads::decode_png({kGolden, sizeof(kGolden)}, g_golden);
        check(decoded, "the golden PNG decodes");
        if (decoded) {
            const ui_quads::Difference difference = ui_quads::compare(g_pixels, g_golden);
            test_write_metric("maximum channel difference=", difference.maximum);
            test_write_metric("pixels over tolerance=", difference.over_tolerance);
            if (difference.over_tolerance) {
                test_write_metric("first pixel x=", difference.first_x);
                test_write_metric("first pixel y=", difference.first_y);
            }
            check(ui_quads::within_tolerance(difference), "the render matches tests/gpu/golden/ui_quads.png");
        }
    }
    gpu_release(raster);
    gpu_release(blend);
    gpu_release(pixel_shader);
    gpu_release(layout);
    gpu_release(vertex_shader);
    gpu_release(background);
    gpu_release(frame);
    gpu_release(instances);
    gpu_release(view);
    gpu_release(target);
    gpu_device_destroy(&device, &context);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_ui_quads_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_gpu_ui_quads");
}
