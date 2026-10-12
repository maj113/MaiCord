#pragma once
#include "maicord/app/account_media.hpp"

namespace maicord::app_detail {
// A permanent negative-cache entry cannot produce a selected decoder. Keep
// finite retries pending, but retire loading for terminal source failures.
inline bool application_video_source_pending(const AccountMediaRequestResult& result) noexcept {
    return result.state != AccountMediaRequestState::invalid &&
           result.state != AccountMediaRequestState::unsupported &&
           result.state != AccountMediaRequestState::failed &&
           !(result.state == AccountMediaRequestState::retry_wait && result.retry_at_ms == ~u64{0});
}
} // namespace maicord::app_detail
