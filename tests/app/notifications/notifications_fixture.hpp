#pragma once

// Target-only forced include of MaiCordApplicationNotificationTests: the notification owner
// (src/app/notifications/notifications.cpp) reads the test's clock, and commits and releases pages through the test,
// which records the retention banks and can refuse them. Linked libraries keep the real calls.
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/notifications/retention.hpp"

namespace maicord::notification_fixture {

u64 now() noexcept;
// kind: 1 the retention rows, 2 a block of u32 (the retention index), 0 anything else.
void* pages(usize bytes, u32 kind) noexcept;
bool release(void* base) noexcept;
template <typename T> inline constexpr u32 page_kind = 0;
template <> inline constexpr u32 page_kind<NotificationRetentionRow> = 1;
template <> inline constexpr u32 page_kind<u32> = 2;
inline void* commit_pages(usize bytes) noexcept {
    return pages(bytes, 0);
}
template <typename T> inline T* commit_pages(usize bytes = sizeof(T)) noexcept {
    return static_cast<T*>(pages(bytes, page_kind<T>));
}
inline bool release(void* base, usize) noexcept {
    return release(base);
}

} // namespace maicord::notification_fixture

#define clock_monotonic_milliseconds ::maicord::notification_fixture::now
#define commit_pages ::maicord::notification_fixture::commit_pages
#define pages_release ::maicord::notification_fixture::release
