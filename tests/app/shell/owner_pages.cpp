// A large feature owner (the size of the Server Settings one) is wiped page by page on release: the pages it wrote
// are cleared, the pages it never wrote stay out of memory.
#include "../../../src/app/shell/owner.hpp"

#if defined(_WIN32)
#include "maicord/sys/process_info.hpp"
#endif

namespace maicord {
namespace {

constexpr usize kOwnerPages = 688;
constexpr usize kPage = 4096;
struct LargeOwner {
    u8 bytes[kOwnerPages * kPage];
};
constexpr usize kWrittenPages[]{0, 1, 37, 400, kOwnerPages - 1};
constexpr usize kReadPage = 200, kUntouchedPage = 600;

// Whether the page is in memory: in the working set on Windows, present or swapped on Linux.
bool page_in_memory(const void* page) noexcept {
#if defined(_WIN32)
    PSAPI_WORKING_SET_EX_INFORMATION info{};
    info.VirtualAddress = const_cast<void*>(page);
    return QueryWorkingSetEx(GetCurrentProcess(), &info, sizeof(info)) && info.VirtualAttributes.Valid;
#else
    const long file = sys_open("/proc/self/pagemap", kOpenReadOnly);
    u64 entry = 0;
    const bool read = !syscall_failed(file) &&
                      sys_pread(static_cast<int>(file), &entry, sizeof(entry),
                                reinterpret_cast<usize>(page) / process_startup().page_size * sizeof(u64)) ==
                          static_cast<long>(sizeof(entry));
    if (!syscall_failed(file))
        sys_close(static_cast<int>(file));
    return read && (entry & (3ull << 62));
#endif
}

#if defined(_WIN32)
u64 page_faults() noexcept {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.PageFaultCount : 0;
}
#endif

} // namespace

void test_owner_pages(void (*check)(bool, const char*) noexcept) noexcept {
    LargeOwner* owner = app_detail::owner_allocate<LargeOwner>();
    check(owner != nullptr, "owner_pages_allocate");
    if (!owner)
        return;
    for (const usize page : kWrittenPages)
        for (usize at = 0; at < kPage; at += 512)
            owner->bytes[page * kPage + at] = 0xA5;
    volatile u8 read = owner->bytes[kReadPage * kPage];
    (void)read;
#if defined(_WIN32)
    const u64 faults = page_faults();
#endif
    app_detail::owner_wipe_written(owner, sizeof(LargeOwner));
#if defined(_WIN32)
    // The written pages are resident already: the wipe itself takes no fault.
    check(page_faults() - faults <= 1, "owner_wipe_faults_no_page_in");
#endif
    bool cleared = true;
    for (const usize page : kWrittenPages)
        for (usize at = 0; at < kPage; ++at)
            cleared = cleared && owner->bytes[page * kPage + at] == 0;
    check(cleared, "owner_wipe_clears_every_written_page");
    check(!page_in_memory(owner->bytes + kUntouchedPage * kPage), "owner_wipe_leaves_untouched_pages_out_of_memory");
    app_detail::owner_release(owner);
    check(owner == nullptr, "owner_release_clears_the_pointer");

    // Pages allocated without the write watch are wiped whole on Windows; on Linux the written ones are present.
    u8* plain = commit_pages<u8>(4 * kPage);
    check(plain != nullptr, "owner_wipe_plain_pages_allocate");
    if (plain) {
        for (usize index = 0; index < 4 * kPage; ++index)
            plain[index] = 0x5A;
        app_detail::owner_wipe_written(plain, 4 * kPage);
        bool zero = true;
        for (usize index = 0; index < 4 * kPage; ++index)
            zero = zero && plain[index] == 0;
        check(zero, "owner_wipe_clears_a_range_without_write_watch");
        pages_release(plain, 4 * kPage);
    }
}

} // namespace maicord
