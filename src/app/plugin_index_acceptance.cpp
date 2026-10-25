#include "maicord/app/plugin_index_acceptance.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using RE = AcceptanceRecordError;
using Result = PluginIndexAcceptanceResult;
constexpr u8 kMagic[8]{'M','A','I','P','I','A','C','C'};
Result fail(RE error, DWORD native = 0, PluginIndexError index = PluginIndexError::none) noexcept {
    return {error, native, index, false};
}
Result convert(AcceptanceRecordResult result) noexcept { return {result.error, result.native_error, PluginIndexError::none, false}; }
bool object_and_path(const void* object, u32 bytes, ConstSlice<wchar_t> path) noexcept {
    const ConstSlice<u8> ranges[]{{static_cast<const u8*>(object), bytes},
        {reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}};
    return object && byte_ranges_all_disjoint({ranges, 2});
}
bool equal(const PluginIndexAcceptanceState& a, const PluginIndexAcceptanceState& b) noexcept {
    for (u32 i = 0; i < kPluginIndexAcceptanceSlots; ++i)
        if (a.slots[i].present != b.slots[i].present || a.slots[i].provider != b.slots[i].provider ||
            a.slots[i].sequence != b.slots[i].sequence || !bytes_equal(a.slots[i].index_sha256, b.slots[i].index_sha256, 32))
            return false;
    return true;
}
Result load(const wchar_t* path, PluginIndexAcceptanceState* state) noexcept {
    u8 record[kPluginIndexAcceptanceRecordBytes]{};
    const auto result = convert(acceptance_record_read(path, {record, sizeof(record)}));
    return result.ok() ? plugin_index_acceptance_decode({record, sizeof(record)}, state) : result;
}
Result store(const PluginIndexAcceptanceTransaction& t, const PluginIndexAcceptanceState& state) noexcept {
    u8 record[kPluginIndexAcceptanceRecordBytes]{};
    const auto result = plugin_index_acceptance_encode(state, {record, sizeof(record)});
    if (!result.ok()) return result;
    auto written = convert(acceptance_record_write(t.path, t.path_characters, {record, sizeof(record)}));
    written.written = written.ok();
    return written;
}
// The disk still holds the transaction's snapshot (the update_acceptance rule).
Result current(const PluginIndexAcceptanceTransaction& t) noexcept {
    if (!t.lock || !t.path_characters || !acceptance_record_path_valid({t.path, t.path_characters}) ||
        !plugin_index_acceptance_valid(t.state)) return fail(RE::invalid_input);
    auto result = convert(acceptance_record_disk_file(t.lock));
    if (!result.ok()) return fail(RE::lock, result.native_error);
    wchar_t checked[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_terminate({t.path, t.path_characters}, checked);
    result = convert(acceptance_record_ancestors(checked, t.path_characters));
    if (!result.ok()) return result;
    PluginIndexAcceptanceState state{};
    result = load(t.path, &state);
    if (result.error == RE::missing && !t.record_present) return {};
    if (!result.ok()) return result;
    if (!t.record_present || !equal(state, t.state)) return fail(RE::state);
    return {};
}
u32 slot_of(const PluginIndexAcceptanceState& state, u64 provider) noexcept {
    if (provider == kPluginProviderOfficial) return 0;
    for (u32 i = 1; i < kPluginIndexAcceptanceSlots; ++i)
        if (state.slots[i].present && state.slots[i].provider == provider) return i;
    return kPluginIndexAcceptanceSlots;
}
} // namespace

bool plugin_index_acceptance_valid(const PluginIndexAcceptanceState& state) noexcept {
    for (u32 i = 0; i < kPluginIndexAcceptanceSlots; ++i) {
        const PluginIndexFloor& slot = state.slots[i];
        if (!slot.present) {
            if (slot.provider || slot.sequence || !bytes_zero(slot.index_sha256, 32)) return false;
            continue;
        }
        if (!slot.sequence || (i == 0) != (slot.provider == kPluginProviderOfficial) || slot.provider == kPluginProviderFile)
            return false;
        for (u32 j = 1; j < i; ++j) if (state.slots[j].present && state.slots[j].provider == slot.provider) return false;
    }
    return true;
}

PluginIndexAcceptanceResult plugin_index_acceptance_encode(const PluginIndexAcceptanceState& state, Slice<u8> record) noexcept {
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(&state), sizeof(state)}, {record.data, record.count}};
    if (record.count != kPluginIndexAcceptanceRecordBytes || !record.data || !plugin_index_acceptance_valid(state) ||
        !byte_ranges_all_disjoint({ranges, 2})) return fail(RE::invalid_input);
    u8 candidate[kPluginIndexAcceptanceRecordBytes]{};
    copy_bytes(candidate, kMagic, 8);
    byte_store_le32(candidate + 8, kPluginIndexAcceptanceVersion);
    byte_store_le32(candidate + 12, kPluginIndexAcceptanceRecordBytes);
    byte_store_le32(candidate + 16, kPluginIndexAcceptanceSlots);
    for (u32 i = 0; i < kPluginIndexAcceptanceSlots; ++i) {
        u8* slot = candidate + 32 + i * kPluginIndexAcceptanceSlotBytes;
        byte_store_le64(slot, state.slots[i].provider);
        byte_store_le64(slot + 8, state.slots[i].sequence);
        copy_bytes(slot + 16, state.slots[i].index_sha256, 32);
        slot[48] = state.slots[i].present ? 1 : 0;
    }
    byte_store_le64(candidate + 24, plugin_record_checksum({candidate, sizeof(candidate)}));
    copy_bytes(record.data, candidate, sizeof(candidate));
    return {};
}

PluginIndexAcceptanceResult plugin_index_acceptance_decode(ConstSlice<u8> record, PluginIndexAcceptanceState* state) noexcept {
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(state), sizeof(*state)}, record};
    if (!state || !record.data || !byte_ranges_all_disjoint({ranges, 2})) return fail(RE::invalid_input);
    if (record.count != kPluginIndexAcceptanceRecordBytes) return fail(RE::size);
    if (!bytes_equal(record.data, kMagic, 8) || byte_load_le32(record.data + 12) != kPluginIndexAcceptanceRecordBytes ||
        byte_load_le32(record.data + 16) != kPluginIndexAcceptanceSlots || byte_load_le32(record.data + 20)) return fail(RE::format);
    if (byte_load_le32(record.data + 8) != kPluginIndexAcceptanceVersion) return fail(RE::version);
    if (byte_load_le64(record.data + 24) != plugin_record_checksum(record)) return fail(RE::checksum);
    PluginIndexAcceptanceState candidate{};
    for (u32 i = 0; i < kPluginIndexAcceptanceSlots; ++i) {
        const u8* slot = record.data + 32 + i * kPluginIndexAcceptanceSlotBytes;
        if (slot[48] > 1 || !bytes_zero(slot + 49, 7)) return fail(RE::format);
        candidate.slots[i].provider = byte_load_le64(slot);
        candidate.slots[i].sequence = byte_load_le64(slot + 8);
        copy_bytes(candidate.slots[i].index_sha256, slot + 16, 32);
        candidate.slots[i].present = slot[48] == 1;
    }
    if (!plugin_index_acceptance_valid(candidate)) return fail(RE::format);
    copy_value(state, candidate);
    return {};
}

PluginIndexAcceptanceResult plugin_index_acceptance_load(ConstSlice<wchar_t> path, PluginIndexAcceptanceState* state) noexcept {
    if (!state || !acceptance_record_path_valid(path) || !object_and_path(state, sizeof(*state), path)) return fail(RE::invalid_input);
    wchar_t terminated[kAcceptanceRecordMaximumPathCharacters + 1];
    acceptance_record_terminate(path, terminated);
    const auto result = convert(acceptance_record_ancestors(terminated, path.count));
    return result.ok() ? load(terminated, state) : result;
}

const PluginIndexFloor* plugin_index_floor(const PluginIndexAcceptanceState& state, u64 provider) noexcept {
    const u32 slot = slot_of(state, provider);
    return slot < kPluginIndexAcceptanceSlots && state.slots[slot].present ? &state.slots[slot] : nullptr;
}

PluginIndexFloorVerdict plugin_index_floor_check(const PluginIndexAcceptanceState& state, u64 provider, u64 sequence,
    const u8* index_sha256) noexcept {
    const PluginIndexFloor* floor = plugin_index_floor(state, provider);
    if (!index_sha256 || !sequence) return PluginIndexFloorVerdict::replay;
    if (!floor || sequence > floor->sequence) return PluginIndexFloorVerdict::advance;
    if (sequence == floor->sequence && bytes_equal(index_sha256, floor->index_sha256, 32)) return PluginIndexFloorVerdict::same;
    return PluginIndexFloorVerdict::replay;
}

PluginIndexAcceptanceResult plugin_index_acceptance_begin(ConstSlice<wchar_t> path, PluginIndexAcceptanceTransaction* transaction,
    bool allow_first_use) noexcept {
    if (!transaction || !acceptance_record_path_valid(path) || !object_and_path(transaction, sizeof(*transaction), path) ||
        transaction->lock) return fail(RE::invalid_input);
    PluginIndexAcceptanceTransaction candidate{};
    acceptance_record_terminate(path, candidate.path);
    candidate.path_characters = path.count;
    auto result = convert(acceptance_record_ancestors(candidate.path, path.count));
    if (!result.ok()) return result;
    result = convert(acceptance_record_lock(candidate.path, path.count, &candidate.lock));
    if (!result.ok()) return result;
    result = load(candidate.path, &candidate.state);
    candidate.record_present = result.ok();
    if (result.error == RE::missing && allow_first_use) reset_value(&result);
    if (!result.ok()) { CloseHandle(candidate.lock); return result; }
    *transaction = candidate;
    return {};
}

PluginIndexAcceptanceResult plugin_index_acceptance_verify_commit(PluginIndexAcceptanceTransaction* transaction, u64 provider,
    Utf8View document, ConstSlice<u8> signature, const P256Key* key, KdfSha256Workspace& workspace,
    const PluginIndexPolicy& policy, Slice<u32> scratch, PluginIndex* output) noexcept {
    const bool official = policy.kind == PluginProviderKind::official;
    if (!transaction || !output || provider == kPluginProviderFile || official != (provider == kPluginProviderOfficial) ||
        (key == nullptr) != (policy.kind == PluginProviderKind::custom_unsigned))
        return fail(RE::invalid_input);
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(transaction), sizeof(*transaction)},
        {reinterpret_cast<const u8*>(output), static_cast<u32>(sizeof(*output))}};
    if (!byte_ranges_all_disjoint({ranges, 2})) return fail(RE::invalid_input);
    auto result = current(*transaction);
    if (!result.ok()) return result;
    const auto verified = key ? plugin_index_verify(document, signature, *key, workspace, policy, scratch, output)
                              : plugin_index_parse_unsigned(document, workspace, policy, scratch, output);
    if (!verified.ok()) return fail(verified.error == PluginIndexError::invalid_input ? RE::invalid_input : RE::document, 0, verified.error);
    const auto verdict = plugin_index_floor_check(transaction->state, provider, output->sequence, output->digest);
    if (verdict != PluginIndexFloorVerdict::advance) {
        if (verdict == PluginIndexFloorVerdict::same) return {};
        clear_bytes(output, sizeof(*output));
        return fail(RE::replay);
    }
    u32 slot = slot_of(transaction->state, provider);
    for (u32 i = 1; slot == kPluginIndexAcceptanceSlots && i < kPluginIndexAcceptanceSlots; ++i)
        if (!transaction->state.slots[i].present) slot = i;
    if (slot == kPluginIndexAcceptanceSlots) { clear_bytes(output, sizeof(*output)); return fail(RE::capacity); }
    // Check the disk snapshot again immediately before mutation.
    result = current(*transaction);
    if (!result.ok()) { clear_bytes(output, sizeof(*output)); return result; }
    PluginIndexAcceptanceState next;
    copy_value(&next, transaction->state);
    next.slots[slot] = {provider, output->sequence, {}, true};
    copy_bytes(next.slots[slot].index_sha256, output->digest, 32);
    result = store(*transaction, next);
    if (!result.ok()) { clear_bytes(output, sizeof(*output)); return result; }
    copy_value(&transaction->state, next);
    transaction->record_present = true;
    return result;
}

PluginIndexAcceptanceResult plugin_index_acceptance_forget(PluginIndexAcceptanceTransaction* transaction, u64 provider) noexcept {
    if (!transaction || provider == kPluginProviderFile) return fail(RE::invalid_input);
    auto result = current(*transaction);
    if (!result.ok()) return result;
    const u32 slot = slot_of(transaction->state, provider);
    if (slot == kPluginIndexAcceptanceSlots || !transaction->state.slots[slot].present) return {};
    PluginIndexAcceptanceState next;
    copy_value(&next, transaction->state);
    reset_value(&next.slots[slot]);
    result = store(*transaction, next);
    if (!result.ok()) return result;
    copy_value(&transaction->state, next);
    transaction->record_present = true;
    return result;
}

PluginIndexAcceptanceResult plugin_index_acceptance_release(PluginIndexAcceptanceTransaction* transaction) noexcept {
    if (!transaction) return fail(RE::invalid_input);
    if (transaction->lock && !CloseHandle(transaction->lock)) return fail(RE::release, GetLastError());
    secure_clear_bytes(transaction, sizeof(*transaction));
    return {};
}

} // namespace maicord
