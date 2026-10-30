#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct RestClientResponse;

namespace app_detail {

void reset_message_edit() noexcept;
void end_message_edit_request() noexcept;
bool cancel_message_edit_editor() noexcept;
bool application_message_edit_available(u64 message_id) noexcept;
// Newest resident message you can edit in the composer channel; zero when none.
u64 application_message_edit_latest_own() noexcept;
bool application_message_edit_pending_available() noexcept;
bool application_message_edit_retains_content() noexcept;
bool begin_message_edit(u64 message_id) noexcept;
bool queue_message_edit() noexcept;
// Edits without opening the editor (composer s/old/new/).
bool queue_message_edit_content(u64 message_id, Utf8View content) noexcept;
bool accept_message_edit_response(const RestClientResponse& response) noexcept;

} // namespace app_detail
} // namespace maicord
