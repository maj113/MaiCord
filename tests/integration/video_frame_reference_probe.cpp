// video_frame.hlsl (shared-chroma fast path for even steps) must match the
// frozen per-pixel converter for every step, crop, range and matrix, on both
// the direct plane-view path and the scratch-copy path.
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/render/video_frame_gpu.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/d3d11.hpp"

#include "video_frame_reference.hpp"

namespace maicord {
namespace {
constexpr u32 kCodedWidth = 196, kCodedHeight = 132;
// Every case owns one cell of a single target, so each side needs one readback.
constexpr u32 kCellWidth = 200, kCellHeight = 140, kColumns = 10, kRows = 7;
constexpr u32 kTargetWidth = kCellWidth * kColumns, kTargetHeight = kCellHeight * kRows;

bool read_target(ID3D11DeviceContext1* context, ID3D11Texture2D* staging, ID3D11Texture2D* target,
                 u64* hash) noexcept {
    context->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    u64 value = kHash64Seed;
    for (u32 y = 0; y < kTargetHeight; ++y)
        value = hash64_counted_bytes(value, static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch,
                                     kTargetWidth * 4);
    context->Unmap(staging, 0);
    *hash = value;
    return true;
}

// Each turned picture equals the upright one read through the inverse
// mapping, on the per-pixel and the shared-chroma (even step) paths, and
// writes nothing past its transposed extent.
bool rotation_matches(ID3D11Device* device, ID3D11DeviceContext1* context, VideoFrameGpu* converter,
                      const GpuVideoFrame& upright, ID3D11UnorderedAccessView* view, ID3D11Texture2D* target,
                      ID3D11Texture2D* staging) noexcept {
    constexpr u32 kPitch = 80, kClear = 0x44332211u;
    const UINT clear[4]{0x11, 0x22, 0x33, 0x44};
    context->ClearUnorderedAccessViewUint(view, clear);
    for (u32 step = 1; step <= 2; ++step)
        for (u8 rotation = 0; rotation < 4; ++rotation) {
            GpuVideoFrame frame = upright;
            frame.rotation = rotation;
            if (FAILED(video_frame_gpu_present(converter, device, context, frame, view, rotation * kPitch + 2,
                                               (step - 1) * kPitch + 2, step)))
                return false;
        }
    context->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const auto pixel = [&](u32 x, u32 y) noexcept {
        u32 value{};
        copy_bytes(&value, static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4u, 4);
        return value;
    };
    bool valid = true;
    for (u32 step = 1; valid && step <= 2; ++step) {
        const u32 width = gpu_video_frame_reduced(upright.width, step);
        const u32 height = gpu_video_frame_reduced(upright.height, step);
        const u32 top = (step - 1) * kPitch + 2;
        for (u32 rotation = 1; valid && rotation < 4; ++rotation) {
            const bool turned = (rotation & 1) != 0;
            const u32 left = rotation * kPitch + 2, written_width = turned ? height : width,
                      written_height = turned ? width : height;
            for (u32 y = 0; valid && y < written_height; ++y)
                for (u32 x = 0; valid && x < written_width; ++x) {
                    const u32 sx = rotation == 1 ? y : rotation == 2 ? width - 1 - x : width - 1 - y;
                    const u32 sy = rotation == 1 ? height - 1 - x : rotation == 2 ? height - 1 - y : x;
                    valid = pixel(left + x, top + y) == pixel(2 + sx, top + sy);
                }
            valid = valid && pixel(left + written_width, top) == kClear &&
                    pixel(left, top + written_height) == kClear;
        }
    }
    context->Unmap(staging, 0);
    return valid;
}
} // namespace

bool video_frame_reference_probe(D3D11Renderer& renderer) noexcept {
    auto* device = renderer.graphics.device;
    auto* context = renderer.graphics.context1;
    static u8 pixels[kCodedWidth * kCodedHeight * 3 / 2];
    const u32 luma_bytes = kCodedWidth * kCodedHeight;
    for (u32 y = 0; y < kCodedHeight; ++y)
        for (u32 x = 0; x < kCodedWidth; ++x)
            pixels[y * kCodedWidth + x] = static_cast<u8>(x * 37u + y * 71u + (x ^ y) * 12u);
    for (u32 y = 0; y < kCodedHeight / 2; ++y)
        for (u32 x = 0; x < kCodedWidth; ++x)
            pixels[luma_bytes + y * kCodedWidth + x] = static_cast<u8>(x * 53u + y * 29u + (x ^ y) * 6u);
    // Range extremes in Y, Cb and Cr at the first visible samples.
    pixels[0] = pixels[luma_bytes] = pixels[luma_bytes + 1] = 0;
    pixels[1] = pixels[luma_bytes + 2] = pixels[luma_bytes + 3] = 255;

    ID3D11Texture2D* source = nullptr;
    ID3D11ShaderResourceView* planes[2]{};
    ID3D11Texture2D* target = nullptr;
    ID3D11UnorderedAccessView* target_view = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11ComputeShader* reference = nullptr;
    VideoFrameGpu converter{};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kCodedWidth;
    desc.Height = kCodedHeight;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{pixels, kCodedWidth, 0};
    bool valid = SUCCEEDED(device->CreateTexture2D(&desc, &initial, &source));
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = 1;
    view.Format = DXGI_FORMAT_R8_UNORM;
    valid = valid && SUCCEEDED(device->CreateShaderResourceView(source, &view, &planes[0]));
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    valid = valid && SUCCEEDED(device->CreateShaderResourceView(source, &view, &planes[1]));
    desc.Width = kTargetWidth;
    desc.Height = kTargetHeight;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    valid = valid && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target)) &&
            SUCCEEDED(device->CreateUnorderedAccessView(target, nullptr, &target_view));
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    valid = valid && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)) &&
            SUCCEEDED(device->CreateComputeShader(g_video_frame_reference, sizeof(g_video_frame_reference), nullptr,
                                                  &reference));

    struct Geometry { u32 crop_x, crop_y, width, height; };
    constexpr Geometry geometries[] = {{0, 0, kCodedWidth, kCodedHeight}, {2, 4, 189, 121}, {6, 2, 64, 63}};
    u32 cases = 0;
    u64 hashes[2]{};
    ID3D11ComputeShader* production = nullptr;
    for (u32 side = 0; valid && side < 2; ++side) {
        if (side) {
            production = converter.shader;
            converter.shader = reference;
        }
        const UINT clear[4]{17, 34, 51, 68};
        context->ClearUnorderedAccessViewUint(target_view, clear);
        u32 cell = 0;
        for (const auto& geometry : geometries) {
            for (u32 step = 1; valid && step <= 5; ++step) {
                // Matrix/range and direct/copy path; cells stay inside the target.
                for (u32 variant = 0; valid && variant < 4; ++variant, ++cell) {
                    GpuVideoFrame frame{source, 0, geometry.crop_x, geometry.crop_y, geometry.width,
                                        geometry.height, (variant & 1) != 0, (variant & 1) != 0, {}};
                    if (variant & 2) {
                        frame.luma = planes[0];
                        frame.chroma = planes[1];
                    }
                    valid = cell < kColumns * kRows &&
                            SUCCEEDED(video_frame_gpu_present(&converter, device, context, frame, target_view,
                                                              cell % kColumns * kCellWidth + 1 + cell % 3,
                                                              cell / kColumns * kCellHeight + 1 + cell % 5, step));
                }
            }
        }
        cases = cell;
        valid = valid && read_target(context, staging, target, &hashes[side]);
    }
    if (production) converter.shader = production;
    valid = valid && production && hashes[0] == hashes[1] && cases == 60;
    const bool rotated = valid && rotation_matches(device, context, &converter,
                                                   GpuVideoFrame{source, 0, 6, 2, 64, 63, false, false, 0, {}},
                                                   target_view, target, staging);
    log_u64(rotated ? LogLevel::info : LogLevel::error, "video_frame_reference", "rotation_exact", rotated);
    valid = valid && rotated;
    video_frame_gpu_release(&converter);
    com_release(reference);
    com_release(staging);
    com_release(target_view);
    com_release(target);
    com_release(planes[1]);
    com_release(planes[0]);
    com_release(source);
    log_u64(valid ? LogLevel::info : LogLevel::error, "video_frame_reference", "exact_cases", cases);
    return valid;
}
} // namespace maicord
