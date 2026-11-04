// Notification sounds on Linux: short PCM16 stereo clips mixed on a worker and played through PulseAudio playback
// streams, one per source rate (44100 and 48000 Hz; the server converts to the device), the counterpart of the WASAPI
// worker in src/platform/audio/notification_audio.cpp. Clip validation, request preparation and the offline mixer are
// shared (notification_audio_mixer.cpp); this file is the worker and the control calls.
//
// A voice is finished when the stream has played past its end. The stream timeline (track_time) gives that while it
// advances; it stops short of the last frames by the device latency, so once every written frame belongs to a voice
// the stream is drained and the drain's completion (or its grace period: some sinks never answer) finishes the rest.
#include "../audio_support.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "pulse.hpp"
#include "timing.hpp"

namespace maicord {
namespace {
using audio_detail::add;
using audio_detail::fail;

constexpr u32 kServerTimeoutMs = 3000;
// How often the clock is read while a stream plays (a write request also reads it).
constexpr u32 kClockPollMs = 10;
constexpr u32 kChannels = 2;
constexpr u32 kFrameBytes = kChannels * sizeof(i16);
constexpr u32 kPulseFlags = pulse::kStreamStartCorked | pulse::kStreamInterpolateTiming |
                            pulse::kStreamAutoTimingUpdate | pulse::kStreamAdjustLatency;
constexpr u32 kRates[]{kNotificationAudioRate, kNotificationAudioAlternateRate};

struct Runtime {
    SRWLOCK lock{};
    NotificationAudioConfig config{};
    Thread* thread{};
    Waitable command{}, stop{}, render{};
    NotificationAudioRequest requests[kNotificationAudioQueueCapacity]{};
    NotificationAudioMixer mixer{};
    NotificationAudioStatus status{}, last_notified{};
    u32 head{}, count{}, reserved{};
    bool stopping{}, wake_pending{};
};
static_assert(sizeof(Runtime) <= 16 * 1024, "notification audio metadata remains bounded");
// One playback stream of one source rate.
struct Engine {
    pulse::Track track{};
    // Frames of this rate's timeline already played when the stream started.
    u64 anchor{};
    // The frames submitted when the pending drain was requested: all of them are played once it completes.
    u64 drain_target{};
    // When the pending drain stops being waited for (the monotonic millisecond clock).
    u64 drain_deadline_ms{};
    bool started{}, drain_requested{};
};
// What the worker owns besides the runtime: one connection and the two streams.
struct Transport {
    pulse::Session session{};
    // Read-the-clock timer, armed while a stream plays; created by the worker and not counted as an attached handle.
    Waitable poll{};
    Engine engines[2]{};
    i16 scratch[kNotificationAudioMixChunkFrames * kChannels]{};
};

u64 submitted_of(const NotificationAudioMixer& m, u32 rate) noexcept {
    return rate == kNotificationAudioRate ? m.submitted : m.submitted_48000;
}
u64 consumed_of(const NotificationAudioMixer& m, u32 rate) noexcept {
    return rate == kNotificationAudioRate ? m.consumed : m.consumed_48000;
}

void notify_locked(Runtime& r) noexcept {
    if (bytes_equal(&r.status, &r.last_notified, sizeof(r.status)))
        return;
    if (!r.wake_pending && r.config.wake.window) {
        r.wake_pending = window_post(r.config.wake, 0, 0);
        if (!r.wake_pending)
            r.status.wake_error = thread_last_error();
    }
    r.last_notified = r.status;
}
void publish(Runtime& r) noexcept {
    AcquireSRWLockExclusive(&r.lock);
    r.status.queued = r.count;
    r.status.outstanding = r.reserved;
    r.status.active = r.mixer.count;
    notify_locked(r);
    ReleaseSRWLockExclusive(&r.lock);
}

void close_stream(Engine& e) noexcept {
    pulse::track_close(&e.track);
    e.started = false;
    e.drain_requested = false;
}
void close_streams(Transport& t) noexcept {
    for (auto& e : t.engines)
        close_stream(e);
}
void close_transport(Transport& t) noexcept {
    close_streams(t);
    pulse::session_close(&t.session);
    if (t.poll)
        waitable_close(t.poll);
    t.poll = nullptr;
}

bool current(Runtime& r, u64 epoch) noexcept {
    AcquireSRWLockShared(&r.lock);
    const bool same = !r.stopping && r.status.enabled && r.status.desired_epoch == epoch;
    ReleaseSRWLockShared(&r.lock);
    return same;
}

// Opens the stream of one rate on the default sink (which the server keeps following); returns 0 or the native error.
u32 open_engine(Runtime& r, Transport& t, Engine& e, u32 rate) noexcept {
    u32 error = 0;
    if (t.session.loop && !pulse::session_ready(t.session))
        pulse::session_close(&t.session);
    if (!pulse::session_ready(t.session)) {
        AcquireSRWLockExclusive(&r.lock);
        add(r.status.device_open_attempts);
        ReleaseSRWLockExclusive(&r.lock);
        if (!pulse::session_open(&t.session, r.render, "MaiCord", AudioFlow::render, true, r.stop, kServerTimeoutMs,
                                 &error))
            return error;
    }
    AcquireSRWLockExclusive(&r.lock);
    add(r.status.stream_open_attempts);
    // Whatever an earlier stream left submitted but unplayed is mixed again.
    notification_audio_mixer_rewind_rate(&r.mixer, rate);
    e.anchor = consumed_of(r.mixer, rate);
    ReleaseSRWLockExclusive(&r.lock);
    const pulse::TrackConfig config{
        "MaiCord notification",
        "event",
        {pulse::kSampleS16LE, rate, kChannels},
        nullptr,
        pulse::playback_attributes(rate, kFrameBytes, pulse::kNotificationBufferMs, pulse::kVoicePeriodMs),
        kPulseFlags,
        // The clip gain is the mixer's; the server's stream-restore database must not change it.
        1.0f};
    if (!pulse::track_open_playback(&t.session, &e.track, config, &error) ||
        !pulse::track_wait_ready(&e.track, r.stop, kServerTimeoutMs, &error)) {
        pulse::track_close(&e.track);
        return error;
    }
    return 0;
}

void retire_played(Runtime& r, u32 padding, u32 rate) noexcept {
    const u32 n = notification_audio_mixer_played_rate(&r.mixer, padding, rate);
    if (!n)
        return;
    AcquireSRWLockExclusive(&r.lock);
    r.reserved -= n;
    add(r.status.completed, n);
    ReleaseSRWLockExclusive(&r.lock);
}

void failed_audio(Runtime& r, Transport& t, NotificationAudioError error, u32 native) noexcept {
    close_streams(t);
    // A failure of an old stream must not discard accepted new-epoch commands that arrived while it was closing.
    AcquireSRWLockExclusive(&r.lock);
    if (r.mixer.epoch && r.mixer.epoch != r.status.desired_epoch) {
        const u32 old = r.mixer.count;
        r.reserved -= old;
        add(r.status.canceled, old);
        notification_audio_mixer_reset(&r.mixer, r.status.desired_epoch);
        r.status.observed_epoch = r.status.desired_epoch;
        r.status.stream_open = false;
        notify_locked(r);
        ReleaseSRWLockExclusive(&r.lock);
        return;
    }
    add(r.status.failed, r.reserved);
    clear_bytes(r.requests, sizeof(r.requests));
    r.head = r.count = r.reserved = 0;
    r.status.error = error;
    r.status.native_error = native;
    r.status.stream_open = false;
    notification_audio_mixer_reset(&r.mixer, r.status.desired_epoch);
    r.status.observed_epoch = r.status.desired_epoch;
    notify_locked(r);
    ReleaseSRWLockExclusive(&r.lock);
}

void admit_requests(Runtime& r) noexcept {
    for (u32 i = 0; i < kNotificationAudioQueueCapacity; ++i) {
        NotificationAudioRequest request{};
        AcquireSRWLockExclusive(&r.lock);
        if (!r.count || r.stopping || r.status.desired_epoch != r.mixer.epoch) {
            ReleaseSRWLockExclusive(&r.lock);
            break;
        }
        request = r.requests[r.head];
        reset_value(&r.requests[r.head]);
        r.head = (r.head + 1) % kNotificationAudioQueueCapacity;
        --r.count;
        ReleaseSRWLockExclusive(&r.lock);
        NotificationAudioClip clip{};
        float gain{};
        const bool live = current(r, request.epoch);
        const auto prepared = live ? notification_audio_prepare_request(r.config, request, &clip, &gain)
                                   : fail(NotificationAudioError::stale_epoch);
        NotificationAudioResult admitted = fail(NotificationAudioError::stale_epoch);
        if (prepared.ok() && current(r, request.epoch))
            admitted = notification_audio_mixer_add(&r.mixer, request.epoch, clip, gain);
        if (!admitted.ok()) {
            AcquireSRWLockExclusive(&r.lock);
            --r.reserved;
            if (!live || r.status.desired_epoch != request.epoch || !r.status.enabled)
                add(r.status.canceled);
            else {
                add(r.status.failed);
                r.status.error = prepared.ok() ? admitted.error : prepared.error;
                r.status.native_error = 0;
            }
            ReleaseSRWLockExclusive(&r.lock);
        }
    }
}

// Retires the voices the stream has played, queues what the mixer still has for the stream and starts it; returns 0
// or the native error that makes the stream unusable.
u32 render_audio(Runtime& r, Transport& t, Engine& e, u32 rate) noexcept {
    if (!pulse::track_alive(e.track))
        return pulse::kErrorPulse;
    const u64 submitted = submitted_of(r.mixer, rate), consumed = consumed_of(r.mixer, rate);
    u64 played = consumed;
    if (pulse::drain_finished(e.drain_requested, InterlockedCompareExchange(&e.track.drained, 0, 0) != 0,
                              e.drain_deadline_ms, clock_monotonic_milliseconds())) {
        // The drain is over: every frame submitted before it was requested has been heard.
        played = e.drain_target;
        e.drain_requested = false;
    } else {
        u64 microseconds = 0;
        if (e.started && pulse::track_time(&e.track, &microseconds))
            played = pulse::played_frames(e.anchor, microseconds, rate, submitted, consumed);
    }
    if (played > consumed)
        retire_played(r, static_cast<u32>(minimum<u64>(submitted - played, ~u32{0})), rate);
    const u32 needed = notification_audio_mixer_needed_rate(r.mixer, rate);
    const u32 frames = minimum(needed, pulse::request_frames(pulse::track_writable(&e.track), kFrameBytes,
                                                             pulse::frames_in_milliseconds(
                                                                 rate, pulse::kNotificationBufferMs)));
    const u64 epoch = r.mixer.epoch;
    bool wrote = false, canceled = false;
    for (u32 offset = 0; offset < frames;) {
        if (!current(r, epoch)) {
            canceled = true;
            break;
        }
        const u32 n = minimum(kNotificationAudioMixChunkFrames, frames - offset);
        const auto mixed = notification_audio_mixer_render_rate(&r.mixer, {t.scratch, n * kChannels}, rate);
        if (mixed.error != NotificationAudioError::none || mixed.frames != n)
            return pulse::kErrorPulse;
        if (!pulse::track_write(&e.track, t.scratch, n * kFrameBytes))
            return pulse::kErrorPulse;
        wrote = true;
        offset += n;
    }
    if (canceled || !current(r, epoch))
        return 0;
    if (wrote && !e.started) {
        if (!pulse::track_cork(&e.track, false))
            return pulse::kErrorPulse;
        e.started = true;
    }
    // Everything the mixer holds for this rate is queued: ask for its end to be confirmed.
    if (e.started && !e.drain_requested && frames == needed &&
        notification_audio_mixer_needed_rate(r.mixer, rate) == 0 &&
        notification_audio_mixer_voices_rate(r.mixer, rate) && submitted_of(r.mixer, rate) > consumed_of(r.mixer, rate)) {
        e.drain_target = submitted_of(r.mixer, rate);
        e.drain_requested = pulse::track_drain(&e.track);
        e.drain_deadline_ms = clock_monotonic_milliseconds() + pulse::kDrainGraceMs;
    }
    return 0;
}

u32 worker_main(void* pointer) noexcept {
    auto& r = *static_cast<Runtime*>(pointer);
    Transport t{};
    t.poll = timer_create_waitable(false, true);
    thread_boost_realtime();
    for (;;) {
        AcquireSRWLockShared(&r.lock);
        const bool stopping = r.stopping;
        const u64 desired = r.status.desired_epoch;
        ReleaseSRWLockShared(&r.lock);
        if (stopping)
            break;
        if (r.mixer.epoch != desired) {
            const u32 old = r.mixer.count;
            close_streams(t);
            notification_audio_mixer_reset(&r.mixer, desired);
            AcquireSRWLockExclusive(&r.lock);
            r.reserved -= old;
            add(r.status.canceled, old);
            r.status.observed_epoch = desired;
            r.status.stream_open = false;
            ReleaseSRWLockExclusive(&r.lock);
        }
        // A change of the default sink needs no work: the server moves streams that follow it. It is counted.
        if (t.session.loop && (pulse::session_take_dirty(&t.session) & kAudioEndpointDefaultChanged))
            for (auto& e : t.engines)
                if (e.track.stream) {
                    AcquireSRWLockExclusive(&r.lock);
                    add(r.status.switches);
                    ReleaseSRWLockExclusive(&r.lock);
                }
        admit_requests(r);
        AcquireSRWLockShared(&r.lock);
        const bool epoch_changed = r.status.desired_epoch != r.mixer.epoch || r.stopping;
        ReleaseSRWLockShared(&r.lock);
        if (epoch_changed) {
            publish(r);
            continue;
        }
        for (u32 index = 0; index < 2; ++index) {
            Engine& e = t.engines[index];
            const u32 rate = kRates[index];
            if (notification_audio_mixer_voices_rate(r.mixer, rate) && !e.track.stream) {
                const u32 error = open_engine(r, t, e, rate);
                if (error)
                    failed_audio(r, t, NotificationAudioError::device, error);
            }
            if (e.track.stream) {
                const u32 error = pulse::session_ready(t.session) ? render_audio(r, t, e, rate) : pulse::kErrorPulse;
                if (error)
                    failed_audio(r, t, NotificationAudioError::stream, error);
                else if (!notification_audio_mixer_voices_rate(r.mixer, rate))
                    close_stream(e);
            }
        }
        const bool open = t.engines[0].track.stream || t.engines[1].track.stream;
        const bool started = t.engines[0].started || t.engines[1].started;
        AcquireSRWLockExclusive(&r.lock);
        r.status.stream_open = open;
        ReleaseSRWLockExclusive(&r.lock);
        publish(r);
        // The clock is read between write requests too, while audio plays.
        if (started && t.poll)
            timer_arm(t.poll, kClockPollMs, 0);
        const Waitable events[]{r.stop, r.command, r.render, t.poll};
        const u32 waited = wait_any(events, started && t.poll ? 4u : open ? 3u : 2u, INFINITE);
        if (waited == WAIT_OBJECT_0)
            break;
        if (waited == WAIT_FAILED) {
            failed_audio(r, t, NotificationAudioError::wait, thread_last_error());
            break;
        }
    }
    close_transport(t);
    AcquireSRWLockExclusive(&r.lock);
    add(r.status.canceled, r.reserved);
    r.reserved = r.count = r.head = 0;
    clear_bytes(r.requests, sizeof(r.requests));
    notification_audio_mixer_reset(&r.mixer, r.status.desired_epoch);
    r.status.queued = r.status.outstanding = r.status.active = 0;
    r.status.stream_open = false;
    r.status.observed_epoch = r.status.desired_epoch;
    r.status.worker_exited = true;
    notify_locked(r);
    ReleaseSRWLockExclusive(&r.lock);
    return 0;
}

void free_runtime(Runtime* r) noexcept {
    if (r->thread)
        thread_close(r->thread);
    const Waitable waitables[]{r->command, r->stop, r->render};
    for (Waitable waitable : waitables)
        if (waitable)
            waitable_close(waitable);
    pages_release(r, sizeof(Runtime));
}

NotificationAudioResult launch(NotificationAudio& owner) noexcept {
    auto* r = commit_pages<Runtime>();
    if (!r)
        return fail(NotificationAudioError::memory, thread_last_error());
    // Pages are zeroed; every queue and voice slot is written in full on insertion.
    r->config = owner.config;
    r->status.desired_epoch = owner.epoch;
    r->status.enabled = owner.enabled;
    r->command = event_create(false, false);
    r->stop = event_create(true, false);
    r->render = event_create(false, false);
    if (!r->command || !r->stop || !r->render) {
        const u32 error = thread_last_error();
        free_runtime(r);
        return fail(NotificationAudioError::event, error);
    }
    u32 error = 0;
    r->thread = thread_start(worker_main, r, 256 * 1024, &error);
    if (!r->thread) {
        free_runtime(r);
        return fail(NotificationAudioError::thread, error);
    }
    owner.runtime = r;
    return {};
}
} // namespace

NotificationAudioResult notification_audio_initialize(NotificationAudio* owner,
                                                      const NotificationAudioConfig& config) noexcept {
    if (!owner || owner->initialized || owner->runtime || !config.initial_epoch || config.wake.cause > wake_tray ||
        (!config.wake.window && config.wake.cause))
        return fail(NotificationAudioError::invalid_input);
    *owner = {config, nullptr, config.initial_epoch, true, config.enabled};
    return {};
}
bool notification_audio_can_accept(const NotificationAudio* owner, u32 needed) noexcept {
    if (!owner || !owner->initialized || !owner->enabled || !needed || needed > kNotificationAudioQueueCapacity)
        return false;
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (!r)
        return true;
    AcquireSRWLockShared(&r->lock);
    const bool available = !r->stopping && !r->status.worker_exited && r->status.enabled &&
                           needed <= kNotificationAudioQueueCapacity - r->count &&
                           needed <= kNotificationAudioVoiceCapacity - r->reserved;
    ReleaseSRWLockShared(&r->lock);
    return available;
}
NotificationAudioResult notification_audio_submit(NotificationAudio* owner,
                                                  const NotificationAudioRequest& request) noexcept {
    if (!owner || !owner->initialized || !notification_audio_gain_valid(request.gain) ||
        (request.clip.samples.count ? !notification_audio_clip_valid(request.clip)
                                    : !owner->config.resolve || !request.clip_id))
        return fail(NotificationAudioError::invalid_input);
    if (request.epoch != owner->epoch)
        return fail(NotificationAudioError::stale_epoch);
    if (!owner->enabled)
        return fail(NotificationAudioError::stopped);
    if (!owner->runtime) {
        const auto result = launch(*owner);
        if (!result.ok())
            return result;
    }
    auto& r = *static_cast<Runtime*>(owner->runtime);
    AcquireSRWLockExclusive(&r.lock);
    NotificationAudioResult result{};
    if (r.stopping || r.status.worker_exited)
        result = fail(NotificationAudioError::stopped);
    else if (r.count == kNotificationAudioQueueCapacity)
        result = fail(NotificationAudioError::full_queue);
    else if (r.reserved == kNotificationAudioVoiceCapacity)
        result = fail(NotificationAudioError::full_voices);
    else {
        r.requests[(r.head + r.count) % kNotificationAudioQueueCapacity] = request;
        ++r.count;
        ++r.reserved;
        if (!event_set(r.command)) {
            --r.count;
            --r.reserved;
            reset_value(&r.requests[(r.head + r.count) % kNotificationAudioQueueCapacity]);
            result = fail(NotificationAudioError::event, thread_last_error());
        } else {
            add(r.status.accepted);
            notify_locked(r);
        }
    }
    ReleaseSRWLockExclusive(&r.lock);
    return result;
}
NotificationAudioResult notification_audio_reset(NotificationAudio* owner, u64 epoch, bool enabled) noexcept {
    if (!owner || !owner->initialized || !epoch)
        return fail(NotificationAudioError::invalid_input);
    if (epoch <= owner->epoch)
        return fail(NotificationAudioError::stale_epoch);
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (r) {
        AcquireSRWLockExclusive(&r->lock);
        if (r->stopping || r->status.worker_exited) {
            ReleaseSRWLockExclusive(&r->lock);
            return fail(NotificationAudioError::stopped);
        }
        add(r->status.canceled, r->count);
        r->reserved -= r->count;
        r->count = r->head = 0;
        clear_bytes(r->requests, sizeof(r->requests));
        r->status.desired_epoch = epoch;
        r->status.enabled = enabled;
        r->status.error = NotificationAudioError::none;
        r->status.native_error = 0;
        owner->epoch = epoch;
        owner->enabled = enabled;
        const bool signal = event_set(r->command);
        const u32 error = signal ? 0 : thread_last_error();
        notify_locked(*r);
        ReleaseSRWLockExclusive(&r->lock);
        return signal ? NotificationAudioResult{} : fail(NotificationAudioError::event, error);
    }
    owner->epoch = epoch;
    owner->enabled = enabled;
    return {};
}
NotificationAudioStatus notification_audio_status(const NotificationAudio* owner) noexcept {
    if (!owner || !owner->initialized)
        return {};
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (!r) {
        NotificationAudioStatus status{};
        status.desired_epoch = status.observed_epoch = owner->epoch;
        status.enabled = owner->enabled;
        return status;
    }
    AcquireSRWLockExclusive(&r->lock);
    auto status = r->status;
    status.queued = r->count;
    status.outstanding = r->reserved;
    status.worker_started = true;
    status.stopping = r->stopping;
    r->wake_pending = false;
    r->last_notified = r->status;
    ReleaseSRWLockExclusive(&r->lock);
    return status;
}
NotificationAudioResult notification_audio_release(NotificationAudio* owner, u32 timeout) noexcept {
    if (!owner)
        return fail(NotificationAudioError::invalid_input);
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (!r) {
        reset_value(owner);
        return {};
    }
    AcquireSRWLockExclusive(&r->lock);
    r->stopping = true;
    r->status.enabled = false;
    owner->enabled = false;
    const bool signaled = event_set(r->stop);
    const u32 signal_error = signaled ? 0 : thread_last_error();
    ReleaseSRWLockExclusive(&r->lock);
    if (!signaled)
        return fail(NotificationAudioError::event, signal_error);
    if (!thread_wait(r->thread, timeout))
        return fail(NotificationAudioError::join_pending, ERROR_TIMEOUT);
    free_runtime(r);
    reset_value(owner);
    return {};
}
u32 notification_audio_owned_handles(const NotificationAudio* owner) noexcept {
    auto* r = owner ? static_cast<Runtime*>(owner->runtime) : nullptr;
    if (!r)
        return 0;
    AcquireSRWLockShared(&r->lock);
    const u32 n = u32(r->thread != nullptr) + u32(r->command != nullptr) + u32(r->stop != nullptr) +
                  u32(r->render != nullptr);
    ReleaseSRWLockShared(&r->lock);
    return n;
}
} // namespace maicord
