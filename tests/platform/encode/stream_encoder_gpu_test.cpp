// The Go Live encoder on real hardware (label "gpu"): for every codec the GPU encodes, a synthetic BGRA picture
// sequence (smooth moving gradients and a soft disc) goes through the encoder's GPU conversion and the hardware Media
// Foundation transform, and MaiCord's own stream decoder and video conversion bring it back: every picture must reach
// at least 40 dB PSNR against the source. Along the way: keyframes on request (also with a still capture), the
// self-preview picture, a reconfiguration to another extent, and that a WARP device offers no codec. Skipped when the
// GPU has no hardware video encoder at all.
#include "maicord/platform/encode/gpu_stream_encoder.hpp"
#include "maicord/core/time.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"
#include "maicord/sys/d3d11.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {

// Short, to keep the suite's time: every codec, four pictures; one size change. 720p and 540p: extents every hardware
// encoder here takes (AMD's reopened H.264 transform produced nothing at 480x272).
constexpr u32 kWidth = 1280, kHeight = 720, kFrames = 4, kKeyframeAt = 2, kBitrate = 12'000'000;
constexpr u32 kResizedWidth = 960, kResizedHeight = 540;
// A real stream's rate: some hardware encoders refuse much higher frame rates in their media type.
constexpr u32 kMaxFps = 60;
constexpr u64 kFrameUs = 1'000'000 / kMaxFps + 1;
constexpr u32 kUnits = 160, kUnitBytes = 16u * 1024u * 1024u;
// PSNR >= 40 dB is MSE <= 255^2 / 10^4.
constexpr double kMaximumMse = 65025.0 / 10000.0;

struct Unit {
    u32 offset, bytes;
    u64 timestamp_us;
    u32 rtp_timestamp;
    u16 width, height;
    bool keyframe;
};

struct Collector {
    SRWLOCK lock;
    Unit units[kUnits];
    u32 count;
    u32 used;
    bool ended;
    bool overflow;
};

alignas(64) u8 g_unit_bytes[kUnitBytes];
alignas(64) u8 g_pixels[kWidth * kHeight * 4];
Collector g_collector;

void on_unit(void* context, const GpuStreamAccessUnit& unit) noexcept {
    auto* c = static_cast<Collector*>(context);
    AcquireSRWLockExclusive(&c->lock);
    if (c->count < kUnits && c->used + unit.data.count <= kUnitBytes) {
        for (u32 i = 0; i < unit.data.count; ++i)
            g_unit_bytes[c->used + i] = unit.data.data[i];
        c->units[c->count++] = {c->used, static_cast<u32>(unit.data.count), unit.timestamp_us, unit.rtp_timestamp,
                                unit.width, unit.height, unit.keyframe};
        c->used += static_cast<u32>(unit.data.count);
    } else {
        c->overflow = true;
    }
    ReleaseSRWLockExclusive(&c->lock);
}

void on_ended(void* context, GpuStreamEncoderEnd) noexcept {
    auto* c = static_cast<Collector*>(context);
    AcquireSRWLockExclusive(&c->lock);
    c->ended = true;
    ReleaseSRWLockExclusive(&c->lock);
}

u32 unit_count() noexcept {
    AcquireSRWLockShared(&g_collector.lock);
    const u32 count = g_collector.count;
    ReleaseSRWLockShared(&g_collector.lock);
    return count;
}

// Waits (bounded) for the encoder thread to deliver `count` units.
bool wait_units(u32 count) noexcept {
    for (u32 i = 0; i < 1000 && unit_count() < count; ++i)
        Sleep(1);
    return unit_count() >= count;
}

// The capture's clock (the client clock in microseconds): the next picture at least a 60 fps interval after `last`.
u64 next_timestamp(u64 last) noexcept {
    const Clock clock = clock_initialize();
    for (;;) {
        const u64 now = clock_microseconds(clock, 0, clock_now());
        if (now >= last + kFrameUs)
            return now;
        Sleep(1);
    }
}

u32 triangle(u32 t) noexcept {
    t %= 512;
    return t < 256 ? t : 511 - t;
}

// The source picture `index`: BGRA, every channel within 24-224 (no clipping in limited range).
void source_pixel(u32 index, u32 x, u32 y, u8* bgra) noexcept {
    double r = 24 + triangle(x / 2 + 6 * index) * 200.0 / 255.0;
    double g = 24 + triangle(y + 4 * index) * 200.0 / 255.0;
    double b = 24 + triangle((x + y) / 3 + 9 * index) * 200.0 / 255.0;
    const double dx = static_cast<double>(x) - (300.0 + 20.0 * index), dy = static_cast<double>(y) - 360.0;
    const double d = (dx * dx + dy * dy) / (140.0 * 140.0);
    if (d < 1) {
        const double f = (1 - d) * 0.6;
        r = r * (1 - f) + 220 * f;
        g = g * (1 - f) + 60 * f;
        b = b * (1 - f) + 200 * f;
    }
    bgra[0] = static_cast<u8>(b + 0.5);
    bgra[1] = static_cast<u8>(g + 0.5);
    bgra[2] = static_cast<u8>(r + 0.5);
    bgra[3] = 255;
}

void fill(u32 index) noexcept {
    for (u32 y = 0; y < kHeight; ++y)
        for (u32 x = 0; x < kWidth; ++x)
            source_pixel(index, x, y, g_pixels + (u64{y} * kWidth + x) * 4);
}

double natural_log(double x) noexcept {
    i32 exponent = 0;
    while (x >= 2) {
        x *= 0.5;
        ++exponent;
    }
    while (x < 1) {
        x *= 2;
        --exponent;
    }
    const double y = (x - 1) / (x + 1), y2 = y * y;
    double sum = 0, term = y;
    for (u32 k = 1; k < 60; k += 2) {
        sum += term / k;
        term *= y2;
    }
    return 2 * sum + exponent * 0.6931471805599453;
}

// Centi-decibels (4000 is 40 dB).
u64 psnr_centi(double mse) noexcept {
    if (mse <= 0)
        return 99'99;
    const double db = 10 * natural_log(65025.0 / mse) / natural_log(10.0);
    return db <= 0 ? 0 : static_cast<u64>(db * 100);
}

const char* codec_name(GpuStreamCodec codec) noexcept {
    return codec == GpuStreamCodec::h264 ? "h264" : codec == GpuStreamCodec::h265 ? "h265" : "av1";
}

struct Decode {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11Texture2D* target;
    ID3D11UnorderedAccessView* target_view;
    ID3D11Texture2D* staging;
    VideoFrameGpu converter;
    u32 frames, measured, failures;
    double worst_mse;
    // The source picture of each delivered unit, by RTP timestamp.
    u32 rtp[kUnits];
    u32 picture[kUnits];
    u32 units;
};

// MaiCord's NV12-to-RGB conversion of the decoded picture, read back and compared with the source.
void on_frame(void* context, u32, const GpuStreamFrame& frame) noexcept {
    auto* d = static_cast<Decode*>(context);
    ++d->frames;
    const GpuVideoFrame& gpu = *frame.gpu;
    if (gpu.width != kWidth || gpu.height != kHeight)
        return; // The reconfigured pictures are checked for decoding only.
    u32 index = kUnits;
    for (u32 i = 0; i < d->units; ++i)
        if (d->rtp[i] == frame.rtp_timestamp)
            index = d->picture[i];
    ID3D11DeviceContext1* context1 = nullptr;
    if (index == kUnits ||
        FAILED(d->context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))) {
        ++d->failures;
        return;
    }
    const HRESULT h = video_frame_gpu_present(&d->converter, d->device, context1, gpu, d->target_view, 0, 0);
    context1->Release();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(h)) {
        ++d->failures;
        return;
    }
    d->context->CopyResource(d->staging, d->target);
    if (FAILED(d->context->Map(d->staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        ++d->failures;
        return;
    }
    double sum = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) + u64{y} * mapped.RowPitch;
        for (u32 x = 0; x < kWidth; ++x) {
            u8 expected[4];
            source_pixel(index, x, y, expected);
            // Target is RGBA; the source BGRA.
            const double dr = row[x * 4] - expected[2], dg = row[x * 4 + 1] - expected[1],
                         db = row[x * 4 + 2] - expected[0];
            sum += dr * dr + dg * dg + db * db;
        }
    }
    d->context->Unmap(d->staging, 0);
    const double mse = sum / (3.0 * kWidth * kHeight);
    if (mse > d->worst_mse)
        d->worst_mse = mse;
    ++d->measured;
}

void on_keyframe(void*, u32) noexcept {}

bool decode_create(Decode* d) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(d->device->CreateTexture2D(&desc, nullptr, &d->target)) ||
        FAILED(d->device->CreateUnorderedAccessView(d->target, nullptr, &d->target_view)))
        return false;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(d->device->CreateTexture2D(&desc, nullptr, &d->staging));
}

void decode_release(Decode* d) noexcept {
    video_frame_gpu_release(&d->converter);
    com_release(d->staging);
    com_release(d->target_view);
    com_release(d->target);
}

// Every codec encodes and decodes at 40 dB with a keyframe on request. `full` also checks, once (each costs time: a
// still-capture wait and a transform activation), max_fps pacing, the still-capture keyframe and a size change.
int run_codec(ID3D11Device* device, ID3D11DeviceContext* context, GpuStreamCodec codec, bool full) noexcept {
    u32 failures = 0;
    reset_value(&g_collector);
    ID3D11Texture2D* source = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &source)))
        return 1;
    GpuStreamEncoder* encoder = gpu_stream_encoder_create(
        {device, codec, {kWidth, kHeight, kMaxFps, kBitrate}, {&g_collector, on_unit, on_ended}});
    test_check(&failures, encoder != nullptr, "the encoder starts");
    u32 picture_of_unit[kUnits]{};
    u32 expected_units = 0;
    u64 timestamp = 0;
    for (u32 i = 0; encoder && i < kFrames; ++i) {
        fill(i);
        context->UpdateSubresource(source, 0, nullptr, g_pixels, kWidth * 4, 0);
        if (i == kKeyframeAt)
            gpu_stream_encoder_request_keyframe(encoder);
        timestamp = next_timestamp(timestamp);
        test_check(&failures, gpu_stream_encoder_submit(encoder, {source, kWidth, kHeight, timestamp, false}),
                   "every picture is taken");
        picture_of_unit[expected_units++] = i;
        test_check(&failures, wait_units(expected_units), "every picture becomes a unit");
    }
    // Faster than max_fps: refused without a unit.
    if (encoder && full)
        test_check(&failures, !gpu_stream_encoder_submit(encoder, {source, kWidth, kHeight, timestamp + kFrameUs / 2, false}),
                   "a picture faster than max_fps is dropped");
    // A still capture: once a frame interval passed without a picture, a keyframe request encodes the newest picture
    // again at once.
    if (encoder && full) {
        Sleep(110); // the encoder's still-capture threshold is 100 ms
        gpu_stream_encoder_request_keyframe(encoder);
        picture_of_unit[expected_units++] = kFrames - 1;
        test_check(&failures, wait_units(expected_units), "a keyframe request with a still capture is answered");
    }
    // Held (nobody watches): a picture is still taken and converted, but nothing is encoded until the release, which
    // encodes it as a keyframe at once.
    u32 released_unit = kUnits;
    if (encoder && full && unit_count()) {
        gpu_stream_encoder_hold(encoder, true);
        timestamp = next_timestamp(g_collector.units[unit_count() - 1].timestamp_us);
        test_check(&failures, gpu_stream_encoder_submit(encoder, {source, kWidth, kHeight, timestamp, false}),
                   "a held encoder still takes pictures");
        Sleep(30);
        test_check(&failures, unit_count() == expected_units, "a held encoder encodes nothing");
        gpu_stream_encoder_hold(encoder, false);
        released_unit = expected_units;
        picture_of_unit[expected_units++] = kFrames - 1;
        test_check(&failures, wait_units(expected_units), "the release encodes the held picture");
    }
    GpuVideoFrame preview{};
    u32 preview_sequence = 0;
    if (encoder && gpu_stream_encoder_preview(encoder, &preview, &preview_sequence)) {
        test_check(&failures, preview_sequence != 0, "the preview carries a picture sequence");
        test_check(&failures, preview.texture && preview.luma && preview.chroma && preview.width == kWidth &&
                                  preview.height == kHeight && preview.bt709 && !preview.full_range,
                   "the preview is the newest NV12 picture");
        com_release(preview.chroma);
        com_release(preview.luma);
        com_release(preview.texture);
    } else {
        test_check(&failures, false, "the preview exists");
    }
    // Another extent: the stream restarts with a keyframe at it.
    const u32 before_reconfiguration = expected_units;
    // The repeated picture was stamped when it was repeated: later pictures follow it.
    if (unit_count())
        timestamp = g_collector.units[unit_count() - 1].timestamp_us;
    if (encoder && full) {
        gpu_stream_encoder_configure(encoder, {kResizedWidth, kResizedHeight, kMaxFps, kBitrate / 2});
        timestamp = next_timestamp(timestamp);
        test_check(&failures, gpu_stream_encoder_submit(encoder, {source, kWidth, kHeight, timestamp, false}),
                   "a picture after the size change is taken");
        picture_of_unit[expected_units++] = kFrames - 1;
        test_check(&failures, wait_units(expected_units), "a picture after the size change is encoded");
    }
    const GpuStreamEncoderStats stats = gpu_stream_encoder_stats(encoder);
    if (failures) {
        test_write_metric("submitted: ", stats.submitted);
        test_write_metric("dropped: ", stats.dropped);
        test_write_metric("encoded: ", stats.encoded);
        test_write_metric("reconfigurations: ", stats.reconfigurations);
        test_write_metric("failure: ", stats.failure);
    }
    gpu_stream_encoder_destroy(encoder);
    com_release(source);

    const Collector& c = g_collector;
    test_check(&failures, !c.ended && !c.overflow && c.count == expected_units && stats.encoded == c.count &&
                              stats.reconfigurations == (full ? 1u : 0u) && !stats.failure,
               "the unit count and statistics");
    for (u32 i = 0; i < c.count; ++i) {
        const Unit& unit = c.units[i];
        const bool reconfigured = i >= before_reconfiguration;
        const bool must_key = i == 0 || i == kKeyframeAt || (full && i == kFrames) || i == released_unit ||
                              i == before_reconfiguration;
        test_check(&failures, !must_key || unit.keyframe, "keyframes where they were asked for");
        test_check(&failures, unit.width == (reconfigured ? kResizedWidth : kWidth) &&
                                  unit.height == (reconfigured ? kResizedHeight : kHeight),
                   "the units' extent");
        test_check(&failures, i == 0 || unit.timestamp_us > c.units[i - 1].timestamp_us, "units in time order");
        test_check(&failures, unit.rtp_timestamp == static_cast<u32>(unit.timestamp_us * 9 / 100), "90 kHz RTP time");
        const u8* data = g_unit_bytes + unit.offset;
        const bool annexb = unit.bytes > 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
        // AV1: an OBU with obu_has_size_field, never a temporal delimiter (type 2).
        const bool obu = unit.bytes > 2 && (data[0] & 0x02) && ((data[0] >> 3) & 0xF) != 2;
        test_check(&failures, codec == GpuStreamCodec::av1 ? obu : annexb, "the unit's framing");
        if (unit.keyframe && codec != GpuStreamCodec::av1) {
            // The keyframe starts with its parameter sets: H.264 SPS (7), H.265 VPS (32).
            const u8 type = codec == GpuStreamCodec::h264 ? data[4] & 0x1F : (data[4] >> 1) & 0x3F;
            test_check(&failures, type == (codec == GpuStreamCodec::h264 ? 7 : 32), "keyframes lead with parameter sets");
        }
        if (unit.keyframe && codec == GpuStreamCodec::av1)
            test_check(&failures, ((data[0] >> 3) & 0xF) == 1, "AV1 keyframes lead with the sequence header");
    }

    static Decode decode;
    reset_value(&decode);
    decode.device = device;
    decode.context = context;
    decode.units = c.count;
    for (u32 i = 0; i < c.count; ++i) {
        decode.rtp[i] = c.units[i].rtp_timestamp;
        decode.picture[i] = picture_of_unit[i];
    }
    GpuStreamDecoder* decoder =
        decode_create(&decode)
            ? gpu_stream_decoder_create({device, 0xE0, {&decode, on_frame, on_keyframe}, codec, true})
            : nullptr;
    test_check(&failures, decoder != nullptr, "MaiCord's decoder opens the stream");
    for (u32 i = 0; decoder && i < c.count; ++i)
        gpu_stream_decoder_push(decoder, {g_unit_bytes + c.units[i].offset, c.units[i].bytes},
                                c.units[i].rtp_timestamp);
    gpu_stream_decoder_destroy(decoder);
    decode_release(&decode);
    test_check(&failures, decode.frames == c.count && decode.measured == before_reconfiguration && !decode.failures,
               "every unit decodes");
    test_check(&failures, decode.worst_mse <= kMaximumMse, "every picture reaches 40 dB PSNR");
    test_write_text(codec_name(codec));
    test_write_metric(" worst picture PSNR (centi-dB): ", psnr_centi(decode.worst_mse));
    test_write_text(codec_name(codec));
    test_write_metric(" bytes per picture: ", c.count ? c.used / c.count : 0);
    return failures ? 1 : 0;
}

u64 last_unit_time() noexcept {
    AcquireSRWLockShared(&g_collector.lock);
    const u64 time = g_collector.count ? g_collector.units[g_collector.count - 1].timestamp_us : 0;
    ReleaseSRWLockShared(&g_collector.lock);
    return time;
}

// A capture that keeps producing, as the screen capture does for changing content: a changing picture every frame
// interval for 0.8 simulated seconds (24 pictures at 30 fps, in four segments; the clock runs about twice as fast),
// submitted while earlier ones are still in the encoder. Units must keep coming for the whole run (never stop after
// the first picture), and the self-preview must show a new picture each time it is read.
int run_stream(ID3D11Device* device, ID3D11DeviceContext* context, GpuStreamCodec codec) noexcept {
    constexpr u32 kStreamFrames = 24, kSegment = 6;
    constexpr u64 kStreamInterval = 33'333;
    u32 failures = 0;
    reset_value(&g_collector);
    ID3D11Texture2D* source = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &source)))
        return 1;
    GpuStreamEncoder* encoder = gpu_stream_encoder_create(
        {device, codec, {kWidth, kHeight, 30, kBitrate}, {&g_collector, on_unit, on_ended}});
    test_check(&failures, encoder != nullptr, "the stream encoder starts");
    auto* pixels = reinterpret_cast<u32*>(g_pixels);
    const u64 first = next_timestamp(0);
    u64 timestamp = first;
    u32 accepted = 0, checkpoint_units = 0, previous_sequence = 0;
    bool grows = true, previews = true;
    for (u32 i = 0; encoder && i < kStreamFrames; ++i, timestamp += kStreamInterval) {
        const u32 shade = 16 + (i * 7) % 200;
        for (u32 p = 0; p < kWidth * kHeight; ++p)
            pixels[p] = 0xFF000000u | (shade << 16) | ((shade + p % 32) << 8) | (255 - shade);
        context->UpdateSubresource(source, 0, nullptr, g_pixels, kWidth * 4, 0);
        accepted += gpu_stream_encoder_submit(encoder, {source, kWidth, kHeight, timestamp, false});
        Sleep(10);
        if ((i + 1) % kSegment)
            continue;
        // Every segment of the run has produced a unit and a new preview picture.
        grows = wait_units(checkpoint_units + 1) && grows;
        checkpoint_units = unit_count();
        GpuVideoFrame preview{};
        u32 sequence = 0;
        if (gpu_stream_encoder_preview(encoder, &preview, &sequence)) {
            previews = previews && sequence > previous_sequence;
            previous_sequence = sequence;
            com_release(preview.chroma);
            com_release(preview.luma);
            com_release(preview.texture);
        } else {
            previews = false;
        }
    }
    // The last pictures come out too: the stream did not stop part way.
    const u64 last_sent = first + u64{kStreamFrames - 6} * kStreamInterval;
    for (u32 i = 0; i < 1000 && last_unit_time() < last_sent; ++i)
        Sleep(1);
    const u64 last_time = last_unit_time();
    // Drained: every accepted picture is a unit or was replaced while it waited, and the collector has them all (a
    // unit counts as encoded just before its callback runs, so the last one is in flight for a moment).
    GpuStreamEncoderStats stats{};
    for (u32 i = 0; i < 2000; ++i) {
        stats = gpu_stream_encoder_stats(encoder);
        const u32 replaced = stats.dropped - (stats.submitted - accepted);
        if (stats.encoded + replaced == accepted && unit_count() == stats.encoded)
            break;
        Sleep(1);
    }
    if (failures || accepted < kStreamFrames * 3 / 4) {
        test_write_metric("submitted: ", stats.submitted);
        test_write_metric("dropped: ", stats.dropped);
        test_write_metric("encoded: ", stats.encoded);
    }
    gpu_stream_encoder_destroy(encoder);
    com_release(source);
    test_check(&failures, accepted >= kStreamFrames * 3 / 4, "a continuous capture's pictures are taken");
    test_check(&failures, grows, "units keep coming through the whole stream");
    test_check(&failures, previews, "the preview shows a new picture every time it is read");
    test_check(&failures, last_time >= last_sent, "the last pictures of the stream are encoded");
    test_check(&failures, g_collector.count >= accepted / 2 && !g_collector.ended && !g_collector.overflow &&
                              stats.encoded == g_collector.count && !stats.failure,
               "most pictures become units and none is lost to a failure");
    test_write_text(codec_name(codec));
    test_write_metric(" continuous stream units: ", g_collector.count);
    return failures ? 1 : 0;
}

// MAICORD_STREAM_ENCODER_CODEC (h264, h265 or av1) selects one codec, so that the registry runs each as its own case
// side by side; unset, every codec the GPU encodes runs. The pacing, still-capture and size-change checks and the WARP
// check belong to the first codec the GPU encodes, whichever case that is.
u32 wanted_codecs() noexcept {
    wchar_t text[8]{};
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_STREAM_ENCODER_CODEC", text, 8);
    if (!length || length >= 8)
        return 7;
    return text[0] == L'a' ? 4u : text[1] == L'2' && text[3] == L'5' ? 2u : 1u;
}

int run(ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    u32 failures = 0;
    const u32 codecs = gpu_stream_encoder_codecs(device), wanted = wanted_codecs();
    // No hardware, no codec: WARP has no encoder transform.
    if (!codecs || (wanted & codecs & (~codecs + 1))) {
        ID3D11Device* warp = nullptr;
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &warp,
                                        nullptr, nullptr)))
            test_check(&failures, gpu_stream_encoder_codecs(warp) == 0, "WARP encodes nothing");
        com_release(warp);
    }
    test_check(&failures, gpu_stream_encoder_codecs(device) == codecs, "the probe is kept");
    test_write_metric("hardware encoders (bit 0 H.264, 1 H.265, 2 AV1): ", codecs);
    if (!codecs) {
        test_write_line("stream encoder gpu: no hardware video encoder; skipped");
        return static_cast<int>(failures);
    }
    for (u8 codec = 0; codec <= static_cast<u8>(GpuStreamCodec::av1); ++codec)
        if (codecs & wanted & (1u << codec)) {
            const bool full = !(codecs & ((1u << codec) - 1));
            failures += static_cast<u32>(run_codec(device, context, static_cast<GpuStreamCodec>(codec), full));
            failures += static_cast<u32>(run_stream(device, context, static_cast<GpuStreamCodec>(codec)));
        }
    return static_cast<int>(failures);
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_stream_encoder_gpu_tests_entry() noexcept {
    using namespace maicord;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) {
        test_write_line("stream encoder gpu: no hardware D3D11 video device; skipped");
        ExitProcess(0);
    }
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    const int failures = run(device, context);
    context->Release();
    device->Release();
    test_exit(static_cast<u32>(failures), "stream encoder gpu");
}
