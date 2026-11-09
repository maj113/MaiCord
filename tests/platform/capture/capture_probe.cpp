// The opt-in manual probe of Go Live capture on the real desktop (MAICORD_CAPTURE_PROBE=1; status 77 otherwise, so the
// suite never captures anyone's screen). It shows no window and prints counts and sizes only, never a name or a pixel:
//   Windows: the source list (counts), a thumbnail of the primary screen, two seconds of the primary screen at 30 fps
//            (frames, extent, cursor flag, timestamps on the client clock), and two seconds of stream audio.
//   Linux:   the portal's own picker opens: pick a screen or window; two seconds of frames, then stream audio.
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/audio/stream_audio.hpp"
#include "maicord/platform/capture/screen_capture.hpp"
#include "../../support/test_output.hpp"
#if defined(_WIN32)
#include "maicord/gpu/api.hpp"
#else
#include "maicord/sys/linux/process.hpp"
#endif

namespace maicord {
namespace {

u32 g_failures = 0;
void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

u64 now_us() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}

struct Video {
    volatile LONG frames, cursor_frames, stale;
    u32 width, height;
    u64 first_us, last_us;
    volatile LONG ended;
};
Video g_video;

void on_frame(void* context, const CaptureFrame& frame) noexcept {
    auto* v = static_cast<Video*>(context);
    const u64 now = now_us();
    if (frame.timestamp_us > now || frame.timestamp_us + 500'000 < now)
        InterlockedIncrement(&v->stale);
    if (!v->first_us)
        v->first_us = frame.timestamp_us;
    v->last_us = frame.timestamp_us;
    v->width = frame.width;
    v->height = frame.height;
    if (frame.cursor)
        InterlockedIncrement(&v->cursor_frames);
    InterlockedIncrement(&v->frames);
}
void on_ended(void* context, CaptureEnd reason) noexcept {
    InterlockedExchange(&static_cast<Video*>(context)->ended, static_cast<LONG>(reason) + 1);
}

struct Audio {
    volatile LONG blocks;
    u64 frames;
    u32 peak;
};
Audio g_audio;
void on_block(void* context, ConstSlice<i16> stereo, u64) noexcept {
    auto* a = static_cast<Audio*>(context);
    a->frames += stereo.count / kStreamAudioChannels;
    for (u32 i = 0; i < stereo.count; ++i) {
        const u32 magnitude = static_cast<u32>(stereo[i] < 0 ? -stereo[i] : stereo[i]);
        a->peak = magnitude > a->peak ? magnitude : a->peak;
    }
    InterlockedIncrement(&a->blocks);
}

#if defined(_WIN32)
CaptureSource g_sources[64];
u8 g_thumbnail[320 * 180 * 4];
#endif

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_capture_probe_entry) {
    using namespace maicord;
#if defined(_WIN32)
    wchar_t flag[4]{};
    const bool enabled = GetEnvironmentVariableW(L"MAICORD_CAPTURE_PROBE", flag, 4) > 0;
#else
    const bool enabled = environment_get("MAICORD_CAPTURE_PROBE") != nullptr;
#endif
    if (!enabled) {
        test_write_line("SKIP capture_probe (opt-in: MAICORD_CAPTURE_PROBE=1 captures this desktop's screen and sound)");
        test_process_exit(77);
    }
    CaptureOptions options{};
    options.max_fps = 30;
    options.cursor = true;
    options.kinds = 3;
#if defined(_WIN32)
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    check(gpu_ok(gpu_device_create(&device, &context)), "probe: a hardware Direct3D 11 device");
    const u32 count = capture_sources({g_sources, 64});
    u32 screens = 0;
    for (u32 i = 0; i < count; ++i)
        screens += g_sources[i].kind == CaptureSourceKind::screen;
    test_write_metric("screens: ", screens);
    test_write_metric("windows: ", count - screens);
    check(screens >= 1, "probe: the primary screen is listed");
    u32 width = 0, height = 0;
    const bool thumbnail = device && screens &&
                           capture_thumbnail(device, g_sources[0], 320, 180, g_thumbnail, &width, &height);
    check(thumbnail && width && height && width <= 320 && height <= 180, "probe: a thumbnail of the primary screen");
    options.device = device;
    options.source = g_sources[0];
#endif
    ScreenCapture* capture = capture_start(options, {&g_video, on_frame, on_ended, nullptr});
    check(capture != nullptr, "probe: the capture starts");
    // Linux: the portal's picker waits for a choice first.
    const u64 deadline = clock_monotonic_milliseconds() + 60'000;
    while (capture && !ReadAcquire(&g_video.frames) && !ReadAcquire(&g_video.ended) &&
           clock_monotonic_milliseconds() < deadline)
        thread_sleep(50);
    thread_sleep(2000);
    capture_stop(capture);
    test_write_metric("frames: ", static_cast<u64>(g_video.frames));
    test_write_metric("width: ", g_video.width);
    test_write_metric("height: ", g_video.height);
    test_write_metric("frames with the cursor: ", static_cast<u64>(g_video.cursor_frames));
    test_write_metric("ended: ", static_cast<u64>(g_video.ended));
    check(g_video.frames > 0 && g_video.width && g_video.height, "probe: frames arrive with their extent");
    check(!g_video.stale && g_video.last_us >= g_video.first_us, "probe: timestamps are fresh on the client clock");
    check(g_video.frames <= 2 * 30 + 15, "probe: the 30 fps cap holds");

    test_write_metric("stream audio method: ", static_cast<u64>(stream_audio_method()));
    StreamAudioCapture audio{};
    const bool audio_started =
        stream_audio_start(&audio, {StreamAudioScope::everything, 0}, {&g_audio, on_block});
    check(audio_started || !stream_audio_available(stream_audio_method()), "probe: stream audio starts");
    thread_sleep(2000);
    stream_audio_stop(&audio);
    test_write_metric("audio frames: ", g_audio.frames);
    test_write_metric("audio peak: ", g_audio.peak);
    check(!audio_started || g_audio.frames >= kStreamAudioRate, "probe: about two seconds of stream audio (silence counts)");
#if defined(_WIN32)
    if (device)
        gpu_device_destroy(&device, &context);
#endif
    test_exit(g_failures, "capture_probe");
}
