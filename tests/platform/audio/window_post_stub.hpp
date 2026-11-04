#pragma once

#include "maicord/core/base.hpp"
#include "maicord/os/wait.hpp"

namespace maicord {
// How many wakes window_post (window_post_stub.cpp) has accepted, and the WakeCause of the last.
u32 test_window_posts() noexcept;
LONG test_window_last_cause() noexcept;
} // namespace maicord
