// Runs the glyph scene (glyph_scene.hpp) through the GPU API: the glyph compute shaders (plain and tile-culling
// variants) rasterize the scene's paths into an R8 atlas (an RGBA8 storage image on OpenGL ES, whose red channel is the
// coverage), and the glyph vertex and pixel shaders draw quads sampling it, with the text-effect materials. Both
// results are compared with tests/gpu/golden/glyph_atlas.png and glyph_draw.png, which the Direct3D 11 side
// (glyphs_windows.cpp) captured on Windows. MAICORD_GPU_GLYPH_DUMP=<prefix> also writes <prefix>.atlas and
// <prefix>.draw as raw bytes (tools/update_gpu_goldens.py's formats), for inspecting a difference.
#include "maicord/core/log.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../support/test_output.hpp"
#include "capture_io.hpp"
#include "glyph_scene.hpp"

namespace maicord {
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
alignas(16) constexpr u8 kAtlasGolden[] = {
#embed "golden/glyph_atlas.png"
};
alignas(16) constexpr u8 kDrawGolden[] = {
#embed "golden/glyph_draw.png"
};
#pragma clang diagnostic pop

constexpr u32 kAtlasBytes = glyph_scene::kAtlasWidth * glyph_scene::kAtlasHeight;
constexpr u32 kFrameBytes = glyph_scene::kFrameWidth * glyph_scene::kFrameHeight * 4;

u8 g_atlas[kAtlasBytes];
u8 g_copy[kAtlasBytes];
u8 g_frame[kFrameBytes];
u8 g_atlas_golden[kAtlasBytes];
u8 g_frame_golden[kFrameBytes];
u8 g_batch[(glyph_scene::kJobRecords + 64) * glyph_scene::kRecordBytes];
glyph_scene::TextEffects g_effects;

u32 g_failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

// One dispatch of a batch: its raw bytes through a dynamic raw buffer (the production path), the atlas as the
// storage image.
bool dispatch_batch(GpuDevice* device, GpuContext* context, GpuUav* atlas, GpuComputeShader* shader,
                    const glyph_scene::Batch& batch) noexcept {
    const u32 bytes = glyph_scene::encode(batch, g_batch);
    GpuBuffer* buffer = nullptr;
    GpuSrv* view = nullptr;
    bool ok = gpu_ok(gpu_buffer_create(device, {bytes, GpuUsage::dynamic, gpu_bind_srv, true}, nullptr, &buffer)) &&
              gpu_ok(gpu_srv_create(device, buffer, {GpuViewKind::buffer_raw, GpuFormat::r32_uint, bytes / 4}, &view));
    if (ok) {
        void* mapped = gpu_map(context, buffer, GpuMap::discard, nullptr);
        ok = mapped != nullptr;
        if (mapped) {
            copy_bytes(mapped, g_batch, bytes);
            gpu_unmap(context, buffer);
        }
    }
    if (ok) {
        GpuSrv* views[1] = {view};
        GpuUav* uavs[1] = {atlas};
        gpu_set_shader(context, shader);
        gpu_set_srvs(context, GpuStage::compute, 0, 1, views);
        gpu_set_uavs(context, 0, 1, uavs);
        gpu_dispatch(context, glyph_scene::groups_x(batch), glyph_scene::groups_y(batch), batch.count);
        GpuSrv* no_view[1] = {nullptr};
        GpuUav* no_uav[1] = {nullptr};
        gpu_set_srvs(context, GpuStage::compute, 0, 1, no_view);
        gpu_set_uavs(context, 0, 1, no_uav);
    }
    gpu_release(view);
    gpu_release(buffer);
    return ok;
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
    // Coverage is written by compute, so a device where the atlas cannot be a storage image has no glyph path.
    check(gpu_ok(gpu_format_check(device, GpuFormat::r8, gpu_bind_srv | gpu_bind_uav)),
          "an R8 atlas can be a sampled storage image");
    check(gpu_caps(device).max_compute_invocations >= 64, "the compute group fits an 8 x 8 tile");

    // The atlas is a paged surface of 4 pages (the glyph shaders' t0): the rasters write page 1, which is copied to
    // page 3, and the quads sample both pages in one draw.
    constexpr u32 kAtlasPages = 4, kScenePage = 1, kCopyPage = 3;
    GpuPagedTexture* paged = nullptr;
    check(gpu_ok(gpu_paged_texture_create(device,
                                          {glyph_scene::kAtlasWidth, glyph_scene::kAtlasHeight, kAtlasPages,
                                           GpuFormat::r8, gpu_bind_srv | gpu_bind_uav},
                                          &paged)) &&
              gpu_ok(gpu_paged_texture_commit(context, paged, kScenePage, glyph_scene::kAtlasHeight)) &&
              gpu_ok(gpu_paged_texture_commit(context, paged, kCopyPage, glyph_scene::kAtlasHeight)),
          "the paged atlas");
    const GpuPage scene_page = paged ? gpu_paged_texture_page(paged, kScenePage * glyph_scene::kAtlasHeight) : GpuPage{};
    const GpuPage copy_page = paged ? gpu_paged_texture_page(paged, kCopyPage * glyph_scene::kAtlasHeight) : GpuPage{};
    GpuTexture* atlas = scene_page.texture;
    GpuUav* atlas_uav = scene_page.uav;
    GpuSrv* atlas_view = paged ? gpu_paged_texture_view(paged) : nullptr;
    check(atlas && atlas_uav && atlas_view && copy_page.texture, "the atlas pages");
    GpuComputeShader* plain = nullptr;
    GpuComputeShader* culled = nullptr;
    check(gpu_ok(gpu_compute_shader_create(device, ShaderId::glyph_compute, &plain)), "glyph_compute");
    check(gpu_ok(gpu_compute_shader_create(device, ShaderId::glyph_culled_compute, &culled)), "glyph_culled_compute");

    GpuTexture* target = nullptr;
    GpuRtv* view = nullptr;
    check(gpu_ok(gpu_texture_create(device,
                                    {glyph_scene::kFrameWidth, glyph_scene::kFrameHeight, GpuFormat::rgba8,
                                     GpuUsage::default_, gpu_bind_srv | gpu_bind_rtv},
                                    nullptr, &target)) &&
              gpu_ok(gpu_rtv_create(device, target, &view)),
          "render target");
    // The colour glyph atlas the pixel shader declares; nothing in the scene samples it.
    static const u8 kBlank[4 * 4 * 4] = {};
    const GpuInitialData blank_data{kBlank, 16};
    GpuTexture* blank = nullptr;
    GpuSrv* blank_view = nullptr;
    check(gpu_ok(gpu_texture_create(device, {4, 4, GpuFormat::rgba8, GpuUsage::immutable, gpu_bind_srv}, &blank_data,
                                    &blank)) &&
              gpu_ok(gpu_srv_create(device, blank, {GpuViewKind::texture, GpuFormat::rgba8}, &blank_view)),
          "colour glyph atlas stand-in");

    GpuBuffer* instances = nullptr;
    check(gpu_ok(gpu_buffer_create(device, {sizeof(glyph_scene::kQuads), GpuUsage::dynamic, gpu_bind_vertex, false},
                                   nullptr, &instances)),
          "instance buffer");
    GpuBuffer* frame = nullptr;
    check(gpu_ok(gpu_buffer_create(device,
                                   {sizeof(glyph_scene::kFrame), GpuUsage::default_, gpu_bind_constant, false},
                                   &glyph_scene::kFrame, &frame)),
          "frame constants");
    glyph_scene::fill_effects(&g_effects);
    GpuBuffer* effects = nullptr;
    check(gpu_ok(gpu_buffer_create(device, {sizeof(g_effects), GpuUsage::default_, gpu_bind_constant, false},
                                   &g_effects, &effects)),
          "text effect constants");

    // The elements of D3D11DrawPipeline's glyph layout (draw_pipeline.cpp).
    const GpuInputElement elements[] = {{"RECT", 0, 0}, {"TEXCOORD", 0, 16}, {"COLOR", 0, 32}};
    GpuVertexShader* vertex_shader = nullptr;
    GpuInputLayout* layout = nullptr;
    bool layout_failed = false;
    check(gpu_ok(gpu_vertex_shader_create(device, ShaderId::glyph_vertex, {elements, 3}, &vertex_shader, &layout,
                                          &layout_failed)),
          "glyph vertex shader");
    GpuPixelShader* pixel_shader = nullptr;
    check(gpu_ok(gpu_pixel_shader_create(device, ShaderId::glyph_pixel, &pixel_shader)), "glyph pixel shader");
    GpuSampler* sampler = nullptr;
    check(gpu_ok(gpu_sampler_create(device, {GpuFilter::linear, GpuAddress::clamp}, &sampler)), "glyph sampler");
    GpuBlend* blend = nullptr;
    check(gpu_ok(gpu_blend_create(device,
                                  {true, GpuBlendFactor::one, GpuBlendFactor::inv_src_alpha, GpuBlendFactor::one,
                                   GpuBlendFactor::inv_src_alpha},
                                  &blend)),
          "premultiplied blend");
    GpuRaster* raster = nullptr;
    check(gpu_ok(gpu_raster_create(device, &raster)), "rasterizer");

    if (g_failures == 0) {
        check(dispatch_batch(device, context, atlas_uav, plain,
                             {glyph_scene::kPlainJobs, MAICORD_GLYPH_COUNT(glyph_scene::kPlainJobs)}),
              "the plain glyph dispatch");
        check(dispatch_batch(device, context, atlas_uav, culled,
                             {glyph_scene::kCulledJobs, MAICORD_GLYPH_COUNT(glyph_scene::kCulledJobs)}),
              "the tile-culling glyph dispatch");
        check(gpu_ok(gpu_readback(device, context, atlas, {0, 0, glyph_scene::kAtlasWidth, glyph_scene::kAtlasHeight},
                                  g_atlas, glyph_scene::kAtlasWidth)),
              "atlas readback");

        gpu_copy_region(context, copy_page.texture, 0, 0, atlas,
                        {0, 0, glyph_scene::kAtlasWidth, glyph_scene::kAtlasHeight});
        check(gpu_ok(gpu_readback(device, context, copy_page.texture,
                                  {0, 0, glyph_scene::kAtlasWidth, glyph_scene::kAtlasHeight}, g_copy,
                                  glyph_scene::kAtlasWidth)) &&
                  bytes_equal(g_copy, g_atlas, kAtlasBytes),
              "the copy page holds the scene page");

        // The draw reads the atlas the compute just wrote: the backend orders them. The quads keep the scene's uv:
        // v is in the rows of the page a draw binds, and the quads alternate between the scene page and the copy
        // page, so the draw splits per quad as the renderer does where consecutive glyphs change page.
        void* mapped = gpu_map(context, instances, GpuMap::discard, nullptr);
        check(mapped != nullptr, "instance map");
        if (mapped) {
            copy_bytes(mapped, glyph_scene::kQuads, sizeof(glyph_scene::kQuads));
            gpu_unmap(context, instances);
        }
        gpu_clear_rtv(context, view, glyph_scene::kClear);
        gpu_set_render_target(context, view);
        gpu_set_viewport(context, 0, 0, static_cast<float>(glyph_scene::kFrameWidth),
                         static_cast<float>(glyph_scene::kFrameHeight));
        gpu_set_scissor(context, {0, 0, static_cast<i32>(glyph_scene::kFrameWidth),
                                  static_cast<i32>(glyph_scene::kFrameHeight)});
        gpu_set_raster(context, raster);
        const float no_factor[4] = {0, 0, 0, 0};
        gpu_set_blend(context, blend, no_factor);
        GpuBuffer* constants[2] = {frame, effects};
        gpu_set_constants(context, GpuStage::vertex, 0, 2, constants);
        gpu_set_constants(context, GpuStage::pixel, 0, 2, constants);
        GpuSampler* samplers[1] = {sampler};
        gpu_set_samplers(context, GpuStage::pixel, 0, 1, samplers);
        gpu_set_input_layout(context, layout);
        gpu_set_vertex_buffer(context, instances, sizeof(glyph_scene::Quad), 0);
        gpu_set_shader(context, vertex_shader);
        gpu_set_shader(context, pixel_shader);
        for (u32 i = 0; i < glyph_scene::kQuadCount; ++i) {
            GpuSrv* views[2] = {gpu_paged_texture_binding_view(paged, i & 1 ? kCopyPage : kScenePage), blank_view};
            gpu_set_srvs(context, GpuStage::pixel, 0, 2, views);
            gpu_draw_instanced(context, 4, 1, 0, i);
        }
        check(gpu_ok(gpu_readback(device, context, target, {0, 0, glyph_scene::kFrameWidth, glyph_scene::kFrameHeight},
                                  g_frame, glyph_scene::kFrameWidth * 4)),
              "frame readback");

        const char* dump_prefix = environment_get("MAICORD_GPU_GLYPH_DUMP");
        if (dump_prefix && dump_prefix[0]) {
            capture_io::dump(dump_prefix, ".atlas", g_atlas, sizeof(g_atlas));
            capture_io::dump(dump_prefix, ".draw", g_frame, sizeof(g_frame));
        }
        const bool decoded = png_golden::decode({kAtlasGolden, sizeof(kAtlasGolden)}, glyph_scene::kAtlasWidth,
                                                glyph_scene::kAtlasHeight, 1, g_atlas_golden) &&
                             png_golden::decode({kDrawGolden, sizeof(kDrawGolden)}, glyph_scene::kFrameWidth,
                                                glyph_scene::kFrameHeight, 4, g_frame_golden);
        check(decoded, "the golden PNGs decode");
        if (decoded) {
            const png_golden::Difference atlas_difference =
                png_golden::compare(g_atlas, g_atlas_golden, glyph_scene::kAtlasWidth, glyph_scene::kAtlasHeight, 1,
                                    glyph_scene::kAtlasTolerance.channel);
            test_write_metric("atlas maximum channel difference=", atlas_difference.maximum);
            test_write_metric("atlas pixels over tolerance=", atlas_difference.over_tolerance);
            check(png_golden::within(atlas_difference, glyph_scene::kAtlasTolerance),
                  "the atlas matches tests/gpu/golden/glyph_atlas.png");
            const png_golden::Difference frame_difference =
                png_golden::compare(g_frame, g_frame_golden, glyph_scene::kFrameWidth, glyph_scene::kFrameHeight, 4,
                                    glyph_scene::kFrameTolerance.channel);
            test_write_metric("frame maximum channel difference=", frame_difference.maximum);
            test_write_metric("frame pixels over tolerance=", frame_difference.over_tolerance);
            if (frame_difference.over_tolerance) {
                test_write_metric("first pixel x=", frame_difference.first_x);
                test_write_metric("first pixel y=", frame_difference.first_y);
            }
            check(png_golden::within(frame_difference, glyph_scene::kFrameTolerance),
                  "the frame matches tests/gpu/golden/glyph_draw.png");
        }
    }
    gpu_release(raster);
    gpu_release(blend);
    gpu_release(sampler);
    gpu_release(pixel_shader);
    gpu_release(layout);
    gpu_release(vertex_shader);
    gpu_release(effects);
    gpu_release(frame);
    gpu_release(instances);
    gpu_release(blank_view);
    gpu_release(blank);
    gpu_release(view);
    gpu_release(target);
    gpu_release(culled);
    gpu_release(plain);
    gpu_paged_texture_release(&paged);
    gpu_device_destroy(&device, &context);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_glyphs_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_gpu_glyphs");
}
