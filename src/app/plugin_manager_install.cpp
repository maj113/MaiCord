#include "plugin_manager_common.hpp"
#include "maicord/app/plugin_manager_install.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using namespace plugin_manager_detail;
using ME = PluginManagerError;
using Op = PluginJournalOperation;
using Phase = PluginJournalPhase;
using Action = PluginRecoveryAction;
using Path = wchar_t[kPluginManagerPathCapacity];
constexpr wchar_t kPreviousSuffix[] = L".previous.json"; // staged next rollback snapshot
constexpr wchar_t kDownloadSuffix[] = L".download";

bool exists(const wchar_t* path) noexcept { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
// "<owner16>-<sha64>.dll", the installer's managed DLL name.
void managed_leaf(u64 owner, const u8* sha256, wchar_t (&leaf)[86]) noexcept {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    for (u32 i = 0; i < 16; ++i) leaf[i] = digits[(owner >> ((15 - i) * 4)) & 15];
    leaf[16] = L'-';
    for (u32 i = 0; i < 32; ++i) { leaf[17 + i * 2] = digits[sha256[i] >> 4]; leaf[18 + i * 2] = digits[sha256[i] & 15]; }
    leaf[81] = L'.'; leaf[82] = L'd'; leaf[83] = L'l'; leaf[84] = L'l'; leaf[85] = 0;
}
u32 join(const wchar_t* directory, u32 count, const wchar_t* leaf, Path& output) noexcept {
    u32 length = 0;
    if (!count || count >= kPluginManagerPathCapacity) return 0;
    copy_bytes(output, directory, count * static_cast<u32>(sizeof(wchar_t)));
    length = count;
    output[length] = 0;
    return wide_append(output, kPluginManagerPathCapacity, &length, L"\\") && wide_append(output, kPluginManagerPathCapacity, &length, leaf)
        ? length : 0;
}
u32 append(Path& path, u32 count, const wchar_t* suffix) noexcept {
    u32 length = count;
    return wide_append(path, kPluginManagerPathCapacity, &length, suffix) ? length : 0;
}
bool write_file(const wchar_t* path, const void* data, u32 bytes, u32* native) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { *native = GetLastError(); return false; }
    DWORD written = 0;
    bool ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
    if (!ok) *native = GetLastError();
    ok = CloseHandle(file) && ok;
    if (!ok) DeleteFileW(path);
    return ok;
}
// A whole small file (at most capacity - 1 bytes).
bool read_file(const wchar_t* path, char* output, u32 capacity, u32* bytes) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const bool ok = acceptance_record_disk_file(file).ok() && ReadFile(file, output, capacity, &got, nullptr) && got < capacity;
    CloseHandle(file);
    *bytes = got;
    return ok;
}
// Staging belongs to the plugin manager: every regular file directly in it goes.
u32 empty_staging(const PluginManagerPaths& paths) noexcept {
    Path pattern{};
    if (!join(paths.staging, paths.staging_count, L"*", pattern)) return 0;
    WIN32_FIND_DATAW found{};
    const HANDLE scan = FindFirstFileW(pattern, &found);
    if (scan == INVALID_HANDLE_VALUE) return 0;
    u32 removed = 0, visited = 0;
    do {
        if (++visited > 256) break;
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        Path path{};
        if (join(paths.staging, paths.staging_count, found.cFileName, path) && DeleteFileW(path)) ++removed;
    } while (FindNextFileW(scan, &found));
    FindClose(scan);
    return removed;
}
// AMD64 PE image with the DLL characteristic and a PE32+ optional header.
bool amd64_dll(const u8* data, u32 bytes) noexcept {
    if (bytes < 64 || data[0] != 'M' || data[1] != 'Z') return false;
    const u32 pe = byte_load_le32(data + 0x3C);
    if (pe < 64 || pe > bytes - 26 || !bytes_equal(data + pe, "PE\0\0", 4)) return false;
    const u16 machine = byte_load_le16(data + pe + 4), optional = byte_load_le16(data + pe + 20),
        characteristics = byte_load_le16(data + pe + 22), magic = byte_load_le16(data + pe + 24);
    return machine == 0x8664 && optional >= 2 && (characteristics & 0x2002) == 0x2002 && magic == 0x20B;
}
// Re-hashes a staged file through a pinned handle (the loader's walker); a
// nonzero bytes must match exactly, and image checks the PE header.
ME verify_staged(const PluginManagerPaths& paths, const wchar_t* leaf, const u8* sha256, u64 bytes, bool image) noexcept {
    NativePluginVerifiedFile directory{}, file{};
    ME error = ME::none;
    if (!native_plugin_directory_pin({paths.staging, paths.staging_count}, &directory).ok() ||
        !native_plugin_pinned_file_open(directory, {leaf, wide_length(leaf, 128)}, &file).ok()) error = ME::storage;
    if (error == ME::none && ((bytes && file.bytes != bytes) || !native_plugin_verified_file_hash(&file, {sha256, 32}).ok()))
        error = ME::integrity;
    if (error == ME::none && image) {
        u8 header[4096];
        DWORD got = 0;
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(file.file, start, nullptr, FILE_BEGIN) ||
            !ReadFile(file.file, header, static_cast<DWORD>(minimum<u64>(sizeof(header), file.bytes)), &got, nullptr) || !amd64_dll(header, got))
            error = ME::image;
    }
    native_plugin_verified_file_release(&file);
    native_plugin_verified_file_release(&directory);
    return error;
}
struct DownloadSink {
    HANDLE file;
};
bool download_sink(void* context, ConstSlice<u8> bytes) noexcept {
    DWORD written = 0;
    return WriteFile(static_cast<DownloadSink*>(context)->file, bytes.data, bytes.count, &written, nullptr) && written == bytes.count;
}

// ---- records -----------------------------------------------------------------------
AcceptanceRecordResult load_journal(const PluginManagerPaths& paths, PluginJournal* journal) noexcept {
    reset_value(journal);
    const auto loaded = plugin_journal_load({paths.journal, paths.journal_count}, journal);
    return loaded.error == AcceptanceRecordError::missing ? AcceptanceRecordResult{} : loaded;
}
AcceptanceRecordResult save_journal(const PluginManagerPaths& paths, const PluginJournal& journal) noexcept {
    return plugin_journal_save({paths.journal, paths.journal_count}, journal);
}
PluginManagerResult trust_failure(NativePluginPreferencesResult result) noexcept {
    PluginManagerResult r = failure(ME::trust);
    r.native_error = result.win32_error;
    return r;
}

// The owner's manifest in plugins\: absent when the directory or the manifest
// is missing; inventory when the scan fails or the owner's entry is invalid.
enum class Installed : u8 { absent, present, error };
Installed installed_manifest(const PluginManagerPaths& paths, u64 owner, PluginManagerWork* work, NativePluginManifest* manifest) noexcept {
    clear_bytes(&work->catalog, sizeof(work->catalog));
    auto scanned = native_plugin_catalog_initialize(&work->catalog, {paths.plugins, paths.plugins_count},
        {work->entries, kNativePluginCatalogMaximumEntries});
    if (scanned.ok()) scanned = native_plugin_catalog_refresh(&work->catalog);
    Installed result = Installed::absent;
    if (!scanned.ok())
        result = scanned.win32_error == ERROR_FILE_NOT_FOUND || scanned.win32_error == ERROR_PATH_NOT_FOUND ? Installed::absent : Installed::error;
    for (u32 i = 0; scanned.ok() && i < work->catalog.count; ++i) {
        const NativePluginCatalogEntry& entry = work->entries[i];
        if (entry.manifest.owner_id != owner) continue;
        if (entry.status != NativePluginCatalogError::none || result == Installed::present) { result = Installed::error; break; }
        copy_value(manifest, entry.manifest);
        result = Installed::present;
    }
    if (!native_plugin_catalog_release(&work->catalog).ok()) result = Installed::error;
    return result;
}

void log_operation(const char* event, const PluginJournal& j, u32 error) noexcept {
    UpdateLog("plugins", event).field("op", static_cast<u8>(j.operation)).field("phase", static_cast<u8>(j.phase))
        .hex("owner", j.owner).field("to", j.to_code).field("error", error).write(error ? LogLevel::warning : LogLevel::info);
}
// A failed prepare leaves no operation behind.
PluginManagerResult abandon(const PluginManagerPaths& paths, PluginManagerResult r, const PluginJournal& journal) noexcept {
    empty_staging(paths);
    if (journal.operation != Op::none) {
        const auto cleared = save_journal(paths, PluginJournal{});
        if (!cleared.ok() && r.error == ME::none) r = record_failure(cleared);
    }
    log_operation("prepare_failed", journal, static_cast<u32>(r.error));
    return r;
}
PluginManagerResult begin(const PluginManagerPaths& paths) noexcept {
    u32 native = 0;
    if (!plugin_manager_paths_ensure(paths, &native)) { auto r = failure(ME::storage); r.native_error = native; return r; }
    PluginJournal journal{};
    const auto loaded = load_journal(paths, &journal);
    if (!loaded.ok()) return record_failure(loaded);
    return failure(journal.operation == Op::none ? ME::none : ME::busy);
}
// Writes the canonical manifest record to <staging>\<owner16><suffix>.
ME stage_manifest(const PluginManagerPaths& paths, const NativePluginManifest& manifest, const wchar_t* suffix, PluginManagerWork* work,
    u32* native) noexcept {
    Path path{};
    u32 bytes = 0;
    if (!native_plugin_manifest_write(manifest, {work->record, sizeof(work->record)}, &bytes).ok()) return ME::manifest;
    if (!plugin_manager_id_path(paths.staging, paths.staging_count, manifest.owner_id, suffix, path)) return ME::storage;
    return write_file(path, work->record, bytes, native) ? ME::none : ME::storage;
}
bool prepared_manifest_path(const PluginManagerPaths& paths, u64 owner, PluginManagerPrepared* prepared) noexcept {
    prepared->manifest_count = plugin_manager_id_path(paths.staging, paths.staging_count, owner,
        kNativePluginManifestSuffix, prepared->manifest);
    return prepared->manifest_count != 0;
}

// ---- settling --------------------------------------------------------------------------
NativePluginBundleRecoveryResult trim(const PluginManagerPaths& paths, u64 owner, const u8* keep) noexcept {
    NativePluginBundleKeep kept{};
    if (keep && !bytes_zero(keep, 32)) { copy_bytes(kept.sha256[0], keep, 32); kept.count = 1; }
    if (!exists(paths.plugins)) return {};
    return native_plugin_bundle_recover(owner, {paths.plugins, paths.plugins_count}, kept);
}
PluginManagerResult clear_operation(const PluginManagerPaths& paths, PluginManagerSettled* out) noexcept {
    const auto cleared = save_journal(paths, PluginJournal{});
    out->staging_removed = empty_staging(paths);
    return cleared.ok() ? failure(ME::none) : record_failure(cleared);
}
PluginManagerResult committed(const PluginManagerPaths& paths, const PluginJournal& journal) noexcept {
    if (journal.phase == Phase::committed) return failure(ME::none);
    PluginJournal next = journal;
    next.phase = Phase::committed;
    const auto saved = save_journal(paths, next);
    return saved.ok() ? failure(ME::none) : record_failure(saved);
}
// Install, update or rollback whose manifest now names the new build.
PluginManagerResult settle_commit(const PluginManagerPaths& paths, const PluginJournal& journal, const NativePluginManifest& manifest,
    const PluginManagerFacts* facts, PluginManagerWork* work, PluginManagerSettled* out) noexcept {
    auto r = committed(paths, journal);
    if (!r.ok()) return r;
    // Trust carry-over keeps off flags.
    const auto trust_loaded = load_trust(paths, &work->trust);
    if (!trust_loaded.ok()) return trust_failure(trust_loaded);
    if (journal.had_trust || journal.enable_after) {
        NativePluginPreferences next = work->trust;
        const NativePluginPreference* old = trusted_owner(work->trust, journal.owner);
        const u32 flags = old ? old->flags : 0;
        const auto enabled = native_plugin_preferences_enable(&next, manifest, journal.approved_permissions & manifest.permissions);
        if (enabled.ok()) {
            if (flags) native_plugin_preferences_set_flags(&next, journal.owner, flags);
            if (next.count != work->trust.count || !bytes_equal(next.entries, work->trust.entries, sizeof(next.entries))) {
                const auto saved = save_trust(paths, next);
                if (!saved.ok()) return trust_failure(saved);
            }
            out->trusted = true;
        } else if (enabled.error != NativePluginPreferencesError::capacity) {
            return trust_failure(enabled);
        } // capacity: installed but not enabled; eight plugins are already trusted
    } else {
        out->trusted = native_plugin_preferences_find(work->trust, manifest) != nullptr;
    }
    // The rollback snapshot follows the retained previous version.
    Path staged{}, snapshot{};
    if (!plugin_manager_id_path(paths.staging, paths.staging_count, journal.owner, kPreviousSuffix, staged) ||
        !plugin_manager_id_path(paths.rollback, paths.rollback_count, journal.owner, kNativePluginManifestSuffix,
            snapshot)) return failure(ME::storage);
    if (journal.operation == Op::install) DeleteFileW(snapshot);
    else if (exists(staged) && !MoveFileExW(staged, snapshot, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        r = failure(ME::storage); r.native_error = GetLastError(); return r;
    }
    // state.bin: an entry already naming the new build was settled before.
    const auto state_loaded = load_state(paths, &work->state);
    if (!state_loaded.ok()) return record_failure(state_loaded);
    PluginManagerState& state = work->state;
    PluginManagerEntry* old = state_entry(state, journal.owner);
    const u8* previous = nullptr;
    if (!old || !bytes_equal(old->installed_sha256, journal.to_sha256, 32) || old->installed_code != journal.to_code) {
        PluginManagerEntry next{};
        next.owner = journal.owner;
        next.provider = journal.provider;
        next.installed_code = journal.to_code;
        copy_bytes(next.installed_sha256, journal.to_sha256, 32);
        if (old) {
            // Another build: its revocation status comes from the next check.
            next.flags = old->flags & ~(kPluginStateReviewPending | kPluginStateIncompatibleHost | kPluginStateRevokedMalicious |
                kPluginStateRevokedBroken | kPluginStateWithdrawn);
            next.held_code = old->held_code;
            next.max_host_abi = old->max_host_abi;
            next.admitted_sequence = old->admitted_sequence;
            copy_bytes(next.slug, old->slug, kPluginManagerSlugCapacity);
        }
        if (facts && facts->known) {
            copy_bytes(next.slug, facts->slug, kPluginManagerSlugCapacity);
            next.max_host_abi = facts->max_host_abi;
            next.admitted_sequence = facts->admitted_sequence;
        }
        const bool replaced_known = old && bytes_equal(old->installed_sha256, journal.from_sha256, 32) && !bytes_zero(journal.from_sha256, 32);
        if (journal.operation != Op::install && replaced_known) {
            next.previous_code = old->installed_code;
            copy_bytes(next.previous_sha256, journal.from_sha256, 32);
        }
        if (journal.operation == Op::rollback) {
            // The version left is not offered again until a newer one appears.
            next.held_code = old ? old->installed_code : 0;
            if (next.held_code) next.flags |= kPluginStateHeld;
        } else if (next.held_code && next.installed_code > next.held_code) {
            next.held_code = 0;
            next.flags &= ~kPluginStateHeld;
        }
        if (journal.operation == Op::install) { next.held_code = 0; next.flags &= ~kPluginStateHeld; }
        if (old) *old = next;
        else if (state.count < kPluginManagerStateMaximumEntries) state.entries[state.count++] = next;
        else return failure(ME::capacity);
        const auto saved = plugin_manager_state_save({paths.state, paths.state_count}, state);
        if (!saved.ok()) return record_failure(saved);
        old = state_entry(state, journal.owner);
    }
    if (old && old->previous_code) previous = old->previous_sha256;
    // Trim to {installed, previous}.
    const auto trimmed = trim(paths, journal.owner, previous);
    out->removed_dlls = trimmed.removed_dlls;
    out->removed_manifests = trimmed.removed_manifests;
    out->action = Action::finish;
    return clear_operation(paths, out);
}
void remove_data(const PluginManagerPaths& paths, u64 owner, PluginManagerSettled* out) noexcept {
    Path directory{};
    const u32 count = plugin_manager_id_path(paths.data, paths.data_count, owner, L"", directory);
    if (!count || !exists(directory)) { out->data_removed = count != 0; return; }
    Path pattern{};
    if (!join(directory, count, L"*", pattern)) return;
    WIN32_FIND_DATAW found{};
    const HANDLE scan = FindFirstFileW(pattern, &found);
    if (scan != INVALID_HANDLE_VALUE) {
        u32 visited = 0;
        do {
            if (++visited > 256) break;
            if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            Path path{};
            if (join(directory, count, found.cFileName, path)) DeleteFileW(path);
        } while (FindNextFileW(scan, &found));
        FindClose(scan);
    }
    // Directories the plugin never created (subfolders) keep it in place.
    out->data_removed = RemoveDirectoryW(directory) != 0;
}
PluginManagerResult settle_uninstall(const PluginManagerPaths& paths, const PluginJournal& journal, bool delete_data,
    PluginManagerWork* work, PluginManagerSettled* out) noexcept {
    auto r = committed(paths, journal);
    if (!r.ok()) return r;
    const auto trimmed = trim(paths, journal.owner, nullptr);
    out->removed_dlls = trimmed.removed_dlls;
    out->removed_manifests = trimmed.removed_manifests;
    Path snapshot{};
    if (plugin_manager_id_path(paths.rollback, paths.rollback_count, journal.owner, kNativePluginManifestSuffix,
            snapshot))
        DeleteFileW(snapshot);
    const auto state_loaded = load_state(paths, &work->state);
    if (!state_loaded.ok()) return record_failure(state_loaded);
    if (PluginManagerEntry* entry = state_entry(work->state, journal.owner)) {
        PluginManagerState& state = work->state;
        const u32 index = static_cast<u32>(entry - state.entries);
        for (u32 i = index + 1; i < state.count; ++i) state.entries[i - 1] = state.entries[i];
        reset_value(&state.entries[--state.count]);
        const auto saved = plugin_manager_state_save({paths.state, paths.state_count}, state);
        if (!saved.ok()) return record_failure(saved);
    }
    const auto trust_loaded = load_trust(paths, &work->trust);
    if (!trust_loaded.ok()) return trust_failure(trust_loaded);
    if (native_plugin_preferences_remove_owner(&work->trust, journal.owner)) {
        const auto saved = save_trust(paths, work->trust);
        if (!saved.ok()) return trust_failure(saved);
    }
    if (delete_data) remove_data(paths, journal.owner, out);
    out->action = Action::finish_uninstall;
    return clear_operation(paths, out);
}
PluginManagerResult settle(const PluginManagerPaths& paths, const PluginJournal& journal, const PluginManagerFacts* facts, bool delete_data,
    bool recovering, PluginManagerWork* work, PluginManagerSettled* out) noexcept {
    reset_value(out);
    out->operation = journal.operation;
    out->owner = journal.owner;
    if (journal.operation == Op::none) return failure(ME::none);
    NativePluginManifest manifest{};
    const Installed installed = installed_manifest(paths, journal.owner, work, &manifest);
    if (installed == Installed::error) return failure(ME::inventory);
    Action action = plugin_manager_recovery_plan(journal, installed == Installed::present ? manifest.sha256 : nullptr);
    PluginManagerResult r{};
    if (action == Action::restart_uninstall) {
        if (!recovering) {
            // The installer did not commit: the plugin stays, untrusted.
            out->action = Action::discard;
            r = clear_operation(paths, out);
            log_operation("finish", journal, static_cast<u32>(r.error));
            return r;
        }
        NativePluginBundleInstall installer{};
        const auto started = native_plugin_bundle_uninstall_start(&installer, journal.owner, {paths.plugins, paths.plugins_count});
        const auto ran = started == NativePluginBundleInstallError::none ? plugin_manager_run_installer(&installer, kInstallerTimeoutMs)
                                                                          : NativePluginBundleInstallResult{};
        if (!ran.committed) {
            r = failure(ME::installer);
            log_operation("recover_failed", journal, static_cast<u32>(r.error));
            return r;
        }
        action = Action::finish_uninstall;
    }
    switch (action) {
    case Action::discard: {
        const auto state_loaded = load_state(paths, &work->state);
        if (!state_loaded.ok()) return record_failure(state_loaded);
        const PluginManagerEntry* entry = state_entry(work->state, journal.owner);
        const auto trimmed = trim(paths, journal.owner, entry && entry->previous_code ? entry->previous_sha256 : nullptr);
        out->removed_dlls = trimmed.removed_dlls;
        out->removed_manifests = trimmed.removed_manifests;
        out->action = Action::discard;
        r = clear_operation(paths, out);
        break;
    }
    case Action::finish: r = settle_commit(paths, journal, manifest, facts, work, out); break;
    case Action::finish_uninstall: r = settle_uninstall(paths, journal, delete_data, work, out); break;
    default:
        out->action = Action::clear;
        r = clear_operation(paths, out);
        break;
    }
    UpdateLog("plugins", recovering ? "recovered" : "finished").field("action", static_cast<u8>(out->action))
        .field("op", static_cast<u8>(journal.operation)).hex("owner", journal.owner).field("to", journal.to_code)
        .field("removed", out->removed_dlls).field("error", static_cast<u8>(r.error)).write(r.ok() ? LogLevel::info : LogLevel::warning);
    return r;
}

// ---- prepare -------------------------------------------------------------------------------
PluginManagerResult prepare_download(const PluginManagerPaths& paths, const PluginCatalogSource& source, const PluginManagerRequest& request,
    const UpdateTransport& transport, KdfSha256Workspace& workspace, PluginIndexWork* index_work, const PluginIndex* official_cache,
    PluginManagerWork* work, u64 now, PluginManagerPrepared* prepared) noexcept {
    const bool install = request.operation == Op::install;
    const auto cached = plugin_index_load_cache(paths, source, workspace, index_work, now);
    if (!cached.installable) {
        auto r = failure(ME::cache);
        r.index_error = cached.index_error; r.record_error = cached.record_error;
        return r;
    }
    const PluginIndex& index = index_work->index;
    const Utf8View document = plugin_index_work_document(*index_work);
    const PluginIndex* official = source.kind == PluginProviderKind::official ? &index : official_cache;
    u32 plugin = index.plugin_count, version = kPluginIndexNoVersion;
    for (u32 i = 0; i < index.plugin_count && plugin == index.plugin_count; ++i) if (index.plugins[i].owner_id == request.owner) plugin = i;
    if (plugin == index.plugin_count) return failure(ME::not_listed);
    for (u32 i = 0; i < index.plugins[plugin].version_count; ++i)
        if (index.versions[index.plugins[plugin].first_version + i].version_code == request.version_code) version = i;
    if (version == kPluginIndexNoVersion) return failure(ME::not_listed);
    const PluginIndexVersion& listed = index.versions[index.plugins[plugin].first_version + version];
    const PluginHost host = plugin_index_current_host();
    if (plugin_index_compatibility(listed, host) != PluginCompatibility::compatible) return failure(ME::incompatible);
    auto r = failure(ME::none);
    const PluginIndexFile& file = *plugin_index_file(listed, host.arch);
    const auto revocation = plugin_index_revocation(&index, official, request.owner, file.sha256);
    if (revocation.revoked || revocation.blocked) return failure(ME::revoked);
    if (source.kind != PluginProviderKind::official && official && plugin_index_owner_reserved(*official, request.owner))
        return failure(ME::reserved);
    NativePluginManifest current{};
    const Installed installed = installed_manifest(paths, request.owner, work, &current);
    if (installed == Installed::error) return failure(ME::inventory);
    const auto state_loaded = load_state(paths, &work->state);
    if (!state_loaded.ok()) return record_failure(state_loaded);
    const PluginManagerEntry* entry = state_entry(work->state, request.owner);
    if (install) {
        if (installed == Installed::present) return failure(ME::installed);
        if (!entry && work->state.count >= kPluginManagerStateMaximumEntries) return failure(ME::capacity);
    } else {
        if (installed != Installed::present || !entry || !bytes_equal(entry->installed_sha256, current.sha256, 32)) return failure(ME::not_installed);
        if (entry->provider != source.provider) return failure(ME::provider);
        if (entry->installed_code == request.version_code) return failure(ME::current);
    }
    if (entry) {
        // (provider, owner, version code) -> SHA-256 is immutable for the builds held.
        PluginKnownBuild builds[2]{};
        builds[0].version_code = entry->installed_code;
        builds[1].version_code = entry->previous_code;
        copy_bytes(builds[0].sha256, entry->installed_sha256, 32);
        copy_bytes(builds[1].sha256, entry->previous_sha256, 32);
        for (const auto& build : builds) if (plugin_index_build_conflicts(index, plugin, build, host.arch)) return failure(ME::conflict);
    }
    const auto trust_loaded = load_trust(paths, &work->trust);
    if (!trust_loaded.ok()) return trust_failure(trust_loaded);
    const bool had_trust = installed == Installed::present && native_plugin_preferences_find(work->trust, current) != nullptr;
    r.permissions = plugin_index_permission_diff(request.approved_permissions, listed.permissions);
    if ((had_trust || (install && request.enable_after)) && r.permissions.added) { r.error = ME::review; return r; }
    NativePluginManifest manifest{};
    const auto synthesized = plugin_index_manifest(index, document, plugin, version, host.arch, &manifest);
    if (!synthesized.ok()) { r.error = ME::manifest; r.index_error = synthesized.error; return r; }
    UpdateLocation location{};
    if (!plugin_catalog_file_location(source, index, document, plugin, version, host.arch, &location)) return failure(ME::not_listed);
    reset_value(&prepared->facts);
    const auto slug = plugin_index_text_decode(document, index.plugins[plugin].slug, {prepared->facts.slug, kPluginManagerSlugCapacity});
    prepared->facts.known = slug.ok();
    prepared->facts.max_host_abi = listed.max_host_abi;
    prepared->facts.admitted_sequence = index.sequence;
    // Journal the intent before any byte lands in staging.
    PluginJournal journal{};
    journal.operation = request.operation;
    journal.phase = Phase::intent;
    journal.enable_after = install && request.enable_after;
    journal.had_trust = had_trust;
    journal.owner = request.owner;
    journal.provider = source.provider;
    journal.to_code = request.version_code;
    journal.approved_permissions = request.approved_permissions;
    if (installed == Installed::present) copy_bytes(journal.from_sha256, current.sha256, 32);
    copy_bytes(journal.to_sha256, file.sha256, 32);
    const auto saved = save_journal(paths, journal);
    if (!saved.ok()) return record_failure(saved);
    log_operation("prepare", journal, 0);
    empty_staging(paths);
    wchar_t leaf[86];
    managed_leaf(request.owner, file.sha256, leaf);
    Path download{}, staged{};
    const u32 download_count = join(paths.staging, paths.staging_count, leaf, staged);
    if (!download_count || !append(download, 0, staged) || !append(download, download_count, kDownloadSuffix)) return abandon(paths, failure(ME::storage), journal);
    DownloadSink sink{CreateFileW(download, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (sink.file == INVALID_HANDLE_VALUE) { r = failure(ME::storage); r.native_error = GetLastError(); return abandon(paths, r, journal); }
    UpdateTransport plugins = transport;
    plugins.log_category = "plugins";
    const auto fetched = update_fetch(plugins, location, file.bytes, file.bytes, download_sink, &sink, now);
    const bool flushed = FlushFileBuffers(sink.file) != FALSE;
    CloseHandle(sink.file);
    if (!fetched.ok() || !flushed) {
        r = failure(fetched.ok() ? ME::storage : ME::fetch);
        r.fetch_error = fetched.error; r.native_error = fetched.native_error;
        return abandon(paths, r, journal);
    }
    UpdateLog("plugins", "downloaded").hex("owner", request.owner).field("bytes", fetched.bytes).field("redirects", fetched.redirects).write();
    wchar_t download_leaf[96];
    copy_bytes(download_leaf, leaf, 85 * sizeof(wchar_t));
    copy_bytes(download_leaf + 85, kDownloadSuffix, sizeof(kDownloadSuffix));
    const ME verified = verify_staged(paths, download_leaf, file.sha256, file.bytes, true);
    if (verified != ME::none) return abandon(paths, failure(verified), journal);
    if (!MoveFileExW(download, staged, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        r = failure(ME::storage); r.native_error = GetLastError(); return abandon(paths, r, journal);
    }
    u32 native = 0;
    ME staged_error = stage_manifest(paths, manifest, kNativePluginManifestSuffix, work, &native);
    // The replaced manifest becomes the rollback snapshot at commit.
    if (staged_error == ME::none && !install) staged_error = stage_manifest(paths, current, kPreviousSuffix, work, &native);
    if (staged_error != ME::none) { r = failure(staged_error); r.native_error = native; return abandon(paths, r, journal); }
    journal.phase = Phase::staged;
    const auto staged_saved = save_journal(paths, journal);
    if (!staged_saved.ok()) return abandon(paths, record_failure(staged_saved), journal);
    prepared->journal = journal;
    prepared->options.retain_previous = !install;
    if (!prepared_manifest_path(paths, request.owner, prepared)) return abandon(paths, failure(ME::storage), journal);
    log_operation("staged", journal, 0);
    return failure(ME::none);
}
PluginManagerResult prepare_rollback(const PluginManagerPaths& paths, const PluginCatalogSource& source, const PluginManagerRequest& request,
    KdfSha256Workspace& workspace, PluginIndexWork* index_work, const PluginIndex* official_cache, PluginManagerWork* work, u64 now,
    PluginManagerPrepared* prepared) noexcept {
    NativePluginManifest current{};
    const Installed installed = installed_manifest(paths, request.owner, work, &current);
    if (installed == Installed::error) return failure(ME::inventory);
    const auto state_loaded = load_state(paths, &work->state);
    if (!state_loaded.ok()) return record_failure(state_loaded);
    const PluginManagerEntry* found = state_entry(work->state, request.owner);
    if (installed != Installed::present || !found || !bytes_equal(found->installed_sha256, current.sha256, 32)) return failure(ME::not_installed);
    const PluginManagerEntry entry = *found;
    if (entry.provider != source.provider) return failure(ME::provider);
    if (!entry.previous_code) return failure(ME::rollback);
    // The snapshot must describe exactly the retained managed DLL.
    Path snapshot{}, retained{}, staged{};
    wchar_t leaf[86];
    managed_leaf(request.owner, entry.previous_sha256, leaf);
    u32 bytes = 0;
    NativePluginManifest previous{};
    if (!plugin_manager_id_path(paths.rollback, paths.rollback_count, request.owner, kNativePluginManifestSuffix,
            snapshot) ||
        !read_file(snapshot, work->record, sizeof(work->record), &bytes) ||
        !native_plugin_manifest_parse({work->record, bytes}, &previous).ok() || previous.owner_id != request.owner ||
        !bytes_equal(previous.sha256, entry.previous_sha256, 32) || previous.dll_bytes != 85) return failure(ME::rollback);
    for (u32 i = 0; i < 85; ++i) if (previous.dll[i] != static_cast<char>(leaf[i])) return failure(ME::rollback);
    if (!join(paths.plugins, paths.plugins_count, leaf, retained) || !join(paths.staging, paths.staging_count, leaf, staged) || !exists(retained))
        return failure(ME::rollback);
    // A revoked or blocked build is never rolled back to, when a cache says so.
    const auto cached = plugin_index_load_cache(paths, source, workspace, index_work, now);
    if (cached.error == PluginIndexCheckError::none) {
        const PluginIndex* official = source.kind == PluginProviderKind::official ? &index_work->index : official_cache;
        const auto revocation = plugin_index_revocation(&index_work->index, official, request.owner, entry.previous_sha256);
        if (revocation.revoked || revocation.blocked) return failure(ME::revoked);
    }
    const auto trust_loaded = load_trust(paths, &work->trust);
    if (!trust_loaded.ok()) return trust_failure(trust_loaded);
    const bool had_trust = native_plugin_preferences_find(work->trust, current) != nullptr;
    auto r = failure(ME::none);
    r.permissions = plugin_index_permission_diff(request.approved_permissions, previous.permissions);
    if (had_trust && r.permissions.added) { r.error = ME::review; return r; }
    PluginJournal journal{};
    journal.operation = Op::rollback;
    journal.phase = Phase::intent;
    journal.had_trust = had_trust;
    journal.owner = request.owner;
    journal.provider = entry.provider;
    journal.to_code = entry.previous_code;
    journal.approved_permissions = request.approved_permissions;
    copy_bytes(journal.from_sha256, current.sha256, 32);
    copy_bytes(journal.to_sha256, entry.previous_sha256, 32);
    const auto saved = save_journal(paths, journal);
    if (!saved.ok()) return record_failure(saved);
    log_operation("prepare", journal, 0);
    empty_staging(paths);
    // The installer reads its source from staging.
    if (!CopyFileW(retained, staged, FALSE)) { r = failure(ME::storage); r.native_error = GetLastError(); return abandon(paths, r, journal); }
    const ME verified = verify_staged(paths, leaf, entry.previous_sha256, 0, true);
    if (verified != ME::none) return abandon(paths, failure(verified), journal);
    u32 native = 0;
    ME staged_error = stage_manifest(paths, previous, kNativePluginManifestSuffix, work, &native);
    if (staged_error == ME::none) staged_error = stage_manifest(paths, current, kPreviousSuffix, work, &native);
    if (staged_error != ME::none) { r = failure(staged_error); r.native_error = native; return abandon(paths, r, journal); }
    journal.phase = Phase::staged;
    const auto staged_saved = save_journal(paths, journal);
    if (!staged_saved.ok()) return abandon(paths, record_failure(staged_saved), journal);
    prepared->journal = journal;
    prepared->options.retain_previous = true;
    reset_value(&prepared->facts);
    if (!prepared_manifest_path(paths, request.owner, prepared)) return abandon(paths, failure(ME::storage), journal);
    log_operation("staged", journal, 0);
    return failure(ME::none);
}
} // namespace

PluginManagerResult plugin_manager_prepare(const PluginManagerPaths& paths, const PluginCatalogSource& source,
    const PluginManagerRequest& request, const UpdateTransport& transport, KdfSha256Workspace& workspace,
    PluginIndexWork* index_work, const PluginIndex* official, PluginManagerWork* work, u64 now, PluginManagerPrepared* prepared) noexcept {
    if (!index_work || !work || !prepared || !now || request.owner <= 1 || !paths.manager_count ||
        (request.operation != Op::install && request.operation != Op::update && request.operation != Op::rollback) ||
        (request.approved_permissions & ~kNativePluginPermissionsAll) || (request.operation != Op::rollback && !transport.get))
        return failure(ME::invalid_input);
    reset_value(prepared);
    auto r = begin(paths);
    if (!r.ok()) return r;
    return request.operation == Op::rollback
        ? prepare_rollback(paths, source, request, workspace, index_work, official, work, now, prepared)
        : prepare_download(paths, source, request, transport, workspace, index_work, official, work, now, prepared);
}

PluginManagerResult plugin_manager_uninstall_prepare(const PluginManagerPaths& paths, u64 owner, PluginManagerWork* work,
    PluginManagerPrepared* prepared) noexcept {
    if (!work || !prepared || owner <= 1 || !paths.manager_count) return failure(ME::invalid_input);
    reset_value(prepared);
    auto r = begin(paths);
    if (!r.ok()) return r;
    NativePluginManifest current{};
    const Installed installed = installed_manifest(paths, owner, work, &current);
    if (installed == Installed::error) return failure(ME::inventory);
    if (installed != Installed::present) return failure(ME::not_installed);
    const auto state_loaded = load_state(paths, &work->state);
    if (!state_loaded.ok()) return record_failure(state_loaded);
    const PluginManagerEntry* entry = state_entry(work->state, owner);
    const auto trust_loaded = load_trust(paths, &work->trust);
    if (!trust_loaded.ok()) return trust_failure(trust_loaded);
    PluginJournal journal{};
    journal.operation = Op::uninstall;
    journal.phase = Phase::intent;
    journal.owner = owner;
    journal.provider = entry ? entry->provider : kPluginProviderFile;
    journal.had_trust = trusted_owner(work->trust, owner) != nullptr;
    copy_bytes(journal.from_sha256, current.sha256, 32);
    const auto saved = save_journal(paths, journal);
    if (!saved.ok()) return record_failure(saved);
    // Trust goes first: an interrupted uninstall never leaves autoload trust behind.
    if (native_plugin_preferences_remove_owner(&work->trust, owner)) {
        const auto removed = save_trust(paths, work->trust);
        if (!removed.ok()) return trust_failure(removed); // the journal stays; recovery repeats the removal
    }
    prepared->journal = journal;
    log_operation("prepare", journal, 0);
    return failure(ME::none);
}

PluginManagerResult plugin_manager_finish(const PluginManagerPaths& paths, const PluginManagerPrepared& prepared, bool delete_data,
    PluginManagerWork* work, PluginManagerSettled* settled) noexcept {
    if (!work || !settled || !paths.manager_count || prepared.journal.operation == Op::none) return failure(ME::invalid_input);
    PluginJournal journal{};
    const auto loaded = load_journal(paths, &journal);
    if (!loaded.ok()) return record_failure(loaded);
    // Only the journaled operation (a committed phase may already be recorded).
    const PluginJournal& p = prepared.journal;
    const bool same = journal.operation == p.operation && journal.owner == p.owner && journal.provider == p.provider &&
        journal.to_code == p.to_code && journal.approved_permissions == p.approved_permissions && journal.enable_after == p.enable_after &&
        journal.had_trust == p.had_trust && bytes_equal(journal.from_sha256, p.from_sha256, 32) && bytes_equal(journal.to_sha256, p.to_sha256, 32);
    if (journal.operation == Op::none || !same) return failure(ME::busy);
    return settle(paths, journal, &prepared.facts, delete_data, false, work, settled);
}

PluginManagerResult plugin_manager_recover(const PluginManagerPaths& paths, PluginManagerWork* work, PluginManagerSettled* settled) noexcept {
    if (!work || !settled || !paths.manager_count) return failure(ME::invalid_input);
    reset_value(settled);
    PluginJournal journal{};
    const auto loaded = load_journal(paths, &journal);
    if (!loaded.ok()) return record_failure(loaded);
    return settle(paths, journal, nullptr, false, true, work, settled);
}

NativePluginBundleInstallResult plugin_manager_run_installer(NativePluginBundleInstall* installer, u32 timeout_ms) noexcept {
    using Stage = NativePluginBundleInstallPhase;
    NativePluginBundleInstallResult result{};
    result.error = NativePluginBundleInstallError::wait;
    if (!installer || !installer->runtime) { result.error = NativePluginBundleInstallError::invalid_input; return result; }
    const u64 deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        const auto status = native_plugin_bundle_install_status(installer);
        if (status.phase == Stage::complete) { result = status; break; }
        if (status.phase == Stage::prepared && native_plugin_bundle_install_commit(installer) != NativePluginBundleInstallError::none) {
            native_plugin_bundle_install_cancel(installer);
            continue;
        }
        const u64 now = GetTickCount64();
        if (now >= deadline || WaitForSingleObject(installer->notification, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0) {
            native_plugin_bundle_install_cancel(installer);
            break;
        }
        native_plugin_bundle_install_acknowledge(installer);
    }
    if (result.phase == Stage::complete && result.cleanup_pending &&
        native_plugin_bundle_install_retry_cleanup(installer) == NativePluginBundleInstallError::none) {
        WaitForSingleObject(installer->thread, timeout_ms);
        result = native_plugin_bundle_install_status(installer);
    }
    if (!native_plugin_bundle_install_release(installer, timeout_ms) && result.ok()) result.error = NativePluginBundleInstallError::release;
    return result;
}

} // namespace maicord
