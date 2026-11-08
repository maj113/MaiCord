// The Direct3D 11 side of the media scene (media_scene.hpp): the PNG, VP8 and JPEG compute shaders write synthetic
// pictures into an RGBA8 media atlas, the draw pipeline's media shaders and states draw quads sampling it, and both
// results must match tests/gpu/golden/media_atlas.png and media_draw.png, which the OpenGL ES test (media.cpp)
// reproduces on Linux. To renew the goldens after a shader or scene change, run with MAICORD_GPU_MEDIA_CAPTURE=<prefix>
// set: the atlas is written to <prefix>.atlas (96 x 64 RGBA bytes) and the frame to <prefix>.draw (160 x 112 RGBA
// bytes), which tools/update_gpu_goldens.py turns into the PNGs.
#include "maicord/gpu/api.hpp"
#include "maicord/render/d3d11/draw_pipeline.hpp"
#include "d3d_harness.hpp"
#include "media_scene.hpp"

namespace maicord {
namespace {

using d3d_harness::fail;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
alignas(16) constexpr u8 kAtlasGolden[] = {
#embed "golden/media_atlas.png"
};
alignas(16) constexpr u8 kDrawGolden[] = {
#embed "golden/media_draw.png"
};
#pragma clang diagnostic pop

constexpr u32 kAtlasBytes = media_scene::kAtlasWidth * media_scene::kAtlasHeight * 4;
constexpr u32 kFrameBytes = media_scene::kFrameWidth * media_scene::kFrameHeight * 4;

u8 g_atlas[kAtlasBytes];
u8 g_frame[kFrameBytes];
u8 g_atlas_golden[kAtlasBytes];
u8 g_frame_golden[kFrameBytes];
media_scene::Data g_data;

struct Compute {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11UnorderedAccessView* atlas;
};

// One dispatch: constants in b0, the shader resources from t0, the unordered access views from u0.
bool dispatch(const Compute& compute, ID3D11ComputeShader* shader, const void* constants, u32 constant_bytes,
              u32 view_count, ID3D11ShaderResourceView* const* views, u32 uav_count,
              ID3D11UnorderedAccessView* const* uavs, u32 groups_x, u32 groups_y) noexcept {
    ID3D11Buffer* buffer = nullptr;
    if (!d3d_harness::constant_buffer(compute.device, constants, constant_bytes, &buffer))
        return false;
    compute.context->CSSetShader(shader, nullptr, 0);
    compute.context->CSSetConstantBuffers(0, 1, &buffer);
    compute.context->CSSetShaderResources(0, view_count, views);
    compute.context->CSSetUnorderedAccessViews(0, uav_count, uavs, nullptr);
    compute.context->Dispatch(groups_x, groups_y, 1);
    ID3D11UnorderedAccessView* no_uavs[2] = {};
    ID3D11ShaderResourceView* no_views[2] = {};
    compute.context->CSSetUnorderedAccessViews(0, uav_count, no_uavs, nullptr);
    compute.context->CSSetShaderResources(0, view_count, no_views);
    com_release(buffer);
    return true;
}

constexpr u32 groups(u32 extent) noexcept {
    return (extent + 7) / 8;
}

bool run() noexcept {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (!d3d_harness::create_device(&device, &context))
        return fail("no Direct3D 11 device");

    ID3D11ComputeShader *png = nullptr, *vp8 = nullptr, *jpeg_idct = nullptr, *jpeg_atlas = nullptr;
    if (!gpu_ok(gpu_compute_shader_create(device, ShaderId::png_compute, &png)) ||
        !gpu_ok(gpu_compute_shader_create(device, ShaderId::vp8_atlas, &vp8)) ||
        !gpu_ok(gpu_compute_shader_create(device, ShaderId::jpeg_idct_rows, &jpeg_idct)) ||
        !gpu_ok(gpu_compute_shader_create(device, ShaderId::jpeg_atlas, &jpeg_atlas)))
        return fail("a decode shader");
    D3D11DrawPipelineOwner pipeline{};
    const D3D11DrawPipelineConfig config{64, 16, 48, 16, 64, 16};
    if (!d3d11_draw_pipeline_create(&pipeline, device, config).ok())
        return fail("draw pipeline creation");

    // The atlas: sampled, rendered to (cleared) and written by compute.
    D3D11_TEXTURE2D_DESC description{};
    description.Width = media_scene::kAtlasWidth;
    description.Height = media_scene::kAtlasHeight;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* atlas = nullptr;
    ID3D11ShaderResourceView* atlas_view = nullptr;
    ID3D11RenderTargetView* atlas_target = nullptr;
    ID3D11UnorderedAccessView* atlas_uav = nullptr;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &atlas)) ||
        FAILED(device->CreateShaderResourceView(atlas, nullptr, &atlas_view)) ||
        FAILED(device->CreateRenderTargetView(atlas, nullptr, &atlas_target)) ||
        FAILED(device->CreateUnorderedAccessView(atlas, nullptr, &atlas_uav)))
        return fail("the atlas");
    const float transparent[4] = {};
    context->ClearRenderTargetView(atlas_target, transparent);
    const Compute compute{device, context, atlas_uav};

    media_scene::build(&g_data);
    // PNG: four dispatches onto the atlas, the second over-blending onto the first.
    ID3D11Buffer *palette_buffer = nullptr;
    ID3D11ShaderResourceView* palette_view = nullptr;
    if (!d3d_harness::typed_buffer(device, g_data.palette, 16, &palette_buffer, &palette_view))
        return fail("the palette");
    for (u32 job = 0; job < media_scene::kPngJobs; ++job) {
        const media_scene::PngConstants constants = media_scene::png_job(job);
        ID3D11Buffer* scanlines = nullptr;
        ID3D11ShaderResourceView* scanline_view = nullptr;
        if (!d3d_harness::raw_buffer(device, media_scene::png_bytes(g_data, job), media_scene::png_byte_count(job), &scanlines,
                                     &scanline_view))
            return fail("PNG scanlines");
        ID3D11ShaderResourceView* views[2] = {scanline_view, palette_view};
        ID3D11UnorderedAccessView* uavs[1] = {atlas_uav};
        const bool ok = dispatch(compute, png, &constants, sizeof(constants), 2, views, 1, uavs,
                                 groups(constants.image_width), groups(constants.image_height));
        com_release(scanline_view);
        com_release(scanlines);
        if (!ok)
            return fail("a PNG dispatch");
    }

    // VP8: samples and alpha, replacing and over-blending.
    ID3D11Buffer *yuv = nullptr, *alpha = nullptr;
    ID3D11ShaderResourceView *yuv_view = nullptr, *alpha_view = nullptr;
    if (!d3d_harness::raw_buffer(device, g_data.yuv, sizeof(g_data.yuv), &yuv, &yuv_view) ||
        !d3d_harness::raw_buffer(device, g_data.vp8_alpha, sizeof(g_data.vp8_alpha), &alpha, &alpha_view))
        return fail("VP8 samples");
    for (u32 job = 0; job < media_scene::kVp8Jobs; ++job) {
        const media_scene::Vp8Constants constants = media_scene::vp8_job(job);
        ID3D11ShaderResourceView* views[2] = {yuv_view, alpha_view};
        ID3D11UnorderedAccessView* uavs[1] = {atlas_uav};
        if (!dispatch(compute, vp8, &constants, sizeof(constants), 2, views, 1, uavs, (media_scene::kVp8Width + 63) / 64,
                      media_scene::kVp8Height))
            return fail("a VP8 dispatch");
    }

    // JPEG: the inverse transform into a GPU-written sample buffer, then the atlas passes reading it.
    ID3D11Buffer *coefficients = nullptr, *quantization = nullptr, *samples = nullptr;
    ID3D11ShaderResourceView *coefficient_view = nullptr, *quantization_view = nullptr, *sample_view = nullptr;
    ID3D11UnorderedAccessView* sample_uav = nullptr;
    if (!d3d_harness::raw_buffer(device, g_data.jpeg_coefficients, sizeof(g_data.jpeg_coefficients), &coefficients,
                                 &coefficient_view) ||
        !d3d_harness::raw_buffer(device, g_data.jpeg_quantization, sizeof(g_data.jpeg_quantization), &quantization,
                                 &quantization_view) ||
        !d3d_harness::raw_buffer(device, nullptr, media_scene::kJpegSamples * 4, &samples, &sample_view, &sample_uav))
        return fail("JPEG buffers");
    {
        const media_scene::JpegIdctConstants constants = media_scene::jpeg_idct_constants();
        ID3D11ShaderResourceView* views[2] = {coefficient_view, quantization_view};
        ID3D11UnorderedAccessView* uavs[1] = {sample_uav};
        if (!dispatch(compute, jpeg_idct, &constants, sizeof(constants), 2, views, 1, uavs, media_scene::kJpegBlocks, 1))
            return fail("the JPEG inverse transform");
    }
    for (const media_scene::JpegJob& job : media_scene::kJpegJobs) {
        const media_scene::JpegAtlasConstants constants =
            media_scene::jpeg_atlas_constants(job.destination_x, job.destination_y, job.orientation);
        ID3D11ShaderResourceView* views[1] = {sample_view};
        ID3D11UnorderedAccessView* uavs[1] = {atlas_uav};
        if (!dispatch(compute, jpeg_atlas, &constants, sizeof(constants), 1, views, 1, uavs,
                      groups(media_scene::kJpegSize), groups(media_scene::kJpegSize)))
            return fail("a JPEG atlas dispatch");
    }
    if (!d3d_harness::read_back(device, context, atlas, media_scene::kAtlasWidth, media_scene::kAtlasHeight, 4, g_atlas))
        return fail("atlas readback");

    // The draw: three atlases (the media atlas, the animation cache, the preview) and the media shaders.
    description.Width = media_scene::kFrameWidth;
    description.Height = media_scene::kFrameHeight;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target, nullptr, &view)))
        return fail("render target");
    ID3D11Texture2D* side[2] = {};
    ID3D11ShaderResourceView* side_views[2] = {};
    static u8 side_pixels[2][16 * 16 * 4];
    for (u32 i = 0; i < 2; ++i) {
        media_scene::side_picture(i, side_pixels[i]);
        D3D11_TEXTURE2D_DESC small{};
        small.Width = small.Height = 16;
        small.MipLevels = small.ArraySize = small.SampleDesc.Count = 1;
        small.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        small.Usage = D3D11_USAGE_DEFAULT;
        small.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{side_pixels[i], 64, 0};
        if (FAILED(device->CreateTexture2D(&small, &initial, &side[i])) ||
            FAILED(device->CreateShaderResourceView(side[i], nullptr, &side_views[i])))
            return fail("a side atlas");
    }
    ID3D11Buffer* frame = nullptr;
    if (!d3d_harness::constant_buffer(device, &media_scene::kFrame, sizeof(media_scene::kFrame), &frame))
        return fail("frame constants");

    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(media_scene::kFrameWidth),
                                  static_cast<float>(media_scene::kFrameHeight), 0, 1};
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(media_scene::kFrameWidth),
                             static_cast<LONG>(media_scene::kFrameHeight)};
    const float blend_factor[4]{};
    context->OMSetRenderTargets(1, &view, nullptr);
    context->ClearRenderTargetView(view, media_scene::kClear);
    context->RSSetViewports(1, &viewport);
    context->RSSetState(pipeline.ui_rasterizer_state);
    context->RSSetScissorRects(1, &scissor);
    context->OMSetBlendState(pipeline.ui_blend_state, blend_factor, 0xFFFFFFFFu);
    context->VSSetConstantBuffers(0, 1, &frame);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(pipeline.media_instance_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return fail("instance buffer map");
    copy_bytes(mapped.pData, media_scene::kQuads, sizeof(media_scene::kQuads));
    context->Unmap(pipeline.media_instance_buffer, 0);
    const UINT stride = config.media_instance_stride, offset = 0;
    context->IASetInputLayout(pipeline.media_input_layout);
    context->IASetVertexBuffers(0, 1, &pipeline.media_instance_buffer, &stride, &offset);
    context->VSSetShader(pipeline.media_vertex_shader, nullptr, 0);
    context->PSSetShader(pipeline.media_pixel_shader, nullptr, 0);
    ID3D11ShaderResourceView* views[3] = {atlas_view, side_views[0], side_views[1]};
    context->PSSetShaderResources(0, 3, views);
    context->PSSetSamplers(0, 1, &pipeline.glyph_sampler);
    context->DrawInstanced(4, media_scene::kQuadCount, 0, 0);
    if (!d3d_harness::read_back(device, context, target, media_scene::kFrameWidth, media_scene::kFrameHeight, 4, g_frame))
        return fail("frame readback");

    WCHAR path[520];
    const u32 prefix = d3d_harness::capture_prefix(L"MAICORD_GPU_MEDIA_CAPTURE", path, 520);
    bool result = true;
    if (prefix) {
        if (!d3d_harness::write_capture(path, prefix, L".atlas", g_atlas, sizeof(g_atlas)) ||
            !d3d_harness::write_capture(path, prefix, L".draw", g_frame, sizeof(g_frame)))
            result = fail("a capture file cannot be written");
        else
            test_write_line("PASS captured the media scene");
    } else if (!png_golden::decode({kAtlasGolden, sizeof(kAtlasGolden)}, media_scene::kAtlasWidth,
                                   media_scene::kAtlasHeight, 4, g_atlas_golden) ||
               !png_golden::decode({kDrawGolden, sizeof(kDrawGolden)}, media_scene::kFrameWidth,
                                   media_scene::kFrameHeight, 4, g_frame_golden)) {
        result = fail("a golden PNG does not decode");
    } else {
        const png_golden::Difference atlas_difference =
            png_golden::compare(g_atlas, g_atlas_golden, media_scene::kAtlasWidth, media_scene::kAtlasHeight, 4,
                                media_scene::kAtlasTolerance.channel);
        const png_golden::Difference frame_difference =
            png_golden::compare(g_frame, g_frame_golden, media_scene::kFrameWidth, media_scene::kFrameHeight, 4,
                                media_scene::kFrameTolerance.channel);
        test_write_metric("atlas maximum difference=", atlas_difference.maximum);
        test_write_metric("frame maximum difference=", frame_difference.maximum);
        if (!png_golden::within(atlas_difference, media_scene::kAtlasTolerance))
            result = fail("the atlas differs from tests/gpu/golden/media_atlas.png");
        else if (!png_golden::within(frame_difference, media_scene::kFrameTolerance))
            result = fail("the frame differs from tests/gpu/golden/media_draw.png");
        else
            test_write_line("PASS media matches the goldens");
    }

    com_release(frame);
    for (u32 i = 0; i < 2; ++i) {
        com_release(side_views[i]);
        com_release(side[i]);
    }
    com_release(view);
    com_release(target);
    com_release(sample_uav);
    com_release(sample_view);
    com_release(samples);
    com_release(quantization_view);
    com_release(quantization);
    com_release(coefficient_view);
    com_release(coefficients);
    com_release(alpha_view);
    com_release(alpha);
    com_release(yuv_view);
    com_release(yuv);
    com_release(palette_view);
    com_release(palette_buffer);
    com_release(atlas_uav);
    com_release(atlas_target);
    com_release(atlas_view);
    com_release(atlas);
    d3d11_draw_pipeline_release(&pipeline);
    com_release(jpeg_atlas);
    com_release(jpeg_idct);
    com_release(vp8);
    com_release(png);
    com_release(context);
    com_release(device);
    return result;
}

} // namespace
} // namespace maicord

extern "C" void maicord_gpu_media_windows_entry() {
    ExitProcess(maicord::run() ? 0 : 1);
}
