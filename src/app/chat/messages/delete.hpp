#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

struct RestClientResponse;
struct MessageRecord;

namespace app_detail {

struct MessageDeleteRequest {
    u64 channel_id;
    u64 message_id;
    u64 retry_at_ms;
};

void reset_message_delete() noexcept;
bool message_can_delete(const MessageRecord& record) noexcept;
bool application_message_delete_pending_available() noexcept;
bool queue_message_delete(u64 message_id) noexcept;
// Opens a confirmation (Cancel focused) unless skipped with Shift.
bool application_message_delete_request(u64 message_id, bool skip_confirmation) noexcept;
bool accept_message_delete_response(const RestClientResponse& response) noexcept;

} // namespace app_detail
} // namespace maicord
