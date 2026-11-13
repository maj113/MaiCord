// The Direct3D 11 side of the glyph scene (glyph_scene.hpp): the glyph compute shader (with its tile culling,
// as GlyphAtlasGpu builds it) rasterizes the scene's paths into an R8 atlas, the draw
// pipeline's glyph shaders and states draw the quads sampling it, and both results must match
// tests/gpu/golden/glyph_atlas.png and glyph_draw.png, which the OpenGL ES test (glyphs.cpp) reproduces on Linux. To
// renew the goldens after a shader or scene change, run with MAICORD_GPU_GLYPH_CAPTURE=<prefix> set: the atlas is
// written to <prefix>.atlas (64 x 128 R8 bytes) and the frame to <prefix>.draw (160 x 112 RGBA bytes), which
// tools/update_gpu_goldens.py turns into the PNGs.
#include "maicord/render/d3d11/draw_pipeline.hpp"
#include "maicord/render/text/glyph_atlas_gpu.hpp"
#include "d3d_harness.hpp"
#include "glyph_scene.hpp"

namespace maicord {
namespace {

using d3d_harness::fail;

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
u8 g_frame[kFrameBytes];
u8 g_atlas_golden[kAtlasBytes];
u8 g_frame_golden[kFrameBytes];
u8 g_batch[(glyph_scene::kJobRecords + 64) * glyph_scene::kRecordBytes];
glyph_scene::TextEffects g_effects;

bool dispatch_batch(ID3D11Device* device, ID3D11DeviceContext* context, const GlyphAtlasGpu& atlas,
                    ID3D11ComputeShader* shader, const glyph_scene::Batch& batch) noexcept {
    const u32 bytes = glyph_scene::encode(batch, g_batch);
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    const bool made = d3d_harness::raw_buffer(device, g_batch, bytes, &buffer, &view);
    if (made) {
        ID3D11UnorderedAccessView* uav = atlas.texture_uav;
        context->CSSetShader(shader, nullptr, 0);
        context->CSSetShaderResources(0, 1, &view);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch(glyph_scene::groups_x(batch), glyph_scene::groups_y(batch), batch.count);
        ID3D11UnorderedAccessView* none = nullptr;
        ID3D11ShaderResourceView* no_view = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        context->CSSetShaderResources(0, 1, &no_view);
    }
    com_release(view);
    com_release(buffer);
    return made;
}

bool run() noexcept {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (!d3d_harness::create_device(&device, &context))
        return fail("no Direct3D 11 device");

    GlyphAtlasGpu atlas{};
    if (!glyph_atlas_gpu_initialize(&atlas, device, glyph_scene::kAtlasWidth, glyph_scene::kAtlasHeight, 128).ok())
        return fail("glyph atlas creation");
    D3D11DrawPipelineOwner pipeline{};
    const D3D11DrawPipelineConfig config{64, 16, 48, 16, 48, 16};
    if (!d3d11_draw_pipeline_create(&pipeline, device, config).ok())
        return fail("draw pipeline creation");

    if (!dispatch_batch(device, context, atlas, atlas.raster_shader,
                        {glyph_scene::kPlainJobs, MAICORD_GLYPH_COUNT(glyph_scene::kPlainJobs)}) ||
        !dispatch_batch(device, context, atlas, atlas.raster_shader,
                        {glyph_scene::kCulledJobs, MAICORD_GLYPH_COUNT(glyph_scene::kCulledJobs)}))
        return fail("the glyph raster dispatches");
    if (!d3d_harness::read_back(device, context, atlas.texture, glyph_scene::kAtlasWidth, glyph_scene::kAtlasHeight, 1,
                                g_atlas))
        return fail("atlas readback");

    D3D11_TEXTURE2D_DESC description{};
    description.Width = glyph_scene::kFrameWidth;
    description.Height = glyph_scene::kFrameHeight;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target, nullptr, &view)))
        return fail("render target");
    // The colour glyph atlas the pixel shader declares; nothing in the scene samples it.
    description.Width = description.Height = 4;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    static const u8 kBlank[4 * 4 * 4] = {};
    const D3D11_SUBRESOURCE_DATA blank_data{kBlank, 16, 0};
    ID3D11Texture2D* blank = nullptr;
    ID3D11ShaderResourceView* blank_view = nullptr;
    if (FAILED(device->CreateTexture2D(&description, &blank_data, &blank)) ||
        FAILED(device->CreateShaderResourceView(blank, nullptr, &blank_view)))
        return fail("colour glyph atlas stand-in");

    ID3D11Buffer* frame = nullptr;
    ID3D11Buffer* effects = nullptr;
    glyph_scene::fill_effects(&g_effects);
    if (!d3d_harness::constant_buffer(device, &glyph_scene::kFrame, sizeof(glyph_scene::kFrame), &frame) ||
        !d3d_harness::constant_buffer(device, &g_effects, sizeof(g_effects), &effects))
        return fail("constant buffers");

    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(glyph_scene::kFrameWidth),
                                  static_cast<float>(glyph_scene::kFrameHeight), 0, 1};
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(glyph_scene::kFrameWidth),
                             static_cast<LONG>(glyph_scene::kFrameHeight)};
    const float blend_factor[4]{};
    context->OMSetRenderTargets(1, &view, nullptr);
    context->ClearRenderTargetView(view, glyph_scene::kClear);
    context->RSSetViewports(1, &viewport);
    context->RSSetState(pipeline.ui_rasterizer_state);
    context->RSSetScissorRects(1, &scissor);
    context->OMSetBlendState(pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
    ID3D11Buffer* constants[2] = {frame, effects};
    context->VSSetConstantBuffers(0, 2, constants);
    context->PSSetConstantBuffers(0, 2, constants);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(pipeline.glyph_instance_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return fail("instance buffer map");
    copy_bytes(mapped.pData, glyph_scene::kQuads, sizeof(glyph_scene::kQuads));
    context->Unmap(pipeline.glyph_instance_buffer, 0);
    const UINT stride = config.glyph_instance_stride, offset = 0;
    context->IASetInputLayout(pipeline.glyph_input_layout);
    context->IASetVertexBuffers(0, 1, &pipeline.glyph_instance_buffer, &stride, &offset);
    context->VSSetShader(pipeline.glyph_vertex_shader, nullptr, 0);
    context->PSSetShader(pipeline.glyph_pixel_shader, nullptr, 0);
    ID3D11ShaderResourceView* views[2] = {atlas.texture_view, blank_view};
    context->PSSetShaderResources(0, 2, views);
    context->PSSetSamplers(0, 1, &pipeline.glyph_sampler);
    context->DrawInstanced(4, glyph_scene::kQuadCount, 0, 0);
    if (!d3d_harness::read_back(device, context, target, glyph_scene::kFrameWidth, glyph_scene::kFrameHeight, 4,
                                g_frame))
        return fail("frame readback");

    com_release(blank_view);
    com_release(blank);
    com_release(effects);
    com_release(frame);
    com_release(view);
    com_release(target);
    d3d11_draw_pipeline_release(&pipeline);
    glyph_atlas_gpu_release(&atlas);
    com_release(context);
    com_release(device);

    WCHAR path[520];
    const u32 prefix = d3d_harness::capture_prefix(L"MAICORD_GPU_GLYPH_CAPTURE", path, 520);
    if (prefix) {
        if (!d3d_harness::write_capture(path, prefix, L".atlas", g_atlas, sizeof(g_atlas)) ||
            !d3d_harness::write_capture(path, prefix, L".draw", g_frame, sizeof(g_frame)))
            return fail("a capture file cannot be written");
        test_write_line("PASS captured the glyph scene");
        return true;
    }
    if (!png_golden::decode({kAtlasGolden, sizeof(kAtlasGolden)}, glyph_scene::kAtlasWidth, glyph_scene::kAtlasHeight, 1,
                            g_atlas_golden) ||
        !png_golden::decode({kDrawGolden, sizeof(kDrawGolden)}, glyph_scene::kFrameWidth, glyph_scene::kFrameHeight, 4,
                            g_frame_golden))
        return fail("a golden PNG does not decode");
    const png_golden::Difference atlas_difference =
        png_golden::compare(g_atlas, g_atlas_golden, glyph_scene::kAtlasWidth, glyph_scene::kAtlasHeight, 1,
                            glyph_scene::kAtlasTolerance.channel);
    const png_golden::Difference frame_difference =
        png_golden::compare(g_frame, g_frame_golden, glyph_scene::kFrameWidth, glyph_scene::kFrameHeight, 4,
                            glyph_scene::kFrameTolerance.channel);
    test_write_metric("atlas maximum difference=", atlas_difference.maximum);
    test_write_metric("frame maximum difference=", frame_difference.maximum);
    if (!png_golden::within(atlas_difference, glyph_scene::kAtlasTolerance))
        return fail("the atlas differs from tests/gpu/golden/glyph_atlas.png");
    if (!png_golden::within(frame_difference, glyph_scene::kFrameTolerance))
        return fail("the frame differs from tests/gpu/golden/glyph_draw.png");
    test_write_line("PASS glyphs match the goldens");
    return true;
}

} // namespace
} // namespace maicord

extern "C" void maicord_gpu_glyphs_windows_entry() {
    ExitProcess(maicord::run() ? 0 : 1);
}
