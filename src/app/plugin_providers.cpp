#include "maicord/app/plugin_providers.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/app/plugin_index.hpp"
#include "maicord/app/update_fetch.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {
using RE = AcceptanceRecordError;
constexpr u8 kMagic[8]{'M','A','I','P','P','R','O','V'};
AcceptanceRecordResult fail(RE error, DWORD native = 0) noexcept { return {error, native}; }
bool zero(const void* data, u32 bytes) noexcept {
    const auto* p = static_cast<const u8*>(data);
    for (u32 i = 0; i < bytes; ++i) if (p[i]) return false;
    return true;
}
bool disjoint(ConstSlice<u8> a, ConstSlice<u8> b) noexcept {
    return byte_range_valid(a) && byte_range_valid(b) && !byte_ranges_overlap(a, b);
}
// "/" or "/segment/.../" of RFC 3986 path characters, without empty, "." or ".." segments.
bool base_path_valid(const char* path, u32 bytes) noexcept {
    if (!bytes || bytes > kPluginProviderPathCapacity || path[0] != '/' || path[bytes - 1] != '/') return false;
    u32 start = 1;
    for (u32 i = 1; i < bytes; ++i) {
        const char c = path[i];
        if (c == '/') {
            const u32 n = i - start;
            if (!n || (n == 1 && path[start] == '.') || (n == 2 && path[start] == '.' && path[start + 1] == '.')) return false;
            start = i + 1;
            continue;
        }
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        for (const char* a = "-._~!$&'()*+,;=:@%"; *a && !allowed; ++a) allowed = c == *a;
        if (!allowed) return false;
    }
    return true;
}
// Field by field: the struct has tail padding.
bool entry_empty(const PluginProviderRecord& e) noexcept {
    return !e.id && !e.added && !e.flags && !e.host_bytes && !e.path_bytes && !e.name_bytes && zero(e.host, sizeof(e.host)) &&
        zero(e.path, sizeof(e.path)) && zero(e.key, sizeof(e.key)) && zero(e.fingerprint, sizeof(e.fingerprint)) &&
        zero(e.name, sizeof(e.name));
}
bool entry_valid(const PluginProviderRecord& e) noexcept {
    if (e.id <= kPluginProviderOfficial || (e.flags & ~kPluginProviderKnownFlags) || !e.host_bytes ||
        e.host_bytes > kPluginProviderHostCapacity || e.path_bytes > kPluginProviderPathCapacity ||
        e.name_bytes > kPluginProviderNameCapacity) return false;
    for (u32 i = 0; i < e.host_bytes; ++i) if (e.host[i] >= 'A' && e.host[i] <= 'Z') return false;
    if (!update_origin_host_valid({e.host, e.host_bytes}) || !base_path_valid(e.path, e.path_bytes) ||
        !plugin_index_display_text_valid({e.name, e.name_bytes}, kPluginProviderNameCapacity, true)) return false;
    const bool signed_provider = e.flags & kPluginProviderSigned;
    if (signed_provider ? e.key[0] != 4 || zero(e.fingerprint, 32) : !zero(e.key, 65) || !zero(e.fingerprint, 32)) return false;
    return zero(e.host + e.host_bytes, kPluginProviderHostCapacity - e.host_bytes) &&
        zero(e.path + e.path_bytes, kPluginProviderPathCapacity - e.path_bytes) &&
        zero(e.name + e.name_bytes, kPluginProviderNameCapacity - e.name_bytes);
}
} // namespace

u64 plugin_record_checksum(ConstSlice<u8> record) noexcept {
    return hash64_bytes(hash64_bytes(kHash64Seed, record.data, 24), record.data + 32, record.count - 32);
}

AcceptanceRecordResult plugin_record_read(ConstSlice<wchar_t> path, Slice<u8> record) noexcept {
    if (!record.data || !acceptance_record_path_valid(path) ||
        !disjoint({reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}, {record.data, record.count}))
        return fail(RE::invalid_input);
    wchar_t terminated[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_terminate(path, terminated);
    const auto result = acceptance_record_ancestors(terminated, path.count);
    return result.ok() ? acceptance_record_read(terminated, record) : result;
}

AcceptanceRecordResult plugin_record_write(ConstSlice<wchar_t> path, ConstSlice<u8> record) noexcept {
    if (!record.data || !record.count || !acceptance_record_path_valid(path)) return fail(RE::invalid_input);
    wchar_t terminated[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_terminate(path, terminated);
    const auto ancestors = acceptance_record_ancestors(terminated, path.count);
    if (!ancestors.ok()) return ancestors;
    const auto written = file_persistence_write_atomic(terminated, path.count, record);
    switch (written.error) {
    case FilePersistenceWriteError::none: return {};
    case FilePersistenceWriteError::open: return fail(RE::open, written.native_error);
    case FilePersistenceWriteError::write: return fail(RE::write, written.native_error);
    case FilePersistenceWriteError::flush: return fail(RE::flush, written.native_error);
    default: return fail(RE::commit, written.native_error);
    }
}

bool plugin_providers_valid(const PluginProviders& providers) noexcept {
    if (providers.count > kPluginProviderMaximum || (providers.official_flags & ~kPluginOfficialKnownFlags)) return false;
    for (u32 i = 0; i < providers.count; ++i) {
        if (!entry_valid(providers.entries[i])) return false;
        for (u32 j = 0; j < i; ++j) if (providers.entries[j].id == providers.entries[i].id) return false;
    }
    for (u32 i = providers.count; i < kPluginProviderMaximum; ++i)
        if (!entry_empty(providers.entries[i])) return false;
    return true;
}

AcceptanceRecordResult plugin_providers_encode(const PluginProviders& providers, Slice<u8> record) noexcept {
    if (record.count != kPluginProvidersRecordBytes || !record.data || !plugin_providers_valid(providers) ||
        !disjoint({record.data, record.count}, {reinterpret_cast<const u8*>(&providers), sizeof(providers)}))
        return fail(RE::invalid_input);
    clear_bytes(record.data, record.count);
    copy_bytes(record.data, kMagic, 8);
    byte_store_le32(record.data + 8, kPluginProvidersVersion);
    byte_store_le32(record.data + 12, record.count);
    byte_store_le32(record.data + 16, providers.count);
    byte_store_le32(record.data + 20, providers.official_flags);
    for (u32 i = 0; i < providers.count; ++i) {
        const PluginProviderRecord& e = providers.entries[i];
        u8* row = record.data + 32 + i * kPluginProvidersEntryBytes;
        byte_store_le64(row, e.id);
        byte_store_le64(row + 8, e.added);
        byte_store_le32(row + 16, e.flags);
        row[20] = static_cast<u8>(e.host_bytes); row[21] = static_cast<u8>(e.path_bytes); row[22] = static_cast<u8>(e.name_bytes);
        copy_bytes(row + 24, e.host, kPluginProviderHostCapacity);
        copy_bytes(row + 88, e.path, kPluginProviderPathCapacity);
        copy_bytes(row + 280, e.key, 65);
        copy_bytes(row + 348, e.fingerprint, 32);
        copy_bytes(row + 380, e.name, kPluginProviderNameCapacity);
    }
    byte_store_le64(record.data + 24, plugin_record_checksum({record.data, record.count}));
    return {};
}

AcceptanceRecordResult plugin_providers_decode(ConstSlice<u8> record, PluginProviders* providers) noexcept {
    if (!providers || !record.data || !disjoint(record, {reinterpret_cast<const u8*>(providers), sizeof(*providers)}))
        return fail(RE::invalid_input);
    if (record.count != kPluginProvidersRecordBytes) return fail(RE::size);
    if (!bytes_equal(record.data, kMagic, 8) || byte_load_le32(record.data + 12) != record.count) return fail(RE::format);
    if (byte_load_le32(record.data + 8) != kPluginProvidersVersion) return fail(RE::version);
    if (byte_load_le64(record.data + 24) != plugin_record_checksum(record)) return fail(RE::checksum);
    PluginProviders candidate{};
    candidate.count = byte_load_le32(record.data + 16);
    candidate.official_flags = byte_load_le32(record.data + 20);
    for (u32 i = 0; i < kPluginProviderMaximum; ++i) {
        const u8* row = record.data + 32 + i * kPluginProvidersEntryBytes;
        PluginProviderRecord& e = candidate.entries[i];
        if (row[23] || !zero(row + 345, 3) || !zero(row + 444, 4)) return fail(RE::format);
        e.id = byte_load_le64(row);
        e.added = byte_load_le64(row + 8);
        e.flags = byte_load_le32(row + 16);
        e.host_bytes = row[20]; e.path_bytes = row[21]; e.name_bytes = row[22];
        copy_bytes(e.host, row + 24, kPluginProviderHostCapacity);
        copy_bytes(e.path, row + 88, kPluginProviderPathCapacity);
        copy_bytes(e.key, row + 280, 65);
        copy_bytes(e.fingerprint, row + 348, 32);
        copy_bytes(e.name, row + 380, kPluginProviderNameCapacity);
    }
    if (!plugin_providers_valid(candidate)) return fail(RE::format);
    *providers = candidate;
    return {};
}

AcceptanceRecordResult plugin_providers_load(ConstSlice<wchar_t> path, PluginProviders* providers) noexcept {
    if (!providers) return fail(RE::invalid_input);
    u8 record[kPluginProvidersRecordBytes];
    const auto result = plugin_record_read(path, {record, sizeof(record)});
    return result.ok() ? plugin_providers_decode({record, sizeof(record)}, providers) : result;
}

AcceptanceRecordResult plugin_providers_save(ConstSlice<wchar_t> path, const PluginProviders& providers) noexcept {
    u8 record[kPluginProvidersRecordBytes];
    const auto result = plugin_providers_encode(providers, {record, sizeof(record)});
    return result.ok() ? plugin_record_write(path, {record, sizeof(record)}) : result;
}

} // namespace maicord
