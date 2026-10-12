// GPU VP8 probes. vp8_tiled_probe runs in the registered UI probe; MAICORD_WEBP_PROBE=<animated
// webp> adds a real-stream benchmark over an external file and is manual.
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/media/animated_webp.hpp"

#include <d3d11_1.h>

namespace maicord {
bool vp8_tiled_probe(D3D11Renderer& renderer) noexcept;
bool webp_scroll_probe(D3D11Renderer& renderer, ConstSlice<u8> encoded) noexcept;
namespace {
struct Probe {
    u8 bytes[2 * 1024 * 1024];
    AnimatedWebpWorkspace animation;
    LossyWebpWorkspace lossy;
    Vp8lWorkspace lossless;
    u8 alpha_scratch[512 * 512 * 4];
    u8 alpha[512 * 512];
};
Probe probe;
bool query(ID3D11DeviceContext* context, ID3D11Query* value, void* data, u32 bytes) noexcept {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    HRESULT result;
    while ((result = context->GetData(value, data, bytes, 0)) == S_FALSE && GetTickCount64() < deadline) Sleep(1);
    return result == S_OK;
}
}

// Opt-in real-stream benchmark: excludes network, CPU entropy decoding and
// readback from GPU timestamps. Five independent decode submissions per tick.
bool webp_gpu_probe(D3D11Renderer& renderer) noexcept {
    if (!vp8_tiled_probe(renderer)) return false;
    wchar_t path[1024]{};
    if (!GetEnvironmentVariableW(L"MAICORD_WEBP_PROBE", path, 1024)) return true;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD bytes = 0;
    bool ok = ReadFile(file, probe.bytes, sizeof(probe.bytes), &bytes, nullptr) != 0;
    CloseHandle(file);
    AnimatedWebpImage animation{};
    ok = ok && animated_webp_parse({probe.bytes, bytes}, &animation, &probe.animation).ok();
    auto* device = renderer.graphics.device;
    auto* context = renderer.graphics.context1;
    ID3D11Query* disjoint = nullptr; ID3D11Query* start = nullptr; ID3D11Query* end = nullptr;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ok = ok && SUCCEEDED(device->CreateQuery(&desc, &disjoint));
    desc.Query = D3D11_QUERY_TIMESTAMP;
    ok = ok && SUCCEEDED(device->CreateQuery(&desc, &start)) && SUCCEEDED(device->CreateQuery(&desc, &end));
    ID3D11Query* phases[10]{};
    for (auto& phase : phases) ok = ok && SUCCEEDED(device->CreateQuery(&desc, &phase));
    ID3D11Buffer* readback = nullptr;
    u64 gpu_us = 0, max_us = 0, hash = kHash64Seed, cpu_decode_us = 0;
    u64 reconstruct_us = 0, filter_us = 0;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    for (u32 frame_index = 0; ok && frame_index < animation.frame_count; ++frame_index) {
        Vp8Keyframe frame{}; Vp8ControlHeader header{};
        LARGE_INTEGER begin_cpu{}, end_cpu{}; QueryPerformanceCounter(&begin_cpu);
        const auto decoded = animated_webp_decode_parsed_frame({probe.bytes, bytes}, animation, frame_index,
            &probe.lossless, &probe.lossy, &frame, &header, {},
            {probe.alpha_scratch, sizeof(probe.alpha_scratch)}, {probe.alpha, sizeof(probe.alpha)});
        QueryPerformanceCounter(&end_cpu);
        cpu_decode_us += static_cast<u64>(end_cpu.QuadPart - begin_cpu.QuadPart) * 1000000 / frequency.QuadPart;
        ok = decoded.ok() && decoded.frame.codec == AnimatedWebpCodec::vp8_lossy;
        if (!ok) break;
        context->Begin(disjoint); context->End(start);
        for (u32 instance = 0; ok && instance < 5; ++instance) {
            ok = renderer_reconstruct_vp8_keyframe(&renderer, frame, probe.lossy.modes, probe.lossy.coefficients).ok();
            context->End(phases[instance * 2]);
            ok = ok && renderer_filter_vp8_keyframe(&renderer, frame, header, probe.lossy.modes, probe.lossy.coefficients).ok();
            context->End(phases[instance * 2 + 1]);
        }
        context->End(end); context->End(disjoint); context->Flush();
        UINT64 first = 0, last = 0; D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
        ok = ok && query(context, disjoint, &timing, sizeof(timing)) && !timing.Disjoint && timing.Frequency &&
             query(context, start, &first, sizeof(first)) && query(context, end, &last, sizeof(last));
        if (!ok) break;
        const u64 elapsed = (last - first) * 1000000 / timing.Frequency;
        gpu_us += elapsed; max_us = maximum(max_us, elapsed);
        UINT64 previous = first;
        for (u32 i = 0; i < 10; ++i) {
            UINT64 current = 0;
            ok = ok && query(context, phases[i], &current, sizeof(current));
            const u64 time = (current - previous) * 1000000 / timing.Frequency;
            if (i & 1u) filter_us += time; else reconstruct_us += time;
            previous = current;
        }
        auto* gpu_buffer = renderer.media_gpu.vp8_yuv_buffer;
        if (!readback) {
            D3D11_BUFFER_DESC buffer{}; gpu_buffer->GetDesc(&buffer);
            buffer.Usage = D3D11_USAGE_STAGING; buffer.BindFlags = 0;
            buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ; buffer.MiscFlags = 0;
            ok = SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &readback));
        }
        if (!ok) break;
        context->CopyResource(readback, gpu_buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
        if (!ok) break;
        const u32 samples = static_cast<u32>(renderer.media_gpu.vp8_luma_stride) * renderer.media_gpu.vp8_luma_rows +
            2u * renderer.media_gpu.vp8_chroma_stride * renderer.media_gpu.vp8_chroma_rows;
        hash = hash64_counted_bytes(hash, mapped.pData, samples * 4);
        context->Unmap(readback, 0);
    }
    if (readback) readback->Release();
    for (auto* phase : phases) if (phase) phase->Release();
    if (end) end->Release(); if (start) start->Release(); if (disjoint) disjoint->Release();
    wchar_t expected_value[24]{};
    const DWORD expected_length = GetEnvironmentVariableW(L"MAICORD_WEBP_EXPECTED_YUV_HASH", expected_value, 24);
    if (expected_length != 0) {
        u64 expected_hash = 0;
        ok = ok && expected_length < 24;
        for (u32 i = 0; ok && i < expected_length; ++i) {
            const u32 digit = static_cast<u32>(expected_value[i] - L'0');
            ok = digit < 10 && expected_hash <= (~0ull - digit) / 10;
            if (ok) expected_hash = expected_hash * 10 + digit;
        }
        ok = ok && expected_hash == hash;
    }
    log_u64(LogLevel::info, "webp_probe", "frames", animation.frame_count);
    log_u64(LogLevel::info, "webp_probe", "five_instances_gpu_average_us", gpu_us / maximum(1u, animation.frame_count));
    log_u64(LogLevel::info, "webp_probe", "five_instances_gpu_max_us", max_us);
    log_u64(LogLevel::info, "webp_probe", "five_instances_reconstruct_average_us", reconstruct_us / maximum(1u, animation.frame_count));
    log_u64(LogLevel::info, "webp_probe", "five_instances_filter_average_us", filter_us / maximum(1u, animation.frame_count));
    log_u64(LogLevel::info, "webp_probe", "one_instance_cpu_decode_average_us", cpu_decode_us / maximum(1u, animation.frame_count));
    log_u64(LogLevel::info, "webp_probe", "all_frames_yuv_hash", hash);
    log_write(ok ? LogLevel::info : LogLevel::error, "webp_probe", ok ? "pass" : "failed");
    return ok && webp_scroll_probe(renderer, {probe.bytes, bytes});
}
} // namespace maicord
