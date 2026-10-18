#include "maicord/app/update_acceptance.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/app/acceptance_record.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u8 kMagic[8]{'M','A','I','U','A','C','C','1'};
constexpr u32 kChecksumOffset = 120;
UpdateAcceptanceResult fail(UpdateAcceptanceError error, DWORD native = 0,
    UpdateManifestError manifest = UpdateManifestError::none) noexcept { return {error, native, manifest}; }
bool valid_state(const UpdateAcceptanceState& state) noexcept {
    for (const auto& channel : state.channels)
        if (channel.present ? !channel.highest_version :
            (channel.highest_version || !bytes_zero(channel.manifest_sha256, 32))) return false;
    return true;
}
bool equal(const UpdateAcceptanceState& a, const UpdateAcceptanceState& b) noexcept {
    for (u32 i = 0; i < 2; ++i)
        if (a.channels[i].present != b.channels[i].present ||
            a.channels[i].highest_version != b.channels[i].highest_version ||
            !bytes_equal(a.channels[i].manifest_sha256, b.channels[i].manifest_sha256, 32)) return false;
    return true;
}
bool object_and_buffer(const void* object, u32 bytes, ConstSlice<u8> buffer) noexcept {
    const ConstSlice<u8> ranges[]{{static_cast<const u8*>(object), bytes}, buffer};
    return object && byte_ranges_all_disjoint({ranges, 2});
}
// The shared record errors use the updater's ordinals up to release.
UpdateAcceptanceResult convert(AcceptanceRecordResult result) noexcept {
    return {static_cast<UpdateAcceptanceError>(result.error), result.native_error};
}
constexpr bool same(AcceptanceRecordError a, UpdateAcceptanceError b) noexcept { return static_cast<u32>(a) == static_cast<u32>(b); }
static_assert(same(AcceptanceRecordError::path, UpdateAcceptanceError::path) && same(AcceptanceRecordError::lock, UpdateAcceptanceError::lock) &&
    same(AcceptanceRecordError::missing, UpdateAcceptanceError::missing) && same(AcceptanceRecordError::open, UpdateAcceptanceError::open) &&
    same(AcceptanceRecordError::file, UpdateAcceptanceError::file) && same(AcceptanceRecordError::size, UpdateAcceptanceError::size) &&
    same(AcceptanceRecordError::read, UpdateAcceptanceError::read) && same(AcceptanceRecordError::write, UpdateAcceptanceError::write) &&
    same(AcceptanceRecordError::flush, UpdateAcceptanceError::flush) && same(AcceptanceRecordError::commit, UpdateAcceptanceError::commit) &&
    same(AcceptanceRecordError::release, UpdateAcceptanceError::release));
static_assert(kUpdateAcceptanceMaximumPathCharacters == kAcceptanceRecordMaximumPathCharacters);
UpdateAcceptanceResult load(const wchar_t* path, UpdateAcceptanceState* state) noexcept {
    u8 record[kUpdateAcceptanceRecordBytes]{};
    const auto result = convert(acceptance_record_read(path, {record, sizeof(record)}));
    return result.ok() ? update_acceptance_decode({record, sizeof(record)}, state) : result;
}
UpdateAcceptanceResult current(const UpdateAcceptanceTransaction& transaction) noexcept {
    if (!transaction.lock || !transaction.path_characters ||
        !acceptance_record_path_valid({transaction.path, transaction.path_characters}) || !valid_state(transaction.state))
        return fail(UpdateAcceptanceError::invalid_input);
    auto result = convert(acceptance_record_disk_file(transaction.lock));
    if (!result.ok()) return fail(UpdateAcceptanceError::lock, result.native_error);
    wchar_t checked[kUpdateAcceptanceMaximumPathCharacters + 1];
    acceptance_record_terminate({transaction.path, transaction.path_characters}, checked);
    result = convert(acceptance_record_ancestors(checked, transaction.path_characters));
    if (!result.ok()) return result;
    UpdateAcceptanceState state{};
    result = load(transaction.path, &state);
    if (result.error == UpdateAcceptanceError::missing && !transaction.record_present) return {};
    if (!result.ok()) return result;
    if (!transaction.record_present || !equal(state, transaction.state)) return fail(UpdateAcceptanceError::state);
    return {};
}
UpdateAcceptanceResult store(const wchar_t* path, u32 characters, const UpdateAcceptanceState& state) noexcept {
    u8 record[kUpdateAcceptanceRecordBytes]{};
    const auto result = update_acceptance_encode(state, {record, sizeof(record)});
    return result.ok() ? convert(acceptance_record_write(path, characters, {record, sizeof(record)})) : result;
}
} // namespace

UpdateAcceptanceResult update_acceptance_encode(const UpdateAcceptanceState& state, Slice<u8> record) noexcept {
    if (record.count != kUpdateAcceptanceRecordBytes || !record.data || !valid_state(state) ||
        !object_and_buffer(&state, sizeof(state), {record.data, record.count})) return fail(UpdateAcceptanceError::invalid_input);
    u8 candidate[kUpdateAcceptanceRecordBytes]{};
    copy_bytes(candidate, kMagic, sizeof(kMagic));
    byte_store_le32(candidate + 8, 1);
    byte_store_le32(candidate + 12, kUpdateAcceptanceRecordBytes);
    for (u32 i = 0; i < 2; ++i) {
        const u32 offset = 16 + i * 48;
        byte_store_le64(candidate + offset, state.channels[i].highest_version);
        copy_bytes(candidate + offset + 8, state.channels[i].manifest_sha256, 32);
        candidate[offset + 40] = state.channels[i].present ? 1 : 0;
    }
    byte_store_le64(candidate + kChecksumOffset, hash64_bytes(kHash64Seed, candidate, kChecksumOffset));
    copy_bytes(record.data, candidate, sizeof(candidate));
    return {};
}
UpdateAcceptanceResult update_acceptance_decode(ConstSlice<u8> record, UpdateAcceptanceState* state) noexcept {
    if (!state || !record.data || !object_and_buffer(state, sizeof(*state), record)) return fail(UpdateAcceptanceError::invalid_input);
    if (record.count != kUpdateAcceptanceRecordBytes) return fail(UpdateAcceptanceError::size);
    if (!bytes_equal(record.data, kMagic, sizeof(kMagic)) || byte_load_le32(record.data + 8) != 1 ||
        byte_load_le32(record.data + 12) != kUpdateAcceptanceRecordBytes || !bytes_zero(record.data + 112, 8))
        return fail(UpdateAcceptanceError::format);
    if (byte_load_le64(record.data + kChecksumOffset) != hash64_bytes(kHash64Seed, record.data, kChecksumOffset))
        return fail(UpdateAcceptanceError::checksum);
    UpdateAcceptanceState candidate{};
    for (u32 i = 0; i < 2; ++i) {
        const u32 offset = 16 + i * 48;
        if (record[offset + 40] > 1 || !bytes_zero(record.data + offset + 41, 7)) return fail(UpdateAcceptanceError::format);
        candidate.channels[i].highest_version = byte_load_le64(record.data + offset);
        copy_bytes(candidate.channels[i].manifest_sha256, record.data + offset + 8, 32);
        candidate.channels[i].present = record[offset + 40] == 1;
    }
    if (!valid_state(candidate)) return fail(UpdateAcceptanceError::format);
    *state = candidate;
    return {};
}
UpdateAcceptanceResult update_acceptance_load(ConstSlice<wchar_t> path, UpdateAcceptanceState* state) noexcept {
    if (!state || !acceptance_record_path_valid(path) || !object_and_buffer(state, sizeof(*state),
        {reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}))
        return fail(UpdateAcceptanceError::invalid_input);
    wchar_t terminated[kUpdateAcceptanceMaximumPathCharacters + 1];
    acceptance_record_terminate(path, terminated);
    const auto result = convert(acceptance_record_ancestors(terminated, path.count));
    return result.ok() ? load(terminated, state) : result;
}
UpdateAcceptanceResult update_acceptance_begin(ConstSlice<wchar_t> path,
    UpdateAcceptanceTransaction* transaction, bool allow_first_use) noexcept {
    if (!transaction || !acceptance_record_path_valid(path) || !object_and_buffer(transaction, sizeof(*transaction),
        {reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}) || transaction->lock)
        return fail(UpdateAcceptanceError::invalid_input);
    UpdateAcceptanceTransaction candidate{};
    acceptance_record_terminate(path, candidate.path);
    candidate.path_characters = path.count;
    auto result = convert(acceptance_record_ancestors(candidate.path, path.count));
    if (!result.ok()) return result;
    result = convert(acceptance_record_lock(candidate.path, path.count, &candidate.lock));
    if (!result.ok()) return result;
    result = load(candidate.path, &candidate.state);
    candidate.record_present = result.ok();
    if (result.error == UpdateAcceptanceError::missing && allow_first_use) result = {};
    if (!result.ok()) { CloseHandle(candidate.lock); return result; }
    *transaction = candidate;
    return {};
}
UpdateAcceptanceResult update_acceptance_capture_policy(const UpdateAcceptanceTransaction& transaction,
    const UpdateManifestPolicy& trusted, UpdateManifestPolicy* captured) noexcept {
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(&transaction), sizeof(transaction)},
        {reinterpret_cast<const u8*>(&trusted), sizeof(trusted)},
        {reinterpret_cast<const u8*>(captured), sizeof(*captured)}};
    if (!captured || trusted.channel > UpdateChannel::preview || !byte_ranges_all_disjoint({ranges, 3}))
        return fail(UpdateAcceptanceError::invalid_input);
    const auto result = current(transaction);
    if (!result.ok()) return result;
    const auto& accepted = transaction.state.channels[static_cast<u32>(trusted.channel)];
    if (trusted.highest_accepted_version > accepted.highest_version ||
        (trusted.highest_accepted_version == accepted.highest_version && trusted.has_accepted_manifest_digest &&
         (!accepted.present || !bytes_equal(trusted.accepted_manifest_sha256, accepted.manifest_sha256, 32))))
        return fail(UpdateAcceptanceError::state);
    auto candidate = trusted;
    candidate.highest_accepted_version = accepted.highest_version;
    candidate.has_accepted_manifest_digest = accepted.present;
    copy_bytes(candidate.accepted_manifest_sha256, accepted.manifest_sha256, 32);
    *captured = candidate;
    return {};
}
UpdateAcceptanceResult update_acceptance_verify_commit(UpdateAcceptanceTransaction* transaction,
    Utf8View document, ConstSlice<u8> signature, const P256Key& key, KdfSha256Workspace& workspace,
    UpdateManifestPolicy* policy, Slice<u32> scratch, UpdateManifest* approved) noexcept {
    if (!transaction || !policy || !approved || scratch.count > ~u32{0} / sizeof(u32))
        return fail(UpdateAcceptanceError::invalid_input);
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(transaction), sizeof(*transaction)},
        {reinterpret_cast<const u8*>(document.data), document.bytes}, signature,
        {reinterpret_cast<const u8*>(&key), sizeof(key)},
        {reinterpret_cast<const u8*>(&workspace), sizeof(workspace)}, {workspace.storage.data, workspace.storage.capacity},
        {reinterpret_cast<const u8*>(policy), sizeof(*policy)},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))},
        {reinterpret_cast<const u8*>(approved), sizeof(*approved)}};
    if (!byte_ranges_all_disjoint({ranges, sizeof(ranges) / sizeof(ranges[0])})) return fail(UpdateAcceptanceError::invalid_input);
    UpdateManifestPolicy captured{};
    auto result = update_acceptance_capture_policy(*transaction, *policy, &captured);
    if (!result.ok()) return result;
    UpdateManifest candidate{};
    const auto verified = update_manifest_verify(document, signature, key, workspace, captured, scratch, &candidate);
    if (!verified.ok()) return fail(UpdateAcceptanceError::manifest, 0, verified.error);
    // Check the disk snapshot again immediately before mutation. The stable lock
    // prevents cooperating writers; a protected parent is still required.
    result = current(*transaction);
    if (!result.ok()) return result;
    auto next = transaction->state;
    auto& accepted = next.channels[static_cast<u32>(candidate.channel)];
    const bool retry = accepted.present && accepted.highest_version == candidate.version &&
        bytes_equal(accepted.manifest_sha256, candidate.manifest_sha256, 32);
    accepted.highest_version = candidate.version;
    accepted.present = true;
    copy_bytes(accepted.manifest_sha256, candidate.manifest_sha256, 32);
    if (!retry) {
        result = store(transaction->path, transaction->path_characters, next);
        if (!result.ok()) return result;
    }
    transaction->state = next;
    transaction->record_present = true;
    policy->highest_accepted_version = candidate.version;
    policy->has_accepted_manifest_digest = true;
    copy_bytes(policy->accepted_manifest_sha256, candidate.manifest_sha256, 32);
    *approved = candidate;
    return {};
}
UpdateAcceptanceResult update_acceptance_release(UpdateAcceptanceTransaction* transaction) noexcept {
    if (!transaction) return fail(UpdateAcceptanceError::invalid_input);
    if (transaction->lock && !CloseHandle(transaction->lock)) return fail(UpdateAcceptanceError::release, GetLastError());
    secure_clear_bytes(transaction, sizeof(*transaction));
    return {};
}
} // namespace maicord
