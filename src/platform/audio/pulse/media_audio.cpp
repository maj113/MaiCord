// Media playback on Linux: float32 PCM from the caller's ring through one PulseAudio playback stream at the
// source rate, the counterpart of the WASAPI worker in src/platform/audio/media_audio.cpp. The ring, the gain rule
// and the clock conversions are shared (media_audio_ring.cpp); this file is the worker and the control calls.
//
// The audible position is the stream's own timeline (track_time): it advances only while the stream plays queued
// audio, so a producer that falls behind or a pause leaves the clock where it was, with no re-anchoring. Playback
// that has reached the end of the ring is confirmed by a drain (or its grace period: some sinks never answer), since
// the timeline stops short of the last frames by the device latency.
#include "../audio_support.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/os/thread.hpp"
#include "pulse.hpp"
#include "maicord/core/time.hpp"
#include "timing.hpp"

namespace maicord {
namespace {
using Error = MediaAudioError;
using audio_detail::disjoint;
using audio_detail::fail;

constexpr u32 kServerTimeoutMs = 3000;
// How often the clock is read while a stream plays (a write request also reads it).
constexpr u32 kClockPollMs = 10;
constexpr u32 kPulseFlags = pulse::kStreamStartCorked | pulse::kStreamInterpolateTiming |
                            pulse::kStreamAutoTimingUpdate | pulse::kStreamAdjustLatency;

struct Runtime {
    SRWLOCK lock{};
    MediaAudioConfig config{};
    MediaAudioRing ring{};
    MediaAudioStatus status{}, last_notified{};
    Thread* thread{};
    Waitable command{}, stop{}, render{};
    u64 observed{}, command_serial{};
    i64 origin{};
    float volume{};
    bool desired_play{}, stopping{}, failed{}, wake_pending{}, refill_pending{};
};
struct Engine {
    pulse::Session session{};
    pulse::Track track{};
    // Frames of the source timeline already played when the stream started.
    u64 anchor_frames{};
    float applied_volume{-1};
    bool started{}, drain_requested{};
    // When the pending drain stops being waited for (the monotonic millisecond clock).
    u64 drain_deadline_ms{};
    float scratch[kMediaAudioMaximumWriteFrames * 2]{};
};
// Whether the drain asked for at the end of the ring is over: confirmed, or past its grace period.
bool drain_over(const Engine& e) noexcept {
    return pulse::drain_finished(e.drain_requested,
                                 InterlockedCompareExchange(const_cast<volatile LONG*>(&e.track.drained), 0, 0) != 0,
                                 e.drain_deadline_ms, clock_monotonic_milliseconds());
}

void notify_locked(Runtime& r) noexcept {
    r.status.written_frames = r.ring.written;
    r.status.submitted_frames = r.ring.submitted;
    r.status.consumed_frames = r.ring.consumed;
    r.status.free_frames = media_audio_ring_free(r.ring);
    r.status.observed_epoch = r.observed;
    r.status.owned_handles = u32(r.thread != nullptr) + u32(r.command != nullptr) + u32(r.stop != nullptr) +
                             u32(r.render != nullptr);
    const auto& a = r.status;
    const auto& b = r.last_notified;
    const bool changed = a.desired_epoch != b.desired_epoch || a.observed_epoch != b.observed_epoch ||
                         a.error != b.error || a.native_error != b.native_error || a.playing != b.playing ||
                         a.paused != b.paused || a.buffering != b.buffering || a.ended != b.ended ||
                         a.stream_open != b.stream_open || a.worker_started != b.worker_started ||
                         a.worker_exited != b.worker_exited || a.clock_valid != b.clock_valid ||
                         a.desired_volume != b.desired_volume || a.applied_volume != b.applied_volume ||
                         a.volume_valid != b.volume_valid;
    if (!changed)
        return;
    if (r.config.status_event && !event_set(r.config.status_event))
        r.status.wake_error = thread_last_error();
    if (!r.wake_pending && r.config.wake.window) {
        r.wake_pending = window_post(r.config.wake, 0, 0);
        if (!r.wake_pending)
            r.status.wake_error = thread_last_error();
    }
    r.last_notified = r.status;
}

void close_track(Engine& e) noexcept {
    pulse::track_close(&e.track);
    e.started = false;
    e.drain_requested = false;
    e.applied_volume = -1;
}

void failure(Runtime& r, Engine& e, u64 epoch, u64 serial, Error error, u32 native) noexcept {
    close_track(e);
    AcquireSRWLockExclusive(&r.lock);
    if (r.ring.epoch == epoch)
        media_audio_ring_rewind(&r.ring);
    if (r.status.desired_epoch == epoch && r.command_serial == serial) {
        r.failed = true;
        r.status.error = error;
        r.status.native_error = native;
        r.status.playing = false;
        r.status.paused = false;
        r.status.stream_open = false;
        r.status.clock_valid = r.status.volume_valid = false;
        notify_locked(r);
    }
    ReleaseSRWLockExclusive(&r.lock);
}

// Opens the stream on the default sink (which the server keeps following) and returns 0 or the native error.
u32 open(Runtime& r, Engine& e) noexcept {
    u32 error = 0;
    if (e.session.loop && !pulse::session_ready(e.session))
        pulse::session_close(&e.session);
    if (!pulse::session_ready(e.session) &&
        !pulse::session_open(&e.session, r.render, "MaiCord", AudioFlow::render, true, r.stop, kServerTimeoutMs,
                             &error))
        return error;
    AcquireSRWLockExclusive(&r.lock);
    // Whatever an earlier stream left submitted but unplayed is submitted again.
    media_audio_ring_rewind(&r.ring);
    e.anchor_frames = r.ring.consumed;
    const float volume = r.volume;
    ReleaseSRWLockExclusive(&r.lock);
    const u32 bytes_per_frame = pulse::frame_bytes(pulse::kSampleFloat32LE, r.config.channels);
    const pulse::TrackConfig config{
        "MaiCord media",
        "music",
        {pulse::kSampleFloat32LE, r.config.sample_rate, static_cast<u8>(r.config.channels)},
        nullptr,
        pulse::playback_attributes(r.config.sample_rate, bytes_per_frame, pulse::kMediaBufferMs,
                                   pulse::kVoicePeriodMs),
        kPulseFlags,
        volume};
    if (!pulse::track_open_playback(&e.session, &e.track, config, &error) ||
        !pulse::track_wait_ready(&e.track, r.stop, kServerTimeoutMs, &error)) {
        pulse::track_close(&e.track);
        return error;
    }
    e.applied_volume = volume;
    return 0;
}

// Moves ring.consumed to what the stream has played and publishes the presentation time.
void clock_snapshot(Runtime& r, Engine& e, u64 epoch) noexcept {
    u64 microseconds = 0;
    const bool known = e.track.stream && pulse::track_time(&e.track, &microseconds);
    const bool drained = drain_over(e);
    if (!known && !drained)
        return;
    const u32 rate = r.config.sample_rate;
    u64 anchor_time{}, elapsed{}, submitted_time{};
    if (!media_audio_clock_time(e.anchor_frames, rate, &anchor_time) ||
        !media_audio_clock_time(microseconds, 1'000'000, &elapsed))
        return;
    AcquireSRWLockExclusive(&r.lock);
    if (r.status.desired_epoch != epoch || r.ring.epoch != epoch ||
        !media_audio_clock_time(r.ring.submitted, rate, &submitted_time) || elapsed > ~u64{0} - anchor_time) {
        ReleaseSRWLockExclusive(&r.lock);
        return;
    }
    const u64 content_time = drained ? submitted_time : minimum(anchor_time + elapsed, submitted_time);
    if (content_time > static_cast<u64>(0x7fffffffffffffffll) ||
        r.origin > 0x7fffffffffffffffll - static_cast<i64>(content_time)) {
        ReleaseSRWLockExclusive(&r.lock);
        return;
    }
    const u64 played = drained ? r.ring.submitted
                               : pulse::played_frames(e.anchor_frames, microseconds, rate, r.ring.submitted,
                                                      r.ring.consumed);
    const u64 old = r.ring.consumed;
    r.ring.consumed = maximum(old, played);
    r.status.clock_valid = true;
    r.status.clock_frequency = 1'000'000;
    r.status.presentation_time_100ns = r.origin + static_cast<i64>(content_time);
    if (r.ring.consumed > old && media_audio_ring_free(r.ring) >= kMediaAudioMaximumWriteFrames &&
        !r.refill_pending && r.config.refill_event) {
        r.refill_pending = event_set(r.config.refill_event);
        if (!r.refill_pending)
            r.status.wake_error = thread_last_error();
    }
    notify_locked(r);
    ReleaseSRWLockExclusive(&r.lock);
}

// Fills the stream from the ring; returns 0 or the native error that makes the stream unusable.
u32 render(Runtime& r, Engine& e, u64 epoch, bool playing) noexcept {
    clock_snapshot(r, e, epoch);
    if (!pulse::track_alive(e.track))
        return pulse::kErrorPulse;
    if (e.started && drain_over(e)) {
        // Everything written has been heard: the stream stops, as a WASAPI client does when its queue is empty.
        if (!pulse::track_cork(&e.track, true))
            return pulse::kErrorPulse;
        e.started = false;
    }
    if (!playing) {
        if (e.started) {
            if (!pulse::track_cork(&e.track, true))
                return pulse::kErrorPulse;
            e.started = false;
        }
        return 0;
    }
    const u32 channels = r.config.channels;
    const u32 bytes_per_frame = pulse::frame_bytes(pulse::kSampleFloat32LE, channels);
    AcquireSRWLockShared(&r.lock);
    const bool live = r.status.desired_epoch == epoch && !r.stopping && r.desired_play;
    const u32 pending = live ? static_cast<u32>(minimum<u64>(r.ring.written - r.ring.submitted, ~u32{0})) : 0;
    ReleaseSRWLockShared(&r.lock);
    if (!live)
        return 0;
    const u32 frames = minimum(pending, pulse::request_frames(pulse::track_writable(&e.track), bytes_per_frame,
                                                              pulse::frames_in_milliseconds(r.config.sample_rate,
                                                                                            pulse::kMediaBufferMs)));
    bool valid = true;
    for (u32 at = 0; valid && at < frames;) {
        const u32 chunk = minimum(kMediaAudioMaximumWriteFrames, frames - at);
        AcquireSRWLockExclusive(&r.lock);
        valid = !r.stopping && r.desired_play && r.status.desired_epoch == epoch && r.ring.epoch == epoch;
        if (valid)
            valid = media_audio_ring_render(&r.ring, {e.scratch, chunk * channels}, 1) == chunk;
        ReleaseSRWLockExclusive(&r.lock);
        if (!valid)
            break;
        if (!pulse::track_write(&e.track, e.scratch, chunk * bytes_per_frame))
            return pulse::kErrorPulse;
        at += chunk;
    }
    if (!valid) {
        // A pause or reset arrived while frames were being queued. They cannot be taken back, so the stream is
        // dropped and the retained PCM is played again from the confirmed position.
        close_track(e);
        AcquireSRWLockExclusive(&r.lock);
        if (r.ring.epoch == epoch) {
            media_audio_ring_rewind(&r.ring);
            r.status.clock_valid = false;
        }
        ReleaseSRWLockExclusive(&r.lock);
        return 0;
    }
    AcquireSRWLockShared(&r.lock);
    const bool start = !e.started && r.desired_play && !r.stopping && r.status.desired_epoch == epoch &&
                       r.ring.submitted > r.ring.consumed;
    const bool finished = r.ring.ended && r.ring.written == r.ring.submitted;
    ReleaseSRWLockShared(&r.lock);
    if (start) {
        if (!pulse::track_cork(&e.track, false))
            return pulse::kErrorPulse;
        e.started = true;
    }
    if (finished && e.started && !e.drain_requested && pulse::track_drain(&e.track)) {
        e.drain_requested = true;
        e.drain_deadline_ms = clock_monotonic_milliseconds() + pulse::kDrainGraceMs;
    }
    return 0;
}

u32 worker(void* pointer) noexcept {
    auto& r = *static_cast<Runtime*>(pointer);
    auto* engine = commit_pages<Engine>();
    // The command start gate publishes the creator's thread handle before any worker snapshot uses it. Stop also
    // wakes a failed launch without opening a connection.
    const Waitable startup[]{r.stop, r.command};
    const u32 started = wait_any(startup, 2, INFINITE);
    if (started != WAIT_OBJECT_0 + 1 || !engine) {
        AcquireSRWLockExclusive(&r.lock);
        r.status.worker_exited = true;
        if (started == WAIT_FAILED) {
            r.status.error = Error::wait;
            r.status.native_error = thread_last_error();
        } else if (started == WAIT_OBJECT_0 + 1) {
            r.status.error = Error::memory;
        }
        if (r.config.status_event)
            event_set(r.config.status_event);
        ReleaseSRWLockExclusive(&r.lock);
        if (engine)
            pages_release(engine, sizeof(Engine));
        return 0;
    }
    thread_boost_realtime();
    Engine& e = *engine;
    e.applied_volume = -1;
    for (;;) {
        AcquireSRWLockShared(&r.lock);
        const bool stopping = r.stopping, playing = r.desired_play, failed = r.failed;
        const u64 epoch = r.status.desired_epoch, serial = r.command_serial;
        const float volume = r.volume;
        ReleaseSRWLockShared(&r.lock);
        if (stopping)
            break;
        if (r.observed != epoch) {
            close_track(e);
            AcquireSRWLockExclusive(&r.lock);
            if (r.status.desired_epoch == epoch) {
                r.observed = epoch;
                r.status.clock_valid = false;
                r.status.presentation_time_100ns = r.origin;
                r.status.stream_open = false;
                r.status.playing = false;
                r.status.paused = !playing;
                r.status.ended = false;
                // Closing prior-epoch error cannot contaminate the fresh epoch.
                notify_locked(r);
            }
            ReleaseSRWLockExclusive(&r.lock);
        }
        // Device changes only wake the loop: the server moves a stream that follows the default sink.
        if (e.session.loop)
            pulse::session_take_dirty(&e.session);
        AcquireSRWLockShared(&r.lock);
        const bool current = r.status.desired_epoch == epoch && r.observed == epoch && !r.stopping;
        const bool available = r.ring.written != r.ring.consumed;
        ReleaseSRWLockShared(&r.lock);
        if (!current)
            continue;
        if (playing && !failed && available && !e.track.stream) {
            const u32 opened = open(r, e);
            if (opened)
                failure(r, e, epoch, serial, Error::device, opened);
        }
        if (e.track.stream) {
            u32 error = 0;
            if (!pulse::session_ready(e.session))
                error = pulse::kErrorPulse;
            else {
                if (volume != e.applied_volume && pulse::track_set_volume(&e.track, volume))
                    e.applied_volume = volume;
                error = render(r, e, epoch, playing);
            }
            if (error)
                failure(r, e, epoch, serial, Error::stream, error);
        }
        AcquireSRWLockExclusive(&r.lock);
        if (r.status.desired_epoch == epoch) {
            r.status.stream_open = e.track.stream != nullptr;
            r.status.playing = e.started;
            if (!e.track.stream)
                r.status.clock_valid = false;
            r.status.desired_volume = r.volume;
            r.status.applied_volume = e.applied_volume >= 0 ? e.applied_volume : 0;
            r.status.volume_valid = e.track.stream && e.applied_volume >= 0;
            r.status.paused = !r.desired_play && !r.failed;
            r.status.buffering = r.desired_play && !e.started && !r.ring.ended && !r.failed;
            r.status.ended = r.ring.ended && r.ring.written == r.ring.consumed && !e.started;
            notify_locked(r);
        }
        const bool repeat = r.status.desired_epoch != epoch || r.stopping;
        ReleaseSRWLockExclusive(&r.lock);
        if (repeat)
            continue;
        // The clock is read between write requests too, while audio plays: the wait times out after kClockPollMs.
        const Waitable events[]{r.stop, r.command, r.render};
        const u32 waited = wait_any(events, e.track.stream ? 3u : 2u, e.started ? kClockPollMs : INFINITE);
        if (waited == WAIT_OBJECT_0)
            break;
        if (waited == WAIT_FAILED) {
            failure(r, e, epoch, serial, Error::wait, thread_last_error());
            break;
        }
    }
    close_track(e);
    pulse::session_close(&e.session);
    pages_release(engine, sizeof(Engine));
    AcquireSRWLockExclusive(&r.lock);
    r.status.worker_exited = true;
    r.status.stream_open = r.status.playing = false;
    r.status.clock_valid = r.status.volume_valid = false;
    notify_locked(r);
    ReleaseSRWLockExclusive(&r.lock);
    return 0;
}

MediaAudioResult free_runtime(MediaAudio& owner) noexcept {
    auto& r = *static_cast<Runtime*>(owner.runtime);
    if (r.thread) {
        thread_close(r.thread);
        r.thread = nullptr;
    }
    Waitable* const waitables[]{&r.command, &r.stop, &r.render};
    for (Waitable* waitable : waitables)
        if (*waitable) {
            waitable_close(*waitable);
            *waitable = nullptr;
        }
    if (!pages_release(&r, sizeof(Runtime)))
        return fail(Error::release, thread_last_error());
    owner.runtime = nullptr;
    return {};
}

MediaAudioResult launch(MediaAudio& owner) noexcept {
    auto* r = commit_pages<Runtime>();
    if (!r)
        return fail(Error::memory, thread_last_error());
    owner.runtime = r;
    r->config = owner.config;
    r->ring = owner.ring;
    r->observed = owner.epoch;
    r->status.desired_epoch = r->status.observed_epoch = owner.epoch;
    r->origin = owner.config.source_origin_100ns;
    r->volume = owner.config.volume;
    r->status.desired_volume = r->volume;
    r->desired_play = true;
    r->command_serial = 1;
    r->status.worker_started = true;
    r->command = event_create(false, false);
    r->stop = event_create(true, false);
    r->render = event_create(false, false);
    if (!r->command || !r->stop || !r->render) {
        const u32 error = thread_last_error();
        r->status.worker_exited = true;
        free_runtime(owner);
        return fail(Error::event, error);
    }
    u32 error = 0;
    r->thread = thread_start(worker, r, 256 * 1024, &error);
    if (!r->thread) {
        r->status.worker_exited = true;
        free_runtime(owner);
        return fail(Error::thread, error);
    }
    if (!event_set(r->command) || (r->config.refill_event && !event_set(r->config.refill_event)))
        return fail(Error::event, thread_last_error());
    return {};
}

MediaAudioResult command(MediaAudio* owner, u32 kind, float volume = 0) noexcept {
    if (!owner || !owner->initialized)
        return fail(Error::invalid_input);
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (!r) {
        if (kind == 1)
            return launch(*owner);
        if (kind == 3)
            owner->config.volume = volume;
        return {};
    }
    AcquireSRWLockExclusive(&r->lock);
    MediaAudioResult result{};
    if (r->stopping || r->status.worker_exited)
        result = fail(Error::stopped);
    else if (kind == 1 && r->command_serial == ~u64{0})
        result = fail(Error::timeline);
    else {
        if (kind == 1) {
            ++r->command_serial;
            r->desired_play = true;
            r->failed = false;
            r->status.error = Error::none;
            r->status.native_error = 0;
        } else if (kind == 2)
            r->desired_play = false;
        else {
            r->volume = volume;
            r->status.desired_volume = volume;
            owner->config.volume = volume;
        }
        if (!event_set(r->command))
            result = fail(Error::event, thread_last_error());
    }
    ReleaseSRWLockExclusive(&r->lock);
    return result;
}
} // namespace

MediaAudioResult media_audio_initialize(MediaAudio* owner, const MediaAudioConfig& config) noexcept {
    if (!owner || owner->initialized || owner->runtime || !media_audio_gain_valid(config.volume) ||
        !disjoint(owner, sizeof(*owner), &config, sizeof(config)) || config.pcm.count > 32768 ||
        !disjoint(owner, sizeof(*owner), config.pcm.data, config.pcm.count * 4))
        return fail(Error::invalid_input);
    MediaAudioRing ring{};
    if (!media_audio_ring_initialize(&ring, config.pcm, config.sample_rate, config.channels, config.initial_epoch))
        return fail(Error::invalid_input);
    owner->config = config;
    owner->ring = ring;
    owner->epoch = config.initial_epoch;
    owner->initialized = true;
    return {};
}
MediaAudioResult media_audio_play(MediaAudio* owner) noexcept {
    return command(owner, 1);
}
MediaAudioResult media_audio_pause(MediaAudio* owner) noexcept {
    return command(owner, 2);
}
MediaAudioResult media_audio_volume(MediaAudio* owner, float volume) noexcept {
    return media_audio_gain_valid(volume) ? command(owner, 3, volume) : fail(Error::invalid_input);
}
MediaAudioResult media_audio_write(MediaAudio* owner, u64 epoch, ConstSlice<float> pcm) noexcept {
    if (!owner || !owner->initialized || pcm.count > 4096 || !disjoint(owner, sizeof(*owner), pcm.data, pcm.count * 4))
        return fail(Error::invalid_input);
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (!r)
        return media_audio_ring_write(&owner->ring, epoch, pcm);
    if (!disjoint(r, sizeof(*r), pcm.data, pcm.count * 4))
        return fail(Error::invalid_input);
    AcquireSRWLockExclusive(&r->lock);
    MediaAudioResult result =
        r->stopping || r->status.worker_exited ? fail(Error::stopped) : media_audio_ring_write(&r->ring, epoch, pcm);
    if (result.ok()) {
        r->refill_pending = false;
        if (!event_set(r->command))
            result = fail(Error::event, thread_last_error());
    }
    ReleaseSRWLockExclusive(&r->lock);
    return result;
}
MediaAudioResult media_audio_end(MediaAudio* owner, u64 epoch) noexcept {
    if (!owner || !owner->initialized)
        return fail(Error::invalid_input);
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (!r) {
        if (epoch != owner->epoch)
            return fail(Error::stale_epoch);
        owner->ring.ended = true;
        return {};
    }
    AcquireSRWLockExclusive(&r->lock);
    MediaAudioResult result{};
    if (r->stopping || r->status.worker_exited)
        result = fail(Error::stopped);
    else if (epoch != r->ring.epoch)
        result = fail(Error::stale_epoch);
    else {
        r->ring.ended = true;
        if (!event_set(r->command))
            result = fail(Error::event, thread_last_error());
    }
    ReleaseSRWLockExclusive(&r->lock);
    return result;
}
MediaAudioResult media_audio_reset(MediaAudio* owner, u64 epoch, i64 origin) noexcept {
    if (!owner || !owner->initialized || !epoch)
        return fail(Error::invalid_input);
    if (epoch <= owner->epoch)
        return fail(Error::stale_epoch);
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (!r) {
        owner->ring.epoch = epoch;
        owner->ring.written = owner->ring.submitted = owner->ring.consumed = 0;
        owner->ring.ended = false;
        owner->epoch = epoch;
        owner->config.source_origin_100ns = origin;
        return {};
    }
    AcquireSRWLockExclusive(&r->lock);
    MediaAudioResult result{};
    if (r->stopping || r->status.worker_exited)
        result = fail(Error::stopped);
    else {
        r->ring.epoch = epoch;
        r->ring.written = r->ring.submitted = r->ring.consumed = 0;
        r->ring.ended = false;
        r->status.desired_epoch = epoch;
        r->origin = origin;
        r->failed = false;
        r->refill_pending = false;
        r->status.error = Error::none;
        r->status.native_error = 0;
        owner->epoch = epoch;
        owner->config.source_origin_100ns = origin;
        if (!event_set(r->command) || (r->config.refill_event && !event_set(r->config.refill_event)))
            result = fail(Error::event, thread_last_error());
    }
    ReleaseSRWLockExclusive(&r->lock);
    return result;
}
MediaAudioStatus media_audio_status(const MediaAudio* owner) noexcept {
    if (!owner || !owner->initialized)
        return {};
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (!r) {
        MediaAudioStatus status{};
        status.desired_epoch = status.observed_epoch = owner->epoch;
        status.written_frames = owner->ring.written;
        status.free_frames = media_audio_ring_free(owner->ring);
        status.presentation_time_100ns = owner->config.source_origin_100ns;
        status.desired_volume = owner->config.volume;
        status.paused = true;
        status.ended = owner->ring.ended;
        return status;
    }
    AcquireSRWLockExclusive(&r->lock);
    notify_locked(*r);
    auto status = r->status;
    r->wake_pending = false;
    ReleaseSRWLockExclusive(&r->lock);
    return status;
}
MediaAudioResult media_audio_release(MediaAudio* owner, u32 timeout) noexcept {
    if (!owner)
        return fail(Error::invalid_input);
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (!r) {
        reset_value(owner);
        return {};
    }
    if (r->thread) {
        AcquireSRWLockExclusive(&r->lock);
        r->stopping = true;
        r->desired_play = false;
        const bool signaled = event_set(r->stop);
        const u32 error = signaled ? 0 : thread_last_error();
        ReleaseSRWLockExclusive(&r->lock);
        if (!signaled)
            return fail(Error::event, error);
        if (!thread_wait(r->thread, timeout))
            return fail(Error::join_pending, ERROR_TIMEOUT);
    }
    const auto result = free_runtime(*owner);
    if (result.ok())
        reset_value(owner);
    return result;
}
} // namespace maicord
