#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"

#include <d3d11_1.h>

#include "vp8_reference_reconstruct.hpp"
#include "vp8_reference_idct.hpp"
#include "vp8_reference_filter.hpp"

namespace maicord {
namespace {
Vp8ModeWorkspace modes;
Vp8CoefficientWorkspace coefficients;
bool read_hash(ID3D11DeviceContext1* context, ID3D11Buffer* staging, ID3D11Buffer* source,
               u32 bytes, u64* hash) noexcept {
    context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    *hash = hash64_bytes(kHash64Seed, mapped.pData, bytes);
    context->Unmap(staging, 0);
    return true;
}
}
bool vp8_tiled_probe(D3D11Renderer& renderer) noexcept {
    auto* device = renderer.graphics.device;
    auto* context = renderer.graphics.context1;
    auto& gpu = renderer.media_gpu;
    ID3D11ComputeShader* reference = nullptr;
    ID3D11ComputeShader* reference_idct = nullptr;
    ID3D11ComputeShader* reference_filter = nullptr;
    bool valid = SUCCEEDED(device->CreateComputeShader(g_vp8_reference_reconstruct,
        sizeof(g_vp8_reference_reconstruct), nullptr, &reference)) &&
        SUCCEEDED(device->CreateComputeShader(g_vp8_reference_idct, sizeof(g_vp8_reference_idct), nullptr, &reference_idct)) &&
        SUCCEEDED(device->CreateComputeShader(g_vp8_reference_filter, sizeof(g_vp8_reference_filter), nullptr,
                                              &reference_filter));
    const u16 sizes[][2] = {{1,1},{17,31},{31,17},{16,512},{512,16},{128,128},{512,192},{511,509},{512,512}};
    u32 cases = 0, filter_cases = 0;
    u64 samples = 0;
    for (const auto& size : sizes) for (u32 pattern = 0; valid && pattern < 3; ++pattern) {
        const u32 columns = (size[0] + 15u) / 16u, rows = (size[1] + 15u) / 16u;
        modes.columns = coefficients.columns = static_cast<u16>(columns);
        modes.rows = coefficients.rows = static_cast<u16>(rows);
        modes.macroblock_count = coefficients.macroblock_count = columns * rows;
        for (u32 mb = 0; mb < columns * rows; ++mb) {
            auto& mode = modes.macroblocks[mb];
            mode = {};
            mode.luma = pattern == 0 ? static_cast<Vp8IntraMode>(mb % 4) :
                pattern == 1 || mb % 3 ? Vp8IntraMode::subblocks : static_cast<Vp8IntraMode>(mb % 4);
            mode.chroma = static_cast<Vp8IntraMode>((mb + pattern) % 4);
            mode.segment_id = static_cast<u8>(mb % 4);
            for (u32 block = 0; block < 16; ++block)
                mode.luma_subblock[block] = static_cast<Vp8SubblockMode>((mb * 7 + block + pattern) % 10);
            auto& coefficient = coefficients.macroblocks[mb];
            coefficient.has_coefficients = mb % 7 != 3;
            coefficient.reserved = 0;
            for (u32 block = 0; block < 24; ++block) for (u32 c = 0; c < 16; ++c) {
                const u32 seed = mb * 77 + block * 31 + c * 997 + pattern;
                coefficient.values[block][c] = c && seed % 5 ? 0 :
                    static_cast<i16>(static_cast<i32>(seed % 8192) - 4096);
            }
        }
        const Vp8Keyframe frame{{}, {}, 0, 0, size[0], size[1], 0, 0, 0, 0};
        valid = media_atlas_gpu_reconstruct_vp8_keyframe(&gpu, device, context, frame, modes, coefficients).ok();
        D3D11_BUFFER_DESC desc{};
        ID3D11Buffer* staging = nullptr;
        if (valid) {
            gpu.vp8_yuv_buffer->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
            valid = SUCCEEDED(device->CreateBuffer(&desc, nullptr, &staging));
        }
        const u32 bytes = columns * rows * 384u * sizeof(u32);
        u64 actual = 0, expected = 0;
        valid = valid && read_hash(context, staging, gpu.vp8_yuv_buffer, bytes, &actual);
        if (valid) {
            ID3D11ShaderResourceView* inputs[]{gpu.vp8_mode_view, gpu.vp8_coefficient_view};
            context->CSSetShaderResources(0, 2, inputs);
            context->CSSetConstantBuffers(0, 1, &gpu.vp8_constants);
            context->CSSetUnorderedAccessViews(0, 1, &gpu.vp8_yuv_uav, nullptr);
            context->CSSetShader(reference_idct, nullptr, 0);
            context->Dispatch((columns * rows * 24u + 63u) / 64u, 1, 1);
            context->CSSetShader(reference, nullptr, 0);
            context->Dispatch(1, 1, 1);
            ID3D11UnorderedAccessView* none = nullptr;
            ID3D11ShaderResourceView* no_inputs[2]{};
            ID3D11Buffer* no_constants = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
            context->CSSetShaderResources(0, 2, no_inputs);
            context->CSSetConstantBuffers(0, 1, &no_constants);
            context->CSSetShader(nullptr, nullptr, 0);
            valid = read_hash(context, staging, gpu.vp8_yuv_buffer, bytes, &expected) && actual == expected;
        }
        // The wavefront loop filter (rank x + 2y, 16 macroblocks per rank) must
        // match the frozen serial filter for normal, sharp, simple and segment levels.
        // The mostly-subblock pattern exercises every edge class; one pattern keeps readbacks few.
        for (u32 config = 0; valid && pattern == 1 && config < 4; ++config) {
            Vp8ControlHeader header{};
            auto& filter = header.loop_filter;
            filter.level = config == 1 ? 20 : config == 2 ? 40 : 63;
            filter.sharpness = config == 1 ? 5 : 0;
            filter.simple = config == 2;
            if (config == 3) {
                auto& segments = header.segmentation;
                segments.enabled = 1;
                segments.filter_strength[1] = 63;
                segments.filter_strength[2] = -63;
                segments.filter_strength[3] = 12;
                filter.use_delta = 1;
                filter.reference_delta[0] = -3;
                filter.mode_delta[0] = 7;
            }
            u64 filtered[2]{};
            ID3D11ComputeShader* production = nullptr;
            for (u32 side = 0; valid && side < 2; ++side) {
                if (side) {
                    production = gpu.vp8_filter_shader;
                    gpu.vp8_filter_shader = reference_filter;
                }
                valid = media_atlas_gpu_reconstruct_vp8_keyframe(&gpu, device, context, frame, modes, coefficients).ok() &&
                        media_atlas_gpu_filter_vp8_keyframe(&gpu, device, context, frame, header, modes, coefficients).ok() &&
                        read_hash(context, staging, gpu.vp8_yuv_buffer, bytes, &filtered[side]);
            }
            if (production) gpu.vp8_filter_shader = production;
            valid = valid && production && filtered[0] == filtered[1];
            filter_cases += valid;
        }
        if (staging) staging->Release();
        if (!valid) { log_u64(LogLevel::error, "vp8_tiles", "failed_case", cases); break; }
        samples += bytes / sizeof(u32);
        ++cases;
    }
    if (reference) reference->Release();
    if (reference_idct) reference_idct->Release();
    if (reference_filter) reference_filter->Release();
    log_u64(LogLevel::info, "vp8_tiles", "exact_cases", cases);
    log_u64(LogLevel::info, "vp8_tiles", "exact_samples", samples);
    log_u64(LogLevel::info, "vp8_tiles", "exact_filter_cases", filter_cases);
    return valid;
}
} // namespace maicord
