#include "maicord/app/update_install.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
UpdateInstallResult fail(UpdateInstallError error, DWORD native = 0) noexcept {
    UpdateInstallResult r{}; r.error = error; r.native_error = native; return r;
}
UpdateInstallResult generation(UpdateGenerationResult g) noexcept {
    auto r = g.ok() ? UpdateInstallResult{} : fail(UpdateInstallError::generation, g.native_error); r.generation = g; return r;
}
UpdateInstallResult package_result(UpdatePackageResult p, UpdateInstallError error) noexcept {
    auto r = p.ok() ? UpdateInstallResult{} : fail(error, p.native_error); r.package = p;
    // Diagnostic snapshots never duplicate cleanup capabilities.
    for (auto& owner : r.package.cleanup_files) owner = nullptr;
    r.package.cleanup_hash = nullptr; return r;
}
bool file_info(HANDLE h, UpdateGenerationDirectoryIdentity* id, u64* bytes) noexcept {
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) || !GetFileSizeEx(h, &size) ||
        size.QuadPart < 0 || info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || info.nNumberOfLinks != 1) return false;
    *id = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
    *bytes = static_cast<u64>(size.QuadPart); return true;
}
bool add(u64* total, u64 bytes) noexcept { if (bytes > ~u64{0} - *total) return false; *total += bytes; return true; }
bool add_allocated(u64* total, u64 bytes, u64 cluster) noexcept {
    const u64 remainder = bytes % cluster;
    return add(total, bytes) && (!remainder || add(total, cluster - remainder));
}
bool directory_owned(const UpdateInstallContext& c) noexcept { return c.directory.directory || c.directory.created; }
bool files_owned(const UpdateInstallContext& c) noexcept {
    if (c.seal_owner || c.extraction.cleanup_hash || c.extraction.cleanup_error) return true;
    for (auto h : c.package.files) if (h) return true;
    for (auto h : c.files) if (h) return true;
    for (auto h : c.extraction.cleanup_files) if (h) return true;
    return false;
}
UpdateInstallResult valid(UpdateInstallContext* c) noexcept {
    if (!c || !c->initialized || !c->transaction || !c->authentication.key || !c->authentication.workspace)
        return fail(UpdateInstallError::invalid_input);
    return generation(update_generation_validate(c->transaction));
}
bool output_disjoint(const UpdateInstallContext* c, const void* output, u32 bytes) noexcept {
    const ConstSlice<u8> destination{static_cast<const u8*>(output), bytes};
    if (!output || !byte_range_valid(destination) || !c || !c->transaction || !c->authentication.key || !c->authentication.workspace) return false;
    const ConstSlice<u8> owners[]{{reinterpret_cast<const u8*>(c), sizeof(*c)},
        {reinterpret_cast<const u8*>(c->transaction), sizeof(*c->transaction)},
        {reinterpret_cast<const u8*>(c->authentication.key), sizeof(*c->authentication.key)},
        {reinterpret_cast<const u8*>(c->authentication.workspace), sizeof(*c->authentication.workspace)},
        {c->authentication.workspace->storage.data, c->authentication.workspace->storage.capacity}};
    for (auto owner : owners) if (byte_ranges_overlap(destination, owner)) return false;
    return true;
}
void clear_activation(UpdateInstallContext* c) noexcept {
    c->activation_lease = false; c->activation_sequence = 0; reset_value(&c->activation_transaction);
    SecureZeroMemory(c->activation_document_sha256, sizeof(c->activation_document_sha256));
}
void clear_directory(UpdateInstallContext* c) noexcept {
    reset_value(&c->directory); reset_value(&c->admitted_package); c->cleanup_authorized = false;
    c->disposal_started = false;
    for (u32 i = 0; i < kUpdateGenerationMaximumFiles; ++i) {
        c->known_files[i] = false; c->disposed_files[i] = false; c->created_file_owners[i] = false;
        c->disposition_set[i] = false; reset_value(&c->file_identities[i]);
    }
}
const UpdateGenerationReference* disposal_reference(const UpdateInstallContext& c) noexcept {
    const auto& s = c.transaction->state;
    return s.disposal.target == UpdateGenerationDisposalTarget::previous ? &s.previous :
        s.disposal.target == UpdateGenerationDisposalTarget::candidate ? &s.candidate : nullptr;
}
bool durable_directory(const UpdateInstallContext& c) noexcept {
    const auto* ref = disposal_reference(c);
    return ref && ref->present && bytes_equal(ref->package.document_sha256, c.directory.document_sha256, 32) &&
        c.directory.identity == c.transaction->state.disposal.directory;
}
// The package whose files the owned directory holds: its disposal target or
// the candidate this installer extracted.
const UpdatePackageDescriptor& directory_package(const UpdateInstallContext& c) noexcept {
    return durable_directory(c) ? disposal_reference(c)->package : c.admitted_package;
}
bool root_contains(const UpdateGenerationTransaction& t, ConstSlice<wchar_t> path) noexcept {
    if (path.count < t.root_characters) return false;
    if (CompareStringOrdinal(t.root, static_cast<int>(t.root_characters), path.data,
        static_cast<int>(t.root_characters), TRUE) != CSTR_EQUAL) return false;
    return path.count == t.root_characters || path[t.root_characters] == L'\\';
}
bool stage_outside_root(const UpdateGenerationTransaction& t, HANDLE h) noexcept {
    wchar_t path[kUpdateAcceptanceMaximumPathCharacters + 5]{};
    const DWORD n = GetFinalPathNameByHandleW(h, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= sizeof(path) / sizeof(path[0]) || n < 7 || path[0] != L'\\' || path[1] != L'\\' || path[2] != L'?' || path[3] != L'\\') return false;
    if (!((path[4] >= L'A' && path[4] <= L'Z') || (path[4] >= L'a' && path[4] <= L'z')) || path[5] != L':' || path[6] != L'\\') return false;
    wchar_t drive[]{path[4], L':', L'\\', 0}; if (GetDriveTypeW(drive) != DRIVE_FIXED) return false;
    return !root_contains(t, {path + 4, n - 4});
}
UpdateInstallResult preflight(UpdateInstallContext* c, UpdateStage* stage, const UpdateManifest& offer, ConstSlice<HANDLE> extra) noexcept {
    if (extra.count > 4 || (extra.count && !extra.data)) return fail(UpdateInstallError::invalid_input);
    DWORD sectors = 0, sector_bytes = 0, free_clusters = 0, total_clusters = 0;
    wchar_t drive[]{c->transaction->root[0], L':', L'\\', 0};
    if (!GetDiskFreeSpaceW(drive, &sectors, &sector_bytes, &free_clusters, &total_clusters)) return fail(UpdateInstallError::disk, GetLastError());
    const u64 cluster = static_cast<u64>(sectors) * sector_bytes;
    if (!cluster || cluster > 1024 * 1024) return fail(UpdateInstallError::disk, ERROR_NOT_SUPPORTED);
    // One allocation unit per file plus two cover the directory and metadata
    // replacement for each bounded generation. Logical package/member bytes are
    // counted separately; member total cannot exceed the stored ZIP.
    u64 candidate = 0;
    if (!add(&candidate, offer.artifact_bytes) || !add(&candidate, offer.artifact_bytes) ||
        !add(&candidate, kUpdateManifestMaximumBytes + 64 + cluster * (kUpdateGenerationMaximumFiles + 2))) return fail(UpdateInstallError::budget);
    u64 retained = candidate + kUpdateGenerationRecordBytes * 2 + cluster * 4;
    const UpdateGenerationReference* refs[]{&c->transaction->state.active, &c->transaction->state.previous, &c->transaction->state.candidate};
    for (u32 i = 0; i < 3; ++i) if (refs[i]->present) {
        bool duplicate = false;
        for (u32 j = 0; j < i; ++j) if (refs[j]->present && bytes_equal(refs[i]->package.document_sha256, refs[j]->package.document_sha256, 32)) duplicate = true;
        if (duplicate) continue;
        const auto& p = refs[i]->package;
        if (p.package_bytes > c->limits.maximum_artifact_bytes || !add_allocated(&retained, p.package_bytes, cluster) ||
            !add_allocated(&retained, kUpdateManifestMaximumBytes, cluster) || !add_allocated(&retained, 64, cluster) ||
            !add(&retained, cluster * 2)) return fail(UpdateInstallError::budget);
        for (u32 m = 0; m < p.member_count && m < kUpdatePackageMaximumMembers; ++m)
            if (!add_allocated(&retained, p.members[m].bytes, cluster)) return fail(UpdateInstallError::budget);
    }
    UpdateGenerationDirectoryIdentity ids[5]{}; u32 count = 0; // the stage and at most four extra files
    for (u32 i = 0; i <= extra.count; ++i) {
        const HANDLE h = i ? extra[i - 1] : stage->file; u64 bytes = 0; UpdateGenerationDirectoryIdentity id{};
        if (!file_info(h, &id, &bytes) || id.volume != c->transaction->identities[c->transaction->ancestor_count - 1].volume ||
            !stage_outside_root(*c->transaction, h)) return fail(UpdateInstallError::stage, ERROR_INVALID_DATA);
        bool duplicate = false; for (u32 j = 0; j < count; ++j) if (id == ids[j]) duplicate = true;
        if (!duplicate) { ids[count++] = id; if (!add_allocated(&retained, bytes, cluster)) return fail(UpdateInstallError::budget); }
    }
    UpdateInstallResult r{}; r.retained_bytes_bound = retained;
    r.required_free_bytes = candidate;
    if (!add(&r.required_free_bytes, c->limits.free_space_reserve_bytes)) return fail(UpdateInstallError::budget);
    if (retained > c->limits.maximum_retained_bytes) { r.error = UpdateInstallError::budget; return r; }
    ULARGE_INTEGER available{}, total{}, free{};
    if (!GetDiskFreeSpaceExW(c->transaction->root, &available, &total, &free)) { r.error = UpdateInstallError::disk; r.native_error = GetLastError(); return r; }
    if (available.QuadPart < r.required_free_bytes) { r.error = UpdateInstallError::disk; r.native_error = ERROR_DISK_FULL; }
    return r;
}
bool write_all(HANDLE h, const void* bytes, u32 count, DWORD* native) noexcept {
    u32 done = 0;
    while (done < count) { DWORD written = 0;
        if (!WriteFile(h, static_cast<const u8*>(bytes) + done, count - done, &written, nullptr)) { *native = GetLastError(); return false; }
        if (!written || written > count - done) { *native = ERROR_WRITE_FAULT; return false; } done += written;
    } return true;
}
bool copy_stage(UpdateStage* stage, HANDLE target, DWORD* native) noexcept {
    LARGE_INTEGER zero{}; u8 buffer[kUpdatePackageReadBufferBytes]{};
    if (!SetFilePointerEx(stage->file, zero, nullptr, FILE_BEGIN)) { *native = GetLastError(); return false; }
    u64 remaining = stage->bytes;
    while (remaining) { DWORD got = 0; const DWORD wanted = static_cast<DWORD>(minimum<u64>(remaining, sizeof(buffer)));
        if (!ReadFile(stage->file, buffer, wanted, &got, nullptr)) { *native = GetLastError(); return false; }
        if (!got || got > wanted) { *native = ERROR_HANDLE_EOF; return false; }
        if (!write_all(target, buffer, got, native)) return false; remaining -= got;
    }
    return true;
}
UpdateInstallResult capture(UpdateInstallContext* c, u32 i, HANDLE h) noexcept {
    u64 bytes = 0;
    if (!file_info(h, &c->file_identities[i], &bytes)) return fail(UpdateInstallError::provenance, ERROR_INVALID_DATA);
    c->known_files[i] = true; return {};
}
UpdateInstallResult preserve_files(UpdateInstallContext* c) noexcept {
    auto r = package_result(update_package_release(&c->package, false), UpdateInstallError::release); if (!r.ok()) return r;
    for (u32 i = 0; i < kUpdateGenerationMaximumFiles; ++i) if (c->files[i]) {
        if (!CloseHandle(c->files[i])) return fail(UpdateInstallError::release, GetLastError());
        if (c->disposition_set[i]) c->disposed_files[i] = true;
        c->files[i] = nullptr; c->created_file_owners[i] = false;
    }
    if (c->seal_owner) { if (!CloseHandle(c->seal_owner)) return fail(UpdateInstallError::release, GetLastError()); c->seal_owner = nullptr; }
    return {};
}
UpdateInstallResult commit(UpdateInstallContext* c, const UpdateGenerationState& state) noexcept {
    return generation(update_generation_commit(c->transaction, state, update_generation_authenticator(&c->authentication)));
}
UpdateInstallResult install(UpdateInstallContext* c, UpdateStage* stage, const UpdatePackageOffer& input,
    const UpdateManifestPolicy& policy, const UpdateAcceptanceTransaction& acceptance,
    const UpdateGenerationIdentity* identity, ConstSlice<HANDLE> extra) noexcept {
    auto r = valid(c); if (!r.ok()) return r;
    if (!stage || extra.count > 4 || (extra.count && !extra.data) || directory_owned(*c) || files_owned(*c) || c->transaction->record_owner ||
        !input.document.data || !input.document.bytes || input.document.bytes > kUpdateManifestMaximumBytes ||
        !input.signature.data || input.signature.count != 64 || c->transaction->record_present != (identity != nullptr) ||
        (!identity && !c->transaction->provisioning_admitted) ||
        (identity && (c->transaction->state.phase != UpdateGenerationPhase::stable || c->transaction->state.previous.present)))
        return fail(UpdateInstallError::state);
    const ConstSlice<u8> owner{reinterpret_cast<const u8*>(c), sizeof(*c)};
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(stage), sizeof(*stage)}, {reinterpret_cast<const u8*>(input.document.data), input.document.bytes}, input.signature,
        {reinterpret_cast<const u8*>(&policy), sizeof(policy)}, {reinterpret_cast<const u8*>(&acceptance), sizeof(acceptance)},
        {reinterpret_cast<const u8*>(identity), identity ? static_cast<u32>(sizeof(*identity)) : 0},
        {reinterpret_cast<const u8*>(extra.data), extra.count * static_cast<u32>(sizeof(HANDLE))}};
    for (auto range : ranges) if (!byte_range_valid(range) || byte_ranges_overlap(owner, range)) return fail(UpdateInstallError::invalid_input);
    if (policy.maximum_artifact_bytes > c->limits.maximum_artifact_bytes || policy.maximum_validity_seconds > c->limits.maximum_validity_seconds ||
        (identity && policy.installed_version < c->transaction->state.active.package.version)) return fail(UpdateInstallError::invalid_input);
    UpdateGenerationDirectoryIdentity acceptance_identity{}; u64 acceptance_lock_bytes = 0;
    if (!file_info(acceptance.lock, &acceptance_identity, &acceptance_lock_bytes) ||
        acceptance_identity.volume != c->transaction->identities[c->transaction->ancestor_count - 1].volume ||
        !stage_outside_root(*c->transaction, acceptance.lock)) return fail(UpdateInstallError::acceptance, ERROR_INVALID_DATA);
    UpdateManifestPolicy captured{};
    const auto admitted = update_acceptance_capture_policy(acceptance, policy, &captured);
    if (!admitted.ok()) return fail(UpdateInstallError::acceptance, admitted.native_error);
    copy_bytes(c->document, input.document.data, input.document.bytes); copy_bytes(c->signature, input.signature.data, 64);
    const UpdatePackageOffer offer{{c->document, input.document.bytes}, {c->signature, 64}};
    UpdateManifest manifest{};
    const auto verified = update_manifest_verify(offer.document, offer.signature, *c->authentication.key, *c->authentication.workspace,
        captured, {c->structural_scratch, 256}, &manifest);
    if (!verified.ok()) { r = fail(UpdateInstallError::offer); r.manifest_error = verified.error; return r; }
    const auto& accepted = acceptance.state.channels[static_cast<u32>(manifest.channel)];
    if (!acceptance.record_present || !accepted.present || accepted.highest_version != manifest.version ||
        !bytes_equal(accepted.manifest_sha256, manifest.manifest_sha256, 32)) return fail(UpdateInstallError::acceptance);
    if (stage->bytes != manifest.artifact_bytes || stage->version != manifest.version ||
        !bytes_equal(stage->manifest_sha256, manifest.manifest_sha256, 32) || !bytes_equal(stage->sha256, manifest.artifact_sha256, 32))
        return fail(UpdateInstallError::stage);
    if (identity) {
        DWORD native = 0;
        const auto auth = update_generation_authenticator(&c->authentication);
        if (!auth.verify_installed(auth.context, c->transaction, c->transaction->state.active.package, &native))
            return fail(UpdateInstallError::enrollment, native);
    }
    r = preflight(c, stage, manifest, extra); if (!r.ok()) return r; const auto budget = r;
    r = generation(update_generation_directory_open(c->transaction, {manifest.manifest_sha256, 32}, true, &c->directory));
    if (!r.ok()) { if (r.generation.error == UpdateGenerationError::state) r.error = UpdateInstallError::remediation; return r; }
    c->authentication.retained_candidate = &c->directory;
    c->extraction = update_package_extract(stage, offer, *c->authentication.key, *c->authentication.workspace, policy, acceptance,
        {c->structural_scratch, 256}, update_generation_package_target(&c->directory), &c->package);
    r = package_result(c->extraction, UpdateInstallError::extraction); if (!r.ok()) return r;
    copy_value(&c->admitted_package, c->package.descriptor);
    for (u32 i = 0; i < c->package.descriptor.member_count; ++i) {
        r = capture(c, kUpdateGenerationFirstMemberSlot + i, c->package.files[i]); if (!r.ok()) return r;
    }
    for (u32 i = 0; i < kUpdateGenerationFirstMemberSlot; ++i) {
        r = generation(update_generation_file_open(&c->directory, update_generation_file_name(c->admitted_package, i), true, &c->files[i]));
        c->created_file_owners[i] = c->files[i] != nullptr;
        if (!r.ok()) return r;
        r = capture(c, i, c->files[i]); if (!r.ok()) return r;
        DWORD native = 0;
        const bool copied = i == kUpdateGenerationOfferSlot ? write_all(c->files[i], c->document, offer.document.bytes, &native) :
            i == kUpdateGenerationSignatureSlot ? write_all(c->files[i], c->signature, 64, &native) : copy_stage(stage, c->files[i], &native);
        if (!copied) return fail(UpdateInstallError::provenance, native);
        r = generation(update_generation_file_seal(&c->directory, &c->files[i], &c->seal_owner));
        if (!r.ok()) return r;
        c->files[i] = c->seal_owner; c->seal_owner = nullptr;
    }
    r = preserve_files(c); if (!r.ok()) return r;
    UpdateGenerationState proposed{};
    if (identity) r = generation(update_generation_prepare(c->transaction->state, *identity, c->admitted_package, &proposed));
    else {
        proposed.installation = c->transaction->installation; proposed.sequence = 1;
        copy_value(&proposed.active.package, c->admitted_package); proposed.active.present = true;
    }
    if (!r.ok()) return r;
    r = commit(c, proposed); if (!r.ok()) return r;
    r = generation(update_generation_directory_release(&c->directory, false)); if (!r.ok()) return r;
    c->authentication.retained_candidate = nullptr; clear_directory(c);
    return budget;
}
UpdateInstallResult dispose_files(UpdateInstallContext* c) noexcept {
    if (c->remediation_required) return fail(UpdateInstallError::remediation, ERROR_INVALID_DATA);
    auto r = generation(update_generation_authentication_release(&c->authentication)); if (!r.ok()) return r;
    const bool durable = durable_directory(*c);
    const UpdateGenerationReference* references[]{&c->transaction->state.active, &c->transaction->state.previous, &c->transaction->state.candidate};
    for (auto* ref : references) if (ref->present && bytes_equal(ref->package.document_sha256, c->directory.document_sha256, 32) && !durable)
        return fail(UpdateInstallError::state);
    if (c->cleanup_authorized && !durable) return fail(UpdateInstallError::state);
    if (durable) {
        r = generation(update_generation_disposal_inventory(&c->directory));
        if (!r.ok()) { if (r.generation.error == UpdateGenerationError::identity) { c->remediation_required = true; r.error = UpdateInstallError::remediation; } return r; }
    }
    r = package_result(update_package_cleanup_release(&c->extraction), UpdateInstallError::cleanup); if (!r.ok()) return r;
    c->disposal_started = true;
    // Created member files remain exact exclusive disposal capabilities, even if
    // capturing their metadata or a later provenance operation failed.
    r = package_result(update_package_release(&c->package, true), UpdateInstallError::cleanup); if (!r.ok()) return r;
    if (c->seal_owner) {
        FILE_DISPOSITION_INFO info{TRUE};
        if (!SetFileInformationByHandle(c->seal_owner, FileDispositionInfo, &info, sizeof(info)) || !CloseHandle(c->seal_owner))
            return fail(UpdateInstallError::cleanup, GetLastError());
        c->seal_owner = nullptr;
    }
    const auto& files = directory_package(*c);
    // Members first: a pinned member stops disposal before any provenance goes.
    for (u32 k = 0; k < kUpdateGenerationMaximumFiles; ++k) {
        const u32 i = (k + kUpdateGenerationFirstMemberSlot) % kUpdateGenerationMaximumFiles;
        if (c->disposed_files[i]) continue;
        if (!c->files[i] && c->known_files[i]) {
            auto g = update_generation_directory_validate(&c->directory); if (!g.ok()) return generation(g);
            wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 n = 0;
            g = update_generation_path(*c->transaction, {c->directory.document_sha256, 32}, update_generation_file_name(files, i),
                {path, sizeof(path) / sizeof(path[0])}, &n); if (!g.ok()) return generation(g);
            c->files[i] = CreateFileW(path, GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (c->files[i] == INVALID_HANDLE_VALUE) {
                const DWORD native = GetLastError(); c->files[i] = nullptr;
                // Package/failed extraction cleanup already disposed exact owners.
                if (native == ERROR_FILE_NOT_FOUND && (c->directory.created || durable)) { c->disposed_files[i] = true; continue; }
                return fail(UpdateInstallError::cleanup, native);
            }
            UpdateGenerationDirectoryIdentity id{}; u64 bytes = 0;
            if (!file_info(c->files[i], &id, &bytes) || id != c->file_identities[i]) {
                // Retain for preserve-close only; never dispose a replacement.
                c->known_files[i] = false; c->remediation_required = true; return fail(UpdateInstallError::remediation, ERROR_INVALID_DATA);
            }
        }
        if (c->files[i]) {
            if (!c->known_files[i] && !c->created_file_owners[i]) {
                c->remediation_required = true; return fail(UpdateInstallError::remediation, ERROR_INVALID_DATA);
            }
            FILE_DISPOSITION_INFO info{TRUE};
            if (!c->disposition_set[i]) {
                if (!SetFileInformationByHandle(c->files[i], FileDispositionInfo, &info, sizeof(info))) return fail(UpdateInstallError::cleanup, GetLastError());
                c->disposition_set[i] = true;
            }
            if (!CloseHandle(c->files[i])) return fail(UpdateInstallError::cleanup, GetLastError());
            c->files[i] = nullptr; c->created_file_owners[i] = false;
        }
        c->disposed_files[i] = true;
    }
    r = generation(update_generation_directory_release(&c->directory, true)); if (!r.ok()) return r;
    c->authentication.retained_candidate = nullptr; clear_directory(c); return {};
}
UpdateInstallResult dispose_reference(UpdateInstallContext* c, const UpdatePackageDescriptor& package) noexcept {
    if (c->remediation_required) return fail(UpdateInstallError::remediation, ERROR_INVALID_DATA);
    const auto target = c->transaction->state.phase == UpdateGenerationPhase::rollback ?
        UpdateGenerationDisposalTarget::candidate : UpdateGenerationDisposalTarget::previous;
    if (c->transaction->state.disposal.target == UpdateGenerationDisposalTarget::none) {
        auto r = update_install_begin_disposal(c, target); if (!r.ok()) return r;
    }
    const auto* ref = disposal_reference(*c);
    if (!ref || !ref->present || !update_package_descriptor_equal(package, ref->package)) return fail(UpdateInstallError::state);
    if (directory_owned(*c)) {
        if (!bytes_equal(c->directory.document_sha256, package.document_sha256, 32)) return fail(UpdateInstallError::state);
        if (!durable_directory(*c)) { c->remediation_required = true; return fail(UpdateInstallError::remediation, ERROR_INVALID_DATA); }
        if (c->disposal_started) return dispose_files(c);
        auto r = preserve_files(c); if (!r.ok()) return r;
    } else {
        bool absent = false;
        auto r = generation(update_generation_disposal_directory_open(c->transaction, &c->directory, &absent));
        if (!r.ok()) { if (r.generation.error == UpdateGenerationError::identity) { c->remediation_required = true; r.error = UpdateInstallError::remediation; } return r; }
        if (absent) return {};
    }
    c->authentication.retained_candidate = &c->directory;
    auto r = generation(update_generation_directory_authorize_disposal(&c->directory, update_generation_authenticator(&c->authentication)));
    if (!r.ok()) return r;
    c->cleanup_authorized = true;
    for (u32 i = 0; i < c->transaction->state.disposal.file_count; ++i) {
        c->file_identities[i] = c->transaction->state.disposal.files[i]; c->known_files[i] = true;
    }
    return dispose_files(c);
}
} // namespace

UpdateInstallResult update_install_initialize(UpdateInstallContext* c, UpdateGenerationTransaction* transaction,
    const P256Key& key, KdfSha256Workspace& workspace, const UpdateInstallLimits& limits) noexcept {
    if (!c || c->initialized || c->transaction || !transaction || !limits.maximum_artifact_bytes ||
        limits.maximum_artifact_bytes > kUpdatePackageMaximumBytes || !limits.maximum_validity_seconds ||
        !limits.maximum_retained_bytes || limits.maximum_retained_bytes > kUpdateInstallMaximumRetainedBytes ||
        limits.free_space_reserve_bytes > kUpdateInstallMaximumRetainedBytes) return fail(UpdateInstallError::invalid_input);
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(c), sizeof(*c)}, {reinterpret_cast<const u8*>(transaction), sizeof(*transaction)},
        {reinterpret_cast<const u8*>(&key), sizeof(key)}, {reinterpret_cast<const u8*>(&workspace), sizeof(workspace)},
        {workspace.storage.data, workspace.storage.capacity}, {reinterpret_cast<const u8*>(&limits), sizeof(limits)}};
    if (!byte_ranges_all_disjoint({ranges, sizeof(ranges) / sizeof(ranges[0])})) return fail(UpdateInstallError::invalid_input);
    auto r = generation(update_generation_validate(transaction)); if (!r.ok()) return r;
    u8 fingerprint[32]{};
    if (!update_generation_inventory_fingerprint(key, workspace, {fingerprint, 32}).ok() ||
        !bytes_equal(fingerprint, transaction->publisher_inventory_sha256, 32)) return fail(UpdateInstallError::enrollment);
    c->transaction = transaction; c->limits = limits; c->authentication.key = &key; c->authentication.workspace = &workspace;
    c->authentication.maximum_artifact_bytes = limits.maximum_artifact_bytes; c->authentication.maximum_validity_seconds = limits.maximum_validity_seconds;
    c->initialized = true; return {};
}
UpdateInstallResult update_install_prepare(UpdateInstallContext* c, UpdateStage* stage, const UpdatePackageOffer& offer,
    const UpdateManifestPolicy& policy, const UpdateAcceptanceTransaction& acceptance, const UpdateGenerationIdentity& identity,
    ConstSlice<HANDLE> extra) noexcept { return install(c, stage, offer, policy, acceptance, &identity, extra); }
UpdateInstallResult update_install_provision_current(UpdateInstallContext* c, UpdateStage* stage, const UpdatePackageOffer& offer,
    const UpdateManifestPolicy& policy, const UpdateAcceptanceTransaction& acceptance, ConstSlice<HANDLE> extra) noexcept {
    return install(c, stage, offer, policy, acceptance, nullptr, extra);
}
UpdateInstallResult update_install_activate(UpdateInstallContext* c) noexcept {
    auto r = valid(c); if (!r.ok()) return r;
    if (directory_owned(*c) || files_owned(*c)) return fail(UpdateInstallError::state);
    UpdateGenerationState proposed{}; r = generation(update_generation_activate(c->transaction->state, &proposed)); if (!r.ok()) return r;
    r = commit(c, proposed); if (!r.ok()) return r;
    c->activation_lease = true; c->activation_transaction = proposed.transaction; c->activation_sequence = proposed.sequence;
    copy_bytes(c->activation_document_sha256, proposed.active.package.document_sha256, 32); return {};
}
UpdateInstallResult update_install_acknowledge(UpdateInstallContext* c, const UpdateGenerationIdentity& transaction, ConstSlice<u8> digest) noexcept {
    auto r = valid(c); if (!r.ok()) return r; UpdateGenerationState proposed{};
    r = generation(update_generation_acknowledge(c->transaction->state, transaction, digest, &proposed)); if (!r.ok()) return r;
    r = commit(c, proposed); if (r.ok()) clear_activation(c); return r;
}
UpdateInstallResult update_install_begin_disposal(UpdateInstallContext* c, UpdateGenerationDisposalTarget target) noexcept {
    auto r = valid(c); if (!r.ok()) return r;
    const auto& state = c->transaction->state;
    if (target == UpdateGenerationDisposalTarget::none || target > UpdateGenerationDisposalTarget::candidate) return fail(UpdateInstallError::invalid_input);
    if (state.disposal.target != UpdateGenerationDisposalTarget::none)
        return state.disposal.target == target ? UpdateInstallResult{} : fail(UpdateInstallError::state);
    const auto* ref = target == UpdateGenerationDisposalTarget::previous ? &state.previous : &state.candidate;
    if (!ref->present || !((target == UpdateGenerationDisposalTarget::previous && state.phase == UpdateGenerationPhase::stable) ||
        (target == UpdateGenerationDisposalTarget::candidate && state.phase == UpdateGenerationPhase::rollback)) || c->remediation_required || c->disposal_started)
        return fail(UpdateInstallError::state);
    r = generation(update_generation_cleanup_record(c->transaction)); if (!r.ok()) return r;
    r = generation(update_generation_authentication_release(&c->authentication)); if (!r.ok()) return r;
    r = preserve_files(c); if (!r.ok()) return r;
    if (directory_owned(*c)) {
        if (!bytes_equal(c->directory.document_sha256, ref->package.document_sha256, 32)) return fail(UpdateInstallError::state);
        r = generation(update_generation_directory_validate(&c->directory)); if (!r.ok()) return r;
    } else {
        r = generation(update_generation_directory_open(c->transaction, {ref->package.document_sha256, 32}, false, &c->directory)); if (!r.ok()) return r;
    }
    c->authentication.retained_candidate = &c->directory;
    // Capture under read pins, then let the journal commit independently verify
    // publisher provenance and these exact identities before minting authority.
    UpdateGenerationDisposalIntent intent{}; intent.target = target; intent.directory = c->directory.identity;
    intent.file_count = update_generation_file_count(ref->package);
    for (u32 i = 0; i < intent.file_count; ++i) {
        r = generation(update_generation_file_open(&c->directory, update_generation_file_name(ref->package, i), false, &c->files[i]));
        if (!r.ok()) return r;
        UpdateGenerationDirectoryIdentity id{}; u64 bytes = 0;
        if (!file_info(c->files[i], &id, &bytes) || (c->known_files[i] && id != c->file_identities[i])) {
            c->remediation_required = true; return fail(UpdateInstallError::remediation, ERROR_INVALID_DATA);
        }
        intent.files[i] = id; c->file_identities[i] = id; c->known_files[i] = true;
    }
    UpdateGenerationState proposed{};
    r = generation(update_generation_disposal_begin(state, intent, &proposed)); if (!r.ok()) return r;
    r = commit(c, proposed); if (!r.ok()) return r;
    return preserve_files(c);
}
UpdateInstallResult update_install_cleanup(UpdateInstallContext* c) noexcept {
    auto r = valid(c); if (!r.ok()) return r;
    r = generation(update_generation_cleanup_record(c->transaction)); if (!r.ok()) return r;
    if (const auto* ref = disposal_reference(*c)) return dispose_reference(c, ref->package);
    if (!directory_owned(*c)) return {};
    const auto& s = c->transaction->state;
    if ((s.active.present && bytes_equal(s.active.package.document_sha256, c->directory.document_sha256, 32)) ||
        (s.candidate.present && s.phase != UpdateGenerationPhase::rollback && bytes_equal(s.candidate.package.document_sha256, c->directory.document_sha256, 32)))
        return fail(UpdateInstallError::state);
    if (s.previous.present && bytes_equal(s.previous.package.document_sha256, c->directory.document_sha256, 32)) return dispose_reference(c, s.previous.package);
    if (s.candidate.present && bytes_equal(s.candidate.package.document_sha256, c->directory.document_sha256, 32)) return dispose_reference(c, s.candidate.package);
    return dispose_files(c);
}
UpdateInstallResult update_install_recover(UpdateInstallContext* c) noexcept {
    auto r = valid(c); if (!r.ok()) return r; clear_activation(c);
    if (c->transaction->record_owner) { r = generation(update_generation_cleanup_record(c->transaction)); if (!r.ok()) return r; }
    r = generation(update_generation_recover_temporary(c->transaction, update_generation_authenticator(&c->authentication)));
    if (!r.ok()) return r;
    UpdateGenerationRecovery plan{}; r = generation(update_generation_recovery(c->transaction->state, &plan)); if (!r.ok()) return r;
    UpdateGenerationState proposed{};
    if (plan == UpdateGenerationRecovery::roll_back) {
        r = generation(update_generation_rollback(c->transaction->state, &proposed)); if (!r.ok()) return r;
        r = commit(c, proposed); if (!r.ok()) return r; plan = UpdateGenerationRecovery::discard_candidate;
    }
    if (plan == UpdateGenerationRecovery::discard_candidate) {
        r = dispose_reference(c, c->transaction->state.candidate.package); if (!r.ok()) return r;
        r = generation(update_generation_finish(c->transaction->state, true, &proposed)); if (!r.ok()) return r; return commit(c, proposed);
    }
    if (plan == UpdateGenerationRecovery::finish_acknowledgement) {
        r = generation(update_generation_finish(c->transaction->state, true, &proposed)); if (!r.ok()) return r; return commit(c, proposed);
    }
    if (plan == UpdateGenerationRecovery::discard_previous) return update_install_forget_previous(c);
    return {};
}
UpdateInstallResult update_install_cancel(UpdateInstallContext* c) noexcept {
    auto r = valid(c); if (!r.ok()) return r; clear_activation(c); UpdateGenerationState proposed{};
    r = generation(update_generation_rollback(c->transaction->state, &proposed)); if (!r.ok()) return r;
    r = commit(c, proposed); if (!r.ok()) return r; return update_install_recover(c);
}
UpdateInstallResult update_install_forget_previous(UpdateInstallContext* c) noexcept {
    auto r = valid(c); if (!r.ok()) return r;
    if (c->transaction->state.phase != UpdateGenerationPhase::stable || !c->transaction->state.previous.present) return fail(UpdateInstallError::state);
    r = generation(update_generation_cleanup_record(c->transaction)); if (!r.ok()) return r;
    r = dispose_reference(c, c->transaction->state.previous.package); if (!r.ok()) return r;
    UpdateGenerationState proposed{}; r = generation(update_generation_forget_previous(c->transaction->state, true, &proposed));
    return r.ok() ? commit(c, proposed) : r;
}
UpdateInstallResult update_install_revert(UpdateInstallContext* c) noexcept {
    auto r = valid(c); if (!r.ok()) return r;
    if (directory_owned(*c) || files_owned(*c)) return fail(UpdateInstallError::state);
    UpdateGenerationState proposed{}; r = generation(update_generation_revert(c->transaction->state, &proposed));
    return r.ok() ? commit(c, proposed) : r;
}
UpdateInstallResult update_install_select_package(UpdateInstallContext* c, UpdatePackage* output) noexcept {
    if (!output_disjoint(c, output, sizeof(*output))) return fail(UpdateInstallError::invalid_input);
    auto r = valid(c); return r.ok() ? generation(update_generation_select_package(c->transaction, &c->authentication, output)) : r;
}
UpdateInstallResult update_install_select_activation_package(UpdateInstallContext* c, UpdatePackage* output) noexcept {
    if (!output_disjoint(c, output, sizeof(*output))) return fail(UpdateInstallError::invalid_input);
    auto r = valid(c); if (!r.ok()) return r; if (!c->activation_lease) return fail(UpdateInstallError::state);
    UpdateGenerationLaunchAdmission admission{}; admission.transaction = c->activation_transaction; admission.sequence = c->activation_sequence;
    copy_bytes(admission.document_sha256, c->activation_document_sha256, 32);
    r = generation(update_generation_select_pending_package_for_launch(c->transaction, &c->authentication, admission, output));
    if (r.ok()) c->activation_lease = false; return r;
}
UpdateInstallResult update_install_activation_admission(const UpdateInstallContext* c, UpdateGenerationLaunchAdmission* output) noexcept {
    if (!c || !c->initialized || !c->transaction || !c->activation_sequence || !output_disjoint(c, output, sizeof(*output)))
        return fail(UpdateInstallError::invalid_input);
    const auto& s = c->transaction->state;
    if (s.phase != UpdateGenerationPhase::pending || s.sequence != c->activation_sequence ||
        !bytes_equal(s.transaction.bytes, c->activation_transaction.bytes, 16) ||
        !bytes_equal(s.active.package.document_sha256, c->activation_document_sha256, 32)) return fail(UpdateInstallError::state);
    UpdateGenerationLaunchAdmission admission{}; admission.transaction = c->activation_transaction;
    admission.sequence = c->activation_sequence; copy_bytes(admission.document_sha256, c->activation_document_sha256, 32);
    *output = admission; return {};
}
UpdateInstallResult update_install_release(UpdateInstallContext* c) noexcept {
    if (!c) return fail(UpdateInstallError::invalid_input);
    if (!c->initialized) return {};
    clear_activation(c);
    auto r = generation(update_generation_authentication_release(&c->authentication)); if (!r.ok()) return r;
    if (c->remediation_required) {
        r = preserve_files(c); if (!r.ok()) return r;
        r = generation(update_generation_directory_release(&c->directory, false)); if (!r.ok()) return r;
        SecureZeroMemory(c, sizeof(*c)); return fail(UpdateInstallError::remediation, ERROR_INVALID_DATA);
    }
    if (directory_owned(*c)) {
        const auto& s = c->transaction->state;
        const bool committed = (s.active.present && bytes_equal(s.active.package.document_sha256, c->directory.document_sha256, 32)) ||
            (s.previous.present && bytes_equal(s.previous.package.document_sha256, c->directory.document_sha256, 32)) ||
            (s.candidate.present && bytes_equal(s.candidate.package.document_sha256, c->directory.document_sha256, 32));
        if (!committed) { r = update_install_cleanup(c); if (!r.ok()) return r; }
        else { r = preserve_files(c); if (!r.ok()) return r;
            r = generation(update_generation_directory_release(&c->directory, false)); if (!r.ok()) return r; }
    }
    SecureZeroMemory(c, sizeof(*c)); return {};
}
} // namespace maicord
