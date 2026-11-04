#include "account_media_worker_internal.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/media/embed_provider.hpp"

namespace maicord {
namespace {

bool stop_lane_media(AccountMediaWorkerLane* lane, u64 deadline_ms) noexcept {
    const u64 now_ms = clock_monotonic_milliseconds();
    const u32 remaining =
        deadline_ms > now_ms ? static_cast<u32>(minimum<u64>(deadline_ms - now_ms, 0xFFFFFFFFull)) : 1;
    // A lane that never started has no media to stop.
    return !lane->initialized || (lane->media.started && account_media_stop(&lane->media, remaining));
}

// Initialization starts no lane thread (lanes start on their first command).
void cleanup_failed_initialize(AccountMediaWorker* worker) noexcept {
    for (u32 index = kAccountMediaDecodeLaneCount; index != 0; --index) {
        AccountMediaWorkerLane& lane = worker->lanes[index - 1];
        if (lane.media.started) {
            account_media_stop(&lane.media, 5'000);
        }
        if (lane.command_event) {
            waitable_close(lane.command_event);
        }
    }
    if (worker->output_event) {
        waitable_close(worker->output_event);
    }
    if (worker->stop_event) {
        waitable_close(worker->stop_event);
    }
    SecureZeroMemory(worker, sizeof(*worker));
}

} // namespace

bool account_media_worker_stop_requested(AccountMediaWorker* worker) noexcept {
    const LONG tick = static_cast<LONG>(clock_monotonic_milliseconds());
    if (ReadAcquire(&worker->stop_checked_tick) == tick)
        return false;
    if (wait_one(worker->stop_event, 0) == WAIT_OBJECT_0)
        return true;
    WriteRelease(&worker->stop_checked_tick, tick);
    return false;
}

AccountMediaRequestResult account_media_worker_collection_title(AccountMediaWorker* worker,
    const AccountMediaCollectionTitleRequest& request, MediaTrackTitle* out) noexcept {
    if (!worker || !worker->initialized || !out || !request.track || !media_asset_key_valid(request.key) ||
        account_media_worker_stop_requested(worker))
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    collect_collection_titles(worker);
    MediaTrackTitle title{}; MediaTrackId id{}; u64 retry{};
    auto* owner = collection_title_owner(worker, request, &title, &id, &retry);
    if (!owner) return request_result(AccountMediaRequestState::busy);
    if (title.bytes) { *out = title; return request_result(AccountMediaRequestState::ready); }
    const auto now = clock_monotonic_milliseconds();
    if (retry > now) return retry_wait_until(retry);
    if (id.apple_track) return request_result(AccountMediaRequestState::unsupported);
    if (!request.fetch) return request_result(AccountMediaRequestState::pending);
    // Hover never fans out a batch of requests while the pointer moves. One
    // bounded metadata operation runs alongside the reserved playback lane.
    for (const auto& lane : worker->lanes) {
        const auto phase = load_phase(&lane);
        if ((phase == AccountMediaWorkerPhase::command_ready || phase == AccountMediaWorkerPhase::working) &&
            lane.command.kind == AccountMediaWorkKind::collection_title) return request_result(AccountMediaRequestState::pending);
    }
    AccountMediaWorkItem work{};
    work.kind = AccountMediaWorkKind::collection_title; work.key = request.key;
    work.source_message_revision = request.source_revision; work.collection_track = request.track;
    work.title_track = id; work.monotonic_now_ms = now;
    SoundcloudTitleBatch batch{};
    if (!id.youtube_track && account_media_collection_title_batch(owner,request,&batch)) {
        static_assert(sizeof(batch)<=sizeof(work.text));
        work.title_batch=true; work.text_bytes=sizeof(batch); copy_bytes(work.text,&batch,sizeof(batch));
    }
    work.collection = 1; // A title failure must not poison the playable embed's retry cache.
    return queue(worker, work);
}

u32 account_media_worker_animation_lane(const AccountMediaWorker* worker, MediaAssetKey key) noexcept {
    u32 spare[2]{kAccountMediaDecodeLaneCount, kAccountMediaDecodeLaneCount}; // started, unstarted
    for (u32 index = 0; worker && index < kAccountMediaDecodeLaneCount; ++index) {
        const AccountMediaWorkerLane& lane = worker->lanes[index];
        if (load_phase(&lane) != AccountMediaWorkerPhase::idle)
            continue;
        if (lane.initialized && media_asset_key_equal(lane.command.key, key))
            return index;
        u32& slot = spare[lane.initialized ? 0 : 1];
        if (slot == kAccountMediaDecodeLaneCount &&
            lane.command.kind != AccountMediaWorkKind::animation_advance &&
            !(lane.command.key.flags & media_asset_flag_animated))
            slot = index;
    }
    return spare[0] != kAccountMediaDecodeLaneCount ? spare[0] : spare[1];
}

AccountMediaRequestResult account_media_worker_initialize(AccountMediaWorker* worker) noexcept {
    if (!worker) {
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    SecureZeroMemory(worker, sizeof(*worker));
    worker->stop_event = event_create(true, false);
    worker->output_event = event_create(false, false);
    worker->preferred_lane = -1;
    if (!worker->stop_event || !worker->output_event) {
        cleanup_failed_initialize(worker);
        return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
    }

    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        AccountMediaWorkerLane& lane = worker->lanes[index];
        lane.owner = worker;
        lane.index = index;
        lane.command_event = event_create(false, false);
        if (!lane.command_event) {
            cleanup_failed_initialize(worker);
            return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
        }
        store_phase(&lane, AccountMediaWorkerPhase::idle);
    }
    // Only the cache owner commits its storage now; see start_lane.
    const AccountMediaRequestResult initialized = initialize_lane_media(worker, worker->lanes[0]);
    if (!initialized.ok()) {
        cleanup_failed_initialize(worker);
        return initialized;
    }
    worker->initialized = true;
    return request_result(AccountMediaRequestState::ready);
}

AccountMediaRequestResult account_media_worker_start_lane(AccountMediaWorker* worker, u32 index) noexcept {
    if (!worker || !worker->initialized || index >= kAccountMediaDecodeLaneCount ||
        load_phase(&worker->lanes[index]) != AccountMediaWorkerPhase::idle ||
        account_media_worker_stop_requested(worker))
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    return start_lane(worker, worker->lanes[index]);
}

AccountMediaRequestResult account_media_worker_request_static_asset(AccountMediaWorker* worker,
                                                                    MediaAssetKey key, u64 monotonic_now_ms,
                                                                    u64 unix_now_seconds) noexcept {
    AccountMediaWorkItem work{};
    work.key = key;
    work.monotonic_now_ms = monotonic_now_ms;
    work.unix_now_seconds = unix_now_seconds;
    work.kind = AccountMediaWorkKind::static_asset;
    return queue(worker, work);
}

AccountMediaRequestResult account_media_worker_request_guild_icon(
    AccountMediaWorker* worker, const AccountMediaGuildIconRequest& request, u64 monotonic_now_ms,
    u64 unix_now_seconds) noexcept {
    if (request.icon_hash.bytes > kAccountMediaWorkerTextCapacity ||
        (!request.icon_hash.data && request.icon_hash.bytes != 0) || request.reserved[0] > 4'096) {
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    AccountMediaWorkItem work{};
    work.key = request.key;
    work.proxy_width = static_cast<u16>(request.reserved[0]);
    work.monotonic_now_ms = monotonic_now_ms;
    work.unix_now_seconds = unix_now_seconds;
    work.text_bytes = static_cast<u16>(request.icon_hash.bytes);
    work.kind = AccountMediaWorkKind::guild_icon;
    if (!copy_bytes(work.text, sizeof(work.text), request.icon_hash.data, request.icon_hash.bytes)) {
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    return queue(worker, work);
}

AccountMediaRequestResult account_media_worker_request_attachment(
    AccountMediaWorker* worker, const AccountMediaAttachmentRequest& request, u64 monotonic_now_ms,
    u64 unix_now_seconds) noexcept {
    const bool collection = embed_collection_url(request.signed_url);
    if (request.options.progressive > 1 || request.options.reserved[0] || request.options.reserved[1] ||
        request.options.collection_track > 8192 ||
        (request.options.collection_track && (!collection || !request.options.progressive || (request.image_type != CdnImageType::m4a && request.image_type != CdnImageType::mp4))) ||
        (request.options.progressive &&
         (request.key.kind != MediaAssetKind::embed || (request.image_type != CdnImageType::mp4 && request.image_type != CdnImageType::m4a))) ||
        request.signed_url.bytes > kAccountMediaWorkerTextCapacity ||
        (!request.signed_url.data && request.signed_url.bytes != 0)) {
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    AccountMediaWorkItem work{};
    work.key = request.key;
    work.monotonic_now_ms = monotonic_now_ms;
    work.unix_now_seconds = unix_now_seconds;
    work.source_message_id = request.source_message_id;
    work.video_device = request.video_device;
    work.source_message_revision = request.source_message_revision;
    work.proxy_width = request.proxy_width;
    work.proxy_height = request.proxy_height;
    work.text_bytes = static_cast<u16>(request.signed_url.bytes);
    work.image_type = request.image_type;
    work.kind = AccountMediaWorkKind::attachment;
    work.progressive = request.options.progressive;
    work.collection = collection;
    work.collection_track = request.options.collection_track;
    if (!copy_bytes(work.text, sizeof(work.text), request.signed_url.data, request.signed_url.bytes)) {
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    // A playing progressive source takes a refreshed URL in place.
    if (account_media_video_offer_url(worker, request.key, request.signed_url))
        return request_result(AccountMediaRequestState::pending);
    return queue(worker, work);
}

AccountMediaRequestResult account_media_worker_request_catalog_preview(
    AccountMediaWorker* worker, const AccountMediaCatalogPreviewRequest& request, u64 monotonic_now_ms,
    u64 unix_now_seconds) noexcept {
    bool reserved = false;
    for (u8 value : request.reserved)
        reserved = reserved || value != 0;
    if (reserved || !media_asset_key_valid(request.key) ||
        request.key.kind != MediaAssetKind::catalog_preview || !request.catalog_scope ||
        !request.catalog_generation || request.key.variant != request.catalog_generation ||
        request.format != media_asset_catalog_format(request.key) || !request.width || !request.height ||
        request.width > 512 || request.height > 512 || !request.url.data || !request.url.bytes ||
        request.url.bytes > kAccountMediaWorkerTextCapacity || !monotonic_now_ms || !unix_now_seconds)
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    u8 target[kCdnMaximumTargetBytes];
    if (!parse_catalog_preview_url(request.url, {target, sizeof(target)}, request.format, request.width,
                                   request.height).ok())
        return request_result(AccountMediaRequestState::failed, AccountMediaError::target);
    AccountMediaWorkItem work{};
    work.key = request.key;
    work.monotonic_now_ms = monotonic_now_ms;
    work.unix_now_seconds = unix_now_seconds;
    work.catalog_scope = request.catalog_scope;
    work.catalog_generation = request.catalog_generation;
    work.proxy_width = request.width;
    work.proxy_height = request.height;
    work.video_device = request.video_device;
    work.text_bytes = static_cast<u16>(request.url.bytes);
    work.kind = AccountMediaWorkKind::catalog_preview;
    if (!copy_bytes(work.text, sizeof(work.text), request.url.data, request.url.bytes))
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    return queue(worker, work);
}

AccountMediaRequestResult account_media_worker_advance_animation(AccountMediaWorker* worker,
                                                                 MediaAssetKey key, u16 cache_generation,
                                                                 u32 frame_index,
                                                                 u64 unix_now_seconds) noexcept {
    AccountMediaWorkItem work{};
    work.key = key;
    work.unix_now_seconds = unix_now_seconds;
    work.frame_index = frame_index;
    work.cache_generation = cache_generation;
    work.kind = AccountMediaWorkKind::animation_advance;
    return queue(worker, work);
}

AccountMediaRequestResult account_media_worker_request_local_preview(
    AccountMediaWorker* worker, const AccountMediaLocalPreviewRequest& request, u64 now_ms,
    u64 now_seconds) noexcept {
    if (!local_draft_preview_request_valid(request) || !now_ms || !now_seconds)
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    AccountMediaWorkItem work{};
    work.kind = AccountMediaWorkKind::local_draft_preview;
    work.key = request.key;
    work.monotonic_now_ms = now_ms;
    work.unix_now_seconds = now_seconds;
    work.catalog_scope = request.owner_scope;
    work.catalog_generation = request.owner_generation;
    work.local_content = request.content;
    work.local_snapshot = request.snapshot;
    work.source_message_id = request.key.id;
    work.source_message_revision = request.key.variant;
    work.video_device = request.video_device;
    work.text_bytes = static_cast<u16>(request.path.count * sizeof(wchar_t));
    static_assert((kLocalDraftPreviewPathMaximum + 1) * sizeof(wchar_t) <= kAccountMediaWorkerTextCapacity);
    if (work.text_bytes)
        CopyMemory(work.text, request.path.data, work.text_bytes);
    // Never reuse a live atlas/cache key for a different source or owner.
    if (worker && worker->initialized)
        for (auto& lane : worker->lanes) {
            const auto phase = load_phase(&lane);
            const auto& prior =
                phase == AccountMediaWorkerPhase::output_ready ? lane.completed : lane.command;
            if (phase != AccountMediaWorkerPhase::idle && phase != AccountMediaWorkerPhase::leased &&
                prior.kind == work.kind && media_asset_key_equal(prior.key, work.key) &&
                !same_work(prior, work))
                return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
        }
    return queue(worker, work);
}

AccountMediaRequestResult account_media_worker_retire_local_preview(
    AccountMediaWorker* worker, const AccountMediaLocalPreviewRequest& request) noexcept {
    if (!worker || !worker->initialized || !exact_local_request(request, false))
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    bool pending = false;
    for (auto& lane : worker->lanes) {
        AcquireSRWLockExclusive(lane.poster_lock.native());
        const auto phase = load_phase(&lane);
        const auto& prior = phase == AccountMediaWorkerPhase::output_ready ? lane.completed : lane.command;
        if (!exact_local_owner(prior, request) || account_media_video_reserved(worker, lane.index)) {
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            continue;
        }
        InterlockedExchange(&lane.local_retired, 1);
        worker->poster_present_mask &= ~(1u << lane.index);
        if (phase == AccountMediaWorkerPhase::working || phase == AccountMediaWorkerPhase::command_ready) {
            pending = true;
            if (phase == AccountMediaWorkerPhase::working)
                thread_cancel_synchronous_io(lane.thread);
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            continue;
        }
        // Publication already released the caller's source retain. The lane
        // owns all remaining cleanup, so retirement needs no UI retry timer.
        if (phase == AccountMediaWorkerPhase::poster_fence_wait ||
            phase == AccountMediaWorkerPhase::poster_release_ready ||
            phase == AccountMediaWorkerPhase::poster_releasing) {
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            continue;
        }
        if (phase == AccountMediaWorkerPhase::output_ready) {
            lane.media.frame_ready = false;
            lane.completed_result = request_result(AccountMediaRequestState::failed);
        }
        // Before registration, normal D3D deferred destruction preserves
        // queued GPU uses. The worker releases without an artificial Present.
        if ((phase == AccountMediaWorkerPhase::output_ready || phase == AccountMediaWorkerPhase::idle ||
             phase == AccountMediaWorkerPhase::leased || phase == AccountMediaWorkerPhase::poster_present_pending) &&
            (account_media_local_video_resident(&lane.media) || lane.poster_fence || lane.poster_context)) {
            store_phase(&lane, AccountMediaWorkerPhase::poster_release_ready);
            const bool signaled = event_set(lane.command_event) != FALSE;
            if (!signaled)
                store_phase(&lane, AccountMediaWorkerPhase::failed);
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            if (!signaled)
                return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
            continue;
        }
        if (phase == AccountMediaWorkerPhase::output_ready)
            store_phase(&lane, AccountMediaWorkerPhase::idle);
        ReleaseSRWLockExclusive(lane.poster_lock.native());
    }
    return request_result(pending ? AccountMediaRequestState::pending : AccountMediaRequestState::ready);
}

AccountMediaRequestResult account_media_worker_local_preview_uploaded(
    AccountMediaWorker* worker, const AccountMediaLocalPreviewRequest& request) noexcept {
    if (!worker || !worker->initialized || !exact_local_request(request, true))
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    if (account_media_worker_stop_requested(worker))
        return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
    for (auto& lane : worker->lanes) {
        if (!exact_local_owner(lane.command, request) || lane.command.lease_serial != request.lease_serial ||
            account_media_video_reserved(worker, lane.index))
            continue;
        AcquireSRWLockExclusive(lane.poster_lock.native());
        const auto phase = load_phase(&lane);
        // A malformed early receipt must not read completed while publish()
        // still writes it. These phases all follow the publication barrier.
        if ((phase != AccountMediaWorkerPhase::leased && phase != AccountMediaWorkerPhase::idle &&
             !poster_phase(phase)) || lane.completed.lease_serial != request.lease_serial) {
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
        }
        if (phase == AccountMediaWorkerPhase::poster_present_pending) {
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            return request_result(AccountMediaRequestState::pending);
        }
        if (poster_phase(phase) || phase == AccountMediaWorkerPhase::idle || ReadAcquire(&lane.local_retired)) {
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            return request_result(AccountMediaRequestState::ready);
        }
        const bool valid = phase == AccountMediaWorkerPhase::leased && lane.command.video_device &&
                           media_asset_catalog_format(request.key) == MediaAttachmentFormat::mp4 &&
                           lane.completed_result.state == AccountMediaRequestState::ready &&
                           account_media_local_video_resident(&lane.media) &&
                           media_asset_key_equal(lane.media.mp4_key, request.key) &&
                           lane.media.mp4_decoder && lane.media.mp4_decoder->ready && lane.media.mp4_decoder->gpu;
        if (!valid) {
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
        }
        if (worker->poster_disabled_device == lane.command.video_device) {
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            return request_result(AccountMediaRequestState::ready);
        }
        worker->poster_present_mask |= 1u << lane.index;
        store_phase(&lane, AccountMediaWorkerPhase::poster_present_pending);
        ReleaseSRWLockExclusive(lane.poster_lock.native());
        return request_result(AccountMediaRequestState::pending);
    }
    return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
}

AccountMediaRequestResult account_media_worker_local_previews_presented(AccountMediaWorker* worker) noexcept {
    if (!worker || !worker->initialized)
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    if (account_media_worker_stop_requested(worker))
        return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
    u32 pending = worker->poster_present_mask;
    worker->poster_present_mask = 0;
    for (u32 index = 0; pending && index < kAccountMediaDecodeLaneCount; ++index) {
        const u32 bit = 1u << index;
        if (!(pending & bit))
            continue;
        pending &= ~bit;
        auto& lane = worker->lanes[index];
        AcquireSRWLockExclusive(lane.poster_lock.native());
        if (load_phase(&lane) != AccountMediaWorkerPhase::poster_present_pending) {
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            continue;
        }
        auto* device = lane.command.video_device;
        if (device == worker->poster_disabled_device) {
            store_phase(&lane, AccountMediaWorkerPhase::leased);
            ReleaseSRWLockExclusive(lane.poster_lock.native());
            continue;
        }
        // The fence and the context interface that signals it, made together on the presenting device.
        HRESULT result = gpu_fence_create(device, &lane.poster_context, &lane.poster_fence);
        // The host called this only after a real successful Present. Signal
        // therefore does not split the pending conversion/draw command batch.
        if (SUCCEEDED(result))
            result = gpu_fence_signal(lane.poster_context, lane.poster_fence, 1);
        if (SUCCEEDED(result)) {
            store_phase(&lane, AccountMediaWorkerPhase::poster_fence_wait);
            result = gpu_fence_notify(lane.poster_fence, 1, lane.command_event);
        }
        lane.poster_registered = SUCCEEDED(result);
        if (FAILED(result)) {
            // No registered event exists on this path. Retain baseline
            // lifetime, including inactive fence refs, until worker reuse,
            // retirement, or stop. Do not release the decoder on the UI.
            worker->poster_disabled_device = device;
            store_phase(&lane, AccountMediaWorkerPhase::leased);
        }
        // Immediate fence completion can wake the worker before registration
        // returns. This lock keeps all final resource releases on that worker.
        ReleaseSRWLockExclusive(lane.poster_lock.native());
    }
    return request_result(AccountMediaRequestState::ready);
}

bool account_media_worker_take_frame(AccountMediaWorker* worker, AccountMediaFrame* frame) noexcept {
    if (!worker || !worker->initialized || !frame) {
        return false;
    }
    const LONG preferred = worker->preferred_lane;
    worker->preferred_lane = -1;
    const u32 first =
        static_cast<u32>(preferred) < kAccountMediaDecodeLaneCount ? static_cast<u32>(preferred) : 0;
    for (u32 offset = 0; offset < kAccountMediaDecodeLaneCount; ++offset) {
        AccountMediaWorkerLane& lane = worker->lanes[(first + offset) % kAccountMediaDecodeLaneCount];
        if (!frame_available(&lane))
            continue;
        if (InterlockedCompareExchange(&lane.phase, phase_value(AccountMediaWorkerPhase::leased),
                                       phase_value(AccountMediaWorkerPhase::output_ready)) !=
            phase_value(AccountMediaWorkerPhase::output_ready)) {
            continue;
        }
        if (!account_media_take_frame(&lane.media, frame)) {
            store_phase(&lane, AccountMediaWorkerPhase::idle);
            return false;
        }
        frame->content_identity = lane.completed_content_identity;
        if (frame->source_kind == AccountMediaSourceKind::local_draft_preview &&
            frame->kind == AccountMediaFrameKind::video_nv12_gpu)
            frame->lease_serial = lane.completed.lease_serial;
        for (u32 remaining = 0; remaining < kAccountMediaDecodeLaneCount; ++remaining) {
            if (frame_available(&worker->lanes[remaining])) {
                event_set(worker->output_event);
                break;
            }
        }
        return true;
    }
    return false;
}

bool account_media_worker_healthy(const AccountMediaWorker* worker) noexcept {
    if (!worker || !worker->initialized) {
        return false;
    }
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        const AccountMediaWorkerPhase phase = load_phase(&worker->lanes[index]);
        if (phase == AccountMediaWorkerPhase::empty || phase == AccountMediaWorkerPhase::stopped ||
            phase == AccountMediaWorkerPhase::failed) {
            return false;
        }
    }
    return true;
}

HANDLE account_media_worker_notification_handle(const AccountMediaWorker* worker) noexcept {
    return worker && worker->initialized ? worker->output_event : nullptr;
}

void account_media_worker_expire_retries(AccountMediaWorker* worker, u64 now_ms) noexcept {
    if (!worker || !worker->initialized)
        return;
    // A deadline wakes the UI once even when its image left the viewport.
    // Visible work is retried when requested again; invisible failures must not spin the wait loop.
    for (auto& retry : worker->retries) {
        if (retry.retry_at_ms && retry.retry_at_ms <= now_ms)
            reset_value(&retry);
    }
}

u64 account_media_worker_next_wake_at_ms(const AccountMediaWorker* worker) noexcept {
    if (!worker || !worker->initialized) {
        return 0;
    }
    u64 earliest = 0;
    // The retry table belongs to the UI thread and holds only work that is waiting
    // for retry. Completed workers keep their output for frame leasing, so do not
    // cache a wake deadline in a worker: an expired deadline would stay live after
    // the worker goes idle and turn the application wait into a zero-timeout scan loop.
    for (u32 index = 0; index < kAccountMediaWorkerRetryCapacity; ++index) {
        const u64 wake = worker->retries[index].retry_at_ms;
        if (wake != 0 && (earliest == 0 || wake < earliest)) {
            earliest = wake;
        }
    }
    return earliest;
}

bool account_media_worker_stop(AccountMediaWorker* worker, u32 timeout_ms) noexcept {
    if (!worker || !worker->initialized || timeout_ms == 0) {
        return false;
    }
    const u64 started_at_ms = clock_monotonic_milliseconds();
    const u64 deadline_ms = started_at_ms + timeout_ms;
    if (!event_set(worker->stop_event)) {
        return false;
    }
    for (auto& lane : worker->lanes)
        account_media_cancel_provider(&lane.media);
    // Also wakes a selected owner waiting for a joined-release retry after the
    // manual-reset stop event has already been consumed.
    for (auto& lane : worker->lanes)
        if (lane.command_event && account_media_video_reserved(worker, lane.index))
            event_set(lane.command_event);
    // Only started lanes have a thread to join.
    Thread* threads[kAccountMediaDecodeLaneCount]{};
    u32 started = 0;
    for (const auto& lane : worker->lanes) {
        if (!lane.thread)
            continue;
        threads[started++] = lane.thread;
        if (load_phase(&lane) == AccountMediaWorkerPhase::working &&
            lane.command.kind == AccountMediaWorkKind::local_draft_preview)
            thread_cancel_synchronous_io(lane.thread);
    }
    if (started && !threads_wait_all(threads, started, timeout_ms)) {
        return false;
    }
    // A wait failure cannot make a still-registered event safe to close.
    // Keep the existing host/module ownership if any lane could not drain it.
    for (const auto& lane : worker->lanes)
        if (lane.poster_registered)
            return false;
    if (!account_media_video_destroy(worker))
        return false;
    // Shared-cache workers must release their pipelines and clients before the
    // owner frees the one encoded backing allocation.
    for (u32 index = kAccountMediaDecodeLaneCount; index != 0; --index) {
        if (!stop_lane_media(&worker->lanes[index - 1], deadline_ms)) {
            return false;
        }
    }
    for (const auto& lane : worker->lanes) {
        if (lane.collection_titles) pages_release(lane.collection_titles);
        if (lane.thread)
            thread_close(lane.thread);
        waitable_close(lane.command_event);
    }
    waitable_close(worker->output_event);
    waitable_close(worker->stop_event);
    SecureZeroMemory(worker, sizeof(*worker));
    return true;
}

} // namespace maicord
