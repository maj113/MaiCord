#include "maicord/app/update_generation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/acceptance_record.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/hash.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u8 magic[8]{'M','A','I','G','E','N','0','3'};
// Record: 64-byte header, three 408-byte references, the disposal intent at
// 1288, zero padding and the checksum in the last 8 bytes.
constexpr u32 reference_bytes = 408, member_bytes = 80, disposal_offset = 64 + reference_bytes * 3,
    disposal_bytes = 24 + kUpdateGenerationMaximumFiles * 16, checksum_offset = kUpdateGenerationRecordBytes - 8;
static_assert(disposal_offset + disposal_bytes <= checksum_offset);
static_assert(88 + kUpdatePackageMaximumMembers * member_bytes == reference_bytes);
constexpr Utf8View provenance[]{utf8_literal("offer.json"), utf8_literal("offer.sig"), utf8_literal("package.zip")};
UpdateGenerationResult fail(UpdateGenerationError e, DWORD native = 0) noexcept { return {e, native}; }
bool identity(const UpdateGenerationIdentity& a, const UpdateGenerationIdentity& b) noexcept { return bytes_equal(a.bytes, b.bytes, 16); }
bool empty_package(const UpdatePackageDescriptor& p) noexcept {
    if (p.version || p.package_bytes || p.member_count || p.channel != UpdateChannel::stable ||
        !bytes_zero(p.document_sha256, 32) || !bytes_zero(p.package_sha256, 32)) return false;
    for (const auto& m : p.members)
        if (m.bytes || m.name_bytes || !bytes_zero(m.sha256, 32) || !bytes_zero(reinterpret_cast<const u8*>(m.name), sizeof(m.name))) return false;
    return true;
}
bool valid_reference(const UpdateGenerationReference& r) noexcept { return r.present ? update_package_descriptor_valid(r.package) : empty_package(r.package); }
bool empty_disposal(const UpdateGenerationDisposalIntent& d) noexcept {
    if (d.target != UpdateGenerationDisposalTarget::none || d.file_count || d.directory.volume || d.directory.file) return false;
    for (auto id : d.files) if (id.volume || id.file) return false;
    return true;
}
bool valid_disposal(const UpdateGenerationDisposalIntent& d) noexcept {
    if (d.target == UpdateGenerationDisposalTarget::none) return empty_disposal(d);
    if (d.target > UpdateGenerationDisposalTarget::candidate || !d.directory.file ||
        d.file_count <= kUpdateGenerationFirstMemberSlot || d.file_count > kUpdateGenerationMaximumFiles) return false;
    for (u32 i = 0; i < kUpdateGenerationMaximumFiles; ++i) {
        if (i >= d.file_count) { if (d.files[i].volume || d.files[i].file) return false; continue; }
        if (!d.files[i].file || d.files[i].volume != d.directory.volume || d.files[i] == d.directory) return false;
        for (u32 j = 0; j < i; ++j) if (d.files[i] == d.files[j]) return false;
    }
    return true;
}
bool disposal_equal(const UpdateGenerationDisposalIntent& a, const UpdateGenerationDisposalIntent& b) noexcept {
    if (a.target != b.target || a.file_count != b.file_count || a.directory != b.directory) return false;
    for (u32 i = 0; i < kUpdateGenerationMaximumFiles; ++i) if (a.files[i] != b.files[i]) return false;
    return true;
}
const UpdateGenerationReference* disposal_reference(const UpdateGenerationState& s) noexcept {
    return s.disposal.target == UpdateGenerationDisposalTarget::previous ? &s.previous :
        s.disposal.target == UpdateGenerationDisposalTarget::candidate ? &s.candidate : nullptr;
}
bool valid_state(const UpdateGenerationState& s) noexcept {
    if (bytes_zero(s.installation.bytes, 16) || !s.sequence || s.phase > UpdateGenerationPhase::rollback ||
        !s.active.present || !valid_reference(s.active) || !valid_reference(s.previous) || !valid_reference(s.candidate) || !valid_disposal(s.disposal)) return false;
    if (s.disposal.target != UpdateGenerationDisposalTarget::none) {
        const auto* target = s.disposal.target == UpdateGenerationDisposalTarget::previous ? &s.previous : &s.candidate;
        if (s.disposal.file_count != update_generation_file_count(target->package)) return false;
        const bool previous = s.phase == UpdateGenerationPhase::stable && s.previous.present && s.disposal.target == UpdateGenerationDisposalTarget::previous;
        const bool candidate = s.phase == UpdateGenerationPhase::rollback && s.candidate.present && s.disposal.target == UpdateGenerationDisposalTarget::candidate;
        if (s.sequence == ~u64{0} || (!previous && !candidate)) return false;
    }
    if ((s.previous.present && bytes_equal(s.active.package.document_sha256, s.previous.package.document_sha256, 32)) ||
        (s.candidate.present && bytes_equal(s.active.package.document_sha256, s.candidate.package.document_sha256, 32)) ||
        (s.previous.present && s.candidate.present)) return false;
    switch (s.phase) {
    case UpdateGenerationPhase::stable: return bytes_zero(s.transaction.bytes, 16) && !s.candidate.present;
    case UpdateGenerationPhase::prepared: case UpdateGenerationPhase::rollback:
        return !bytes_zero(s.transaction.bytes, 16) && s.candidate.present && !s.previous.present;
    case UpdateGenerationPhase::pending: case UpdateGenerationPhase::acknowledged:
        return !bytes_zero(s.transaction.bytes, 16) && !s.candidate.present && s.previous.present;
    }
    return false;
}
bool reference_equal(const UpdateGenerationReference& a, const UpdateGenerationReference& b) noexcept {
    return a.present == b.present && update_package_descriptor_equal(a.package, b.package);
}
bool state_equal(const UpdateGenerationState& a, const UpdateGenerationState& b) noexcept {
    return identity(a.installation, b.installation) && identity(a.transaction, b.transaction) &&
        a.sequence == b.sequence && a.phase == b.phase && reference_equal(a.active, b.active) &&
        reference_equal(a.previous, b.previous) && reference_equal(a.candidate, b.candidate) && disposal_equal(a.disposal, b.disposal);
}
bool next(const UpdateGenerationState& s, UpdateGenerationState* output) noexcept {
    return output && valid_state(s) && s.sequence != ~u64{0};
}
void put_reference(u8* b, const UpdateGenerationReference& r) noexcept {
    const auto& p = r.package;
    byte_store_le64(b, p.version); byte_store_le64(b + 8, p.package_bytes);
    copy_bytes(b + 16, p.document_sha256, 32); copy_bytes(b + 48, p.package_sha256, 32);
    b[80] = static_cast<u8>(p.channel); b[81] = static_cast<u8>(p.member_count); b[82] = r.present ? 1 : 0;
    for (u32 i = 0; i < kUpdatePackageMaximumMembers; ++i) {
        u8* m = b + 88 + i * member_bytes;
        byte_store_le64(m, p.members[i].bytes); copy_bytes(m + 8, p.members[i].sha256, 32);
        m[40] = p.members[i].name_bytes; copy_bytes(m + 41, p.members[i].name, kUpdatePackageMaximumNameBytes);
    }
}
bool get_reference(const u8* b, UpdateGenerationReference* r) noexcept {
    if (b[81] > kUpdatePackageMaximumMembers || b[82] > 1 || !bytes_zero(b + 83, 5)) return false;
    auto& p = r->package;
    p.version = byte_load_le64(b); p.package_bytes = byte_load_le64(b + 8);
    copy_bytes(p.document_sha256, b + 16, 32); copy_bytes(p.package_sha256, b + 48, 32);
    p.channel = static_cast<UpdateChannel>(b[80]); p.member_count = b[81];
    for (u32 i = 0; i < kUpdatePackageMaximumMembers; ++i) {
        const u8* m = b + 88 + i * member_bytes;
        if (m[40] > kUpdatePackageMaximumNameBytes || !bytes_zero(m + 73, member_bytes - 73)) return false;
        p.members[i].bytes = byte_load_le64(m); copy_bytes(p.members[i].sha256, m + 8, 32);
        p.members[i].name_bytes = m[40]; copy_bytes(p.members[i].name, m + 41, kUpdatePackageMaximumNameBytes);
    }
    r->present = b[82] == 1;
    return valid_reference(*r);
}
void put_disposal(u8* b, const UpdateGenerationDisposalIntent& d) noexcept {
    b[0] = static_cast<u8>(d.target); b[1] = static_cast<u8>(d.file_count);
    byte_store_le32(b + 8, d.directory.volume); byte_store_le64(b + 16, d.directory.file);
    for (u32 i = 0; i < kUpdateGenerationMaximumFiles; ++i) {
        byte_store_le32(b + 24 + i * 16, d.files[i].volume); byte_store_le64(b + 32 + i * 16, d.files[i].file);
    }
}
bool get_disposal(const u8* b, UpdateGenerationDisposalIntent* d) noexcept {
    if (!bytes_zero(b + 2, 6) || !bytes_zero(b + 12, 4)) return false;
    d->target = static_cast<UpdateGenerationDisposalTarget>(b[0]); d->file_count = b[1];
    d->directory = {byte_load_le32(b + 8), byte_load_le64(b + 16)};
    for (u32 i = 0; i < kUpdateGenerationMaximumFiles; ++i) {
        if (!bytes_zero(b + 28 + i * 16, 4)) return false;
        d->files[i] = {byte_load_le32(b + 24 + i * 16), byte_load_le64(b + 32 + i * 16)};
    }
    return valid_disposal(*d);
}
bool path_valid(ConstSlice<wchar_t> p, u32* ancestors) noexcept {
    if (!p.data || p.count < 4 || p.count > kUpdateGenerationMaximumRootCharacters ||
        ascii_lower(p[0]) < L'a' || ascii_lower(p[0]) > L'z' || p[1] != L':' || p[2] != L'\\') return false;
    u32 component = 3, count = 1;
    for (u32 i = 3; i <= p.count; ++i) {
        if (i == p.count || p[i] == L'\\') {
            if (i == component || p[i - 1] == L'.' || p[i - 1] == L' ' || acceptance_record_reserved_name(p.data + component, i - component)) return false;
            ++count; component = i + 1;
        } else {
            const wchar_t c = p[i];
            if (c < 32 || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') return false;
            if (c >= 0xd800 && c <= 0xdbff) {
                if (++i >= p.count || p[i] < 0xdc00 || p[i] > 0xdfff) return false;
            } else if (c >= 0xdc00 && c <= 0xdfff) return false;
        }
    }
    if (count > kUpdateGenerationMaximumAncestors) return false;
    if (ancestors) *ancestors = count;
    return true;
}
bool directory_identity(HANDLE h, UpdateGenerationDirectoryIdentity* id) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info)) return false;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetLastError(ERROR_BAD_PATHNAME); return false;
    }
    id->volume = info.dwVolumeSerialNumber;
    id->file = static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow;
    return true;
}
bool file_valid(HANDLE h) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    return h && h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_DISK && GetFileInformationByHandle(h, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) && info.nNumberOfLinks == 1;
}
void root_suffix(const UpdateGenerationTransaction& t, const wchar_t* name, wchar_t* path) noexcept {
    for (u32 i = 0; i < t.root_characters; ++i) path[i] = t.root[i];
    u32 n = t.root_characters; path[n++] = L'\\';
    for (u32 i = 0;; ++i) { path[n + i] = name[i]; if (!name[i]) break; }
}
void generation_name(const u8* digest, wchar_t* output) noexcept {
    constexpr wchar_t digits[]{L"0123456789abcdef"};
    output[0] = L'g'; output[1] = L'-';
    for (u32 i = 0; i < 32; ++i) { output[2 + i * 2] = digits[digest[i] >> 4]; output[3 + i * 2] = digits[digest[i] & 15]; }
    output[66] = 0;
}
void directory_path(const UpdateGenerationDirectory& d, wchar_t* output) noexcept {
    wchar_t name[67]{}; generation_name(d.document_sha256, name); root_suffix(*d.transaction, name, output);
}
bool name_equal(const wchar_t* a, const wchar_t* b) noexcept {
    for (u32 i = 0;; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
}
UpdateGenerationResult bounded_inventory(UpdateGenerationTransaction* t) noexcept {
    if (t->inventory_owner) return fail(UpdateGenerationError::release);
    wchar_t pattern[kUpdateGenerationMaximumRootCharacters + 8]{}; root_suffix(*t, L"*", pattern);
    WIN32_FIND_DATAW data{};
    t->inventory_owner = FindFirstFileW(pattern, &data);
    if (t->inventory_owner == INVALID_HANDLE_VALUE) {
        const DWORD native = GetLastError(); t->inventory_owner = nullptr;
        return fail(UpdateGenerationError::open, native);
    }
    UpdateGenerationResult result{}; u32 entries = 0;
    for (;;) {
        if (!name_equal(data.cFileName, L".") && !name_equal(data.cFileName, L"..")) {
            if (++entries > kUpdateGenerationMaximumRootEntries || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                result = fail(UpdateGenerationError::state); break;
            }
            bool recognized = false;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (t->record_present) {
                    const UpdateGenerationReference* refs[]{&t->state.active, &t->state.previous, &t->state.candidate};
                    for (auto* ref : refs) if (ref->present) {
                        wchar_t name[67]{}; generation_name(ref->package.document_sha256, name);
                        if (name_equal(data.cFileName, name)) { recognized = true; break; }
                    }
                }
            } else recognized = name_equal(data.cFileName, L"install.lock") || name_equal(data.cFileName, L"install.state") ||
                name_equal(data.cFileName, L"install.state.new");
            if (!recognized) { result = fail(UpdateGenerationError::state); break; }
        }
        if (!FindNextFileW(t->inventory_owner, &data)) {
            const DWORD native = GetLastError(); if (native != ERROR_NO_MORE_FILES) result = fail(UpdateGenerationError::read, native);
            break;
        }
    }
    if (!FindClose(t->inventory_owner)) return fail(UpdateGenerationError::release, GetLastError());
    t->inventory_owner = nullptr; return result;
}
UpdateGenerationResult authenticate(UpdateGenerationTransaction* t, const UpdateGenerationState& state,
    const UpdateGenerationAuthenticator& a) noexcept {
    if (!a.verify_installed) return fail(UpdateGenerationError::invalid_input);
    const UpdateGenerationReference* references[]{&state.active, &state.previous, &state.candidate};
    const auto* disposing = disposal_reference(t->state);
    const bool minting = t->state.disposal.target == UpdateGenerationDisposalTarget::none &&
        state.disposal.target != UpdateGenerationDisposalTarget::none;
    for (u32 i = 0; i < 3; ++i) {
        const auto* r = references[i];
        // A failed candidate must never veto returning to the independently
        // authenticated old package. It remains recorded for explicit cleanup.
        if (!r->present || (disposing && bytes_equal(r->package.document_sha256, disposing->package.document_sha256, 32)) ||
            (i == 2 && state.phase == UpdateGenerationPhase::rollback && !minting)) continue;
        DWORD native = 0;
        if (!a.verify_installed(a.context, t, r->package, &native)) return fail(UpdateGenerationError::authenticity, native);
        auto result = update_generation_validate(t); if (!result.ok()) return result;
    }
    return {};
}
bool legal_transition(const UpdateGenerationState& old, const UpdateGenerationState& proposed) noexcept {
    UpdateGenerationState expected{};
    const auto is = [&](UpdateGenerationResult r) noexcept { return r.ok() && state_equal(expected, proposed); };
    if (old.disposal.target == UpdateGenerationDisposalTarget::none && proposed.disposal.target != UpdateGenerationDisposalTarget::none)
        return is(update_generation_disposal_begin(old, proposed.disposal, &expected));
    switch (old.phase) {
    case UpdateGenerationPhase::stable:
        if (proposed.phase == UpdateGenerationPhase::prepared)
            return is(update_generation_prepare(old, proposed.transaction, proposed.candidate.package, &expected));
        if (old.disposal.target == UpdateGenerationDisposalTarget::none && proposed.previous.present)
            return is(update_generation_revert(old, &expected));
        return old.disposal.target == UpdateGenerationDisposalTarget::previous &&
            is(update_generation_forget_previous(old, true, &expected));
    case UpdateGenerationPhase::prepared:
        if (proposed.phase == UpdateGenerationPhase::pending) return is(update_generation_activate(old, &expected));
        return is(update_generation_rollback(old, &expected));
    case UpdateGenerationPhase::pending:
        if (proposed.phase == UpdateGenerationPhase::acknowledged) return is(update_generation_acknowledge(old,
            proposed.transaction, {proposed.active.package.document_sha256, 32}, &expected));
        return is(update_generation_rollback(old, &expected));
    case UpdateGenerationPhase::rollback:
        if (old.disposal.target != UpdateGenerationDisposalTarget::candidate) return false;
        [[fallthrough]];
    case UpdateGenerationPhase::acknowledged:
        return is(update_generation_finish(old, true, &expected));
    }
    return false;
}
UpdateGenerationResult close_validation(UpdateGenerationTransaction* t, HANDLE h, UpdateGenerationResult primary) noexcept {
    if (!CloseHandle(h)) { t->validation_owner = h; return fail(UpdateGenerationError::release, GetLastError()); }
    return primary;
}
UpdateGenerationResult validate_disposal_io(UpdateGenerationTransaction* t, const UpdateGenerationState& state, bool complete) noexcept {
    const auto* ref = disposal_reference(state);
    if (!ref || !ref->present || !valid_state(state) || state.disposal.directory.volume != t->identities[t->ancestor_count - 1].volume)
        return fail(UpdateGenerationError::state);
    UpdateGenerationDirectory directory{}; directory.transaction = t;
    copy_bytes(directory.document_sha256, ref->package.document_sha256, 32);
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; directory_path(directory, path);
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD native = GetLastError();
        if (complete && (native == ERROR_FILE_NOT_FOUND || native == ERROR_PATH_NOT_FOUND)) return update_generation_validate(t);
        return fail(UpdateGenerationError::open, native);
    }
    UpdateGenerationDirectoryIdentity id{};
    auto result = directory_identity(h, &id) && id == state.disposal.directory ? UpdateGenerationResult{} : fail(UpdateGenerationError::identity);
    result = close_validation(t, h, result); if (!result.ok()) return result;
    if (complete) return fail(UpdateGenerationError::cleanup, ERROR_DIR_NOT_EMPTY);
    for (u32 i = 0; i < state.disposal.file_count; ++i) {
        u32 count = 0;
        result = update_generation_path(*t, {ref->package.document_sha256, 32}, update_generation_file_name(ref->package, i),
            {path, sizeof(path) / sizeof(path[0])}, &count); if (!result.ok()) return result;
        h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return fail(UpdateGenerationError::open, GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        const bool regular = file_valid(h) && GetFileInformationByHandle(h, &info);
        id = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
        result = close_validation(t, h, regular && id == state.disposal.files[i] ? UpdateGenerationResult{} : fail(UpdateGenerationError::identity));
        if (!result.ok()) return result;
    }
    return update_generation_validate(t);
}
UpdateGenerationResult read_state(HANDLE file, UpdateGenerationState* state) noexcept {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart != kUpdateGenerationRecordBytes) return fail(UpdateGenerationError::size, GetLastError());
    u8 record[kUpdateGenerationRecordBytes]{}; u32 native = 0;
    if (!file_persistence_read_complete(file, {record, sizeof(record)}, &native)) return fail(UpdateGenerationError::read, native);
    return update_generation_decode({record, sizeof(record)}, state);
}
// Authenticates the launchable packages of a proposed journal and the disposal target it begins or finishes.
UpdateGenerationResult authenticate_transition(UpdateGenerationTransaction* t, const UpdateGenerationState& proposed,
    const UpdateGenerationAuthenticator& a) noexcept {
    const auto result = authenticate(t, proposed, a); if (!result.ok()) return result;
    const bool was = t->state.disposal.target != UpdateGenerationDisposalTarget::none;
    const bool is = proposed.disposal.target != UpdateGenerationDisposalTarget::none;
    if (!was && is) return validate_disposal_io(t, proposed, false);
    return was && !is ? validate_disposal_io(t, t->state, true) : UpdateGenerationResult{};
}
bool target_validate(void* opaque, DWORD* native) noexcept {
    const auto r = update_generation_directory_validate(static_cast<UpdateGenerationDirectory*>(opaque));
    if (native) *native = r.native_error; return r.ok();
}
bool target_create(void* opaque, Utf8View name, HANDLE* owner, DWORD* native) noexcept {
    const auto r = update_generation_file_open(static_cast<UpdateGenerationDirectory*>(opaque), name, true, owner);
    if (native) *native = r.native_error; return r.ok();
}
bool target_seal(void* opaque, HANDLE* writable, HANDLE* readonly, DWORD* native) noexcept {
    const auto r = update_generation_file_seal(static_cast<UpdateGenerationDirectory*>(opaque), writable, readonly);
    if (native) *native = r.native_error; return r.ok();
}
// A file name is one of the provenance names or a valid package member name.
bool file_name_valid(Utf8View name) noexcept {
    for (const auto fixed : provenance) if (name.bytes == fixed.bytes && bytes_equal(name.data, fixed.data, name.bytes)) return true;
    return update_package_member_name_valid(name);
}
bool file_name_equal(const wchar_t* found, Utf8View name) noexcept {
    for (u32 i = 0; i < name.bytes; ++i) if (found[i] != static_cast<wchar_t>(name.data[i])) return false;
    return !found[name.bytes];
}
} // namespace

u32 update_generation_file_count(const UpdatePackageDescriptor& p) noexcept {
    return kUpdateGenerationFirstMemberSlot + (p.member_count <= kUpdatePackageMaximumMembers ? p.member_count : 0);
}
Utf8View update_generation_file_name(const UpdatePackageDescriptor& p, u32 slot) noexcept {
    if (slot < kUpdateGenerationFirstMemberSlot) return provenance[slot];
    return slot < update_generation_file_count(p) ? update_package_member_name(p.members[slot - kUpdateGenerationFirstMemberSlot]) : Utf8View{};
}
UpdateGenerationResult update_generation_encode(const UpdateGenerationState& s, Slice<u8> record) noexcept {
    if (!record.data || record.count != kUpdateGenerationRecordBytes || !valid_state(s) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(&s), sizeof(s)}, {record.data, record.count})) return fail(UpdateGenerationError::invalid_input);
    u8 b[kUpdateGenerationRecordBytes]{};
    copy_bytes(b, magic, 8); byte_store_le32(b + 8, kUpdateGenerationRecordVersion); byte_store_le32(b + 12, sizeof(b));
    copy_bytes(b + 16, s.installation.bytes, 16); copy_bytes(b + 32, s.transaction.bytes, 16);
    byte_store_le64(b + 48, s.sequence); b[56] = static_cast<u8>(s.phase);
    put_reference(b + 64, s.active); put_reference(b + 64 + reference_bytes, s.previous); put_reference(b + 64 + reference_bytes * 2, s.candidate);
    put_disposal(b + disposal_offset, s.disposal);
    byte_store_le64(b + checksum_offset, hash64_bytes(kHash64Seed, b, checksum_offset)); copy_bytes(record.data, b, sizeof(b)); return {};
}
UpdateGenerationResult update_generation_decode(ConstSlice<u8> b, UpdateGenerationState* output) noexcept {
    if (!output || !b.data || byte_ranges_overlap({reinterpret_cast<const u8*>(output), sizeof(*output)}, b)) return fail(UpdateGenerationError::invalid_input);
    if (b.count != kUpdateGenerationRecordBytes) return fail(UpdateGenerationError::size);
    if (!bytes_equal(b.data, magic, 8) || byte_load_le32(b.data + 8) != kUpdateGenerationRecordVersion || byte_load_le32(b.data + 12) != b.count ||
        !bytes_zero(b.data + 57, 7) || !bytes_zero(b.data + disposal_offset + disposal_bytes, checksum_offset - disposal_offset - disposal_bytes))
        return fail(UpdateGenerationError::format);
    if (byte_load_le64(b.data + checksum_offset) != hash64_bytes(kHash64Seed, b.data, checksum_offset)) return fail(UpdateGenerationError::checksum);
    UpdateGenerationState s{}; copy_bytes(s.installation.bytes, b.data + 16, 16); copy_bytes(s.transaction.bytes, b.data + 32, 16);
    s.sequence = byte_load_le64(b.data + 48); s.phase = static_cast<UpdateGenerationPhase>(b[56]);
    if (!get_reference(b.data + 64, &s.active) || !get_reference(b.data + 64 + reference_bytes, &s.previous) ||
        !get_reference(b.data + 64 + reference_bytes * 2, &s.candidate) || !get_disposal(b.data + disposal_offset, &s.disposal) || !valid_state(s))
        return fail(UpdateGenerationError::format);
    *output = s; return {};
}
UpdateGenerationResult update_generation_prepare(const UpdateGenerationState& s, const UpdateGenerationIdentity& transaction,
    const UpdatePackageDescriptor& package, UpdateGenerationState* out) noexcept {
    if (!next(s, out) || s.phase != UpdateGenerationPhase::stable || s.previous.present || bytes_zero(transaction.bytes, 16) ||
        !update_package_descriptor_valid(package) || bytes_equal(package.document_sha256, s.active.package.document_sha256, 32))
        return fail(UpdateGenerationError::state);
    auto n = s; ++n.sequence; n.phase = UpdateGenerationPhase::prepared; n.transaction = transaction; n.candidate = {package, true}; *out = n; return {};
}
UpdateGenerationResult update_generation_activate(const UpdateGenerationState& s, UpdateGenerationState* out) noexcept {
    if (!next(s, out) || s.phase != UpdateGenerationPhase::prepared) return fail(UpdateGenerationError::state);
    auto n = s; ++n.sequence; n.phase = UpdateGenerationPhase::pending; n.previous = s.active; n.active = s.candidate; n.candidate = {}; *out = n; return {};
}
UpdateGenerationResult update_generation_acknowledge(const UpdateGenerationState& s, const UpdateGenerationIdentity& transaction,
    ConstSlice<u8> digest, UpdateGenerationState* out) noexcept {
    if (!next(s, out) || s.phase != UpdateGenerationPhase::pending || !identity(s.transaction, transaction) ||
        !digest.data || digest.count != 32 || !bytes_equal(digest.data, s.active.package.document_sha256, 32)) return fail(UpdateGenerationError::identity);
    *out = s; auto& n = *out; ++n.sequence; n.phase = UpdateGenerationPhase::acknowledged; return {};
}
UpdateGenerationResult update_generation_rollback(const UpdateGenerationState& s, UpdateGenerationState* out) noexcept {
    if (!next(s, out) || (s.phase != UpdateGenerationPhase::pending && s.phase != UpdateGenerationPhase::prepared)) return fail(UpdateGenerationError::state);
    const bool pending = s.phase == UpdateGenerationPhase::pending;
    *out = s; auto& n = *out; ++n.sequence; n.phase = UpdateGenerationPhase::rollback;
    if (pending) { n.candidate = n.active; n.active = n.previous; n.previous = {}; }
    return {};
}
UpdateGenerationResult update_generation_finish(const UpdateGenerationState& s, bool disposed, UpdateGenerationState* out) noexcept {
    if (!next(s, out) || (s.phase != UpdateGenerationPhase::acknowledged && s.phase != UpdateGenerationPhase::rollback) ||
        (s.phase == UpdateGenerationPhase::rollback && (!disposed || s.disposal.target != UpdateGenerationDisposalTarget::candidate)))
        return fail(UpdateGenerationError::state);
    *out = s; auto& n = *out; ++n.sequence; n.phase = UpdateGenerationPhase::stable; n.transaction = {}; n.candidate = {}; n.disposal = {}; return {};
}
UpdateGenerationResult update_generation_forget_previous(const UpdateGenerationState& s, bool disposed, UpdateGenerationState* out) noexcept {
    if (!next(s, out) || s.phase != UpdateGenerationPhase::stable || !s.previous.present || !disposed ||
        s.disposal.target != UpdateGenerationDisposalTarget::previous) return fail(UpdateGenerationError::state);
    *out = s; auto& n = *out; ++n.sequence; n.previous = {}; n.disposal = {}; return {};
}
UpdateGenerationResult update_generation_revert(const UpdateGenerationState& s, UpdateGenerationState* out) noexcept {
    if (!next(s, out) || s.phase != UpdateGenerationPhase::stable || !s.previous.present ||
        s.disposal.target != UpdateGenerationDisposalTarget::none) return fail(UpdateGenerationError::state);
    auto n = s; ++n.sequence; n.active = s.previous; n.previous = s.active; *out = n; return {};
}
UpdateGenerationResult update_generation_disposal_begin(const UpdateGenerationState& s,
    const UpdateGenerationDisposalIntent& intent, UpdateGenerationState* out) noexcept {
    if (!next(s, out) || s.sequence >= ~u64{0} - 1 || s.disposal.target != UpdateGenerationDisposalTarget::none)
        return fail(UpdateGenerationError::state);
    // The resulting state checks the target, its phase and its file count.
    auto n = s; ++n.sequence; n.disposal = intent;
    if (!valid_state(n)) return fail(UpdateGenerationError::state);
    *out = n; return {};
}
UpdateGenerationResult update_generation_recovery(const UpdateGenerationState& s, UpdateGenerationRecovery* out) noexcept {
    if (!out || !valid_state(s)) return fail(UpdateGenerationError::state);
    if (s.disposal.target == UpdateGenerationDisposalTarget::previous) { *out = UpdateGenerationRecovery::discard_previous; return {}; }
    constexpr UpdateGenerationRecovery plans[]{UpdateGenerationRecovery::launch_active, UpdateGenerationRecovery::retain_prepared,
        UpdateGenerationRecovery::roll_back, UpdateGenerationRecovery::finish_acknowledgement, UpdateGenerationRecovery::discard_candidate};
    *out = plans[static_cast<u32>(s.phase)]; return {};
}

UpdateGenerationResult update_generation_begin(const UpdateGenerationEnrollment& e, UpdateGenerationTransaction* t, bool provision) noexcept {
    u32 count = 0;
    if (!t || t->gate || t->ancestor_count || t->record_owner || t->record_temporary || t->validation_owner || t->inventory_owner || !e.explicitly_enrolled ||
        bytes_zero(e.installation.bytes, 16) || bytes_zero(e.publisher_inventory_sha256, 32)) return fail(UpdateGenerationError::enrollment);
    if (!path_valid(e.root, &count) || !e.ancestors.data || e.ancestors.count != count) return fail(UpdateGenerationError::path);
    t->state = {}; t->record_present = false; t->provisioning_admitted = false;
    wchar_t drive[4]{e.root[0], L':', L'\\', 0};
    if (GetDriveTypeW(drive) != DRIVE_FIXED) return fail(UpdateGenerationError::path, ERROR_NOT_SUPPORTED);
    t->root_characters = e.root.count; for (u32 i = 0; i < e.root.count; ++i) t->root[i] = e.root[i]; t->root[e.root.count] = 0;
    t->installation = e.installation; copy_bytes(t->publisher_inventory_sha256, e.publisher_inventory_sha256, 32);
    for (u32 i = 2; i <= e.root.count; ++i) if (i == 2 || i == e.root.count || t->root[i] == L'\\') {
        const u32 end = i == 2 ? 3 : i; const wchar_t saved = t->root[end]; t->root[end] = 0;
        HANDLE h = CreateFileW(t->root, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr); t->root[end] = saved;
        if (h == INVALID_HANDLE_VALUE) return fail(UpdateGenerationError::open, GetLastError());
        const u32 index = t->ancestor_count++; t->ancestors[index] = h;
        if (!directory_identity(h, &t->identities[index])) return fail(UpdateGenerationError::file, GetLastError());
        if (t->identities[index] != e.ancestors[index]) return fail(UpdateGenerationError::identity);
    }
    wchar_t canonical[kUpdateGenerationMaximumRootCharacters + 5]{};
    const DWORD canonical_characters = GetFinalPathNameByHandleW(t->ancestors[t->ancestor_count - 1], canonical,
        static_cast<DWORD>(sizeof(canonical) / sizeof(wchar_t)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (canonical_characters != t->root_characters + 4 || canonical[0] != L'\\' || canonical[1] != L'\\' ||
        canonical[2] != L'?' || canonical[3] != L'\\' ||
        CompareStringOrdinal(t->root, static_cast<int>(t->root_characters), canonical + 4,
            static_cast<int>(canonical_characters - 4), TRUE) != CSTR_EQUAL)
        return fail(UpdateGenerationError::path, ERROR_BAD_PATHNAME);
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 32]{}; root_suffix(*t, L"install.lock", path);
    t->gate = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (t->gate == INVALID_HANDLE_VALUE) { t->gate = nullptr; return fail(UpdateGenerationError::gate, GetLastError()); }
    if (!file_valid(t->gate)) return fail(UpdateGenerationError::file, GetLastError());
    root_suffix(*t, L"install.state", path);
    t->record_owner = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (t->record_owner == INVALID_HANDLE_VALUE) {
        const DWORD native = GetLastError(); t->record_owner = nullptr;
        if (provision && native == ERROR_FILE_NOT_FOUND) {
            t->provisioning_admitted = true;
            return {};
        }
        return fail(UpdateGenerationError::open, native);
    }
    if (!file_valid(t->record_owner)) return fail(UpdateGenerationError::file, GetLastError());
    UpdateGenerationState state{}; auto result = read_state(t->record_owner, &state); if (!result.ok()) return result;
    if (!identity(state.installation, t->installation)) return fail(UpdateGenerationError::identity);
    if (!CloseHandle(t->record_owner)) return fail(UpdateGenerationError::release, GetLastError());
    t->record_owner = nullptr; t->state = state; t->record_present = true; return update_generation_validate(t);
}
UpdateGenerationResult update_generation_validate(UpdateGenerationTransaction* t) noexcept {
    u32 count = 0;
    if (!t || !t->gate || t->validation_owner || !path_valid({t->root, t->root_characters}, &count) || count != t->ancestor_count ||
        bytes_zero(t->installation.bytes, 16) || bytes_zero(t->publisher_inventory_sha256, 32) || !file_valid(t->gate)) return fail(UpdateGenerationError::gate);
    for (u32 i = 0; i < t->ancestor_count; ++i) {
        UpdateGenerationDirectoryIdentity actual{};
        if (!directory_identity(t->ancestors[i], &actual)) return fail(UpdateGenerationError::file, GetLastError());
        if (actual != t->identities[i]) return fail(UpdateGenerationError::identity);
    }
    // Directory handles deny rename/deletion. Reopen each component to ensure
    // the current canonical name still selects the held object.
    u32 index = 0;
    for (u32 i = 2; i <= t->root_characters; ++i) if (i == 2 || i == t->root_characters || t->root[i] == L'\\') {
        const u32 end = i == 2 ? 3 : i; const wchar_t saved = t->root[end]; t->root[end] = 0;
        HANDLE h = CreateFileW(t->root, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr); t->root[end] = saved;
        if (h == INVALID_HANDLE_VALUE) return fail(UpdateGenerationError::open, GetLastError());
        UpdateGenerationDirectoryIdentity actual{}; const bool good = directory_identity(h, &actual) && actual == t->identities[index++];
        if (!CloseHandle(h)) { t->validation_owner = h; return fail(UpdateGenerationError::release, GetLastError()); }
        if (!good) return fail(UpdateGenerationError::identity);
    }
    return {};
}
UpdateGenerationResult update_generation_cleanup_record(UpdateGenerationTransaction* t) noexcept {
    if (!t) return fail(UpdateGenerationError::invalid_input);
    if (t->record_temporary && !t->record_owner) {
        wchar_t path[kUpdateGenerationMaximumRootCharacters + 32]{}; root_suffix(*t, L"install.state.new", path);
        t->record_owner = CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (t->record_owner == INVALID_HANDLE_VALUE) { t->record_owner = nullptr; return fail(UpdateGenerationError::cleanup, GetLastError()); }
    }
    if (t->record_owner) {
        if (t->record_temporary) {
            FILE_DISPOSITION_INFO info{TRUE};
            if (!SetFileInformationByHandle(t->record_owner, FileDispositionInfo, &info, sizeof(info))) return fail(UpdateGenerationError::cleanup, GetLastError());
        }
        if (!CloseHandle(t->record_owner)) return fail(UpdateGenerationError::release, GetLastError());
        t->record_owner = nullptr;
    }
    t->record_temporary = false; return {};
}
UpdateGenerationResult update_generation_recover_temporary(UpdateGenerationTransaction* t, const UpdateGenerationAuthenticator& a) noexcept {
    auto result = update_generation_validate(t); if (!result.ok()) return result;
    if (!t->record_present || t->record_owner || t->record_temporary) return fail(UpdateGenerationError::state);
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 32]{}; root_suffix(*t, L"install.state.new", path);
    t->record_owner = CreateFileW(path, GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (t->record_owner == INVALID_HANDLE_VALUE) {
        const DWORD native = GetLastError(); t->record_owner = nullptr;
        return native == ERROR_FILE_NOT_FOUND ? UpdateGenerationResult{} : fail(UpdateGenerationError::open, native);
    }
    if (!file_valid(t->record_owner)) return fail(UpdateGenerationError::file, GetLastError());
    UpdateGenerationState proposed{}; result = read_state(t->record_owner, &proposed); if (!result.ok()) return result;
    if (!identity(proposed.installation, t->installation) || !legal_transition(t->state, proposed)) return fail(UpdateGenerationError::state);
    result = authenticate_transition(t, proposed, a); if (!result.ok()) return result;
    t->record_temporary = true; return update_generation_cleanup_record(t);
}
UpdateGenerationResult update_generation_commit(UpdateGenerationTransaction* t, const UpdateGenerationState& proposed,
    const UpdateGenerationAuthenticator& a) noexcept {
    auto result = update_generation_validate(t); if (!result.ok()) return result;
    if (t->record_owner || t->record_temporary || !valid_state(proposed) || !identity(proposed.installation, t->installation) ||
        (t->record_present ? !legal_transition(t->state, proposed) :
         (!t->provisioning_admitted || proposed.phase != UpdateGenerationPhase::stable ||
             proposed.sequence != 1 || proposed.previous.present))) return fail(UpdateGenerationError::state);
    result = authenticate_transition(t, proposed, a); if (!result.ok()) return result;
    u8 record[kUpdateGenerationRecordBytes]{}; result = update_generation_encode(proposed, {record, sizeof(record)}); if (!result.ok()) return result;
    wchar_t temporary[kUpdateGenerationMaximumRootCharacters + 32]{}, path[kUpdateGenerationMaximumRootCharacters + 32]{};
    root_suffix(*t, L"install.state.new", temporary); root_suffix(*t, L"install.state", path);
    t->record_owner = CreateFileW(temporary, GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (t->record_owner == INVALID_HANDLE_VALUE) { t->record_owner = nullptr; return fail(UpdateGenerationError::open, GetLastError()); }
    t->record_temporary = true; DWORD written = 0;
    if (!WriteFile(t->record_owner, record, sizeof(record), &written, nullptr) || written != sizeof(record)) return fail(UpdateGenerationError::write, GetLastError());
    if (!FlushFileBuffers(t->record_owner)) return fail(UpdateGenerationError::flush, GetLastError());
    if (!CloseHandle(t->record_owner)) return fail(UpdateGenerationError::release, GetLastError());
    t->record_owner = nullptr;
    result = update_generation_validate(t); if (!result.ok()) return result;
    if (!MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return fail(UpdateGenerationError::commit, GetLastError());
    t->record_temporary = false; t->state = proposed; t->record_present = true; t->provisioning_admitted = false; return {};
}
UpdateGenerationResult update_generation_select(UpdateGenerationTransaction* t, const UpdateGenerationAuthenticator& a, UpdatePackageDescriptor* out) noexcept {
    auto result = update_generation_validate(t); if (!result.ok()) return result;
    if (!out || !t->record_present || !valid_state(t->state) || t->record_owner || t->record_temporary ||
        t->state.phase == UpdateGenerationPhase::pending || t->state.phase == UpdateGenerationPhase::acknowledged) return fail(UpdateGenerationError::state);
    // Prepared and failed candidates are never launch targets. Verify only the
    // selected package; bad uncommitted bytes cannot suppress the valid old one.
    if (!a.verify_installed) return fail(UpdateGenerationError::invalid_input);
    DWORD native = 0;
    if (!a.verify_installed(a.context, t, t->state.active.package, &native)) return fail(UpdateGenerationError::authenticity, native);
    result = update_generation_validate(t); if (!result.ok()) return result;
    *out = t->state.active.package; return {};
}
UpdateGenerationResult update_generation_release(UpdateGenerationTransaction* t) noexcept {
    if (!t) return fail(UpdateGenerationError::invalid_input);
    auto r = update_generation_cleanup_record(t); if (!r.ok()) return r;
    if (t->validation_owner) {
        if (!CloseHandle(t->validation_owner)) return fail(UpdateGenerationError::release, GetLastError());
        t->validation_owner = nullptr;
    }
    if (t->inventory_owner) {
        if (!FindClose(t->inventory_owner)) return fail(UpdateGenerationError::release, GetLastError());
        t->inventory_owner = nullptr;
    }
    if (t->gate) { if (!CloseHandle(t->gate)) return fail(UpdateGenerationError::release, GetLastError()); t->gate = nullptr; }
    while (t->ancestor_count) {
        const u32 i = t->ancestor_count - 1;
        if (!CloseHandle(t->ancestors[i])) return fail(UpdateGenerationError::release, GetLastError());
        t->ancestors[i] = nullptr; --t->ancestor_count;
    }
    *t = {};
    return {};
}
UpdateGenerationResult update_generation_directory_open(UpdateGenerationTransaction* t, ConstSlice<u8> digest,
    bool create, UpdateGenerationDirectory* out) noexcept {
    auto result = update_generation_validate(t); if (!result.ok()) return result;
    if (!out || out->directory || out->created || !digest.data || digest.count != 32 || bytes_zero(digest.data, 32)) return fail(UpdateGenerationError::invalid_input);
    if (create) { result = bounded_inventory(t); if (!result.ok()) return result; }
    out->transaction = t; copy_bytes(out->document_sha256, digest.data, 32);
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 80]{}; directory_path(*out, path);
    if (create) { if (!CreateDirectoryW(path, nullptr)) return fail(UpdateGenerationError::open, GetLastError()); out->created = true; }
    out->directory = CreateFileW(path, FILE_READ_ATTRIBUTES | (create ? DELETE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (out->directory == INVALID_HANDLE_VALUE) { out->directory = nullptr; return fail(UpdateGenerationError::open, GetLastError()); }
    if (!directory_identity(out->directory, &out->identity)) return fail(UpdateGenerationError::file, GetLastError());
    out->identity_captured = true; out->disposal_authorized = create;
    return update_generation_directory_validate(out);
}
UpdateGenerationResult update_generation_directory_validate(UpdateGenerationDirectory* d) noexcept {
    if (!d || !d->directory || !d->identity_captured || bytes_zero(d->document_sha256, 32)) return fail(UpdateGenerationError::invalid_input);
    auto result = update_generation_validate(d->transaction); if (!result.ok()) return result;
    UpdateGenerationDirectoryIdentity actual{};
    if (!directory_identity(d->directory, &actual)) return fail(UpdateGenerationError::file, GetLastError());
    if (actual != d->identity) return fail(UpdateGenerationError::identity);
    return {};
}
UpdateGenerationResult update_generation_directory_authorize_disposal(UpdateGenerationDirectory* d,
    const UpdateGenerationAuthenticator& a) noexcept {
    if (!d) return fail(UpdateGenerationError::invalid_input);
    auto result = update_generation_directory_validate(d); if (!result.ok()) return result;
    auto* t = d->transaction;
    if (!t->record_present || !valid_state(t->state) || !a.verify_installed) return fail(UpdateGenerationError::state);
    const auto* ref = disposal_reference(t->state);
    if (ref && ref->present && bytes_equal(ref->package.document_sha256, d->document_sha256, 32) && d->identity == t->state.disposal.directory) {
        DWORD native = 0;
        if (!a.verify_installed(a.context, t, t->state.active.package, &native)) return fail(UpdateGenerationError::authenticity, native);
        result = update_generation_directory_validate(d); if (!result.ok()) return result;
        if (!d->created && !d->disposal_authorized) {
            // ACL-protected enrolled root and the retained exclusive gate forbid
            // another coordinator changing this name during the access upgrade.
            if (!CloseHandle(d->directory)) return fail(UpdateGenerationError::release, GetLastError());
            d->directory = nullptr;
            wchar_t path[kUpdateGenerationMaximumRootCharacters + 80]{}; directory_path(*d, path);
            HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h == INVALID_HANDLE_VALUE) return fail(UpdateGenerationError::open, GetLastError());
            d->directory = h; UpdateGenerationDirectoryIdentity actual{};
            if (!directory_identity(h, &actual) || actual != d->identity) return fail(UpdateGenerationError::identity);
        }
        d->disposal_authorized = true; return {};
    }
    return fail(UpdateGenerationError::state);
}
UpdateGenerationResult update_generation_disposal_directory_open(UpdateGenerationTransaction* t,
    UpdateGenerationDirectory* out, bool* absent) noexcept {
    auto result = update_generation_validate(t); if (!result.ok()) return result;
    if (!out || !absent || out->directory || out->created || !t->record_present || !valid_state(t->state) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(out), sizeof(*out)}, {reinterpret_cast<const u8*>(t), sizeof(*t)}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(absent), sizeof(*absent)}, {reinterpret_cast<const u8*>(out), sizeof(*out)}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(absent), sizeof(*absent)}, {reinterpret_cast<const u8*>(t), sizeof(*t)}))
        return fail(UpdateGenerationError::invalid_input);
    const auto* ref = disposal_reference(t->state);
    if (!ref || !ref->present) return fail(UpdateGenerationError::state);
    result = update_generation_directory_open(t, {ref->package.document_sha256, 32}, false, out);
    if (!result.ok()) {
        if (result.error == UpdateGenerationError::open && (result.native_error == ERROR_FILE_NOT_FOUND || result.native_error == ERROR_PATH_NOT_FOUND)) {
            result = update_generation_validate(t); if (!result.ok()) return result;
            *out = {}; *absent = true; return {};
        }
        return result;
    }
    if (out->identity != t->state.disposal.directory) return fail(UpdateGenerationError::identity);
    *absent = false; return {};
}
UpdateGenerationResult update_generation_disposal_inventory(UpdateGenerationDirectory* d) noexcept {
    auto result = update_generation_directory_validate(d); if (!result.ok()) return result;
    auto* t = d->transaction; const auto* ref = disposal_reference(t->state);
    if (!ref || !ref->present || !d->disposal_authorized || d->identity != t->state.disposal.directory ||
        !bytes_equal(d->document_sha256, ref->package.document_sha256, 32) || t->inventory_owner) return fail(UpdateGenerationError::state);
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; directory_path(*d, path);
    u32 n = 0; while (path[n]) ++n; path[n++] = L'\\'; path[n++] = L'*'; path[n] = 0;
    WIN32_FIND_DATAW data{}; t->inventory_owner = FindFirstFileW(path, &data);
    if (t->inventory_owner == INVALID_HANDLE_VALUE) {
        const DWORD native = GetLastError(); t->inventory_owner = nullptr;
        return native == ERROR_FILE_NOT_FOUND ? update_generation_directory_validate(d) : fail(UpdateGenerationError::open, native);
    }
    u32 entries = 0;
    for (;;) {
        if (!name_equal(data.cFileName, L".") && !name_equal(data.cFileName, L"..")) {
            const u32 files = t->state.disposal.file_count;
            u32 index = files;
            for (u32 i = 0; i < files; ++i) if (file_name_equal(data.cFileName, update_generation_file_name(ref->package, i))) index = i;
            if (++entries > files || index == files || data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
                result = fail(UpdateGenerationError::cleanup, ERROR_INVALID_DATA); break;
            }
            u32 count = 0;
            result = update_generation_path(*t, {d->document_sha256, 32}, update_generation_file_name(ref->package, index),
                {path, sizeof(path) / sizeof(path[0])}, &count); if (!result.ok()) break;
            HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h == INVALID_HANDLE_VALUE) { result = fail(UpdateGenerationError::open, GetLastError()); break; }
            BY_HANDLE_FILE_INFORMATION info{}; const bool regular = file_valid(h) && GetFileInformationByHandle(h, &info);
            const UpdateGenerationDirectoryIdentity id{info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
            result = close_validation(t, h, regular && id == t->state.disposal.files[index] ? UpdateGenerationResult{} : fail(UpdateGenerationError::identity));
            if (!result.ok()) break;
        }
        if (!FindNextFileW(t->inventory_owner, &data)) {
            const DWORD native = GetLastError(); if (native != ERROR_NO_MORE_FILES) result = fail(UpdateGenerationError::read, native);
            break;
        }
    }
    if (!FindClose(t->inventory_owner)) return fail(UpdateGenerationError::release, GetLastError());
    t->inventory_owner = nullptr;
    return result.ok() ? update_generation_directory_validate(d) : result;
}
UpdateGenerationResult update_generation_path(const UpdateGenerationTransaction& t, ConstSlice<u8> digest,
    Utf8View name, Slice<wchar_t> output, u32* characters) noexcept {
    if (!path_valid({t.root, t.root_characters}, nullptr) || !digest.data || digest.count != 32 || bytes_zero(digest.data, 32) ||
        !file_name_valid(name) || !output.data || !characters) return fail(UpdateGenerationError::invalid_input);
    const u32 required = t.root_characters + 1 + 66 + 1 + name.bytes;
    if (output.count <= required) return fail(UpdateGenerationError::size);
    wchar_t generation[67]{}; generation_name(digest.data, generation); root_suffix(t, generation, output.data);
    u32 used = t.root_characters + 67; output[used++] = L'\\';
    for (u32 i = 0; i < name.bytes; ++i) output[used + i] = static_cast<wchar_t>(name.data[i]);
    output[used + name.bytes] = 0; *characters = required; return {};
}
UpdateGenerationResult update_generation_file_open(UpdateGenerationDirectory* d, Utf8View name, bool create, HANDLE* output) noexcept {
    auto result = update_generation_directory_validate(d); if (!result.ok()) return result;
    if (!output || *output || (create && !d->created)) return fail(UpdateGenerationError::invalid_input);
    wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 count = 0;
    result = update_generation_path(*d->transaction, {d->document_sha256, 32}, name, {path, sizeof(path) / sizeof(path[0])}, &count);
    if (!result.ok()) return result;
    HANDLE h = CreateFileW(path, create ? GENERIC_READ | GENERIC_WRITE | DELETE : GENERIC_READ, FILE_SHARE_READ, nullptr,
        create ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | (create ? FILE_FLAG_WRITE_THROUGH : 0), nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(UpdateGenerationError::open, GetLastError());
    *output = h; if (!file_valid(h)) return fail(UpdateGenerationError::file, GetLastError()); return {};
}
UpdateGenerationResult update_generation_file_seal(UpdateGenerationDirectory* d, HANDLE* writable, HANDLE* readonly) noexcept {
    auto result = update_generation_directory_validate(d); if (!result.ok()) return result;
    if (!writable || !readonly || writable == readonly || !*writable || *readonly || !d->created || !file_valid(*writable)) return fail(UpdateGenerationError::invalid_input);
    if (!FlushFileBuffers(*writable)) return fail(UpdateGenerationError::flush, GetLastError());
    if (!DuplicateHandle(GetCurrentProcess(), *writable, GetCurrentProcess(), readonly, GENERIC_READ | DELETE, FALSE, 0)) return fail(UpdateGenerationError::open, GetLastError());
    if (!CloseHandle(*writable)) return fail(UpdateGenerationError::release, GetLastError());
    *writable = nullptr; return update_generation_directory_validate(d);
}
UpdatePackageTarget update_generation_package_target(UpdateGenerationDirectory* d) noexcept { return {d, target_validate, target_create, target_seal}; }
UpdateGenerationResult update_generation_directory_release(UpdateGenerationDirectory* d, bool discard) noexcept {
    if (!d) return fail(UpdateGenerationError::invalid_input);
    if (discard && (!d->disposal_authorized || !d->identity_captured)) return fail(UpdateGenerationError::cleanup);
    if (discard && !d->directory) {
        auto result = update_generation_validate(d->transaction); if (!result.ok()) return result;
        wchar_t path[kUpdateGenerationMaximumRootCharacters + 80]{}; directory_path(*d, path);
        d->directory = CreateFileW(path, FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (d->directory == INVALID_HANDLE_VALUE) { d->directory = nullptr; return fail(UpdateGenerationError::open, GetLastError()); }
        UpdateGenerationDirectoryIdentity actual{};
        if (!directory_identity(d->directory, &actual)) return fail(UpdateGenerationError::file, GetLastError());
        if (actual != d->identity) return fail(UpdateGenerationError::identity);
    }
    if (d->directory) {
        if (discard) {
            auto result = update_generation_directory_validate(d); if (!result.ok()) return result;
            FILE_DISPOSITION_INFO info{TRUE};
            if (!SetFileInformationByHandle(d->directory, FileDispositionInfo, &info, sizeof(info))) return fail(UpdateGenerationError::cleanup, GetLastError());
        }
        if (!CloseHandle(d->directory)) return fail(UpdateGenerationError::release, GetLastError());
        d->directory = nullptr;
    }
    if (discard) { d->created = false; d->disposal_authorized = false; } return {};
}
} // namespace maicord
