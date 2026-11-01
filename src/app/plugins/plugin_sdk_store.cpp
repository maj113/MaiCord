#include "plugin_sdk_internal.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {

static constexpr u8 kStoreMagic[8]{'M', 'C', 'S', 'T', 'O', 'R', 'E', '1'};
constexpr u32 kStoreHeaderBytes = 24;
constexpr u32 kStoreFileBytes = kStoreHeaderBytes + kPluginSdkStoreKeys * 3 + kPluginSdkStoreBytes;

bool store_path(const NativePluginSdkBinding& binding, wchar_t (&path)[600], bool create) noexcept {
    const auto& runtime = *binding.runtime;
    const u32 base = runtime.data_directory_characters;
    if (!base || base >= 512) return false;
    copy_bytes(path, runtime.data_directory, base * 2);
    path[base] = 0;
    if (create) CreateDirectoryW(path, nullptr);
    u32 at = base;
    path[at++] = L'\\';
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    for (u32 i = 0; i < 16; ++i) path[at++] = digits[(binding.owner_id >> (60 - 4 * i)) & 15];
    path[at] = 0;
    if (create) CreateDirectoryW(path, nullptr);
    static constexpr wchar_t leaf[] = L"\\store.bin";
    for (u32 i = 0; i < sizeof(leaf) / sizeof(leaf[0]); ++i) path[at + i] = leaf[i];
    return true;
}
bool key_ok(mc_str key) noexcept {
    if (!key.bytes || key.bytes > kStoreKeyBytes || !key.data) return false;
    for (u32 i = 0; i < key.bytes; ++i)
        if (key.data[i] < 0x20 || key.data[i] > 0x7e) return false;
    return true;
}
bool store_decode(Store& store, const u8* file, u32 bytes) noexcept {
    if (bytes < kStoreHeaderBytes || !bytes_equal(file, kStoreMagic, 8)) return false;
    const u32 count = byte_load_le32(file + 8), payload = byte_load_le32(file + 12);
    if (count > kPluginSdkStoreKeys || payload != bytes - kStoreHeaderBytes ||
        byte_load_le64(file + 16) != hash64_bytes(kHash64Seed, file + kStoreHeaderBytes, payload)) return false;
    Store candidate{};
    u32 at = kStoreHeaderBytes;
    for (u32 i = 0; i < count; ++i) {
        if (bytes - at < 3) return false;
        const u32 key = file[at], value = static_cast<u32>(file[at + 1]) | static_cast<u32>(file[at + 2]) << 8;
        at += 3;
        if (!key || key > kStoreKeyBytes || value > kPluginSdkStoreValueBytes || bytes - at < key + value ||
            candidate.used + key + value > kPluginSdkStoreBytes ||
            !key_ok({reinterpret_cast<const char*>(file + at), key})) return false;
        candidate.entries[i] = {static_cast<u16>(candidate.used), static_cast<u16>(value), static_cast<u8>(key)};
        copy_bytes(candidate.data + candidate.used, file + at, key + value);
        candidate.used += key + value;
        at += key + value;
    }
    if (at != bytes) return false;
    candidate.count = count;
    candidate.loaded = true;
    store = candidate;
    return true;
}
void store_load(NativePluginSdkBinding& binding) noexcept {
    auto& store = binding.store;
    if (store.loaded) return;
    reset_value(&store);
    store.loaded = true;
    wchar_t path[600];
    if (!store_path(binding, path, false)) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    u8 buffer[kStoreFileBytes];
    DWORD read = 0;
    Win32LargeInteger size{};
    const bool ok = GetFileSizeEx(file, size.native()) && size.QuadPart <= kStoreFileBytes &&
                    ReadFile(file, buffer, static_cast<DWORD>(size.QuadPart), &read, nullptr) &&
                    read == static_cast<DWORD>(size.QuadPart);
    CloseHandle(file);
    // A damaged store starts empty; the next write replaces it.
    if (ok) store_decode(store, buffer, read);
}

} // namespace
namespace plugin_sdk_detail {

bool store_save(NativePluginSdkBinding& binding) noexcept {
    auto& store = binding.store;
    if (!store.loaded || !store.dirty) return true;
    wchar_t path[600], temporary[608];
    if (!store_path(binding, path, true)) {
        store.dirty = false; // memory-only runtime
        return true;
    }
    u8 buffer[kStoreFileBytes];
    copy_bytes(buffer, kStoreMagic, 8);
    u32 at = kStoreHeaderBytes;
    for (u32 i = 0; i < store.count; ++i) {
        const auto& entry = store.entries[i];
        buffer[at] = entry.key_bytes;
        buffer[at + 1] = static_cast<u8>(entry.value_bytes);
        buffer[at + 2] = static_cast<u8>(entry.value_bytes >> 8);
        at += 3;
        copy_bytes(buffer + at, store.data + entry.offset, static_cast<u32>(entry.key_bytes) + entry.value_bytes);
        at += static_cast<u32>(entry.key_bytes) + entry.value_bytes;
    }
    byte_store_le32(buffer + 8, store.count);
    byte_store_le32(buffer + 12, at - kStoreHeaderBytes);
    byte_store_le64(buffer + 16, hash64_bytes(kHash64Seed, buffer + kStoreHeaderBytes, at - kStoreHeaderBytes));
    u32 length = 0;
    while (path[length]) ++length;
    copy_bytes(temporary, path, length * 2);
    static constexpr wchar_t suffix[] = L".new";
    for (u32 i = 0; i < 5; ++i) temporary[length + i] = suffix[i];
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = WriteFile(file, buffer, at, &written, nullptr) && written == at;
    CloseHandle(file);
    if (!wrote || !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary);
        return false;
    }
    store.dirty = false;
    return true;
}

} // namespace plugin_sdk_detail
namespace {

u32 store_find(const Store& store, mc_str key) noexcept {
    for (u32 i = 0; i < store.count; ++i) {
        const auto& entry = store.entries[i];
        if (entry.key_bytes == key.bytes && bytes_equal(store.data + entry.offset, key.data, key.bytes)) return i;
    }
    return ~0u;
}
void store_erase(Store& store, u32 index) noexcept {
    const auto removed = store.entries[index];
    const u32 bytes = static_cast<u32>(removed.key_bytes) + removed.value_bytes;
    copy_bytes(store.data + removed.offset, store.data + removed.offset + bytes, store.used - removed.offset - bytes);
    store.used -= bytes;
    for (u32 i = index + 1; i < store.count; ++i) store.entries[i - 1] = store.entries[i];
    reset_value(&store.entries[--store.count]);
    for (u32 i = 0; i < store.count; ++i)
        if (store.entries[i].offset > removed.offset)
            store.entries[i].offset = static_cast<u16>(store.entries[i].offset - bytes);
}
NativePluginSdkBinding* store_binding(void* handle, uint64_t generation, mc_status* status) noexcept {
    auto* binding = active(handle, generation);
    *status = !binding ? MC_E_INACTIVE : !permitted(*binding, MC_PERM_STORE) ? MC_E_DENIED : MC_OK;
    if (*status != MC_OK) return nullptr;
    store_load(*binding);
    return binding;
}
mc_status MC_CALL store_get(void* handle, uint64_t generation, mc_str key, void* buffer, uint32_t capacity,
                            uint32_t* bytes) {
    mc_status status{};
    auto* binding = store_binding(handle, generation, &status);
    if (!binding) return status;
    if (!key_ok(key) || !bytes) return MC_E_INVALID;
    *bytes = 0;
    const auto& store = binding->store;
    const u32 index = store_find(store, key);
    if (index == ~0u) return MC_E_NOT_FOUND;
    const auto& entry = store.entries[index];
    *bytes = entry.value_bytes;
    if (capacity < entry.value_bytes) return MC_E_TRUNCATED;
    if (entry.value_bytes && !buffer) return MC_E_INVALID;
    copy_bytes(buffer, store.data + entry.offset + entry.key_bytes, entry.value_bytes);
    return MC_OK;
}
mc_status MC_CALL store_set(void* handle, uint64_t generation, mc_str key, const void* value, uint32_t bytes) {
    mc_status status{};
    auto* binding = store_binding(handle, generation, &status);
    if (!binding) return status;
    if (!key_ok(key) || (bytes && !value)) return MC_E_INVALID;
    if (bytes > kPluginSdkStoreValueBytes) return MC_E_CAPACITY;
    auto& store = binding->store;
    const u32 index = store_find(store, key);
    const u32 previous = index == ~0u ? 0 : static_cast<u32>(store.entries[index].key_bytes) + store.entries[index].value_bytes;
    if (store.used - previous + key.bytes + bytes > kPluginSdkStoreBytes ||
        (index == ~0u && store.count == kPluginSdkStoreKeys)) return MC_E_BUDGET;
    u8 copy[kPluginSdkStoreValueBytes];
    copy_bytes(copy, value, bytes);
    if (index != ~0u) store_erase(store, index);
    store.entries[store.count++] = {static_cast<u16>(store.used), static_cast<u16>(bytes), static_cast<u8>(key.bytes)};
    copy_bytes(store.data + store.used, key.data, key.bytes);
    copy_bytes(store.data + store.used + key.bytes, copy, bytes);
    store.used += key.bytes + bytes;
    store.dirty = true;
    return MC_OK;
}
mc_status MC_CALL store_remove(void* handle, uint64_t generation, mc_str key) {
    mc_status status{};
    auto* binding = store_binding(handle, generation, &status);
    if (!binding) return status;
    if (!key_ok(key)) return MC_E_INVALID;
    const u32 index = store_find(binding->store, key);
    if (index == ~0u) return MC_E_NOT_FOUND;
    store_erase(binding->store, index);
    binding->store.dirty = true;
    return MC_OK;
}
mc_status MC_CALL store_usage(void* handle, uint64_t generation, uint32_t* used, uint32_t* limit) {
    mc_status status{};
    auto* binding = store_binding(handle, generation, &status);
    if (!binding) return status;
    if (used) *used = binding->store.used;
    if (limit) *limit = kPluginSdkStoreBytes;
    return MC_OK;
}

} // namespace
namespace plugin_sdk_detail {

constexpr mc_store_v1 kStore{sizeof(mc_store_v1), 1, store_get, store_set, store_remove, store_usage};

// ---- mc.timer@1 -------------------------------------------------------------------------

} // namespace plugin_sdk_detail
} // namespace maicord
