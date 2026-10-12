#include "maicord/app/update_acceptance.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/hash.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u8 kMagic[8]{'M','A','I','U','A','C','C','1'};
constexpr u32 kChecksumOffset = 120;
UpdateAcceptanceResult fail(UpdateAcceptanceError error, DWORD native = 0,
    UpdateManifestError manifest = UpdateManifestError::none) noexcept { return {error, native, manifest}; }
bool zero(const u8* data, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i) if (data[i]) return false;
    return true;
}
bool valid_state(const UpdateAcceptanceState& state) noexcept {
    for (const auto& channel : state.channels)
        if (channel.present ? !channel.highest_version :
            (channel.highest_version || !zero(channel.manifest_sha256, 32))) return false;
    return true;
}
bool equal(const UpdateAcceptanceState& a, const UpdateAcceptanceState& b) noexcept {
    for (u32 i = 0; i < 2; ++i)
        if (a.channels[i].present != b.channels[i].present ||
            a.channels[i].highest_version != b.channels[i].highest_version ||
            !bytes_equal(a.channels[i].manifest_sha256, b.channels[i].manifest_sha256, 32)) return false;
    return true;
}
bool disjoint(ConstSlice<ConstSlice<u8>> ranges) noexcept {
    for (u32 i = 0; i < ranges.count; ++i) {
        if (!byte_range_valid(ranges[i])) return false;
        for (u32 j = 0; j < i; ++j) if (byte_ranges_overlap(ranges[i], ranges[j])) return false;
    }
    return true;
}
bool object_and_buffer(const void* object, u32 bytes, ConstSlice<u8> buffer) noexcept {
    const ConstSlice<u8> ranges[]{{static_cast<const u8*>(object), bytes}, buffer};
    return object && disjoint({ranges, 2});
}
wchar_t lower(wchar_t value) noexcept {
    return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + L'a' - L'A') : value;
}
bool reserved_component(const wchar_t* name, u32 count) noexcept {
    u32 stem = 0;
    while (stem < count && name[stem] != L'.') ++stem;
    if (stem == 3) {
        const wchar_t a = lower(name[0]), b = lower(name[1]), c = lower(name[2]);
        return (a == L'c' && b == L'o' && c == L'n') || (a == L'p' && b == L'r' && c == L'n') ||
            (a == L'a' && b == L'u' && c == L'x') || (a == L'n' && b == L'u' && c == L'l');
    }
    if (stem == 4 && name[3] >= L'0' && name[3] <= L'9')
        return (lower(name[0]) == L'c' && lower(name[1]) == L'o' && lower(name[2]) == L'm') ||
            (lower(name[0]) == L'l' && lower(name[1]) == L'p' && lower(name[2]) == L't');
    return false;
}
bool valid_path(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || path.count < 4 || path.count > kUpdateAcceptanceMaximumPathCharacters - 5 ||
        !byte_range_valid({reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}) ||
        lower(path[0]) < L'a' || lower(path[0]) > L'z' || path[1] != L':' || path[2] != L'\\') return false;
    u32 component = 3;
    for (u32 i = 3; i <= path.count; ++i) {
        if (i == path.count || path[i] == L'\\') {
            if (i == component || path[i - 1] == L'.' || path[i - 1] == L' ' ||
                reserved_component(path.data + component, i - component)) return false;
            component = i + 1;
        } else if (path[i] < 32 || path[i] == L'/' || path[i] == L':' || path[i] == L'*' ||
                   path[i] == L'?' || path[i] == L'"' || path[i] == L'<' || path[i] == L'>' || path[i] == L'|') return false;
    }
    return true;
}
void terminate(ConstSlice<wchar_t> path, wchar_t* output) noexcept {
    for (u32 i = 0; i < path.count; ++i) output[i] = path[i];
    output[path.count] = 0;
}
void suffix(const wchar_t* path, u32 count, const wchar_t* ending, wchar_t* output) noexcept {
    for (u32 i = 0; i < count; ++i) output[i] = path[i];
    u32 i = 0;
    do { output[count + i] = ending[i]; } while (ending[i++]);
}
UpdateAcceptanceResult ancestors(wchar_t* path, u32 count) noexcept {
    wchar_t root[4]{path[0], L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_FIXED) return fail(UpdateAcceptanceError::path, ERROR_NOT_SUPPORTED);
    for (u32 i = 3; i < count; ++i) if (path[i] == L'\\') {
        path[i] = 0;
        const DWORD attributes = GetFileAttributesW(path);
        const DWORD native = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        path[i] = L'\\';
        if (attributes == INVALID_FILE_ATTRIBUTES) return fail(UpdateAcceptanceError::path, native);
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return fail(UpdateAcceptanceError::path, ERROR_BAD_PATHNAME);
    }
    return {};
}
UpdateAcceptanceResult disk_file(HANDLE file) noexcept {
    if (GetFileType(file) != FILE_TYPE_DISK) return fail(UpdateAcceptanceError::file);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) return fail(UpdateAcceptanceError::file, GetLastError());
    if (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return fail(UpdateAcceptanceError::file, ERROR_BAD_PATHNAME);
    return {};
}
UpdateAcceptanceResult load(const wchar_t* path, UpdateAcceptanceState* state) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD native = GetLastError();
        return fail(native == ERROR_FILE_NOT_FOUND ? UpdateAcceptanceError::missing : UpdateAcceptanceError::open, native);
    }
    auto result = disk_file(file);
    LARGE_INTEGER size{};
    if (result.ok() && !GetFileSizeEx(file, &size)) result = fail(UpdateAcceptanceError::size, GetLastError());
    if (result.ok() && size.QuadPart != kUpdateAcceptanceRecordBytes) result = fail(UpdateAcceptanceError::size);
    u8 record[kUpdateAcceptanceRecordBytes]{};
    u32 native = 0;
    if (result.ok() && !file_persistence_read_complete(file, {record, sizeof(record)}, &native))
        result = fail(UpdateAcceptanceError::read, native);
    if (!CloseHandle(file) && result.ok()) result = fail(UpdateAcceptanceError::release, GetLastError());
    return result.ok() ? update_acceptance_decode({record, sizeof(record)}, state) : result;
}
UpdateAcceptanceResult current(const UpdateAcceptanceTransaction& transaction) noexcept {
    if (!transaction.lock || !transaction.path_characters ||
        !valid_path({transaction.path, transaction.path_characters}) || !valid_state(transaction.state))
        return fail(UpdateAcceptanceError::invalid_input);
    auto result = disk_file(transaction.lock);
    if (!result.ok()) return fail(UpdateAcceptanceError::lock, result.native_error);
    wchar_t checked[kUpdateAcceptanceMaximumPathCharacters + 1];
    terminate({transaction.path, transaction.path_characters}, checked);
    result = ancestors(checked, transaction.path_characters);
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
    auto result = update_acceptance_encode(state, {record, sizeof(record)});
    if (!result.ok()) return result;
    wchar_t temporary[kUpdateAcceptanceMaximumPathCharacters + 1];
    suffix(path, characters, L".new", temporary);
    // CREATE_NEW never truncates or follows an attacker-controlled/stale sibling.
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail(UpdateAcceptanceError::open, GetLastError());
    result = disk_file(file);
    DWORD written = 0;
    if (result.ok()) {
        if (!WriteFile(file, record, sizeof(record), &written, nullptr)) result = fail(UpdateAcceptanceError::write, GetLastError());
        else if (written != sizeof(record)) result = fail(UpdateAcceptanceError::write, ERROR_WRITE_FAULT);
    }
    if (result.ok() && !FlushFileBuffers(file)) result = fail(UpdateAcceptanceError::flush, GetLastError());
    if (!CloseHandle(file) && result.ok()) result = fail(UpdateAcceptanceError::release, GetLastError());
    if (result.ok() && !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        result = fail(UpdateAcceptanceError::commit, GetLastError());
    if (!result.ok()) DeleteFileW(temporary); // only our successfully CREATE_NEW-owned sibling
    return result;
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
        byte_load_le32(record.data + 12) != kUpdateAcceptanceRecordBytes || !zero(record.data + 112, 8))
        return fail(UpdateAcceptanceError::format);
    if (byte_load_le64(record.data + kChecksumOffset) != hash64_bytes(kHash64Seed, record.data, kChecksumOffset))
        return fail(UpdateAcceptanceError::checksum);
    UpdateAcceptanceState candidate{};
    for (u32 i = 0; i < 2; ++i) {
        const u32 offset = 16 + i * 48;
        if (record[offset + 40] > 1 || !zero(record.data + offset + 41, 7)) return fail(UpdateAcceptanceError::format);
        candidate.channels[i].highest_version = byte_load_le64(record.data + offset);
        copy_bytes(candidate.channels[i].manifest_sha256, record.data + offset + 8, 32);
        candidate.channels[i].present = record[offset + 40] == 1;
    }
    if (!valid_state(candidate)) return fail(UpdateAcceptanceError::format);
    *state = candidate;
    return {};
}
UpdateAcceptanceResult update_acceptance_load(ConstSlice<wchar_t> path, UpdateAcceptanceState* state) noexcept {
    if (!state || !valid_path(path) || !object_and_buffer(state, sizeof(*state),
        {reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}))
        return fail(UpdateAcceptanceError::invalid_input);
    wchar_t terminated[kUpdateAcceptanceMaximumPathCharacters + 1];
    terminate(path, terminated);
    const auto result = ancestors(terminated, path.count);
    return result.ok() ? load(terminated, state) : result;
}
UpdateAcceptanceResult update_acceptance_begin(ConstSlice<wchar_t> path,
    UpdateAcceptanceTransaction* transaction, bool allow_first_use) noexcept {
    if (!transaction || !valid_path(path) || !object_and_buffer(transaction, sizeof(*transaction),
        {reinterpret_cast<const u8*>(path.data), path.count * static_cast<u32>(sizeof(wchar_t))}) || transaction->lock)
        return fail(UpdateAcceptanceError::invalid_input);
    UpdateAcceptanceTransaction candidate{};
    terminate(path, candidate.path);
    candidate.path_characters = path.count;
    auto result = ancestors(candidate.path, path.count);
    if (!result.ok()) return result;
    wchar_t locked[kUpdateAcceptanceMaximumPathCharacters + 1];
    suffix(candidate.path, path.count, L".lock", locked);
    candidate.lock = CreateFileW(locked, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (candidate.lock == INVALID_HANDLE_VALUE) return fail(UpdateAcceptanceError::lock, GetLastError());
    result = disk_file(candidate.lock);
    if (result.ok()) {
        result = load(candidate.path, &candidate.state);
        candidate.record_present = result.ok();
        if (result.error == UpdateAcceptanceError::missing && allow_first_use) result = {};
    }
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
    if (!captured || trusted.channel > UpdateChannel::preview || !disjoint({ranges, 3}))
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
    if (!disjoint({ranges, sizeof(ranges) / sizeof(ranges[0])})) return fail(UpdateAcceptanceError::invalid_input);
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
    SecureZeroMemory(transaction, sizeof(*transaction));
    return {};
}
} // namespace maicord
