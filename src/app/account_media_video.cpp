#include "account_media_video_internal.hpp"
#include "maicord/app/account_video_commands.hpp"
#include "maicord/app/progressive_video_fetch.hpp"
#include "maicord/app/segmented_video_fetch.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"

namespace maicord {
struct AccountMediaVideoRuntime {
    SRWLOCK lock{};
    AccountVideoSession session{};
    AccountVideoSelection selection{};
    AccountVideoCommandQueue commands{};
    AccountVideoPlaybackStatus snapshot{};
    AccountVideoFrame frame{};
    AccountMediaWorker* worker{};
    volatile LONG selected_lane{-1}, command_pending{};
    u64 last_command{}, ack_epoch{}, ack_serial{};
    u64 buffered_generation{}, buffered_at_ms{};
    bool frame_taken{}, ack_pending{}, stop_requested{};
};
static_assert(sizeof(AccountMediaVideoRuntime) <= 4096);
namespace {
// Buffered-range publication is a full table walk; bound its rate.
constexpr u64 kBufferedRefreshMs = 250;
AccountVideoResult failed(AccountVideoError error, u32 native = 0) noexcept {
    return {error, native, false, false};
}
bool cancelled(void* opaque) noexcept {
    auto* v = static_cast<AccountMediaVideoRuntime*>(opaque);
    return WaitForSingleObject(v->worker->stop_event, 0) == WAIT_OBJECT_0 ||
           ReadAcquire(&v->command_pending) != 0;
}
bool source_cancelled(void* opaque) noexcept {
    const auto* worker = static_cast<const AccountMediaWorker*>(opaque);
    return !worker || WaitForSingleObject(worker->stop_event, 0) == WAIT_OBJECT_0;
}
u32 progressive_refresh_delay(const ProgressiveVideoFetch* video, const ProgressiveVideoFetch* audio, u64 now) noexcept {
    // Both tracks share one CDN request. Its completion event wakes this lane;
    // an overdue companion playlist cannot make progress until it retires.
    if ((video && video->inflight_id) || (audio && audio->inflight_id))
        return kInfiniteWaitMilliseconds;
    return minimum(segmented_video_refresh_delay(video, now), segmented_video_refresh_delay(audio, now));
}
DWORD wait_video_events(const HANDLE* events, DWORD count, HANDLE timer, DWORD delay_ms) noexcept {
    if (timer && delay_ms && delay_ms != INFINITE) {
        Win32LargeInteger due{};
        due.QuadPart = -static_cast<i64>(delay_ms) * 10000;
        if (SetWaitableTimer(timer, due.native(), 0, nullptr, nullptr, FALSE)) {
            HANDLE handles[]{events[0], events[1], count == 3 ? events[2] : timer, timer};
            const DWORD result = WaitForMultipleObjects(count + 1, handles, FALSE, INFINITE);
            if (result != WAIT_OBJECT_0 + count)
                CancelWaitableTimer(timer);
            return result == WAIT_OBJECT_0 + count ? WAIT_TIMEOUT : result;
        }
    }
    return WaitForMultipleObjects(count, events, FALSE, delay_ms);
}
bool changed(const AccountVideoStatus& a, const AccountVideoStatus& b) noexcept {
    return a.kind != b.kind || a.live != b.live || a.duration_100ns != b.duration_100ns || a.has_audio != b.has_audio ||
           a.desired_command != b.desired_command || a.observed_command != b.observed_command ||
           a.playback_epoch != b.playback_epoch || a.error != b.error ||
           a.native_error != b.native_error || a.playing != b.playing || a.paused != b.paused ||
           a.seeking != b.seeking || a.buffering != b.buffering || a.ended != b.ended ||
           a.muted != b.muted || a.volume_valid != b.volume_valid ||
           a.rejected_command != b.rejected_command || a.rejected_error != b.rejected_error ||
           a.rejected_native_error != b.rejected_native_error || a.applied_volume != b.applied_volume ||
           a.release_pending != b.release_pending ||
           a.position_100ns / 10000000 != b.position_100ns / 10000000;
}
void publish_status(AccountMediaVideoRuntime& v, const ProgressiveVideoFetch* fetch = nullptr,
                    const AccountVideoFrame* frame = nullptr) noexcept {
    auto status = account_video_session_status(&v.session);
    // A failed progressive source can never deliver the bytes playback waits for.
    if (fetch && fetch->state == ProgressiveVideoFetchState::failed && status.error == AccountVideoError::none) {
        status.error = AccountVideoError::demux;
        status.buffering = false;
    }
    AcquireSRWLockExclusive(&v.lock);
    // The public mailbox acknowledges admission immediately; completion is
    // still reported only by the lane owner after the native operation.
    status.desired_command = maximum(status.desired_command, v.last_command);
    // Publish the frame and its status together before waking the consumer.
    // Returning its lease updates the snapshot without another UI wake.
    const bool notify = frame || changed(v.snapshot.playback, status);
    if (frame) {
        v.frame = *frame;
        v.frame_taken = false;
    }
    v.snapshot.playback = status;
    ReleaseSRWLockExclusive(&v.lock);
    if (notify)
        SetEvent(v.worker->output_event);
}
void publish_error(AccountMediaVideoRuntime& v, const AccountVideoResult& result) noexcept {
    AcquireSRWLockExclusive(&v.lock);
    v.snapshot.playback.error = result.error;
    v.snapshot.playback.native_error = result.native_error;
    ReleaseSRWLockExclusive(&v.lock);
    SetEvent(v.worker->output_event);
}
bool selection_valid(const AccountVideoSelection& s) noexcept {
    return media_asset_key_valid(s.key) && s.identity.account_serial && s.identity.message_id &&
           s.identity.session_epoch && !s.identity.reserved && s.collection_track <= 8192;
}
bool buffered_equal(const AccountVideoBuffered& a, const AccountVideoBuffered& b) noexcept {
    if (a.count != b.count)
        return false;
    for (u32 i = 0; i < a.count; ++i)
        if (a.begin[i] != b.begin[i] || a.end[i] != b.end[i])
            return false;
    return true;
}
void publish_buffered(AccountMediaVideoRuntime& v, const AccountVideoBuffered& buffered) noexcept {
    AcquireSRWLockExclusive(&v.lock);
    const bool notify = !buffered_equal(v.snapshot.buffered, buffered);
    v.snapshot.buffered = buffered;
    ReleaseSRWLockExclusive(&v.lock);
    if (notify)
        SetEvent(v.worker->output_event);
}
bool consume_range(void* context, const CdnClientResponse& response, ConstSlice<u8> body) noexcept {
    account_media_accept_progressive(static_cast<AccountMedia*>(context), response, body);
    return true;
}
// Lane thread, between session services: stores arrived ranges, keeps one
// range in flight ahead of the cursors, and publishes buffered ranges.
void progressive_service(AccountMediaWorkerLane* lane, AccountMediaVideoRuntime& v) noexcept {
    auto& media = lane->media;
    auto* fetch = media.progressive;
    if (!fetch || !media.client_started)
        return;
    char offered[kCdnMaximumTargetBytes];
    u32 offered_bytes = 0;
    AcquireSRWLockExclusive(&v.lock);
    if (fetch->offered_bytes <= sizeof(offered)) {
        offered_bytes = fetch->offered_bytes;
        copy_bytes(offered, fetch->offered, offered_bytes);
    }
    fetch->offered_bytes = 0;
    ReleaseSRWLockExclusive(&v.lock);
    if (offered_bytes) {
        u8 target[kCdnMaximumTargetBytes];
        const auto parsed = parse_signed_attachment_url({offered, offered_bytes}, v.selection.key.id,
                                                        Slice<u8>{target, sizeof(target)});
        if (parsed.ok())
            progressive_video_fetch_refresh(fetch, {target, parsed.target_bytes}, parsed.host,
                                            parsed.expires_at_seconds);
    }
    if (cdn_client_status(&media.client).phase == CdnClientPhase::response_ready) {
        cdn_client_consume_response(&media.client, consume_range, &media);
        account_media_cache_progressive(&media, v.selection.key);
    }
    ProgressiveVideoFetch* selected{};
    const auto next = account_media_progressive_next(&media, v.session.desired_play, clock_unix_milliseconds() / 1'000, &selected);
    if (next.step == ProgressiveVideoStep::cancel) {
        if (selected && cdn_client_cancel(&media.client, selected->inflight_id))
            progressive_video_fetch_cancelling(selected);
    } else if (next.step == ProgressiveVideoStep::submit) {
        account_media_submit_progressive(&media, next.range, selected == media.progressive_audio);
    }
    if (next.step == ProgressiveVideoStep::failed) {
        fetch->state = ProgressiveVideoFetchState::failed;
        account_video_session_source_failed(&v.session);
    }
    const bool live_restart = next.step == ProgressiveVideoStep::failed && fetch->segmented && fetch->segmented->manifest.bytes;
    const bool refresh = next.step == ProgressiveVideoStep::refresh_required || live_restart;
    AcquireSRWLockExclusive(&v.lock);
    // The UI clears the flag when it offers a URL; ask once per expiry.
    const bool notify = refresh && !v.snapshot.source_refresh && !fetch->offered_bytes;
    if (notify)
        v.snapshot.source_refresh = true;
    ReleaseSRWLockExclusive(&v.lock);
    if (notify)
        SetEvent(v.worker->output_event);
    const u64 now = clock_monotonic_milliseconds();
    const u64 generation = progressive_video_fetch_generation(fetch) + progressive_video_fetch_generation(media.progressive_audio);
    if (generation != v.buffered_generation && now - v.buffered_at_ms >= kBufferedRefreshMs) {
        v.buffered_generation = generation;
        v.buffered_at_ms = now;
        AccountVideoBuffered buffered{};
        buffered.count = static_cast<u8>(progressive_video_fetch_buffered(
            fetch, static_cast<u64>(v.session.status.duration_100ns / 10), buffered.begin, buffered.end,
            kAccountVideoBufferedRanges));
        if (media.progressive_audio) {
            AccountVideoBuffered audio{}, both{};
            audio.count = static_cast<u8>(progressive_video_fetch_buffered(media.progressive_audio,
                static_cast<u64>(v.session.status.duration_100ns / 10), audio.begin, audio.end, kAccountVideoBufferedRanges));
            for (u32 i = 0; i < buffered.count; ++i) for (u32 j = 0; j < audio.count; ++j) {
                const u16 first = maximum(buffered.begin[i], audio.begin[j]), last = minimum(buffered.end[i], audio.end[j]);
                if (last > first && both.count < kAccountVideoBufferedRanges) {
                    both.begin[both.count] = first; both.end[both.count++] = last;
                }
            }
            buffered = both;
        }
        publish_buffered(v, buffered);
    }
}
// Joined exit: no range may outlive the selection on the shared client, and an
// idle lane keeps only the pinned metadata of its progressive source.
void progressive_retire(AccountMediaWorkerLane* lane) noexcept {
    auto& media = lane->media;
    const bool live = media.progressive && media.progressive->segmented && media.progressive->segmented->manifest.bytes;
    ProgressiveVideoFetch* sources[]{media.progressive, media.progressive_audio};
    for (auto* fetch : sources) if (fetch) {
        if (media.client_started && fetch->inflight_id) {
            cdn_client_cancel(&media.client, fetch->inflight_id);
            progressive_video_fetch_cancelling(fetch);
            for (u32 attempt = 0; attempt < 50 && fetch->inflight_id; ++attempt) {
                if (cdn_client_status(&media.client).phase == CdnClientPhase::response_ready)
                    cdn_client_consume_response(&media.client, consume_range, &media);
                else
                    cdn_client_wait_response(&media.client, 100);
            }
        }
        if (!live) progressive_video_fetch_retire(fetch);
    }
    // Live ordinals and signed playlists expire. Reopening starts a fresh
    // provider resolution after the old session and all frame leases retire.
    if (live) account_media_release_video(&media);
}
} // namespace
bool account_media_video_offer_url(AccountMediaWorker* worker, MediaAssetKey key, Utf8View url) noexcept {
    if (!worker || !worker->initialized || !worker->video_runtime || !url.data || !url.bytes ||
        url.bytes > kCdnMaximumTargetBytes)
        return false;
    auto& v = *worker->video_runtime;
    AcquireSRWLockExclusive(&v.lock);
    const LONG lane = v.selected_lane;
    // The selected lane cannot replace its fetch while selection holds it.
    auto* fetch = lane >= 0 ? worker->lanes[lane].media.progressive : nullptr;
    const bool wanted = fetch && v.snapshot.source_refresh && media_asset_key_equal(v.selection.key, key);
    if (wanted) {
        copy_bytes(fetch->offered, url.data, url.bytes);
        fetch->offered_bytes = url.bytes;
        v.snapshot.source_refresh = false;
    }
    ReleaseSRWLockExclusive(&v.lock);
    if (wanted)
        SetEvent(worker->lanes[lane].command_event);
    return wanted;
}
bool account_media_video_reserved(const AccountMediaWorker* worker, u32 lane) noexcept {
    return worker && worker->video_runtime &&
           ReadAcquire(&worker->video_runtime->selected_lane) == static_cast<LONG>(lane);
}
AccountVideoResult account_media_worker_video_select(AccountMediaWorker* worker,
                                                     const AccountVideoSelection& requested) noexcept {
    if (!worker || !worker->initialized || !selection_valid(requested))
        return failed(AccountVideoError::invalid_input);
    if (WaitForSingleObject(worker->stop_event, 0) == WAIT_OBJECT_0)
        return failed(AccountVideoError::busy);
    if (!worker->video_runtime) {
        auto* memory = static_cast<AccountMediaVideoRuntime*>(VirtualAlloc(
            nullptr, sizeof(AccountMediaVideoRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!memory)
            return failed(AccountVideoError::memory, GetLastError());
        memory->selected_lane = -1;
        memory->worker = worker;
        worker->video_runtime = memory;
    }
    auto& v = *worker->video_runtime;
    AcquireSRWLockExclusive(&v.lock);
    if (v.selected_lane >= 0) {
        ReleaseSRWLockExclusive(&v.lock);
        return failed(AccountVideoError::busy);
    }
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        auto& lane = worker->lanes[index];
        const LONG phase = ReadAcquire(&lane.phase);
        if (phase != static_cast<LONG>(AccountMediaWorkerPhase::idle) &&
            phase != static_cast<LONG>(AccountMediaWorkerPhase::leased))
            continue;
        const bool audio =
            account_media_audio_matches(&lane.media, requested.key, requested.identity.cache_generation);
        const bool video = !audio && account_media_video_matches(&lane.media, requested.key,
                                                                 requested.identity.cache_generation);
        const u64 source_message =
            audio ? lane.media.audio_source.source_message_id : lane.media.mp4_source_message_id;
        const u32 source_revision = audio ? lane.media.audio_source.source_message_revision
                                          : lane.media.mp4_source_message_revision;
        const u64 content_identity =
            audio ? lane.media.audio_source.content_identity : lane.media.mp4_content_identity;
        const auto collection = account_media_collection_state(&lane.media, requested.key);
        if ((requested.collection_track && requested.collection_track != collection.track) ||
            (!audio && !video) || source_message != requested.identity.message_id ||
            source_revision != requested.identity.message_revision ||
            (requested.identity.content_identity &&
             requested.identity.content_identity != content_identity))
            continue;
        if (InterlockedCompareExchange(
                &lane.phase, static_cast<LONG>(AccountMediaWorkerPhase::selected_video), phase) != phase)
            continue;
        v.selection = requested;
        v.selection.identity.cache_generation =
            audio ? lane.media.audio_source.cache_generation : lane.media.mp4_generation;
        v.selection.identity.content_identity = content_identity;
        reset_value(&v.commands);
        v.last_command = 0;
        v.ack_pending = false;
        v.frame_taken = false;
        v.stop_requested = false;
        reset_value(&v.frame);
        reset_value(&v.snapshot);
        v.snapshot.key = requested.key;
        v.snapshot.playback.identity = v.selection.identity;
        v.snapshot.selected = true;
        v.snapshot.collection_count = collection.count;
        v.snapshot.collection_track = collection.track;
        v.snapshot.collection_more = collection.more;
        const auto format = media_asset_audio_format(requested.key);
        v.snapshot.playback.kind = !audio                                 ? AccountPlaybackKind::mp4
                                   : format == MediaAttachmentFormat::mp3 ? AccountPlaybackKind::mp3
                                   : format == MediaAttachmentFormat::wav ? AccountPlaybackKind::wave_pcm
                                   // AAC audio reuses the MP4 session's decoder path.
                                   : format == MediaAttachmentFormat::m4a ? AccountPlaybackKind::mp4
                                                                          : AccountPlaybackKind::ogg_opus;
        InterlockedExchange(&v.command_pending, 0);
        InterlockedExchange(&v.selected_lane, static_cast<LONG>(index));
        ReleaseSRWLockExclusive(&v.lock);
        if (!SetEvent(lane.command_event)) {
            AcquireSRWLockExclusive(&v.lock);
            v.snapshot.playback.error = AccountVideoError::event;
            v.snapshot.playback.native_error = GetLastError();
            // Publication cannot be rolled back without proving the worker has
            // not consumed selection. Keep ownership for explicit account stop.
            ReleaseSRWLockExclusive(&v.lock);
            return failed(AccountVideoError::event, GetLastError());
        }
        SetEvent(worker->output_event);
        return {AccountVideoError::none, 0, true, false};
    }
    ReleaseSRWLockExclusive(&v.lock);
    return failed(AccountVideoError::busy);
}
AccountVideoResult account_media_worker_video_control(AccountMediaWorker* worker,
                                                      const AccountVideoControl& command) noexcept {
    if (!worker || !worker->initialized || !worker->video_runtime || !command.command_serial ||
        static_cast<u32>(command.command) > static_cast<u32>(AccountVideoCommand::stop) ||
        command.volume != command.volume || command.volume < 0 || command.volume > 1 ||
        command.seek_time_100ns < 0)
        return failed(AccountVideoError::invalid_input);
    auto& v = *worker->video_runtime;
    AcquireSRWLockExclusive(&v.lock);
    const LONG lane = v.selected_lane;
    if (lane < 0 || !account_video_identity_equal(command.identity, v.selection.identity) ||
        command.command_serial <= v.last_command) {
        ReleaseSRWLockExclusive(&v.lock);
        return failed(AccountVideoError::stale);
    }
    if (v.stop_requested && command.command != AccountVideoCommand::stop) {
        ReleaseSRWLockExclusive(&v.lock);
        return failed(AccountVideoError::busy);
    }
    const bool stop = command.command == AccountVideoCommand::stop;
    if (!account_video_commands_push(v.commands, command)) {
        ReleaseSRWLockExclusive(&v.lock);
        return failed(AccountVideoError::busy);
    }
    v.last_command = command.command_serial;
    v.stop_requested = stop;
    v.snapshot.playback.desired_command = command.command_serial;
    InterlockedExchange(&v.command_pending, 1);
    ReleaseSRWLockExclusive(&v.lock);
    if (!SetEvent(worker->lanes[lane].command_event))
        return failed(AccountVideoError::event, GetLastError());
    return {AccountVideoError::none, 0, true, false};
}
AccountVideoPlaybackStatus account_media_worker_video_status(const AccountMediaWorker* worker) noexcept {
    if (!worker || !worker->initialized || !worker->video_runtime)
        return {};
    auto& v = *worker->video_runtime;
    AcquireSRWLockShared(&v.lock);
    const auto copy = v.snapshot;
    ReleaseSRWLockShared(&v.lock);
    return copy;
}
bool account_media_worker_video_take(AccountMediaWorker* worker, AccountVideoFrame* out) noexcept {
    if (!worker || !worker->initialized || !worker->video_runtime || !out)
        return false;
    auto& v = *worker->video_runtime;
    AcquireSRWLockExclusive(&v.lock);
    if (v.selected_lane < 0 || !v.frame.gpu || v.frame_taken) {
        ReleaseSRWLockExclusive(&v.lock);
        return false;
    }
    *out = v.frame;
    v.frame_taken = true;
    ReleaseSRWLockExclusive(&v.lock);
    return true;
}
bool account_media_worker_video_acknowledge(AccountMediaWorker* worker,
                                            const AccountVideoFrame& frame) noexcept {
    if (!worker || !worker->initialized || !worker->video_runtime)
        return false;
    auto& v = *worker->video_runtime;
    AcquireSRWLockExclusive(&v.lock);
    const LONG lane = v.selected_lane;
    if (lane < 0 || !v.frame.gpu || !v.frame_taken || v.ack_pending ||
        !media_asset_key_equal(frame.key, v.selection.key) ||
        !account_video_identity_equal(frame.identity, v.selection.identity) ||
        frame.playback_epoch != v.frame.playback_epoch || frame.frame_serial != v.frame.frame_serial) {
        ReleaseSRWLockExclusive(&v.lock);
        return false;
    }
    v.ack_epoch = frame.playback_epoch;
    v.ack_serial = frame.frame_serial;
    v.ack_pending = true;
    ReleaseSRWLockExclusive(&v.lock);
    return SetEvent(worker->lanes[lane].command_event) != FALSE;
}
bool account_media_video_dispatch(AccountMediaWorkerLane* lane, bool* stopped) noexcept {
    if (!lane || !stopped || !account_media_video_reserved(lane->owner, lane->index))
        return false;
    auto& v = *lane->owner->video_runtime;
    *stopped = false;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool owns_apartment = SUCCEEDED(apartment);
    AccountVideoResult initialized = failed(AccountVideoError::decoder, static_cast<u32>(apartment));
    auto* fetch = lane->media.progressive;
    if (owns_apartment) {
        if (account_media_audio_matches(&lane->media, v.selection.key,
                                        v.selection.identity.cache_generation)) {
            if (fetch && fetch->kind == ProgressiveVideoKind::fragmented_audio) {
                if (progressive_video_fetch_restore(fetch))
                    initialized = account_video_session_initialize_fragment_audio(&v.session, &fetch->audio,
                        v.selection.identity, lane->command_event);
            } else {
                fetch = nullptr;
                initialized = account_video_session_initialize_audio(
                    &v.session, &lane->media.audio_source.metadata, v.selection.identity, lane->command_event);
            }
        } else if (mp4_decoder_acknowledge_frame(lane->media.mp4_decoder)) {
            Mp4SourceAvailability audio{};
            bool restored = true;
            if (fetch) {
                if (fetch->kind == ProgressiveVideoKind::movie) {
                    progressive_source_reset_readers(&fetch->source);
                    audio = progressive_source_reader(&fetch->source, ProgressiveTrack::audio);
                } else restored = progressive_video_fetch_restore(fetch);
            }
            auto* companion = lane->media.progressive_audio;
            if (companion) restored = restored && progressive_video_fetch_restore(companion);
            if (restored) initialized = account_video_session_initialize(&v.session, lane->media.mp4_decoder,
                v.selection.identity, lane->command_event, audio, companion ? &companion->audio : nullptr);
        }
    }
    {
        // A complete source buffers everything; a progressive one publishes as ranges land.
        // Rides along with the next status publication; selection already notified.
        AccountVideoBuffered buffered{};
        buffered.count = fetch ? 0 : 1;
        buffered.end[0] = fetch ? 0 : 10000;
        v.buffered_generation = 0;
        v.buffered_at_ms = 0;
        AcquireSRWLockExclusive(&v.lock);
        v.snapshot.buffered = buffered;
        ReleaseSRWLockExclusive(&v.lock);
    }
    HANDLE range_event =
        fetch && lane->media.client_started ? cdn_client_notification_handle(&lane->media.client) : nullptr;
    if (!initialized.ok())
        publish_error(v, initialized);
    if (initialized.ok()) {
        v.session.source_cancelled = source_cancelled;
        v.session.source_cancel_context = v.worker;
    }
    bool exit_selection = !initialized.ok();
    // Only queued video frames need precise deadlines. This one-shot timer is
    // never armed for paused playback, audio-only playback, or network waits.
    const HANDLE frame_timer = initialized.ok() && v.session.video
        ? CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS) : nullptr;
    bool stopping = false;
    for (;;) {
        stopping = WaitForSingleObject(lane->owner->stop_event, 0) == WAIT_OBJECT_0;
        AccountVideoControl command{};
        bool has_command = false, ack = false, discard = false;
        u64 ack_epoch = 0, ack_serial = 0;
        AcquireSRWLockExclusive(&v.lock);
        if (v.ack_pending) {
            ack = true;
            ack_epoch = v.ack_epoch;
            ack_serial = v.ack_serial;
            v.ack_pending = false;
        }
        if ((stopping || v.stop_requested) && v.frame.gpu && !v.frame_taken) {
            // Stop may race the final decoded publication after the UI's
            // last drain. Claim an unborrowed slot while admission is locked;
            // a concurrent take must not borrow it during native disposal.
            discard = true;
            v.frame_taken = true;
            ack_epoch = v.frame.playback_epoch;
            ack_serial = v.frame.frame_serial;
        }
        const bool queued = account_video_commands_peek(v.commands, command);
        const bool blocked =
            v.frame.gpu && !ack && !discard &&
            (stopping || v.stop_requested || (queued && command.command == AccountVideoCommand::seek));
        if (queued && !blocked) {
            has_command = true;
            account_video_commands_pop(v.commands);
            InterlockedExchange(&v.command_pending, v.commands.count != 0);
        }
        ReleaseSRWLockExclusive(&v.lock);
        if (ack || discard) {
            if (account_video_session_acknowledge(&v.session, v.selection.identity, ack_epoch,
                                                  ack_serial)) {
                AcquireSRWLockExclusive(&v.lock);
                reset_value(&v.frame);
                v.frame_taken = false;
                ReleaseSRWLockExclusive(&v.lock);
            } else {
                AcquireSRWLockExclusive(&v.lock);
                v.snapshot.playback.error = AccountVideoError::decoder;
                v.snapshot.playback.release_pending = true;
                ReleaseSRWLockExclusive(&v.lock);
                SetEvent(lane->owner->output_event);
                exit_selection = true;
            }
        }
        if (stopping && !blocked)
            exit_selection = true;
        if (has_command) {
            if (command.command == AccountVideoCommand::stop)
                exit_selection = true;
            else if (!exit_selection) {
                const auto accepted = account_video_session_control(
                    &v.session, command.identity, command.command_serial, command.command,
                    command.seek_time_100ns, command.volume, command.muted);
                if (!accepted.ok())
                    publish_error(v, accepted);
            }
        }
        if (exit_selection && !blocked) {
            const auto released = account_video_session_release(&v.session, 0);
            if (released.ok())
                break;
            AcquireSRWLockExclusive(&v.lock);
            const bool notify = !v.snapshot.playback.release_pending ||
                                v.snapshot.playback.error != released.error ||
                                v.snapshot.playback.native_error != released.native_error;
            v.snapshot.playback.error = released.error;
            v.snapshot.playback.native_error = released.native_error;
            v.snapshot.playback.release_pending = true;
            ReleaseSRWLockExclusive(&v.lock);
            if (notify)
                SetEvent(lane->owner->output_event);
            // worker_exited signals this borrowed event before the thread
            // handle becomes signaled. A bounded retry closes that race.
            const DWORD waited =
                WaitForSingleObject(lane->command_event, released.error == AccountVideoError::release &&
                                                                 released.native_error == WAIT_TIMEOUT
                                                             ? 10
                                                             : INFINITE);
            if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT)
                break;
            continue;
        }
        AccountVideoService service{};
        if (initialized.ok() && !exit_selection)
            progressive_service(lane, v);
        if (!blocked && !exit_selection) {
            service = account_video_session_service(&v.session, cancelled, &v);
            AccountVideoFrame frame{};
            if (service.frame_ready) {
                frame = {v.selection.key,
                         v.selection.identity,
                         service.frame.gpu,
                         service.playback_epoch,
                         service.frame_serial,
                         service.frame.presentation_time_us,
                         service.frame.source_duration_us,
                         service.frame.display_width,
                         service.frame.display_height};
            }
            publish_status(v, fetch, service.frame_ready ? &frame : nullptr);
        }
        if (service.needs_service || service.interrupted)
            continue;
        // Range responses wake the lane too: they feed pending reads and read-ahead.
        HANDLE handles[]{lane->owner->stop_event, lane->command_event, range_event};
        DWORD delay = blocked ? INFINITE : service.next_service_delay_ms;
        if (!blocked && !exit_selection && v.session.desired_play && v.session.status.error == AccountVideoError::none) {
            const u64 now = clock_unix_milliseconds() / 1000;
            delay = minimum<DWORD>(delay, progressive_refresh_delay(lane->media.progressive, lane->media.progressive_audio, now));
        }
        const DWORD waited = stopping ? WaitForSingleObject(lane->command_event, delay)
            : wait_video_events(handles, range_event ? 3 : 2,
                v.session.held && !v.session.published && !v.session.status.buffering &&
                    delay == service.next_service_delay_ms ? frame_timer : nullptr, delay);
        if (waited == WAIT_FAILED) {
            exit_selection = true;
            continue;
        }
    }
    if (frame_timer)
        CloseHandle(frame_timer);
    // Only successful release allows the borrowed decoder/lane/module to retire.
    const bool retained = v.session.initialized || v.session.workspace;
    if (!retained) {
        if (fetch)
            progressive_retire(lane);
        if (owns_apartment)
            CoUninitialize();
        AcquireSRWLockExclusive(&v.lock);
        // Audio retirement may signal this borrowed event after the last wait.
        // Drain that wake while selection still excludes new generic work.
        while (WaitForSingleObject(lane->command_event, 0) == WAIT_OBJECT_0) {
        }
        v.snapshot.selected = false;
        v.snapshot.playback.playing = false;
        v.snapshot.playback.paused = true;
        if (v.stop_requested)
            v.snapshot.playback.observed_command = v.last_command;
        v.snapshot.playback.release_pending = false;
        reset_value(&v.frame);
        v.frame_taken = false;
        InterlockedExchange(&v.selected_lane, -1);
        InterlockedExchange(&lane->phase, static_cast<LONG>(stopping ? AccountMediaWorkerPhase::stopped
                                                                     : AccountMediaWorkerPhase::idle));
        ReleaseSRWLockExclusive(&v.lock);
        SetEvent(lane->owner->output_event);
        *stopped = stopping;
    }
    if (retained) {
        InterlockedExchange(&lane->phase, static_cast<LONG>(AccountMediaWorkerPhase::failed));
        // The lane terminates without freeing retained audio/source state.
        // Joined account stop still rejects destroy and quarantines the module.
        *stopped = true;
        SetEvent(lane->owner->output_event);
    }
    return true;
}
bool account_media_video_destroy(AccountMediaWorker* worker) noexcept {
    if (!worker || !worker->video_runtime)
        return true;
    auto* v = worker->video_runtime;
    if (ReadAcquire(&v->selected_lane) >= 0 || v->session.workspace || v->session.initialized)
        return false;
    if (!VirtualFree(v, 0, MEM_RELEASE))
        return false;
    worker->video_runtime = nullptr;
    return true;
}
} // namespace maicord
