#include "../support/test_output.hpp"
#include "../support/test_main_shared.hpp"

#include "maicord/core/arena.hpp"
#include "maicord/core/byte_queue.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/spsc_queue.hpp"
#include "maicord/core/spsc_mailbox.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/virtual_memory.hpp"
#if defined(_WIN32)
#include "../support/secure_text_store_access.hpp"
#include "maicord/protocol/user/private_conversation.hpp"
#include "maicord/core/win32_exception.hpp"
#else
#include "maicord/core/data_path.hpp"
#include "maicord/sys/linux/path.hpp"
#endif

namespace maicord {
namespace {

using RuntimeCopyFunction = void*(__cdecl*)(void*, const void*, size_t);
using RuntimeSetFunction = void*(__cdecl*)(void*, int, size_t);
RuntimeCopyFunction volatile g_runtime_copy = &memcpy;
RuntimeCopyFunction volatile g_runtime_move = &memmove;
RuntimeSetFunction volatile g_runtime_set = &memset;
alignas(64) u8 g_runtime_reference[4096];
alignas(64) u8 g_runtime_candidate[4096];
alignas(64) u8 g_runtime_temporary[2048];

#if defined(_WIN32)
// Whether the arena's pages are in the state its initialization promised: the committed prefix (or the whole
// reservation, reserved only, when nothing is committed) and, when the prefix is shorter, a reserved tail.
bool arena_regions_match(const Arena& arena, bool any_committed, usize expected_committed,
                         usize expected_reserved) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    bool matches = VirtualQuery(arena.base, &region, sizeof(region)) != 0 && region.AllocationBase == arena.base &&
                   region.State == static_cast<DWORD>(any_committed ? MEM_COMMIT : MEM_RESERVE) &&
                   region.RegionSize == (any_committed ? expected_committed : expected_reserved);
    if (expected_committed && expected_committed < expected_reserved) {
        matches = matches && VirtualQuery(arena.base + expected_committed, &region, sizeof(region)) != 0 &&
                  region.AllocationBase == arena.base && region.State == MEM_RESERVE &&
                  region.RegionSize == expected_reserved - expected_committed;
    }
    return matches;
}
#else
struct Mapping {
    usize start, end;
    char permissions[5];
};

// The mapping of /proc/self/maps that holds `address`. The kernel merges neighbouring mappings of equal
// permissions, so callers compare containment where a neighbour could have been merged.
bool find_mapping(usize address, Mapping* mapping) noexcept {
    static char text[65536];
    const long handle = sys_open("/proc/self/maps", kOpenReadOnly);
    if (syscall_failed(handle)) return false;
    u32 used = 0;
    while (used < sizeof(text) - 1) {
        const long count = sys_read(static_cast<int>(handle), text + used, sizeof(text) - 1 - used);
        if (syscall_failed(count) || count == 0) break;
        used += static_cast<u32>(count);
    }
    sys_close(static_cast<int>(handle));
    text[used] = 0;
    const auto hex = [](const char*& at) noexcept {
        usize value = 0;
        for (;; ++at) {
            const char c = *at;
            const u32 digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16u;
            if (digit == 16) return value;
            value = value << 4 | digit;
        }
    };
    for (const char* at = text; *at;) {
        Mapping found{};
        found.start = hex(at);
        if (*at++ != '-') return false;
        found.end = hex(at);
        if (*at++ != ' ') return false;
        for (u32 i = 0; i < 4; ++i) found.permissions[i] = *at++;
        if (address >= found.start && address < found.end) {
            *mapping = found;
            return true;
        }
        while (*at && *at != '\n') ++at;
        if (*at) ++at;
    }
    return false;
}

bool inaccessible(const Mapping& mapping) noexcept {
    return mapping.permissions[0] == '-' && mapping.permissions[1] == '-' && mapping.permissions[2] == '-';
}

// As on Windows: the committed prefix is read-write; a tail beyond it is a separate inaccessible mapping (it cannot
// merge with the writable prefix) that reaches the end of the reservation.
bool arena_regions_match(const Arena& arena, bool any_committed, usize expected_committed,
                         usize expected_reserved) noexcept {
    const usize base = reinterpret_cast<usize>(arena.base);
    Mapping head{};
    if (!find_mapping(base, &head)) return false;
    if (any_committed) {
        if (head.permissions[0] != 'r' || head.permissions[1] != 'w' || head.end < base + expected_committed)
            return false;
        if (expected_committed < expected_reserved && head.end != base + expected_committed) return false;
    } else if (!inaccessible(head) || head.end < base + expected_reserved) {
        return false;
    }
    if (expected_committed && expected_committed < expected_reserved) {
        Mapping tail{};
        if (!find_mapping(base + expected_committed, &tail) || !inaccessible(tail) ||
            tail.start > base + expected_committed || tail.end < base + expected_reserved)
            return false;
    }
    return true;
}
#endif

} // namespace

void test_deadline_policy() noexcept {
    expect(deadline_after(100, 23) == 123, "deadline_after_regular");
    expect(deadline_after(~0ull - 4, 5) == ~0ull, "deadline_after_saturates");
    expect(earliest_deadline(0, 25) == 25 && earliest_deadline(40, 0) == 40 &&
               earliest_deadline(40, 25) == 25,
           "deadline_earliest_zero_sentinel");
    expect(!deadline_due(100, 0) && !deadline_due(99, 100) && deadline_due(100, 100),
           "deadline_due_policy");
    expect(wait_milliseconds_until(100, 100) == 0 && wait_milliseconds_until(100, 125) == 25,
           "deadline_wait_regular");
    expect(wait_milliseconds_until(0, 0xFFFFFFFFull) == kFiniteWaitMaximumMilliseconds &&
               wait_milliseconds_until(0, ~0ull, kInfiniteWaitMilliseconds) == kInfiniteWaitMilliseconds,
           "deadline_wait_caps");
}

void test_spsc_queue() noexcept {
    SpscQueue<u32, 8> queue{};
    bool filled = true;
    for (u32 value = 0; value < 8; ++value) {
        filled = filled && spsc_queue_push(&queue, value + 10);
    }
    expect(filled && spsc_queue_size(queue) == 8, "spsc_queue_full_capacity");
    expect(!spsc_queue_push(&queue, static_cast<u32>(99)), "spsc_queue_bounded_drop");
    bool ordered = true;
    for (u32 expected = 0; expected < 8; ++expected) {
        u32 value = 0;
        ordered = ordered && spsc_queue_pop(&queue, &value) && value == expected + 10;
    }
    u32 empty_value = 0;
    expect(ordered && spsc_queue_size(queue) == 0 && !spsc_queue_pop(&queue, &empty_value),
           "spsc_queue_fifo_empty");

    queue.read_sequence = static_cast<Atomic32>(0xFFFFFFFCu);
    queue.write_sequence = static_cast<Atomic32>(0xFFFFFFFCu);
    bool wrapped = true;
    for (u32 value = 0; value < 8; ++value) {
        wrapped = wrapped && spsc_queue_push(&queue, 100 + value);
    }
    for (u32 expected = 0; expected < 8; ++expected) {
        u32 value = 0;
        wrapped = wrapped && spsc_queue_pop(&queue, &value) && value == 100 + expected;
    }
    expect(wrapped && spsc_queue_size(queue) == 0, "spsc_queue_sequence_wrap");
}

void test_spsc_mailbox() noexcept {
    SpscMailbox<u32, 4, 8> mailbox{};
    const u8 first_payload[] = {1, 2, 3, 4, 5};
    const u8 second_payload[] = {6, 7, 8};
    expect(spsc_mailbox_push(&mailbox, 10, ConstSlice<u8>{first_payload, 5}).ok() &&
               spsc_mailbox_push(&mailbox, 20, ConstSlice<u8>{second_payload, 3}).ok(),
           "spsc_mailbox_fill_payload");
    const u8 overflow_payload[] = {9};
    expect(spsc_mailbox_push(&mailbox, 30, ConstSlice<u8>{overflow_payload, 1}).error ==
               SpscMailboxError::payload_capacity,
           "spsc_mailbox_payload_backpressure");

    u32 value = 0;
    u8 copied[8]{};
    const SpscMailboxResult popped = spsc_mailbox_pop(&mailbox, &value, Slice<u8>{copied, 8});
    bool first_equal = popped.ok() && popped.payload_bytes == 5 && value == 10;
    for (u32 index = 0; index < 5; ++index) {
        first_equal = first_equal && copied[index] == first_payload[index];
    }
    expect(first_equal, "spsc_mailbox_pop_copy");

    const u8 wrapped_payload[] = {9, 10, 11, 12};
    expect(spsc_mailbox_push(&mailbox, 30, ConstSlice<u8>{wrapped_payload, 4}).ok(),
           "spsc_mailbox_wrapped_push");
    const SpscMailboxResult discarded = spsc_mailbox_pop(&mailbox, &value, Slice<u8>{copied, 8});
    for (u32 index = 0; index < 8; ++index) {
        copied[index] = 0;
    }
    const SpscMailboxResult wrapped = spsc_mailbox_pop(&mailbox, &value, Slice<u8>{copied, 8});
    bool wrapped_equal = discarded.ok() && discarded.payload_bytes == 3 && value == 30 && wrapped.ok() &&
                         wrapped.payload_bytes == 4;
    for (u32 index = 0; index < 4; ++index) {
        wrapped_equal = wrapped_equal && copied[index] == wrapped_payload[index];
    }
    // Every payload byte is back: the ring accepts a record of its full capacity.
    expect(wrapped_equal && spsc_mailbox_push(&mailbox, 40, ConstSlice<u8>{copied, 8}).ok() &&
               spsc_mailbox_pop(&mailbox, &value, Slice<u8>{copied, 8}).payload_bytes == 8 && value == 40 &&
               spsc_mailbox_pop(&mailbox, &value, Slice<u8>{copied, 8}).error == SpscMailboxError::empty,
           "spsc_mailbox_wrap_and_discard");

    bool descriptor_full = true;
    for (u32 index = 0; index < 4; ++index) {
        descriptor_full =
            descriptor_full && spsc_mailbox_push(&mailbox, 100 + index, ConstSlice<u8>{nullptr, 0}).ok();
    }
    expect(descriptor_full &&
               spsc_mailbox_push(&mailbox, 999, ConstSlice<u8>{nullptr, 0}).error ==
                   SpscMailboxError::event_capacity,
           "spsc_mailbox_descriptor_backpressure");
    bool descriptor_order = true;
    for (u32 index = 0; index < 4; ++index) {
        descriptor_order =
            descriptor_order && spsc_mailbox_pop(&mailbox, &value, Slice<u8>{copied, 8}).ok() && value == 100 + index;
    }
    expect(descriptor_order && spsc_mailbox_pop(&mailbox, &value, Slice<u8>{copied, 8}).error == SpscMailboxError::empty,
           "spsc_mailbox_descriptor_fifo");
}

void test_trace_record() noexcept {
    struct Guarded {
        u64 before = 0x123456789abcdef0ull;
        TraceRecord record{"test"};
        u64 after = 0xfedcba9876543210ull;
    } fixture;
    constexpr const char* suffixes[] = {"abc", " n=0", " n=18446744073709551615", " key=value"};
    bool bounded = true;
    // memmove-style truncation only depends on the bytes left before capacity: 25 prefixes cover the
    // untruncated case and every truncation point of the longest (23-byte) suffix.
    for (u32 prefix = sizeof(fixture.record.text) - 25; prefix < sizeof(fixture.record.text); ++prefix) {
        for (u32 operation = 0; operation < sizeof(suffixes) / sizeof(suffixes[0]); ++operation) {
            auto& record = fixture.record;
            clear_bytes(record.text, sizeof(record.text));
            for (u32 index = 0; index < prefix; ++index) record.text[index] = 'x';
            record.used = prefix;
            switch (operation) {
            case 0: record.append("abc"); break;
            case 1: record.field("n", u64{0}); break;
            case 2: record.field("n", ~u64{0}); break;
            case 3: record.field("key", "value"); break;
            }
            const char* suffix = suffixes[operation];
            u32 expected = prefix;
            while (*suffix && expected + 1 < sizeof(record.text))
                bounded = (record.text[expected++] == *suffix++) && bounded;
            bounded = bounded && record.used == expected && record.text[expected] == 0;
            for (u32 index = 0; index < prefix; ++index)
                bounded = bounded && record.text[index] == 'x';
            bounded = bounded && fixture.before == 0x123456789abcdef0ull &&
                      fixture.after == 0xfedcba9876543210ull;
        }
    }
    expect(bounded, "trace_record_all_capacity_boundaries");
    clear_bytes(fixture.record.text, sizeof(fixture.record.text));
    fixture.record.used = 0;
    fixture.record.append("abc");
    fixture.record.append(fixture.record.text);
    bool overlapping = fixture.record.used == sizeof(fixture.record.text) - 1;
    for (u32 index = 0; index < fixture.record.used; ++index)
        overlapping = overlapping && fixture.record.text[index] == "abc"[index % 3];
    expect(overlapping && fixture.record.text[fixture.record.used] == 0,
           "trace_record_overlapping_text_append");

    // An event and its fields are one line: no newline inside, the values after their keys, error codes named.
    const auto same = [](const LogEvent& event, const char* expected) noexcept {
        const u32 length = c_string_length(expected);
        return event.used == length && maicord::bytes_equal(event.text, expected, length) && event.text[length] == 0;
    };
    LogEvent event("visible_media_deferred");
    event.field("asset_kind", u64{7})
        .field("frame", "450x880")
        .hex("mask", 0x1Fu)
        .error("hr", 0x887A0005u)
        .error("native_error", 5)
        .error("other", 1234)
        .quoted("cpu", "AMD \"Ryzen\" 9");
    const u64 values[] = {1, 2, 3};
    LogEvent group("group");
    group.fields("a\0b\0c", values);
    expect(same(event, "visible_media_deferred asset_kind=7 frame=450x880 mask=0x1F hr=0x887A0005 "
                       "DXGI_ERROR_DEVICE_REMOVED native_error=5 ERROR_ACCESS_DENIED other=1234 cpu=\"AMD 'Ryzen' 9\"") &&
               same(group, "group a=1 b=2 c=3"),
           "log_event_formats_one_line");

    // The common codes only: an HRESULT_FROM_WIN32 value by its Win32 name, anything unlisted unnamed.
    const auto named = [](u32 code, const char* expected) noexcept {
        const char* name = log_error_name(code);
        return expected ? name && maicord::bytes_equal(name, expected, c_string_length(expected) + 1) : !name;
    };
    bool table = named(2, "ERROR_FILE_NOT_FOUND") && named(1460, "ERROR_TIMEOUT") && named(10054, "WSAECONNRESET") &&
                 named(0x80070005u, "ERROR_ACCESS_DENIED") && named(0x8007000Eu, "ERROR_OUTOFMEMORY") &&
                 named(0x80004005u, "E_FAIL") && named(0x887A0006u, "DXGI_ERROR_DEVICE_HUNG") &&
                 named(0x887A0020u, "DXGI_ERROR_DRIVER_INTERNAL_ERROR") && named(0, nullptr) &&
                 named(0x80070001u, nullptr) && named(0x887A0002u, nullptr);
#if !defined(_WIN32)
    table = table && named(0x40000068u, "ECONNRESET") && named(0x4000001Cu, "ENOSPC") && named(0x40000001u, nullptr);
#else
    table = table && named(0x40000068u, nullptr);
#endif
    expect(table, "log_error_names_common_codes");
}

void test_byte_queue() noexcept {
    u8 storage[8]{};
    ByteQueue queue{};
    expect(byte_queue_initialize(&queue, Slice<u8>{storage, 8}), "byte_queue_initialize");
    const u8 first[] = {1, 2, 3, 4, 5, 6};
    expect(byte_queue_append(&queue, ConstSlice<u8>{first, 6}), "byte_queue_append_first");
    expect(byte_queue_size(queue) == 6 && byte_queue_available(queue) == 2, "byte_queue_size_available");
    expect(byte_queue_consume(&queue, 4), "byte_queue_consume");
    const u8 second[] = {7, 8, 9, 10, 11, 12};
    expect(byte_queue_append(&queue, ConstSlice<u8>{second, 6}), "byte_queue_compact_append");
    const ConstSlice<u8> readable = byte_queue_readable(queue);
    expect(readable.count == 8 && readable[0] == 5 && readable[1] == 6 && readable[2] == 7 &&
               readable[7] == 12,
           "byte_queue_compaction_order");
    const u8 overflow[] = {13};
    expect(!byte_queue_append(&queue, ConstSlice<u8>{overflow, 1}), "byte_queue_overflow_bound");
    expect(!byte_queue_consume(&queue, 9), "byte_queue_consume_bound");
    byte_queue_clear(&queue, true);
    bool erased = byte_queue_size(queue) == 0;
    for (u32 index = 0; index < 8; ++index) {
        erased = erased && storage[index] == 0;
    }
    expect(erased, "byte_queue_secure_clear");

    Slice<u8> writable = byte_queue_writable(&queue);
    expect(writable.count == 8, "byte_queue_writable_span");
    writable[0] = 21;
    writable[1] = 22;
    expect(byte_queue_produce(&queue, 2) && byte_queue_readable(queue)[1] == 22,
           "byte_queue_external_produce");
    expect(!byte_queue_produce(&queue, 7), "byte_queue_produce_bound");
    expect(!byte_queue_initialize(nullptr, Slice<u8>{storage, 8}), "byte_queue_null_reject");

    // Exercise unaligned appends and overlapping compaction across copy boundaries. The runtime memmove
    // uses only unaligned loads, so its paths depend on length alone; three offsets keep misalignment.
    u8 payload[512]{};
    u8 window[576]{};
    for (u32 index = 0; index < sizeof(payload); ++index) {
        payload[index] = static_cast<u8>(index * 37 + 17);
    }
    constexpr u32 lengths[]{0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64,
                            127, 128, 255, 256, 511};
    bool ordered = true;
    for (const u32 length : lengths) {
        for (u32 offset = 1; offset <= 31; offset += 15) {
            clear_bytes(window, sizeof(window));
            ByteQueue current{window + offset, length + 1, 0, 0};
            ordered = byte_queue_append(&current, ConstSlice<u8>{payload, length}) && ordered;
            ordered = current.end == length && maicord::bytes_equal(current.data, payload, length) && ordered;
            if (length != 0) {
                ordered = byte_queue_consume(&current, 1) && ordered;
                // Two incoming bytes require compaction of the resident suffix.
                ordered = byte_queue_append(&current, ConstSlice<u8>{payload + 510, 2}) && ordered;
                ordered = current.begin == 0 && current.end == length + 1 &&
                          maicord::bytes_equal(current.data, payload + 1, length - 1) &&
                          current.data[length - 1] == payload[510] &&
                          current.data[length] == payload[511] && ordered;
            }
            ordered = window[offset - 1] == 0 && window[offset + length + 1] == 0 && ordered;
        }
    }
    expect(ordered, "byte_queue_fragmented_copy_boundaries");
}

// What one event with five fields costs the open log, against the same five values as five lines (how call sites
// logged before LogEvent): one line, one file write and one prefix, where the split form takes five of each. Both are
// warnings, which reach the file before the call returns, so each line is its own write. `newline` is 2 for "\r\n".
bool log_write_counts_match(u32 newline) noexcept {
    constexpr u32 prefix = 35; // "YYYY-MM-DD HH:MM:SS.mmm [W] [test] "
    const u64 values[] = {1, 2, 3, 4, 5};
    const LogCounters start = log_counters();
    LogEvent("deferred").fields("a\0b\0c\0d\0e", values).write(LogLevel::warning, "test");
    const LogCounters event = log_counters();
    const char* keys = "a\0b\0c\0d\0e";
    for (const u64 value : values) {
        log_u64(LogLevel::warning, "test", keys, value);
        keys += 2;
    }
    const LogCounters split = log_counters();
    return event.lines - start.lines == 1 && event.file_writes - start.file_writes == 1 &&
           event.file_bytes - start.file_bytes == prefix + c_string_length("deferred a=1 b=2 c=3 d=4 e=5") + newline &&
           split.lines - event.lines == 5 && split.file_writes - event.file_writes == 5 &&
           split.file_bytes - event.file_bytes == 5 * (prefix + 3 + newline);
}

#if defined(_WIN32)
// Buffered logging: ordinary lines reach the file through the deferred flush
// without further log traffic, error lines flush synchronously, order is kept.
void test_log_buffered_flush() noexcept {
    wchar_t previous[1024]{};
    const DWORD previous_length = GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 1024);
    wchar_t root[512]{};
    DWORD length = GetTempPathW(400, root);
    const wchar_t suffix[] = L"MaiCord-log-test";
    bool ok = length != 0 && length < 400;
    for (u32 index = 0; ok && suffix[index]; ++index) root[length++] = suffix[index];
    wchar_t maicord_dir[512]{}, logs_dir[512]{}, file[512]{};
    const wchar_t* parts[] = {L"\\MaiCord", L"\\logs", L"\\maicord.log"};
    wchar_t* outputs[] = {maicord_dir, logs_dir, file};
    u32 cursor = length;
    for (u32 part = 0; ok && part < 3; ++part) {
        for (u32 index = 0; index < cursor; ++index) outputs[part][index] = part ? outputs[part - 1][index] : root[index];
        for (u32 index = 0; parts[part][index]; ++index) outputs[part][cursor++] = parts[part][index];
    }
    ok = ok && (CreateDirectoryW(root, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) &&
         SetEnvironmentVariableW(L"LOCALAPPDATA", root) && log_open();
    const auto file_text = [&](char* output, u32 capacity) noexcept -> u32 {
        const HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return 0;
        DWORD read = 0;
        ReadFile(handle, output, capacity - 1, &read, nullptr);
        CloseHandle(handle);
        output[read] = 0;
        return read;
    };
    char text[1024]{};
    log_write(LogLevel::info, "test", "buffered_line");
    const bool initially_buffered = ok && file_text(text, sizeof(text)) == 0;
    u32 bytes = 0;
    for (u32 attempt = 0; ok && attempt < 200 && bytes == 0; ++attempt) {
        Sleep(10);
        bytes = file_text(text, sizeof(text));
    }
    const bool deferred = bytes != 0;
    log_u64(LogLevel::info, "test", "pending", 7);
    log_write(LogLevel::error, "test", "error_line");
    const bool counted = ok && log_write_counts_match(2);
    bytes = file_text(text, sizeof(text));
    log_close();
    const auto find = [&](const char* needle) noexcept -> u32 {
        for (u32 start = 0; text[start]; ++start) {
            u32 index = 0;
            while (needle[index] && text[start + index] == needle[index]) ++index;
            if (!needle[index]) return start;
        }
        return ~0u;
    };
    const u32 first = find("[I] [test] buffered_line\r\n"), second = find("[I] [test] pending=7\r\n"),
              third = find("[E] [test] error_line\r\n");
    expect(ok && initially_buffered && deferred && first != ~0u && second != ~0u && third != ~0u &&
               first < second && second < third && text[4] == '-' && text[10] == ' ' && text[19] == '.' &&
               text[23] == ' ',
           "log_buffers_lines_flushes_deferred_and_errors_in_order");
    const u32 event_line = find("[W] [test] deferred a=1 b=2 c=3 d=4 e=5\r\n");
    expect(counted && event_line != ~0u && event_line > third,
           "log_event_is_one_line_one_write_and_warnings_reach_the_file_at_once");
    DeleteFileW(file);
    RemoveDirectoryW(logs_dir);
    RemoveDirectoryW(maicord_dir);
    RemoveDirectoryW(root);
    SetEnvironmentVariableW(L"LOCALAPPDATA", previous_length && previous_length < 1024 ? previous : nullptr);
}
#else
// The same contract on the Linux log, whose file is <XDG_STATE_HOME>/maicord/logs/maicord.log and whose lines end in
// "\n". The harness gives every test a private XDG_STATE_HOME ("test-profile" in its path); the test refuses any
// other, so it never deletes the user's log.
void test_log_buffered_flush() noexcept {
    const char* data_home = environment_get("XDG_STATE_HOME");
    bool private_home = false;
    for (u32 start = 0; data_home && data_home[start] && !private_home; ++start) {
        const char* name = "test-profile";
        u32 index = 0;
        while (name[index] && data_home[start + index] == name[index]) ++index;
        private_home = name[index] == 0;
    }
    wchar_t file[768]{};
    char path[2304]{};
    bool ok = private_home && state_path(file, 768, L"logs/maicord.log", true).ok() &&
              path_to_utf8(file, path, sizeof(path)) != 0 && log_open();
    const auto file_text = [&](char* output, u32 capacity) noexcept -> u32 {
        const long handle = sys_open(path, kOpenReadOnly);
        if (syscall_failed(handle)) return 0;
        const long read = sys_read(static_cast<int>(handle), output, capacity - 1);
        sys_close(static_cast<int>(handle));
        const u32 count = syscall_failed(read) ? 0 : static_cast<u32>(read);
        output[count] = 0;
        return count;
    };
    char text[1024]{};
    log_write(LogLevel::info, "test", "buffered_line");
    const bool initially_buffered = ok && file_text(text, sizeof(text)) == 0;
    u32 bytes = 0;
    for (u32 attempt = 0; ok && attempt < 200 && bytes == 0; ++attempt) {
        const TimeSpec pause{0, 10'000'000};
        sys_nanosleep(&pause);
        bytes = file_text(text, sizeof(text));
    }
    const bool deferred = bytes != 0;
    log_u64(LogLevel::info, "test", "pending", 7);
    log_write(LogLevel::error, "test", "error_line");
    const bool counted = ok && log_write_counts_match(1);
    bytes = file_text(text, sizeof(text));
    log_close();
    const auto find = [&](const char* needle) noexcept -> u32 {
        for (u32 start = 0; text[start]; ++start) {
            u32 index = 0;
            while (needle[index] && text[start + index] == needle[index]) ++index;
            if (!needle[index]) return start;
        }
        return ~0u;
    };
    const u32 first = find("[I] [test] buffered_line\n"), second = find("[I] [test] pending=7\n"),
              third = find("[E] [test] error_line\n");
    expect(ok && initially_buffered && deferred && first != ~0u && second != ~0u && third != ~0u &&
               first < second && second < third && text[4] == '-' && text[10] == ' ' && text[19] == '.' &&
               text[23] == ' ',
           "log_buffers_lines_flushes_deferred_and_errors_in_order");
    const u32 event_line = find("[W] [test] deferred a=1 b=2 c=3 d=4 e=5\n");
    expect(counted && event_line != ~0u && event_line > third,
           "log_event_is_one_line_one_write_and_warnings_reach_the_file_at_once");
    // Remove the file, then the logs and MaiCord directories above it.
    sys_unlink(path);
    for (u32 directories = 0; ok && directories < 2; ++directories) {
        u32 end = 0;
        while (path[end]) ++end;
        while (end && path[end - 1] != '/') --end;
        path[end ? end - 1 : 0] = 0;
        sys_remove_directory(path);
    }
}
#endif

void test_arena() noexcept {
    Arena arena{};
    expect(arena_initialize(&arena, 1024 * 1024, 4096, 64 * 1024), "arena_initialize");

    auto* first = static_cast<u8*>(arena_allocate(&arena, 3, 1));
    auto* aligned = static_cast<u8*>(arena_allocate(&arena, 17, 64));
    expect(first == arena.base, "arena_first_allocation");
    expect(aligned && (reinterpret_cast<usize>(aligned) & 63u) == 0, "arena_alignment");

    const ArenaMark mark = arena_mark(&arena);
    auto* cleared = static_cast<u8*>(arena_allocate_zero(&arena, 128, 16));
    bool all_zero = cleared != nullptr;
    for (u32 index = 0; index < 128 && all_zero; ++index) {
        all_zero = cleared[index] == 0;
        cleared[index] = 0xA5;
    }
    expect(all_zero, "arena_allocate_zero");
    expect(arena_rewind(&arena, mark, ArenaRewindMode::erase), "arena_secure_rewind");
    bool erased = true;
    for (u32 index = 0; index < 128; ++index) {
        erased = erased && cleared[index] == 0;
    }
    expect(erased, "arena_secure_rewind_erases");
    // Zeroed allocations erase only below the peak (beyond it the pages were
    // never handed out); an allocation over retained dirty bytes still reads zero.
    const ArenaMark retained_mark = arena_mark(&arena);
    auto* retained = static_cast<u8*>(arena_allocate(&arena, 64, 16));
    for (u32 index = 0; retained && index < 64; ++index) {
        retained[index] = 0x5A;
    }
    expect(retained && arena_rewind(&arena, retained_mark), "arena_retained_rewind");
    auto* straddle = static_cast<u8*>(arena_allocate_zero(&arena, 256, 16));
    bool straddle_zero = straddle && straddle == retained;
    for (u32 index = 0; straddle_zero && index < 256; ++index) {
        straddle_zero = straddle[index] == 0;
    }
    expect(straddle_zero, "arena_allocate_zero_below_peak");

    const usize committed_before_growth = arena.committed;
    auto* growth = static_cast<u8*>(arena_allocate(&arena, 70 * 1024, 4096));
    expect(growth != nullptr, "arena_commit_growth_allocation");
    expect(arena.committed > committed_before_growth && arena.commit_count >= 2,
           "arena_commit_growth_recorded");
    expect((reinterpret_cast<usize>(growth) & 4095u) == 0, "arena_page_alignment");

    expect(arena_allocate(&arena, arena.reserved + 1, 1) == nullptr, "arena_capacity_bound");
    expect(arena_allocate(&arena, 8, 3) == nullptr, "arena_alignment_reject");
    expect(!arena_rewind(&arena, ArenaMark{arena.offset + 1}), "arena_forward_rewind_reject");
    expect(arena.peak >= mark.offset, "arena_peak_recorded");
    expect(!arena_allocate(&arena, 1, usize{1} << 63), "arena_huge_alignment_reject");
    bool aligned_bounds = true;
    for (u32 exponent = 0; exponent <= 24; ++exponent) {
        arena_reset(&arena);
        const usize alignment = usize{1} << exponent;
        const usize address = reinterpret_cast<usize>(arena.base);
        const usize expected = (address + alignment - 1) & ~(alignment - 1);
        const bool fits = expected - address <= arena.reserved - 8;
        const auto* value = arena_allocate(&arena, 8, alignment);
        aligned_bounds = aligned_bounds && (fits ? reinterpret_cast<usize>(value) == expected
                                                 : !value && arena.offset == 0);
    }
    expect(aligned_bounds, "arena_absolute_alignment_and_capacity_across_page_boundaries");
    arena_release(&arena);

    const usize page_mask = pages_page_size() - 1;
    const usize reserve_inputs[]{4095, 4096, 17000, 65536};
    bool initial_regions = true;
    for (usize reserve : reserve_inputs) for (u32 policy = 0; policy < 4; ++policy) {
        const usize initial = policy == 0 ? 0 : policy == 1 ? 1 : policy == 2 ? reserve - 1 : reserve;
        const usize expected_reserved = (reserve + page_mask) & ~page_mask;
        const usize expected_committed = (initial + page_mask) & ~page_mask;
        if (!arena_initialize(&arena, reserve, initial, 4096)) {
            initial_regions = false;
            continue;
        }
        initial_regions = initial_regions && arena.reserved == expected_reserved &&
                          arena.committed == expected_committed && arena.offset == 0 &&
                          arena.commit_count == (initial ? 1u : 0u) &&
                          arena_regions_match(arena, initial != 0, expected_committed, expected_reserved);
        for (usize i = 0; i < expected_committed; ++i)
            initial_regions = initial_regions && arena.base[i] == 0;
        arena_release(&arena);
    }
    expect(initial_regions, "arena_initial_commit_regions_round_and_zero_without_committing_the_tail");
    expect(!arena_initialize(&arena, 4096, ~usize{0}, 4096) && !arena.base &&
               !arena_initialize(&arena, 4096, 0, ~usize{0}) && !arena.base,
           "arena_initial_commit_and_step_overflow_leave_owner_empty");
    expect(arena.base == nullptr && arena.reserved == 0, "arena_release");

    // Dirty retained storage, unaligned marks and lengths straddling word/page
    // boundaries: clearing must neither leave bytes behind nor touch guards.
    Arena dirty{};
    bool bounds = arena_initialize(&dirty, 8192, 8192, 4096);
    const u32 lengths[]{1, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 4095, 4096};
    for (u32 offset = 0; bounds && offset < 32; ++offset) {
        for (u32 bytes : lengths) {
            for (u32 mode = 0; bounds && mode < 2; ++mode) {
                arena_reset(&dirty);
                auto* backing = static_cast<u8*>(arena_allocate(&dirty, bytes + 64, 1));
                if (!backing) { bounds = false; break; }
                for (u32 i = 0; i < bytes + 64; ++i) backing[i] = 0xA5;
                bounds = arena_rewind(&dirty, {offset + (mode ? bytes : 0)});
                if (mode) bounds = bounds && arena_rewind(&dirty, {offset}, ArenaRewindMode::erase);
                else bounds = bounds && arena_allocate_zero(&dirty, bytes, 1) == backing + offset;
                for (u32 i = 0; bounds && i < bytes + 64; ++i)
                    bounds = backing[i] == (i >= offset && i < offset + bytes ? 0 : 0xA5);
            }
        }
    }
    expect(bounds, "arena_zero_and_secure_rewind_preserve_unaligned_guards");
    arena_release(&dirty);

    // The per-tag account is the arenas' own commits at every step: initialization, growth, trim and release.
    const ArenaAccount base = arena_account(ArenaTag::platform);
    Arena first_tagged{}, second_tagged{};
    const auto account_is = [&](i64 arenas, i64 reserved, i64 committed) noexcept {
        const ArenaAccount now = arena_account(ArenaTag::platform);
        return now.arenas == base.arenas + arenas && now.reserved == base.reserved + reserved &&
               now.committed == base.committed + committed && now.peak_committed >= now.committed;
    };
    bool accounted = arena_initialize(&first_tagged, 1024 * 1024, 4096, 64 * 1024, ArenaTag::platform) &&
                     arena_initialize(&second_tagged, 256 * 1024, 0, 4096, ArenaTag::platform) &&
                     first_tagged.tag == ArenaTag::platform;
    const auto sum = [&](usize Arena::*member) noexcept {
        return static_cast<i64>(first_tagged.*member + second_tagged.*member);
    };
    accounted = accounted && account_is(2, sum(&Arena::reserved), sum(&Arena::committed));
    accounted = accounted && arena_allocate(&first_tagged, 200 * 1024, 16) && arena_allocate(&second_tagged, 5000, 8) &&
                first_tagged.committed == 256 * 1024 && second_tagged.committed == 8192 &&
                account_is(2, sum(&Arena::reserved), sum(&Arena::committed));
    const i64 high = base.committed + sum(&Arena::committed);
    accounted = accounted && arena_account(ArenaTag::platform).peak_committed == maximum(base.peak_committed, high);
    accounted = accounted && arena_trim(&first_tagged, 0) && first_tagged.committed == 0 &&
                account_is(2, sum(&Arena::reserved), sum(&Arena::committed)) &&
                arena_account(ArenaTag::platform).peak_committed == maximum(base.peak_committed, high);
    arena_release(&second_tagged);
    accounted = accounted && account_is(1, static_cast<i64>(first_tagged.reserved), 0);
    arena_release(&first_tagged);
    accounted = accounted && account_is(0, 0, 0);
    expect(accounted, "arena_account_matches_commits_across_growth_trim_and_release");
}

void test_stable_storage() noexcept {
    StableStorage storage{};
    expect(stable_storage_initialize(&storage, 16, 1, sizeof(u64), alignof(u64), true),
           "stable_storage_initialize");
    u64* const base = stable_storage_data<u64>(storage);
    base[0] = 0x1020304050607080ull;
    expect(stable_storage_grow(&storage, 3) && storage.capacity == 3 &&
               stable_storage_data<u64>(storage) == base && base[0] == 0x1020304050607080ull &&
               base[1] == 0 && base[2] == 0,
           "stable_storage_growth_preserves_address_and_zeroes_tail");
    base[1] = 7;
    expect(stable_storage_grow(&storage, 4) && storage.capacity == 6 && base[1] == 7 && base[3] == 0 &&
               base[4] == 0 && base[5] == 0,
           "stable_storage_growth_is_geometric");
    expect(!stable_storage_grow(&storage, 17) && storage.capacity == 6,
           "stable_storage_growth_respects_reserved_bound");
    stable_storage_release(&storage);
    expect(!storage.arena.base && storage.capacity == 0 && storage.reserved_capacity == 0,
           "stable_storage_release");

    StableStorage invalid{};
    expect(!stable_storage_initialize(&invalid, 4, 1, 3, 4),
           "stable_storage_rejects_noncontiguous_element_alignment");

    StableStorage lazy{};
    expect(stable_storage_initialize(&lazy, 16, 0, sizeof(u32), alignof(u32), true) && lazy.capacity == 0,
           "stable_storage_zero_initial_capacity");
    expect(stable_storage_grow(&lazy, 1) && lazy.capacity == 1,
           "stable_storage_zero_capacity_first_growth");
    u32* const lazy_base = stable_storage_data<u32>(lazy);
    lazy_base[0] = 41;
    expect(stable_storage_grow(&lazy, 3) && lazy.capacity == 3 &&
               stable_storage_data<u32>(lazy) == lazy_base && lazy_base[0] == 41 && lazy_base[1] == 0 &&
               lazy_base[2] == 0,
           "stable_storage_lazy_growth_preserves_address");
    stable_storage_release(&lazy);
}

namespace {

void expect_valid_utf8(Utf8View text, u32 codepoints, const char* name) noexcept {
    const Utf8Validation result = utf8_validate(text);
    expect(result.valid && result.codepoints == codepoints && result.error_offset == text.bytes, name);
}

void expect_invalid_utf8(Utf8View text, u32 error_offset, const char* name) noexcept {
    const Utf8Validation result = utf8_validate(text);
    expect(!result.valid && result.error_offset == error_offset, name);
}

} // namespace

void test_utf8() noexcept {
    expect_valid_utf8(utf8_literal(""), 0, "utf8_empty");
    expect_valid_utf8(utf8_literal("MaiCord"), 7, "utf8_ascii");
    expect_valid_utf8(utf8_literal("\xC2\xA2"), 1, "utf8_two_byte");
    expect_valid_utf8(utf8_literal("\xE2\x82\xAC"), 1, "utf8_three_byte");
    expect_valid_utf8(utf8_literal("\xF0\x9F\x98\x80"), 1, "utf8_four_byte");
    expect_valid_utf8(utf8_literal("\xF4\x8F\xBF\xBF"), 1, "utf8_max_codepoint");

    expect_invalid_utf8(utf8_literal("\x80"), 0, "utf8_isolated_continuation");
    expect_invalid_utf8(utf8_literal("\xC0\xAF"), 0, "utf8_overlong_two");
    expect_invalid_utf8(utf8_literal("\xE0\x80\x80"), 0, "utf8_overlong_three");
    expect_invalid_utf8(utf8_literal("\xED\xA0\x80"), 0, "utf8_surrogate");
    expect_invalid_utf8(utf8_literal("\xF4\x90\x80\x80"), 0, "utf8_above_max");
    expect_invalid_utf8(utf8_literal("A\xE2\x82"), 1, "utf8_truncated_offset");

    const auto scalar_matches = [](Utf8View input) noexcept {
        u32 offset = 0;
        u32 count = 0;
        while (offset < input.bytes) {
            const auto decoded = utf8_decode_one(input.data + offset, input.bytes - offset);
            if (!decoded.valid) {
                const auto actual = utf8_validate(input);
                return !actual.valid && actual.codepoints == count && actual.error_offset == offset;
            }
            offset += decoded.width;
            ++count;
        }
        const auto actual = utf8_validate(input);
        return actual.valid && actual.codepoints == count && actual.error_offset == input.bytes;
    };
    char block_text[256];
    bool blocks_exact = true;
    const u32 block_scalars[]{0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff,
                              0x10000, 0x10ffff};
    // utf8_validate uses unaligned 16-byte loads, so one misaligned base suffices; prefixes 0..33 put
    // the scalar at every offset within the first, second and third 16-byte block.
    for (u32 alignment = 0; alignment < 2 && blocks_exact; ++alignment) {
        for (u32 prefix = 0; prefix < 34 && blocks_exact; ++prefix) {
            for (u32 scalar : block_scalars) {
                for (char& value : block_text) value = 'a';
                const u32 width = utf8_encode(scalar, block_text + alignment + prefix);
                for (u32 tail = 0; tail < 18 && blocks_exact; ++tail) {
                    const Utf8View input{block_text + alignment, prefix + width + tail};
                    blocks_exact = scalar_matches(input);
                    block_text[alignment + prefix] = static_cast<char>(0xff);
                    blocks_exact = blocks_exact && scalar_matches(input);
                    utf8_encode(scalar, block_text + alignment + prefix);
                }
                for (u32 partial = 0; partial < width && blocks_exact; ++partial)
                    blocks_exact = scalar_matches({block_text + alignment, prefix + partial});
            }
        }
    }
    expect(blocks_exact, "utf8_ascii_block_scalar_boundaries_and_error_counts");

    u32 random_state = 0x735a9012;
    const auto random_byte = [&random_state]() noexcept {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        return static_cast<char>(random_state);
    };
    bool random_exact = true;
    for (u32 trial = 0; trial < 512 && random_exact; ++trial) {
        const u32 length = trial % static_cast<u32>(sizeof(block_text));
        for (u32 index = 0; index < length; ++index) block_text[index] = random_byte();
        random_exact = scalar_matches({block_text, length});
    }
    expect(random_exact, "utf8_random_scalar_reference_and_error_counts");

    const u32 page_bytes = static_cast<u32>(pages_page_size());
    char* guard_text = static_cast<char*>(pages_allocate(static_cast<usize>(page_bytes) * 2));
    bool guard_exact = guard_text && pages_protect(guard_text + page_bytes, page_bytes, PageAccess::none);
    for (u32 length = 0; length <= 48 && guard_exact; ++length) {
        char* start = guard_text + page_bytes - length;
        for (u32 index = 0; index < length; ++index) start[index] = 'a';
        guard_exact = scalar_matches({start, length});
        for (u32 index = 0; index < length && guard_exact; ++index) {
            start[index] = static_cast<char>(0x80);
            guard_exact = scalar_matches({start, length});
            start[index] = 'a';
        }
    }
    if (guard_text) pages_release(guard_text, static_cast<usize>(page_bytes) * 2);
    expect(guard_exact, "utf8_ascii_block_protected_page_bounds");

    // Width minima/maxima, surrogate neighbours and alternating payload bits per width: every encode/decode
    // branch and every payload bit as both 0 and 1, without sweeping all 1.1M scalars.
    const u32 codepoints[] = {0x0u,    0x24u,   0x7Fu,   0x80u,    0xA2u,    0x555u,    0x7FFu,  0x800u,
                              0x20ACu, 0xAAAAu, 0xD7FFu, 0xE000u, 0xFFFFu, 0x10000u, 0x15555u, 0x1F600u,
                              0x10FFFFu};
    bool round_trip = true;
    for (u32 codepoint : codepoints) {
        char encoded[4]{};
        const u8 width = utf8_encode(codepoint, encoded);
        const Utf8DecodeResult decoded = utf8_decode_one(encoded, width);
        round_trip = round_trip && width != 0 && decoded.valid && decoded.codepoint == codepoint &&
                     decoded.width == width;
    }
    expect(round_trip, "utf8_round_trip");
    char invalid_output[4]{};
    expect(utf8_encode(0xD800u, invalid_output) == 0, "utf8_encode_surrogate_reject");
    expect(utf8_encode(0x110000u, invalid_output) == 0, "utf8_encode_range_reject");

    constexpr Utf8View mapped = utf8_literal("A\xF0\x9F\x98\x80\xE2\x82\xAC");
    u32 position = ~0u;
    bool mapping_exact = utf8_utf16_position_for_byte_offset(mapped, 0, &position) && position == 0 &&
                         utf8_utf16_position_for_byte_offset(mapped, 1, &position) && position == 1 &&
                         utf8_utf16_position_for_byte_offset(mapped, 5, &position) && position == 3 &&
                         utf8_utf16_position_for_byte_offset(mapped, 8, &position) && position == 4 &&
                         !utf8_utf16_position_for_byte_offset(mapped, 2, &position) &&
                         !utf8_utf16_position_for_byte_offset(mapped, 9, &position) &&
                         utf8_byte_offset_for_utf16_position(mapped, 0, &position) && position == 0 &&
                         utf8_byte_offset_for_utf16_position(mapped, 1, &position) && position == 1 &&
                         utf8_byte_offset_for_utf16_position(mapped, 3, &position) && position == 5 &&
                         utf8_byte_offset_for_utf16_position(mapped, 4, &position) && position == 8 &&
                         !utf8_byte_offset_for_utf16_position(mapped, 2, &position) &&
                         !utf8_byte_offset_for_utf16_position(mapped, 5, &position) &&
                         utf8_utf16_position_for_codepoint(mapped, 0, &position) && position == 0 &&
                         utf8_utf16_position_for_codepoint(mapped, 1, &position) && position == 1 &&
                         utf8_utf16_position_for_codepoint(mapped, 2, &position) && position == 3 &&
                         utf8_utf16_position_for_codepoint(mapped, 3, &position) && position == 4 &&
                         !utf8_utf16_position_for_codepoint(mapped, 4, &position) &&
                         utf8_codepoint_for_utf16_position(mapped, 0, &position) && position == 0 &&
                         utf8_codepoint_for_utf16_position(mapped, 1, &position) && position == 1 &&
                         utf8_codepoint_for_utf16_position(mapped, 3, &position) && position == 2 &&
                         utf8_codepoint_for_utf16_position(mapped, 4, &position) && position == 3 &&
                         !utf8_codepoint_for_utf16_position(mapped, 2, &position) &&
                         !utf8_codepoint_for_utf16_position(mapped, 5, &position);
    expect(mapping_exact, "utf8_utf16_boundary_mapping");
}

#if defined(_WIN32)
void test_secure_buffer() noexcept {
    SecureBuffer secret{};
    const SecureBufferResult initialized = secure_buffer_initialize(&secret, 64);
    expect(initialized.ok() && secret.data && secret.capacity == 64 && secret.locked,
           "secure_buffer_initialize_locked");
    if (!initialized.ok()) {
        return;
    }
    const u8 token[] = {'s', 'e', 'c', 'r', 'e', 't'};
    expect(secure_buffer_assign(&secret, ConstSlice<u8>{token, sizeof(token)}).ok() &&
               secure_buffer_view(secret).count == sizeof(token) && secret.data[5] == 't',
           "secure_buffer_assign");

    SecureBuffer transferred{};
    expect(secure_buffer_transfer(&transferred, &secret) && !secret.data && transferred.data &&
               transferred.bytes == sizeof(token),
           "secure_buffer_transfer_ownership");
    expect(!secure_buffer_transfer(&secret, &secret) && secure_buffer_transfer(&secret, &transferred),
           "secure_buffer_transfer_restore");

    const SecureBufferResult protected_size = secure_buffer_protect_user_bound(secret, {}, Slice<u8>{nullptr, 0});
    expect(protected_size.error == SecureBufferError::output_too_small && protected_size.bytes != 0 &&
               protected_size.bytes <= 512,
           "secure_buffer_dpapi_size");
    u8 protected_data[512]{};
    const SecureBufferResult protected_result =
        secure_buffer_protect_user_bound(secret, {}, Slice<u8>{protected_data, sizeof(protected_data)});
    expect(protected_result.ok() && protected_result.bytes != 0, "secure_buffer_dpapi_protect");

    SecureBuffer restored{};
    const SecureBufferResult restored_initialized = secure_buffer_initialize(&restored, 64);
    expect(restored_initialized.ok(), "secure_buffer_restore_initialize");
    if (restored_initialized.ok() && protected_result.ok()) {
        const SecureBufferResult unprotected =
            secure_buffer_unprotect_user_bound(ConstSlice<u8>{protected_data, protected_result.bytes}, {}, &restored);
        const bool equal = unprotected.ok() && restored.bytes == sizeof(token) &&
                           maicord::bytes_equal(restored.data, token, sizeof(token));
        expect(equal, "secure_buffer_dpapi_round_trip");
        secure_buffer_clear(&restored);
        bool cleared = restored.bytes == 0;
        for (u32 index = 0; cleared && index < restored.capacity; ++index) {
            cleared = restored.data[index] == 0;
        }
        expect(cleared, "secure_buffer_explicit_clear");

        const u8 entropy[] = {'M', '2', '5', 1};
        const SecureBufferResult protected_bound =
            secure_buffer_protect_user_bound(secret, ConstSlice<u8>{entropy, sizeof(entropy)},
                                             Slice<u8>{protected_data, sizeof(protected_data)});
        const SecureBufferResult unprotected_bound =
            protected_bound.ok()
                ? secure_buffer_unprotect_user_bound(ConstSlice<u8>{protected_data, protected_bound.bytes},
                                                     ConstSlice<u8>{entropy, sizeof(entropy)}, &restored)
                : SecureBufferResult{};
        expect(protected_bound.ok() && unprotected_bound.ok() && restored.bytes == sizeof(token) &&
                   restored.data[0] == 's' && restored.data[5] == 't',
               "secure_buffer_dpapi_bound_round_trip");
        secure_buffer_clear(&restored);
        const u8 wrong_entropy[] = {'M', '2', '5', 2};
        const SecureBufferResult rejected_entropy =
            protected_bound.ok()
                ? secure_buffer_unprotect_user_bound(ConstSlice<u8>{protected_data, protected_bound.bytes},
                                                     ConstSlice<u8>{wrong_entropy, sizeof(wrong_entropy)},
                                                     &restored)
                : SecureBufferResult{};
        expect(protected_bound.ok() && !rejected_entropy.ok() && restored.bytes == 0,
               "secure_buffer_dpapi_reject_wrong_entropy");
    }
    secure_buffer_release(&restored);
    secure_buffer_release(&secret);
    expect(!secret.data && !secret.locked && secret.capacity == 0 && !restored.data,
           "secure_buffer_release");
}

#endif

void test_runtime_memory() noexcept {
    bool valid = true;
    u32 state = 0x6a09e667u;
    // Counts 0..256 are swept exhaustively at page bounds below; these trials cover
    // overlap direction and the long-copy remainders.
    for (u32 trial = 0; trial < 1024 && valid; ++trial) {
        for (u32 index = 0; index < sizeof(g_runtime_reference); ++index) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            g_runtime_reference[index] = static_cast<u8>(state);
            g_runtime_candidate[index] = g_runtime_reference[index];
        }
        const u32 count = state % 2049;
        const u32 source = (state >> 4) % (sizeof(g_runtime_reference) - count + 1);
        const u32 destination = (state >> 11) % (sizeof(g_runtime_reference) - count + 1);
        for (u32 index = 0; index < count; ++index) {
            g_runtime_temporary[index] = g_runtime_reference[source + index];
        }
        for (u32 index = 0; index < count; ++index) {
            g_runtime_reference[destination + index] = g_runtime_temporary[index];
        }
        valid = g_runtime_move(g_runtime_candidate + destination, g_runtime_candidate + source, count) ==
                g_runtime_candidate + destination;
        for (u32 index = 0; index < sizeof(g_runtime_reference) && valid; ++index) {
            valid = g_runtime_reference[index] == g_runtime_candidate[index];
        }

        const u8 value = static_cast<u8>(state >> 19);
        valid = valid && g_runtime_set(g_runtime_candidate + destination, value, count) ==
                             g_runtime_candidate + destination;
        for (u32 index = 0; index < count && valid; ++index) {
            valid = g_runtime_candidate[destination + index] == value;
        }

        if (source + count <= destination || destination + count <= source) {
            for (u32 index = 0; index < sizeof(g_runtime_candidate); ++index) {
                g_runtime_candidate[index] = static_cast<u8>(index ^ trial);
                g_runtime_reference[index] = g_runtime_candidate[index];
            }
            for (u32 index = 0; index < count; ++index) {
                g_runtime_reference[destination + index] = g_runtime_candidate[source + index];
            }
            valid = g_runtime_copy(g_runtime_candidate + destination, g_runtime_candidate + source,
                                   count) == g_runtime_candidate + destination;
            for (u32 index = 0; index < sizeof(g_runtime_reference) && valid; ++index) {
                valid = g_runtime_reference[index] == g_runtime_candidate[index];
            }
        }
    }
    expect(valid, "runtime_memory_randomized_overlap");

    const usize page_bytes = pages_page_size();
    u8* pages = static_cast<u8*>(pages_allocate(page_bytes * 6));
    bool boundaries = pages != nullptr;
    if (boundaries) {
        boundaries = pages_protect(pages, page_bytes, PageAccess::none) &&
                     pages_protect(pages + page_bytes * 2, page_bytes, PageAccess::none) &&
                     pages_protect(pages + page_bytes * 3, page_bytes, PageAccess::none) &&
                     pages_protect(pages + page_bytes * 5, page_bytes, PageAccess::none);
    }
    if (boundaries) {
        u8* source_page = pages + page_bytes;
        u8* destination_page = pages + page_bytes * 4;
        for (u32 count = 0; count <= 256; ++count) {
            u8* source_end = source_page + page_bytes - count;
            u8* destination_end = destination_page + page_bytes - count;
            for (u32 index = 0; index < count; ++index) {
                source_end[index] = static_cast<u8>(index * 13u + count);
                destination_end[index] = 0;
            }
            boundaries = g_runtime_copy(destination_end, source_end, count) == destination_end;
            for (u32 index = 0; index < count && boundaries; ++index) {
                boundaries = destination_end[index] == source_end[index];
            }
            boundaries = boundaries && g_runtime_set(destination_end, 0xa5, count) == destination_end;
            for (u32 index = 0; index < count && boundaries; ++index) {
                boundaries = destination_end[index] == 0xa5;
            }
            if (!boundaries) {
                break;
            }
        }
    }
    if (pages) {
        pages_release(pages, page_bytes * 6);
    }
    expect(boundaries, "runtime_memory_guard_page_bounds");

    // Counts from 1 MiB on: the Linux runtime streams them (non-temporal stores), and the head, the aligned body and
    // the tail of a misaligned copy, a forward overlap and a fill must each come out exact, with the bytes around
    // them untouched.
    constexpr usize kLarge = (1u << 20) + 77;
    constexpr usize kSlack = 256;
    u8* large = static_cast<u8*>(pages_allocate(kLarge * 2 + kSlack * 4));
    bool streamed = large != nullptr;
    for (u32 variant = 0; variant < 4 && streamed; ++variant) {
        const usize source_offset = variant == 1 ? 1 : variant == 2 ? 31 : 0;
        const usize destination_offset = variant == 1 ? 3 : variant == 2 ? 17 : 0;
        u8* source = large + kSlack + source_offset;
        u8* destination = large + kLarge + kSlack * 3 + destination_offset;
        for (usize index = 0; index < kLarge; ++index) {
            source[index] = static_cast<u8>(index * 29u + variant);
        }
        destination[-1] = 0xc3;
        destination[kLarge] = 0xc3;
        streamed = g_runtime_copy(destination, source, kLarge) == destination && destination[-1] == 0xc3 &&
                   destination[kLarge] == 0xc3;
        for (usize index = 0; index < kLarge && streamed; ++index) {
            streamed = destination[index] == static_cast<u8>(index * 29u + variant);
        }
        // The destination a few bytes below the source, overlapping it from below.
        u8* below = source - 5 - variant;
        streamed = streamed && g_runtime_move(below, source, kLarge) == below;
        for (usize index = 0; index < kLarge && streamed; ++index) {
            streamed = below[index] == static_cast<u8>(index * 29u + variant);
        }
        destination[-1] = 0x3c;
        destination[kLarge] = 0x3c;
        streamed = streamed && g_runtime_set(destination, 0x5a + variant, kLarge) == destination &&
                   destination[-1] == 0x3c && destination[kLarge] == 0x3c;
        for (usize index = 0; index < kLarge && streamed; ++index) {
            streamed = destination[index] == static_cast<u8>(0x5a + variant);
        }
    }
    if (large) {
        pages_release(large, kLarge * 2 + kSlack * 4);
    }
    expect(streamed, "runtime_memory_streaming_sizes");
}

} // namespace maicord
