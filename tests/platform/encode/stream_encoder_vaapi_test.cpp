// The Linux Go Live encoder on real VA-API hardware: for every codec the display encodes, a synthetic BGRX picture
// sequence in a linear dma-buf (memfd through /dev/udmabuf, what a capture hands over) goes through the driver's video
// processing and encoder, and MaiCord's own stream decoder and video conversion bring it back: every picture must reach
// at least 40 dB PSNR against the source. Also: keyframes on request, the self-preview picture and a reconfiguration.
//
// Status 77: no EGL device, no VA-API encoder (WSL has no VA-API driver for its GPU), or no /dev/udmabuf.
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/decode/video_capability.hpp"
#include "maicord/platform/encode/gpu_stream_encoder.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord {
namespace {

constexpr u32 kWidth = 1280, kHeight = 720, kFrames = 4, kKeyframeAt = 2, kBitrate = 12'000'000;
constexpr u64 kFrameUs = 16'667;
constexpr u32 kUnits = 32, kUnitBytes = 8u * 1024u * 1024u;
constexpr double kMaximumMse = 65025.0 / 10000.0; // 40 dB
constexpr u32 kDrmXrgb8888 = 0x34325258;
// <linux/udmabuf.h>: UDMABUF_CREATE and its argument.
constexpr unsigned long kUdmabufCreate = 0x40187542;
struct UdmabufCreate {
    u32 memfd, flags;
    u64 offset, size;
};

u32 g_failures;
alignas(64) u8 g_unit_bytes[kUnitBytes];
alignas(64) u8 g_rgba[kWidth * kHeight * 4];

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

struct Unit {
    u32 offset, bytes, rtp;
    u16 width, height;
    bool keyframe;
};
struct Collector {
    SRWLOCK lock;
    Unit units[kUnits];
    u32 count, used;
    bool ended;
} g_collector;

void on_unit(void*, const GpuStreamAccessUnit& unit) noexcept {
    AcquireSRWLockExclusive(&g_collector.lock);
    if (g_collector.count < kUnits && g_collector.used + unit.data.count <= kUnitBytes) {
        copy_bytes(g_unit_bytes + g_collector.used, unit.data.data, static_cast<u32>(unit.data.count));
        g_collector.units[g_collector.count++] = {g_collector.used, static_cast<u32>(unit.data.count),
                                                  unit.rtp_timestamp, unit.width, unit.height, unit.keyframe};
        g_collector.used += static_cast<u32>(unit.data.count);
    }
    ReleaseSRWLockExclusive(&g_collector.lock);
}
void on_ended(void*, GpuStreamEncoderEnd) noexcept {
    g_collector.ended = true;
}
u32 unit_count() noexcept {
    AcquireSRWLockShared(&g_collector.lock);
    const u32 count = g_collector.count;
    ReleaseSRWLockShared(&g_collector.lock);
    return count;
}
bool wait_units(u32 count) noexcept {
    for (u32 i = 0; i < 1000 && unit_count() < count; ++i)
        thread_sleep(1);
    return unit_count() >= count;
}
u64 next_timestamp(u64 last) noexcept {
    const Clock clock = clock_initialize();
    for (;;) {
        const u64 now = clock_microseconds(clock, 0, clock_now());
        if (now >= last + kFrameUs)
            return now;
        thread_sleep(1);
    }
}

u32 triangle(u32 t) noexcept {
    t %= 512;
    return t < 256 ? t : 511 - t;
}
// The source picture `index`, as the Windows test draws it: R, G, B within 24-224.
void source_pixel(u32 index, u32 x, u32 y, u8* rgb) noexcept {
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
    rgb[0] = static_cast<u8>(r + 0.5);
    rgb[1] = static_cast<u8>(g + 0.5);
    rgb[2] = static_cast<u8>(b + 0.5);
}

// A linear XRGB8888 dma-buf over a sealed memfd, mapped for the test to draw into.
struct Buffer {
    int memfd, dmabuf;
    u8* pixels;
};
constexpr usize kBufferBytes = usize{kWidth} * kHeight * 4;

bool buffer_create(Buffer* buffer) noexcept {
    *buffer = {-1, -1, nullptr};
    const long memfd = sys_memfd_create("maicord-encode-test", kMemfdCloseOnExec | kMemfdAllowSealing);
    if (syscall_failed(memfd))
        return false;
    buffer->memfd = static_cast<int>(memfd);
    const long device = sys_open("/dev/udmabuf", kOpenReadWrite);
    UdmabufCreate create{static_cast<u32>(memfd), 1, 0, kBufferBytes};
    long dmabuf = -1;
    if (!syscall_failed(device)) {
        if (!syscall_failed(sys_ftruncate(buffer->memfd, kBufferBytes)) &&
            !syscall_failed(sys_fcntl(buffer->memfd, kFcntlAddSeals, kSealShrink)))
            dmabuf = sys_ioctl(static_cast<int>(device), kUdmabufCreate, &create);
        sys_close(static_cast<int>(device));
    }
    const long mapped = sys_mmap_file(buffer->memfd, kBufferBytes, kProtectRead | kProtectWrite, kMapShared);
    if (syscall_failed(dmabuf) || syscall_failed(mapped))
        return false;
    buffer->dmabuf = static_cast<int>(dmabuf);
    buffer->pixels = reinterpret_cast<u8*>(mapped);
    return true;
}

void buffer_release(Buffer* buffer) noexcept {
    if (buffer->pixels)
        sys_munmap(buffer->pixels, kBufferBytes);
    if (buffer->dmabuf >= 0)
        sys_close(buffer->dmabuf);
    if (buffer->memfd >= 0)
        sys_close(buffer->memfd);
}

void fill(const Buffer& buffer, u32 index) noexcept {
    for (u32 y = 0; y < kHeight; ++y)
        for (u32 x = 0; x < kWidth; ++x) {
            u8 rgb[3];
            source_pixel(index, x, y, rgb);
            u8* p = buffer.pixels + (usize{y} * kWidth + x) * 4;
            p[0] = rgb[2];
            p[1] = rgb[1];
            p[2] = rgb[0];
            p[3] = 255;
        }
}

CaptureFrame frame_of(const Buffer& buffer, u64 timestamp) noexcept {
    CaptureFrame frame{};
    frame.plane = {buffer.dmabuf, kDrmXrgb8888, kWidth, kHeight, 0, kWidth * 4, kDrmFormatModifierLinear, -1, 0, 0};
    frame.width = kWidth;
    frame.height = kHeight;
    frame.timestamp_us = timestamp;
    return frame;
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

struct Decode {
    GpuDevice* device;
    GpuContext* context;
    GpuTexture* target;
    GpuUav* uav;
    VideoFrameGpu converter;
    u32 rtp[kUnits], picture[kUnits], units;
    u32 frames, measured, failures;
    double worst_mse;
};

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
    if (index == kUnits || !gpu_ok(video_frame_gpu_present(&d->converter, d->device, d->context, gpu, d->uav, 0, 0)) ||
        !gpu_ok(gpu_readback(d->device, d->context, d->target, {0, 0, kWidth, kHeight}, g_rgba, kWidth * 4))) {
        ++d->failures;
        return;
    }
    double sum = 0;
    for (u32 y = 0; y < kHeight; ++y)
        for (u32 x = 0; x < kWidth; ++x) {
            u8 expected[3];
            source_pixel(index, x, y, expected);
            const u8* got = g_rgba + (usize{y} * kWidth + x) * 4;
            for (u32 c = 0; c < 3; ++c) {
                const double difference = static_cast<double>(got[c]) - expected[c];
                sum += difference * difference;
            }
        }
    const double mse = sum / (3.0 * kWidth * kHeight);
    if (mse > d->worst_mse)
        d->worst_mse = mse;
    ++d->measured;
}
void on_keyframe(void*, u32) noexcept {}

void run_codec(GpuDevice* device, GpuContext* context, const Buffer& buffer, GpuStreamCodec codec) noexcept {
    reset_value(&g_collector);
    GpuStreamEncoder* encoder =
        gpu_stream_encoder_create({device, codec, {kWidth, kHeight, 60, kBitrate}, {nullptr, on_unit, on_ended}});
    check(encoder != nullptr, "the encoder starts");
    u32 picture_of_unit[kUnits]{};
    u32 expected = 0;
    u64 timestamp = 0;
    for (u32 i = 0; encoder && i < kFrames; ++i) {
        fill(buffer, i);
        if (i == kKeyframeAt)
            gpu_stream_encoder_request_keyframe(encoder);
        timestamp = next_timestamp(timestamp);
        check(gpu_stream_encoder_submit(encoder, frame_of(buffer, timestamp)), "every picture is taken");
        picture_of_unit[expected++] = i;
        check(wait_units(expected), "every picture becomes a unit");
    }
    if (encoder) {
        thread_sleep(150);
        gpu_stream_encoder_request_keyframe(encoder);
        picture_of_unit[expected++] = kFrames - 1;
        check(wait_units(expected), "a keyframe request with a still capture is answered");
        if (unit_count())
            timestamp = clock_microseconds(clock_initialize(), 0, clock_now());
    }
    GpuVideoFrame preview{};
    u32 preview_sequence = 0;
    const bool previewed = encoder && gpu_stream_encoder_preview(encoder, &preview, &preview_sequence);
    check(previewed && preview_sequence != 0 && preview.texture && preview.luma && preview.chroma &&
              preview.width == kWidth,
          "the preview is the newest NV12 picture");
    if (previewed) {
        gpu_release(preview.chroma);
        gpu_release(preview.luma);
        gpu_release(preview.texture);
    }
    const u32 before_reconfiguration = expected;
    if (encoder) {
        gpu_stream_encoder_configure(encoder, {960, 544, 60, kBitrate / 2});
        for (u32 i = 0; i < 2; ++i) {
            timestamp = next_timestamp(timestamp);
            check(gpu_stream_encoder_submit(encoder, frame_of(buffer, timestamp)),
                  "pictures after the reconfiguration are taken");
            picture_of_unit[expected++] = kFrames - 1;
            check(wait_units(expected), "pictures after the reconfiguration are encoded");
        }
    }
    const GpuStreamEncoderStats stats = gpu_stream_encoder_stats(encoder);
    gpu_stream_encoder_destroy(encoder);
    check(!g_collector.ended && g_collector.count == expected && !stats.failure, "the unit count");
    for (u32 i = 0; i < g_collector.count; ++i) {
        const Unit& unit = g_collector.units[i];
        const bool must_key = i == 0 || i == kKeyframeAt || i == kFrames || i == before_reconfiguration;
        check(!must_key || unit.keyframe, "keyframes where they were asked for");
        const u8* data = g_unit_bytes + unit.offset;
        if (codec == GpuStreamCodec::av1) {
            // OBUs with obu_size, no temporal delimiter (type 2); key frames start with the sequence header (type 1).
            check(unit.bytes > 2 && (data[0] & 0x02) && ((data[0] >> 3) & 0xF) != 2, "AV1 OBUs with obu_size");
            check(!unit.keyframe || ((data[0] >> 3) & 0xF) == 1, "AV1 key frames lead with the sequence header");
            continue;
        }
        check(unit.bytes > 5 && !data[0] && !data[1] && !data[2] && data[3] == 1, "Annex B with 4-byte start codes");
        if (unit.keyframe) {
            const u8 type = codec == GpuStreamCodec::h264 ? data[4] & 0x1F : (data[4] >> 1) & 0x3F;
            check(type == (codec == GpuStreamCodec::h264 ? 7 : 32), "keyframes lead with parameter sets");
        }
    }
    // MaiCord's own decoder: a codec it has no decoder for on this machine is checked for framing only.
    if (!(gpu_stream_codecs_decodable(gpu_stream_decoder_codecs(device)) & gpu_stream_codec_bit(codec))) {
        test_write_line("no decoder for this codec here: pictures not compared");
        return;
    }
    static Decode decode;
    reset_value(&decode);
    decode.device = device;
    decode.context = context;
    decode.units = g_collector.count;
    for (u32 i = 0; i < g_collector.count; ++i) {
        decode.rtp[i] = g_collector.units[i].rtp;
        decode.picture[i] = picture_of_unit[i];
    }
    check(gpu_ok(gpu_texture_create(device, {kWidth, kHeight, GpuFormat::rgba8, GpuUsage::default_, gpu_bind_uav},
                                    nullptr, &decode.target)) &&
              gpu_ok(gpu_uav_create(device, decode.target, &decode.uav)),
          "the conversion target");
    GpuStreamDecoder* decoder = gpu_stream_decoder_create({device, 0xE0, {&decode, on_frame, on_keyframe}, codec, true});
    check(decoder != nullptr, "MaiCord's decoder opens the stream");
    for (u32 i = 0; decoder && i < g_collector.count; ++i)
        gpu_stream_decoder_push(decoder, {g_unit_bytes + g_collector.units[i].offset, g_collector.units[i].bytes},
                                g_collector.units[i].rtp);
    gpu_stream_decoder_destroy(decoder);
    video_frame_gpu_release(&decode.converter);
    gpu_release(decode.uav);
    gpu_release(decode.target);
    check(decode.frames == g_collector.count && decode.measured == before_reconfiguration && !decode.failures,
          "every unit decodes");
    check(decode.worst_mse <= kMaximumMse, "every picture reaches 40 dB PSNR");
    const double db = decode.worst_mse > 0 ? 10 * natural_log(65025.0 / decode.worst_mse) / natural_log(10.0) : 99.99;
    test_write_text(codec == GpuStreamCodec::h264 ? "h264" : codec == GpuStreamCodec::h265 ? "h265" : "av1");
    test_write_metric(" worst picture PSNR (centi-dB): ", db > 0 ? static_cast<u64>(db * 100) : 0);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_stream_encoder_vaapi_entry) {
    using namespace maicord;
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        test_write_line("skipped: no EGL device");
        test_process_exit(77);
    }
    const u32 codecs = gpu_stream_encoder_codecs(device);
    Buffer buffer{};
    if (!codecs || !buffer_create(&buffer)) {
        test_write_line(codecs ? "skipped: no /dev/udmabuf to make a test dma-buf with"
                               : "skipped: no VA-API encoder on this machine");
        buffer_release(&buffer);
        gpu_device_destroy(&device, &context);
        test_process_exit(77);
    }
    constexpr GpuStreamCodec kCodecs[]{GpuStreamCodec::av1, GpuStreamCodec::h265, GpuStreamCodec::h264};
    for (const GpuStreamCodec codec : kCodecs)
        if (codecs & gpu_stream_codec_bit(codec))
            run_codec(device, context, buffer, codec);
    buffer_release(&buffer);
    gpu_device_destroy(&device, &context);
    test_exit(g_failures, "maicord_stream_encoder_vaapi");
}
