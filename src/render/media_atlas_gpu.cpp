#include "maicord/render/media_atlas_gpu.hpp"

#include "maicord/render/shader_bytecode.hpp"
#include "media_atlas_gpu_internal.hpp"

namespace maicord {
namespace media_atlas_gpu_detail {

MediaAtlasGpuResult failure(MediaAtlasGpuError error, HRESULT native_error,
                            MediaAtlasResult allocation) noexcept {
    return MediaAtlasGpuResult{allocation, native_error, error};
}

MediaAtlasGpuResult reject(MediaAtlasGpu* gpu, MediaAtlasGpuError error, HRESULT native_error,
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
u32 retained_upload_capacity(u32 current, u32 required) noexcept {
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

HRESULT create_png_scanline_buffer(MediaAtlasGpu* gpu, ID3D11Device* device, u32 required_bytes) noexcept {
    return ensure_dynamic_raw_buffer(device, required_bytes, &gpu->png_scanline_buffer,
                                     &gpu->png_scanline_view, &gpu->png_scanline_capacity);
}

HRESULT create_png_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept {
    if (gpu->png_palette_buffer && gpu->png_palette_view && gpu->png_constants && gpu->png_shader) {
        return S_OK;
    }
    com_release(gpu->png_shader);
    com_release(gpu->png_constants);
    com_release(gpu->png_palette_view);
    com_release(gpu->png_palette_buffer);

    D3D11_BUFFER_DESC palette_description{};
    palette_description.ByteWidth = kPngPaletteCapacity * sizeof(u32);
    palette_description.Usage = D3D11_USAGE_DYNAMIC;
    palette_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    palette_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT result = device->CreateBuffer(&palette_description, nullptr, &gpu->png_palette_buffer);
    if (SUCCEEDED(result)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC palette_view{};
        palette_view.Format = DXGI_FORMAT_R32_UINT;
        palette_view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        palette_view.Buffer.NumElements = kPngPaletteCapacity;
        result = device->CreateShaderResourceView(gpu->png_palette_buffer, &palette_view,
                                                  &gpu->png_palette_view);
    }

    D3D11_BUFFER_DESC constant_description{};
    constant_description.ByteWidth = sizeof(PngAtlasConstants);
    constant_description.Usage = D3D11_USAGE_DEFAULT;
    constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result)) {
        result = device->CreateBuffer(&constant_description, nullptr, &gpu->png_constants);
    }
    if (SUCCEEDED(result)) {
        result = shader_create_compute(device, ShaderId::png_compute, &gpu->png_shader);
    }
    if (FAILED(result)) {
        com_release(gpu->png_shader);
        com_release(gpu->png_constants);
        com_release(gpu->png_palette_view);
        com_release(gpu->png_palette_buffer);
    }
    return result;
}

// Dynamic buffers live in CPU-visible memory mapped into the process. One
// that a large image grew past this is replaced by the next small upload.
inline constexpr u32 kRetainedDynamicUploadBytes = 4u * 1024u * 1024u;

HRESULT ensure_dynamic_raw_buffer(ID3D11Device* device, u32 required_bytes, ID3D11Buffer** buffer,
                                  ID3D11ShaderResourceView** view, u32* capacity) noexcept {
    const bool shrink = *capacity > kRetainedDynamicUploadBytes && required_bytes <= *capacity / 4u;
    if (*buffer && *view && *capacity >= required_bytes && !shrink) {
        return S_OK;
    }
    const u32 aligned = retained_upload_capacity(shrink ? 0u : *capacity, required_bytes);
    if (aligned == 0) {
        return E_INVALIDARG;
    }
    ID3D11Buffer* replacement_buffer = nullptr;
    ID3D11ShaderResourceView* replacement_view = nullptr;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = aligned;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    HRESULT result = device->CreateBuffer(&description, nullptr, &replacement_buffer);
    if (SUCCEEDED(result)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view_description{};
        view_description.Format = DXGI_FORMAT_R32_TYPELESS;
        view_description.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view_description.BufferEx.NumElements = aligned / 4u;
        view_description.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        result = device->CreateShaderResourceView(replacement_buffer, &view_description, &replacement_view);
    }
    if (FAILED(result)) {
        com_release(replacement_view);
        com_release(replacement_buffer);
        // A failed shrink keeps the larger buffer.
        return shrink && *buffer && *view ? S_OK : result;
    }
    com_release(*view);
    com_release(*buffer);
    *buffer = replacement_buffer;
    *view = replacement_view;
    *capacity = aligned;
    return S_OK;
}

HRESULT ensure_vp8_yuv_buffer(MediaAtlasGpu* gpu, ID3D11Device* device, u32 required_bytes) noexcept {
    if (gpu->vp8_yuv_buffer && gpu->vp8_yuv_view && gpu->vp8_yuv_uav &&
        gpu->vp8_yuv_capacity >= required_bytes) {
        return S_OK;
    }
    const u32 aligned = retained_upload_capacity(gpu->vp8_yuv_capacity, required_bytes);
    if (aligned == 0) {
        return E_INVALIDARG;
    }
    ID3D11Buffer* replacement_buffer = nullptr;
    ID3D11ShaderResourceView* replacement_view = nullptr;
    ID3D11UnorderedAccessView* replacement_uav = nullptr;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = aligned;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    HRESULT result = device->CreateBuffer(&description, nullptr, &replacement_buffer);
    if (SUCCEEDED(result)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view_description{};
        view_description.Format = DXGI_FORMAT_R32_TYPELESS;
        view_description.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view_description.BufferEx.NumElements = aligned / 4u;
        view_description.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        result = device->CreateShaderResourceView(replacement_buffer, &view_description, &replacement_view);
    }
    if (SUCCEEDED(result)) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav_description{};
        uav_description.Format = DXGI_FORMAT_R32_TYPELESS;
        uav_description.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uav_description.Buffer.NumElements = aligned / 4u;
        uav_description.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        result = device->CreateUnorderedAccessView(replacement_buffer, &uav_description, &replacement_uav);
    }
    if (FAILED(result)) {
        com_release(replacement_uav);
        com_release(replacement_view);
        com_release(replacement_buffer);
        return result;
    }
    com_release(gpu->vp8_yuv_uav);
    com_release(gpu->vp8_yuv_view);
    com_release(gpu->vp8_yuv_buffer);
    gpu->vp8_yuv_buffer = replacement_buffer;
    gpu->vp8_yuv_view = replacement_view;
    gpu->vp8_yuv_uav = replacement_uav;
    gpu->vp8_yuv_capacity = aligned;
    return S_OK;
}

HRESULT create_vp8_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept {
    if (gpu->vp8_constants && gpu->vp8_reconstruct_shader && gpu->vp8_idct_shader) {
        return S_OK;
    }
    ID3D11Buffer* replacement_constants = nullptr;
    ID3D11ComputeShader* replacement_shader = nullptr;
    ID3D11ComputeShader* replacement_idct = nullptr;
    D3D11_BUFFER_DESC constant_description{};
    constant_description.ByteWidth = sizeof(Vp8ReconstructionConstants);
    constant_description.Usage = D3D11_USAGE_DEFAULT;
    constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HRESULT result = device->CreateBuffer(&constant_description, nullptr, &replacement_constants);
    if (SUCCEEDED(result)) {
        result = shader_create_compute(device, ShaderId::vp8_reconstruct, &replacement_shader);
    }
    if (SUCCEEDED(result)) result = shader_create_compute(device, ShaderId::vp8_idct, &replacement_idct);
    if (FAILED(result)) {
        com_release(replacement_idct);
        com_release(replacement_shader);
        com_release(replacement_constants);
        return result;
    }
    com_release(gpu->vp8_idct_shader);
    gpu->vp8_idct_shader = replacement_idct;
    com_release(gpu->vp8_reconstruct_shader);
    com_release(gpu->vp8_constants);
    gpu->vp8_constants = replacement_constants;
    gpu->vp8_reconstruct_shader = replacement_shader;
    return S_OK;
}

// A constant buffer and the compute shader that reads it, replaced together.
static HRESULT replace_constants_and_shader(ID3D11Device* device, u32 constant_bytes,
                                            ShaderId shader_id, ID3D11Buffer** constants,
                                            ID3D11ComputeShader** shader) noexcept {
    ID3D11Buffer* replacement_constants = nullptr;
    ID3D11ComputeShader* replacement_shader = nullptr;
    D3D11_BUFFER_DESC constant_description{};
    constant_description.ByteWidth = constant_bytes;
    constant_description.Usage = D3D11_USAGE_DEFAULT;
    constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HRESULT result = device->CreateBuffer(&constant_description, nullptr, &replacement_constants);
    if (SUCCEEDED(result)) {
        result = shader_create_compute(device, shader_id, &replacement_shader);
    }
    if (FAILED(result)) {
        com_release(replacement_shader);
        com_release(replacement_constants);
        return result;
    }
    com_release(*shader);
    com_release(*constants);
    *constants = replacement_constants;
    *shader = replacement_shader;
    return S_OK;
}

HRESULT create_vp8_filter_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept {
    if (gpu->vp8_filter_constants && gpu->vp8_filter_shader) {
        return S_OK;
    }
    return replace_constants_and_shader(device, sizeof(Vp8FilterConstants), ShaderId::vp8_filter,
                                        &gpu->vp8_filter_constants, &gpu->vp8_filter_shader);
}

HRESULT create_vp8_atlas_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept {
    if (gpu->vp8_atlas_constants && gpu->vp8_atlas_shader) {
        return S_OK;
    }
    return replace_constants_and_shader(device, sizeof(Vp8AtlasConstants), ShaderId::vp8_atlas,
                                        &gpu->vp8_atlas_constants, &gpu->vp8_atlas_shader);
}

HRESULT create_jpeg_resources(MediaAtlasGpu* gpu, ID3D11Device* device) noexcept {
    if (gpu->jpeg_idct_constants && gpu->jpeg_idct_shader && gpu->jpeg_atlas_constants &&
        gpu->jpeg_atlas_shader) {
        return S_OK;
    }
    ID3D11Buffer* idct_constants = nullptr;
    ID3D11ComputeShader* idct_shader = nullptr;
    ID3D11Buffer* atlas_constants = nullptr;
    ID3D11ComputeShader* atlas_shader = nullptr;
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(JpegIdctConstants);
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HRESULT result = device->CreateBuffer(&description, nullptr, &idct_constants);
    if (SUCCEEDED(result)) {
        result = shader_create_compute(device, ShaderId::jpeg_idct, &idct_shader);
    }
    description.ByteWidth = sizeof(JpegAtlasConstants);
    if (SUCCEEDED(result)) {
        result = device->CreateBuffer(&description, nullptr, &atlas_constants);
    }
    if (SUCCEEDED(result)) {
        result = shader_create_compute(device, ShaderId::jpeg_atlas, &atlas_shader);
    }
    if (FAILED(result)) {
        com_release(atlas_shader);
        com_release(atlas_constants);
        com_release(idct_shader);
        com_release(idct_constants);
        return result;
    }
    com_release(gpu->jpeg_atlas_shader);
    com_release(gpu->jpeg_atlas_constants);
    com_release(gpu->jpeg_idct_shader);
    com_release(gpu->jpeg_idct_constants);
    gpu->jpeg_idct_constants = idct_constants;
    gpu->jpeg_idct_shader = idct_shader;
    gpu->jpeg_atlas_constants = atlas_constants;
    gpu->jpeg_atlas_shader = atlas_shader;
    return S_OK;
}


HRESULT create_media_surface(ID3D11Device* device, ID3D11DeviceContext1* context, u16 width, u16 height,
                             ID3D11Texture2D** texture, ID3D11ShaderResourceView** texture_view,
                             ID3D11RenderTargetView** target,
                             ID3D11UnorderedAccessView** texture_uav) noexcept {
    if (!device || !context || !texture || !texture_view || !target || !texture_uav ||
        !dimensions_valid(width, height) || *texture || *texture_view || *target || *texture_uav) {
        return E_INVALIDARG;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

    UINT format_support = 0;
    HRESULT result = device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM, &format_support);
    constexpr UINT required_support = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                                      D3D11_FORMAT_SUPPORT_RENDER_TARGET |
                                      D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if (FAILED(result) || (format_support & required_support) != required_support) {
        return FAILED(result) ? result : E_FAIL;
    }
    result = device->CreateTexture2D(&description, nullptr, texture);
    if (SUCCEEDED(result)) {
        result = device->CreateShaderResourceView(*texture, nullptr, texture_view);
    }
    if (SUCCEEDED(result)) {
        result = device->CreateRenderTargetView(*texture, nullptr, target);
    }
    if (SUCCEEDED(result)) {
        result = device->CreateUnorderedAccessView(*texture, nullptr, texture_uav);
    }
    if (SUCCEEDED(result)) {
        const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->ClearRenderTargetView(*target, transparent);
    }
    return result;
}

} // namespace media_atlas_gpu_detail
} // namespace maicord
