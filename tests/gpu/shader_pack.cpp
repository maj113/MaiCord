// Every built-in shader of the GLSL pack inflates, compiles (graphics) or compiles and links (compute) on the
// driver, every vertex/pixel pair the renderer draws with links, and a request that names the wrong stage or an
// unknown id is refused. White-box: the pair link goes through the backend's program cache.
#include "maicord/core/log.hpp"
#include "gpu/gles/internal.hpp"
#include "../support/test_output.hpp"

namespace maicord {
namespace {

u32 g_failures = 0;
u32 g_checks = 0;

void check(bool condition, const char* label) noexcept {
    ++g_checks;
    test_check(&g_failures, condition, label);
}

// The layouts of D3D11DrawPipeline (src/render/d3d11/draw_pipeline.cpp).
const GpuInputElement kUi[] = {{"RECT", 0, 0}, {"COLOR", 0, 16}, {"COLOR", 1, 32}, {"TEXCOORD", 0, 48}};
const GpuInputElement kGlyph[] = {{"RECT", 0, 0}, {"TEXCOORD", 0, 16}, {"COLOR", 0, 32}};
const GpuInputElement kMedia[] = {{"RECT", 0, 0}, {"TEXCOORD", 0, 16}, {"TEXCOORD", 1, 32}, {"TEXCOORD", 2, 48}};

struct Pair {
    ShaderId vertex, pixel;
    ConstSlice<GpuInputElement> elements;
    const char* name;
};

void run() noexcept {
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    check(gpu_ok(gpu_device_create(&device, &context)), "device creation");
    if (!device)
        return;
    const Pair pairs[] = {
        {ShaderId::ui_vertex, ShaderId::ui_pixel, {kUi, 4}, "ui"},
        {ShaderId::glyph_vertex, ShaderId::glyph_pixel, {kGlyph, 3}, "glyph"},
        {ShaderId::media_vertex, ShaderId::media_pixel, {kMedia, 4}, "media"},
        {ShaderId::fx_vertex, ShaderId::fx_builtin, {}, "fx builtin"},
        {ShaderId::fx_composite_vertex, ShaderId::fx_composite_pixel, {}, "fx composite"},
    };
    for (const Pair& pair : pairs) {
        GpuVertexShader* vertex = nullptr;
        GpuInputLayout* layout = nullptr;
        GpuPixelShader* pixel = nullptr;
        bool layout_failed = true;
        const bool created =
            gpu_ok(gpu_vertex_shader_create(device, pair.vertex, pair.elements, &vertex, &layout, &layout_failed)) &&
            !layout_failed && (pair.elements.count == 0) == (layout == nullptr) &&
            gpu_ok(gpu_pixel_shader_create(device, pair.pixel, &pixel));
        check(created, pair.name);
        if (created) {
            context->vertex_shader = vertex;
            context->pixel_shader = pixel;
            check(gles::graphics_program(context) != 0, pair.name);
        }
        gpu_release(pixel);
        gpu_release(layout);
        gpu_release(vertex);
    }
    // The vertex shader that covers a whole target pairs with the package's pixel shader, not a built-in one.
    GpuVertexShader* target_vertex = nullptr;
    GpuInputLayout* no_layout = nullptr;
    check(gpu_ok(gpu_vertex_shader_create(device, ShaderId::fx_target_vertex, {}, &target_vertex, &no_layout, nullptr)),
          "fx target vertex");
    gpu_release(target_vertex);

    const ShaderId compute[] = {ShaderId::glyph_compute, ShaderId::colr_compute,
                                ShaderId::colr_wide_compute, ShaderId::colr_layers_compute, ShaderId::png_compute,
                                ShaderId::vp8_reconstruct, ShaderId::vp8_idct, ShaderId::vp8_filter,
                                ShaderId::vp8_atlas, ShaderId::jpeg_idct, ShaderId::jpeg_atlas,
                                ShaderId::video_frame};
    for (const ShaderId id : compute) {
        GpuComputeShader* shader = nullptr;
        const GpuStatus status = gpu_compute_shader_create(device, id, &shader);
        if (!gpu_ok(status))
            test_write_metric("compute shader failed, id=", static_cast<u32>(id));
        check(gpu_ok(status) && shader, "a compute shader links");
        gpu_release(shader);
    }
    // The same compute shader made twice is one program, which outlives the first handle.
    GpuComputeShader* first = nullptr;
    GpuComputeShader* second = nullptr;
    check(gpu_ok(gpu_compute_shader_create(device, ShaderId::glyph_compute, &first)) &&
              gpu_ok(gpu_compute_shader_create(device, ShaderId::glyph_compute, &second)) && first->name == second->name,
          "a compute shader made twice shares its program");
    const u32 shared_program = second ? second->name : 0;
    gpu_release(first);
    check(second && second->name == shared_program && shared_program != 0, "the program outlives the first handle");
    gpu_release(second);
    GpuComputeShader* again = nullptr;
    check(gpu_ok(gpu_compute_shader_create(device, ShaderId::glyph_compute, &again)) && again,
          "a compute shader is made again after the last handle went");
    gpu_release(again);

    GpuPixelShader* pixel = nullptr;
    check(gpu_pixel_shader_create(device, ShaderId::ui_vertex, &pixel) == kGpuInvalidArgument && !pixel,
          "a vertex shader is not a pixel shader");
    GpuVertexShader* vertex = nullptr;
    GpuInputLayout* layout = nullptr;
    check(gpu_vertex_shader_create(device, ShaderId::count, {}, &vertex, &layout, nullptr) == kGpuInvalidArgument &&
              !vertex,
          "an unknown id is refused");
    GpuComputeShader* shader = nullptr;
    check(gpu_compute_shader_create(device, ShaderId::ui_pixel, &shader) == kGpuInvalidArgument && !shader,
          "a pixel shader is not a compute shader");
    gpu_device_destroy(&device, &context);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_shader_pack_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_write_metric("checks=", maicord::g_checks);
    maicord::test_exit(maicord::g_failures, "maicord_gpu_shader_pack");
}
