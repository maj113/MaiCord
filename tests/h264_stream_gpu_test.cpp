// Live H.264 stream decoder on real DXVA hardware (CTest label "gpu"; not run
// on hosted CI). Synthetic I_PCM/P_Skip streams make every decoded pixel
// predictable, so frames are verified by readback without a reference decoder.
#include "maicord/platform/h264_gpu_stream_decoder.hpp"
#include "maicord/app/video_stream_decoders.hpp"
#include "h264_stream_fixture.hpp"
#include <Windows.h>
#include <d3d11_4.h>
#include <new>

namespace maicord {
namespace {
using namespace h264_fixture;

void say(const char* text) noexcept {
    u32 count = 0;
    while (text[count]) ++count;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, count, &written, nullptr);
}

void say_number(const char* label, i64 value) noexcept {
    say(label);
    char digits[24]{};
    u32 n = 0;
    const bool negative = value < 0;
    u64 v = negative ? static_cast<u64>(-value) : static_cast<u64>(value);
    do { digits[n++] = static_cast<char>('0' + v % 10u); v /= 10u; } while (v && n < 20);
    char text[26]{};
    u32 count = 0;
    if (negative) text[count++] = '-';
    while (n) text[count++] = digits[--n];
    text[count++] = '\n';
    say(text);
}

struct Capture {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11Texture2D* staging;
    u32 staging_width, staging_height;
    u32 pattern, width_mbs, height_mbs, visible_width, visible_height;
    u32 slice_mbs; // Compact IDR: each slice repeats its first macroblock.
    u32 frames, keyframes, mismatches, requests;
    u32 last_rtp;
};

void verify(Capture* capture, const H264StreamFrame& frame) noexcept {
    const GpuVideoFrame& gpu = *frame.gpu;
    D3D11_TEXTURE2D_DESC desc{};
    gpu.texture->GetDesc(&desc);
    if (!capture->staging || capture->staging_width != desc.Width || capture->staging_height != desc.Height) {
        com_release(capture->staging);
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.ArraySize = 1;
        staging.BindFlags = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(capture->device->CreateTexture2D(&staging, nullptr, &capture->staging))) {
            ++capture->mismatches;
            return;
        }
        capture->staging_width = desc.Width;
        capture->staging_height = desc.Height;
    }
    capture->context->CopySubresourceRegion(capture->staging, 0, 0, 0, 0, gpu.texture, gpu.subresource, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(capture->context->Map(capture->staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        ++capture->mismatches;
        return;
    }
    const auto* luma = static_cast<const u8*>(mapped.pData);
    const auto* chroma = luma + u64{mapped.RowPitch} * desc.Height;
    bool ok = gpu.width == capture->visible_width && gpu.height == capture->visible_height && gpu.crop_x == 0 &&
              gpu.crop_y == 0;
    for (u32 y = 0; y < capture->height_mbs && ok; ++y)
        for (u32 x = 0; x < capture->width_mbs && ok; ++x) {
            if (y * 16u + 8u >= gpu.height) continue;
            const u32 m = y * capture->width_mbs + x;
            const u32 source = capture->slice_mbs ? m / capture->slice_mbs * capture->slice_mbs : m;
            ok = luma[u64{y * 16u + 8u} * mapped.RowPitch + x * 16u + 3u] == pcm_luma(source, capture->pattern) &&
                 chroma[u64{y * 8u + 4u} * mapped.RowPitch + x * 16u + 6u] == pcm_chroma(capture->pattern);
        }
    capture->context->Unmap(capture->staging, 0);
    if (!ok) ++capture->mismatches;
}

void on_frame(void* context, u32 ssrc, const H264StreamFrame& frame) noexcept {
    auto* capture = static_cast<Capture*>(context);
    if (ssrc != 0x1234) ++capture->mismatches;
    ++capture->frames;
    capture->keyframes += frame.keyframe ? 1u : 0u;
    capture->last_rtp = frame.rtp_timestamp;
    verify(capture, frame);
}
void on_keyframe(void* context, u32) noexcept { ++static_cast<Capture*>(context)->requests; }

struct Buffers {
    Bits bits;
    Output out;
};

bool push(H264StreamDecoder* decoder, Buffers* b, u32 rtp, bool intact = true) noexcept {
    const bool ok = b->out.ok && h264_stream_decoder_push(decoder, {b->out.data, b->out.count}, rtp, intact);
    b->out.count = 0;
    b->out.ok = true;
    return ok;
}

void expect(Capture* capture, const Sps& sps, u32 pattern, u32 slice_mbs = 0) noexcept {
    capture->pattern = pattern;
    capture->slice_mbs = slice_mbs;
    capture->width_mbs = sps.width_mbs;
    capture->height_mbs = sps.height_mbs;
    capture->visible_width = sps.width_mbs * 16u;
    capture->visible_height = sps.height_mbs * 16u - sps.crop_bottom;
}

// Actual D3D objects own these markers. No resource reference counts or fake
// texture interfaces participate in the lifetime check.
constexpr GUID kLifetimeMarker = {0xf136e064, 0x25c4, 0x460a, {0x89, 0xe2, 0x8a, 0x5d, 0x42, 0x79, 0xb0, 0xd8}};
volatile LONG g_lifetime_released[6]{};

struct LifetimeMarker final : IUnknown {
    LONG references = 1;
    u32 index;
    explicit LifetimeMarker(u32 value) noexcept : index(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!IsEqualIID(iid, __uuidof(IUnknown))) return E_NOINTERFACE;
        *output = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = InterlockedDecrement(&references);
        if (!remaining) {
            InterlockedIncrement(&g_lifetime_released[index]);
            this->~LifetimeMarker();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return static_cast<ULONG>(remaining);
    }
};

bool mark_lifetime(ID3D11DeviceChild* object, u32 index) noexcept {
    void* storage = HeapAlloc(GetProcessHeap(), 0, sizeof(LifetimeMarker));
    if (!storage) return false;
    auto* marker = new (storage) LifetimeMarker(index);
    const HRESULT status = object->SetPrivateDataInterface(kLifetimeMarker, marker);
    marker->Release();
    return SUCCEEDED(status);
}

void release_frame(VideoStreamFrame* frame) noexcept {
    com_release(frame->gpu.chroma);
    com_release(frame->gpu.luma);
    com_release(frame->gpu.texture);
}

bool owned_views_valid(const GpuVideoFrame& frame) noexcept {
    if (!frame.texture || static_cast<bool>(frame.luma) != static_cast<bool>(frame.chroma)) return false;
    if (!frame.luma) return true; // The device can require the copy fallback.
    ID3D11ShaderResourceView* views[]{frame.luma, frame.chroma};
    const DXGI_FORMAT formats[]{DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM};
    for (u32 i = 0; i < 2; ++i) {
        ID3D11Resource* resource = nullptr;
        views[i]->GetResource(&resource);
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        views[i]->GetDesc(&desc);
        const bool valid = resource == frame.texture && desc.Format == formats[i] &&
                           desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
                           desc.Texture2D.MostDetailedMip == 0 && desc.Texture2D.MipLevels == 1;
        com_release(resource);
        if (!valid) return false;
    }
    return true;
}

bool wait_frame(VideoStreamDecoders* decoders, u32 sequence, u64 deadline, VideoStreamFrame* frame) noexcept {
    do {
        if (video_stream_decoders_frame(decoders, 0x1234, frame)) {
            if (frame->sequence >= sequence) return true;
            release_frame(frame);
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return false;
}

bool mark_frame(const GpuVideoFrame& frame, u32 offset, u32* expected) noexcept {
    ID3D11DeviceChild* objects[]{frame.texture, frame.luma, frame.chroma};
    for (u32 i = 0; i < 3; ++i) {
        if (!objects[i]) continue;
        if (!mark_lifetime(objects[i], offset + i)) return false;
        *expected |= 1u << (offset + i);
    }
    return true;
}

u32 released_mask() noexcept {
    u32 mask = 0;
    for (u32 i = 0; i < 6; ++i)
        if (InterlockedCompareExchange(&g_lifetime_released[i], 0, 0)) mask |= 1u << i;
    return mask;
}

bool frame_ownership(ID3D11Device* device, ID3D11DeviceContext* context, Buffers* b) noexcept {
    VideoStreamDecoders decoders{};
    video_stream_decoders_initialize(&decoders);
    video_stream_decoders_set_device(&decoders, device);
    const VideoFrameSink sink = video_stream_decoders_sink(&decoders);
    VideoStreamFrame first{}, repeated{}, replacement{}, absent{};
    Capture capture{device, context};
    const Sps base{0, 66, 8, 6, 2, 1, false, 0, 0, 31};
    const Pps base_pps{0, 0};
    const Sps changed{1, 100, 10, 8, 0, 2, true, 0, 0, 40};
    const Pps changed_pps{3, 1};
    const u64 deadline = GetTickCount64() + 2'500;
    const auto submit = [&](u32 rtp, bool keyframe) noexcept {
        const bool ok = b->out.ok && sink.push_frame(sink.context, 0x1234,
                                                     {b->out.data, b->out.count}, rtp, keyframe, true);
        b->out.count = 0;
        b->out.ok = true;
        return ok;
    };
    u32 expected = 0;
    write_idr(&b->out, &b->bits, base, base_pps, 9, 0);
    bool ok = submit(900, true) && wait_frame(&decoders, 1, deadline, &first) &&
              owned_views_valid(first.gpu) && mark_frame(first.gpu, 0, &expected);
    if (ok) {
        write_p(&b->out, &b->bits, base, base_pps, 1, 0);
        ok = submit(901, false) && wait_frame(&decoders, first.sequence + 1, deadline, &repeated) &&
             repeated.gpu.texture == first.gpu.texture && repeated.gpu.luma == first.gpu.luma &&
             repeated.gpu.chroma == first.gpu.chroma && !released_mask();
    }
    release_frame(&repeated);
    if (ok) {
        write_idr(&b->out, &b->bits, changed, changed_pps, 10, 0);
        ok = submit(902, true) && wait_frame(&decoders, first.sequence + 2, deadline, &replacement) &&
             replacement.gpu.texture != first.gpu.texture && owned_views_valid(replacement.gpu) &&
             mark_frame(replacement.gpu, 3, &expected) && !released_mask();
    }
    // Stream end drops both producer and latest references. UI queries remain valid.
    sink.end_stream(sink.context, 0x1234);
    ok = ok && !video_stream_decoders_frame(&decoders, 0x1234, &absent) && !released_mask();
    if (ok) {
        expect(&capture, base, 9);
        verify(&capture, {&first.gpu, 900, first.display_width, first.display_height, true});
        expect(&capture, changed, 10);
        verify(&capture, {&replacement.gpu, 902, replacement.display_width, replacement.display_height, true});
        ok = !capture.mismatches && owned_views_valid(first.gpu) && owned_views_valid(replacement.gpu);
        say_number("h264 stream owned plane views ", first.gpu.luma && replacement.gpu.luma ? 4 : 0);
    }
    release_frame(&absent);
    release_frame(&first);
    release_frame(&replacement);
    video_stream_decoders_release(&decoders);
    com_release(capture.staging);
    // The preceding readback completes the submitted copies. Flush releases
    // deferred D3D objects without a long idle timeout or a working-set trim.
    context->Flush();
    const u64 release_deadline = GetTickCount64() + 1000;
    while (released_mask() != expected && GetTickCount64() < release_deadline) {
        context->Flush();
        Sleep(1);
    }
    return ok && released_mask() == expected;
}

int run(ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    constexpr u32 kBytes = 14u * 1024u * 1024u;
    auto* memory = static_cast<u8*>(VirtualAlloc(nullptr, u64{kBytes} * 2u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return 1;
    Buffers b{{memory, kBytes, 0, true}, {memory + kBytes, kBytes, 0, true}};
    Capture capture{device, context};
    H264StreamDecoder* decoder = h264_stream_decoder_create({device, 0x1234, {&capture, on_frame, on_keyframe}, true});
    int status = decoder ? 0 : 2;
    const auto finish = [&](int code) noexcept {
        if (!status) status = code;
    };
    // Small Constrained Baseline stream: two-slice IDR then P_Skip frames.
    const Sps base{0, 66, 8, 6, 2, 1, false, 0, 0, 31};
    const Pps base_pps{0, 0};
    if (!status) {
        expect(&capture, base, 1);
        write_p(&b.out, &b.bits, base, base_pps, 1, 0); // before any IDR: dropped
        push(decoder, &b, 10);
        write_idr(&b.out, &b.bits, base, base_pps, 1, 0, 2);
        push(decoder, &b, 100);
        for (u16 i = 1; i <= 3; ++i) {
            write_p(&b.out, &b.bits, base, base_pps, i, 0);
            push(decoder, &b, 100u + i);
        }
        const auto stats = h264_stream_decoder_stats(decoder);
        if (stats.unsupported || stats.reinitializations == 0) {
            say("h264 stream gpu: DXVA H.264 short-slice decoding unavailable; skipped\n");
            h264_stream_decoder_destroy(decoder);
            com_release(capture.staging);
            VirtualFree(memory, 0, MEM_RELEASE);
            return 0;
        }
        if (capture.frames != 4 || capture.keyframes != 1 || capture.mismatches || capture.last_rtp != 103 ||
            capture.requests != 1 || stats.decoded != 4)
            finish(10);
    }
    // Lost reference picture: frozen output and a throttled keyframe request.
    if (!status) {
        write_p(&b.out, &b.bits, base, base_pps, 5, 0);
        push(decoder, &b, 105);
        write_p(&b.out, &b.bits, base, base_pps, 6, 0);
        push(decoder, &b, 106);
        const auto stats = h264_stream_decoder_stats(decoder);
        if (capture.frames != 4 || stats.gaps != 1 || capture.requests != 2) finish(11);
    }
    // New ids, High profile and new geometry at an IDR: decoder recreated.
    const Sps high{1, 100, 10, 8, 0, 2, true, 0, 0, 40};
    const Pps high_pps{3, 1};
    if (!status) {
        expect(&capture, high, 2);
        write_idr(&b.out, &b.bits, high, high_pps, 2, 0, 3);
        push(decoder, &b, 200);
        write_p(&b.out, &b.bits, high, high_pps, 1, 2, true, 2);
        push(decoder, &b, 201);
        const auto stats = h264_stream_decoder_stats(decoder);
        if (capture.frames != 6 || capture.mismatches || stats.reinitializations != 2 || stats.coded_width != 160)
            finish(12);
    }
    // Damaged input (an incomplete access unit, missing leading slices):
    // never submitted; recovery at the next IDR.
    if (!status) {
        write_p(&b.out, &b.bits, high, high_pps, 2, 4);
        push(decoder, &b, 202, false);
        write_slice(&b.out, &b.bits, high, high_pps, {false, 2, 3, 6, 0, 40, 40, 0});
        push(decoder, &b, 203);
        expect(&capture, high, 3);
        write_idr(&b.out, &b.bits, high, high_pps, 3, 1);
        push(decoder, &b, 300);
        const auto stats = h264_stream_decoder_stats(decoder);
        // Three damaged pictures: these two plus the first push, which arrived
        // before any parameter set.
        if (capture.frames != 7 || capture.mismatches || stats.damaged != 3 || stats.gpu_failures) finish(13);
    }
    // 1080p (cropped), 1440p and 4K keyframes, one slice per macroblock row.
    const Sps large[] = {{2, 100, 120, 68, 2, 1, true, 0, 8, 40},
                         {3, 100, 160, 90, 2, 1, true, 0, 0, 50},
                         {4, 100, 240, 135, 2, 1, true, 0, 0, 51}};
    for (u32 i = 0; i < 3 && !status; ++i) {
        const Pps pps{static_cast<u8>(10 + i), large[i].id};
        expect(&capture, large[i], 4 + i, large[i].width_mbs);
        const u32 before = capture.frames;
        write_idr(&b.out, &b.bits, large[i], pps, 4 + i, static_cast<u16>(i), large[i].height_mbs, true, true);
        push(decoder, &b, 400 + i);
        write_p(&b.out, &b.bits, large[i], pps, 1, 2, true, 4);
        push(decoder, &b, 500 + i);
        const auto stats = h264_stream_decoder_stats(decoder);
        if (i == 2 && stats.unsupported) {
            say("h264 stream gpu: this GPU does not decode 4K H.264; 4K case skipped\n");
            break;
        }
        if (capture.frames != before + 2 || capture.mismatches || stats.unsupported || stats.gpu_failures) {
            say_number("frames ", capture.frames - before);
            say_number("mismatches ", capture.mismatches);
            say_number("unsupported ", stats.unsupported);
            say_number("gpu_failures ", stats.gpu_failures);
            say_number("last_status ", stats.last_status);
            say_number("damaged ", stats.damaged);
            say_number("dropped ", stats.dropped);
            say_number("reinitializations ", stats.reinitializations);
            say_number("coded_width ", stats.coded_width);
            say_number("surfaces ", stats.surfaces);
            finish(20 + static_cast<int>(i));
        }
    }
    // Idle release (call view hidden): the DXVA decoder, its surface pool and
    // the display copy are freed; the next picture asks for a keyframe and the
    // IDR recreates everything with identical output.
    if (!status && decoder) {
        const auto before = h264_stream_decoder_stats(decoder);
        const u32 frames = capture.frames, requests = capture.requests;
        if (!h264_stream_decoder_release_idle(decoder)) finish(40);
        const auto idle = h264_stream_decoder_stats(decoder);
        if (idle.surfaces || idle.coded_width || idle.idle_releases != before.idle_releases + 1) finish(41);
        if (h264_stream_decoder_release_idle(decoder)) finish(42); // Nothing left to free.
        expect(&capture, base, 8);
        write_p(&b.out, &b.bits, base, base_pps, 1, 0);
        push(decoder, &b, 800);
        if (capture.frames != frames || capture.requests != requests + 1) finish(43);
        write_idr(&b.out, &b.bits, base, base_pps, 8, 2);
        push(decoder, &b, 801);
        write_p(&b.out, &b.bits, base, base_pps, 1, 0);
        push(decoder, &b, 802);
        const auto resumed = h264_stream_decoder_stats(decoder);
        if (capture.frames != frames + 2 || capture.mismatches || !resumed.surfaces ||
            resumed.reinitializations != before.reinitializations + 1)
            finish(44);
    }
    // Asynchronous decoder: same content through the decode thread.
    if (decoder) h264_stream_decoder_destroy(decoder);
    decoder = nullptr;
    if (!status) {
        Capture threaded{device, context};
        expect(&threaded, base, 7);
        decoder = h264_stream_decoder_create({device, 0x1234, {&threaded, on_frame, on_keyframe}, false});
        if (!decoder) finish(30);
        else {
            write_idr(&b.out, &b.bits, base, base_pps, 7, 0);
            push(decoder, &b, 700);
            for (u16 i = 1; i <= 5; ++i) {
                write_p(&b.out, &b.bits, base, base_pps, i, 0);
                push(decoder, &b, 700u + i);
            }
            for (u32 wait = 0; wait < 400 && h264_stream_decoder_stats(decoder).decoded < 6; ++wait) Sleep(5);
            h264_stream_decoder_destroy(decoder);
            decoder = nullptr;
            // The callback ran on the decode thread; all reads finished before destroy returned.
            if (threaded.frames != 6 || threaded.mismatches || threaded.last_rtp != 705) finish(31);
            com_release(threaded.staging);
        }
    }
    if (!status && !frame_ownership(device, context, &b)) finish(45);
    com_release(capture.staging);
    VirtualFree(memory, 0, MEM_RELEASE);
    return status;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_h264_stream_gpu_tests_entry() noexcept {
    using namespace maicord;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                       2, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(status)) {
        say("h264 stream gpu: no hardware D3D11 video device; skipped\n");
        ExitProcess(0);
    }
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    const int result = run(device, context);
    if (result) {
        char text[] = "h264 stream gpu failed: 00\n";
        text[24] = static_cast<char>('0' + result / 10 % 10);
        text[25] = static_cast<char>('0' + result % 10);
        say(text);
    }
    context->Release();
    device->Release();
    ExitProcess(static_cast<UINT>(result));
}
