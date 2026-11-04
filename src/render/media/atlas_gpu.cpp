#include "maicord/render/media/atlas_gpu.hpp"

#include "maicord/gpu/api.hpp"
#include "atlas_gpu_internal.hpp"
// Resource creation and decode validation, once per image: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace media_atlas_gpu_detail {

MediaAtlasGpuResult failure(MediaAtlasGpuError error, GpuStatus native_error,
                            MediaAtlasResult allocation) noexcept {
    return MediaAtlasGpuResult{allocation, native_error, error};
}

MediaAtlasGpuResult reject(MediaAtlasGpu* gpu, MediaAtlasGpuError error, GpuStatus native_error,
                           MediaAtlasResult allocation) noexcept {
    if (gpu) {
        ++gpu->rejected;
    }
    return failure(error, native_error, allocation);
}

bool dimensions_valid(u16 width, u16 height) noexcept {
    return width >= 16 && height >= 16 && width <= 4'096 && height <= 4'096;
}

// Upload resources grow geometrically: exact-size growth would allocate a new
// D3D resource for every image in a sequence of gradually larger ones.
static u32 retained_upload_capacity(u32 current, u32 required) noexcept {
    if (required == 0 || required > 0xFFFFFFFCu) {
        return 0;
    }
    u32 capacity = current >= 4'096u ? current : 4'096u;
    while (capacity < required && capacity <= 0x7FFFFFFCu) {
        capacity *= 2u;
    }
    if (capacity < required) {
        capacity = (required + 3u) & ~3u;
    }
    return capacity;
}


bool png_format_valid(const PngImage& image) noexcept {
    u32 channels = 0;
    bool depth_valid = false;
    switch (image.color_type) {
    case 0:
        channels = 1;
        depth_valid = image.bit_depth == 1 || image.bit_depth == 2 || image.bit_depth == 4 ||
                      image.bit_depth == 8 || image.bit_depth == 16;
        break;
    case 2:
        channels = 3;
        depth_valid = image.bit_depth == 8 || image.bit_depth == 16;
        break;
    case 3:
        channels = 1;
        depth_valid =
            image.bit_depth == 1 || image.bit_depth == 2 || image.bit_depth == 4 || image.bit_depth == 8;
        break;
    case 4:
        channels = 2;
        depth_valid = image.bit_depth == 8 || image.bit_depth == 16;
        break;
    case 6:
        channels = 4;
        depth_valid = image.bit_depth == 8 || image.bit_depth == 16;
        break;
    }
    const u64 row_bits = static_cast<u64>(image.width) * channels * image.bit_depth;
    const u64 row_bytes = (row_bits + 7) / 8;
    const u64 inflated = (row_bytes + 1) * image.height;
    return depth_valid && image.width != 0 && image.height != 0 && image.width <= kPngDimensionMaximum &&
           image.height <= kPngDimensionMaximum && image.channels == channels &&
           image.row_bytes == row_bytes && image.row_stride == row_bytes + 1 &&
           image.inflated_bytes == inflated && inflated <= kPngInflatedMaximumBytes &&
           (image.color_type != 3 ||
             (image.palette_count != 0 && image.palette_count <= kPngPaletteCapacity));
}

bool png_scanlines_reconstructed(const PngImage& image, ConstSlice<u8> scanlines) noexcept {
    if (!scanlines.data || scanlines.count < image.inflated_bytes) {
        return false;
    }
    for (u32 y = 0; y < image.height; ++y) {
        if (scanlines[y * image.row_stride] != 0) {
            return false;
        }
    }
    return true;
}

// A constant buffer and the compute shader that reads it, created together
// unless both already exist.
static GpuStatus ensure_constants_and_shader(GpuDevice* device, u32 constant_bytes, ShaderId shader_id,
                                             GpuBuffer** constants, GpuComputeShader** shader) noexcept {
    if (*constants && *shader) {
        return S_OK;
    }
    GpuBuffer* replacement_constants = nullptr;
    GpuComputeShader* replacement_shader = nullptr;
    GpuStatus result = gpu_buffer_create(device, {constant_bytes, GpuUsage::default_, gpu_bind_constant, false}, nullptr,
                                         &replacement_constants);
    if (SUCCEEDED(result)) {
        result = gpu_compute_shader_create(device, shader_id, &replacement_shader);
    }
    if (FAILED(result)) {
        gpu_release(replacement_shader);
        gpu_release(replacement_constants);
        return result;
    }
    gpu_release(*shader);
    gpu_release(*constants);
    *constants = replacement_constants;
    *shader = replacement_shader;
    return S_OK;
}

GpuStatus create_png_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept {
    if (!gpu->png_palette_buffer || !gpu->png_palette_view) {
        gpu_release(gpu->png_palette_view);
        gpu_release(gpu->png_palette_buffer);
        GpuStatus result = gpu_buffer_create(device,
                                             {kPngPaletteCapacity * sizeof(u32), GpuUsage::dynamic, gpu_bind_srv, false},
                                             nullptr, &gpu->png_palette_buffer);
        if (SUCCEEDED(result)) {
            result = gpu_srv_create(device, gpu->png_palette_buffer,
                                    {GpuViewKind::buffer_typed, GpuFormat::r32_uint, kPngPaletteCapacity},
                                    &gpu->png_palette_view);
        }
        if (FAILED(result)) {
            gpu_release(gpu->png_palette_view);
            gpu_release(gpu->png_palette_buffer);
            return result;
        }
    }
    return ensure_constants_and_shader(device, sizeof(PngAtlasConstants), ShaderId::png_compute,
                                       &gpu->png_constants, &gpu->png_shader);
}

// A raw (byte-address) buffer and its view: CPU-writable and read-only for the
// upload buffers, GPU-writable with a UAV when `uav` is given. Nothing is left
// behind on failure.
static GpuStatus create_raw_buffer(GpuDevice* device, u32 bytes, GpuBuffer** buffer,
                                   GpuSrv** view, GpuUav** uav) noexcept {
    GpuStatus result = gpu_buffer_create(
        device,
        {bytes, uav ? GpuUsage::default_ : GpuUsage::dynamic,
         static_cast<u8>(gpu_bind_srv | (uav ? gpu_bind_uav : 0)), true},
        nullptr, buffer);
    if (SUCCEEDED(result)) {
        result = gpu_srv_create(device, *buffer, {GpuViewKind::buffer_raw, GpuFormat::r32_uint, bytes / 4u}, view);
    }
    if (SUCCEEDED(result) && uav) {
        result = gpu_uav_create(device, *buffer, bytes / 4u, uav);
    }
    if (FAILED(result)) {
        if (uav) {
            gpu_release(*uav);
        }
        gpu_release(*view);
        gpu_release(*buffer);
    }
    return result;
}

// Dynamic buffers live in CPU-visible memory mapped into the process. One
// that a large image grew past this is replaced by the next small upload.
inline constexpr u32 kRetainedDynamicUploadBytes = 4u * 1024u * 1024u;

GpuStatus ensure_dynamic_raw_buffer(GpuDevice* device, u32 required_bytes, GpuBuffer** buffer,
                                    GpuSrv** view, u32* capacity) noexcept {
    const bool shrink = *capacity > kRetainedDynamicUploadBytes && required_bytes <= *capacity / 4u;
    if (*buffer && *view && *capacity >= required_bytes && !shrink) {
        return S_OK;
    }
    const u32 aligned = retained_upload_capacity(shrink ? 0u : *capacity, required_bytes);
    if (aligned == 0) {
        return E_INVALIDARG;
    }
    GpuBuffer* replacement_buffer = nullptr;
    GpuSrv* replacement_view = nullptr;
    const GpuStatus result = create_raw_buffer(device, aligned, &replacement_buffer, &replacement_view, nullptr);
    if (FAILED(result)) {
        // A failed shrink keeps the larger buffer.
        return shrink && *buffer && *view ? S_OK : result;
    }
    gpu_release(*view);
    gpu_release(*buffer);
    *buffer = replacement_buffer;
    *view = replacement_view;
    *capacity = aligned;
    return S_OK;
}

GpuStatus ensure_vp8_yuv_buffer(MediaAtlasGpu* gpu, GpuDevice* device, u32 required_bytes) noexcept {
    if (gpu->vp8_yuv_buffer && gpu->vp8_yuv_view && gpu->vp8_yuv_uav &&
        gpu->vp8_yuv_capacity >= required_bytes) {
        return S_OK;
    }
    const u32 aligned = retained_upload_capacity(gpu->vp8_yuv_capacity, required_bytes);
    if (aligned == 0) {
        return E_INVALIDARG;
    }
    GpuBuffer* replacement_buffer = nullptr;
    GpuSrv* replacement_view = nullptr;
    GpuUav* replacement_uav = nullptr;
    const GpuStatus result =
        create_raw_buffer(device, aligned, &replacement_buffer, &replacement_view, &replacement_uav);
    if (FAILED(result)) {
        return result;
    }
    gpu_release(gpu->vp8_yuv_uav);
    gpu_release(gpu->vp8_yuv_view);
    gpu_release(gpu->vp8_yuv_buffer);
    gpu->vp8_yuv_buffer = replacement_buffer;
    gpu->vp8_yuv_view = replacement_view;
    gpu->vp8_yuv_uav = replacement_uav;
    gpu->vp8_yuv_capacity = aligned;
    return S_OK;
}

GpuStatus create_vp8_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept {
    const GpuStatus result = ensure_constants_and_shader(device, sizeof(Vp8ReconstructionConstants),
                                                       ShaderId::vp8_reconstruct, &gpu->vp8_constants,
                                                       &gpu->vp8_reconstruct_shader);
    return SUCCEEDED(result) && !gpu->vp8_idct_shader
               ? gpu_compute_shader_create(device, ShaderId::vp8_idct, &gpu->vp8_idct_shader)
               : result;
}

GpuStatus create_vp8_filter_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept {
    return ensure_constants_and_shader(device, sizeof(Vp8FilterConstants), ShaderId::vp8_filter,
                                       &gpu->vp8_filter_constants, &gpu->vp8_filter_shader);
}

GpuStatus create_vp8_atlas_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept {
    return ensure_constants_and_shader(device, sizeof(Vp8AtlasConstants), ShaderId::vp8_atlas,
                                       &gpu->vp8_atlas_constants, &gpu->vp8_atlas_shader);
}

GpuStatus create_jpeg_resources(MediaAtlasGpu* gpu, GpuDevice* device) noexcept {
    const GpuStatus result = ensure_constants_and_shader(device, sizeof(JpegIdctConstants), ShaderId::jpeg_idct,
                                                       &gpu->jpeg_idct_constants, &gpu->jpeg_idct_shader);
    return FAILED(result) ? result
                          : ensure_constants_and_shader(device, sizeof(JpegAtlasConstants), ShaderId::jpeg_atlas,
                                                        &gpu->jpeg_atlas_constants, &gpu->jpeg_atlas_shader);
}


GpuStatus create_media_surface(GpuDevice* device, GpuContext1* context, u16 width, u16 height,
                               GpuTexture** texture, GpuSrv** texture_view,
                               GpuRtv** target,
                               GpuUav** texture_uav) noexcept {
    GpuStatus result = gpu_format_check(device, GpuFormat::rgba8,
                                        static_cast<u8>(gpu_bind_srv | gpu_bind_rtv | gpu_bind_uav));
    if (!gpu_ok(result)) {
        return result == kGpuUnsupported ? kGpuFail : result;
    }
    result = gpu_texture_create(
        device, {width, height, GpuFormat::rgba8, GpuUsage::default_, gpu_bind_srv | gpu_bind_rtv | gpu_bind_uav},
        nullptr, texture);
    if (SUCCEEDED(result)) {
        result = gpu_srv_create(device, *texture, {GpuViewKind::texture, GpuFormat::rgba8}, texture_view);
    }
    if (SUCCEEDED(result)) {
        result = gpu_rtv_create(device, *texture, target);
    }
    if (SUCCEEDED(result)) {
        result = gpu_uav_create(device, *texture, texture_uav);
    }
    if (SUCCEEDED(result)) {
        const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        gpu_clear_rtv(context, *target, transparent);
    }
    return result;
}

} // namespace media_atlas_gpu_detail
} // namespace maicord
#pragma clang attribute pop
