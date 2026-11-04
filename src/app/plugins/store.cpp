#include "binding_internal.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_error.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::plugin_runtime_detail {
namespace {

constexpr u8 kMagic[8]{'M', 'C', 'P', 'K', 'V', '0', '0', '1'};
constexpr u32 kPageBytes = 4096;
constexpr u32 kHeaderBytes = 12;  // magic, entry count
constexpr u32 kEntryHeader = 5;   // key bytes (1), value bytes (4)
constexpr u32 kWriteBuffer = 4096;

struct Entry {
    u32 offset;      // into the heap
    u32 value_bytes;
    u32 key_bytes;
};

constexpr u32 round_page(u32 bytes) noexcept {
    return (bytes + kPageBytes - 1) & ~(kPageBytes - 1);
}
u32 table_bytes(const PluginKeyValue& kv) noexcept {
    return round_page(kv.maximum_keys * static_cast<u32>(sizeof(Entry)));
}
Entry* entries(const PluginKeyValue& kv) noexcept {
    return reinterpret_cast<Entry*>(kv.memory);
}
u8* heap(const PluginKeyValue& kv) noexcept {
    return kv.memory + table_bytes(kv);
}

// Makes the heap writable up to used bytes.
bool ensure(PluginKeyValue* kv, u32 used) noexcept {
    const u32 needed = table_bytes(*kv) + round_page(used);
    if (needed <= kv->committed_bytes) return true;
    if (needed > kv->reserved_bytes ||
        !pages_commit_at(kv->memory + kv->committed_bytes, needed - kv->committed_bytes))
        return false;
    kv->committed_bytes = needed;
    return true;
}

u32 find_index(const PluginKeyValue& kv, mc_str key) noexcept {
    const Entry* table = entries(kv);
    for (u32 i = 0; i < kv.count; ++i)
        if (table[i].key_bytes == key.bytes && bytes_equal(heap(kv) + table[i].offset, key.data, key.bytes)) return i;
    return ~0u;
}

void erase(PluginKeyValue* kv, u32 index) noexcept {
    Entry* table = entries(*kv);
    const Entry removed = table[index];
    const u32 bytes = removed.key_bytes + removed.value_bytes;
    copy_bytes_call(heap(*kv) + removed.offset, heap(*kv) + removed.offset + bytes, kv->used - removed.offset - bytes);
    kv->used -= bytes;
    for (u32 i = index + 1; i < kv->count; ++i) table[i - 1] = table[i];
    --kv->count;
    for (u32 i = 0; i < kv->count; ++i)
        if (table[i].offset > removed.offset) table[i].offset -= bytes;
}

void mark_dirty(PluginKeyValue* kv, u64 now_ms) noexcept {
    if (!kv->dirty) kv->dirty_since = now_ms;
    kv->dirty = true;
}

// Writes through a buffer; one failed write fails the file.
struct Writer {
    HANDLE file;
    u8 buffer[kWriteBuffer];
    u32 used;
    u64 hash;
    bool failed;

    void flush() noexcept {
        DWORD written = 0;
        if (used && (!WriteFile(file, buffer, used, &written, nullptr) || written != used)) failed = true;
        used = 0;
    }
    void put(const void* data, u32 bytes, bool hashed = true) noexcept {
        if (hashed) hash = hash64_bytes(hash, data, bytes);
        if (bytes >= kWriteBuffer) {
            flush();
            DWORD written = 0;
            if (!WriteFile(file, data, bytes, &written, nullptr) || written != bytes) failed = true;
            return;
        }
        if (used + bytes > kWriteBuffer) flush();
        copy_bytes(buffer + used, data, bytes);
        used += bytes;
    }
};

bool read_exact(HANDLE file, void* output, u32 bytes) noexcept {
    DWORD read = 0;
    return ReadFile(file, output, bytes, &read, nullptr) && read == bytes;
}

} // namespace

bool key_valid(mc_str key, u32 maximum_bytes) noexcept {
    if (!key.data || !key.bytes || key.bytes > maximum_bytes) return false;
    for (u32 i = 0; i < key.bytes; ++i)
        if (static_cast<u8>(key.data[i]) < 0x21 || static_cast<u8>(key.data[i]) > 0x7e) return false;
    return true;
}

bool kv_open(PluginKeyValue* kv, u32 maximum_keys, u32 maximum_key_bytes, u32 maximum_value_bytes,
             u32 maximum_total_bytes) noexcept {
    reset_value(kv);
    kv->maximum_keys = maximum_keys;
    kv->maximum_key_bytes = maximum_key_bytes;
    kv->maximum_value_bytes = maximum_value_bytes;
    kv->maximum_total_bytes = maximum_total_bytes;
    kv->reserved_bytes = table_bytes(*kv) + round_page(maximum_total_bytes);
    kv->memory = static_cast<u8*>(pages_reserve(kv->reserved_bytes));
    if (!kv->memory) return false;
    if (!pages_commit_at(kv->memory, table_bytes(*kv))) {
        pages_release(kv->memory);
        kv->memory = nullptr;
        return false;
    }
    kv->committed_bytes = table_bytes(*kv);
    return true;
}

void kv_close(PluginKeyValue* kv) noexcept {
    if (kv->memory) pages_release(kv->memory);
    reset_value(kv);
}

const u8* kv_find(const PluginKeyValue& kv, const mc_str& key, u32* bytes) noexcept {
    const u32 index = find_index(kv, key);
    if (index == ~0u) return nullptr;
    const Entry& entry = entries(kv)[index];
    *bytes = entry.value_bytes;
    return heap(kv) + entry.offset + entry.key_bytes;
}

mc_status kv_get(const PluginKeyValue& kv, mc_str key, void* output, u32 capacity, u32* bytes) noexcept {
    u32 size = 0;
    const u8* value = kv_find(kv, key, &size);
    *bytes = size;
    if (!value) return MC_E_NOT_FOUND;
    if (size > capacity) return MC_E_TRUNCATED;
    if (size && !output) return MC_E_INVALID;
    copy_bytes(output, value, size);
    return MC_OK;
}

mc_status kv_set(PluginKeyValue* kv, const mc_str& key, const void* value, u32 bytes, u64 now_ms) noexcept {
    if (bytes > kv->maximum_value_bytes) return MC_E_CAPACITY;
    const u32 index = find_index(*kv, key);
    const u32 previous = index == ~0u ? 0u : entries(*kv)[index].key_bytes + entries(*kv)[index].value_bytes;
    if (kv->used - previous + key.bytes + bytes > kv->maximum_total_bytes ||
        (index == ~0u && kv->count == kv->maximum_keys))
        return MC_E_BUDGET;
    // The plugin's value never aliases the table, so the old entry can go first.
    if (index != ~0u) erase(kv, index);
    if (!ensure(kv, kv->used + key.bytes + bytes)) return MC_E_BUDGET;
    Entry& entry = entries(*kv)[kv->count++];
    entry = {kv->used, bytes, key.bytes};
    copy_bytes(heap(*kv) + kv->used, key.data, key.bytes);
    copy_bytes(heap(*kv) + kv->used + key.bytes, value, bytes);
    kv->used += key.bytes + bytes;
    mark_dirty(kv, now_ms);
    return MC_OK;
}

mc_status kv_remove(PluginKeyValue* kv, mc_str key, u64 now_ms) noexcept {
    const u32 index = find_index(*kv, key);
    if (index == ~0u) return MC_E_NOT_FOUND;
    erase(kv, index);
    mark_dirty(kv, now_ms);
    return MC_OK;
}

bool kv_load(PluginKeyValue* kv, const wchar_t* path) noexcept {
    kv->loaded = true;
    kv->count = 0;
    kv->used = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    u8 header[kHeaderBytes];
    u64 hash = 0;
    bool ok = read_exact(file, header, kHeaderBytes) && bytes_equal(header, kMagic, 8);
    const u32 count = ok ? byte_load_le32(header + 8) : 0;
    ok = ok && count <= kv->maximum_keys;
    if (ok) hash = hash64_bytes(kHash64Seed, header, kHeaderBytes);
    for (u32 i = 0; ok && i < count; ++i) {
        u8 record[kEntryHeader];
        ok = read_exact(file, record, kEntryHeader);
        const u32 key_bytes = ok ? record[0] : 0, value_bytes = ok ? byte_load_le32(record + 1) : 0;
        ok = ok && key_bytes && key_bytes <= kv->maximum_key_bytes && value_bytes <= kv->maximum_value_bytes &&
             kv->used + key_bytes + value_bytes <= kv->maximum_total_bytes && ensure(kv, kv->used + key_bytes + value_bytes);
        if (!ok) break;
        u8* at = heap(*kv) + kv->used;
        ok = read_exact(file, at, key_bytes) && key_valid({reinterpret_cast<const char*>(at), key_bytes, 0}, key_bytes) &&
             read_exact(file, at + key_bytes, value_bytes) &&
             find_index(*kv, {reinterpret_cast<const char*>(at), key_bytes, 0}) == ~0u;
        if (!ok) break;
        hash = hash64_bytes(hash, record, kEntryHeader);
        hash = hash64_bytes(hash, at, key_bytes);
        hash = hash64_bytes(hash, at + key_bytes, value_bytes);
        entries(*kv)[kv->count++] = {kv->used, value_bytes, key_bytes};
        kv->used += key_bytes + value_bytes;
    }
    u8 tail[9];
    // The digest ends the file exactly: one more byte means it is not ours.
    DWORD tail_read = 0;
    ok = ok && ReadFile(file, tail, sizeof(tail), &tail_read, nullptr) && tail_read == 8 && byte_load_le64(tail) == hash;
    CloseHandle(file);
    if (!ok) {
        // A damaged table starts empty; the next change replaces the file.
        kv->count = 0;
        kv->used = 0;
    }
    return ok;
}

bool kv_save(PluginKeyValue* kv, const wchar_t* path) noexcept {
    wchar_t temporary[640];
    u32 length = 0;
    while (path[length]) ++length;
    if (length + 5 > sizeof(temporary) / sizeof(temporary[0])) return false;
    copy_bytes(temporary, path, length * 2);
    static constexpr wchar_t suffix[] = L".new";
    for (u32 i = 0; i < 5; ++i) temporary[length + i] = suffix[i];
    // Create <data>\<owner>: the two directories above the file.
    u32 owner = length, data = length;
    while (owner && path[owner - 1] != L'\\') --owner;
    data = owner ? owner - 1 : 0;
    while (data && path[data - 1] != L'\\') --data;
    wchar_t directory[640];
    const u32 ends[2]{data ? data - 1 : 0, owner ? owner - 1 : 0};
    for (const u32 end : ends) {
        if (!end) continue;
        copy_bytes(directory, path, end * 2);
        directory[end] = 0;
        CreateDirectoryW(directory, nullptr);
    }
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    Writer writer;
    writer.file = file;
    writer.used = 0;
    writer.hash = kHash64Seed;
    writer.failed = false;
    u8 header[kHeaderBytes];
    copy_bytes(header, kMagic, 8);
    byte_store_le32(header + 8, kv->count);
    writer.put(header, kHeaderBytes);
    for (u32 i = 0; i < kv->count; ++i) {
        const Entry& entry = entries(*kv)[i];
        u8 record[kEntryHeader];
        record[0] = static_cast<u8>(entry.key_bytes);
        byte_store_le32(record + 1, entry.value_bytes);
        writer.put(record, kEntryHeader);
        writer.put(heap(*kv) + entry.offset, entry.key_bytes);
        writer.put(heap(*kv) + entry.offset + entry.key_bytes, entry.value_bytes);
    }
    u8 digest[8];
    byte_store_le64(digest, writer.hash);
    writer.put(digest, 8, false);
    writer.flush();
    const bool wrote = !writer.failed;
    const bool closed = CloseHandle(file) != 0;
    if (!wrote || !closed || !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary);
        return false;
    }
    kv->dirty = false;
    return true;
}

} // namespace maicord::plugin_runtime_detail
