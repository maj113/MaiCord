#include "maicord/app/account_media_worker.hpp"
#include "maicord/app/embed_resolver.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "account_media_video_internal.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

constexpr LONG phase_value(AccountMediaWorkerPhase phase) noexcept {
    return static_cast<LONG>(phase);
}

__forceinline AccountMediaWorkerPhase load_phase(const AccountMediaWorkerLane* lane) noexcept {
    return static_cast<AccountMediaWorkerPhase>(ReadAcquire(&lane->phase));
}

__forceinline bool frame_available(const AccountMediaWorkerLane* lane) noexcept {
    return ReadAcquire(&lane->phase) == phase_value(AccountMediaWorkerPhase::output_ready) &&
           lane->completed.kind != AccountMediaWorkKind::collection_title &&
           !(lane->completed.kind == AccountMediaWorkKind::local_draft_preview &&
             ReadAcquire(&lane->local_retired)) &&
           lane->completed_result.state == AccountMediaRequestState::ready;
}

void store_phase(AccountMediaWorkerLane* lane, AccountMediaWorkerPhase phase) noexcept {
    InterlockedExchange(&lane->phase, phase_value(phase));
}

AccountMediaRequestResult request_result(AccountMediaRequestState state,
                                         AccountMediaError error = AccountMediaError::none) noexcept {
    AccountMediaRequestResult result{};
    result.state = state;
    result.error = error;
    return result;
}

// A looping video source that a lane's resident decoder plays in place: a
// gifv embed's MP4 or a VIDEO GIF favourite's picker tile.
bool looping_video_key(MediaAssetKey key) noexcept {
    return (key.kind == MediaAssetKind::catalog_preview &&
            media_asset_catalog_format(key) == MediaAttachmentFormat::mp4) ||
           (key.kind == MediaAssetKind::embed && (key.flags & media_asset_flag_animated) != 0 &&
            media_asset_embed_resource(key) == MediaEmbedResource::video &&
            media_asset_embed_format(key) == MediaAttachmentFormat::mp4);
}
// A resident decoder that published no frame for this long no longer plays.
constexpr u64 kLoopingVideoIdleMs = 1'500;

bool poster_phase(AccountMediaWorkerPhase phase) noexcept {
    return phase == AccountMediaWorkerPhase::poster_present_pending ||
           phase == AccountMediaWorkerPhase::poster_fence_wait ||
           phase == AccountMediaWorkerPhase::poster_release_ready ||
           phase == AccountMediaWorkerPhase::poster_releasing;
}

bool exact_local_request(const AccountMediaLocalPreviewRequest& request, bool uploaded) noexcept {
    return request.owner_scope && request.owner_generation &&
           request.key.kind == MediaAssetKind::local_draft_preview && media_asset_key_valid(request.key) &&
           (uploaded ? request.lease_serial != 0 : request.lease_serial == 0) &&
           !request.reserved && !request.content.context && !request.content.ops && !request.content.bytes &&
           !request.path.data && !request.path.count && !request.snapshot.bytes &&
           !request.snapshot.identity.index && !request.snapshot.identity.last_write &&
           !request.snapshot.identity.volume && !request.snapshot.identity.reserved && !request.video_device;
}

bool exact_local_owner(const AccountMediaWorkItem& work, const AccountMediaLocalPreviewRequest& request) noexcept {
    return work.kind == AccountMediaWorkKind::local_draft_preview &&
           media_asset_key_equal(work.key, request.key) && work.catalog_scope == request.owner_scope &&
           work.catalog_generation == request.owner_generation;
}

// Only the lane worker calls this helper. An armed registration must first
// deliver its actual event; a fence value check does not consume that event.
void release_poster_resources(AccountMediaWorkerLane* lane) noexcept {
    if (lane->poster_fence) {
        lane->poster_fence->Release();
        lane->poster_fence = nullptr;
    }
    if (lane->poster_context) {
        lane->poster_context->Release();
        lane->poster_context = nullptr;
    }
}

bool finish_poster_release(AccountMediaWorkerLane* lane) noexcept {
    account_media_release_video(&lane->media);
    release_poster_resources(lane);
    const bool stopped = WaitForSingleObject(lane->owner->stop_event, 0) == WAIT_OBJECT_0;
    store_phase(lane, stopped ? AccountMediaWorkerPhase::stopped : AccountMediaWorkerPhase::idle);
    // A resident tile needs no UI wake. Notify only admission blocked by a
    // held poster lane. This does not request a frame or start a retry timer.
    if (!stopped && InterlockedExchange(&lane->owner->poster_admission_waiter, 0))
        SetEvent(lane->owner->output_event);
    return stopped;
}

AccountMediaWorkerRetry* find_retry(AccountMediaWorker* worker, MediaAssetKey key) noexcept {
    static_assert(kAccountMediaWorkerRetryCapacity % kAccountMediaWorkerRetryWays == 0);
    constexpr u32 bucket_count = kAccountMediaWorkerRetryCapacity / kAccountMediaWorkerRetryWays;
    static_assert((bucket_count & (bucket_count - 1u)) == 0);
    const u32 begin =
        (static_cast<u32>(media_asset_hash(key)) & (bucket_count - 1u)) * kAccountMediaWorkerRetryWays;
    for (u32 index = begin; index < begin + kAccountMediaWorkerRetryWays; ++index) {
        if (worker->retries[index].retry_at_ms != 0 &&
            media_asset_key_equal(worker->retries[index].key, key)) {
            return &worker->retries[index];
        }
    }
    return nullptr;
}

void remember_retry(AccountMediaWorker* worker, MediaAssetKey key, u64 now_ms, u64 retry_at_ms) noexcept {
    AccountMediaWorkerRetry* slot = find_retry(worker, key);
    if (!slot) {
        constexpr u32 bucket_count = kAccountMediaWorkerRetryCapacity / kAccountMediaWorkerRetryWays;
        const u32 begin =
            (static_cast<u32>(media_asset_hash(key)) & (bucket_count - 1u)) * kAccountMediaWorkerRetryWays;
        slot = &worker->retries[begin];
        for (u32 index = begin; index < begin + kAccountMediaWorkerRetryWays; ++index) {
            if (worker->retries[index].retry_at_ms == 0) {
                slot = &worker->retries[index];
                break;
            }
            if (worker->retries[index].retry_at_ms < slot->retry_at_ms) {
                slot = &worker->retries[index];
            }
        }
    }
    slot->key = key;
    slot->retry_at_ms =
        retry_at_ms != 0 ? retry_at_ms : deadline_after(now_ms, kAccountMediaFailureRetryMs);
}

AccountMediaRequestResult retry_wait_until(u64 retry_at_ms) noexcept {
    AccountMediaRequestResult waiting = request_result(AccountMediaRequestState::retry_wait);
    waiting.retry_at_ms = retry_at_ms;
    return waiting;
}

bool same_work(const AccountMediaWorkItem& left, const AccountMediaWorkItem& right) noexcept {
    if (left.kind != right.kind || !media_asset_key_equal(left.key, right.key)) {
        return false;
    }
    if (left.kind == AccountMediaWorkKind::animation_advance) {
        return left.cache_generation == right.cache_generation && left.frame_index == right.frame_index;
    }
    if (left.kind == AccountMediaWorkKind::attachment) {
        return left.source_message_id == right.source_message_id &&
               left.source_message_revision == right.source_message_revision && left.collection_track == right.collection_track;
    }
    if (left.kind == AccountMediaWorkKind::collection_title)
        return media_track_id_equal(left.title_track, right.title_track) && left.collection_track == right.collection_track &&
            left.source_message_revision == right.source_message_revision;
    if (left.kind == AccountMediaWorkKind::catalog_preview)
        return left.catalog_scope == right.catalog_scope &&
               left.catalog_generation == right.catalog_generation;
    if (left.kind == AccountMediaWorkKind::local_draft_preview) {
        if (left.catalog_scope != right.catalog_scope ||
            left.catalog_generation != right.catalog_generation ||
            left.local_content.context != right.local_content.context ||
            left.local_content.ops != right.local_content.ops ||
            !local_draft_preview_snapshot_equal(left.local_snapshot, right.local_snapshot) ||
            left.text_bytes != right.text_bytes)
            return false;
        for (u32 i = 0; i < left.text_bytes; ++i)
            if (left.text[i] != right.text[i])
                return false;
    }
    return true;
}

void release_local_source(AccountMediaWorkerLane* lane) noexcept {
    if (lane->local_retained) {
        upload_source_release(lane->command.local_content);
        lane->local_retained = false;
    }
}
bool local_retired(const AccountMediaWorkerLane* lane) noexcept {
    return ReadAcquire(&lane->local_retired) ||
           WaitForSingleObject(lane->owner->stop_event, 0) == WAIT_OBJECT_0;
}
bool audio_source_cancelled(void* opaque) noexcept {
    const auto* lane = static_cast<const AccountMediaWorkerLane*>(opaque);
    return lane && WaitForSingleObject(lane->owner->stop_event, 0) == WAIT_OBJECT_0;
}
// Exact random-access reads for the composer video poster; a retirement or
// account stop cancels between blocks (and CancelSynchronousIo within one).
struct LocalPreviewReader {
    AccountMediaWorkerLane* lane;
    HANDLE file;
    UploadSource content;
};
bool read_local_preview(void* opaque, u64 offset, Slice<u8> output) noexcept {
    auto& reader = *static_cast<LocalPreviewReader*>(opaque);
    if (local_retired(reader.lane) || !output.data || !output.count)
        return false;
    u32 done = 0;
    while (done < output.count) {
        if (reader.content.context) {
            const auto part = reader.content.ops->read(reader.content.context, offset + done,
                                                       {output.data + done, output.count - done});
            if (!part.data || !part.count || part.count > output.count - done)
                return false;
            if (part.data != output.data + done)
                CopyMemory(output.data + done, part.data, part.count);
            done += part.count;
            continue;
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset + done);
        DWORD copied = 0;
        if (!SetFilePointerEx(reader.file, position, nullptr, FILE_BEGIN) ||
            !ReadFile(reader.file, output.data + done, output.count - done, &copied, nullptr) || !copied)
            return false;
        done += copied;
    }
    return true;
}

AccountMediaRequestResult execute_local_preview(AccountMediaWorkerLane* lane,
                                                const AccountMediaWorkItem& work) noexcept {
    if (local_retired(lane))
        return request_result(AccountMediaRequestState::failed);
    const AccountMediaLocalPreviewRequest request{
        work.local_content,
        work.local_snapshot,
        work.local_content.context ? ConstSlice<wchar_t>{}
                                   : ConstSlice<wchar_t>{reinterpret_cast<const wchar_t*>(work.text),
                                                         work.text_bytes / sizeof(wchar_t)},
        work.key,
        work.catalog_scope,
        work.catalog_generation,
        0,
        work.video_device,
        0};
    const bool video = media_asset_catalog_format(work.key) == MediaAttachmentFormat::mp4;
    HANDLE file = INVALID_HANDLE_VALUE;
    const HRESULT apartment =
        work.local_content.context ? CoInitializeEx(nullptr, COINIT_MULTITHREADED) : S_FALSE;
    bool valid = !work.local_content.context || SUCCEEDED(apartment);
    FileSnapshot found{};
    if (valid && !work.local_content.context) {
        file = CreateFileW(reinterpret_cast<const wchar_t*>(work.text), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, video ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
        valid = file != INVALID_HANDLE_VALUE && file_snapshot_read(file, &found) &&
                local_draft_preview_snapshot_equal(found, work.local_snapshot);
    }
    if (video) {
        // Posters stream the movie header and first samples, never the file.
        AccountMediaRequestResult result =
            request_result(AccountMediaRequestState::failed, AccountMediaError::invalid_input);
        if (valid && !local_retired(lane)) {
            LocalPreviewReader reader{lane, file, work.local_content};
            result = account_media_decode_local_video_preview(&lane->media, request, read_local_preview, &reader);
        }
        if (file != INVALID_HANDLE_VALUE) {
            // A file replaced while it was read publishes nothing.
            if (result.state == AccountMediaRequestState::ready &&
                (!file_snapshot_read(file, &found) ||
                 !local_draft_preview_snapshot_equal(found, work.local_snapshot))) {
                lane->media.frame_ready = false;
                account_media_release_video(&lane->media);
                result = request_result(AccountMediaRequestState::failed, AccountMediaError::invalid_input);
            }
            CloseHandle(file);
        }
        if (work.local_content.context && SUCCEEDED(apartment))
            CoUninitialize();
        return result;
    }
    if (valid && !local_retired(lane)) {
        const auto cached = account_media_decode_local_preview(&lane->media, request, {});
        if (cached.state == AccountMediaRequestState::ready || cached.error != AccountMediaError::cache) {
            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);
            if (work.local_content.context && SUCCEEDED(apartment))
                CoUninitialize();
            return cached;
        }
        if (work.local_content.context && lane->local_retained && !local_retired(lane)) {
            const auto borrowed = work.local_content.ops->read(
                work.local_content.context, 0, {nullptr, static_cast<u32>(work.local_snapshot.bytes)});
            if (borrowed.data && borrowed.count == work.local_snapshot.bytes) {
                // The lane retains immutable storage through synchronous decode.
                // Published frames use decoded output or cached source bytes.
                const auto decoded = local_retired(lane)
                    ? request_result(AccountMediaRequestState::failed, AccountMediaError::invalid_input)
                    : account_media_decode_local_preview(&lane->media, request, borrowed);
                CoUninitialize();
                return decoded;
            }
        }
    }
    auto* bytes =
        valid ? static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(work.local_snapshot.bytes),
                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
              : nullptr;
    if (!bytes) {
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        if (work.local_content.context && SUCCEEDED(apartment))
            CoUninitialize();
        return request_result(AccountMediaRequestState::failed,
                              valid ? AccountMediaError::memory : AccountMediaError::invalid_input);
    }
    u32 offset = 0;
    while (valid && offset < work.local_snapshot.bytes && !local_retired(lane)) {
        const u32 count = minimum<u32>(64u * 1024u, static_cast<u32>(work.local_snapshot.bytes) - offset);
        if (work.local_content.context) {
            const auto part =
                work.local_content.ops->read(work.local_content.context, offset, {bytes + offset, count});
            valid = part.data && part.count && part.count <= count;
            if (valid) {
                if (part.data != bytes + offset)
                    CopyMemory(bytes + offset, part.data, part.count);
                offset += part.count;
            }
        } else {
            DWORD copied = 0;
            valid = ReadFile(file, bytes + offset, count, &copied, nullptr) && copied == count;
            if (valid)
                offset += copied;
        }
    }
    if (file != INVALID_HANDLE_VALUE) {
        valid = valid && file_snapshot_read(file, &found) &&
                local_draft_preview_snapshot_equal(found, work.local_snapshot);
        CloseHandle(file);
    }
    AccountMediaRequestResult result =
        request_result(AccountMediaRequestState::failed, AccountMediaError::invalid_input);
    if (valid && offset == work.local_snapshot.bytes && !local_retired(lane)) {
        result = account_media_decode_local_preview(&lane->media, request, {bytes, offset});
    }
    SecureZeroMemory(bytes, static_cast<SIZE_T>(work.local_snapshot.bytes));
    VirtualFree(bytes, 0, MEM_RELEASE);
    if (work.local_content.context && SUCCEEDED(apartment))
        CoUninitialize();
    return result;
}

AccountMediaRequestResult execute(AccountMediaWorkerLane* lane, AccountMediaWorkItem& work) noexcept {
    switch (work.kind) {
    case AccountMediaWorkKind::collection_title: {
        auto* resolver = lane->media.provider_resolver;
        if (!resolver) {
            resolver = static_cast<EmbedResolver*>(VirtualAlloc(nullptr, sizeof(EmbedResolver), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!resolver) return request_result(AccountMediaRequestState::failed, AccountMediaError::memory);
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&lane->media.provider_resolver), resolver);
        }
        if (WaitForSingleObject(lane->owner->stop_event, 0) == WAIT_OBJECT_0) embed_resolver_cancel(resolver);
        MediaTrackTitle title{}; EmbedResolveResult fetched{};
        if (work.title_batch) {
            SoundcloudTitleBatch batch{};
            if (work.text_bytes != sizeof(batch)) return request_result(AccountMediaRequestState::invalid,AccountMediaError::invalid_input);
            copy_bytes(&batch,work.text,sizeof(batch)); work.text_bytes=0;
            struct Output { AccountMediaWorkerLane* lane; const SoundcloudTitleBatch* batch; } output{lane,&batch};
            if (lane->collection_titles) VirtualFree(lane->collection_titles,0,MEM_RELEASE);
            lane->collection_titles=nullptr; lane->collection_title_bytes=lane->collection_title_committed=0;
            const MediaTrackTitleSink sink{&output,+[](void* opaque,u16 index,const MediaTrackTitle& title) noexcept {
                auto& o=*static_cast<Output*>(opaque); auto& lane=*o.lane;
                const u32 required=lane.collection_title_bytes+12+title.bytes;
                constexpr u32 capacity=32768;
                if (required>capacity) return false;
                if (!lane.collection_titles) lane.collection_titles=static_cast<char*>(VirtualAlloc(nullptr,capacity,MEM_RESERVE,PAGE_READWRITE));
                if (!lane.collection_titles) return false;
                if (required>lane.collection_title_committed) {
                    const u32 committed=(required+4095)&~4095u;
                    if (!VirtualAlloc(lane.collection_titles,committed,MEM_COMMIT,PAGE_READWRITE)) return false;
                    lane.collection_title_committed=committed;
                }
                u64 id=0; for (u32 i=0;i<o.batch->count;++i) if (o.batch->tracks[i].index==index) { id=o.batch->tracks[i].id; break; }
                if (!id) return false;
                char* p=lane.collection_titles+lane.collection_title_bytes;
                copy_bytes(p,&index,2); copy_bytes(p+2,&title.bytes,2); copy_bytes(p+4,&id,8); copy_bytes(p+12,title.text,title.bytes);
                lane.collection_title_bytes=required; return true;
            }};
            fetched=soundcloud_batch_titles_resolve(resolver,batch,work.collection_track,sink);
            if (!fetched.ok()) lane->collection_title_bytes=0;
        } else {
            fetched = work.title_track.youtube_track ? youtube_title_resolve(resolver, {work.title_track.youtube, 11}, &title) : soundcloud_title_resolve(resolver, work.title_track.numeric, &title);
            work.text_bytes=fetched.ok()?title.bytes:0; copy_bytes(work.text,title.text,work.text_bytes);
        }
        auto result = request_result(fetched.ok() ? AccountMediaRequestState::ready : AccountMediaRequestState::failed,
            fetched.ok() ? AccountMediaError::none : AccountMediaError::http);
        if (!fetched.ok()) result.retry_at_ms = deadline_after(clock_monotonic_milliseconds(),
            fetched.retry_after_seconds ? u64{fetched.retry_after_seconds} * 1000 : kAccountMediaFailureRetryMs);
        return result;
    }
    case AccountMediaWorkKind::local_draft_preview:
        return execute_local_preview(lane, work);
    case AccountMediaWorkKind::static_asset:
        return account_media_request_static_asset(&lane->media, work.key, work.monotonic_now_ms,
                                                  work.unix_now_seconds);
    case AccountMediaWorkKind::guild_icon: {
        // proxy_width carries the hash-addressed CDN size (zero: default).
        const AccountMediaGuildIconRequest request{Utf8View{work.text, work.text_bytes}, work.key,
                                                   {work.proxy_width, 0}};
        return account_media_request_guild_icon(&lane->media, request, work.monotonic_now_ms,
                                                work.unix_now_seconds);
    }
    case AccountMediaWorkKind::attachment: {
        const AccountMediaAttachmentRequest request{Utf8View{work.text, work.text_bytes},
                                                    work.key,
                                                    work.source_message_id,
                                                    work.source_message_revision,
                                                    work.proxy_width,
                                                    work.proxy_height,
                                                    work.image_type,
                                                    {work.progressive, {0, 0}, work.collection_track},
                                                    work.video_device};
        return account_media_request_attachment(&lane->media, request, work.monotonic_now_ms,
                                                work.unix_now_seconds);
    }
    case AccountMediaWorkKind::catalog_preview: {
        const AccountMediaCatalogPreviewRequest request{{work.text, work.text_bytes},
                                                        work.key,
                                                        work.catalog_scope,
                                                        work.catalog_generation,
                                                        work.proxy_width,
                                                        work.proxy_height,
                                                        media_asset_catalog_format(work.key),
                                                        {0, 0, 0, 0},
                                                        work.video_device};
        return account_media_request_catalog_preview(&lane->media, request, work.monotonic_now_ms,
                                                     work.unix_now_seconds);
    }
    case AccountMediaWorkKind::animation_advance:
        return account_media_advance_animation(&lane->media, work.key, work.cache_generation,
                                               work.frame_index, work.unix_now_seconds);
    default:
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
}

AccountMediaRequestResult asynchronous_failure(const AccountMediaWorkerLane* lane) noexcept {
    if (lane->media.last_failure.error != AccountMediaError::none) {
        return lane->media.last_failure;
    }
    return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
}

bool wait_for_network(AccountMediaWorkerLane* lane, AccountMediaRequestResult* result) noexcept {
    while (lane->media.active) {
        const HANDLE notification = account_media_notification_handle(&lane->media);
        if (!notification) {
            *result = asynchronous_failure(lane);
            return false;
        }
        HANDLE handles[2]{lane->owner->stop_event, notification};
        const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, lane->media.progressive_continue ? 0 : INFINITE);
        if (waited == WAIT_OBJECT_0) {
            return false;
        }
        if (waited != WAIT_OBJECT_0 + 1 && waited != WAIT_TIMEOUT) {
            *result = asynchronous_failure(lane);
            return false;
        }
        if (!account_media_pump(&lane->media, clock_monotonic_milliseconds(),
                                clock_unix_milliseconds() / 1'000)) {
            *result = asynchronous_failure(lane);
            return false;
        }
    }
    if (lane->media.frame_ready) {
        *result = request_result(AccountMediaRequestState::ready);
        return true;
    }
    *result = asynchronous_failure(lane);
    return false;
}

void publish(AccountMediaWorkerLane* lane, const AccountMediaWorkItem& completed,
             AccountMediaRequestResult result) noexcept {
    if (completed.kind == AccountMediaWorkKind::local_draft_preview) {
        release_local_source(lane);
        if (local_retired(lane)) {
            lane->media.frame_ready = false;
            result = request_result(AccountMediaRequestState::failed);
        }
    }
    // Cache locks and content matching stay on the codec worker. The UI
    // consumes only the identity delivered with this completion.
    lane->completed_content_identity = 0;
    if (result.state == AccountMediaRequestState::ready &&
        lane->media.frame_kind == AccountMediaFrameKind::audio_source) {
        lane->completed_content_identity = lane->media.audio_source.content_identity;
    } else if (result.state == AccountMediaRequestState::ready && lane->media.frame_cache_generation != 0 &&
        lane->media.frame_kind != AccountMediaFrameKind::video_nv12_gpu) {
        const auto encoded = encoded_media_cache_find(lane->media.pipeline.cache, lane->media.frame_key, 0);
        if (encoded.found() && encoded.generation == lane->media.frame_cache_generation)
            lane->completed_content_identity = encoded.content_identity;
    }
    if (completed.collection) {
        const auto collection = account_media_collection_state(&lane->media, completed.key, false);
        result.collection_count = collection.count;
        result.collection_track = collection.track;
        result.collection_more = collection.more;
    }
    lane->completed = completed;
    lane->completed_result = result;
    store_phase(lane, AccountMediaWorkerPhase::output_ready);
    if (result.state == AccountMediaRequestState::ready)
        InterlockedIncrement64(&lane->owner->counters.ready);
    SetEvent(lane->owner->output_event);
}

DWORD WINAPI media_worker_entry(void* opaque) noexcept {
    auto* lane = static_cast<AccountMediaWorkerLane*>(opaque);
    HANDLE handles[2]{lane->owner->stop_event, lane->command_event};
    for (;;) {
        const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (account_media_video_reserved(lane->owner, lane->index)) {
            bool stopped = false;
            if (account_media_video_dispatch(lane, &stopped)) {
                if (stopped)
                    return load_phase(lane) == AccountMediaWorkerPhase::failed ? ERROR_INVALID_STATE : 0;
                continue;
            }
        }
        if (poster_phase(load_phase(lane))) {
            AcquireSRWLockExclusive(lane->poster_lock.native());
            const auto phase = load_phase(lane);
            if (phase == AccountMediaWorkerPhase::poster_fence_wait) {
                ReleaseSRWLockExclusive(lane->poster_lock.native());
                // Stop never signals this event. Only the registered fence owns
                // it until this wait consumes the notification, including when
                // stop and GPU completion become ready at the same time.
                const DWORD completion = waited == WAIT_OBJECT_0
                                             ? WaitForSingleObject(lane->command_event, INFINITE)
                                             : waited == WAIT_OBJECT_0 + 1 ? WAIT_OBJECT_0 : WAIT_FAILED;
                if (completion != WAIT_OBJECT_0) {
                    store_phase(lane, AccountMediaWorkerPhase::failed);
                    SetEvent(lane->owner->output_event);
                    return ERROR_INVALID_STATE;
                }
                AcquireSRWLockExclusive(lane->poster_lock.native());
                lane->poster_registered = false;
                store_phase(lane, AccountMediaWorkerPhase::poster_releasing);
                ReleaseSRWLockExclusive(lane->poster_lock.native());
                if (finish_poster_release(lane))
                    return 0;
                continue;
            }
            if (phase == AccountMediaWorkerPhase::poster_release_ready &&
                (waited == WAIT_OBJECT_0 || waited == WAIT_OBJECT_0 + 1)) {
                store_phase(lane, AccountMediaWorkerPhase::poster_releasing);
                ReleaseSRWLockExclusive(lane->poster_lock.native());
                if (finish_poster_release(lane))
                    return 0;
                continue;
            }
            ReleaseSRWLockExclusive(lane->poster_lock.native());
        }
        if (waited == WAIT_OBJECT_0) {
            release_local_source(lane);
            account_media_release_audio_source(&lane->media);
            account_media_release_video(&lane->media);
            release_poster_resources(lane);
            store_phase(lane, AccountMediaWorkerPhase::stopped);
            return 0;
        }
        if (waited != WAIT_OBJECT_0 + 1 ||
            InterlockedCompareExchange(&lane->phase, phase_value(AccountMediaWorkerPhase::working),
                                       phase_value(AccountMediaWorkerPhase::command_ready)) !=
                phase_value(AccountMediaWorkerPhase::command_ready)) {
            account_media_release_audio_source(&lane->media);
            account_media_release_video(&lane->media);
            release_poster_resources(lane);
            store_phase(lane, AccountMediaWorkerPhase::failed);
            release_local_source(lane);
            SetEvent(lane->owner->output_event);
            return ERROR_INVALID_STATE;
        }

        // A new request cannot replace the command while this worker is busy.
        AccountMediaWorkItem& work = lane->command;
        account_media_release_audio_source(&lane->media);
        account_media_trim_scanlines(&lane->media);
        // A composer poster was copied into the atlas when its frame was taken;
        // its decoder and sparse source never outlive the next lane command.
        if (account_media_local_video_resident(&lane->media))
            account_media_release_video(&lane->media);
        release_poster_resources(lane);
        AccountMediaRequestResult result = execute(lane, work);
        if (result.state == AccountMediaRequestState::queued ||
            result.state == AccountMediaRequestState::pending) {
            if (!wait_for_network(lane, &result) &&
                WaitForSingleObject(lane->owner->stop_event, 0) == WAIT_OBJECT_0) {
                account_media_release_audio_source(&lane->media);
                account_media_release_video(&lane->media);
                store_phase(lane, AccountMediaWorkerPhase::stopped);
                return 0;
            }
        }
        publish(lane, work, result);
    }
}

// Lane 0 owns the encoded cache every lane shares; the other lanes bind to it.
AccountMediaRequestResult initialize_lane_media(AccountMediaWorker* worker,
                                                AccountMediaWorkerLane& lane) noexcept {
    const AccountMediaRequestResult initialized =
        lane.index == 0 ? account_media_initialize(&lane.media)
                        : account_media_initialize_shared(&lane.media, &worker->lanes[0].media.cache);
    if (!initialized.ok())
        return initialized;
    lane.initialized = true;
    lane.media.audio_cancelled = audio_source_cancelled;
    lane.media.audio_cancel_context = &lane;
    return request_result(AccountMediaRequestState::ready);
}

// A lane starts on its first command: it commits its decode storage (lane 0
// does at initialization) and creates its thread. Lanes that never receive
// work cost neither, so a light session runs on one or two decode threads.
AccountMediaRequestResult start_lane(AccountMediaWorker* worker, AccountMediaWorkerLane& lane) noexcept {
    if (!lane.initialized) {
        const AccountMediaRequestResult initialized = initialize_lane_media(worker, lane);
        if (!initialized.ok())
            return initialized;
    }
    if (!lane.thread) {
        DWORD thread_id = 0;
        lane.thread = CreateThread(nullptr, kAccountMediaWorkerStackBytes, media_worker_entry, &lane,
                                   STACK_SIZE_PARAM_IS_A_RESERVATION, &thread_id);
        if (!lane.thread)
            return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
        lane.thread_id = static_cast<u32>(thread_id);
    }
    return request_result(AccountMediaRequestState::ready);
}

void reclaim_leased_lanes(AccountMediaWorker* worker) noexcept {
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        if (account_media_video_reserved(worker, index))
            continue;
        InterlockedCompareExchange(&worker->lanes[index].phase, phase_value(AccountMediaWorkerPhase::idle),
                                   phase_value(AccountMediaWorkerPhase::leased));
    }
}

bool collection_readable(AccountMediaWorkerPhase phase) noexcept {
    return phase == AccountMediaWorkerPhase::idle || phase == AccountMediaWorkerPhase::leased ||
        phase == AccountMediaWorkerPhase::output_ready || phase == AccountMediaWorkerPhase::selected_video;
}
AccountMedia* collection_title_owner(AccountMediaWorker* worker, const AccountMediaCollectionTitleRequest& request,
    MediaTrackTitle* title, MediaTrackId* id, u64* retry) noexcept {
    for (auto& lane : worker->lanes) if (collection_readable(load_phase(&lane))) {
        if (account_media_collection_title(&lane.media, request, title, id, retry)) return &lane.media;
    }
    return nullptr;
}
void collect_collection_titles(AccountMediaWorker* worker) noexcept {
    for (auto& lane : worker->lanes) {
        if (load_phase(&lane) != AccountMediaWorkerPhase::output_ready || lane.completed.kind != AccountMediaWorkKind::collection_title) continue;
        const auto& work = lane.completed;
        AccountMediaCollectionTitleRequest request{work.key, work.source_message_revision, work.collection_track};
        MediaTrackTitle title{}, cached{}; MediaTrackId id{}; u64 retry{};
        if (auto* owner=collection_title_owner(worker,request,&cached,&id,&retry); owner && media_track_id_equal(id,work.title_track)) {
            bool found=false;
            if (work.title_batch && lane.completed_result.state==AccountMediaRequestState::ready) {
                for (u32 at=0;at+12<=lane.collection_title_bytes;) {
                    u16 index{},bytes{}; MediaTrackId item{};
                    copy_bytes(&index,lane.collection_titles+at,2); copy_bytes(&bytes,lane.collection_titles+at+2,2); copy_bytes(&item.numeric,lane.collection_titles+at+4,8); at+=12;
                    if (bytes>lane.collection_title_bytes-at || bytes>sizeof(title.text)) break;
                    title.bytes=bytes; copy_bytes(title.text,lane.collection_titles+at,bytes); at+=bytes;
                    auto query=request; query.track=index;
                    const bool stored=account_media_collection_title_store(owner,query,item,title,0);
                    found=found || (stored && index==request.track);
                }
            } else if (!work.title_batch && lane.completed_result.state==AccountMediaRequestState::ready && work.text_bytes<=sizeof(title.text)) {
                title.bytes=work.text_bytes; copy_bytes(title.text,work.text,title.bytes);
                found=account_media_collection_title_store(owner,request,id,title,0);
            }
            if (!found) account_media_collection_title_store(owner,request,id,{},lane.completed_result.retry_at_ms ?
                lane.completed_result.retry_at_ms : deadline_after(clock_monotonic_milliseconds(),kAccountMediaFailureRetryMs));
        }
        if (lane.collection_titles) VirtualFree(lane.collection_titles,0,MEM_RELEASE);
        lane.collection_titles=nullptr; lane.collection_title_bytes=lane.collection_title_committed=0;
        lane.media.frame_ready = false;
        store_phase(&lane, AccountMediaWorkerPhase::idle);
    }
}
AccountMediaRequestResult queue(AccountMediaWorker* worker, const AccountMediaWorkItem& work) noexcept {
    if (!worker || !worker->initialized || work.kind == AccountMediaWorkKind::none ||
        !media_asset_key_valid(work.key)) {
        return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
    }
    if (WaitForSingleObject(worker->stop_event, 0) == WAIT_OBJECT_0)
        return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
    collect_collection_titles(worker);
    AccountMediaWorkerRetry* retry = work.collection ? nullptr : find_retry(worker, work.key);
    const u64 retry_now_ms =
        retry && work.monotonic_now_ms == 0 ? clock_monotonic_milliseconds() : work.monotonic_now_ms;
    if (retry && retry_now_ms < retry->retry_at_ms)
        return retry_wait_until(retry->retry_at_ms);
    if (retry) {
        reset_value<AccountMediaWorkerRetry>(retry);
    }
    reclaim_leased_lanes(worker);

    bool poster_busy = false;
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        AccountMediaWorkerLane& lane = worker->lanes[index];
        AccountMediaWorkerPhase phase = load_phase(&lane);
        if (work.kind == AccountMediaWorkKind::attachment && work.collection &&
            lane.command.kind == AccountMediaWorkKind::attachment && lane.command.collection && media_asset_key_equal(lane.command.key, work.key)) {
            // A track switch cannot run on a spare lane while the previous
            // selection still owns the source, decoder or PCM reader.
            if (phase == AccountMediaWorkerPhase::selected_video ||
                ((phase == AccountMediaWorkerPhase::working || phase == AccountMediaWorkerPhase::command_ready) &&
                 !same_work(lane.command, work))) return request_result(AccountMediaRequestState::busy);
            if (phase == AccountMediaWorkerPhase::output_ready && !same_work(lane.completed, work) &&
                lane.completed_result.state == AccountMediaRequestState::ready) {
                lane.media.frame_ready = false;
                store_phase(&lane, AccountMediaWorkerPhase::idle); phase = AccountMediaWorkerPhase::idle;
            }
        }
        if (poster_phase(phase)) {
            poster_busy = true;
            if (same_work(lane.command, work)) {
                // Publish before the phase recheck. Cleanup either sees this
                // waiter or the UI observes idle and admits the request below.
                InterlockedExchange(&worker->poster_admission_waiter, 1);
                phase = load_phase(&lane);
                if (poster_phase(phase))
                    return request_result(AccountMediaRequestState::pending);
            }
        }
        if (phase == AccountMediaWorkerPhase::output_ready &&
            lane.completed.kind == AccountMediaWorkKind::local_draft_preview &&
            ReadAcquire(&lane.local_retired)) {
            lane.media.frame_ready = false;
            lane.completed_result = request_result(AccountMediaRequestState::failed);
            store_phase(&lane, AccountMediaWorkerPhase::idle);
            phase = AccountMediaWorkerPhase::idle;
        }
        if (phase == AccountMediaWorkerPhase::output_ready && same_work(lane.completed, work)) {
            const AccountMediaRequestResult completed = lane.completed_result;
            if (completed.retry_at_ms == 0 || work.monotonic_now_ms < completed.retry_at_ms ||
                completed.state == AccountMediaRequestState::ready) {
                if (completed.state == AccountMediaRequestState::ready) {
                    worker->preferred_lane = static_cast<LONG>(index);
                } else if (completed.state != AccountMediaRequestState::retry_wait) {
                    // A failed asynchronous completion is reported once. Deliver its diagnostics,
                    // then expose the worker as retry_wait until its deadline; repeating the
                    // original failure on every visibility scan would flood the log and invalidate
                    // healthy frames.
                    lane.completed_result.state = AccountMediaRequestState::retry_wait;
                }
                if (!work.collection && completed.state != AccountMediaRequestState::ready &&
                    completed.state != AccountMediaRequestState::retry_wait &&
                    (work.monotonic_now_ms != 0 || completed.error == AccountMediaError::decode)) {
                    // A bad on-demand animation frame must not immediately
                    // restart/redownload through another worker. Cache misses
                    // still permit a normal reload of an evicted source.
                    remember_retry(worker, work.key,
                                   work.monotonic_now_ms != 0 ? work.monotonic_now_ms
                                                              : clock_monotonic_milliseconds(),
                                   completed.retry_at_ms);
                }
                return completed;
            }
            if (InterlockedCompareExchange(&lane.phase, phase_value(AccountMediaWorkerPhase::idle),
                                           phase_value(AccountMediaWorkerPhase::output_ready)) ==
                phase_value(AccountMediaWorkerPhase::output_ready)) {
                phase = AccountMediaWorkerPhase::idle;
            }
        }
        if ((phase == AccountMediaWorkerPhase::command_ready ||
             phase == AccountMediaWorkerPhase::working) &&
            same_work(lane.command, work)) {
            return request_result(AccountMediaRequestState::pending);
        }
    }

    // Set this before the final idle-lane scan so the last cleanup cannot
    // finish between a failed admission scan and waiter publication.
    if (poster_busy)
        InterlockedExchange(&worker->poster_admission_waiter, 1);

    if (work.kind == AccountMediaWorkKind::animation_advance) {
        for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
            AccountMediaWorkerLane& lane = worker->lanes[index];
            if (poster_phase(load_phase(&lane)))
                continue;
            if (!account_media_video_matches(&lane.media, work.key, work.cache_generation)) {
                continue;
            }
            if (load_phase(&lane) != AccountMediaWorkerPhase::idle)
                return request_result(AccountMediaRequestState::busy);
            if (const auto started = start_lane(worker, lane); !started.ok())
                return started;
            lane.command = work;
            if (InterlockedCompareExchange(&lane.phase, phase_value(AccountMediaWorkerPhase::command_ready),
                                           phase_value(AccountMediaWorkerPhase::idle)) !=
                phase_value(AccountMediaWorkerPhase::idle))
                return request_result(AccountMediaRequestState::busy);
            if (!SetEvent(lane.command_event)) {
                store_phase(&lane, AccountMediaWorkerPhase::failed);
                return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
            }
            InterlockedIncrement64(&worker->counters.queued);
            return request_result(AccountMediaRequestState::queued);
        }
    }

    // A different visible asset may reach this worker before the failed asset
    // is revisited. Keep the failed asset's cooldown before reusing the worker;
    // otherwise the fixed scan order can repeatedly queue the same cached failure.
    // Successful outputs keep their borrowed frame until media_take consumes it.
    u64 reclaimed_at_ms = 0;
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        AccountMediaWorkerLane& lane = worker->lanes[index];
        if (load_phase(&lane) == AccountMediaWorkerPhase::output_ready &&
            lane.completed_result.state != AccountMediaRequestState::ready) {
            if (!reclaimed_at_ms)
                reclaimed_at_ms =
                    work.monotonic_now_ms ? work.monotonic_now_ms : clock_monotonic_milliseconds();
            const auto& completed = lane.completed_result;
            const bool failure = completed.state == AccountMediaRequestState::invalid ||
                                 completed.state == AccountMediaRequestState::unsupported ||
                                 completed.state == AccountMediaRequestState::failed ||
                                 completed.state == AccountMediaRequestState::retry_wait;
            // Expired URLs and evicted animation sources need a normal
            // refresh/reload rather than a new negative-cache cooldown.
            const bool retain =
                failure && (lane.completed.kind != AccountMediaWorkKind::animation_advance ||
                            completed.error == AccountMediaError::decode);
            if (retain && !lane.completed.collection && (!completed.retry_at_ms || completed.retry_at_ms > reclaimed_at_ms))
                remember_retry(worker, lane.completed.key, reclaimed_at_ms,
                               lane.completed_result.retry_at_ms);
            InterlockedCompareExchange(&lane.phase, phase_value(AccountMediaWorkerPhase::idle),
                                       phase_value(AccountMediaWorkerPhase::output_ready));
        }
    }
    // The reclaimed output may use another request kind for this same asset.
    // Its newly preserved cooldown must apply to this request as well.
    if (reclaimed_at_ms && !work.collection) {
        retry = find_retry(worker, work.key);
        if (retry && reclaimed_at_ms < retry->retry_at_ms)
            return retry_wait_until(retry->retry_at_ms);
    }

    const bool animation = work.kind != AccountMediaWorkKind::collection_title &&
        (work.kind == AccountMediaWorkKind::animation_advance || (work.key.flags & media_asset_flag_animated) != 0);
    const u32 lane_count = animation ? kAccountMediaDecodeLaneCount : kAccountMediaDecodeLaneCount - 1;
    const bool flag_origin = work.key.kind == MediaAssetKind::regional_flag;
    // A new looping video source replaces its lane's resident decoder. Take a
    // lane without one (or already holding this source), else the lane whose
    // decoder stopped playing longest ago. While every idle lane still plays a
    // video, report busy instead of evicting one: visible videos beyond the
    // lane count wait for a decoder rather than restarting each other forever.
    u32 video_lane = kAccountMediaDecodeLaneCount;
    if (work.collection && work.kind == AccountMediaWorkKind::attachment) for (u32 index = animation ? 0 : 1; index < kAccountMediaDecodeLaneCount; ++index) {
        const auto& lane = worker->lanes[index];
        if (load_phase(&lane) == AccountMediaWorkerPhase::idle &&
            ((lane.command.collection && media_asset_key_equal(lane.command.key, work.key)) ||
             account_media_collection_state(&lane.media, work.key).count)) { video_lane = index; break; }
    }
    if (video_lane == kAccountMediaDecodeLaneCount && work.kind != AccountMediaWorkKind::animation_advance && work.kind != AccountMediaWorkKind::collection_title && looping_video_key(work.key)) {
        const u64 now_ms = clock_monotonic_milliseconds();
        u64 oldest = ~0ull;
        for (u32 index = animation ? 0 : 1; index < kAccountMediaDecodeLaneCount; ++index) {
            const AccountMediaWorkerLane& lane = worker->lanes[index];
            if (load_phase(&lane) != AccountMediaWorkerPhase::idle)
                continue;
            const Mp4Decoder* decoder = lane.media.mp4_decoder;
            if (!decoder || !decoder->ready || media_asset_key_equal(lane.media.mp4_key, work.key)) {
                video_lane = index;
                break;
            }
            const u64 used = lane.media.mp4_used_ms;
            if (now_ms - used >= kLoopingVideoIdleMs && used < oldest) {
                oldest = used;
                video_lane = index;
            }
        }
        if (video_lane == kAccountMediaDecodeLaneCount)
            return request_result(AccountMediaRequestState::busy);
    }
    // Animation frames keep to the lane holding their source's parse instead of
    // first-idle order, where alternating sources re-parse on every frame.
    if (video_lane == kAccountMediaDecodeLaneCount && work.kind == AccountMediaWorkKind::animation_advance)
        video_lane = account_media_worker_animation_lane(worker, work.key);
    for (u32 affinity = 0; affinity < 2; ++affinity)
        for (u32 order = 0; order < lane_count; ++order) {
            // Worker 0 is the latency reserve for animated media. Animated work
            // may spill into every idle worker; non-animated images use workers 1..3
            // so three large still decodes cannot delay the next emoji/PFP frame.
            const u32 index = animation ? order : order + 1;
            if (video_lane != kAccountMediaDecodeLaneCount && index != video_lane)
                continue;
            AccountMediaWorkerLane& lane = worker->lanes[index];
            const bool same_origin =
                (lane.command.key.kind == MediaAssetKind::regional_flag) == flag_origin;
            if (same_origin != (affinity == 0))
                continue;
            if (load_phase(&lane) != AccountMediaWorkerPhase::idle) {
                continue;
            }
            if (work.kind == AccountMediaWorkKind::local_draft_preview &&
                worker->next_lease_serial == ~u64{0})
                return request_result(AccountMediaRequestState::invalid, AccountMediaError::invalid_input);
            if (const auto started = start_lane(worker, lane); !started.ok())
                return started;
            lane.command = work;
            if (work.kind == AccountMediaWorkKind::local_draft_preview)
                lane.command.lease_serial = ++worker->next_lease_serial;
            InterlockedExchange(&lane.local_retired, 0);
            if (work.kind == AccountMediaWorkKind::local_draft_preview && work.local_content.context) {
                upload_source_retain(work.local_content);
                lane.local_retained = true;
            }
            if (InterlockedCompareExchange(&lane.phase, phase_value(AccountMediaWorkerPhase::command_ready),
                                           phase_value(AccountMediaWorkerPhase::idle)) !=
                phase_value(AccountMediaWorkerPhase::idle)) {
                release_local_source(&lane);
                continue;
            }
            if (!SetEvent(lane.command_event)) {
                store_phase(&lane, AccountMediaWorkerPhase::failed);
                release_local_source(&lane);
                return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
            }
            InterlockedIncrement64(&worker->counters.queued);
            return request_result(AccountMediaRequestState::queued);
        }
    return request_result(AccountMediaRequestState::busy);
}

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
            CloseHandle(lane.command_event);
        }
    }
    if (worker->output_event) {
        CloseHandle(worker->output_event);
    }
    if (worker->stop_event) {
        CloseHandle(worker->stop_event);
    }
    SecureZeroMemory(worker, sizeof(*worker));
}

} // namespace

AccountMediaRequestResult account_media_worker_collection_title(AccountMediaWorker* worker,
    const AccountMediaCollectionTitleRequest& request, MediaTrackTitle* out) noexcept {
    if (!worker || !worker->initialized || !out || !request.track || !media_asset_key_valid(request.key) ||
        WaitForSingleObject(worker->stop_event, 0) == WAIT_OBJECT_0)
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
    worker->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker->output_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker->preferred_lane = -1;
    if (!worker->stop_event || !worker->output_event) {
        cleanup_failed_initialize(worker);
        return request_result(AccountMediaRequestState::failed, AccountMediaError::client);
    }

    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        AccountMediaWorkerLane& lane = worker->lanes[index];
        lane.owner = worker;
        lane.index = index;
        lane.command_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
        WaitForSingleObject(worker->stop_event, 0) == WAIT_OBJECT_0)
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
                CancelSynchronousIo(lane.thread);
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
            const bool signaled = SetEvent(lane.command_event) != FALSE;
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
    if (WaitForSingleObject(worker->stop_event, 0) == WAIT_OBJECT_0)
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
    if (WaitForSingleObject(worker->stop_event, 0) == WAIT_OBJECT_0)
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
        ID3D11DeviceContext* context = nullptr;
        ID3D11Device5* device5 = nullptr;
        device->GetImmediateContext(&context);
        HRESULT result = context ? context->QueryInterface(__uuidof(ID3D11DeviceContext4),
            reinterpret_cast<void**>(&lane.poster_context)) : E_NOINTERFACE;
        if (SUCCEEDED(result))
            result = device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5));
        if (SUCCEEDED(result))
            result = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence),
                                         reinterpret_cast<void**>(&lane.poster_fence));
        if (device5) device5->Release();
        if (context) context->Release();
        // The host called this only after a real successful Present. Signal
        // therefore does not split the pending conversion/draw command batch.
        if (SUCCEEDED(result))
            result = lane.poster_context->Signal(lane.poster_fence, 1);
        if (SUCCEEDED(result)) {
            store_phase(&lane, AccountMediaWorkerPhase::poster_fence_wait);
            result = lane.poster_fence->SetEventOnCompletion(1, lane.command_event);
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
                SetEvent(worker->output_event);
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
    if (!SetEvent(worker->stop_event)) {
        return false;
    }
    for (auto& lane : worker->lanes)
        account_media_cancel_provider(&lane.media);
    // Also wakes a selected owner waiting for a joined-release retry after the
    // manual-reset stop event has already been consumed.
    for (auto& lane : worker->lanes)
        if (lane.command_event && account_media_video_reserved(worker, lane.index))
            SetEvent(lane.command_event);
    // Only started lanes have a thread to join.
    HANDLE threads[kAccountMediaDecodeLaneCount]{};
    u32 started = 0;
    for (const auto& lane : worker->lanes) {
        if (!lane.thread)
            continue;
        threads[started++] = lane.thread;
        if (load_phase(&lane) == AccountMediaWorkerPhase::working &&
            lane.command.kind == AccountMediaWorkKind::local_draft_preview)
            CancelSynchronousIo(lane.thread);
    }
    if (started && WaitForMultipleObjects(started, threads, TRUE, timeout_ms) != WAIT_OBJECT_0) {
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
        if (lane.collection_titles) VirtualFree(lane.collection_titles,0,MEM_RELEASE);
        if (lane.thread)
            CloseHandle(lane.thread);
        CloseHandle(lane.command_event);
    }
    CloseHandle(worker->output_event);
    CloseHandle(worker->stop_event);
    SecureZeroMemory(worker, sizeof(*worker));
    return true;
}

} // namespace maicord
