// The opt-in measurement of Go Live's GPU work per streamed picture (MAICORD_GO_LIVE_GPU_PROBE=<seconds>; status 77
// otherwise, so the suite never runs it). It runs the real producer chain on the hardware device and prints counts and
// times only, never a pixel:
//   source   a synthetic desktop at 60 pictures a second: 2560x1440 text-like rows with the middle half a window
//            scrolling 16 rows a picture, or (MAICORD_GO_LIVE_GPU_CONTENT=pan) a soft photographic texture panning 7
//            columns and 5 rows a picture as a game does; or with MAICORD_GO_LIVE_GPU_SOURCE=screen (and
//            MAICORD_CAPTURE_PROBE=1) the real primary screen through the real capture (WGC or Desktop Duplication);
//   convert  gpu_stream_encoder_submit: the BGRA-to-NV12 conversion the encoder queues on the immediate context;
//   preview  the self-preview: each new encoder picture converted to RGBA (video_frame_gpu_present, the call view's
//            conversion into its preview atlas page) when MAICORD_GO_LIVE_GPU_PREVIEW=1;
//   encode   the hardware encoder (MAICORD_GO_LIVE_GPU_CODEC=h264|h265|av1, else the best one) at the stream's extent
//            (MAICORD_GO_LIVE_GPU_EXTENT=1440|1080) and its top bitrate; MAICORD_GO_LIVE_GPU_KEYFRAMES=n forces n
//            keyframes a second; MAICORD_GO_LIVE_GPU_PSNR=1 then decodes the stream with MaiCord's own decoder and
//            prints its PSNR against the synthetic source.
// Convert and preview are timed with D3D11 timestamp queries (GPU microseconds per picture: mean, 95th percentile,
// maximum); the encoder runs on the video engine, outside the queries: its units' latency (submit to unit, CPU clock)
// and sizes are printed per kind. The video engine's share is read beside it from the GPU Engine performance counters
// of this process (`\GPU Engine(pid_<id>_*)\Utilization Percentage`; the probe prints its process id once the pictures
// start).
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/media/video/stream_quality.hpp"
#include "maicord/platform/encode/gpu_stream_encoder.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"
#include "maicord/sys/d3d11.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {
constexpr u32 kSourceWidth = 2560, kSourceHeight = 1440, kPages = 8, kScroll = 16, kFps = 60;
constexpr u32 kSamples = 1u << 14, kRing = 16;

struct Series {
    u32 values[kSamples];
    u32 count;
    u64 sum;
    u32 maximum;
    void add(u32 value) noexcept {
        if (count < kSamples)
            values[count++] = value;
        sum += value;
        maximum = value > maximum ? value : maximum;
    }
};

struct Pending {
    ID3D11Query* disjoint;
    ID3D11Query* stamps[3];
    bool busy, previewed;
};

// A unit kept for the quality check: its bytes in Probe::stream and the source page it encodes.
struct Stored {
    u32 offset, bytes, rtp_timestamp, page;
};
constexpr u32 kStreamBytes = 96u << 20;
struct Probe {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11DeviceContext1* context1;
    ID3D11Multithread* guard;
    GpuStreamEncoder* encoder;
    ID3D11Texture2D* pages[kPages];
    ID3D11Texture2D* preview_target;
    ID3D11UnorderedAccessView* preview_view;
    VideoFrameGpu converter;
    Pending ring[kRing];
    u32 frame, preview_sequence, keyframe_every, unmeasured, previews;
    bool preview, psnr;
    Series convert, preview_us, delta_latency, key_latency;
    u64 delta_bytes, key_bytes;
    volatile LONG units, keyframes, captured, ended;
    // The quality check (MAICORD_GO_LIVE_GPU_PSNR=1, synthetic source): every unit, and each picture's time and page.
    u8* stream;
    u32 stream_used, stored_count;
    Stored stored[kSamples];
    u64 picture_time[kSamples];
};
Probe* g;

u64 now_us() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}

u32 environment_u32(const wchar_t* name, u32 fallback) noexcept {
    wchar_t text[16]{};
    const DWORD length = GetEnvironmentVariableW(name, text, 16);
    if (!length || length >= 16)
        return fallback;
    u32 value = 0;
    for (DWORD i = 0; i < length && text[i] >= L'0' && text[i] <= L'9'; ++i)
        value = value * 10 + static_cast<u32>(text[i] - L'0');
    return value;
}

bool environment_is(const wchar_t* name, const wchar_t* expected) noexcept {
    wchar_t text[16]{};
    const DWORD length = GetEnvironmentVariableW(name, text, 16);
    for (DWORD i = 0; i < length; ++i)
        if (text[i] != expected[i])
            return false;
    return length && !expected[length];
}

// Text-like rows over a soft gradient: glyphs of 2x2-pixel strokes in 8x12 cells, two cells in three inked, a line
// every 20 rows.
u32 texel(u32 x, u32 y) noexcept {
    const u32 line = y / 20, row = y % 20, cell = x / 8;
    u32 hash = (line * 0x9E3779B1u) ^ (cell * 0x85EBCA77u);
    hash ^= hash >> 15;
    hash *= 0x2C1B3C6Du;
    hash ^= hash >> 12;
    const bool inked = row >= 4 && row < 16 && hash % 3 != 0 && x % 8 < 6 &&
                       ((hash >> (((row - 4) / 2) * 3 + (x % 8) / 2)) & 1u);
    const u32 base = 200 + (x * 40 / kSourceWidth) - (y % 1440) * 30 / 1440;
    const u32 value = inked ? 40 + (hash >> 24) % 40 : base;
    return 0xFF000000u | value << 16 | (value - 10) << 8 | (value + 10 > 255 ? 255 : value + 10);
}

// Value noise of three octaves (64, 16 and 4 pixels), each a bilinear blend of hashed lattice values: a soft
// photographic texture for MAICORD_GO_LIVE_GPU_CONTENT=pan.
u32 lattice(u32 x, u32 y, u32 seed) noexcept {
    u32 h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    return (h ^ (h >> 15)) & 0xFF;
}
u32 octave(u32 x, u32 y, u32 shift, u32 seed) noexcept {
    const u32 cell = 1u << shift, mask = cell - 1, gx = x >> shift, gy = y >> shift, fx = x & mask, fy = y & mask;
    const u32 top = lattice(gx, gy, seed) * (cell - fx) + lattice(gx + 1, gy, seed) * fx;
    const u32 bottom = lattice(gx, gy + 1, seed) * (cell - fx) + lattice(gx + 1, gy + 1, seed) * fx;
    return (top * (cell - fy) + bottom * fy) >> (2 * shift);
}
u32 natural(u32 x, u32 y) noexcept {
    const u32 r = (octave(x, y, 6, 1) * 5 + octave(x, y, 4, 2) * 2 + octave(x, y, 2, 3)) / 8;
    const u32 g = (octave(x, y, 6, 4) * 5 + octave(x, y, 4, 5) * 2 + octave(x, y, 2, 6)) / 8;
    const u32 b = (octave(x, y, 6, 7) * 5 + octave(x, y, 4, 8) * 2 + octave(x, y, 2, 9)) / 8;
    return 0xFF000000u | r << 16 | g << 8 | b;
}
bool g_pan;

// A desktop with one window scrolling: the middle half of the screen moves kScroll rows a picture, the rest stays.
// The pan content instead moves the whole picture 7 columns and 5 rows a picture, as a game or a video does.
u32 page_texel(u32 x, u32 y, u32 page) noexcept {
    if (g_pan)
        return natural(x + 7 * page, y + 5 * page);
    const bool window = x >= kSourceWidth / 4 && x < kSourceWidth * 3 / 4;
    return texel(x, y + (window ? page * kScroll : 0));
}

bool create_pages(Probe& p) noexcept {
    auto* pixels = static_cast<u32*>(pages_allocate(usize{kSourceWidth} * kSourceHeight * 4));
    if (!pixels)
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kSourceWidth;
    desc.Height = kSourceHeight;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; // As WGC's frame pool surfaces.
    bool ok = true;
    for (u32 i = 0; i < kPages && ok; ++i) {
        for (u32 y = 0; y < kSourceHeight; ++y)
            for (u32 x = 0; x < kSourceWidth; ++x)
                pixels[usize{y} * kSourceWidth + x] = page_texel(x, y, i);
        const D3D11_SUBRESOURCE_DATA data{pixels, kSourceWidth * 4, 0};
        ok = SUCCEEDED(p.device->CreateTexture2D(&desc, &data, &p.pages[i]));
    }
    pages_release(pixels);
    return ok;
}

bool create_queries(Probe& p) noexcept {
    D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
    bool ok = true;
    for (Pending& slot : p.ring) {
        ok = ok && SUCCEEDED(p.device->CreateQuery(&disjoint, &slot.disjoint));
        for (ID3D11Query*& query : slot.stamps)
            ok = ok && SUCCEEDED(p.device->CreateQuery(&stamp, &query));
    }
    return ok;
}

bool create_preview(Probe& p, u32 width, u32 height) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width + 16; // An atlas page: the picture and its padding.
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(p.device->CreateTexture2D(&desc, nullptr, &p.preview_target)) &&
           SUCCEEDED(p.device->CreateUnorderedAccessView(p.preview_target, nullptr, &p.preview_view));
}

// Reads every finished slot's times (never waits on the GPU).
void collect(Probe& p) noexcept {
    for (Pending& slot : p.ring) {
        if (!slot.busy)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        u64 stamps[3]{};
        p.guard->Enter();
        bool ready = p.context->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        for (u32 i = 0; ready && i < 3; ++i)
            ready = p.context->GetData(slot.stamps[i], &stamps[i], 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        p.guard->Leave();
        if (!ready)
            continue;
        slot.busy = false;
        if (disjoint.Disjoint || !disjoint.Frequency)
            continue;
        const auto us = [&](u64 from, u64 to) noexcept {
            return static_cast<u32>((to - from) * 1'000'000 / disjoint.Frequency);
        };
        p.convert.add(us(stamps[0], stamps[1]));
        if (slot.previewed)
            p.preview_us.add(us(stamps[1], stamps[2]));
    }
}

// One picture through the chain, on whichever thread delivers it.
void picture(Probe& p, const CaptureFrame& frame) noexcept {
    collect(p);
    Pending& slot = p.ring[p.frame % kRing];
    const bool measured = !slot.busy;
    if (!measured)
        ++p.unmeasured;
    if (p.keyframe_every && p.frame % p.keyframe_every == 0)
        gpu_stream_encoder_request_keyframe(p.encoder);
    ++p.frame;
    if (measured) {
        p.guard->Enter();
        p.context->Begin(slot.disjoint);
        p.context->End(slot.stamps[0]);
        p.guard->Leave();
    }
    gpu_stream_encoder_submit(p.encoder, frame);
    if (measured) {
        p.guard->Enter();
        p.context->End(slot.stamps[1]);
        p.guard->Leave();
    }
    GpuVideoFrame preview{};
    u32 sequence = 0;
    slot.previewed = false;
    if (p.preview && gpu_stream_encoder_preview(p.encoder, &preview, &sequence)) {
        if (sequence != p.preview_sequence &&
            SUCCEEDED(video_frame_gpu_present(&p.converter, p.device, p.context1, preview, p.preview_view, 0, 0, 1))) {
            p.preview_sequence = sequence;
            slot.previewed = true;
            ++p.previews;
        }
        gpu_release(preview.chroma);
        gpu_release(preview.luma);
        gpu_release(preview.texture);
    }
    if (measured) {
        p.guard->Enter();
        p.context->End(slot.stamps[2]);
        p.context->End(slot.disjoint);
        p.context->Flush();
        p.guard->Leave();
        slot.busy = true;
    }
}

void on_unit(void*, const GpuStreamAccessUnit& unit) noexcept {
    const u64 latency = now_us() - unit.timestamp_us;
    InterlockedIncrement(&g->units);
    if (unit.keyframe) {
        InterlockedIncrement(&g->keyframes);
        g->key_latency.add(static_cast<u32>(latency));
        g->key_bytes += unit.data.count;
    } else {
        g->delta_latency.add(static_cast<u32>(latency));
        g->delta_bytes += unit.data.count;
    }
    if (!g->psnr || g->stored_count == kSamples || g->stream_used + unit.data.count > kStreamBytes)
        return;
    u32 picture = g->frame < kSamples ? g->frame : kSamples;
    while (picture && g->picture_time[picture - 1] != unit.timestamp_us)
        --picture;
    if (!picture)
        return;
    copy_bytes(g->stream + g->stream_used, unit.data.data, unit.data.count);
    g->stored[g->stored_count++] = {g->stream_used, static_cast<u32>(unit.data.count), unit.rtp_timestamp,
                                    (picture - 1) % kPages};
    g->stream_used += static_cast<u32>(unit.data.count);
}

// The quality check: every stored unit through MaiCord's own decoder, every sixth picture converted to RGBA as the
// call view converts it, read back and compared with its source page.
struct Quality {
    ID3D11Texture2D* staging;
    u32 width, height, decoded, measured;
    double mse_sum, worst_mse;
};
void on_decoded(void* context, u32, const GpuStreamFrame& frame) noexcept {
    auto& q = *static_cast<Quality*>(context);
    Probe& p = *g;
    if (q.decoded++ % 6)
        return;
    u32 page = kPages;
    for (u32 i = 0; i < p.stored_count && page == kPages; ++i)
        if (p.stored[i].rtp_timestamp == frame.rtp_timestamp)
            page = p.stored[i].page;
    const GpuVideoFrame& picture = *frame.gpu;
    if (page == kPages || picture.width != q.width || picture.height != q.height ||
        FAILED(video_frame_gpu_present(&p.converter, p.device, p.context1, picture, p.preview_view, 0, 0, 1)))
        return;
    p.context->CopyResource(q.staging, p.preview_target);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(p.context->Map(q.staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return;
    double sum = 0;
    for (u32 y = 0; y < q.height; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) + usize{y} * mapped.RowPitch;
        for (u32 x = 0; x < q.width; ++x) {
            const u32 source = page_texel(x, y, page);
            const i32 r = row[x * 4] - static_cast<i32>((source >> 16) & 0xFF);
            const i32 gr = row[x * 4 + 1] - static_cast<i32>((source >> 8) & 0xFF);
            const i32 b = row[x * 4 + 2] - static_cast<i32>(source & 0xFF);
            sum += r * r + gr * gr + b * b;
        }
    }
    p.context->Unmap(q.staging, 0);
    const double mse = sum / (3.0 * q.width * q.height);
    q.mse_sum += mse;
    q.worst_mse = mse > q.worst_mse ? mse : q.worst_mse;
    ++q.measured;
}
void on_decoder_keyframe(void*, u32) noexcept {}

// 10 log10(255^2 / mse) in hundredths of a dB (log10 by its series: the probe has no C library).
u32 psnr_centi(double mse) noexcept {
    if (mse <= 0)
        return 99'99;
    double ratio = 65025.0 / mse, decades = 0;
    while (ratio >= 10) {
        ratio /= 10;
        decades += 1;
    }
    // ln(ratio) with ratio in [1, 10): 2 atanh((r - 1) / (r + 1)).
    const double t = (ratio - 1) / (ratio + 1), t2 = t * t;
    double term = t, series = 0;
    for (u32 k = 1; k < 60; k += 2) {
        series += term / k;
        term *= t2;
    }
    return static_cast<u32>((decades + 2 * series / 2.302585092994046) * 1000 + 0.5);
}

void measure_quality(Probe& p, GpuStreamCodec codec, u32 width, u32 height) noexcept {
    static Quality q;
    q.width = width;
    q.height = height;
    D3D11_TEXTURE2D_DESC desc{};
    p.preview_target->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    GpuStreamDecoder* decoder =
        SUCCEEDED(p.device->CreateTexture2D(&desc, nullptr, &q.staging))
            ? gpu_stream_decoder_create({p.device, 0xE0, {&q, on_decoded, on_decoder_keyframe}, codec, true})
            : nullptr;
    for (u32 i = 0; decoder && i < p.stored_count; ++i)
        gpu_stream_decoder_push(decoder, {p.stream + p.stored[i].offset, p.stored[i].bytes}, p.stored[i].rtp_timestamp);
    gpu_stream_decoder_destroy(decoder);
    com_release(q.staging);
    test_write_metric("quality: units decoded: ", q.decoded);
    test_write_metric("quality: pictures compared: ", q.measured);
    test_write_metric("quality: mean PSNR (centi-dB): ", q.measured ? psnr_centi(q.mse_sum / q.measured) : 0);
    test_write_metric("quality: worst PSNR (centi-dB): ", q.measured ? psnr_centi(q.worst_mse) : 0);
}
void on_encoder_ended(void*, GpuStreamEncoderEnd) noexcept {
    InterlockedIncrement(&g->ended);
}
void on_capture(void*, const CaptureFrame& frame) noexcept {
    InterlockedIncrement(&g->captured);
    picture(*g, frame);
}
void on_capture_ended(void*, CaptureEnd) noexcept {
    InterlockedIncrement(&g->ended);
}

// Mean, 95th percentile and maximum of a series, in its unit.
void report(const char* label, Series& s) noexcept {
    for (u32 i = 1; i < s.count; ++i) // Insertion sort: a few thousand values once.
        for (u32 j = i; j && s.values[j - 1] > s.values[j]; --j) {
            const u32 t = s.values[j];
            s.values[j] = s.values[j - 1];
            s.values[j - 1] = t;
        }
    test_write_text(label);
    test_write_text(" n=");
    test_write_u64(s.count);
    test_write_text(" mean=");
    test_write_u64(s.count ? s.sum / s.count : 0);
    test_write_text(" p95=");
    test_write_u64(s.count ? s.values[s.count * 95 / 100] : 0);
    test_write_text(" max=");
    test_write_u64(s.maximum);
    test_write_line("");
}

CaptureSource g_sources[64];
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_go_live_gpu_probe_entry) {
    using namespace maicord;
    const u32 seconds = environment_u32(L"MAICORD_GO_LIVE_GPU_PROBE", 0);
    if (!seconds) {
        test_write_line("SKIP go_live_gpu_probe (opt-in: MAICORD_GO_LIVE_GPU_PROBE=<seconds>)");
        test_process_exit(77);
    }
    const bool screen = environment_is(L"MAICORD_GO_LIVE_GPU_SOURCE", L"screen");
    if (screen && !GetEnvironmentVariableW(L"MAICORD_CAPTURE_PROBE", nullptr, 0)) {
        test_write_line("SKIP go_live_gpu_probe (the screen source also needs MAICORD_CAPTURE_PROBE=1)");
        test_process_exit(77);
    }
    g = static_cast<Probe*>(pages_allocate(sizeof(Probe)));
    Probe& p = *g;
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context)))
        test_process_exit(1);
    p.device = device;
    p.context = context;
    p.context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&p.context1));
    p.context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&p.guard));
    const u32 codecs = gpu_stream_encoder_codecs(device);
    GpuStreamCodec codec{};
    if (environment_is(L"MAICORD_GO_LIVE_GPU_CODEC", L"h264"))
        codec = GpuStreamCodec::h264;
    else if (environment_is(L"MAICORD_GO_LIVE_GPU_CODEC", L"h265"))
        codec = GpuStreamCodec::h265;
    else if (environment_is(L"MAICORD_GO_LIVE_GPU_CODEC", L"av1"))
        codec = GpuStreamCodec::av1;
    else if (!gpu_stream_encoder_pick(codecs, codecs, &codec))
        test_process_exit(77);
    if (!(codecs & gpu_stream_codec_bit(codec))) {
        test_write_line("SKIP go_live_gpu_probe (the GPU does not encode that codec)");
        test_process_exit(77);
    }
    const bool full = environment_u32(L"MAICORD_GO_LIVE_GPU_EXTENT", 1440) != 1080;
    const u32 width = full ? 2560 : 1920, height = full ? 1440 : 1080;
    p.preview = environment_u32(L"MAICORD_GO_LIVE_GPU_PREVIEW", 0) != 0;
    g_pan = environment_is(L"MAICORD_GO_LIVE_GPU_CONTENT", L"pan");
    p.psnr = !screen && environment_u32(L"MAICORD_GO_LIVE_GPU_PSNR", 0) != 0;
    if (p.psnr && !(p.stream = static_cast<u8*>(pages_allocate(kStreamBytes))))
        test_process_exit(1);
    const u32 keyframes = environment_u32(L"MAICORD_GO_LIVE_GPU_KEYFRAMES", 0);
    p.keyframe_every = keyframes ? kFps / keyframes : 0;
    const StreamBitrate bitrate = stream_quality_bitrate({static_cast<u16>(height), kFps});
    p.encoder = gpu_stream_encoder_create(
        {device, codec, {width, height, kFps, bitrate.maximum}, {nullptr, on_unit, on_encoder_ended}});
    if (!p.encoder || !create_queries(p) || !create_preview(p, width, height) || (!screen && !create_pages(p)))
        test_process_exit(1);
    test_write_metric("codec (0 h264, 1 h265, 2 av1): ", static_cast<u8>(codec));
    test_write_metric("extent height: ", height);
    test_write_metric("bitrate: ", bitrate.maximum);
    test_write_metric("preview: ", p.preview);
    test_write_metric("forced keyframes per second: ", keyframes);
    test_write_metric("process id: ", GetCurrentProcessId());
    const u64 start = now_us();
    ScreenCapture* capture = nullptr;
    if (screen) {
        const u32 count = capture_sources({g_sources, 64});
        u32 primary = count;
        for (u32 i = 0; i < count && primary == count; ++i)
            if (g_sources[i].kind == CaptureSourceKind::screen && g_sources[i].primary)
                primary = i;
        CaptureOptions options{};
        options.device = device;
        options.source = g_sources[primary < count ? primary : 0];
        options.max_fps = kFps;
        options.cursor = true;
        capture = count ? capture_start(options, {nullptr, on_capture, on_capture_ended, nullptr}) : nullptr;
        if (!capture)
            test_process_exit(1);
        thread_sleep(seconds * 1000);
        capture_stop(capture);
    } else {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const u64 interval = 1'000'000 / kFps;
        for (u32 i = 0; i < seconds * kFps; ++i) {
            const u64 due = start + u64{i} * interval, now = now_us();
            if (due > now) {
                LARGE_INTEGER wait{};
                wait.QuadPart = -static_cast<LONGLONG>((due - now) * 10);
                SetWaitableTimer(timer, &wait, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            }
            CaptureFrame frame{};
            frame.texture = p.pages[i % kPages];
            frame.width = kSourceWidth;
            frame.height = kSourceHeight;
            frame.timestamp_us = now_us();
            InterlockedIncrement(&p.captured);
            if (i < kSamples)
                p.picture_time[i] = frame.timestamp_us;
            picture(p, frame);
        }
        CloseHandle(timer);
    }
    thread_sleep(200); // The last units and queries.
    collect(p);
    const u64 elapsed_ms = (now_us() - start) / 1000;
    const GpuStreamEncoderStats stats = gpu_stream_encoder_stats(p.encoder);
    gpu_stream_encoder_destroy(p.encoder);
    if (p.psnr && full)
        measure_quality(p, codec, width, height);
    test_write_metric("elapsed ms: ", elapsed_ms);
    test_write_metric("pictures captured: ", static_cast<u32>(p.captured));
    test_write_metric("encoder took: ", stats.submitted - stats.dropped);
    test_write_metric("encoder dropped: ", stats.dropped);
    test_write_metric("units: ", static_cast<u32>(p.units));
    test_write_metric("keyframes: ", static_cast<u32>(p.keyframes));
    test_write_metric("previews converted: ", p.previews);
    test_write_metric("pictures not timed (queries busy): ", p.unmeasured);
    report("convert gpu us:", p.convert);
    report("preview gpu us:", p.preview_us);
    report("delta unit latency us:", p.delta_latency);
    report("key unit latency us:", p.key_latency);
    const u32 deltas = p.delta_latency.count, keys = p.key_latency.count;
    test_write_metric("delta unit mean bytes: ", deltas ? p.delta_bytes / deltas : 0);
    test_write_metric("key unit mean bytes: ", keys ? p.key_bytes / keys : 0);
    test_write_metric("ended: ", static_cast<u32>(p.ended));
    test_process_exit(p.ended ? 1 : 0);
}
