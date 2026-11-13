// Integration test of the Linux audio backend against a real PulseAudio server (opt-in: tools/test_pulse_audio.sh;
// WSLg's /mnt/wslg/PulseServer, a local pulseaudio or pipewire-pulse). Nothing it plays leaves the machine's
// speakers alone: the test loads its own null sink, plays into it, and records from the sink's monitor with the
// voice capture worker, asserting that samples flow, and what they are.
//
//   voice enumeration   the null sink is listed (monitors are not offered as microphones), the default first
//   voice output        a square wave to the sink by name, then by a name that does not exist (falls back)
//   notifications       one clip at 44100 Hz and one at 48000 Hz, both completed and heard
//   media               a stream at unity and at volume 0, the clock and the end of the stream, pause and resume
//   endpoint events     the sink list watcher reports a sink appearing and disappearing
//
// The waves are square so no maths library is needed: the recording is measured by its peak, the length of the
// active span and the number of sign changes in it, which give the frequency.
//
// Exit status: 0 pass, 1 fail, 77 skipped (no libpulse, no server, or a server that refuses to load a module).
#include "../../../src/platform/audio/pulse/pulse.hpp"
#include "../../../src/platform/audio/pulse/timing.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/audio/media_audio.hpp"
#include "maicord/platform/audio/notification_audio.hpp"
#include "maicord/platform/audio/voice_audio_capture.hpp"
#include "maicord/platform/audio/voice_audio_output.hpp"
#include "window_post_stub.hpp"

namespace maicord {
namespace {

constexpr const char* kSink = "maicord_test_sink";
constexpr const char* kMonitor = "maicord_test_sink.monitor";
constexpr const char* kEventSink = "maicord_test_event_sink";
constexpr u32 kRate = 48000;

u32 g_failures;

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

// ---- The recording: what the voice capture worker hands to its sink --------------------------------------------

constexpr u32 kRecordCapacity = kRate * 2;
i16 g_record[kRecordCapacity];
volatile LONG g_recorded;     // samples stored since the last reset (at most kRecordCapacity)
volatile LONG g_delivered;    // every sample delivered since the start
volatile LONG g_reset;        // 1 asks the sink to start the recording again; the sink clears it
volatile LONG g_sink_calls;
volatile LONG g_largest_piece;

void record_sink(void*, ConstSlice<i16> mono, u64) noexcept {
    InterlockedIncrement(&g_sink_calls);
    if (static_cast<LONG>(mono.count) > InterlockedCompareExchange(&g_largest_piece, 0, 0))
        InterlockedExchange(&g_largest_piece, static_cast<LONG>(mono.count));
    LONG stored = InterlockedCompareExchange(&g_recorded, 0, 0);
    if (InterlockedExchange(&g_reset, 0))
        stored = 0;
    for (u32 i = 0; i < mono.count && stored < static_cast<LONG>(kRecordCapacity); ++i)
        g_record[stored++] = mono[i];
    InterlockedExchange(&g_recorded, stored);
    InterlockedExchangeAdd(&g_delivered, static_cast<LONG>(mono.count));
}
u64 record_tick(void*, u64) noexcept {
    return 0;
}

// Starts a new recording; false when the capture worker never took the request.
bool restart_recording() noexcept {
    InterlockedExchange(&g_reset, 1);
    return wait_until([]() noexcept { return InterlockedCompareExchange(&g_reset, 0, 0) == 0; }, 2000);
}

void settle(u32 milliseconds) noexcept {
    wait_until([]() noexcept { return false; }, milliseconds);
}

struct Measure {
    u32 peak;         // the largest absolute sample
    u32 span;         // samples between the first and the last sign change that crossed the threshold
    u32 changes;      // sign changes in the span
    u32 frequency_hz; // of a square wave
};

// Sign changes through +-threshold, so quiet noise does not count. Square waves only.
Measure measure(const i16* samples, u32 count, i32 threshold) noexcept {
    Measure result{};
    i32 state = 0;
    u32 first = 0, last = 0;
    for (u32 i = 0; i < count; ++i) {
        const i32 value = samples[i];
        const u32 magnitude = static_cast<u32>(value < 0 ? -value : value);
        if (magnitude > result.peak)
            result.peak = magnitude;
        const i32 next = value >= threshold ? 1 : value <= -threshold ? -1 : state;
        if (next != state) {
            if (state != 0) {
                if (!result.changes)
                    first = i;
                last = i;
                ++result.changes;
            }
            state = next;
        }
    }
    if (result.changes > 1) {
        result.span = last - first;
        result.frequency_hz = static_cast<u32>(static_cast<u64>(result.changes - 1) * kRate / (2ull * result.span));
    }
    return result;
}

Measure measure_recording() noexcept {
    return measure(g_record, static_cast<u32>(InterlockedCompareExchange(&g_recorded, 0, 0)), 1000);
}

// A new recording that starts in silence: the monitor still delivers the tail of whatever played before, so the
// recording is restarted until a tenth of a second of it is quiet.
bool quiet_recording() noexcept {
    for (u32 attempt = 0; attempt < 40; ++attempt) {
        if (!restart_recording())
            return false;
        settle(100);
        if (measure(g_record, static_cast<u32>(InterlockedCompareExchange(&g_recorded, 0, 0)), 1000).peak < 200)
            return restart_recording();
    }
    return false;
}

bool near(u32 value, u32 expected, u32 percent) noexcept {
    return value * 100ull >= expected * (100ull - percent) && value * 100ull <= expected * (100ull + percent);
}

void show(const char* label, const Measure& heard) noexcept {
    test_write_text("  ");
    test_write_text(label);
    test_write_text(": peak ");
    test_write_u64(heard.peak);
    test_write_text(", ");
    test_write_u64(heard.frequency_hz);
    test_write_text(" Hz over ");
    test_write_u64(heard.span);
    test_write_text(" samples, ");
    test_write_u64(heard.changes);
    test_write_line(" sign changes");
}

// ---- The voice output's tone ------------------------------------------------------------------------------------

struct Tone {
    u32 period_frames;
    i16 amplitude;
    volatile LONG phase;
    volatile LONG calls;
    volatile LONG frames;
    volatile LONG largest;
};
Tone g_tone;

void tone_render(void* context, Slice<i16> pcm, u64) noexcept {
    auto& tone = *static_cast<Tone*>(context);
    LONG phase = InterlockedCompareExchange(&tone.phase, 0, 0);
    const u32 frames = pcm.count / 2;
    for (u32 i = 0; i < frames; ++i, ++phase) {
        const i16 value = static_cast<u32>(phase) % tone.period_frames < tone.period_frames / 2 ? tone.amplitude
                                                                                                 : -tone.amplitude;
        pcm[2 * i] = pcm[2 * i + 1] = value;
    }
    InterlockedExchange(&tone.phase, phase);
    InterlockedIncrement(&tone.calls);
    InterlockedExchangeAdd(&tone.frames, static_cast<LONG>(frames));
    if (static_cast<LONG>(frames) > InterlockedCompareExchange(&tone.largest, 0, 0))
        InterlockedExchange(&tone.largest, static_cast<LONG>(frames));
}

// ---- A null sink the test owns ------------------------------------------------------------------------------------

struct ModuleReply {
    volatile LONG done;
    volatile u32 index;
};

void module_loaded(pulse::Context*, u32 index, void* userdata) noexcept {
    auto* reply = static_cast<ModuleReply*>(userdata);
    reply->index = index;
    InterlockedExchange(&reply->done, 1);
}

void module_unloaded(pulse::Context*, i32, void* userdata) noexcept {
    InterlockedExchange(&static_cast<ModuleReply*>(userdata)->done, 1);
}

// The module calls only this test makes, so the backend's table (pulse/abi.hpp) does not carry them: looked up in the
// libpulse the backend has open (opening it again only adds a reference, which is dropped at once).
using ModuleIndexCb = void (*)(pulse::Context*, u32 index, void*) noexcept;
using LoadModule = pulse::Operation* (*)(pulse::Context*, const char*, const char*, ModuleIndexCb, void*) noexcept;
using UnloadModule = pulse::Operation* (*)(pulse::Context*, u32, pulse::ContextSuccessCb, void*) noexcept;

template <typename Function> Function pulse_function(const char* name) noexcept {
    SharedLibrary library{};
    void* address = shared_library_open(&library, "libpulse.so.0") ? dlsym(library.handle, name) : nullptr;
    shared_library_close(&library);
    return reinterpret_cast<Function>(address);
}

// Loads module-null-sink for `name`; pulse::kInvalid when the server refuses.
u32 load_null_sink(pulse::Session& session, const char* name) noexcept {
    const auto load_module = pulse_function<LoadModule>("pa_context_load_module");
    if (!load_module)
        return pulse::kInvalid;
    char arguments[160];
    u32 length = 0;
    const char* const parts[] = {"sink_name=", name, " rate=48000 channels=2 sink_properties=device.description=MaiCord-test"};
    for (const char* part : parts)
        for (; *part && length + 1 < sizeof(arguments); ++part)
            arguments[length++] = *part;
    arguments[length] = 0;
    ModuleReply reply{0, pulse::kInvalid};
    {
        pulse::Lock lock(session);
        pulse::Operation* operation = load_module(session.context, "module-null-sink", arguments, module_loaded, &reply);
        if (!operation)
            return pulse::kInvalid;
        pulse::api()->pa_operation_unref(operation);
    }
    wait_until([&]() noexcept { return InterlockedCompareExchange(&reply.done, 0, 0) != 0; }, 3000);
    return reply.index;
}

void unload_module(pulse::Session& session, u32 index) noexcept {
    const auto unload = pulse_function<UnloadModule>("pa_context_unload_module");
    if (index == pulse::kInvalid || !unload)
        return;
    ModuleReply reply{0, 0};
    {
        pulse::Lock lock(session);
        pulse::Operation* operation = unload(session.context, index, module_unloaded, &reply);
        if (!operation)
            return;
        pulse::api()->pa_operation_unref(operation);
    }
    wait_until([&]() noexcept { return InterlockedCompareExchange(&reply.done, 0, 0) != 0; }, 3000);
}

bool same_text(const char* a, const char* b) noexcept {
    u32 i = 0;
    while (a[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}

bool ends_with(const char* text, const char* suffix) noexcept {
    u32 text_length = 0, suffix_length = 0;
    while (text[text_length])
        ++text_length;
    while (suffix[suffix_length])
        ++suffix_length;
    if (suffix_length > text_length)
        return false;
    return same_text(text + text_length - suffix_length, suffix);
}

// ---- The scenario ------------------------------------------------------------------------------------------------

void devices(pulse::Session& session) noexcept {
    (void)session;
    VoiceAudioDevice list[32];
    const u32 render = voice_audio_devices(false, {list, 32});
    bool has_sink = false, any_default = false;
    wchar_t sink_id[kVoiceDeviceIdCharacters];
    pulse::device_id_from_name(kSink, sink_id, kVoiceDeviceIdCharacters);
    for (u32 i = 0; i < render; ++i) {
        has_sink |= voice_device_id_equal(list[i].id, sink_id);
        any_default |= list[i].is_default;
    }
    const bool default_first = render && (!any_default || list[0].is_default);
    check(render >= 1 && has_sink, "voice devices: the render list holds the test's null sink");
    check(default_first, "voice devices: the default sink, when there is one, is listed first");
    bool description = false;
    for (u32 i = 0; i < render; ++i)
        if (voice_device_id_equal(list[i].id, sink_id))
            description = list[i].name_bytes == 12 && list[i].name[0] == 'M' && list[i].name[11] == 't';
    check(description, "voice devices: the sink is listed by its description, not its name");
    const u32 capture = voice_audio_devices(true, {list, 32});
    bool monitor = false;
    for (u32 i = 0; i < capture; ++i) {
        char name[kVoiceDeviceIdCharacters];
        if (pulse::device_name_from_id(list[i].id, name, sizeof(name)))
            monitor |= ends_with(name, ".monitor");
    }
    check(!monitor, "voice devices: monitors of sinks are not offered as microphones");
}

void show_attributes(const char* label, const pulse::BufferAttr& attributes) noexcept {
    test_write_text("  ");
    test_write_text(label);
    test_write_text(" granted: tlength ");
    test_write_u64(attributes.tlength);
    test_write_text(" minreq ");
    test_write_u64(attributes.minreq);
    test_write_text(" prebuf ");
    test_write_u64(attributes.prebuf);
    test_write_text(" fragsize ");
    test_write_u64(attributes.fragsize);
    test_write_text(" maxlength ");
    test_write_u64(attributes.maxlength);
    test_write_line(" bytes");
}

// What the server grants of the 20 ms render and 40 ms capture buffers the workers ask for (timing.hpp): it may adjust
// them, but not by more than a factor of two, and it must not make the render buffer so large it adds latency.
void buffers(pulse::Session& session) noexcept {
    constexpr u32 flags = pulse::kStreamStartCorked | pulse::kStreamInterpolateTiming | pulse::kStreamAutoTimingUpdate |
                          pulse::kStreamAdjustLatency;
    const pulse::BufferAttr render_plan =
        pulse::playback_attributes(kRate, 4, pulse::kRenderBufferMs, pulse::kVoicePeriodMs);
    const pulse::BufferAttr capture_plan =
        pulse::record_attributes(kRate, 2, pulse::kCaptureBufferMs, pulse::kVoicePeriodMs);
    pulse::Track render{}, capture{};
    u32 error = 0;
    pulse::BufferAttr rendered{}, captured{};
    // What the server granted (it may adjust the requested attributes), read under the session's lock.
    using GetBufferAttr = const pulse::BufferAttr* (*)(pulse::Stream*);
    const auto get_buffer_attr = pulse_function<GetBufferAttr>("pa_stream_get_buffer_attr");
    const auto granted = [&](pulse::Track& track, pulse::BufferAttr* out) noexcept {
        pulse::Lock lock(session);
        const pulse::BufferAttr* attributes = get_buffer_attr ? get_buffer_attr(track.stream) : nullptr;
        if (attributes)
            *out = *attributes;
        return attributes != nullptr;
    };
    const pulse::TrackConfig playback{"MaiCord test render", "phone", {pulse::kSampleS16LE, kRate, 2}, kSink, render_plan,
                                      flags, -1.0f};
    const pulse::TrackConfig record{"MaiCord test capture", "phone", {pulse::kSampleS16LE, kRate, 1}, kMonitor,
                                    capture_plan, flags & ~pulse::kStreamStartCorked, -1.0f};
    check(pulse::track_open_playback(&session, &render, playback, &error) &&
              pulse::track_wait_ready(&render, nullptr, 3000, &error) && granted(render, &rendered),
          "buffers: the render stream opens with the voice render plan");
    check(pulse::track_open_record(&session, &capture, record, &error) &&
              pulse::track_wait_ready(&capture, nullptr, 3000, &error) && granted(capture, &captured),
          "buffers: the capture stream opens with the voice capture plan");
    show_attributes("render (asked 3840/1920)", rendered);
    show_attributes("capture (asked fragsize 960, max 3840)", captured);
    check(rendered.tlength >= render_plan.minreq && rendered.tlength <= 2 * render_plan.tlength &&
              rendered.minreq > 0 && rendered.minreq <= 2 * render_plan.minreq,
          "buffers: the granted render buffer stays within twice the 20 ms asked for, in 10 ms requests");
    check(captured.fragsize > 0 && captured.fragsize <= 2 * capture_plan.fragsize,
          "buffers: the granted capture fragment stays within twice the 10 ms asked for");
    pulse::track_close(&render);
    pulse::track_close(&capture);
}

void output(const char* device_name, const char* label_ok, const char* label_flow) noexcept {
    g_tone = {};
    g_tone.period_frames = 100;  // 480 Hz at 48 kHz
    g_tone.amplitude = 10000;
    wchar_t id[kVoiceDeviceIdCharacters];
    const bool named = device_name && pulse::device_id_from_name(device_name, id, kVoiceDeviceIdCharacters);
    VoiceAudioOutput voice{};
    check(voice_audio_output_start_device(&voice, named ? id : nullptr, tone_render, &g_tone) && voice_audio_output_running(voice),
          "voice output: the worker starts");
    check(wait_until([]() noexcept { return measure_recording().peak > 3000; }, 5000),
          label_flow);
    check(restart_recording() &&
              wait_until([]() noexcept { return InterlockedCompareExchange(&g_recorded, 0, 0) >= 24000; }, 5000),
          "voice output: half a second more of the tone is recorded");
    const Measure heard = measure_recording();
    show("voice output", heard);
    check(near(heard.peak, 10000, 10) && near(heard.frequency_hz, 480, 10) && heard.span >= 20000, label_ok);
    voice_audio_output_stop(&voice);
    check(!voice_audio_output_running(voice) && InterlockedCompareExchange(&g_tone.calls, 0, 0) > 10 &&
              InterlockedCompareExchange(&g_tone.largest, 0, 0) <= 960,
          "voice output: the render callback runs in 20 ms-buffer sized requests and the worker joins");
}

void notifications() noexcept {
    // 300 ms square clips at 44100 Hz (period 90, 490 Hz) and 48000 Hz (period 100, 480 Hz).
    static i16 clip_44100[44100 * 3 / 10 * 2], clip_48000[48000 * 3 / 10 * 2];
    for (u32 i = 0; i < 44100 * 3 / 10; ++i)
        clip_44100[2 * i] = clip_44100[2 * i + 1] = i % 90 < 45 ? 8000 : -8000;
    for (u32 i = 0; i < 48000 * 3 / 10; ++i)
        clip_48000[2 * i] = clip_48000[2 * i + 1] = i % 100 < 50 ? 8000 : -8000;
    NotificationAudio audio{};
    NotificationAudioConfig config{};
    config.wake = {reinterpret_cast<NativeWindow>(usize{1}), wake_notifications};
    check(notification_audio_initialize(&audio, config).ok(), "notification audio: initializes");
    const u32 posts_before = test_window_posts();
    check(quiet_recording(), "notification audio: the monitor goes quiet and the recording restarts");
    NotificationAudioRequest first{1, 0, 0.5f, {{clip_44100, 44100 * 3 / 10 * 2}, kNotificationAudioRate, 0}};
    check(notification_audio_submit(&audio, first).ok(), "notification audio: a 44100 Hz clip is accepted");
    check(wait_until([&]() noexcept { return notification_audio_status(&audio).completed == 1; }, 8000),
          "notification audio: the 44100 Hz clip completes");
    // The clip's tail is still in flight when it completes in the stream's own timeline: give the recording the
    // 100 ms the sink and the monitor need to deliver it.
    settle(150);
    const Measure first_heard = measure_recording();
    show("notification 44100 Hz clip", first_heard);
    // The sink runs at 48000 Hz, so this clip is resampled and rings a little at its edges: the peak overshoots.
    check(first_heard.peak >= 3500 && first_heard.peak <= 5500 && near(first_heard.frequency_hz, 490, 10) &&
              first_heard.span >= kRate / 10 && first_heard.span <= kRate * 45 / 100,
          "notification audio: the 44100 Hz clip is heard at its pitch, half gain and about its length");
    check(quiet_recording(), "notification audio: the recording restarts for the second clip");
    NotificationAudioRequest second{1, 0, 0.5f, {{clip_48000, 48000 * 3 / 10 * 2}, kNotificationAudioAlternateRate, 0}};
    check(notification_audio_submit(&audio, second).ok(), "notification audio: a 48000 Hz clip is accepted");
    check(wait_until([&]() noexcept { return notification_audio_status(&audio).completed == 2; }, 8000),
          "notification audio: the 48000 Hz clip completes beside the first rate's stream");
    settle(150);
    const Measure second_heard = measure_recording();
    show("notification 48000 Hz clip", second_heard);
    check(near(second_heard.peak, 4000, 12) && near(second_heard.frequency_hz, 480, 10),
          "notification audio: the 48000 Hz clip is heard at its pitch");
    const auto status = notification_audio_status(&audio);
    check(status.accepted == 2 && status.completed == 2 && !status.failed && !status.canceled && !status.outstanding &&
              status.error == NotificationAudioError::none && status.device_open_attempts == 1 &&
              status.stream_open_attempts == 2 && test_window_posts() > posts_before,
          "notification audio: counters balance, one connection served both rates, the window was woken");
    check(wait_until([&]() noexcept { return !notification_audio_status(&audio).stream_open; }, 3000),
          "notification audio: the streams close once nothing is playing");
    check(notification_audio_release(&audio, 2000).ok() && !audio.runtime, "notification audio: the worker joins");
}

constexpr u32 kMediaRingFrames = 16384;
alignas(4) float g_media_ring[kMediaRingFrames * 2];

// Plays `frames` of a 400 Hz square wave (amplitude 0.3 = 9830 in PCM16 terms) through a MediaAudio at `volume`; the
// producer refills the ring as the playback frees it. Returns the final status.
struct MediaRun {
    MediaAudioStatus status;
    bool completed;
};
void show_run(const char* label, const MediaRun& run) noexcept {
    const auto& s = run.status;
    test_write_text("  media ");
    test_write_text(label);
    test_write_text(": error ");
    test_write_u64(static_cast<u64>(s.error));
    test_write_text(" native ");
    test_write_u64(s.native_error);
    test_write_text(", written ");
    test_write_u64(s.written_frames);
    test_write_text(" submitted ");
    test_write_u64(s.submitted_frames);
    test_write_text(" consumed ");
    test_write_u64(s.consumed_frames);
    test_write_text(", ended ");
    test_write_u64(s.ended);
    test_write_text(" playing ");
    test_write_u64(s.playing);
    test_write_text(" paused ");
    test_write_u64(s.paused);
    test_write_text(" clock ");
    test_write_u64(s.clock_valid);
    test_write_text(" presentation ");
    test_write_u64(static_cast<u64>(s.presentation_time_100ns));
    test_write_text(" volume x1000 ");
    test_write_u64(static_cast<u64>(s.applied_volume * 1000.0f));
    test_write_line(run.completed ? ", completed" : ", not completed");
}

MediaRun play_media(u32 frames, float volume, bool pause_midway) noexcept {
    MediaRun run{};
    const Waitable refill = event_create(false, false);
    MediaAudio media{};
    MediaAudioConfig config{};
    config.pcm = {g_media_ring, kMediaRingFrames * 2};
    config.sample_rate = kRate;
    config.channels = 2;
    config.volume = volume;
    config.refill_event = refill;
    check(refill && media_audio_initialize(&media, config).ok(), "media audio: initializes");
    float chunk[2048 * 2];
    u32 written = 0;
    auto produce = [&]() noexcept {
        while (written < frames) {
            const u32 want = minimum<u32>(2048, frames - written);
            for (u32 i = 0; i < want; ++i)
                chunk[2 * i] = chunk[2 * i + 1] = (written + i) % 120 < 60 ? 0.3f : -0.3f;
            const auto result = media_audio_write(&media, 1, {chunk, want * 2});
            if (result.error == MediaAudioError::capacity)
                return true;
            if (!result.ok())
                return false;
            written += want;
        }
        return true;
    };
    check(produce() && media_audio_play(&media).ok(), "media audio: the first PCM is accepted and playback starts");
    bool producing = true;
    bool paused = false, resumed = !pause_midway;
    const u64 end = clock_monotonic_milliseconds() + 10000;
    while (clock_monotonic_milliseconds() < end) {
        if (written < frames)
            producing = produce();
        else if (!media_audio_status(&media).ended && !media.ring.ended)
            media_audio_end(&media, 1);
        const auto status = media_audio_status(&media);
        if (pause_midway && !paused && status.consumed_frames > 4800) {
            paused = media_audio_pause(&media).ok();
            check(wait_until([&]() noexcept {
                      const auto s = media_audio_status(&media);
                      return s.paused && !s.playing;
                  },
                  3000),
                  "media audio: pause stops the stream and reports paused");
        } else if (paused && !resumed) {
            resumed = media_audio_play(&media).ok();
        }
        if (status.ended || !producing || status.error != MediaAudioError::none)
            break;
        wait_one(refill, 20);
    }
    run.status = media_audio_status(&media);
    run.completed = run.status.ended && run.status.consumed_frames == frames;
    check(resumed && (!pause_midway || paused), "media audio: pause and resume were issued when asked for");
    check(media_audio_release(&media, 3000).ok() && !media.runtime, "media audio: the worker joins");
    waitable_close(refill);
    return run;
}

void media() noexcept {
    check(quiet_recording(), "media audio: the monitor goes quiet and the recording restarts");
    const MediaRun silent = play_media(11520, 0.0f, false);
    show_run("volume 0", silent);
    check(silent.completed && silent.status.error == MediaAudioError::none && silent.status.applied_volume == 0.0f,
          "media audio: a stream at volume 0 plays to its end with the volume applied");
    settle(150);
    const Measure muted = measure_recording();
    show("volume 0", muted);
    check(muted.peak < 300, "media audio: volume 0 is silent in the recording (per-stream volume)");

    check(quiet_recording(), "media audio: the recording restarts for unity volume");
    const MediaRun loud = play_media(28800, 1.0f, false);
    show_run("unity", loud);
    check(loud.completed && loud.status.error == MediaAudioError::none && loud.status.clock_valid &&
              loud.status.clock_frequency == 1'000'000 && loud.status.presentation_time_100ns == 6'000'000 &&
              !loud.status.playing && loud.status.written_frames == 28800 && loud.status.submitted_frames == 28800,
          "media audio: 28800 frames play to the end; the clock, the presentation time and the counters agree");
    settle(150);
    const Measure heard = measure_recording();
    show("unity", heard);
    check(near(heard.peak, 9830, 10) && near(heard.frequency_hz, 400, 10) && heard.span >= kRate / 2,
          "media audio: unity volume is heard at the source level and pitch");

    const MediaRun paused = play_media(14400, 1.0f, true);
    show_run("paused and resumed", paused);
    check(paused.completed && paused.status.error == MediaAudioError::none,
          "media audio: a paused and resumed stream still plays to its end");
}

void events(pulse::Session& session) noexcept {
    pulse::session_take_dirty(&session);
    const u32 module = load_null_sink(session, kEventSink);
    check(module != pulse::kInvalid, "endpoint events: a second null sink loads");
    check(wait_until([&]() noexcept { return (InterlockedCompareExchange(&session.dirty, 0, 0) & kAudioEndpointTopologyChanged) != 0; }, 3000),
          "endpoint events: a sink appearing is reported as a topology change");
    pulse::session_take_dirty(&session);
    unload_module(session, module);
    check(wait_until([&]() noexcept { return (InterlockedCompareExchange(&session.dirty, 0, 0) & kAudioEndpointTopologyChanged) != 0; }, 3000),
          "endpoint events: a sink disappearing is reported as a topology change");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_pulse_loopback_entry) {
    using namespace maicord;
    if (!pulse::api())
        skip("libpulse.so.0 is not installed");
    const Waitable wake = event_create(false, false);
    pulse::Session session{};
    u32 error = 0;
    if (!pulse::session_open(&session, wake, "MaiCord test", AudioFlow::render, true, nullptr, 3000, &error))
        skip("no PulseAudio server to test against");
    u32 module = load_null_sink(session, kSink);
    if (module == pulse::kInvalid) {
        if (!pulse::device_exists(&session, AudioFlow::render, kSink, nullptr))
            skip("the server does not load modules, so there is no sink to play into");
        // A leftover of an earlier run: use it, leave it alone.
    }
    VoiceAudioCapture capture{};
    wchar_t monitor_id[kVoiceDeviceIdCharacters];
    pulse::device_id_from_name(kMonitor, monitor_id, kVoiceDeviceIdCharacters);
    if (!pulse::device_exists(&session, AudioFlow::capture, kMonitor, nullptr))
        skip("the null sink has no monitor source");
    devices(session);
    buffers(session);
    check(voice_audio_capture_start(&capture, monitor_id, false, record_sink, record_tick, nullptr),
          "voice capture: the worker starts on the sink's monitor");
    check(wait_until([&]() noexcept { return voice_audio_capture_status(capture).state == VoiceCaptureState::capturing; }, 5000) &&
              voice_audio_capture_status(capture).native_error == 0,
          "voice capture: the stream opens and the state is capturing");
    check(wait_until([]() noexcept { return InterlockedCompareExchange(&g_delivered, 0, 0) > 4800; }, 5000) &&
              InterlockedCompareExchange(&g_largest_piece, 0, 0) <= 1920,
          "voice capture: samples flow from the monitor in at most 40 ms pieces");
    output(kSink, "voice output: the tone is heard at its level and pitch on the named sink",
           "voice output: the tone reaches the monitor");
    output("maicord_no_such_sink", "voice output: a missing sink falls back to the default and is heard",
           "voice output: the tone reaches the monitor through the default sink");
    notifications();
    media();
    events(session);
    voice_audio_capture_stop(&capture);
    check(!voice_audio_capture_running(capture), "voice capture: stops and joins");
    if (module != pulse::kInvalid)
        unload_module(session, module);
    pulse::session_close(&session);
    waitable_close(wake);
    test_exit(g_failures, "pulse_loopback");
}
