#include "media_atlas_gpu_internal.hpp"

namespace maicord::media_atlas_gpu_detail {

HRESULT upload_discard(ID3D11DeviceContext1* context, ID3D11Buffer* buffer, const void* source,
                       u32 bytes) noexcept {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(result)) {
        CopyMemory(mapped.pData, source, bytes);
        context->Unmap(buffer, 0);
    }
    return result;
}

HRESULT upload_png_shader_inputs(MediaAtlasGpu* gpu, ID3D11Device* device,
                                 ID3D11DeviceContext1* context, ConstSlice<u8> source,
                                 u32 source_bytes, const u32* palette, u32 palette_count) noexcept {
    HRESULT result = create_png_resources(gpu, device);
    if (SUCCEEDED(result)) {
        result = ensure_dynamic_raw_buffer(device, source_bytes, &gpu->png_scanline_buffer,
                                           &gpu->png_scanline_view, &gpu->png_scanline_capacity);
    }
    if (FAILED(result)) {
        return result;
    }
    result = upload_discard(context, gpu->png_scanline_buffer, source.data, source_bytes);
    return SUCCEEDED(result) && palette_count != 0
               ? upload_discard(context, gpu->png_palette_buffer, palette, palette_count * sizeof(u32))
               : result;
}

HRESULT upload_png_inputs(MediaAtlasGpu* gpu, ID3D11Device* device, ID3D11DeviceContext1* context,
                          const PngImage& image, ConstSlice<u8> scanlines) noexcept {
    const bool indexed = image.color_type == 3;
    return upload_png_shader_inputs(gpu, device, context, scanlines, image.inflated_bytes,
                                    indexed ? image.palette_rgba : nullptr,
                                    indexed ? image.palette_count : 0);
}

HRESULT upload_gif_inputs(MediaAtlasGpu* gpu, ID3D11Device* device, ID3D11DeviceContext1* context,
                          const GifFrame& frame, const GifPalette& palette,
                          ConstSlice<u8> indices) noexcept {
    return upload_png_shader_inputs(gpu, device, context, indices, static_cast<u32>(frame.width) * frame.height,
                                    palette.rgba, palette.count);
}

HRESULT upload_animated_webp_inputs(MediaAtlasGpu* gpu, ID3D11Device* device,
                                    ID3D11DeviceContext1* context, const AnimatedWebpFrame& frame,
                                    ConstSlice<u8> rgba8) noexcept {
    return upload_png_shader_inputs(gpu, device, context, rgba8,
                                     static_cast<u32>(frame.width) * frame.height * 4u, nullptr, 0);
}

void bind_compute_2srv(ID3D11DeviceContext1* context, ID3D11ComputeShader* shader,
                       ID3D11Buffer* constants, ID3D11ShaderResourceView* resource0,
                       ID3D11ShaderResourceView* resource1,
                       ID3D11UnorderedAccessView* target) noexcept {
    ID3D11ShaderResourceView* resources[2] = {resource0, resource1};
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &constants);
    context->CSSetShaderResources(0, 2, resources);
    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
}

void unbind_compute_resources(ID3D11DeviceContext1* context) noexcept {
    ID3D11UnorderedAccessView* null_uav = nullptr;
    ID3D11ShaderResourceView* null_resources[2] = {nullptr, nullptr};
    ID3D11Buffer* null_buffer = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    context->CSSetShaderResources(0, 2, null_resources);
    context->CSSetConstantBuffers(0, 1, &null_buffer);
}

void unbind_compute(ID3D11DeviceContext1* context) noexcept {
    unbind_compute_resources(context);
    context->CSSetShader(nullptr, nullptr, 0);
}

void unbind_media_pixel_resources(ID3D11DeviceContext1* context) noexcept {
    ID3D11ShaderResourceView* null_views[3]{};
    context->PSSetShaderResources(0, 3, null_views);
}

void dispatch_png_shader(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                         const PngAtlasConstants& constants, ID3D11UnorderedAccessView* target,
                         u32 width, u32 height) noexcept {
    context->UpdateSubresource(gpu->png_constants, 0, nullptr, &constants, 0, 0);
    unbind_media_pixel_resources(context);
    bind_compute_2srv(context, gpu->png_shader, gpu->png_constants, gpu->png_scanline_view,
                      gpu->png_palette_view, target);
    context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
    unbind_compute(context);
}

void dispatch_png(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context, const PngImage& image,
                  u32 destination_x, u32 destination_y, ApngBlend blend) noexcept {
    const PngAtlasConstants constants{
        image.row_stride,       image.width,             image.height, image.bit_depth,
        image.color_type,       destination_x,           destination_y, image.palette_count,
        image.transparent_gray, image.transparent_red,   image.transparent_green,
        image.transparent_blue, {blend == ApngBlend::over ? 1u : 0u, 0, 0, 0},
    };
    dispatch_png_shader(gpu, context, constants, gpu->texture_uav, image.width, image.height);
}

void dispatch_gif(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context, const GifFrame& frame,
                  const GifPalette& palette, u32 destination_x, u32 destination_y) noexcept {
    const PngAtlasConstants constants{
        frame.width, frame.width, frame.height, 8, 8, destination_x, destination_y, palette.count, 0, 0,
        0,           0,           {1, 0, 0, 0},
    };
    dispatch_png_shader(gpu, context, constants, gpu->texture_uav, frame.width, frame.height);
}

void dispatch_animated_webp(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                            const AnimatedWebpFrame& frame, u32 destination_x, u32 destination_y) noexcept {
    const PngAtlasConstants constants{
        static_cast<u32>(frame.width) * 4u,
        frame.width,
        frame.height,
        8,
        7,
        destination_x,
        destination_y,
        0,
        0,
        0,
        0,
        0,
        {frame.blend == AnimatedWebpBlend::over ? 1u : 0u, 0, 0, 0},
    };
    dispatch_png_shader(gpu, context, constants, gpu->texture_uav, frame.width, frame.height);
}

HRESULT ensure_apng_backup(MediaAtlasGpu* gpu, ID3D11Device* device, ID3D11DeviceContext1* context,
                           MediaAtlas* atlas, u32 required_bottom) noexcept {
    if (atlas) {
        atlas->disposal_backup_used_ms = atlas->growth.now_ms;
    }
    D3D11_TEXTURE2D_DESC description{};
    u32 current = 0;
    if (gpu->apng_backup_texture) {
        gpu->apng_backup_texture->GetDesc(&description);
        current = description.Height;
    }
    if (current >= required_bottom) {
        return S_OK;
    }
    gpu->texture->GetDesc(&description);
    const u32 height = minimum<u32>(description.Height, (required_bottom + 255u) & ~255u);
    if (height < required_bottom) {
        return E_INVALIDARG;
    }
    description.Height = height;
    description.BindFlags = 0;
    description.CPUAccessFlags = 0;
    description.MiscFlags = 0;
    ID3D11Texture2D* replacement = nullptr;
    const HRESULT result = device->CreateTexture2D(&description, nullptr, &replacement);
    if (FAILED(result)) {
        return result;
    }
    if (current) {
        const D3D11_BOX box{0, 0, 0, description.Width, current, 1};
        context->CopySubresourceRegion(replacement, 0, 0, 0, 0, gpu->apng_backup_texture, 0, &box);
    }
    com_release(gpu->apng_backup_texture);
    gpu->apng_backup_texture = replacement;
    return S_OK;
}

void unbind_apng_atlas(ID3D11DeviceContext1* context) noexcept {
    ID3D11UnorderedAccessView* null_uav = nullptr;
    unbind_media_pixel_resources(context);
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
}

void copy_animation_region(ID3D11DeviceContext1* context, ID3D11Texture2D* destination,
                           ID3D11Texture2D* source, const MediaAtlasResult& allocation,
                           u32 x_offset, u32 y_offset, u32 width, u32 height) noexcept {
    const D3D11_BOX box{
        allocation.x + x_offset,
        allocation.y + y_offset,
        0,
        allocation.x + x_offset + width,
        allocation.y + y_offset + height,
        1,
    };
    context->CopySubresourceRegion(destination, 0, box.left, box.top, 0, source, 0, &box);
}

void clear_animation_region(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                            const MediaAtlasResult& allocation, u32 x_offset, u32 y_offset,
                            u32 width, u32 height, const float* color) noexcept {
    const D3D11_RECT rectangle{
        static_cast<LONG>(allocation.x + x_offset),
        static_cast<LONG>(allocation.y + y_offset),
        static_cast<LONG>(allocation.x + x_offset + width),
        static_cast<LONG>(allocation.y + y_offset + height),
    };
    unbind_media_pixel_resources(context);
    context->ClearView(gpu->target, color, &rectangle, 1);
}

void account_apng_dispatch(MediaAtlasGpu* gpu, const ApngFrame& frame, const PngImage& image) noexcept {
    ++gpu->apng_frames;
    if (frame.blend == ApngBlend::over) {
        ++gpu->apng_over_blends;
    } else {
        ++gpu->apng_source_blends;
    }
    const u64 pixels = static_cast<u64>(image.width) * image.height;
    saturating_accumulate(gpu->apng_dispatched_pixels, pixels);
    saturating_accumulate(gpu->apng_uploaded_bytes, image.inflated_bytes);
}

void account_gif_dispatch(MediaAtlasGpu* gpu, const GifFrame& frame) noexcept {
    ++gpu->gif_frames;
    const u64 pixels = static_cast<u64>(frame.width) * frame.height;
    saturating_accumulate(gpu->gif_uploaded_bytes, pixels);
    saturating_accumulate(gpu->gif_dispatched_pixels, pixels);
}

void premultiplied_clear_color(u32 rgba, float* color) noexcept {
    const u32 alpha = rgba >> 24;
    color[0] = static_cast<float>(((rgba & 255u) * alpha + 127u) / 255u) / 255.0f;
    color[1] = static_cast<float>((((rgba >> 8) & 255u) * alpha + 127u) / 255u) / 255.0f;
    color[2] = static_cast<float>((((rgba >> 16) & 255u) * alpha + 127u) / 255u) / 255.0f;
    color[3] = static_cast<float>(alpha) / 255.0f;
}

void clear_animated_webp_canvas(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                                const MediaAtlasResult& allocation, u32 background_rgba) noexcept {
    float color[4]{};
    premultiplied_clear_color(background_rgba, color);
    clear_animation_region(gpu, context, allocation, 0, 0, allocation.width, allocation.height, color);
}

void clear_animated_webp_region(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context,
                                const MediaAtlasResult& allocation, const AnimatedWebpFrame& frame,
                                u32 background_rgba) noexcept {
    float color[4]{};
    premultiplied_clear_color(background_rgba, color);
    clear_animation_region(gpu, context, allocation, frame.x_offset, frame.y_offset, frame.width,
                           frame.height, color);
}

void account_animated_webp_dispatch(MediaAtlasGpu* gpu, const AnimatedWebpFrame& frame,
                                    u64 uploaded_bytes) noexcept {
    ++gpu->animated_webp_frames;
    if (frame.blend == AnimatedWebpBlend::source) {
        ++gpu->animated_webp_source_blends;
    } else {
        ++gpu->animated_webp_over_blends;
    }
    const u64 pixels = static_cast<u64>(frame.width) * frame.height;
    saturating_accumulate(gpu->animated_webp_uploaded_bytes, uploaded_bytes);
    saturating_accumulate(gpu->animated_webp_dispatched_pixels, pixels);
}

bool vp8_atlas_frame_ready(const MediaAtlasGpu* gpu, const Vp8Keyframe& frame,
                           ConstSlice<u8> alpha) noexcept {
    const u64 pixel_count = static_cast<u64>(frame.width) * frame.height;
    return gpu && gpu->vp8_yuv_view && frame.width != 0 && frame.height != 0 &&
           gpu->vp8_width == frame.width && gpu->vp8_height == frame.height &&
           gpu->vp8_last_filtered_reconstruction == gpu->vp8_reconstructions &&
           gpu->vp8_last_atlas_reconstruction != gpu->vp8_reconstructions &&
           (alpha.count == 0 || (alpha.data && alpha.count == pixel_count));
}

HRESULT upload_vp8_alpha(MediaAtlasGpu* gpu, ID3D11Device* device, ID3D11DeviceContext1* context,
                         ConstSlice<u8> alpha) noexcept {
    HRESULT result = create_vp8_atlas_resources(gpu, device);
    if (SUCCEEDED(result) && alpha.count != 0) {
        result = ensure_dynamic_raw_buffer(device, static_cast<u32>(alpha.count), &gpu->png_scanline_buffer,
                                           &gpu->png_scanline_view, &gpu->png_scanline_capacity);
        if (SUCCEEDED(result)) {
            result = upload_discard(context, gpu->png_scanline_buffer, alpha.data, static_cast<u32>(alpha.count));
        }
    }
    return result;
}

void dispatch_vp8_atlas(MediaAtlasGpu* gpu, ID3D11DeviceContext1* context, const Vp8Keyframe& frame,
                        ConstSlice<u8> alpha, u32 destination_x, u32 destination_y,
                        bool blend_over, ID3D11UnorderedAccessView* texture_uav) noexcept {
    const u32 luma_samples = static_cast<u32>(gpu->vp8_luma_stride) * gpu->vp8_luma_rows;
    const u32 chroma_samples = static_cast<u32>(gpu->vp8_chroma_stride) * gpu->vp8_chroma_rows;
    const Vp8AtlasConstants constants{
        frame.width,
        frame.height,
        gpu->vp8_luma_stride,
        gpu->vp8_chroma_stride,
        luma_samples,
        luma_samples + chroma_samples,
        destination_x,
        destination_y,
        alpha.count != 0 ? 1u : 0u,
        frame.width,
        blend_over ? 1u : 0u,
        {0, 0, 0, 0, 0},
    };
    context->UpdateSubresource(gpu->vp8_atlas_constants, 0, nullptr, &constants, 0, 0);
    unbind_media_pixel_resources(context);
    bind_compute_2srv(context, gpu->vp8_atlas_shader, gpu->vp8_atlas_constants,
                      gpu->vp8_yuv_view, alpha.count != 0 ? gpu->png_scanline_view : nullptr,
                      texture_uav);
    context->Dispatch((frame.width + 7u) / 8u, (frame.height + 7u) / 8u, 1);
    unbind_compute(context);

    gpu->vp8_last_atlas_reconstruction = gpu->vp8_reconstructions;
    ++gpu->vp8_atlas_uploads;
    const u64 pixel_count = static_cast<u64>(frame.width) * frame.height;
    saturating_accumulate(gpu->vp8_atlas_pixels, pixel_count);
    if (alpha.count != 0) {
        ++gpu->vp8_alpha_uploads;
        saturating_accumulate(gpu->vp8_alpha_uploaded_bytes, alpha.count);
    }
}

} // namespace maicord::media_atlas_gpu_detail
