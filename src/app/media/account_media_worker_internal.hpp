#pragma once
// Shared by account_media_worker.cpp and its parts (account_media_worker_*.cpp); private to them.
#include "maicord/app/media/account_media_worker.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/decode/mp4_decoder.hpp"
#include "account_media_video_internal.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace account_media_worker_detail {}
using namespace account_media_worker_detail;
namespace account_media_worker_detail {

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

void store_phase(AccountMediaWorkerLane* lane, AccountMediaWorkerPhase phase) noexcept;
AccountMediaRequestResult request_result(AccountMediaRequestState state,
                                         AccountMediaError error = AccountMediaError::none) noexcept;
bool poster_phase(AccountMediaWorkerPhase phase) noexcept;
bool exact_local_request(const AccountMediaLocalPreviewRequest& request, bool uploaded) noexcept;
bool exact_local_owner(const AccountMediaWorkItem& work, const AccountMediaLocalPreviewRequest& request) noexcept;
AccountMediaRequestResult retry_wait_until(u64 retry_at_ms) noexcept;
bool same_work(const AccountMediaWorkItem& left, const AccountMediaWorkItem& right) noexcept;
AccountMediaRequestResult initialize_lane_media(AccountMediaWorker* worker,
                                                AccountMediaWorkerLane& lane) noexcept;
AccountMediaRequestResult start_lane(AccountMediaWorker* worker, AccountMediaWorkerLane& lane) noexcept;
AccountMedia* collection_title_owner(AccountMediaWorker* worker, const AccountMediaCollectionTitleRequest& request,
    MediaTrackTitle* title, MediaTrackId* id, u64* retry) noexcept;
void collect_collection_titles(AccountMediaWorker* worker) noexcept;
AccountMediaRequestResult queue(AccountMediaWorker* worker, const AccountMediaWorkItem& work) noexcept;

} // namespace account_media_worker_detail
} // namespace maicord
