#include "maicord/app/search_recent.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_process.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u8 kMagic[] = {'M', 'C', 'S', 'R'};
constexpr u8 kVersion = 1;

bool text_valid(Utf8View text) noexcept {
    if (!text.bytes || text.bytes > kSearchRecentTextBytes || !utf8_validate(text).valid)
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (static_cast<u8>(text.data[i]) < 0x20 || text.data[i] == 0x7f)
            return false;
    return true;
}
bool same(const SearchRecentList& list, u32 index, Utf8View text) noexcept {
    return list.bytes[index] == text.bytes && bytes_equal(list.text[index], text.data, text.bytes);
}
} // namespace

bool search_recent_push(SearchRecentList* list, Utf8View text) noexcept {
    if (!list || list->count > kSearchRecentCapacity || (!text.data && text.bytes))
        return false;
    while (text.bytes && text.data[0] == ' ')
        text = {text.data + 1, text.bytes - 1};
    while (text.bytes && text.data[text.bytes - 1] == ' ')
        --text.bytes;
    if (!text_valid(text))
        return false;
    u32 found = list->count;
    for (u32 i = 0; i < list->count; ++i)
        if (same(*list, i, text))
            found = i;
    if (found == 0 && list->count)
        return true;
    // Shift the newer entries down over the duplicate or the oldest slot.
    u32 last = found < list->count ? found : minimum<u32>(list->count, kSearchRecentCapacity - 1);
    for (u32 i = last; i > 0; --i) {
        copy_bytes(list->text[i], list->text[i - 1], list->bytes[i - 1]);
        list->bytes[i] = list->bytes[i - 1];
    }
    copy_bytes(list->text[0], text.data, text.bytes);
    list->bytes[0] = static_cast<u16>(text.bytes);
    if (found == list->count && list->count < kSearchRecentCapacity)
        ++list->count;
    ++list->revision;
    return true;
}

void search_recent_clear(SearchRecentList* list) noexcept {
    if (!list)
        return;
    const u32 revision = list->revision + 1;
    clear_bytes(list, sizeof(*list));
    list->revision = revision;
}

Utf8View search_recent_entry(const SearchRecentList& list, u32 index) noexcept {
    return index < list.count && index < kSearchRecentCapacity
               ? Utf8View{list.text[index], list.bytes[index]}
               : Utf8View{};
}

u32 search_recent_encode(const SearchRecentList& list, Slice<u8> output) noexcept {
    if (list.count > kSearchRecentCapacity || !output.data)
        return 0;
    u32 used = 0;
    const auto put = [&](const void* data, u32 bytes) noexcept {
        if (bytes > output.count - used)
            return false;
        copy_bytes(output.data + used, data, bytes);
        used += bytes;
        return true;
    };
    const u8 header[] = {kMagic[0], kMagic[1], kMagic[2], kMagic[3], kVersion, list.count};
    if (!put(header, sizeof(header)))
        return 0;
    for (u32 i = 0; i < list.count; ++i) {
        const auto entry = search_recent_entry(list, i);
        const u8 length[] = {static_cast<u8>(entry.bytes), static_cast<u8>(entry.bytes >> 8)};
        if (!text_valid(entry) || !put(length, 2) || !put(entry.data, entry.bytes))
            return 0;
    }
    const u32 hash = hash32_bytes(output.data, used);
    const u8 tail[] = {static_cast<u8>(hash), static_cast<u8>(hash >> 8), static_cast<u8>(hash >> 16),
                       static_cast<u8>(hash >> 24)};
    return put(tail, 4) ? used : 0;
}

bool search_recent_decode(ConstSlice<u8> input, SearchRecentList* list) noexcept {
    if (!list || !input.data || input.count < 10 || input.count > kSearchRecentFileBytes)
        return false;
    const u32 body = input.count - 4;
    const u32 stored = input[body] | (u32{input[body + 1]} << 8) | (u32{input[body + 2]} << 16) |
                       (u32{input[body + 3]} << 24);
    if (!bytes_equal(input.data, kMagic, 4) || input[4] != kVersion || input[5] > kSearchRecentCapacity ||
        stored != hash32_bytes(input.data, body))
        return false;
    SearchRecentList decoded{};
    u32 at = 6;
    for (u32 i = 0; i < input[5]; ++i) {
        if (body - at < 2)
            return false;
        const u32 bytes = input[at] | (u32{input[at + 1]} << 8);
        at += 2;
        const Utf8View text{reinterpret_cast<const char*>(input.data + at), bytes};
        if (bytes > body - at || !text_valid(text))
            return false;
        for (u32 j = 0; j < decoded.count; ++j)
            if (same(decoded, j, text))
                return false;
        copy_bytes(decoded.text[i], text.data, bytes);
        decoded.bytes[i] = static_cast<u16>(bytes);
        decoded.count = static_cast<u8>(i + 1);
        at += bytes;
    }
    if (at != body)
        return false;
    decoded.revision = list->revision + 1;
    *list = decoded;
    return true;
}

bool search_recent_path(u64 profile_id, wchar_t* path, u32 capacity) noexcept {
    if (!profile_id)
        return false;
    const DataPathResult built = data_path(path, capacity, L"accounts\\search-");
    u32 length = built.length;
    return built.ok() && wide_append_hex(path, capacity, &length, profile_id) &&
           wide_append(path, capacity, &length, L".recent");
}

bool search_recent_load_file(const wchar_t* path, SearchRecentList* list) noexcept {
    if (!path || !list)
        return false;
    const u32 revision = list->revision;
    search_recent_clear(list);
    list->revision = revision + 1;
    u8 record[kSearchRecentFileBytes];
    const FileReadResult read = file_persistence_read_file(path, {record, sizeof(record)});
    return read.error == FileReadError::missing || (read.ok() && search_recent_decode({record, read.bytes}, list));
}

bool search_recent_save_file(const wchar_t* path, const SearchRecentList& list) noexcept {
    u8 record[kSearchRecentFileBytes];
    const u32 bytes = search_recent_encode(list, {record, sizeof(record)});
    const u32 length = wide_length(path, 32768);
    return bytes && length && file_persistence_write_atomic(path, length, {record, bytes}).ok();
}

} // namespace maicord
