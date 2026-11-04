// Voice capture on Linux: a PulseAudio record stream of 48 kHz mono PCM16 on its own thread, and the device lists
// of the settings selectors. The counterpart of the WASAPI loop in src/platform/audio/voice_audio_capture.cpp.
#include "maicord/platform/audio/voice_audio_capture.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "pulse.hpp"
#include "timing.hpp"

namespace maicord {
namespace {
constexpr u32 kRate = pulse::kVoiceRate;
constexpr u32 kFrameBytes = sizeof(i16);
constexpr u32 kFrameSamples = 480;
constexpr u32 kServerTimeoutMs = 3000;
constexpr usize kDeviceNameBytes = 128;

u64 now_us() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}
struct Runtime {
    VoiceAudioCaptureSink sink;
    VoiceAudioCaptureTick tick;
    void* context;
    Thread* thread;
    Waitable stop, wake, timer;
    volatile LONG state, native_error;
    // The source name; empty selects the server's default.
    char device[kDeviceNameBytes];
    i16 silence[kFrameSamples]; // Zero (pages): fed in place of holes in the recording.
};
struct Engine {
    pulse::Session session;
    pulse::Track track;
};

void publish(Runtime& r, VoiceCaptureState state, u32 native = 0) noexcept {
    InterlockedExchange(&r.state, static_cast<LONG>(state));
    if (native)
        InterlockedExchange(&r.native_error, static_cast<LONG>(native));
}
void close_engine(Engine& e) noexcept {
    pulse::track_close(&e.track);
    pulse::session_close(&e.session);
}

// Hands every fragment the server has captured to the sink; false when the stream failed.
bool drain(Runtime& r, Engine& e) noexcept {
    for (;;) {
        const void* data = nullptr;
        usize bytes = 0;
        if (!pulse::track_peek(&e.track, &data, &bytes))
            return false;
        if (!bytes)
            return true;
        const u64 now = now_us();
        const u32 frames = static_cast<u32>(bytes / kFrameBytes);
        if (!data) {
            for (u32 at = 0; at < frames; at += kFrameSamples)
                r.sink(r.context, {r.silence, pulse::capture_piece(frames, at, kFrameSamples)}, now);
        } else if (frames)
            r.sink(r.context, {static_cast<const i16*>(data), frames}, now);
        pulse::track_drop(&e.track);
    }
}

bool open_stream(Runtime& r, Engine& e, u32* error) noexcept {
    if (e.session.loop && !pulse::session_ready(e.session))
        pulse::session_close(&e.session);
    if (!pulse::session_ready(e.session) &&
        !pulse::session_open(&e.session, r.wake, "MaiCord", AudioFlow::capture, true, r.stop, kServerTimeoutMs, error))
        return false;
    // A missing chosen device falls back to the server's default source.
    const char* device = r.device[0] && pulse::device_exists(&e.session, AudioFlow::capture, r.device, r.stop)
                             ? r.device
                             : nullptr;
    // Communications processing (echo cancellation, noise suppression) is not ours on Linux: it is the server's
    // module-echo-cancel or a PipeWire filter chain, which the user selects as the source.
    const pulse::TrackConfig config{
        "MaiCord voice input",
        "phone",
        {pulse::kSampleS16LE, kRate, 1},
        device,
        pulse::record_attributes(kRate, kFrameBytes, pulse::kCaptureBufferMs, pulse::kVoicePeriodMs),
        pulse::kStreamInterpolateTiming | pulse::kStreamAutoTimingUpdate | pulse::kStreamAdjustLatency,
        -1.0f};
    if (!pulse::track_open_record(&e.session, &e.track, config, error) ||
        !pulse::track_wait_ready(&e.track, r.stop, kServerTimeoutMs, error)) {
        pulse::track_close(&e.track);
        return false;
    }
    return true;
}

void arm(Runtime& r, u64 wake_us) noexcept {
    if (!wake_us || !r.timer)
        return;
    timer_arm(r.timer, pulse::due_milliseconds(wake_us, now_us()), 0);
}

u32 capture_thread(void* parameter) noexcept {
    auto& r = *static_cast<Runtime*>(parameter);
    thread_boost_realtime();
    Engine e{};
    pulse::RetrySchedule retry{};
    for (;;) {
        if (e.track.stream && (!pulse::track_alive(e.track) || !pulse::session_ready(e.session))) {
            // The server went away or the device was removed: reopen soon.
            close_engine(e);
            publish(r, VoiceCaptureState::waiting, pulse::kErrorPulse);
            pulse::retry_after(&retry, now_us(), 500'000);
        }
        if (!e.track.stream && pulse::retry_due(retry, now_us())) {
            publish(r, VoiceCaptureState::opening);
            u32 error = 0;
            if (open_stream(r, e, &error))
                publish(r, VoiceCaptureState::capturing);
            else {
                publish(r, VoiceCaptureState::waiting, error);
                pulse::retry_after(&retry, now_us(), 1'000'000);
            }
        }
        Waitable events[]{r.stop, r.wake, r.timer};
        const u32 waited = wait_any(events, r.timer ? 3u : 2u, e.track.stream ? 200 : 250);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
            break;
        // Device changes only wake the loop: a source that follows the default is moved by the server, and one
        // pinned to a device that disappeared fails and is reopened above.
        if (e.session.loop)
            pulse::session_take_dirty(&e.session);
        if (e.track.stream && !drain(r, e)) {
            close_engine(e);
            publish(r, VoiceCaptureState::waiting, pulse::kErrorPulse);
            pulse::retry_after(&retry, now_us(), 500'000);
        }
        arm(r, r.tick(r.context, now_us()));
    }
    close_engine(e);
    publish(r, VoiceCaptureState::stopped);
    return 0;
}

void free_runtime(Runtime* r) noexcept {
    if (r->thread)
        thread_close(r->thread);
    const Waitable waitables[]{r->stop, r->wake, r->timer};
    for (Waitable waitable : waitables)
        if (waitable)
            waitable_close(waitable);
    pages_release(r, sizeof(Runtime));
}

struct Collect {
    Slice<VoiceAudioDevice> output;
    u32 count;
};

void collect(void* context, const pulse::DeviceInfo& device) noexcept {
    auto& list = *static_cast<Collect*>(context);
    // Monitors record a sink's output: not microphones, and not something to offer in a selector.
    if (list.count >= list.output.count || device.monitor)
        return;
    auto& entry = list.output[list.count];
    reset_value(&entry);
    if (!pulse::device_id_from_name(device.name, entry.id, kVoiceDeviceIdCharacters))
        return;
    u32 used = 0;
    while (device.description[used])
        ++used;
    if (used >= kVoiceAudioDeviceNameBytes) {
        used = kVoiceAudioDeviceNameBytes - 1;
        while (used && (static_cast<u8>(device.description[used]) & 0xc0) == 0x80)
            --used; // Keep whole UTF-8 sequences.
    }
    copy_bytes(entry.name, device.description, used);
    entry.name_bytes = used;
    ++list.count;
}
} // namespace

bool voice_audio_capture_start(VoiceAudioCapture* capture, const wchar_t* device_id, bool communications,
                               VoiceAudioCaptureSink sink, VoiceAudioCaptureTick tick, void* context) noexcept {
    (void)communications;
    if (!capture || capture->runtime || !sink || !tick)
        return false;
    auto* r = commit_pages<Runtime>();
    if (!r)
        return false;
    r->sink = sink;
    r->tick = tick;
    r->context = context;
    // An ID that is not a PulseAudio name (a Windows endpoint ID in shared settings) selects the default.
    if (!device_id || !pulse::device_name_from_id(device_id, r->device, sizeof(r->device)))
        r->device[0] = 0;
    r->stop = event_create(true, false);
    r->wake = event_create(false, false);
    r->timer = timer_create_waitable(false, true);
    r->state = static_cast<LONG>(VoiceCaptureState::opening);
    if (r->stop && r->wake)
        r->thread = thread_start(capture_thread, r, 256 * 1024, nullptr);
    if (!r->thread) {
        free_runtime(r);
        return false;
    }
    capture->runtime = r;
    return true;
}

void voice_audio_capture_stop(VoiceAudioCapture* capture) noexcept {
    if (!capture || !capture->runtime)
        return;
    auto* r = static_cast<Runtime*>(capture->runtime);
    event_set(r->stop);
    // A wedged audio server must not free memory the thread may still use.
    if (!thread_wait(r->thread, 5000))
        return;
    free_runtime(r);
    capture->runtime = nullptr;
}

bool voice_audio_capture_running(const VoiceAudioCapture& capture) noexcept {
    return capture.runtime != nullptr;
}

VoiceAudioCaptureStatus voice_audio_capture_status(const VoiceAudioCapture& capture) noexcept {
    VoiceAudioCaptureStatus status{};
    if (const auto* r = static_cast<const Runtime*>(capture.runtime)) {
        status.state = static_cast<VoiceCaptureState>(InterlockedCompareExchange(const_cast<volatile LONG*>(&r->state), 0, 0));
        status.native_error = static_cast<u32>(InterlockedCompareExchange(const_cast<volatile LONG*>(&r->native_error), 0, 0));
    }
    return status;
}

u32 voice_audio_devices(bool capture, Slice<VoiceAudioDevice> output) noexcept {
    if (!output.data || !output.count)
        return 0;
    const Waitable event = event_create(false, false);
    if (!event)
        return 0;
    const AudioFlow flow = capture ? AudioFlow::capture : AudioFlow::render;
    pulse::Session session{};
    Collect list{output, 0};
    char default_name[kDeviceNameBytes];
    // No sound server: no devices, and the selectors show only "Default".
    u32 error = 0;
    if (pulse::session_open(&session, event, "MaiCord", flow, false, nullptr, 2000, &error)) {
        if (pulse::device_list(&session, flow, nullptr, 2000, collect, &list, default_name, sizeof(default_name)))
            for (u32 i = 0; i < list.count; ++i) {
                wchar_t default_id[kVoiceDeviceIdCharacters];
                auto& entry = output[i];
                entry.is_default = pulse::device_id_from_name(default_name, default_id, kVoiceDeviceIdCharacters) &&
                                   voice_device_id_equal(entry.id, default_id);
                if (entry.is_default && i) {
                    // Default first.
                    const VoiceAudioDevice moved = entry;
                    for (u32 k = i; k > 0; --k)
                        copy_value(&output[k], output[k - 1]);
                    copy_value(&output[0], moved);
                }
            }
        else
            list.count = 0;
        pulse::session_close(&session);
    }
    waitable_close(event);
    return list.count;
}
} // namespace maicord
