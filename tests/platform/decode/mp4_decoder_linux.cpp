// The Linux movie decoder (platform/decode/mp4_decoder.hpp: mp4_decoder.cpp over h264_file_decoder.cpp) on the GPU API,
// with the software tier forced (MAICORD_VAAPI=off), against what libavformat and libavcodec decode from the same
// movies (tools/generate_mp4_fixtures.py): the in-house demuxer, the reference tracker and output queue, libopenh264 and
// the GLES conversion together.
//
//   pictures   every picture of each movie (B pictures with a delayed output, a pyramid, a cropped extent) in
//              presentation order with the container's presentation time and duration, converted on the GPU and read
//              back: the largest difference from the reference's picture converted on the CPU is 0 (1 allowed)
//   loop       a read past the last picture starts the movie again
//   seek       a seek lands on the IDR at or before the target, and reading from it reaches exactly the target picture
//   selected   one outstanding picture at a time: a second selected read and a seek wait for the acknowledgement,
//              and a needs_service read resumes
//   fragments  the fragmented movie through mp4_decoder_initialize_fragmented over an in-memory source
//   audio      the movie's AAC track through the demuxer, the edit-list window and the decoder equals the reference
//   refusals   garbage, an HEVC movie's brand and an oversized request fail cleanly
//
// Status 77: no EGL device or no libopenh264.
#include "../../fixtures/mp4/mp4_fixtures.hpp"
#include "../../support/test_output.hpp"
#include "../../support/yuv_reference.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/media/containers/mp4_audio_demux.hpp"
#include "maicord/media/containers/mp4_fragment_source.hpp"
#include "maicord/media/core/progressive_source.hpp"
#include "maicord/platform/decode/aac_decoder.hpp"
#include "maicord/platform/decode/mp4_decoder.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"
#include "../../../src/platform/decode/software/software_decoder.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr u32 kMaximumWidth = 64, kMaximumHeight = 48;
u8 g_rgba[kMaximumWidth * kMaximumHeight * 4];
float g_pcm[kAacDecoderMaximumOutputBytes / sizeof(float)];

struct Player {
    GpuDevice* device;
    GpuContext* context;
    const Mp4Reference* reference;
    VideoFrameGpu converter;
    GpuTexture* target;
    GpuUav* uav;
    u32 largest_error, pictures, timing_mismatches;
    bool exact;           // the decoder reproduces the reference bit for bit; else within kCloseMseMilli
    u64 worst_mse_milli;  // the worst mean squared error of a converted picture, times 1000
    u32 exact_pictures;
};

// libopenh264 decodes the CABAC B pictures of some streams a little off the standard's (a few blocks, 39 to 47 dB
// against libavcodec's picture on these fixtures; B pictures do not carry the error on): such movies are held to a
// mean squared error of 50 over the converted RGB pictures (about 31 dB), the others to the reference.
constexpr u64 kCloseMseMilli = 50'000;

bool player_open(Player* p, GpuDevice* device, GpuContext* context, const Mp4Reference& reference, bool exact) noexcept {
    *p = {};
    p->exact = exact;
    p->device = device;
    p->context = context;
    p->reference = &reference;
    if (!gpu_ok(gpu_texture_create(device, {reference.width, reference.height, GpuFormat::rgba8, GpuUsage::default_,
                                            static_cast<u8>(gpu_bind_srv | gpu_bind_uav)},
                                   nullptr, &p->target)))
        return false;
    return gpu_ok(gpu_uav_create(device, p->target, &p->uav));
}

void player_close(Player* p) noexcept {
    video_frame_gpu_release(&p->converter);
    gpu_release(p->uav);
    gpu_release(p->target);
}

// Converts the decoded picture on the GPU and compares it with reference picture `index` converted on the CPU.
bool picture_matches(Player* p, const Mp4DecodedFrame& frame, u32 index) noexcept {
    const Mp4Reference& r = *p->reference;
    if (!frame.gpu || frame.width != r.width || frame.height != r.height || index >= r.frames)
        return false;
    if (!gpu_ok(video_frame_gpu_present(&p->converter, p->device, p->context, *frame.gpu, p->uav, 0, 0)) ||
        !gpu_ok(gpu_readback(p->device, p->context, p->target, {0, 0, r.width, r.height}, g_rgba, r.width * 4)))
        return false;
    const u8* y = mp4_reference_picture(r, index);
    const u8* cb = y + usize{r.width} * r.height;
    const u8* cr = cb + usize{r.width / 2} * (r.height / 2);
    const YuvRgbCoefficients coefficients = yuv_rgb_coefficients(frame.gpu->bt709, frame.gpu->full_range);
    u32 largest = 0;
    u64 squared = 0;
    for (u32 row = 0; row < r.height; ++row)
        for (u32 column = 0; column < r.width; ++column) {
            const usize chroma = usize{row / 2} * (r.width / 2) + column / 2;
            u8 expected[4];
            yuv_rgb_pixel(expected, y[usize{row} * r.width + column], cb[chroma] - 128, cr[chroma] - 128, coefficients);
            const u8* got = g_rgba + (usize{row} * r.width + column) * 4;
            for (u32 c = 0; c < 3; ++c) {
                const u32 difference = got[c] > expected[c] ? got[c] - expected[c] : expected[c] - got[c];
                largest = difference > largest ? difference : largest;
                squared += u64{difference} * difference;
            }
        }
    if (largest > p->largest_error)
        p->largest_error = largest;
    const u64 mse_milli = squared * 1000 / (u64{r.width} * r.height * 3);
    p->worst_mse_milli = mse_milli > p->worst_mse_milli ? mse_milli : p->worst_mse_milli;
    p->exact_pictures += largest <= 1;
    return p->exact ? largest <= 1 : mse_milli <= kCloseMseMilli;
}

bool near(i64 a, i64 b) noexcept {
    return (a > b ? a - b : b - a) <= 1;
}

// ---- Pictures, loop, seek ----------------------------------------------------------------------------------------

void pictures(GpuDevice* device, GpuContext* context, const char* name, const u8* movie, usize movie_bytes,
              const Mp4Reference& r, bool exact) noexcept {
    Player p;
    if (!player_open(&p, device, context, r, exact)) {
        check(false, "pictures: the conversion target");
        return;
    }
    Mp4Decoder decoder{};
    const bool opened = mp4_decoder_initialize(&decoder, {movie, static_cast<u32>(movie_bytes)}, 0, 0, device);
    if (!opened) {
        test_write_metric("status ", static_cast<u32>(decoder.native.status));
        test_write_metric("parse error ", static_cast<u32>(decoder.native.parse_error));
        test_write_metric("demux error ", static_cast<u32>(decoder.native.demux_error));
    }
    check(opened && decoder.ready, name);
    const H264GpuMovie info = mp4_decoder_movie(decoder);
    check(info.duration_us && info.first_presentation_valid && info.samples >= r.frames, "pictures: movie metadata");
    u32 matched = 0, timed = 0, durations = 0, count = 0;
    Mp4DecodedFrame frame{};
    for (; count <= r.frames + 1; ++count) {
        frame = mp4_decoder_read(&decoder, false);
        if (!frame.ok())
            break;
        if (count < r.frames) {
            const bool same = picture_matches(&p, frame, count);
            matched += same;
            if (!same)
                test_write_metric("picture differs: ", count);
            timed += near(frame.presentation_time_us, mp4_reference_time(r, count));
            durations += frame.duration_us == mp4_reference_duration(r, count) ||
                         frame.duration_us <= mp4_reference_duration(r, count); // the last may be clipped
            if (!near(frame.presentation_time_us, mp4_reference_time(r, count))) {
                test_write_metric("pts expected ", static_cast<u64>(mp4_reference_time(r, count)));
                test_write_metric("pts got ", static_cast<u64>(frame.presentation_time_us));
            }
        }
    }
    test_write_metric("pictures read ", count);
    test_write_metric("pictures matched ", matched);
    test_write_metric("pictures timed ", timed);
    test_write_metric("final error ", static_cast<u32>(frame.error));
    check(count == r.frames && frame.error == Mp4DecodeError::end_of_stream, "pictures: as many pictures as the reference, then the end");
    check(matched == r.frames, p.exact ? "pictures: every picture equals the reference"
                                       : "pictures: every picture is close to the reference");
    test_write_metric("pictures largest error ", p.largest_error);
    test_write_metric("pictures worst mse (milli) ", p.worst_mse_milli);
    test_write_metric("pictures exact ", p.exact_pictures);
    check(timed == r.frames, "pictures: presentation times are the container's");
    check(durations == r.frames, "pictures: durations are the container's");
    // Loop: a read past the end starts again from the first picture.
    frame = mp4_decoder_read(&decoder, true);
    check(frame.ok() && near(frame.presentation_time_us, mp4_reference_time(r, 0)) && picture_matches(&p, frame, 0),
          "loop: the movie starts again");
    frame = mp4_decoder_read(&decoder, true);
    check(frame.ok() && near(frame.presentation_time_us, mp4_reference_time(r, 1)) && picture_matches(&p, frame, 1),
          "loop: and goes on in order");

    // Seek to a picture in the second GOP, the first, and the last: the decoder lands on the IDR at or before and the
    // pictures read from there reach the target one.
    const u32 targets[]{r.frames / 2 + 1, 0, r.frames - 1};
    for (u32 target_index : targets) {
        const i64 target = mp4_reference_time(r, target_index);
        Mp4SeekPoint point{};
        mp4_decoder_acknowledge_frame(&decoder); // seeking waits for the renderer's copy
        const Mp4DemuxError sought = mp4_decoder_seek(&decoder, target, &point);
        check(sought == Mp4DemuxError::none && point.presentation_time_us <= target, "seek: a point at or before the target");
        bool reached = false, exact = false;
        for (u32 guard = 0; guard < r.frames + 2 && !reached; ++guard) {
            frame = mp4_decoder_read(&decoder, false);
            if (!frame.ok())
                break;
            const auto interval = mp4_frame_interval_from(frame.presentation_time_us, frame.source_duration_us, target,
                                                          info.duration_us, info.edited);
            if (interval.visibility == Mp4FrameVisibility::before)
                continue;
            reached = interval.visibility == Mp4FrameVisibility::visible;
            exact = reached && near(frame.presentation_time_us, target) && picture_matches(&p, frame, target_index);
        }
        check(reached && exact, "seek: reading from the point reaches the target picture");
    }

    // Selected playback: one picture out until it is acknowledged.
    mp4_decoder_acknowledge_frame(&decoder);
    check(mp4_decoder_seek_start(&decoder), "selected: seek to the start");
    Mp4DecodedFrame selected{};
    for (u32 attempts = 0; attempts < 16; ++attempts) {
        selected = mp4_decoder_read_selected(&decoder);
        if (selected.error != Mp4DecodeError::needs_service)
            break;
    }
    check(selected.ok() && near(selected.presentation_time_us, mp4_reference_time(r, 0)) && picture_matches(&p, selected, 0),
          "selected: the first picture after a bounded number of service rounds");
    check(mp4_decoder_read_selected(&decoder).error == Mp4DecodeError::invalid_input,
          "selected: no second picture before the acknowledgement");
    Mp4SeekPoint point{};
    check(mp4_decoder_seek(&decoder, 0, &point) == Mp4DemuxError::invalid_input,
          "selected: no seek before the acknowledgement");
    check(mp4_decoder_acknowledge_frame(&decoder) && mp4_decoder_acknowledge_frame(&decoder),
          "selected: the acknowledgement is idempotent");
    Mp4DecodedFrame second{};
    for (u32 attempts = 0; attempts < 16; ++attempts) {
        second = mp4_decoder_read_selected(&decoder);
        if (second.error != Mp4DecodeError::needs_service)
            break;
    }
    check(second.ok() && near(second.presentation_time_us, mp4_reference_time(r, 1)) && picture_matches(&p, second, 1),
          "selected: the next picture after it");
    mp4_decoder_acknowledge_frame(&decoder);
    mp4_decoder_release(&decoder);
    check(!decoder.ready && !decoder.native.state && mp4_decoder_read(&decoder).error == Mp4DecodeError::invalid_input,
          "release: the decoder is empty again");
    player_close(&p);
}

// ---- Audio -------------------------------------------------------------------------------------------------------

Mp4AudioDemuxer g_audio;

void audio(const char* name, const u8* movie, usize movie_bytes, const Mp4Reference& r) noexcept {
    check(mp4_audio_demux_initialize(&g_audio, {movie, static_cast<u32>(movie_bytes)}) == Mp4DemuxError::none &&
              g_audio.sample_rate == r.rate && g_audio.channels == r.channels,
          name);
    Mp4AudioSampleWindow window{};
    check(mp4_audio_demux_sample_window(g_audio, 0, &window) && window.end_source_frame > window.first_source_frame,
          "audio: the edit-list window");
    AacDecoder decoder{};
    bool ok = aac_decoder_initialize(&decoder, mp4_audio_demux_asc(g_audio), g_audio.sample_rate, g_audio.channels).ok();
    u64 frame_index = 0;
    u32 compared = 0, largest = 0;
    for (;;) {
        const auto unit = mp4_audio_demux_next(&g_audio);
        if (!unit.ok())
            break;
        ok = ok && aac_decoder_submit(&decoder, unit.bytes, unit.presentation_time_us_signed * 10, 1).ok();
        const auto block = ok ? aac_decoder_read(&decoder, {reinterpret_cast<u8*>(g_pcm), sizeof(g_pcm)}) : AacDecodedBlock{};
        ok = ok && block.ok() && block.frames == 1024;
        for (u32 s = 0; ok && s < block.frames; ++s) {
            const u64 position = frame_index + s;
            if (position < window.first_source_frame || position >= window.end_source_frame)
                continue;
            const u64 at = position - window.first_source_frame;
            if (at >= r.samples)
                continue;
            for (u32 c = 0; c < r.channels; ++c) {
                const float v = g_pcm[s * r.channels + c] * 32768.0f;
                const i32 got = v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : static_cast<i32>(v >= 0 ? v + 0.5f : v - 0.5f);
                const u8* ref = r.pcm + (usize{static_cast<u32>(at)} * r.channels + c) * 2;
                const i32 expected = static_cast<i16>(ref[0] | (ref[1] << 8));
                const u32 difference = static_cast<u32>(got > expected ? got - expected : expected - got);
                largest = difference > largest ? difference : largest;
            }
            ++compared;
        }
        frame_index += block.frames;
    }
    aac_decoder_release(&decoder);
    test_write_metric("audio ok ", ok);
    test_write_metric("audio frames decoded ", frame_index);
    test_write_metric("audio window first ", window.first_source_frame);
    test_write_metric("audio window end ", window.end_source_frame);
    test_write_metric("audio reference samples ", r.samples);
    test_write_metric("audio samples compared ", compared);
    test_write_metric("audio largest difference ", largest);
    check(ok && compared >= r.samples - 2048 && compared <= r.samples + 2048,
          "audio: the track decodes to about the reference's length");
    check(largest <= 3, "audio: the decoded PCM equals the reference within rounding");
}


// ---- Fragments ---------------------------------------------------------------------------------------------------

ProgressiveSource g_source;
Mp4FragmentSource g_stream;

// The fragmented movie as one complete sparse entity, through the fragment cursor the progressive players use.
void fragmented(GpuDevice* device, GpuContext* context, const Mp4Reference& r) noexcept {
    Player p;
    if (!player_open(&p, device, context, r, false)) {
        check(false, "fragments: the conversion target");
        return;
    }
    check(progressive_source_initialize(&g_source, sizeof(kMp4H264Fragmented)) &&
              progressive_source_write(&g_source, 0, {kMp4H264Fragmented, sizeof(kMp4H264Fragmented)}) &&
              mp4_fragment_source_initialize(&g_source, false, &g_stream) == Mp4DemuxError::none,
          "fragments: the cursor opens on the movie");
    Mp4Decoder decoder{};
    const bool opened = mp4_decoder_initialize_fragmented(&decoder, &g_stream, device);
    if (!opened) {
        test_write_metric("fragments status ", static_cast<u32>(decoder.native.status));
        test_write_metric("fragments parse error ", static_cast<u32>(decoder.native.parse_error));
        test_write_metric("fragments demux error ", static_cast<u32>(decoder.native.demux_error));
    }
    check(opened && decoder.ready && decoder.fragmented, "fragments: the decoder opens on the cursor");
    u32 matched = 0, timed = 0, count = 0;
    Mp4DecodedFrame frame{};
    for (u32 rounds = 0; rounds < r.frames * 4 && count < r.frames + 1; ++rounds) {
        frame = mp4_decoder_read(&decoder, false);
        if (frame.error == Mp4DecodeError::needs_service)
            continue;
        if (!frame.ok())
            break;
        if (count < r.frames) {
            matched += picture_matches(&p, frame, count);
            timed += near(frame.presentation_time_us, mp4_reference_time(r, count));
        }
        ++count;
    }
    test_write_metric("fragments pictures ", count);
    check(count == r.frames && matched == r.frames && timed == r.frames,
          "fragments: every picture in order, with the container's times, close to the reference");
    // Seek to the second GOP: the cursor lands on its IDR and reading reaches the target picture.
    const u32 target_index = r.frames / 2 + 1;
    const i64 target = mp4_reference_time(r, target_index);
    Mp4SeekPoint point{};
    mp4_decoder_acknowledge_frame(&decoder);
    check(mp4_decoder_seek(&decoder, target, &point) == Mp4DemuxError::none && point.presentation_time_us <= target,
          "fragments: a seek lands at or before the target");
    bool reached = false, exact = false;
    const H264GpuMovie info = mp4_decoder_movie(decoder);
    for (u32 rounds = 0; rounds < r.frames * 4 && !reached; ++rounds) {
        frame = mp4_decoder_read(&decoder, false);
        if (frame.error == Mp4DecodeError::needs_service)
            continue;
        if (!frame.ok())
            break;
        const auto interval = mp4_frame_interval_from(frame.presentation_time_us, frame.source_duration_us, target,
                                                      info.duration_us, info.edited);
        if (interval.visibility == Mp4FrameVisibility::before)
            continue;
        reached = interval.visibility == Mp4FrameVisibility::visible;
        exact = reached && near(frame.presentation_time_us, target) && picture_matches(&p, frame, target_index);
    }
    check(reached && exact, "fragments: reading from the seek point reaches the target picture");
    mp4_decoder_release(&decoder);
    mp4_fragment_source_release(&g_stream);
    progressive_source_release(&g_source);
    player_close(&p);
}

// ---- Refusals ----------------------------------------------------------------------------------------------------

// An HEVC movie has no software tier: with VA-API off, the container, hvcC and first picture parse (the movie is
// recognized as HEVC and understood), and the decoder refuses it for want of a decoder, with the status the
// application reports as an unsupported backend and no parse or demux error.
void hevc_without_driver(GpuDevice* device) noexcept {
    Mp4Decoder decoder{};
    check(!mp4_decoder_initialize(&decoder, {kMp4HevcMainAac, sizeof(kMp4HevcMainAac)}, 0, 0, device) && !decoder.ready &&
              decoder.hevc && decoder.native.status == DXGI_ERROR_UNSUPPORTED &&
              decoder.native.parse_error == H264Error::none && decoder.native.demux_error == Mp4DemuxError::none,
          "hevc: without a driver the movie is understood and refused as an unsupported backend");
    check(mp4_decoder_read(&decoder, true).error == Mp4DecodeError::invalid_input,
          "hevc: the refused decoder answers reads with a refusal");
    mp4_decoder_release(&decoder);
    check(!decoder.hevc && decoder.native.status == 0, "hevc: release empties the decoder");
}

void refusals(GpuDevice* device) noexcept {
    Mp4Decoder decoder{};
    static const u8 garbage[64]{1, 2, 3};
    check(!mp4_decoder_initialize(&decoder, {garbage, sizeof(garbage)}, 0, 0, device) && !decoder.ready &&
              decoder.native.parse_error != H264Error::none,
          "refusals: bytes that are no movie are refused with a parse error");
    check(!mp4_decoder_initialize(&decoder, {kMp4H264Fragmented, sizeof(kMp4H264Fragmented)}, 0, 0, device) && !decoder.ready,
          "refusals: a fragmented movie is not a plain one");
    check(!mp4_decoder_initialize(&decoder, {kMp4H264AacBFrames, sizeof(kMp4H264AacBFrames)}, 1, 0, device) && !decoder.ready,
          "refusals: half a requested extent is invalid");
    check(!mp4_decoder_initialize(&decoder, {kMp4H264AacBFrames, sizeof(kMp4H264AacBFrames)}, 0, 0, nullptr) && !decoder.ready,
          "refusals: no device, no decoder");
    check(mp4_decoder_read(&decoder, true).error == Mp4DecodeError::invalid_input &&
              mp4_decoder_read_selected(&decoder).error == Mp4DecodeError::invalid_input &&
              !mp4_decoder_acknowledge_frame(&decoder) && !mp4_decoder_seek_start(&decoder),
          "refusals: an empty decoder answers every call with a refusal");
    // The same movie opens after the failures, and opens again over itself (the old decoder is released first).
    check(mp4_decoder_initialize(&decoder, {kMp4H264AacBFrames, sizeof(kMp4H264AacBFrames)}, 0, 0, device) &&
              mp4_decoder_initialize(&decoder, {kMp4H264BaselineGops, sizeof(kMp4H264BaselineGops)}, 0, 0, device) &&
              mp4_decoder_movie(decoder).samples == 20,
          "refusals: a decoder is reused for another movie");
    mp4_decoder_release(&decoder);
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_mp4_decoder_linux_entry) {
    using namespace maicord;
    if (!software_codec_available(GpuStreamCodec::h264)) {
        test_write_line("skipped: libopenh264 is not installed");
        test_process_exit(77);
    }
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        test_write_line("skipped: no EGL device");
        test_process_exit(77);
    }
    Mp4Reference bframes, baseline, pyramid;
    check(mp4_reference_parse(kMp4H264AacBFramesRef, sizeof(kMp4H264AacBFramesRef), &bframes) &&
              mp4_reference_parse(kMp4H264BaselineGopsRef, sizeof(kMp4H264BaselineGopsRef), &baseline) &&
              mp4_reference_parse(kMp4H264HighCropPyramidRef, sizeof(kMp4H264HighCropPyramidRef), &pyramid),
          "references parse");
    pictures(device, context, "pictures: Main profile with B pictures and AAC", kMp4H264AacBFrames, sizeof(kMp4H264AacBFrames),
             bframes, false);
    pictures(device, context, "pictures: Baseline, two GOPs", kMp4H264BaselineGops, sizeof(kMp4H264BaselineGops), baseline,
             true);
    pictures(device, context, "pictures: High, cropped, B pyramid", kMp4H264HighCropPyramid, sizeof(kMp4H264HighCropPyramid),
             pyramid, false);
    audio("audio: the AAC track of the B picture movie", kMp4H264AacBFrames, sizeof(kMp4H264AacBFrames), bframes);
    Mp4Reference fragmented_reference;
    check(mp4_reference_parse(kMp4H264FragmentedRef, sizeof(kMp4H264FragmentedRef), &fragmented_reference),
          "fragmented reference parses");
    fragmented(device, context, fragmented_reference);
    hevc_without_driver(device);
    refusals(device);
    gpu_device_destroy(&device, &context);
    test_exit(g_failures, "maicord_mp4_decoder_linux");
}
