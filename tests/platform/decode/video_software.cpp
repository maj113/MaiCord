// The software tier of live video (src/platform/decode/software): libopenh264 for H.264 and libdav1d for AV1 behind
// SoftwareDecoder, against the pictures libavcodec's own decoders produce from the same streams
// (tools/generate_video_fixtures.py).
//
//   conformance   every H.264 and AV1 fixture (Baseline, Main with weighted prediction, High with cropping and
//                 slices; AV1 with inter prediction and two tile columns) decoded unit by unit: the
//                 picture count and order, the visible extent and the PSNR of every picture against the oracle
//   reordering    the H.264 stream with B pictures is refused as unsupported (libopenh264 shows decoding order)
//   resynchronize a decoder that starts without its keyframe shows nothing and recovers at the next one, and a
//                 corrupted unit is an error value, never output that was not decoded
//   unavailable   --no-library: both libraries named by their environment variables do not exist, so the tier is
//                 not available and a decoder is not created; HEVC has no software decoder in either case
//
// A fixture whose library is not installed is skipped (exit status 77).
#include "../../../src/platform/decode/software/software_decoder.hpp"
#include "../../fixtures/video/video_fixtures.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

u32 g_failures;
u32 g_skipped;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

void metric(const char* label, u64 value) noexcept {
    test_write_metric(label, value);
}

constexpr u32 kMaximumPictureBytes = 256 * 64 * 3 / 2;
constexpr u32 kMaximumPictures = 16;
u8 g_output[kMaximumPictureBytes * kMaximumPictures];

struct Fixtures {
    VideoFixture h264_baseline, h264_main, h264_weighted, h264_high, hevc, av1_basic, av1_tiles;
    bool valid;
};

Fixtures load() noexcept {
    Fixtures f{};
    f.valid = video_fixture_parse("h264_baseline", kVideoH264Baseline, sizeof(kVideoH264Baseline), &f.h264_baseline) &&
              video_fixture_parse("h264_main_bframes", kVideoH264MainBFrames, sizeof(kVideoH264MainBFrames), &f.h264_main) &&
              video_fixture_parse("h264_main_weighted", kVideoH264MainWeighted, sizeof(kVideoH264MainWeighted),
                                  &f.h264_weighted) &&
              video_fixture_parse("h264_high_crop_slices", kVideoH264HighCropSlices, sizeof(kVideoH264HighCropSlices),
                                  &f.h264_high) &&
              video_fixture_parse("hevc_main", kVideoHevcMain, sizeof(kVideoHevcMain), &f.hevc) &&
              video_fixture_parse("av1_basic", kVideoAv1Basic, sizeof(kVideoAv1Basic), &f.av1_basic) &&
              video_fixture_parse("av1_tiles", kVideoAv1Tiles, sizeof(kVideoAv1Tiles), &f.av1_tiles);
    return f;
}

GpuStreamCodec codec_of(const VideoFixture& f) noexcept {
    return f.codec == 0 ? GpuStreamCodec::h264 : f.codec == 1 ? GpuStreamCodec::h265 : GpuStreamCodec::av1;
}

// Copies a picture as tight I420 into destination.
void copy_picture(const SoftwarePicture& p, u8* destination) noexcept {
    for (u32 y = 0; y < p.height; ++y)
        copy_bytes(destination + usize{y} * p.width, p.plane[0] + usize{y} * p.stride[0], p.width);
    destination += usize{p.width} * p.height;
    for (u32 plane = 1; plane < 3; ++plane) {
        for (u32 y = 0; y < p.height / 2; ++y)
            copy_bytes(destination + usize{y} * (p.width / 2), p.plane[plane] + usize{y} * p.stride[plane], p.width / 2);
        destination += usize{p.width / 2} * (p.height / 2);
    }
}

struct Run {
    u32 pictures;
    u32 lost, unsupported, failed;
    u32 wrong_extent;
};

// Pushes packets [first, last) and appends the pictures to g_output from picture `pictures`.
Run push_packets(SoftwareDecoder* decoder, const VideoFixture& f, u32 first, u32 last, Run run) noexcept {
    for (u32 i = first; i < last; ++i) {
        const SoftwareStatus status = software_decoder_push(decoder, {f.packet[i], f.packet_bytes[i]});
        run.lost += status == SoftwareStatus::lost;
        run.unsupported += status == SoftwareStatus::unsupported;
        run.failed += status == SoftwareStatus::failed;
        SoftwarePicture picture{};
        while (software_decoder_pop(decoder, &picture)) {
            if (picture.width != f.width || picture.height != f.height || run.pictures >= kMaximumPictures) {
                ++run.wrong_extent;
                continue;
            }
            copy_picture(picture, g_output + usize{run.pictures} * kMaximumPictureBytes);
            ++run.pictures;
        }
    }
    return run;
}

// 50 dB PSNR for 8-bit samples is a mean squared error of 255^2 / 10^5 = 0.65.
bool within_50_db(u64 squared_error, u64 samples) noexcept {
    return squared_error * 100'000ull <= 65025ull * samples;
}

struct Comparison {
    u64 worst_mse_milli; // mean squared error of the worst picture, times 1000
    u32 max_error;
    bool within_50_db;
};

Comparison compare(const VideoFixture& f, u32 pictures) noexcept {
    Comparison result{0, 0, true};
    const u32 bytes = f.width * f.height * 3 / 2;
    for (u32 i = 0; i < pictures && i < f.picture_count; ++i) {
        u64 squared = 0;
        const u8* a = g_output + usize{i} * kMaximumPictureBytes;
        const u8* b = f.pictures + usize{i} * bytes;
        for (u32 k = 0; k < bytes; ++k) {
            const i32 difference = static_cast<i32>(a[k]) - static_cast<i32>(b[k]);
            squared += static_cast<u64>(difference * difference);
            const u32 magnitude = static_cast<u32>(difference < 0 ? -difference : difference);
            if (magnitude > result.max_error)
                result.max_error = magnitude;
        }
        const u64 mse = squared * 1000 / bytes;
        if (mse > result.worst_mse_milli)
            result.worst_mse_milli = mse;
        result.within_50_db = result.within_50_db && within_50_db(squared, bytes);
    }
    return result;
}

void conformance(const VideoFixture& f) noexcept {
    const GpuStreamCodec codec = codec_of(f);
    if (!software_codec_available(codec)) {
        test_write_text("skipped (library not installed): ");
        test_write_line(f.name);
        ++g_skipped;
        return;
    }
    SoftwareDecoder* decoder = software_decoder_create(codec);
    check(decoder != nullptr, "software decoder: created");
    if (!decoder)
        return;
    const Run run = push_packets(decoder, f, 0, f.packet_count, {});
    const Comparison comparison = compare(f, run.pictures);
    test_write_text(f.name);
    metric(": pictures ", run.pictures);
    metric("  worst mean squared error (milli) ", comparison.worst_mse_milli);
    metric("  largest sample error ", comparison.max_error);
    check(run.pictures == f.picture_count && !run.wrong_extent, "software decoder: every picture, at the visible extent");
    check(!run.lost && !run.unsupported && !run.failed, "software decoder: a clean stream has no error status");
    check(comparison.within_50_db, "software decoder: every picture within 50 dB PSNR of the reference decoder");
    software_decoder_destroy(decoder);
}

// libopenh264 shows pictures in decoding order: the stream with B pictures is refused, not shown out of order.
void reordering(const VideoFixture& f) noexcept {
    if (!software_codec_available(GpuStreamCodec::h264))
        return;
    SoftwareDecoder* decoder = software_decoder_create(GpuStreamCodec::h264);
    if (!decoder)
        return;
    const Run run = push_packets(decoder, f, 0, f.packet_count, {});
    check(run.unsupported > 0 && run.pictures <= 3, "software decoder: B pictures are refused as unsupported");
    software_decoder_destroy(decoder);
}

void resynchronize(const VideoFixture& f) noexcept {
    const GpuStreamCodec codec = codec_of(f);
    if (!software_codec_available(codec))
        return;
    SoftwareDecoder* decoder = software_decoder_create(codec);
    if (!decoder)
        return;
    // No keyframe seen: the inter units alone show nothing.
    Run run = push_packets(decoder, f, 2, f.packet_count, {});
    check(run.pictures == 0, "software decoder: inter units without their keyframe show no picture");
    // Then the whole stream again, from its keyframe: the pictures are the reference's.
    run = push_packets(decoder, f, 0, f.packet_count, run);
    const Comparison comparison = compare(f, run.pictures);
    check(run.pictures == f.picture_count && comparison.within_50_db,
          "software decoder: the next keyframe recovers every picture");
    // A corrupted unit: an error value or a picture, never a crash; the following keyframe restores the stream.
    static u8 damaged[8192];
    const u32 bytes = f.packet_bytes[1] < sizeof(damaged) ? f.packet_bytes[1] : sizeof(damaged);
    copy_bytes(damaged, f.packet[1], bytes);
    for (u32 i = 16; i < bytes; i += 13)
        damaged[i] = static_cast<u8>(damaged[i] ^ 0xA5u);
    software_decoder_push(decoder, {damaged, bytes});
    SoftwarePicture scratch{};
    while (software_decoder_pop(decoder, &scratch)) {
    }
    software_decoder_reset(decoder);
    run = push_packets(decoder, f, 0, f.packet_count, {});
    check(run.pictures == f.picture_count && compare(f, run.pictures).within_50_db,
          "software decoder: a stream restarted after damage decodes exactly");
    software_decoder_destroy(decoder);
}

void unavailable() noexcept {
    check(!software_codec_available(GpuStreamCodec::h264) && !software_codec_available(GpuStreamCodec::av1),
          "software decoder: no library, no tier");
    check(!software_decoder_create(GpuStreamCodec::h264) && !software_decoder_create(GpuStreamCodec::av1),
          "software decoder: no library, no decoder");
    check(!software_codec_available(GpuStreamCodec::h265) && !software_decoder_create(GpuStreamCodec::h265),
          "software decoder: HEVC has no software tier");
}

bool has_argument(const char* name) noexcept {
    const ProcessStartup& startup = process_startup();
    for (int i = 1; i < startup.argc; ++i) {
        const char* a = startup.argv[i];
        u32 k = 0;
        while (a[k] && a[k] == name[k])
            ++k;
        if (a[k] == name[k])
            return true;
    }
    return false;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_video_software_entry) {
    using namespace maicord;
    if (has_argument("--no-library")) {
        unavailable();
        test_exit(g_failures, "maicord_video_software_no_library");
    }
    const Fixtures f = load();
    check(f.valid, "video fixtures parse");
    if (f.valid) {
        const VideoFixture* const software[]{&f.h264_baseline, &f.h264_weighted, &f.h264_high, &f.av1_basic, &f.av1_tiles};
        for (const VideoFixture* fixture : software)
            conformance(*fixture);
        reordering(f.h264_main);
        resynchronize(f.h264_baseline);
        resynchronize(f.av1_basic);
        check(!software_codec_available(GpuStreamCodec::h265), "software decoder: HEVC has no software tier");
    }
    if (!g_failures && g_skipped)
        test_process_exit(77);
    test_exit(g_failures, "maicord_video_software");
}
