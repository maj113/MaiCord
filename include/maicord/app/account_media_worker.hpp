#pragma once

#include "maicord/app/account_media.hpp"
#include "maicord/app/account_video_playback.hpp"
#include "maicord/core/win32_records.hpp"

struct ID3D11DeviceContext4;
struct ID3D11Fence;

namespace maicord {

inline constexpr u32 kAccountMediaWorkerTextCapacity = kCdnMaximumTargetBytes + 64;
inline constexpr u32 kAccountMediaWorkerStackBytes = 1024u * 1024u;
inline constexpr u32 kAccountMediaDecodeLaneCount = 8;
inline constexpr u32 kAccountMediaWorkerRetryCapacity = 128;
inline constexpr u32 kAccountMediaWorkerRetryWays = 4;

enum class AccountMediaWorkKind : u8 {
    none,
    static_asset,
    guild_icon,
    attachment,
    animation_advance,
    catalog_preview,
    local_draft_preview,
    collection_title,
};

enum class AccountMediaWorkerPhase : u8 {
    empty,
    idle,
    command_ready,
    working,
    output_ready,
    leased,
    stopped,
    failed,
    selected_video,
    poster_present_pending,
    poster_fence_wait,
    poster_release_ready,
    poster_releasing,
};

struct AccountMediaWorkItem {
    ID3D11Device* video_device;
    MediaAssetKey key;
    u64 monotonic_now_ms;
    u64 unix_now_seconds;
    u64 source_message_id;
    u32 source_message_revision;
    u32 frame_index;
    u16 proxy_width;
    u16 proxy_height;
    u16 cache_generation;
    u16 text_bytes;
    CdnImageType image_type;
    AccountMediaWorkKind kind;
    u8 progressive;
    u8 collection;
    bool title_batch;
    u16 collection_track;
    u64 catalog_scope;
    MediaTrackId title_track;
    u32 catalog_generation;
    u32 catalog_reserved;
    UploadSource local_content;
    FileSnapshot local_snapshot;
    u64 lease_serial;
    char text[kAccountMediaWorkerTextCapacity];
};

struct AccountMediaWorker;
struct AccountMediaVideoRuntime;

struct AccountMediaWorkerLane {
    AccountMedia media;
    AccountMediaWorkItem command;
    AccountMediaWorkItem completed;
    AccountMediaRequestResult completed_result;
    // Transient packed batch names, owned by this lane until completion/stop.
    char* collection_titles;
    u32 collection_title_bytes;
    u32 collection_title_committed;
    u64 completed_content_identity;
    AccountMediaWorker* owner;
    HANDLE thread;
    HANDLE command_event;
    // The UI holds this lock through fence registration. The worker takes
    // cleanup ownership only after the registration call returns.
    SrwLock poster_lock;
    ID3D11DeviceContext4* poster_context;
    ID3D11Fence* poster_fence;
    volatile LONG phase;
    u32 thread_id;
    u32 index;
    volatile LONG local_retired;
    bool initialized;
    bool local_retained;
    bool poster_registered;
    u8 reserved;
};

struct AccountMediaWorkerCounters {
    volatile LONG64 queued;
};

struct AccountMediaWorkerRetry {
    MediaAssetKey key;
    u64 retry_at_ms;
};

static_assert(sizeof(AccountMediaWorkerRetry) == 24);

// Eight bounded media lanes share one immutable, leased encoded-byte cache.
// Each lane owns its parser workspaces and scanline output, so cold decodes can
// run concurrently without any UI-thread codec work or per-lane source-cache
// duplication. One aggregate completion event keeps the account ABI compact.
// A lane starts on its first command: its thread (and, except for lane 0,
// which owns the shared cache, its decode storage) is created then, on the
// admitting UI thread, so lanes that never receive work cost nothing.
struct AccountMediaWorker {
    AccountMediaWorkerLane lanes[kAccountMediaDecodeLaneCount];
    // UI-thread admission cache shared by every decode lane.  Per-lane codec
    // caches protect the decoder itself; this table prevents cached failures
    // from being re-enqueued through a different lane every paint pump.
    AccountMediaWorkerRetry retries[kAccountMediaWorkerRetryCapacity];
    HANDLE stop_event;
    HANDLE output_event;
    // Admission and frame leasing run on the UI thread.
    LONG preferred_lane;
    AccountMediaVideoRuntime* video_runtime;
    AccountMediaWorkerCounters counters;
    // UI-owned admission serial and pre-Present mask. A borrowed device stays
    // alive until account stop, as required by local preview admission.
    u64 next_lease_serial;
    u32 poster_present_mask;
    volatile LONG poster_admission_waiter;
    ID3D11Device* poster_disabled_device;
    bool initialized;
    u8 reserved[7];
};

// Creates the events and lane 0's cache and storage; starts no lane thread.
AccountMediaRequestResult account_media_worker_initialize(AccountMediaWorker* worker) noexcept;
// Starts an idle lane ahead of its first command (storage and thread), as
// admission does. Fails for a busy or stopping worker or a lane out of range.
AccountMediaRequestResult account_media_worker_start_lane(AccountMediaWorker* worker, u32 index) noexcept;
// Lane for an animation frame: the idle lane whose last command decoded this
// asset (its pipeline retains the container parse), else an idle lane holding
// no other animation (started ones first), else kAccountMediaDecodeLaneCount.
u32 account_media_worker_animation_lane(const AccountMediaWorker* worker, MediaAssetKey key) noexcept;
AccountMediaRequestResult account_media_worker_request_static_asset(AccountMediaWorker* worker,
                                                                    MediaAssetKey key, u64 monotonic_now_ms,
                                                                    u64 unix_now_seconds) noexcept;
AccountMediaRequestResult account_media_worker_request_guild_icon(
    AccountMediaWorker* worker, const AccountMediaGuildIconRequest& request, u64 monotonic_now_ms,
    u64 unix_now_seconds) noexcept;
AccountMediaRequestResult account_media_worker_request_attachment(
    AccountMediaWorker* worker, const AccountMediaAttachmentRequest& request, u64 monotonic_now_ms,
    u64 unix_now_seconds) noexcept;
AccountMediaRequestResult account_media_worker_collection_title(AccountMediaWorker*,
    const AccountMediaCollectionTitleRequest&, MediaTrackTitle*) noexcept;
AccountMediaRequestResult account_media_worker_request_catalog_preview(
    AccountMediaWorker* worker, const AccountMediaCatalogPreviewRequest& request, u64 monotonic_now_ms,
    u64 unix_now_seconds) noexcept;
AccountMediaRequestResult account_media_worker_request_local_preview(AccountMediaWorker*,
                                                                     const AccountMediaLocalPreviewRequest&,
                                                                     u64 monotonic_now_ms,
                                                                     u64 unix_now_seconds) noexcept;
// Marks only this exact owner/key. Returns pending while a retained source is
// still in use; completion or account stop performs the release on its lane.
AccountMediaRequestResult account_media_worker_retire_local_preview(
    AccountMediaWorker*, const AccountMediaLocalPreviewRequest&) noexcept;
AccountMediaRequestResult account_media_worker_local_preview_uploaded(
    AccountMediaWorker*, const AccountMediaLocalPreviewRequest&) noexcept;
// The caller must first complete an actual successful renderer Present.
AccountMediaRequestResult account_media_worker_local_previews_presented(AccountMediaWorker*) noexcept;
AccountMediaRequestResult account_media_worker_advance_animation(AccountMediaWorker* worker,
                                                                 MediaAssetKey key, u16 cache_generation,
                                                                 u32 frame_index,
                                                                 u64 unix_now_seconds) noexcept;
bool account_media_worker_take_frame(AccountMediaWorker* worker, AccountMediaFrame* frame) noexcept;
bool account_media_worker_healthy(const AccountMediaWorker* worker) noexcept;
HANDLE account_media_worker_notification_handle(const AccountMediaWorker* worker) noexcept;
void account_media_worker_expire_retries(AccountMediaWorker* worker, u64 now_ms) noexcept;
u64 account_media_worker_next_wake_at_ms(const AccountMediaWorker* worker) noexcept;
bool account_media_worker_stop(AccountMediaWorker* worker, u32 timeout_ms) noexcept;
AccountVideoResult account_media_worker_video_select(AccountMediaWorker*,
                                                     const AccountVideoSelection&) noexcept;
AccountVideoResult account_media_worker_video_control(AccountMediaWorker*,
                                                      const AccountVideoControl&) noexcept;
AccountVideoPlaybackStatus account_media_worker_video_status(const AccountMediaWorker*) noexcept;
bool account_media_worker_video_take(AccountMediaWorker*, AccountVideoFrame*) noexcept;
bool account_media_worker_video_acknowledge(AccountMediaWorker*, const AccountVideoFrame&) noexcept;

static_assert(sizeof(AccountMediaWorkItem) == kAccountMediaWorkerTextCapacity + 176);

} // namespace maicord
