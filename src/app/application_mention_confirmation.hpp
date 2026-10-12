#pragma once

namespace maicord::app_detail {

// Opens the official client's confirmation before a large @everyone/@here send in a guild
// channel when the active draft needs it; "Send now" calls
// application_message_send_confirmed(). True unless an internal failure.
bool application_mention_confirmation_request(bool* opened) noexcept;

} // namespace maicord::app_detail
