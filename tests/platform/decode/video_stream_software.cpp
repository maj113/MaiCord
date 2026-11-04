// The Linux live stream decoder (gpu_stream_decoder.hpp) over the software tier, end to end on the GPU API: access
// units in, the planes it publishes realized and converted by the video_frame compute shader on the test thread
// (the renderer's), read back, and compared with the reference decoder's pictures converted on the CPU with the same
// integer formula.
//
//   conformance   every H.264 and AV1 fixture of tools/generate_video_fixtures.py: every picture converted by the GPU
//                 matches the reference, the display format (crop, matrix) is the stream's
//   threaded      the same through a decode thread: the thread retains and releases frame handles, the test thread
//                 realizes and converts them
//   hold          a held slot is not overwritten while newer pictures decode, and still converts to its picture
//   loss          a damaged unit freezes the picture and asks for a keyframe once; the next keyframe recovers
//   idle          release_idle frees the planes; the next keyframe brings them back
//   unsupported   a stream the software tier refuses (B pictures) shows nothing and asks for nothing
//   capability    the codecs advertised with software decoders only
//
// Status 77: no EGL device, or no libopenh264/libdav1d.
#include "../../fixtures/video/video_fixtures.hpp"
#include "../../support/test_output.hpp"
#include "../../support/yuv_reference.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"
#include "maicord/platform/decode/video_capability.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"
#include "../../../src/platform/decode/software/software_decoder.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

void metric(const char* label, u64 value) noexcept {
    test_write_metric(label, value);
}

constexpr u32 kMaximumWidth = 256, kMaximumHeight = 64;
u8 g_rgba[kMaximumWidth * kMaximumHeight * 4];

struct Capture {
    GpuDevice* device;
    GpuContext* context;
    VideoFrameGpu converter;
    GpuTexture* target;
    GpuUav* uav;
    const VideoFixture* fixture;
    bool convert; // on the callback thread (a synchronous decoder: the test thread)
    u32 frames, requests, mismatches, wrong_format, largest_error;
    u32 next_picture;                // reference index of the next frame
    u32 subresources;                // bit set of the slots frames came from
    bool matrix_bt709;
    GpuVideoFrame latest;            // retained
    bool has_latest;
    u32 latest_subresource;
    volatile LONG sequence;
};

void release_latest(Capture* capture) noexcept {
    if (!capture->has_latest)
        return;
    gpu_release(capture->latest.chroma);
    gpu_release(capture->latest.luma);
    gpu_release(capture->latest.texture);
    capture->has_latest = false;
}

// Converts `frame` on the calling thread and compares it with reference picture `index`.
void convert_and_compare(Capture* capture, const GpuVideoFrame& frame, u32 index) noexcept {
    const VideoFixture& f = *capture->fixture;
    const GpuStatus status =
        video_frame_gpu_present(&capture->converter, capture->device, capture->context, frame, capture->uav, 0, 0);
    if (!gpu_ok(status)) {
        ++capture->mismatches;
        return;
    }
    const u32 width = frame.width, height = frame.height;
    if (!gpu_ok(gpu_readback(capture->device, capture->context, capture->target, {0, 0, width, height}, g_rgba,
                             width * 4))) {
        ++capture->mismatches;
        return;
    }
    const u8* y = f.pictures + usize{index} * f.width * f.height * 3 / 2;
    const u8* cb = y + usize{f.width} * f.height;
    const u8* cr = cb + usize{f.width / 2} * (f.height / 2);
    const YuvRgbCoefficients coefficients = yuv_rgb_coefficients(frame.bt709, frame.full_range);
    u32 largest = 0;
    for (u32 row = 0; row < height; ++row)
        for (u32 column = 0; column < width; ++column) {
            const usize chroma = usize{row / 2} * (f.width / 2) + column / 2;
            u8 expected[4];
            yuv_rgb_pixel(expected, y[usize{row} * f.width + column], cb[chroma] - 128, cr[chroma] - 128, coefficients);
            const u8* got = g_rgba + (usize{row} * width + column) * 4;
            for (u32 c = 0; c < 3; ++c) {
                const u32 difference = got[c] > expected[c] ? got[c] - expected[c] : expected[c] - got[c];
                if (difference > largest)
                    largest = difference;
            }
        }
    if (largest > capture->largest_error)
        capture->largest_error = largest;
    if (largest > 1)
        ++capture->mismatches;
}

void on_frame(void* context, u32, const GpuStreamFrame& frame) noexcept {
    auto* capture = static_cast<Capture*>(context);
    const VideoFixture& f = *capture->fixture;
    const u32 index = capture->next_picture++;
    ++capture->frames;
    capture->subresources |= 1u << frame.gpu->subresource;
    capture->matrix_bt709 = frame.gpu->bt709;
    if (frame.gpu->width != f.width || frame.gpu->height != f.height || frame.display_width != f.width ||
        frame.display_height != f.height)
        ++capture->wrong_format;
    if (capture->convert && index < f.picture_count)
        convert_and_compare(capture, *frame.gpu, index);
    // The app keeps the newest picture with its own references, as video_stream_decoders does.
    release_latest(capture);
    capture->latest = *frame.gpu;
    gpu_retain(capture->latest.texture);
    gpu_retain(capture->latest.luma);
    gpu_retain(capture->latest.chroma);
    capture->has_latest = true;
    capture->latest_subresource = frame.gpu->subresource;
    InterlockedIncrement(&capture->sequence);
}

void on_keyframe(void* context, u32) noexcept {
    ++static_cast<Capture*>(context)->requests;
}

bool setup(Capture* capture, GpuDevice* device, GpuContext* context, const VideoFixture& f) noexcept {
    *capture = {};
    capture->device = device;
    capture->context = context;
    capture->fixture = &f;
    if (!gpu_ok(gpu_texture_create(device, {f.width, f.height, GpuFormat::rgba8, GpuUsage::default_,
                                            static_cast<u8>(gpu_bind_srv | gpu_bind_uav)},
                                   nullptr, &capture->target)))
        return false;
    return gpu_ok(gpu_uav_create(device, capture->target, &capture->uav));
}

void teardown(Capture* capture) noexcept {
    release_latest(capture);
    video_frame_gpu_release(&capture->converter);
    gpu_release(capture->uav);
    gpu_release(capture->target);
}

GpuStreamCodec codec_of(const VideoFixture& f) noexcept {
    return f.codec == 0 ? GpuStreamCodec::h264 : f.codec == 1 ? GpuStreamCodec::h265 : GpuStreamCodec::av1;
}

GpuStreamDecoder* make_decoder(Capture* capture, const VideoFixture& f, bool synchronous) noexcept {
    return gpu_stream_decoder_create(
        {capture->device, 0x7001, {capture, on_frame, on_keyframe}, codec_of(f), synchronous});
}

void push_range(GpuStreamDecoder* decoder, const VideoFixture& f, u32 first, u32 last, u32 base) noexcept {
    for (u32 i = first; i < last; ++i)
        gpu_stream_decoder_push(decoder, {f.packet[i], f.packet_bytes[i]}, base + i * 3000);
}

void conformance(GpuDevice* device, GpuContext* context, const VideoFixture& f) noexcept {
    Capture capture;
    if (!setup(&capture, device, context, f)) {
        check(false, "video stream: a render target of the fixture's size");
        return;
    }
    capture.convert = true;
    GpuStreamDecoder* decoder = make_decoder(&capture, f, true);
    check(decoder != nullptr, "video stream: a decoder for the codec");
    if (decoder) {
        push_range(decoder, f, 0, f.packet_count, 0);
        const GpuStreamDecoderStats stats = gpu_stream_decoder_stats(decoder);
        test_write_text(f.name);
        metric(": pictures ", capture.frames);
        metric("  largest converted error ", capture.largest_error);
        check(capture.frames == f.picture_count, "video stream: every picture is presented");
        check(!capture.mismatches && !capture.wrong_format, "video stream: every picture converts to the reference");
        check(stats.decoded == f.picture_count && stats.presented == f.picture_count && !stats.unsupported &&
                  stats.surfaces == 3 && !capture.requests,
              "video stream: statistics of a clean stream");
        check(f.codec != 0 || capture.matrix_bt709, "video stream: the SPS matrix reaches the frame");
        gpu_stream_decoder_destroy(decoder);
    }
    teardown(&capture);
}

void threaded(GpuDevice* device, GpuContext* context, const VideoFixture& f) noexcept {
    Capture capture;
    if (!setup(&capture, device, context, f))
        return;
    GpuStreamDecoder* decoder = make_decoder(&capture, f, false);
    check(decoder != nullptr, "video stream: a threaded decoder");
    u32 compared = 0;
    for (u32 i = 0; decoder && i < f.packet_count; ++i) {
        gpu_stream_decoder_push(decoder, {f.packet[i], f.packet_bytes[i]}, i * 3000);
        for (u32 wait = 0; wait < 1000 && static_cast<u32>(capture.sequence) <= i; ++wait)
            thread_sleep(2);
        // The newest picture is read on this thread while the decode thread goes on.
        if (static_cast<u32>(capture.sequence) == i + 1 && capture.has_latest) {
            gpu_stream_decoder_hold(decoder, capture.latest_subresource);
            convert_and_compare(&capture, capture.latest, i);
            ++compared;
        }
    }
    check(compared == f.picture_count && !capture.mismatches, "video stream: a decode thread's pictures convert on the test thread");
    gpu_stream_decoder_destroy(decoder);
    teardown(&capture);
}

void hold_rules(GpuDevice* device, GpuContext* context, const VideoFixture& f) noexcept {
    Capture capture;
    if (!setup(&capture, device, context, f))
        return;
    GpuStreamDecoder* decoder = make_decoder(&capture, f, true);
    if (decoder) {
        push_range(decoder, f, 0, 2, 0);
        // Keep picture 1: hold its slot and its own references, then decode everything else.
        GpuVideoFrame kept = capture.latest;
        gpu_retain(kept.texture);
        gpu_retain(kept.luma);
        gpu_retain(kept.chroma);
        const u32 held_slot = capture.latest_subresource;
        gpu_stream_decoder_hold(decoder, held_slot);
        capture.subresources = 0;
        push_range(decoder, f, 2, f.packet_count, 0);
        check(!(capture.subresources & (1u << held_slot)), "video stream: a held slot is not decoded into");
        convert_and_compare(&capture, kept, 1);
        check(!capture.mismatches, "video stream: a held picture still converts to its own pixels");
        gpu_release(kept.chroma);
        gpu_release(kept.luma);
        gpu_release(kept.texture);
        gpu_stream_decoder_destroy(decoder);
    }
    teardown(&capture);
}

void loss_and_idle(GpuDevice* device, GpuContext* context, const VideoFixture& f) noexcept {
    Capture capture;
    if (!setup(&capture, device, context, f))
        return;
    capture.convert = true;
    GpuStreamDecoder* decoder = make_decoder(&capture, f, true);
    if (!decoder) {
        teardown(&capture);
        return;
    }
    push_range(decoder, f, 0, 2, 0);
    check(capture.frames == 2 && !capture.requests, "video stream: pictures before the loss");
    // A unit the RTP assembler lost packets of: nothing is shown, a keyframe is asked for.
    gpu_stream_decoder_push(decoder, {f.packet[2], f.packet_bytes[2]}, 6000, false);
    check(capture.frames == 2 && capture.requests == 1, "video stream: a damaged unit freezes the picture and asks once");
    // Inter units are dropped until a keyframe, with no second request inside the throttle interval.
    push_range(decoder, f, 3, 5, 0);
    check(capture.frames == 2 && capture.requests == 1, "video stream: units without a keyframe are dropped");
    GpuStreamDecoderStats stats = gpu_stream_decoder_stats(decoder);
    check(stats.damaged >= 1 && !stats.unsupported, "video stream: the loss is counted");
    // The keyframe again: the whole stream decodes.
    capture.next_picture = 0;
    push_range(decoder, f, 0, f.packet_count, 100000);
    check(capture.frames == 2 + f.picture_count && !capture.mismatches, "video stream: the next keyframe recovers every picture");
    // An idle stream releases its decoder and planes; the next keyframe brings them back.
    check(gpu_stream_decoder_release_idle(decoder), "video stream: release_idle frees an active stream");
    stats = gpu_stream_decoder_stats(decoder);
    check(!stats.surfaces && stats.idle_releases == 1 && !gpu_stream_decoder_release_idle(decoder),
          "video stream: nothing is left to free afterwards");
    capture.next_picture = 0;
    const u32 before = capture.frames;
    push_range(decoder, f, 0, f.packet_count, 200000);
    check(capture.frames == before + f.picture_count && !capture.mismatches,
          "video stream: a released stream decodes again from its keyframe");
    gpu_stream_decoder_destroy(decoder);
    teardown(&capture);
}

void refused(GpuDevice* device, GpuContext* context, const VideoFixture& f) noexcept {
    Capture capture;
    if (!setup(&capture, device, context, f))
        return;
    GpuStreamDecoder* decoder = make_decoder(&capture, f, true);
    if (decoder) {
        push_range(decoder, f, 0, f.packet_count, 0);
        const GpuStreamDecoderStats stats = gpu_stream_decoder_stats(decoder);
        check(stats.unsupported && stats.presented <= 3 && stats.received == f.packet_count,
              "video stream: a stream with B pictures is unsupported");
        const u32 requests = capture.requests;
        push_range(decoder, f, 0, f.packet_count, 100000);
        check(capture.requests == requests, "video stream: an unsupported stream asks for nothing");
        gpu_stream_decoder_destroy(decoder);
    }
    teardown(&capture);
}

void capability(GpuDevice* device) noexcept {
    const u32 word = gpu_stream_decoder_codecs(device);
    const u32 codecs = gpu_stream_codecs_decodable(word), software = gpu_stream_codecs_software_only(word);
    check((codecs & gpu_stream_codec_bit(GpuStreamCodec::h264)) && (codecs & gpu_stream_codec_bit(GpuStreamCodec::av1)),
          "capability: H.264 and AV1 are advertised with the software libraries");
    check(!(codecs & gpu_stream_codec_bit(GpuStreamCodec::h265)), "capability: HEVC needs hardware");
    check(software == codecs, "capability: with the VA-API off every advertised codec is software only");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_video_stream_software_entry) {
    using namespace maicord;
    if (!software_codec_available(GpuStreamCodec::h264) || !software_codec_available(GpuStreamCodec::av1)) {
        test_write_line("skipped: libopenh264 or libdav1d is not installed");
        test_process_exit(77);
    }
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        test_write_line("skipped: no EGL device");
        test_process_exit(77);
    }
    VideoFixture baseline, weighted, high, bframes, av1_basic, av1_tiles;
    check(video_fixture_parse("h264_baseline", kVideoH264Baseline, sizeof(kVideoH264Baseline), &baseline) &&
              video_fixture_parse("h264_main_weighted", kVideoH264MainWeighted, sizeof(kVideoH264MainWeighted), &weighted) &&
              video_fixture_parse("h264_high_crop_slices", kVideoH264HighCropSlices, sizeof(kVideoH264HighCropSlices),
                                  &high) &&
              video_fixture_parse("h264_main_bframes", kVideoH264MainBFrames, sizeof(kVideoH264MainBFrames), &bframes) &&
              video_fixture_parse("av1_basic", kVideoAv1Basic, sizeof(kVideoAv1Basic), &av1_basic) &&
              video_fixture_parse("av1_tiles", kVideoAv1Tiles, sizeof(kVideoAv1Tiles), &av1_tiles),
          "video fixtures parse");
    const VideoFixture* const all[]{&baseline, &weighted, &high, &av1_basic, &av1_tiles};
    for (const VideoFixture* f : all)
        conformance(device, context, *f);
    threaded(device, context, baseline);
    threaded(device, context, av1_basic);
    hold_rules(device, context, baseline);
    hold_rules(device, context, av1_basic);
    loss_and_idle(device, context, baseline);
    loss_and_idle(device, context, av1_basic);
    refused(device, context, bframes);
    capability(device);
    gpu_device_destroy(&device, &context);
    test_exit(g_failures, "maicord_video_stream_software");
}
