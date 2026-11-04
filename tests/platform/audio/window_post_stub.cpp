// window_post for the Linux audio tests, which link no window backend: counts the wakes the audio workers post so a
// test can assert them. The real definition is the window backend's (src/platform/window/...).
#include "maicord/os/linux/interlocked.hpp"
#include "maicord/platform/window/window.hpp"
#include "window_post_stub.hpp"

namespace maicord {
namespace {
volatile LONG g_posts;
volatile LONG g_last_cause = -1;
} // namespace

bool window_post(const Waker& waker, u64, u64) noexcept {
    if (!waker.window)
        return false;
    InterlockedIncrement(&g_posts);
    InterlockedExchange(&g_last_cause, static_cast<LONG>(waker.cause));
    return true;
}

u32 test_window_posts() noexcept {
    return static_cast<u32>(InterlockedCompareExchange(&g_posts, 0, 0));
}

LONG test_window_last_cause() noexcept {
    return InterlockedCompareExchange(&g_last_cause, 0, 0);
}
} // namespace maicord
