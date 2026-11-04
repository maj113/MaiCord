// Audio with nothing to talk to must be a state, not a crash: the device lists are empty, the voice workers keep
// retrying, the notification and media workers report a device error, speech reports unavailable.
//
//   --no-server    libpulse loads but PULSE_SERVER names a socket nobody listens on (the case of a machine without
//                  a sound server)
//   --no-library   MAICORD_PULSE_LIBRARY names a library that does not exist (a machine without libpulse)
//
// The cases set those variables themselves (cmake/linux_audio_tests.cmake); neither opens an audio device.
#include "../../../src/platform/audio/pulse/pulse.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/audio/media_audio.hpp"
#include "maicord/platform/audio/notification_audio.hpp"
#include "maicord/platform/audio/speech.hpp"
#include "maicord/platform/audio/voice_audio_capture.hpp"
#include "maicord/platform/audio/voice_audio_output.hpp"
#include "maicord/sys/linux/process.hpp"
#include "window_post_stub.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

template <typename Predicate> bool wait_until(Predicate predicate, u32 milliseconds) noexcept {
    const u64 end = clock_monotonic_milliseconds() + milliseconds;
    while (!predicate()) {
        if (clock_monotonic_milliseconds() >= end)
            return false;
        thread_sleep(5);
    }
    return true;
}

bool same_text(const char* a, const char* b) noexcept {
    u32 i = 0;
    while (a[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}

void sink(void*, ConstSlice<i16>, u64) noexcept {}
u64 tick(void*, u64) noexcept {
    return 0;
}
void render(void*, Slice<i16> pcm, u64) noexcept {
    for (u32 i = 0; i < pcm.count; ++i)
        pcm[i] = 0;
}

constexpr u32 kClipFrames = 480;
i16 g_clip[kClipFrames * 2];
alignas(4) float g_ring[4096];

void run(bool no_library) noexcept {
    // Every probe below ends in the same two facts: nothing crashed, and the failure is reported as a value.
    const u32 expected = no_library ? pulse::kErrorLibraryMissing : 0;
    check(!pulse::api() == no_library, no_library ? "audio unavailable: a missing libpulse is not loaded"
                                                  : "audio unavailable: libpulse loads without a server");

    VoiceAudioDevice devices[8];
    check(voice_audio_devices(false, {devices, 8}) == 0 && voice_audio_devices(true, {devices, 8}) == 0,
          "audio unavailable: the device lists are empty, so the selectors show only the default");

    VoiceAudioOutput output{};
    check(voice_audio_output_start_device(&output, nullptr, render, nullptr) && voice_audio_output_running(output),
          "audio unavailable: voice output starts its retrying worker");
    thread_sleep(100);
    voice_audio_output_stop(&output);
    check(!voice_audio_output_running(output), "audio unavailable: voice output stops and joins");

    VoiceAudioCapture capture{};
    check(voice_audio_capture_start(&capture, nullptr, true, sink, tick, nullptr) && voice_audio_capture_running(capture),
          "audio unavailable: voice capture starts its retrying worker");
    check(wait_until(
              [&]() noexcept {
                  const auto status = voice_audio_capture_status(capture);
                  return status.state == VoiceCaptureState::waiting && status.native_error != 0;
              },
              3000),
          "audio unavailable: voice capture reports the waiting state with a native error");
    const auto capture_status = voice_audio_capture_status(capture);
    check(no_library ? capture_status.native_error == expected : (capture_status.native_error & pulse::kErrorPulse) == pulse::kErrorPulse,
          no_library ? "audio unavailable: the capture error names the missing library"
                     : "audio unavailable: the capture error is the server's");
    voice_audio_capture_stop(&capture);
    check(!voice_audio_capture_running(capture), "audio unavailable: voice capture stops and joins");

    for (u32 i = 0; i < kClipFrames * 2; ++i)
        g_clip[i] = (i & 2) ? 4000 : -4000;
    NotificationAudio notification{};
    NotificationAudioConfig notification_config{};
    notification_config.wake = {reinterpret_cast<NativeWindow>(usize{1}), wake_notifications};
    check(notification_audio_initialize(&notification, notification_config).ok(),
          "audio unavailable: notification audio initializes without a device");
    NotificationAudioRequest request{1, 0, 0.5f, {{g_clip, kClipFrames * 2}, kNotificationAudioAlternateRate, 0}};
    check(notification_audio_submit(&notification, request).ok(), "audio unavailable: a notification is accepted");
    check(wait_until([&]() noexcept { return notification_audio_status(&notification).failed == 1; }, 3000),
          "audio unavailable: the notification fails once, as a counted device failure");
    const auto notification_status = notification_audio_status(&notification);
    check(notification_status.error == NotificationAudioError::device && notification_status.native_error != 0 &&
              !notification_status.stream_open && !notification_status.outstanding &&
              (!no_library || notification_status.native_error == expected),
          "audio unavailable: the notification status names the device error and holds no reservation");
    check(test_window_posts() > 0 && test_window_last_cause() == wake_notifications,
          "audio unavailable: the failure wakes the window with the notification cause");
    check(notification_audio_release(&notification, 2000).ok() && !notification.runtime,
          "audio unavailable: the notification worker joins");

    MediaAudio media{};
    MediaAudioConfig media_config{};
    media_config.pcm = {g_ring, 4096};
    media_config.sample_rate = 48000;
    media_config.channels = 2;
    check(media_audio_initialize(&media, media_config).ok(), "audio unavailable: media audio initializes without a device");
    const float one[2]{0.25f, -0.25f};
    check(media_audio_write(&media, 1, {one, 2}).ok() && media_audio_play(&media).ok(),
          "audio unavailable: media audio accepts PCM and starts its worker");
    check(wait_until([&]() noexcept { return media_audio_status(&media).error == MediaAudioError::device; }, 3000),
          "audio unavailable: media audio reports a device error");
    const auto media_status = media_audio_status(&media);
    check(!media_status.stream_open && !media_status.playing && !media_status.clock_valid &&
              media_status.native_error != 0 && (!no_library || media_status.native_error == expected),
          "audio unavailable: media status holds no stream, no clock and the native error");
    check(media_audio_release(&media, 2000).ok() && !media.runtime, "audio unavailable: the media worker joins");

    Speech speech{};
    check(!speech_speak(&speech, {"hello", 5}) && !speech.voice, "audio unavailable: speech reports unavailable");
    speech_release(&speech);
    check(!speech.attempted, "audio unavailable: releasing speech resets it");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_audio_unavailable_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    bool no_server = false, no_library = false;
    for (int i = 1; i < startup.argc; ++i) {
        no_server |= same_text(startup.argv[i], "--no-server");
        no_library |= same_text(startup.argv[i], "--no-library");
    }
    if (no_server == no_library)
        test_exit(1, "usage: --no-server or --no-library");
    run(no_library);
    test_exit(g_failures, no_library ? "audio_unavailable_library" : "audio_unavailable_server");
}
