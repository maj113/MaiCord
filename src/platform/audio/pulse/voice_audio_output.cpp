// Voice output on Linux: a PulseAudio playback stream of 48 kHz stereo PCM16 on its own thread, the counterpart of
// the WASAPI render loop in src/platform/audio/voice_audio_output.cpp.
#include "maicord/platform/audio/voice_audio_output.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "pulse.hpp"
#include "timing.hpp"

namespace maicord {
namespace {
constexpr u32 kRate = pulse::kVoiceRate, kChannels = 2, kChunkFrames = 2048;
constexpr u32 kFrameBytes = kChannels * sizeof(i16);
// The server is asked for at most the render buffer at once; after a stall the missed audio is dropped, not queued.
constexpr u32 kFillLimitFrames = pulse::frames_in_milliseconds(kRate, pulse::kRenderBufferMs);
constexpr u32 kServerTimeoutMs = 3000;
constexpr usize kDeviceNameBytes = 128;

struct Runtime {
    VoiceAudioRender render;
    void* context;
    Thread* thread;
    Waitable stop, wake;
    // The sink name; empty selects the server's default.
    char device[kDeviceNameBytes];
};
struct Engine {
    pulse::Session session;
    pulse::Track track;
};

void close_engine(Engine& e) noexcept {
    pulse::track_close(&e.track);
    pulse::session_close(&e.session);
}

bool fill(Runtime& r, Engine& e) noexcept {
    const u32 frames = pulse::request_frames(pulse::track_writable(&e.track), kFrameBytes, kFillLimitFrames);
    if (!frames)
        return pulse::track_alive(e.track);
    const u64 now = clock_microseconds(clock_initialize(), 0, clock_now());
    i16 pcm[kChunkFrames * kChannels];
    for (u32 at = 0; at < frames;) {
        const u32 chunk = minimum(kChunkFrames, frames - at);
        r.render(r.context, {pcm, chunk * kChannels}, now);
        if (!pulse::track_write(&e.track, pcm, chunk * kFrameBytes))
            return false;
        at += chunk;
    }
    return true;
}

bool open_stream(Runtime& r, Engine& e) noexcept {
    u32 error = 0;
    if (!pulse::session_ready(e.session) &&
        !pulse::session_open(&e.session, r.wake, "MaiCord", AudioFlow::render, true, r.stop, kServerTimeoutMs, &error))
        return false;
    // A missing chosen device falls back to the server's default.
    const char* device = r.device[0] && pulse::device_exists(&e.session, AudioFlow::render, r.device, r.stop)
                             ? r.device
                             : nullptr;
    const pulse::TrackConfig config{
        "MaiCord voice output",
        "phone",
        {pulse::kSampleS16LE, kRate, kChannels},
        device,
        pulse::playback_attributes(kRate, kFrameBytes, pulse::kRenderBufferMs, pulse::kVoicePeriodMs),
        pulse::kStreamStartCorked | pulse::kStreamInterpolateTiming | pulse::kStreamAutoTimingUpdate |
            pulse::kStreamAdjustLatency,
        // Gain is the voice engine's; the server's stream-restore database must not change it.
        1.0f};
    // Filled once before the stream starts, as a WASAPI client is, so playback begins with audio queued.
    if (!pulse::track_open_playback(&e.session, &e.track, config, &error) ||
        !pulse::track_wait_ready(&e.track, r.stop, kServerTimeoutMs, &error) || !fill(r, e) ||
        !pulse::track_cork(&e.track, false)) {
        pulse::track_close(&e.track);
        return false;
    }
    return true;
}

// Waits on stop and endpoint notifications; true when stopping.
bool pause(Runtime& r, u32 milliseconds) noexcept {
    Waitable events[]{r.stop, r.wake};
    return wait_any(events, 2, milliseconds) == WAIT_OBJECT_0;
}

u32 audio_thread(void* parameter) noexcept {
    auto& r = *static_cast<Runtime*>(parameter);
    thread_boost_realtime();
    Engine e{};
    for (;;) {
        if (!pulse::track_alive(e.track) || !pulse::session_ready(e.session)) {
            // The server went away, the device was removed or the stream was never open: start over.
            close_engine(e);
            if (!open_stream(r, e)) {
                if (pause(r, 1000))
                    break;
                continue;
            }
        }
        Waitable events[]{r.stop, r.wake};
        const u32 waited = wait_any(events, 2, 200);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
            break;
        // Device changes only wake the loop: a stream that follows the default is moved by the server, and one
        // pinned to a device that disappeared fails and is reopened above.
        pulse::session_take_dirty(&e.session);
        if (!fill(r, e)) {
            close_engine(e);
            if (pause(r, 500))
                break;
        }
    }
    close_engine(e);
    return 0;
}

void free_runtime(Runtime* r) noexcept {
    if (r->thread)
        thread_close(r->thread);
    const Waitable waitables[]{r->stop, r->wake};
    for (Waitable waitable : waitables)
        if (waitable)
            waitable_close(waitable);
    pages_release(r, sizeof(Runtime));
}
} // namespace

bool voice_audio_output_start_device(VoiceAudioOutput* output, const wchar_t* device_id, VoiceAudioRender render,
                                     void* context) noexcept {
    if (!output || output->runtime || !render)
        return false;
    auto* r = commit_pages<Runtime>();
    if (!r)
        return false;
    r->render = render;
    r->context = context;
    // An ID that is not a PulseAudio name (a Windows endpoint ID in shared settings) selects the default.
    if (!device_id || !pulse::device_name_from_id(device_id, r->device, sizeof(r->device)))
        r->device[0] = 0;
    r->stop = event_create(true, false);
    r->wake = event_create(false, false);
    if (r->stop && r->wake)
        r->thread = thread_start(audio_thread, r, 256 * 1024, nullptr);
    if (!r->thread) {
        free_runtime(r);
        return false;
    }
    output->runtime = r;
    return true;
}

void voice_audio_output_stop(VoiceAudioOutput* output) noexcept {
    if (!output || !output->runtime)
        return;
    auto* r = static_cast<Runtime*>(output->runtime);
    event_set(r->stop);
    // A wedged audio server must not free memory the thread may still use.
    if (!thread_wait(r->thread, 5000))
        return;
    free_runtime(r);
    output->runtime = nullptr;
}

bool voice_audio_output_running(const VoiceAudioOutput& output) noexcept {
    return output.runtime != nullptr;
}
} // namespace maicord
