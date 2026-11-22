// The media worker against a sound server that stops answering (opt-in: tools/test_pulse_stall.sh). The script starts
// a private pulseaudio with a null sink and names its process in MAICORD_TEST_PULSE_PID. The test stops it with
// SIGSTOP, as WSLg's server stalls while its RDP sink waits for Weston, and lets it go on with SIGCONT.
//
//   --recovers   the first attempt and the retry after 1 s fail; the server goes on before the retry after 2 s,
//                which connects, and the tone plays to its end
//   --gives-up   the server never goes on: the first attempt and both retries fail, then the error is the server
//   --cancel     a pause during the wait before a retry cancels it; a release during that wait joins the worker
//
// While the worker waits for a retry, the status shows buffering and no error. Each attempt fails after the 3 s that
// the worker waits for the server.
//
// Exit status: 0 pass, 1 fail, 77 skipped (no libpulse, no server, or no server process named).
#include "../../../src/platform/audio/pulse/pulse.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/test_hook.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/audio/media_audio.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "window_post_stub.hpp"

namespace maicord {
namespace {

constexpr int kSignalContinue = 18; // SIGCONT
constexpr int kSignalStop = 19;     // SIGSTOP
constexpr u32 kRate = 48000, kRingFrames = 8192, kToneFrames = 24000;

u32 g_failures;
int g_server;
alignas(4) float g_ring[kRingFrames * 2];

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

[[noreturn]] void skip(const char* reason) noexcept {
    test_write_text("SKIP ");
    test_write_line(reason);
    test_process_exit(77);
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

void server_signal(int signal) noexcept {
    check(sys_kill(g_server, signal) == 0, signal == kSignalStop ? "the server stops" : "the server goes on");
}

// A MediaAudio with a 400 Hz square wave of kToneFrames, playing on the default sink.
struct Player {
    MediaAudio media{};
    u32 written{};
    bool sealed{};
    bool start() noexcept {
        MediaAudioConfig config{};
        config.pcm = {g_ring, kRingFrames * 2};
        config.sample_rate = kRate;
        config.channels = 2;
        if (!media_audio_initialize(&media, config).ok())
            return false;
        produce();
        return media_audio_play(&media).ok();
    }
    // Writes what the ring takes, and ends the stream after the last frame.
    void produce() noexcept {
        float chunk[2048 * 2];
        while (written < kToneFrames) {
            const u32 want = minimum<u32>(2048, kToneFrames - written);
            for (u32 i = 0; i < want; ++i)
                chunk[2 * i] = chunk[2 * i + 1] = (written + i) % 120 < 60 ? 0.3f : -0.3f;
            if (!media_audio_write(&media, 1, {chunk, want * 2}).ok())
                return;
            written += want;
        }
        if (media.runtime && !sealed)
            sealed = media_audio_end(&media, 1).ok();
    }
    MediaAudioStatus status() noexcept {
        return media_audio_status(&media);
    }
    // The worker has finished `attempts` attempts: the status shows them once the attempt is over.
    bool attempted(u32 attempts, u32 milliseconds) noexcept {
        return wait_until([&]() noexcept { return status().open_attempts >= attempts; }, milliseconds);
    }
    bool waiting() noexcept {
        const auto s = status();
        return s.error == MediaAudioError::none && s.buffering && !s.stream_open;
    }
};

void recovers() noexcept {
    server_signal(kSignalStop);
    Player player{};
    check(player.start(), "recovers: the player starts");
    check(player.attempted(1, 5000) && player.waiting(),
          "recovers: the first attempt fails after the wait for the server, and the player buffers");
    check(player.attempted(2, 5000) && player.waiting(), "recovers: the retry after 1 s fails, and the player buffers");
    server_signal(kSignalContinue);
    check(wait_until([&]() noexcept {
              player.produce();
              return player.status().ended || player.status().error != MediaAudioError::none;
          }, 8000),
          "recovers: the retry after 2 s connects and the tone plays to its end");
    const auto s = player.status();
    check(s.ended && s.error == MediaAudioError::none && s.open_attempts == 3 && s.consumed_frames == kToneFrames,
          "recovers: three attempts in all, no error, every frame played");
    check(media_audio_release(&player.media, 3000).ok(), "recovers: the worker joins");
}

void gives_up() noexcept {
    server_signal(kSignalStop);
    Player player{};
    check(player.start(), "gives up: the player starts");
    check(wait_until([&]() noexcept { return player.status().error != MediaAudioError::none; }, 16000),
          "gives up: the player fails in the end");
    const auto s = player.status();
    check(s.error == MediaAudioError::server && s.native_error == pulse::kErrorServerTimeout && s.open_attempts == 3 &&
              !s.buffering && !s.stream_open,
          "gives up: after the first attempt and two retries the error is the sound server");
    check(media_audio_release(&player.media, 3000).ok(), "gives up: the worker joins");
    server_signal(kSignalContinue);
}

void cancel() noexcept {
    server_signal(kSignalStop);
    Player player{};
    check(player.start(), "cancel: the player starts");
    check(player.attempted(1, 5000) && player.waiting(), "cancel: the first attempt fails and the player buffers");
    check(media_audio_pause(&player.media).ok(), "cancel: pause during the wait for the retry");
    check(wait_until([&]() noexcept { return player.status().open_attempts == 0; }, 1000),
          "cancel: the pause cancels the retries");
    thread_sleep(3500);
    const auto paused = player.status();
    check(paused.open_attempts == 0 && paused.error == MediaAudioError::none && paused.paused && !paused.stream_open,
          "cancel: no retry starts while paused, past the time of both retries");
    check(media_audio_play(&player.media).ok() && player.attempted(1, 5000) && player.waiting(),
          "cancel: a new play tries again at once, fails and waits for a retry");
    check(media_audio_release(&player.media, 2000).ok() && !player.media.runtime,
          "cancel: a release during the wait for a retry joins the worker and frees it");
    server_signal(kSignalContinue);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_pulse_stall_entry) {
    using namespace maicord;
    if (!pulse::api())
        skip("libpulse.so.0 is not installed");
    const char* pid = test_hook("MAICORD_TEST_PULSE_PID");
    for (u32 i = 0; pid && pid[i] >= '0' && pid[i] <= '9' && i < 9; ++i)
        g_server = g_server * 10 + (pid[i] - '0');
    if (g_server <= 1)
        skip("MAICORD_TEST_PULSE_PID names no server process (tools/test_pulse_stall.sh)");
    const Waitable wake = event_create(false, false);
    pulse::Session session{};
    u32 error = 0;
    if (!pulse::session_open(&session, wake, "MaiCord test", AudioFlow::render, false, nullptr, 3000, &error))
        skip("the server does not answer before the test");
    pulse::session_close(&session);
    waitable_close(wake);
    const ProcessStartup& startup = process_startup();
    for (int i = 1; i < startup.argc; ++i) {
        const char* argument = startup.argv[i];
        if (argument[0] == '-' && argument[1] == '-' && argument[2] == 'r')
            recovers();
        else if (argument[0] == '-' && argument[1] == '-' && argument[2] == 'g')
            gives_up();
        else if (argument[0] == '-' && argument[1] == '-' && argument[2] == 'c')
            cancel();
    }
    test_exit(g_failures, "pulse_stall");
}
