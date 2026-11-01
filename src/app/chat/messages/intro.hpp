#pragma once
#include "maicord/core/base.hpp"

namespace maicord::app_detail {
// Projects the official client's channel-beginning header for the selected channel into
// the message scene: shown once the resident window starts at the channel's
// first message (or the channel is empty, or its history is unreadable).
void application_message_intro_synchronize() noexcept;
} // namespace maicord::app_detail
