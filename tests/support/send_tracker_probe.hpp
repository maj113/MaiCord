#pragma once

#include "maicord/app/chat/messages/send_tracker.hpp"

namespace maicord {

// Attempts still waiting on a delivery outcome.
inline u32 send_tracker_active(const MessageSendTracker& tracker) noexcept {
    u32 count = 0;
    for (const MessageSendAttempt& attempt : tracker.attempts) count += attempt.active;
    return count;
}

} // namespace maicord
