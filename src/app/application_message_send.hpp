#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

struct RestClientResponse;

namespace app_detail {

bool queue_message_send() noexcept;
// Continues a send after the large-mention confirmation was accepted.
bool application_message_send_confirmed() noexcept;
bool accept_message_send_response(const RestClientResponse& response, Utf8View body) noexcept;
bool consume_message_send_completions() noexcept;
// Requeues the failed message whose CAPTCHA was just solved; true if requeued.
bool application_message_send_captcha_resume() noexcept;

} // namespace app_detail
} // namespace maicord
