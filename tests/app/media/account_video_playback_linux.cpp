// The media player's own session (account_video_session: the code Windows runs over its DXVA decoder) over the Linux
// movie decoder, with the software tier forced (MAICORD_VAAPI=off), a movie from tools/generate_mp4_fixtures.py and the
// GLES conversion of the pictures it presents:
//
//   play      the session paces the pictures by the clock: every picture of the movie arrives once, in order, with the
//             container's presentation time, converts to the reference's picture, and the movie ends
//   seek      a seek while paused presents the picture of that time first, then plays on from it
//   replay    after the end, play starts the movie again from its first picture
//   picture   picture off decodes and hands out nothing while the clock runs on (to the end); picture on, playing or
//             paused, hands out the picture of the clock first, in the same playback epoch
//   audio     (--audio, against a sound server: tools/test_pulse_audio.sh) the AAC track of a movie plays into the
//             server through the session's PCM transport: the transport writes and consumes as many frames as the
//             movie has, and the first pictures still arrive on time
//
// Status 77: no EGL device or no libopenh264 (--audio: no sound server).
#include "../../fixtures/mp4/mp4_fixtures.hpp"
#include "../../support/test_output.hpp"
#include "../../support/yuv_reference.hpp"
#include "maicord/app/media/account_video_session.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../../src/platform/decode/software/software_decoder.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr u32 kMaximumWidth = 64, kMaximumHeight = 48;
u8 g_rgba[kMaximumWidth * kMaximumHeight * 4];

const AccountVideoIdentity kIdentity{1, 2, 3, 4, 0, 1, 0};

struct Playback {
    GpuDevice* device;
    GpuContext* context;
    const Mp4Reference* reference;
    VideoFrameGpu converter;
    GpuTexture* target;
    GpuUav* uav;
    Mp4Decoder decoder;
    AccountVideoSession session;
    Waitable lane;
    u64 serial;
    u32 frames, wrong_time, wrong_picture, out_of_order;
    i64 last_time, taken_position; // the last picture's time (us) and the clock when it was handed out (100 ns)
};

bool open_playback(Playback* p, GpuDevice* device, GpuContext* context, const Mp4Reference& reference, const u8* movie,
                   usize movie_bytes) noexcept {
    *p = {};
    p->device = device;
    p->context = context;
    p->reference = &reference;
    p->last_time = -1;
    if (!gpu_ok(gpu_texture_create(device, {reference.width, reference.height, GpuFormat::rgba8, GpuUsage::default_,
                                            static_cast<u8>(gpu_bind_srv | gpu_bind_uav)},
                                   nullptr, &p->target)) ||
        !gpu_ok(gpu_uav_create(device, p->target, &p->uav)))
        return false;
    p->lane = event_create(false, false);
    return p->lane && mp4_decoder_initialize(&p->decoder, {movie, static_cast<u32>(movie_bytes)}, 0, 0, device);
}

void close_playback(Playback* p) noexcept {
    account_video_session_release(&p->session, 2000);
    mp4_decoder_release(&p->decoder);
    video_frame_gpu_release(&p->converter);
    gpu_release(p->uav);
    gpu_release(p->target);
    waitable_close(p->lane);
}

bool command(Playback* p, AccountVideoCommand kind, i64 seek_100ns = 0) noexcept {
    return account_video_session_control(&p->session, kIdentity, ++p->serial, kind, seek_100ns).ok();
}

// Compares the presented picture with the reference's picture whose time is nearest (picture `index`).
bool picture_is(Playback* p, const Mp4DecodedFrame& frame, u32 index) noexcept {
    const Mp4Reference& r = *p->reference;
    if (!frame.gpu || index >= r.frames)
        return false;
    if (!gpu_ok(video_frame_gpu_present(&p->converter, p->device, p->context, *frame.gpu, p->uav, 0, 0)) ||
        !gpu_ok(gpu_readback(p->device, p->context, p->target, {0, 0, r.width, r.height}, g_rgba, r.width * 4)))
        return false;
    const u8* y = mp4_reference_picture(r, index);
    const u8* cb = y + usize{r.width} * r.height;
    const u8* cr = cb + usize{r.width / 2} * (r.height / 2);
    const YuvRgbCoefficients coefficients = yuv_rgb_coefficients(frame.gpu->bt709, frame.gpu->full_range);
    u64 squared = 0;
    for (u32 row = 0; row < r.height; ++row)
        for (u32 column = 0; column < r.width; ++column) {
            const usize chroma = usize{row / 2} * (r.width / 2) + column / 2;
            u8 expected[4];
            yuv_rgb_pixel(expected, y[usize{row} * r.width + column], cb[chroma] - 128, cr[chroma] - 128, coefficients);
            const u8* got = g_rgba + (usize{row} * r.width + column) * 4;
            for (u32 c = 0; c < 3; ++c) {
                const i32 difference = static_cast<i32>(got[c]) - static_cast<i32>(expected[c]);
                squared += static_cast<u64>(difference * difference);
            }
        }
    return squared * 1000 <= u64{r.width} * r.height * 3 * 50'000; // mean squared error of 50 at most
}

// The index of the reference picture presented at `time_us` (the nearest start), or the frame count.
u32 index_at(const Mp4Reference& r, i64 time_us) noexcept {
    for (u32 i = 0; i < r.frames; ++i) {
        const i64 start = mp4_reference_time(r, i);
        if (time_us >= start - 1 && time_us <= start + 1)
            return i;
    }
    return r.frames;
}

// Services the session until `stop` says so or `milliseconds` pass; every presented picture is checked and
// acknowledged. Returns the pictures seen by this call.
struct Window {
    u32 pictures;
    u32 first_index;
    bool ended;
};

Window serve(Playback* p, u32 milliseconds, u32 stop_after, bool stop_at_end) noexcept {
    Window window{0, 0, false};
    const u64 deadline = clock_monotonic_milliseconds() + milliseconds;
    bool first = true;
    while (clock_monotonic_milliseconds() < deadline) {
        const AccountVideoService service = account_video_session_service(&p->session);
        if (service.frame_ready) {
            const u32 index = index_at(*p->reference, service.frame.presentation_time_us);
            if (index == p->reference->frames)
                ++p->wrong_time;
            else if (!picture_is(p, service.frame, index))
                ++p->wrong_picture;
            if (service.frame.presentation_time_us < p->last_time)
                ++p->out_of_order;
            p->last_time = service.frame.presentation_time_us;
            p->taken_position = account_video_session_status(&p->session).position_100ns;
            if (first) {
                window.first_index = index;
                first = false;
            }
            ++window.pictures;
            ++p->frames;
            account_video_session_acknowledge(&p->session, kIdentity, service.playback_epoch, service.frame_serial);
            if (stop_after && window.pictures >= stop_after)
                break;
            continue;
        }
        const AccountVideoStatus status = account_video_session_status(&p->session);
        if (status.error != AccountVideoError::none)
            break;
        if (stop_at_end && status.ended) {
            window.ended = true;
            break;
        }
        const u32 wait = service.next_service_delay_ms < 20 ? service.next_service_delay_ms : 20;
        if (service.needs_service)
            continue;
        wait_one(p->lane, wait ? wait : 1);
    }
    return window;
}

// The picture handed out last covers the clock at hand-out: within one frame of it.
bool at_clock(const Playback* p) noexcept {
    const i64 frame = (mp4_reference_time(*p->reference, 1) - mp4_reference_time(*p->reference, 0)) * 10;
    return p->last_time * 10 <= p->taken_position && p->taken_position < p->last_time * 10 + frame;
}

// Hides the picture while playing: nothing is decoded or handed out while the clock (and the audio, in the same epoch)
// runs on; repeated off is a no-op, and picture on hands out the picture of the clock's time first.
void hide_and_show(Playback* p, u32 hidden_ms, const char* off_label, const char* on_label) noexcept {
    const u64 epoch = p->session.playback_epoch;
    const AccountVideoAudioProgress before = account_video_session_audio_progress(&p->session);
    const bool off = command(p, AccountVideoCommand::picture_off);
    const i64 from = account_video_session_status(&p->session).position_100ns;
    const Window hidden = serve(p, hidden_ms, 0, false);
    const AccountVideoStatus status = account_video_session_status(&p->session);
    const AccountVideoAudioProgress during = account_video_session_audio_progress(&p->session);
    check(off && !hidden.pictures && !p->session.held && status.error == AccountVideoError::none && status.playing &&
              status.observed_command == p->serial && p->session.playback_epoch == epoch &&
              status.position_100ns >= from + i64{hidden_ms - 100} * 10000 &&
              (!status.has_audio || (during.epoch == before.epoch && during.written_frames >= before.written_frames &&
                                     during.consumed_frames > before.consumed_frames)),
          off_label);
    const u32 wrong = p->wrong_picture + p->wrong_time;
    const bool on = command(p, AccountVideoCommand::picture_off) && command(p, AccountVideoCommand::picture_on);
    const Window shown = serve(p, 2000, 1, false);
    const AccountVideoAudioProgress after = account_video_session_audio_progress(&p->session);
    check(on && shown.pictures == 1 && at_clock(p) && p->wrong_picture + p->wrong_time == wrong &&
              p->last_time * 10 >= from + i64{hidden_ms - 100} * 10000 && p->session.playback_epoch == epoch &&
              (!status.has_audio || (after.epoch == before.epoch && after.consumed_frames > during.consumed_frames)),
          on_label);
}

void play_movie(GpuDevice* device, GpuContext* context, const char* name, const u8* movie, usize movie_bytes,
                const Mp4Reference& r) noexcept {
    Playback p;
    if (!open_playback(&p, device, context, r, movie, movie_bytes)) {
        check(false, "play: the movie opens");
        return;
    }
    const AccountVideoResult started = account_video_session_initialize(&p.session, &p.decoder, kIdentity, p.lane);
    if (!started.ok()) {
        test_write_metric("session error ", static_cast<u32>(started.error));
        test_write_metric("session native error ", started.native_error);
    }
    check(started.ok(), name);
    if (!started.ok()) {
        close_playback(&p);
        return;
    }
    AccountVideoStatus status = account_video_session_status(&p.session);
    check(!status.has_audio && status.duration_100ns > 0, "play: a movie without audio, with a duration");

    // Play: every picture once, in order, paced by the clock, then the end.
    check(command(&p, AccountVideoCommand::play), "play: the command is accepted");
    const Window whole = serve(&p, 8000, 0, true);
    test_write_metric("played pictures ", whole.pictures);
    check(whole.ended && whole.pictures == r.frames && !p.wrong_time && !p.wrong_picture && !p.out_of_order,
          "play: every picture once, in order, with the container's times, equal to the reference");

    status = account_video_session_status(&p.session);
    check(status.ended && status.position_100ns > 0, "play: the movie ends with a position");

    // Replay: play after the end starts again from the first picture.
    check(command(&p, AccountVideoCommand::play), "replay: the command is accepted");
    const Window again = serve(&p, 8000, r.frames, false);
    check(again.pictures == r.frames && again.first_index == 0 && !p.wrong_picture,
          "replay: the movie starts again from its first picture");

    // Seek while paused: the picture of that time comes first, and play goes on from it.
    check(command(&p, AccountVideoCommand::pause), "seek: pause");
    const u32 target = r.frames * 2 / 3;
    check(command(&p, AccountVideoCommand::seek, mp4_reference_time(r, target) * 10), "seek: the command is accepted");
    const Window sought = serve(&p, 4000, 1, false);
    check(sought.pictures == 1 && sought.first_index == target && !p.wrong_picture && !p.wrong_time,
          "seek: the picture of the target time is the first one");
    check(command(&p, AccountVideoCommand::play), "seek: play on");
    const Window resumed = serve(&p, 8000, 0, true);
    check(resumed.ended && resumed.pictures >= r.frames - target - 1 && !p.wrong_picture && !p.wrong_time,
          "seek: play goes on from the target to the end");

    // Picture off and on: hidden while playing, shown at the clock, landed while paused, and hidden to the end.
    check(command(&p, AccountVideoCommand::play), "picture: replay");
    serve(&p, 2000, 2, false);
    hide_and_show(&p, 450, "picture off: no picture is decoded or handed out while the clock runs on",
                  "picture on: the first picture is the clock's, in the next group of pictures");
    check(command(&p, AccountVideoCommand::pause), "picture: pause");
    const i64 paused_at = account_video_session_status(&p.session).position_100ns;
    const bool toggled = command(&p, AccountVideoCommand::picture_off) && command(&p, AccountVideoCommand::picture_on);
    const Window still = serve(&p, 2000, 1, false);
    check(toggled && still.pictures == 1 && at_clock(&p) && !p.wrong_picture &&
              account_video_session_status(&p.session).position_100ns == paused_at,
          "picture on while paused: the picture of the paused clock is handed out and the clock stays");
    check(command(&p, AccountVideoCommand::picture_off) && command(&p, AccountVideoCommand::play), "picture: hidden play");
    const Window hidden_end = serve(&p, 4000, 0, true);
    check(hidden_end.ended && !hidden_end.pictures, "picture off: a hidden movie still ends on the clock");
    check(command(&p, AccountVideoCommand::picture_on) && command(&p, AccountVideoCommand::play), "picture: shown replay");
    const Window shown_replay = serve(&p, 8000, 0, true);
    check(shown_replay.ended && shown_replay.pictures == r.frames && !p.wrong_picture,
          "picture on after a hidden end: replay hands out every picture again");
    close_playback(&p);
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

// The movie with an AAC track through the session's PCM transport into a sound server.
bool play_with_audio(GpuDevice* device, GpuContext* context, const Mp4Reference& r) noexcept {
    Playback p;
    if (!open_playback(&p, device, context, r, kMp4H264AacBFrames, sizeof(kMp4H264AacBFrames))) {
        check(false, "audio: the movie opens");
        return true;
    }
    const AccountVideoResult started = account_video_session_initialize(&p.session, &p.decoder, kIdentity, p.lane);
    if (!started.ok()) {
        test_write_metric("audio session error ", static_cast<u32>(started.error));
        close_playback(&p);
        return false;
    }
    AccountVideoStatus status = account_video_session_status(&p.session);
    check(status.has_audio && status.duration_100ns > 0, "audio: the session sees the AAC track");
    check(command(&p, AccountVideoCommand::play), "audio: the command is accepted");
    const Window whole = serve(&p, 10000, 0, true);
    status = account_video_session_status(&p.session);
    const AccountVideoAudioProgress progress = account_video_session_audio_progress(&p.session);
    test_write_metric("audio pictures ", whole.pictures);
    test_write_metric("audio frames written ", progress.written_frames);
    test_write_metric("audio frames submitted ", progress.submitted_frames);
    test_write_metric("audio frames consumed ", progress.consumed_frames);
    test_write_metric("audio error ", static_cast<u32>(status.error));
    check(status.error == AccountVideoError::none && whole.ended && whole.pictures == r.frames && !p.wrong_time &&
              !p.wrong_picture && !p.out_of_order,
          "audio: every picture arrives in order while the track plays, and the movie ends");
    check(progress.written_frames > 0 && progress.submitted_frames > 0 && progress.consumed_frames > 0,
          "audio: the transport wrote, submitted and consumed frames");
    // The picture hidden while the track plays: the audio keeps its epoch and keeps playing; shown at its clock.
    check(command(&p, AccountVideoCommand::play), "audio picture: replay");
    serve(&p, 2000, 3, false);
    hide_and_show(&p, 400, "audio picture off: no picture while the track plays on in the same epoch",
                  "audio picture on: the first picture is the audio clock's, and the track was never reset");
    close_playback(&p);
    return true;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_account_video_playback_entry) {
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
    Mp4Reference baseline, bframes;
    check(mp4_reference_parse(kMp4H264BaselineGopsRef, sizeof(kMp4H264BaselineGopsRef), &baseline) &&
              mp4_reference_parse(kMp4H264AacBFramesRef, sizeof(kMp4H264AacBFramesRef), &bframes),
          "references parse");
    play_movie(device, context, "play: Baseline movie opens in the player", kMp4H264BaselineGops,
               sizeof(kMp4H264BaselineGops), baseline);
    if (has_argument("--audio") && !play_with_audio(device, context, bframes)) {
        test_write_line("audio part skipped: the session refused the audio track (no sound server?)");
        test_process_exit(77);
    }
    gpu_device_destroy(&device, &context);
    test_exit(g_failures, "maicord_account_video_playback");
}
