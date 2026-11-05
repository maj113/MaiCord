#pragma once

#include "maicord/ui/accessibility/accessibility.hpp"

namespace maicord::app_detail {

// After each published snapshot: the announcements the application's state calls for (a new message, a connection that
// came or went, a failed send, ...), through the AT-SPI server. Sources start from their current state the first time.
void application_accessibility_announce(const UiAccessTree& tree, u64 now_ms) noexcept;
// Nobody listens: the next announcements start from the then-current state, silently.
void application_accessibility_announce_reset() noexcept;

} // namespace maicord::app_detail
