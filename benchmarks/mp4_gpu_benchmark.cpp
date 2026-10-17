#include "mp4_source_reader_reference.hpp"
#include "mp4_gpu_benchmark.hpp"
#include "maicord/platform/h264_gpu_decoder.hpp"
#include "h264_state_probe.hpp"
#include "mp4_mf_gpu_reference.hpp"
#include "mp4_ffmpeg_gpu_reference.hpp"
#include "mp4_transform_probe.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/media/mp4_demux.hpp"
#include "mp4_mft_decoder.hpp"
#include "mp4_mft_trace.hpp"
#include "maicord/render/video_frame_gpu.hpp"
#include "maicord/render/media_atlas_gpu.hpp"
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <mftransform.h>
#include <codecapi.h>

namespace maicord {
namespace {
template<class T> void release(T*& value) noexcept {
    if (value) { value->Release(); value = nullptr; }
}
struct Less {
    bool operator()(u64 a, u64 b) const noexcept { return a < b; }
};
struct FrameTrace {
    u32 decode_us, present_us, wait_us, completed_us;
};
constexpr u32 percentile_index(u32 count, u32 percentile) noexcept {
    // Nearest-rank empirical quantile: a four-frame clip's p95/p99 must
    // include its slowest frame, not silently select the third frame.
    return count ? (count * percentile + 99u) / 100u - 1u : 0;
}
static_assert(percentile_index(4, 95) == 3 && percentile_index(4, 99) == 3);
static_assert(percentile_index(1, 99) == 0 && percentile_index(100, 95) == 94);
u64 private_bytes() noexcept {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    return memory.PrivateUsage;
}
u64 cycles() noexcept {
    ULONG64 count = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &count);
    return count;
}
bool enabled(const wchar_t* name) noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(name, value, 2) == 1 && value[0] == L'1';
}
struct Fixture {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11DeviceContext1* context1;
    ID3D11DeviceContext4* context4;
    ID3D11Fence* fence;
    ID3D11Texture2D* output;
    ID3D11UnorderedAccessView* uav;
    ID3D11ShaderResourceView* output_view;
    ID3D11Texture2D* readback;
    ID3D11InfoQueue* debug;
    IDXGIAdapter3* adapter;
    HANDLE event;
    u64 signal;
    VideoFrameGpu converter;
};
HRESULT start(Fixture* f, u32 width, u32 height, bool verify, bool debug) noexcept {
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT |
            (enabled(L"MAICORD_MP4_GPU_VIDEO_FLAG") ? D3D11_CREATE_DEVICE_VIDEO_SUPPORT : 0) |
            (debug ? D3D11_CREATE_DEVICE_DEBUG : 0), levels, 2, D3D11_SDK_VERSION,
        &f->device, nullptr, &f->context);
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(status)) status = f->context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread));
    if (SUCCEEDED(status)) multithread->SetMultithreadProtected(TRUE);
    release(multithread);
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(status) && SUCCEEDED(f->device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&f->adapter));
    }
    release(adapter); release(dxgi);
    if (SUCCEEDED(status)) status = f->context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&f->context1));
    ID3D11Device5* device5 = nullptr;
    if (SUCCEEDED(status)) status = f->device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5));
    if (SUCCEEDED(status)) status = f->context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&f->context4));
    if (SUCCEEDED(status)) status = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&f->fence));
    release(device5);
    if (SUCCEEDED(status)) {
        f->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!f->event) status = E_OUTOFMEMORY;
    }
    D3D11_TEXTURE2D_DESC texture{};
    // Offset output catches accidental writes outside the selected atlas cell.
    texture.Width = width + 8u; texture.Height = height + 8u;
    texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(status)) status = f->device->CreateTexture2D(&texture, nullptr, &f->output);
    if (SUCCEEDED(status)) status = f->device->CreateUnorderedAccessView(f->output, nullptr, &f->uav);
    if (verify && SUCCEEDED(status)) status = f->device->CreateShaderResourceView(f->output, nullptr, &f->output_view);
    if (verify && SUCCEEDED(status)) {
        texture.Usage = D3D11_USAGE_STAGING; texture.BindFlags = 0;
        texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        status = f->device->CreateTexture2D(&texture, nullptr, &f->readback);
    }
    if (debug && SUCCEEDED(status)) status = f->device->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&f->debug));
    if (SUCCEEDED(status)) {
        const UINT sentinel[4]{19, 37, 59, 83};
        f->context->ClearUnorderedAccessViewUint(f->uav, sentinel);
    }
    return status;
}
bool complete(Fixture* f) noexcept {
    const u64 signal = ++f->signal;
    if (FAILED(f->context4->Signal(f->fence, signal)) ||
        FAILED(f->fence->SetEventOnCompletion(signal, f->event))) return false;
    f->context->Flush();
    return WaitForSingleObject(f->event, 2'000) == WAIT_OBJECT_0;
}
void stop(Fixture* f) noexcept {
    video_frame_gpu_release(&f->converter);
    release(f->readback); release(f->output_view); release(f->uav); release(f->output);
    release(f->fence); release(f->debug); release(f->adapter); release(f->context4);
    release(f->context1); release(f->context); release(f->device);
    if (f->event) CloseHandle(f->event);
}

bool compare_frame(Fixture* f, const Mp4DecodedFrame& expected,
                   u64* mismatch, u64* max_difference, u64* border_mismatch) noexcept {
    f->context->CopyResource(f->readback, f->output);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(f->context->Map(f->readback, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const auto* pixels = static_cast<const u8*>(mapped.pData);
    D3D11_TEXTURE2D_DESC target{};
    f->readback->GetDesc(&target);
    for (u32 y = 0; y < target.Height; ++y) {
        for (u32 x = 0; x < target.Width; ++x) {
            const bool inside = x >= 4 && y >= 4 && x < expected.width + 4u && y < expected.height + 4u;
            for (u32 c = 0; c < 4; ++c) {
                const u8 actual = pixels[static_cast<u64>(y) * mapped.RowPitch + x * 4u + c];
                if (inside) {
                    const u8 wanted = expected.rgba8.data[static_cast<u64>(y - 4u) * expected.row_pitch + (x - 4u) * 4u + c];
                    const u32 delta = actual > wanted ? actual - wanted : wanted - actual;
                    *mismatch += delta != 0;
                    *max_difference = maximum<u64>(*max_difference, delta);
                } else {
                    const u8 sentinel[]{19, 37, 59, 83};
                    *border_mismatch += actual != sentinel[c];
                }
            }
        }
    }
    f->context->Unmap(f->readback, 0);
    return true;
}

bool atlas_reuse_probe(Fixture* fixture, const GpuVideoFrame& frame) noexcept {
    MediaAtlasEntry entries[8]{};
    GlyphAtlasEntry allocations[8]{};
    GlyphAtlasRow rows[8]{};
    u32 buckets[16]{};
    MediaAtlas atlas{};
    MediaAtlasGpu gpu{};
    const MediaAssetKey key = media_asset_custom_emoji(42, false);
    const u16 width = static_cast<u16>((frame.width + 17u) & ~15u);
    const u16 height = static_cast<u16>((frame.height + 17u) & ~15u);
    bool ok = media_atlas_initialize(&atlas, width, height, 1,
        Slice<MediaAtlasEntry>{entries, 8}, Slice<u32>{buckets, 16},
        Slice<GlyphAtlasEntry>{allocations, 8}, Slice<GlyphAtlasRow>{rows, 8}) &&
        media_atlas_gpu_initialize(&gpu, fixture->device, fixture->context1, width, height).ok();
    for (u32 index = 0; ok && index < 3; ++index) {
        ok = media_atlas_gpu_present_video(&gpu, &atlas, fixture->device,
            fixture->context1, key, frame, false).ok();
    }
    ok = ok && gpu.cleared_regions == 1 && gpu.video.frames == 3 && gpu.video.allocations == 1;
    media_atlas_gpu_release(&gpu);
    return ok;
}

bool odd_extent_probe(Fixture* f, const GpuVideoFrame& frame,
                       const Mp4DecodedFrame& expected) noexcept {
    // Cover partial thread groups, a nonzero even crop, and the smallest legal
    // visible extent. The source image/stride stay unchanged in the CPU oracle.
    const u32 extents[][2]{{1, 1}, {3, 5}, {17, 15}};
    for (const auto& extent : extents) {
        if (frame.width < extent[0] + 2 || frame.height < extent[1] + 2) return false;
        const UINT sentinel[4]{19, 37, 59, 83};
        f->context->ClearUnorderedAccessViewUint(f->uav, sentinel);
        GpuVideoFrame cropped = frame;
        cropped.crop_x += 2; cropped.crop_y += 2;
        cropped.width = extent[0]; cropped.height = extent[1];
        Mp4DecodedFrame reference = expected;
        reference.width = extent[0]; reference.height = extent[1];
        reference.rgba8.data += expected.row_pitch * 2u + 8u;
        u64 mismatch = 0, maximum_difference = 0, borders = 0;
        if (FAILED(video_frame_gpu_present(&f->converter, f->device,
                f->context1, cropped, f->uav, 4, 4)) ||
            !compare_frame(f, reference, &mismatch, &maximum_difference, &borders) ||
            mismatch || borders) return false;
    }
    const UINT sentinel[4]{19, 37, 59, 83};
    f->context->ClearUnorderedAccessViewUint(f->uav, sentinel);
    return true;
}

bool converter_probe(Fixture* f, const GpuVideoFrame& frame,
                       void (*metric)(const char*, u64)) noexcept {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* timestamps[2]{};
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    HRESULT status = f->device->CreateQuery(&desc, &disjoint);
    desc.Query = D3D11_QUERY_TIMESTAMP;
    for (u32 i = 0; i < 2 && SUCCEEDED(status); ++i)
        status = f->device->CreateQuery(&desc, &timestamps[i]);
    u64 samples[128]{};
    u32 measured = 0;
    for (u32 i = 0; i < 136 && SUCCEEDED(status); ++i) {
        f->context->Begin(disjoint);
        f->context->End(timestamps[0]);
        status = video_frame_gpu_present(&f->converter, f->device,
            f->context1, frame, f->uav, 4, 4);
        f->context->End(timestamps[1]);
        f->context->End(disjoint);
        if (FAILED(status) || !complete(f)) { status = E_FAIL; break; }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
        u64 begin = 0, end = 0;
        if (f->context->GetData(disjoint, &clock, sizeof(clock), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            f->context->GetData(timestamps[0], &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            f->context->GetData(timestamps[1], &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            clock.Disjoint || !clock.Frequency || end < begin) { status = E_FAIL; break; }
        if (i >= 8) samples[measured++] = static_cast<u64>(
            static_cast<double>(end - begin) * 1'000'000'000.0 / static_cast<double>(clock.Frequency));
    }
    heap_sort(samples, measured, Less{});
    metric("gpu_converter_samples", measured);
    metric("gpu_converter_p50_ns", measured ? samples[percentile_index(measured, 50)] : 0);
    metric("gpu_converter_p95_ns", measured ? samples[percentile_index(measured, 95)] : 0);
    metric("gpu_converter_p99_ns", measured ? samples[percentile_index(measured, 99)] : 0);
    release(timestamps[0]); release(timestamps[1]); release(disjoint);
    return SUCCEEDED(status) && measured == 128;
}

struct DecodeWorker {
    ConstSlice<u8> encoded;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    HANDLE command, done, thread;
    volatile LONG stopping;
    bool ready;
    bool native;
    i32 status;
    Mp4DecodedFrame frame;
};
bool native_busy(i32 status) noexcept {
    return status == E_PENDING || status == static_cast<i32>(0x8876021Cu) ||
        status == DXGI_ERROR_WAS_STILL_DRAWING;
}
DWORD WINAPI decode_worker(void* opaque) noexcept {
    auto* worker = static_cast<DecodeWorker*>(opaque);
    const bool runtime = worker->native || mp4_reference_thread_start();
    Mp4DirectDecoder decoder{};
    Mp4Decoder native{};
    worker->ready = runtime && (worker->native ? mp4_decoder_initialize(&native,
        worker->encoded, 0, 0, worker->device) :
        mp4_direct_decoder_initialize(&decoder, worker->encoded, worker->device));
    SetEvent(worker->done);
    if (worker->ready) {
        while (WaitForSingleObject(worker->command, 2'000) == WAIT_OBJECT_0 &&
               InterlockedCompareExchange(&worker->stopping, 0, 0) == 0) {
            worker->frame = worker->native ? mp4_decoder_read(&native) : mp4_direct_decoder_read(&decoder);
            worker->status = worker->native ? native.native.status : mp4_direct_decoder_last_status(&decoder);
            SetEvent(worker->done);
        }
    }
    mp4_direct_decoder_release(&decoder);
    mp4_decoder_release(&native);
    if (runtime && !worker->native) mp4_reference_thread_stop();
    return 0;
}

bool concurrent_probe(Fixture* fixture, ConstSlice<u8> encoded, u32 rounds,
                      void (*metric)(const char*, u64), bool native = false) noexcept {
    DecodeWorker workers[4]{};
    Mp4DirectDecoder reference{};
    bool ok = mp4_direct_decoder_initialize(&reference, encoded);
    for (DecodeWorker& worker : workers) {
        if (!ok) break;
        worker.encoded = encoded; worker.device = fixture->device;
        worker.context = fixture->context; worker.native = native;
        worker.command = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        worker.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (worker.command && worker.done) worker.thread = CreateThread(nullptr, 0, decode_worker, &worker, 0, nullptr);
        ok = worker.thread && WaitForSingleObject(worker.done, 2'000) == WAIT_OBJECT_0 && worker.ready;
    }
    if (ok) for (DecodeWorker& worker : workers) SetEvent(worker.command);
    u64 frames = 0, mismatches = 0, max_difference = 0, borders = 0;
    const Clock clock = clock_initialize();
    const i64 start_time = clock_now();
    for (u32 index = 0; ok && index < rounds; ++index) {
        if (clock_microseconds(clock, start_time, clock_now()) > 10'000'000u) { ok = false; break; }
        const Mp4DecodedFrame expected = mp4_direct_decoder_read(&reference);
        if (!expected.ok()) { ok = false; break; }
        for (DecodeWorker& worker : workers) {
            bool completed = false;
            for (u32 retry = 0; retry < 4; ++retry) {
                if (WaitForSingleObject(worker.done, 2'000) != WAIT_OBJECT_0) break;
                if (!worker.frame.ok() && native && native_busy(worker.status)) {
                    if (!complete(fixture)) break;
                    SetEvent(worker.command);
                } else { completed = true; break; }
            }
            if (!completed ||
                !worker.frame.ok() || !worker.frame.gpu ||
                worker.frame.width != expected.width || worker.frame.height != expected.height) { ok = false; break; }
            fixture->context->PSSetShaderResources(2, 1, &fixture->output_view);
            const HRESULT result = video_frame_gpu_present(&fixture->converter, fixture->device,
                fixture->context1, *worker.frame.gpu, fixture->uav, 4, 4);
            if (FAILED(result)) { ok = false; break; }
            // Return this decoder sample lease immediately after queuing its
            // GPU copy. Its next decode overlaps this readback and other lanes'
            // presentation. This catches reuse races hidden by serial tests.
            if (index + 1u < rounds) SetEvent(worker.command);
            if (!compare_frame(fixture, expected, &mismatches, &max_difference, &borders)) { ok = false; break; }
            ++frames;
        }
    }
    for (DecodeWorker& worker : workers) {
        InterlockedExchange(&worker.stopping, 1);
        if (worker.command) SetEvent(worker.command);
    }
    for (DecodeWorker& worker : workers) {
        // Never leave a live child thread referencing an expired stack owner.
        if (worker.thread && WaitForSingleObject(worker.thread, 2'000) != WAIT_OBJECT_0) ExitProcess(23);
        if (worker.thread) CloseHandle(worker.thread);
        if (worker.done) CloseHandle(worker.done);
        if (worker.command) CloseHandle(worker.command);
    }
    mp4_direct_decoder_release(&reference);
    metric("gpu_concurrent_frames", frames);
    metric("gpu_concurrent_native", native);
    metric("gpu_concurrent_mismatch_bytes", mismatches);
    metric("gpu_concurrent_max_difference", max_difference);
    metric("gpu_concurrent_border_mismatch_bytes", borders);
    return ok && frames == static_cast<u64>(rounds) * 4u && mismatches == 0 && borders == 0;
}
} // namespace

int mp4_gpu_benchmark(ConstSlice<u8> encoded, void (*metric)(const char*, u64)) noexcept {
    Mp4Demuxer demux{};
    if (mp4_demux_initialize(&demux, encoded) != Mp4DemuxError::none || demux.sample_count > 4096) return 20;
    const bool verify = enabled(L"MAICORD_MP4_GPU_VERIFY");
    const bool debug = enabled(L"MAICORD_MP4_GPU_DEBUG");
    const bool cpu = enabled(L"MAICORD_MP4_GPU_CPU_BASELINE");
    const bool warm = enabled(L"MAICORD_MP4_GPU_WARM");
    const bool mf = enabled(L"MAICORD_MP4_GPU_MF_BASELINE");
    const bool ffmpeg = enabled(L"MAICORD_MP4_GPU_FFMPEG_BASELINE");
    const bool native_idr = enabled(L"MAICORD_MP4_GPU_NATIVE_IDR");
    const bool native_stream = enabled(L"MAICORD_MP4_GPU_NATIVE_STREAM");
    const bool native = native_idr || native_stream;
    const bool first_only = enabled(L"MAICORD_MP4_GPU_FIRST_ONLY");
    const bool paced = enabled(L"MAICORD_MP4_GPU_PACED");
    const bool idle_after_frame = enabled(L"MAICORD_MP4_GPU_IDLE_AFTER_FRAME");
    if (idle_after_frame && (!first_only || verify || warm || paced)) return 26;
    const bool decode_trace = enabled(L"MAICORD_MP4_DECODE_TRACE");
    if (decode_trace && (mf || ffmpeg || cpu || verify || native)) return 26;
    if (native_idr && !first_only) return 26;
    if (static_cast<u32>(mf) + cpu + ffmpeg + native_idr + native_stream > 1 || (verify && (cpu || warm)) ||
        ((ffmpeg || native) && enabled(L"MAICORD_MP4_INSPECT")) ||
        (first_only && enabled(L"MAICORD_MP4_GPU_STRESS"))) return 26;
    if (native && verify && !h264_state_probe(encoded, metric)) return 28;
    wchar_t depth_value[2]{};
    const u32 depth = !verify && GetEnvironmentVariableW(L"MAICORD_MP4_GPU_QUEUE_DEPTH", depth_value, 2) == 1 &&
        depth_value[0] >= L'1' && depth_value[0] <= L'8' ? static_cast<u32>(depth_value[0] - L'0') : 1u;
    const bool trace_frames = enabled(L"MAICORD_MP4_GPU_TRACE");
    if (paced && (verify || first_only || depth != 1)) return 26;
    if (trace_frames && (verify || depth != 1)) return 26;
    Fixture f{};
    const Clock clock = clock_initialize();
    const i64 setup_begin = clock_now();
    HRESULT status = start(&f, demux.width, demux.height, verify, debug);
    metric("gpu_device_status", static_cast<u32>(status));
    metric("gpu_device_us", clock_microseconds(clock, setup_begin, clock_now()));
    if (FAILED(status)) { stop(&f); return 21; }
    if (native_stream && verify) {
        Mp4Decoder rejected{};
        const bool rejected_invalid = !mp4_decoder_initialize(&rejected, encoded, 0, 0, nullptr) &&
            !mp4_decoder_initialize(&rejected, {encoded.data, 1}, 0, 0, f.device) &&
            !rejected.ready && !rejected.native.state && !rejected.gpu &&
            !mp4_decoder_read(&rejected).ok();
        mp4_decoder_release(&rejected);
        if (!rejected_invalid) { stop(&f); return 29; }
    }
    Mp4DirectDecoder decoder{};
    Mp4DirectDecoder reference{};
    Mp4MfGpuReference mf_decoder{};
    Mp4FfmpegGpuReference ffmpeg_decoder{};
    Mp4Decoder playback_decoder{};
    H264GpuDecoder native_first{};
    H264GpuDecoder& native_decoder = native_stream ? playback_decoder.native : native_first;
    const auto initialize_decoder = [&]() noexcept {
        return native_stream ? mp4_decoder_initialize(&playback_decoder, encoded, 0, 0, f.device) :
            native_idr ? h264_gpu_decoder_initialize(&native_first, encoded, f.device, f.context, false) :
            ffmpeg ? mp4_ffmpeg_gpu_initialize(&ffmpeg_decoder, encoded, f.device) :
            mf ? mp4_mf_gpu_initialize(&mf_decoder, encoded, f.device) :
            mp4_direct_decoder_initialize(&decoder, encoded, cpu ? nullptr : f.device);
    };
    const auto read_decoder = [&](bool loop = false) noexcept {
        if (native_stream) return mp4_decoder_read(&playback_decoder, loop);
        if (native) {
            Mp4DecodedFrame frame{};
            for (u32 retry = 0; retry < 4; ++retry) {
                frame = h264_gpu_decoder_read(&native_decoder, loop);
                if (frame.ok() || !native_busy(native_decoder.status)) break;
                if (!complete(&f)) break; // Existing fence/event, no spin or sleep.
            }
            return frame;
        }
        return ffmpeg ? mp4_ffmpeg_gpu_read(&ffmpeg_decoder, loop) :
            mf ? mp4_mf_gpu_read(&mf_decoder, loop) : mp4_direct_decoder_read(&decoder, loop);
    };
    const auto release_decoder = [&]() noexcept {
        mp4_direct_decoder_release(&decoder);
        mp4_mf_gpu_release(&mf_decoder);
        mp4_ffmpeg_gpu_release(&ffmpeg_decoder);
        h264_gpu_decoder_release(&native_first);
        mp4_decoder_release(&playback_decoder);
    };
    if (warm) {
        const i64 warm_begin = clock_now();
        bool warmed = initialize_decoder();
        const Mp4DecodedFrame frame = warmed ? read_decoder() : Mp4DecodedFrame{};
        warmed = warmed && frame.ok();
        if (warmed && cpu) {
            const D3D11_BOX box{4, 4, 0, frame.width + 4u, frame.height + 4u, 1};
            f.context->UpdateSubresource(f.output, 0, &box, frame.rgba8.data, frame.row_pitch, 0);
        } else if (warmed) {
            warmed = frame.gpu && SUCCEEDED(video_frame_gpu_present(&f.converter, f.device,
                f.context1, *frame.gpu, f.uav, 4, 4));
        }
        warmed = warmed && complete(&f);
        release_decoder();
        metric("gpu_renderer_warmup_us", clock_microseconds(clock, warm_begin, clock_now()));
        if (!warmed || verify) { stop(&f); return 24; }
    }
    auto* traces = trace_frames ? static_cast<FrameTrace*>(VirtualAlloc(nullptr,
        sizeof(FrameTrace) * demux.sample_count, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) : nullptr;
    if (trace_frames && !traces) { stop(&f); return 27; }
    const u64 memory_begin = private_bytes();
    DXGI_QUERY_VIDEO_MEMORY_INFO local_before{}, shared_before{};
    const bool video_memory_available = f.adapter &&
        SUCCEEDED(f.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_before)) &&
        SUCCEEDED(f.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared_before));
    const u64 cycles_begin = cycles();
    const i64 init_begin = clock_now();
    bool ok = initialize_decoder();
    const u64 init_us = clock_microseconds(clock, init_begin, clock_now());
    MFT_INPUT_STREAM_INFO input_info{};
    HRESULT input_status = E_NOTIMPL, low_latency_status = E_NOTIMPL, readable_status = E_NOTIMPL;
    u32 worker_threads = 0;
    if (ok && !mf && !ffmpeg && !native) {
        IMFTransform* transform = mp4_direct_decoder_transform(&decoder);
        input_status = transform->GetInputStreamInfo(0, &input_info);
        wchar_t threads_value[2]{};
        if (GetEnvironmentVariableW(L"MAICORD_MP4_WORKER_THREADS", threads_value, 2) == 1 &&
            threads_value[0] >= L'2' && threads_value[0] <= L'8') {
            IMFAttributes* attributes = nullptr;
            worker_threads = static_cast<u32>(threads_value[0] - L'0');
            status = transform->GetAttributes(&attributes);
            if (SUCCEEDED(status)) status = attributes->SetUINT32(CODECAPI_AVDecNumWorkerThreads, worker_threads);
            release(attributes);
            ok = SUCCEEDED(status);
        }
        if (enabled(L"MAICORD_MP4_SHADER_READABLE")) {
            IMFAttributes* attributes = nullptr;
            status = transform->GetOutputStreamAttributes(0, &attributes);
            if (SUCCEEDED(status)) status = attributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_SHADER_RESOURCE);
            release(attributes);
            readable_status = status;
            ok = SUCCEEDED(status);
        }
        if (enabled(L"MAICORD_MP4_LOW_LATENCY")) {
            IMFAttributes* attributes = nullptr;
            status = transform->GetAttributes(&attributes);
            if (SUCCEEDED(status)) status = attributes->SetUINT32(CODECAPI_AVLowLatencyMode, 1);
            release(attributes);
            low_latency_status = status;
            ok = SUCCEEDED(status);
        }
    }
    if (verify) ok = ok && mp4_direct_decoder_initialize(&reference, encoded);
    const bool inspect = enabled(L"MAICORD_MP4_INSPECT");
    if (inspect && ok) {
        IMFTransform* transform = mf ? mp4_mf_gpu_transform(&mf_decoder) : mp4_direct_decoder_transform(&decoder);
        metric("inspect_stage", 0);
        mp4_inspect_transform(transform, metric);
        if (mf) release(transform);
    }
    u32 frames = 0;
    u64 mismatch = 0, max_difference = 0, border_mismatch = 0;
    u64 first_us = 0, submit_us = 0;
    u64 decode_us = 0, present_us = 0, wait_us = 0;
    u64 first_decode_us = 0, first_present_us = 0, first_wait_us = 0;
    u16 display_width = 0, display_height = 0;
    D3D11_TEXTURE2D_DESC first_texture{};
    u64 latencies[8192]{};
    i64 submitted_at[8]{};
    u32 pending = 0;
    const i64 begin = clock_now();
    u64 pace_at_us = 0;
    Mp4DirectTraceEvent decode_events[64]{};
    Mp4DirectTrace operations{{decode_events, 64}, 0, 0};
    const u32 count = first_only ? 1u : demux.sample_count * (verify ? 2u : 1u);
    for (u32 index = 0; ok && index < count; ++index) {
        if (paced) {
            const u64 elapsed = clock_microseconds(clock, begin, clock_now());
            if (pace_at_us > elapsed) Sleep(static_cast<DWORD>((pace_at_us - elapsed + 999u) / 1000u));
        }
        const i64 frame_begin = clock_now();
        if (clock_microseconds(clock, begin, frame_begin) > 10'000'000u) { ok = false; break; }
        const bool loop = verify && index == demux.sample_count;
        const Mp4DecodedFrame frame = decode_trace && index == 0 ?
            mp4_direct_decoder_read_profiled(&decoder, loop, &operations) : read_decoder(loop);
        const i64 decoded_at = clock_now();
        const u64 frame_decode_us = clock_microseconds(clock, frame_begin, decoded_at);
        if (traces) traces[index].decode_us = static_cast<u32>(frame_decode_us);
        decode_us += frame_decode_us;
        if (frames == 0) first_decode_us = frame_decode_us;
        ok = frame.ok() && frame.width == demux.width && frame.height == demux.height &&
            (cpu ? frame.rgba8.data != nullptr : frame.gpu && frame.gpu->texture && !frame.rgba8.data && decoder.rgba8_capacity == 0);
        if (native_stream) {
            if (!frames) { display_width = frame.display_width; display_height = frame.display_height; }
            ok = ok && display_width && display_height && frame.display_width == display_width &&
                frame.display_height == display_height && frame.gpu &&
                frame.gpu->width == frame.width && frame.gpu->height == frame.height;
        }
        if (!ok) {
            metric("gpu_frame_error", static_cast<u64>(frame.error));
            metric("gpu_frame_status", static_cast<u32>(native ? native_decoder.status : ffmpeg ? ffmpeg_decoder.status :
                mf ? mf_decoder.status : mp4_direct_decoder_last_status(&decoder)));
            break;
        }
        if (paced) pace_at_us += frame.duration_us;
        if (frames == 0 && frame.gpu) {
            frame.gpu->texture->GetDesc(&first_texture);
            if (enabled(L"MAICORD_MP4_CONVERTER_ONLY")) {
                const bool measured = converter_probe(&f, *frame.gpu, metric);
                mp4_direct_decoder_release(&reference);
                release_decoder();
                if (traces) VirtualFree(traces, 0, MEM_RELEASE);
                stop(&f);
                return measured ? 0 : 25;
            }
        }
        if (cpu) {
            const D3D11_BOX box{4, 4, 0, frame.width + 4u, frame.height + 4u, 1};
            f.context->UpdateSubresource(f.output, 0, &box, frame.rgba8.data, frame.row_pitch, 0);
        } else {
            if (verify) {
                if (frames == 0 && !atlas_reuse_probe(&f, *frame.gpu)) { ok = false; break; }
                f.context->PSSetShaderResources(2, 1, &f.output_view);
                GpuVideoFrame invalid = *frame.gpu;
                invalid.crop_x = ~0u;
                const u32 submitted_frames = f.converter.frames;
                ok = video_frame_gpu_present(&f.converter, f.device, f.context1, invalid, f.uav, 4, 4) == E_INVALIDARG &&
                     video_frame_gpu_present(&f.converter, f.device, f.context1, *frame.gpu, f.uav, ~0u, 4) == E_INVALIDARG &&
                     f.converter.frames == submitted_frames;
                if (!ok) break;
            }
            status = video_frame_gpu_present(&f.converter, f.device, f.context1, *frame.gpu, f.uav, 4, 4);
            if (FAILED(status)) { metric("gpu_present_status", static_cast<u32>(status)); ok = false; break; }
        }
        const i64 presented_at = clock_now();
        const u64 frame_present_us = clock_microseconds(clock, decoded_at, presented_at);
        if (traces) traces[index].present_us = static_cast<u32>(frame_present_us);
        present_us += frame_present_us;
        if (frames == 0) first_present_us = frame_present_us;
        submit_us += clock_microseconds(clock, frame_begin, presented_at);
        submitted_at[pending++] = frame_begin;
        // A depth-one latency test and a bounded pipelined throughput test.
        // Every reported latency includes completion of that frame, not merely
        // submission; frame zero always completes alone to measure true TTFF.
        if (frames == 0 || pending == depth || index + 1u == count) {
            ok = complete(&f);
            const i64 completed_at = clock_now();
            const u64 frame_wait_us = clock_microseconds(clock, presented_at, completed_at);
            if (traces) {
                traces[index].wait_us = static_cast<u32>(frame_wait_us);
                traces[index].completed_us = static_cast<u32>(clock_microseconds(clock, frame_begin, completed_at));
            }
            wait_us += frame_wait_us;
            if (frames == 0) first_wait_us = frame_wait_us;
            for (u32 queued = 0; queued < pending; ++queued) {
                latencies[frames + 1u - pending + queued] = clock_microseconds(clock, submitted_at[queued], completed_at);
            }
            pending = 0;
        }
        if (frames == 0) first_us = clock_microseconds(clock, init_begin, clock_now());
        if (verify && ok) {
            const Mp4DecodedFrame expected = mp4_direct_decoder_read(&reference, index == demux.sample_count);
            ok = expected.ok() && expected.width == frame.width && expected.height == frame.height;
            if (!ok) break;
            ok = compare_frame(&f, expected, &mismatch, &max_difference, &border_mismatch);
            if (ok && frames == 0) ok = odd_extent_probe(&f, *frame.gpu, expected);
        }
        ++frames;
    }
    const u64 total_us = clock_microseconds(clock, begin, clock_now());
    const u64 cycles_used = cycles() - cycles_begin;
    const u64 memory_end = private_bytes();
    if (inspect && ok) {
        IMFTransform* transform = mf ? mp4_mf_gpu_transform(&mf_decoder) : mp4_direct_decoder_transform(&decoder);
        metric("inspect_stage", 1);
        mp4_inspect_transform(transform, metric);
        if (mf) release(transform);
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO local_after{}, shared_after{};
    const bool video_memory_complete = video_memory_available &&
        SUCCEEDED(f.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_after)) &&
        SUCCEEDED(f.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared_after));
    if (ok && !first_only) ok = read_decoder().error == Mp4DecodeError::end_of_stream;
    if (ok && verify && enabled(L"MAICORD_MP4_GPU_STRESS")) {
        ok = concurrent_probe(&f, encoded, minimum(demux.sample_count, 192u), metric, native_stream);
    }
    u64 debug_errors = 0;
    if (f.debug) {
        const u64 messages = f.debug->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (u64 index = 0; index < messages; ++index) {
            alignas(D3D11_MESSAGE) u8 storage[4096]{};
            SIZE_T bytes = sizeof(storage);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage);
            if (SUCCEEDED(f.debug->GetMessage(index, message, &bytes)) &&
                message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                ++debug_errors;
                metric("gpu_debug_message_id", static_cast<u32>(message->ID));
            }
        }
    }
    heap_sort(latencies, frames, Less{});
    metric("gpu_decoder_ready", frames != 0);
    metric("gpu_first_only", first_only);
    metric("gpu_paced", paced);
    metric("gpu_idle_after_frame", idle_after_frame);
    metric("gpu_native_idr", native_idr);
    metric("gpu_native_stream", native_stream);
    metric("gpu_native_playback_wrapper", native_stream);
    metric("gpu_display_width", display_width);
    metric("gpu_display_height", display_height);
    if (native) {
        metric("gpu_native_status", static_cast<u32>(native_decoder.status));
        metric("gpu_native_parse_error", static_cast<u32>(native_decoder.parse_error));
    }
    metric("gpu_decoder_init_us", init_us);
    metric("gpu_input_info_status", static_cast<u32>(input_status));
    metric("gpu_input_flags", input_info.dwFlags);
    metric("gpu_worker_threads_requested", worker_threads);
    if (enabled(L"MAICORD_MP4_LOW_LATENCY")) metric("gpu_low_latency_status", static_cast<u32>(low_latency_status));
    if (enabled(L"MAICORD_MP4_SHADER_READABLE")) metric("gpu_shader_readable_hint_status", static_cast<u32>(readable_status));
    metric("gpu_texture_bind_flags", first_texture.BindFlags);
    metric("gpu_texture_array_size", first_texture.ArraySize);
    metric("gpu_texture_storage_width", first_texture.Width);
    metric("gpu_texture_storage_height", first_texture.Height);
    metric("gpu_cpu_baseline", cpu);
    metric("gpu_mf_baseline", mf);
    metric("gpu_ffmpeg_baseline", ffmpeg);
    if (ffmpeg) metric("gpu_ffmpeg_status", static_cast<u32>(ffmpeg_decoder.status));
    metric("gpu_renderer_warm", warm);
    metric("gpu_verify", verify);
    metric("gpu_queue_depth", depth);
    metric("gpu_frames", frames);
    metric("gpu_first_completed_frame_us", first_us);
    metric("gpu_decode_present_us", total_us);
    metric("gpu_submit_us", submit_us);
    metric("gpu_decode_cpu_us", decode_us);
    metric("gpu_present_cpu_us", present_us);
    metric("gpu_completion_wait_us", wait_us);
    metric("gpu_first_decode_cpu_us", first_decode_us);
    metric("gpu_first_present_cpu_us", first_present_us);
    metric("gpu_first_completion_wait_us", first_wait_us);
    metric("gpu_frame_p50_us", frames ? latencies[percentile_index(frames, 50)] : 0);
    metric("gpu_frame_p95_us", frames ? latencies[percentile_index(frames, 95)] : 0);
    metric("gpu_frame_p99_us", frames ? latencies[percentile_index(frames, 99)] : 0);
    metric("gpu_frame_max_us", frames ? latencies[frames - 1u] : 0);
    metric("gpu_process_cycles", cycles_used);
    metric("gpu_private_before_bytes", memory_begin);
    metric("gpu_private_after_bytes", memory_end);
    metric("gpu_private_growth_bytes", memory_end > memory_begin ? memory_end - memory_begin : 0);
    metric("gpu_private_released_bytes", memory_begin > memory_end ? memory_begin - memory_end : 0);
    metric("gpu_video_memory_available", video_memory_complete);
    if (video_memory_complete) {
        metric("gpu_local_before_bytes", local_before.CurrentUsage);
        metric("gpu_local_after_bytes", local_after.CurrentUsage);
        metric("gpu_shared_before_bytes", shared_before.CurrentUsage);
        metric("gpu_shared_after_bytes", shared_after.CurrentUsage);
    }
    metric("gpu_rgba_mismatch_bytes", mismatch);
    metric("gpu_rgba_max_difference", max_difference);
    metric("gpu_border_mismatch_bytes", border_mismatch);
    metric("gpu_debug_errors", debug_errors);
    metric("gpu_surface_allocations", f.converter.allocations);
    metric("gpu_nv12_copied_bytes", f.converter.copied_bytes);
    metric("gpu_decoder_rgba_allocation_bytes", decoder.rgba8_capacity);
    if (decode_trace) {
        metric("decode_trace_events", operations.count);
        metric("decode_trace_dropped", operations.dropped);
        for (u32 i = 0; i < operations.count; ++i) {
            const auto& event = operations.events.data[i];
            metric("decode_event", i);
            metric("decode_operation", static_cast<u32>(event.operation));
            metric("decode_sample", event.sample_index);
            metric("decode_begin_us", clock_microseconds(clock, begin, event.begin_ticks));
            metric("decode_duration_us", clock_microseconds(clock, event.begin_ticks, event.end_ticks));
            metric("decode_status", static_cast<u32>(event.status));
        }
    }
    for (u32 index = 0; traces && index < frames; ++index) {
        metric("trace_frame", index);
        metric("trace_decode_us", traces[index].decode_us);
        metric("trace_present_us", traces[index].present_us);
        metric("trace_wait_us", traces[index].wait_us);
        metric("trace_completed_us", traces[index].completed_us);
    }
    // Diagnostic control: keep the device and completed first frame alive
    // without submitting anything. Not part of playback/throughput timings.
    if (ok && idle_after_frame) Sleep(7'000);
    if (traces) VirtualFree(traces, 0, MEM_RELEASE);
    mp4_direct_decoder_release(&reference);
    release_decoder();
    stop(&f);
    return ok && frames == count && mismatch == 0 && border_mismatch == 0 && debug_errors == 0 ? 0 : 22;
}
} // namespace maicord
