#include "maicord/app/search_recent.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool entry(const SearchRecentList& list, u32 index, Utf8View text) noexcept {
    const auto value = search_recent_entry(list, index);
    return value.bytes == text.bytes && bytes_equal(value.data, text.data, text.bytes);
}
bool temporary_path(wchar_t* path, u32 capacity) noexcept {
    const DWORD length = GetTempPathW(capacity, path);
    constexpr wchar_t name[] = L"maicord-search-recent-test.bin";
    if (!length || length + sizeof(name) / sizeof(wchar_t) > capacity)
        return false;
    for (u32 i = 0; i < sizeof(name) / sizeof(wchar_t); ++i)
        path[length + i] = name[i];
    return true;
}
} // namespace

void test_search_recent(void (*check)(bool, const char*) noexcept) noexcept {
    SearchRecentList list{};
    check(search_recent_push(&list, utf8_literal("  alpha  ")) && list.count == 1 &&
              entry(list, 0, utf8_literal("alpha")),
          "search_recent_trims_and_stores_text");
    check(!search_recent_push(&list, utf8_literal("   ")) && !search_recent_push(&list, {"a\nb", 3}) &&
              !search_recent_push(&list, {"\xff", 1}) && list.count == 1,
          "search_recent_rejects_empty_control_and_invalid_text");
    const char* names[] = {"beta", "gamma", "delta", "epsilon", "zeta"};
    for (const char* name : names) {
        u32 bytes = 0;
        while (name[bytes])
            ++bytes;
        search_recent_push(&list, {name, bytes});
    }
    check(list.count == kSearchRecentCapacity && entry(list, 0, utf8_literal("zeta")) &&
              entry(list, 4, utf8_literal("beta")),
          "search_recent_keeps_the_five_newest");
    check(search_recent_push(&list, utf8_literal("delta")) && list.count == 5 &&
              entry(list, 0, utf8_literal("delta")) && entry(list, 1, utf8_literal("zeta")) &&
              entry(list, 2, utf8_literal("epsilon")) && entry(list, 3, utf8_literal("gamma")),
          "search_recent_moves_a_repeat_to_the_front_without_duplicates");

    u8 record[kSearchRecentFileBytes];
    const u32 bytes = search_recent_encode(list, {record, sizeof(record)});
    SearchRecentList decoded{};
    check(bytes && search_recent_decode({record, bytes}, &decoded) && decoded.count == 5 &&
              entry(decoded, 0, utf8_literal("delta")) && entry(decoded, 4, utf8_literal("beta")),
          "search_recent_round_trips");
    SearchRecentList untouched{};
    search_recent_push(&untouched, utf8_literal("keep"));
    record[8] ^= 1;
    check(!search_recent_decode({record, bytes}, &untouched) && entry(untouched, 0, utf8_literal("keep")),
          "search_recent_rejects_a_corrupt_checksum");
    record[8] ^= 1;
    check(!search_recent_decode({record, bytes - 1}, &untouched) &&
              !search_recent_decode({record, kSearchRecentFileBytes + 1}, &untouched),
          "search_recent_rejects_truncated_and_oversized_records");
    // A well-formed record that repeats an entry is still rejected.
    SearchRecentList twice{};
    twice.count = 2;
    copy_bytes(twice.text[0], "same", 4);
    copy_bytes(twice.text[1], "same", 4);
    twice.bytes[0] = twice.bytes[1] = 4;
    const u32 duplicate = search_recent_encode(twice, {record, sizeof(record)});
    check(duplicate && !search_recent_decode({record, duplicate}, &untouched),
          "search_recent_rejects_duplicated_entries");

    wchar_t path[512];
    SearchRecentList loaded{};
    const bool have_path = temporary_path(path, 512);
    check(have_path && search_recent_save_file(path, list) && search_recent_load_file(path, &loaded) &&
              loaded.count == 5 && entry(loaded, 0, utf8_literal("delta")),
          "search_recent_persists_to_a_bounded_file");
    if (have_path) {
        DeleteFileW(path); // The atomic writer leaves a hidden file.
        HANDLE file =
            CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool rewritten =
            file != INVALID_HANDLE_VALUE && WriteFile(file, "MCSR\x01\x09garbage", 13, &written, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        check(rewritten && !search_recent_load_file(path, &loaded) && loaded.count == 0,
              "search_recent_corrupt_file_loads_empty");
        DeleteFileW(path);
        check(search_recent_load_file(path, &loaded) && loaded.count == 0,
              "search_recent_missing_file_is_empty");
    }
    wchar_t profile[512];
    check(search_recent_path(0x1234, profile, 512) && !search_recent_path(0, profile, 512),
          "search_recent_path_requires_a_profile");
    search_recent_clear(&list);
    check(!list.count && !search_recent_entry(list, 0).bytes, "search_recent_clear_empties_the_list");
}
} // namespace maicord
