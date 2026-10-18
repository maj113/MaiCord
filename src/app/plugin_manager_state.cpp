#include "maicord/app/plugin_manager_state.hpp"
#include "maicord/app/plugin_manifest.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool child_path(const wchar_t* parent, u32 parent_count, const wchar_t* leaf, wchar_t* output, u32* count) noexcept {
    u32 length = 0;
    output[0] = 0;
    if (parent_count >= kPluginManagerPathCapacity) return false;
    copy_bytes(output, parent, parent_count * static_cast<u32>(sizeof(wchar_t)));
    length = parent_count;
    output[length] = 0;
    if (!wide_append(output, kPluginManagerPathCapacity, &length, L"\\") || !wide_append(output, kPluginManagerPathCapacity, &length, leaf))
        return false;
    *count = length;
    return true;
}
using RE = AcceptanceRecordError;
constexpr u8 kStateMagic[8]{'M','A','I','P','S','T','A','T'};
constexpr u8 kJournalMagic[8]{'M','A','I','P','J','R','N','L'};
AcceptanceRecordResult fail(RE error) noexcept { return {error, 0}; }
// Empty, or the index slug grammar [a-z0-9][a-z0-9-]* followed only by zero bytes.
bool slug_valid(const char* slug) noexcept {
    u32 n = 0;
    while (n < kPluginManagerSlugCapacity && slug[n]) {
        const char c = slug[n];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (c != '-' || !n)) return false;
        ++n;
    }
    return bytes_zero(slug + n, kPluginManagerSlugCapacity - n);
}
bool entry_valid(const PluginManagerEntry& e) noexcept {
    return e.owner > 1 && !(e.flags & ~kPluginStateKnownFlags) && e.max_host_abi <= 0xFFFF &&
        (e.previous_code || bytes_zero(e.previous_sha256, 32)) && slug_valid(e.slug);
}
// Header shared by both records; the caller validates count.
bool header(ConstSlice<u8> record, const u8* magic, u32 version, u32 bytes, RE* error) noexcept {
    if (record.count != bytes) { *error = RE::size; return false; }
    if (!bytes_equal(record.data, magic, 8) || byte_load_le32(record.data + 12) != bytes || byte_load_le32(record.data + 20)) {
        *error = RE::format; return false;
    }
    if (byte_load_le32(record.data + 8) != version) { *error = RE::version; return false; }
    if (byte_load_le64(record.data + 24) != plugin_record_checksum(record)) { *error = RE::checksum; return false; }
    return true;
}
void begin(Slice<u8> record, const u8* magic, u32 version, u32 count) noexcept {
    clear_bytes(record.data, record.count);
    copy_bytes(record.data, magic, 8);
    byte_store_le32(record.data + 8, version);
    byte_store_le32(record.data + 12, record.count);
    byte_store_le32(record.data + 16, count);
}
void seal(Slice<u8> record) noexcept {
    byte_store_le64(record.data + 24, plugin_record_checksum({record.data, record.count}));
}
} // namespace

bool plugin_manager_paths_build(ConstSlice<wchar_t> base, PluginManagerPaths* paths) noexcept {
    if (!paths || !base.data || !base.count || base.count > kPluginManagerBaseMaximumCharacters || !acceptance_record_path_valid(base) ||
        base[base.count - 1] == L'\\' || base[base.count - 1] == L'/' ||
        !byte_ranges_disjoint({reinterpret_cast<const u8*>(base.data), base.count * static_cast<u32>(sizeof(wchar_t))},
            {reinterpret_cast<const u8*>(paths), static_cast<u32>(sizeof(*paths))}))
        return false;
    clear_bytes(paths, sizeof(*paths));
    PluginManagerPaths& p = *paths;
    copy_bytes(p.base, base.data, base.count * static_cast<u32>(sizeof(wchar_t)));
    p.base_count = base.count;
    const bool built = child_path(p.base, p.base_count, L"plugins", p.plugins, &p.plugins_count) &&
        child_path(p.base, p.base_count, L"plugins.bin", p.trust, &p.trust_count) &&
        child_path(p.base, p.base_count, L"plugin-data", p.data, &p.data_count) &&
        child_path(p.base, p.base_count, L"plugin-manager", p.manager, &p.manager_count) &&
        child_path(p.manager, p.manager_count, L"acceptance.bin", p.acceptance, &p.acceptance_count) &&
        child_path(p.manager, p.manager_count, L"providers.bin", p.providers, &p.providers_count) &&
        child_path(p.manager, p.manager_count, L"state.bin", p.state, &p.state_count) &&
        child_path(p.manager, p.manager_count, L"journal.bin", p.journal, &p.journal_count) &&
        child_path(p.manager, p.manager_count, L"staging", p.staging, &p.staging_count) &&
        child_path(p.manager, p.manager_count, L"rollback", p.rollback, &p.rollback_count) &&
        child_path(p.manager, p.manager_count, L"cache", p.cache, &p.cache_count);
    if (!built) clear_bytes(paths, sizeof(*paths));
    return built;
}

bool plugin_manager_paths_ensure(const PluginManagerPaths& paths, u32* native_error) noexcept {
    const wchar_t* directories[]{paths.manager, paths.staging, paths.rollback, paths.cache};
    if (!paths.manager_count) return false;
    for (const wchar_t* directory : directories)
        if (!directory_ensure(directory, native_error)) return false;
    return true;
}

u32 plugin_manager_id_path(const wchar_t* directory, u32 count, u64 id, const wchar_t* suffix,
    wchar_t (&output)[kPluginManagerPathCapacity]) noexcept {
    u32 length = 0;
    if (!directory || !count || count >= kPluginManagerPathCapacity) return 0;
    copy_bytes(output, directory, count * static_cast<u32>(sizeof(wchar_t)));
    length = count;
    output[length] = 0;
    if (!wide_append(output, kPluginManagerPathCapacity, &length, L"\\") ||
        !wide_append_hex(output, kPluginManagerPathCapacity, &length, id) ||
        !wide_append(output, kPluginManagerPathCapacity, &length, suffix ? suffix : L""))
        return 0;
    return length;
}

bool plugin_manager_state_valid(const PluginManagerState& state) noexcept {
    if (state.count > kPluginManagerStateMaximumEntries) return false;
    for (u32 i = 0; i < state.count; ++i) {
        if (!entry_valid(state.entries[i])) return false;
        for (u32 j = 0; j < i; ++j) if (state.entries[j].owner == state.entries[i].owner) return false;
    }
    static_assert(sizeof(PluginManagerEntry) == kPluginManagerStateEntryBytes, "no padding: unused entries compare as bytes");
    for (u32 i = state.count; i < kPluginManagerStateMaximumEntries; ++i)
        if (!bytes_zero(&state.entries[i], sizeof(PluginManagerEntry))) return false;
    return true;
}

AcceptanceRecordResult plugin_manager_state_encode(const PluginManagerState& state, Slice<u8> record) noexcept {
    if (record.count != kPluginManagerStateRecordBytes || !record.data || !plugin_manager_state_valid(state) ||
        !byte_ranges_disjoint({record.data, record.count}, {reinterpret_cast<const u8*>(&state), sizeof(state)}))
        return fail(RE::invalid_input);
    begin(record, kStateMagic, kPluginManagerStateVersion, state.count);
    for (u32 i = 0; i < state.count; ++i) {
        const PluginManagerEntry& e = state.entries[i];
        u8* row = record.data + 32 + i * kPluginManagerStateEntryBytes;
        byte_store_le64(row, e.owner);
        byte_store_le64(row + 8, e.provider);
        byte_store_le32(row + 16, e.flags);
        byte_store_le32(row + 20, e.max_host_abi);
        byte_store_le64(row + 24, e.installed_code);
        copy_bytes(row + 32, e.installed_sha256, 32);
        byte_store_le64(row + 64, e.previous_code);
        copy_bytes(row + 72, e.previous_sha256, 32);
        byte_store_le64(row + 104, e.held_code);
        byte_store_le64(row + 112, e.admitted_sequence);
        copy_bytes(row + 120, e.slug, kPluginManagerSlugCapacity);
    }
    seal(record);
    return {};
}

AcceptanceRecordResult plugin_manager_state_decode(ConstSlice<u8> record, PluginManagerState* state) noexcept {
    if (!state || !record.data || !byte_ranges_disjoint(record, {reinterpret_cast<const u8*>(state), sizeof(*state)})) return fail(RE::invalid_input);
    RE error{};
    if (!header(record, kStateMagic, kPluginManagerStateVersion, kPluginManagerStateRecordBytes, &error)) return fail(error);
    PluginManagerState candidate{};
    candidate.count = byte_load_le32(record.data + 16);
    for (u32 i = 0; i < kPluginManagerStateMaximumEntries; ++i) {
        const u8* row = record.data + 32 + i * kPluginManagerStateEntryBytes;
        PluginManagerEntry& e = candidate.entries[i];
        e.owner = byte_load_le64(row);
        e.provider = byte_load_le64(row + 8);
        e.flags = byte_load_le32(row + 16);
        e.max_host_abi = byte_load_le32(row + 20);
        e.installed_code = byte_load_le64(row + 24);
        copy_bytes(e.installed_sha256, row + 32, 32);
        e.previous_code = byte_load_le64(row + 64);
        copy_bytes(e.previous_sha256, row + 72, 32);
        e.held_code = byte_load_le64(row + 104);
        e.admitted_sequence = byte_load_le64(row + 112);
        copy_bytes(e.slug, row + 120, kPluginManagerSlugCapacity);
    }
    if (!plugin_manager_state_valid(candidate)) return fail(RE::format);
    *state = candidate;
    return {};
}

AcceptanceRecordResult plugin_manager_state_load(ConstSlice<wchar_t> path, PluginManagerState* state) noexcept {
    if (!state) return fail(RE::invalid_input);
    u8 record[kPluginManagerStateRecordBytes];
    const auto result = plugin_record_read(path, {record, sizeof(record)});
    return result.ok() ? plugin_manager_state_decode({record, sizeof(record)}, state) : result;
}

AcceptanceRecordResult plugin_manager_state_save(ConstSlice<wchar_t> path, const PluginManagerState& state) noexcept {
    u8 record[kPluginManagerStateRecordBytes];
    const auto result = plugin_manager_state_encode(state, {record, sizeof(record)});
    return result.ok() ? plugin_record_write(path, {record, sizeof(record)}) : result;
}

bool plugin_journal_valid(const PluginJournal& j) noexcept {
    if (j.operation == PluginJournalOperation::none)
        return j.phase == PluginJournalPhase::none && !j.enable_after && !j.had_trust && !j.owner && !j.provider && !j.to_code &&
            !j.approved_permissions && bytes_zero(j.from_sha256, 32) && bytes_zero(j.to_sha256, 32);
    return j.operation <= PluginJournalOperation::uninstall && j.phase != PluginJournalPhase::none &&
        j.phase <= PluginJournalPhase::committed && j.owner > 1 && !(j.approved_permissions & ~kNativePluginPermissionsAll);
}

AcceptanceRecordResult plugin_journal_encode(const PluginJournal& journal, Slice<u8> record) noexcept {
    if (record.count != kPluginJournalRecordBytes || !record.data || !plugin_journal_valid(journal) ||
        !byte_ranges_disjoint({record.data, record.count}, {reinterpret_cast<const u8*>(&journal), sizeof(journal)}))
        return fail(RE::invalid_input);
    const bool active = journal.operation != PluginJournalOperation::none;
    begin(record, kJournalMagic, kPluginJournalVersion, active ? 1 : 0);
    u8* body = record.data + 32;
    body[0] = static_cast<u8>(journal.operation);
    body[1] = static_cast<u8>(journal.phase);
    body[2] = journal.enable_after ? 1 : 0;
    body[3] = journal.had_trust ? 1 : 0;
    byte_store_le64(body + 8, journal.owner);
    byte_store_le64(body + 16, journal.provider);
    byte_store_le64(body + 24, journal.to_code);
    byte_store_le64(body + 32, journal.approved_permissions);
    copy_bytes(body + 40, journal.from_sha256, 32);
    copy_bytes(body + 72, journal.to_sha256, 32);
    seal(record);
    return {};
}

AcceptanceRecordResult plugin_journal_decode(ConstSlice<u8> record, PluginJournal* journal) noexcept {
    if (!journal || !record.data || !byte_ranges_disjoint(record, {reinterpret_cast<const u8*>(journal), sizeof(*journal)})) return fail(RE::invalid_input);
    RE error{};
    if (!header(record, kJournalMagic, kPluginJournalVersion, kPluginJournalRecordBytes, &error)) return fail(error);
    const u8* body = record.data + 32;
    const u32 count = byte_load_le32(record.data + 16);
    if (count > 1 || body[2] > 1 || body[3] > 1 || !bytes_zero(body + 4, 4) || !bytes_zero(body + 104, 24) ||
        body[0] > static_cast<u8>(PluginJournalOperation::uninstall) || body[1] > static_cast<u8>(PluginJournalPhase::committed))
        return fail(RE::format);
    PluginJournal candidate{};
    candidate.operation = static_cast<PluginJournalOperation>(body[0]);
    candidate.phase = static_cast<PluginJournalPhase>(body[1]);
    candidate.enable_after = body[2] != 0;
    candidate.had_trust = body[3] != 0;
    candidate.owner = byte_load_le64(body + 8);
    candidate.provider = byte_load_le64(body + 16);
    candidate.to_code = byte_load_le64(body + 24);
    candidate.approved_permissions = byte_load_le64(body + 32);
    copy_bytes(candidate.from_sha256, body + 40, 32);
    copy_bytes(candidate.to_sha256, body + 72, 32);
    if (!plugin_journal_valid(candidate) || (count == 1) != (candidate.operation != PluginJournalOperation::none))
        return fail(RE::format);
    *journal = candidate;
    return {};
}

AcceptanceRecordResult plugin_journal_load(ConstSlice<wchar_t> path, PluginJournal* journal) noexcept {
    if (!journal) return fail(RE::invalid_input);
    u8 record[kPluginJournalRecordBytes];
    const auto result = plugin_record_read(path, {record, sizeof(record)});
    return result.ok() ? plugin_journal_decode({record, sizeof(record)}, journal) : result;
}

AcceptanceRecordResult plugin_journal_save(ConstSlice<wchar_t> path, const PluginJournal& journal) noexcept {
    u8 record[kPluginJournalRecordBytes];
    const auto result = plugin_journal_encode(journal, {record, sizeof(record)});
    return result.ok() ? plugin_record_write(path, {record, sizeof(record)}) : result;
}

PluginRecoveryAction plugin_manager_recovery_plan(const PluginJournal& journal, const u8* manifest) noexcept {
    switch (journal.operation) {
    case PluginJournalOperation::none: return PluginRecoveryAction::none;
    case PluginJournalOperation::uninstall: return manifest ? PluginRecoveryAction::restart_uninstall : PluginRecoveryAction::finish_uninstall;
    default: break;
    }
    if (journal.phase == PluginJournalPhase::intent) return PluginRecoveryAction::discard;
    if (manifest && bytes_equal(manifest, journal.to_sha256, 32)) return PluginRecoveryAction::finish;
    if (journal.phase == PluginJournalPhase::staged) {
        const bool fresh = bytes_zero(journal.from_sha256, 32);
        if (fresh ? !manifest : manifest && bytes_equal(manifest, journal.from_sha256, 32)) return PluginRecoveryAction::discard;
    }
    return PluginRecoveryAction::clear;
}

} // namespace maicord
