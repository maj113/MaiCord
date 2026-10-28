#include "maicord/app/update_service.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/data_path.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using SE = UpdateServiceError;

bool join(wchar_t* out, const wchar_t* base, u32 base_count, const wchar_t* leaf, u32* count = nullptr) noexcept {
    const u32 leaf_count = wide_length(leaf, kUpdatePathCapacity);
    if (base_count + 1 + leaf_count >= kUpdatePathCapacity) return false;
    copy_bytes(out, base, base_count * sizeof(wchar_t)); out[base_count] = L'\\';
    copy_bytes(out + base_count + 1, leaf, (leaf_count + 1) * sizeof(wchar_t));
    if (count) *count = base_count + 1 + leaf_count;
    return true;
}
// <path><suffix>, NUL-terminated.
bool suffixed(wchar_t* out, ConstSlice<wchar_t> path, const wchar_t* suffix, u32* count = nullptr) noexcept {
    const u32 suffix_count = wide_length(suffix, kUpdatePathCapacity);
    if (path.count + suffix_count >= kUpdatePathCapacity) return false;
    copy_bytes(out, path.data, path.count * sizeof(wchar_t));
    copy_bytes(out + path.count, suffix, (suffix_count + 1) * sizeof(wchar_t));
    if (count) *count = path.count + suffix_count;
    return true;
}
// An absolute drive path: "X:\..." with no trailing separator, forward
// slash, control character or later colon.
bool drive_path(ConstSlice<wchar_t> path) noexcept {
    if (!path.data || path.count < 4 || path.count >= kUpdatePathCapacity || path[1] != L':' || path[2] != L'\\' ||
        path[path.count - 1] == L'\\' || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
        return false;
    for (u32 i = 0; i < path.count; ++i) if (!path[i] || path[i] == L'/' || path[i] < 32 || (i > 1 && path[i] == L':')) return false;
    return true;
}
bool exists(const wchar_t* path) noexcept { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
bool write_exact(HANDLE h, const void* data, u32 bytes) noexcept {
    u32 done = 0;
    while (done < bytes) {
        DWORD put = 0;
        if (!WriteFile(h, static_cast<const u8*>(data) + done, bytes - done, &put, nullptr) || !put || put > bytes - done) return false;
        done += put;
    }
    return true;
}
HANDLE open_read(const wchar_t* path) noexcept {
    const HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}
bool regular_size(HANDLE h, u64* bytes) noexcept {
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    if (!h || !GetFileInformationByHandle(h, &info) || !GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || info.nNumberOfLinks != 1) return false;
    *bytes = static_cast<u64>(size.QuadPart); return true;
}
void close(HANDLE* h) noexcept { if (*h) { CloseHandle(*h); *h = nullptr; } }
// Creates base and work (work is base\work).
bool ensure_storage(const UpdatePaths& paths) noexcept {
    wchar_t partial[kUpdatePathCapacity]{};
    copy_bytes(partial, paths.work, (paths.work_count + 1) * sizeof(wchar_t));
    for (u32 i = 3; i <= paths.work_count; ++i) if (i == paths.work_count || partial[i] == L'\\') {
        const wchar_t saved = partial[i]; partial[i] = 0;
        if (!CreateDirectoryW(partial, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        partial[i] = saved;
    }
    return true;
}
bool file_sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto* s = static_cast<UpdateService*>(context);
    if (!write_exact(s->download, bytes.data, bytes.count)) return false;
    InterlockedExchangeAdd64(&s->progress.received, bytes.count);
    return true;
}
SE fetch_error(UpdateFetchError e) noexcept {
    switch (e) {
    case UpdateFetchError::rate_limited: return SE::rate_limited;
    case UpdateFetchError::not_found: return SE::not_found;
    case UpdateFetchError::redirect: return SE::redirect;
    case UpdateFetchError::oversize: return SE::oversize;
    case UpdateFetchError::size: return SE::size;
    default: return SE::transport;
    }
}
SE manifest_error(UpdateManifestError e) noexcept {
    return e == UpdateManifestError::signature ? SE::signature : e == UpdateManifestError::time ? SE::expired : SE::manifest;
}
UpdateCheckResult failed(SE error) noexcept { UpdateCheckResult r{}; r.error = error; r.outcome = UpdateCheckOutcome::failed; return r; }
UpdateCheckResult outcome(UpdateCheckOutcome o, u64 version) noexcept { UpdateCheckResult r{}; r.outcome = o; r.version = version; return r; }
UpdateCheckResult manifest_failed(SE error, UpdateManifestError manifest) noexcept { auto r = failed(error); r.manifest_error = manifest; return r; }
UpdateCheckResult fetch_failed(const UpdateFetchResult& fetched) noexcept {
    auto r = failed(fetch_error(fetched.error));
    r.fetch_error = fetched.error; r.retry_at_unix = fetched.retry_at_unix; r.native_error = fetched.native_error;
    return r;
}
// One summary line per failed check, after the phase's own detail line.
UpdateCheckResult report(UpdateCheckResult r, const char* phase) noexcept {
    UpdateLog line("check_failed");
    line.add(" phase=").add(phase);
    line.field("error", static_cast<u8>(r.error)).field("fetch", static_cast<u8>(r.fetch_error))
        .field("manifest", static_cast<u8>(r.manifest_error)).field("native", r.native_error);
    if (r.retry_at_unix) line.field("retry_at", r.retry_at_unix);
    line.write(LogLevel::warning);
    return r;
}
// Nothing newer to install: a package an earlier session kept is stale.
UpdateCheckResult up_to_date(const UpdateService* s, u64 floor, u64 offered) noexcept {
    UpdateLog line("up_to_date");
    line.field("floor", floor).field("offered", offered);
    if (DeleteFileW(s->paths.download)) line.field("dropped_package", 1);
    line.write();
    return outcome(UpdateCheckOutcome::up_to_date, floor);
}
// Keeps the package file: a failed check deletes it, a staged one keeps it
// until the update is installed or superseded.
void release_work(UpdateService* s) noexcept {
    update_preparation_release(&s->preparation);
    clear_bytes(&s->preparation, sizeof(s->preparation));
    close(&s->download);
}
void discard_work(UpdateService* s) noexcept {
    release_work(s);
    DeleteFileW(s->paths.download);
}
// Best effort: the changelog is display data, so any failure only leaves it out.
void offer_changelog(UpdateService* s, const UpdateTransport& transport, Utf8View repository, UpdateChannel channel,
    const UpdateManifest& manifest, u64 now) noexcept {
    const UpdateChangelog* verified = nullptr;
    if (auto* d = s->changelog) {
        UpdateLocation location{};
        UpdateBufferSink document{reinterpret_cast<u8*>(d->document), sizeof(d->document), 0};
        UpdateBufferSink signature{d->signature, sizeof(d->signature), 0};
        bool fetched = update_asset_location(repository, channel, UpdateAsset::changelog, 0, &location) &&
            update_fetch(transport, location, kUpdateChangelogMaximumBytes, 0, update_buffer_sink, &document, now).ok() &&
            update_asset_location(repository, channel, UpdateAsset::changelog_signature, 0, &location) &&
            update_fetch(transport, location, 64, 64, update_buffer_sink, &signature, now).ok();
        d->document_bytes = document.used;
        const auto error = fetched ? update_changelog_verify({d->document, d->document_bytes}, {d->signature, 64}, s->key,
            s->workspace, {d->scratch, kUpdateChangelogStructurals}, &d->changelog) : UpdateChangelogError::invalid_input;
        const bool matches = error == UpdateChangelogError::none && d->changelog.version == manifest.version &&
            d->changelog.channel == channel;
        UpdateLog("changelog").field("fetched", fetched).field("error", static_cast<u8>(error)).field("matches", matches)
            .field("entries", matches ? d->changelog.count : 0).write(fetched && !matches ? LogLevel::warning : LogLevel::info);
        if (matches) verified = &d->changelog;
    }
    if (s->observer.offered) s->observer.offered(s->observer.context, manifest, verified);
}
} // namespace

bool update_service_config(Utf8View repository, Utf8View key_hex, u64 own_version, UpdateServiceConfig* out) noexcept {
    if (!out || !own_version) return false;
    UpdateServiceConfig c{};
    c.own_version = own_version; c.maximum_artifact_bytes = kUpdateDefaultArtifactCap;
    c.default_channel = update_version_channel(own_version);
    if (repository.bytes || key_hex.bytes) {
        if (!update_repository_valid(repository) || !update_public_key_from_hex(key_hex, {c.public_key, 65})) return false;
        copy_bytes(c.repository, repository.data, repository.bytes); c.repository_bytes = repository.bytes;
        c.configured = true;
    }
    *out = c; return true;
}

bool update_paths_build(ConstSlice<wchar_t> base, ConstSlice<wchar_t> executable, UpdatePaths* out) noexcept {
    if (!out || !drive_path(base) || !drive_path(executable)) return false;
    UpdatePaths p{};
    copy_bytes(p.base, base.data, base.count * sizeof(wchar_t));
    copy_bytes(p.executable, executable.data, executable.count * sizeof(wchar_t));
    p.executable_count = executable.count;
    const bool ok = join(p.work, p.base, base.count, L"work", &p.work_count) &&
        join(p.channel, p.base, base.count, L"channel") && join(p.interval, p.base, base.count, L"interval") &&
        join(p.acceptance, p.work, p.work_count, L"acceptance.bin", &p.acceptance_count) &&
        join(p.download, p.work, p.work_count, L"package.download", &p.download_count) &&
        suffixed(p.staged, executable, L".update", &p.staged_count) && suffixed(p.replaced, executable, L".old");
    if (!ok) return false;
    *out = p; return true;
}
bool update_paths_default(UpdatePaths* out) noexcept {
    wchar_t base[kUpdatePathCapacity]{}, executable[kUpdatePathCapacity]{};
    const DataPathResult built = data_path(base, kUpdatePathCapacity, L"updates");
    const DWORD n = GetModuleFileNameW(nullptr, executable, kUpdatePathCapacity);
    return built.ok() && n && n < kUpdatePathCapacity && update_paths_build({base, built.length}, {executable, n}, out);
}
void update_paths_cleanup(const UpdatePaths& paths) noexcept {
    const bool replaced = DeleteFileW(paths.replaced) != FALSE;
    const bool stale = DeleteFileW(paths.staged) != FALSE;
    if (replaced || stale) UpdateLog("cleanup").field("replaced", replaced).field("stale_stage", stale).write();
}

Utf8View update_service_error_text(UpdateServiceError e) noexcept {
    switch (e) {
    case SE::none: return {};
    case SE::configuration: return utf8_literal("Updates aren't configured for this build.");
    case SE::transport: return utf8_literal("Couldn't reach GitHub. Check your connection.");
    case SE::rate_limited: return utf8_literal("GitHub is limiting requests. MaiCord will try again later.");
    case SE::not_found: return utf8_literal("No update is published for this channel yet.");
    case SE::unavailable: return utf8_literal("The update's download isn't available yet. MaiCord will try again later.");
    case SE::redirect: case SE::oversize: case SE::size: return utf8_literal("The download didn't match the release.");
    case SE::signature: return utf8_literal("The update isn't signed by the MaiCord publisher.");
    case SE::expired: return utf8_literal("The update information has expired. Check your clock.");
    case SE::manifest: return utf8_literal("The update information isn't valid.");
    case SE::state: return utf8_literal("There's no update to apply.");
    default: return utf8_literal("Couldn't install the update.");
    }
}

UpdateServiceError update_service_initialize(UpdateService* s, const UpdateServiceConfig& config, const UpdatePaths& paths) noexcept {
    if (!s || s->initialized) return SE::state;
    clear_bytes(s, sizeof(*s));
    s->config = config; s->paths = paths;
    if (config.configured && (!kdf_sha256_initialize(&s->workspace).ok() ||
        !p256_import_public(&s->key, P256Kind::ecdsa, {config.public_key, 65}).ok())) {
        p256_release(&s->key); kdf_sha256_release(&s->workspace); clear_bytes(s, sizeof(*s));
        return SE::configuration;
    }
    s->initialized = true;
    return SE::none;
}
void update_service_release(UpdateService* s) noexcept {
    if (!s || !s->initialized) return;
    release_work(s);
    update_package_release(&s->staged, true);
    p256_release(&s->key); kdf_sha256_release(&s->workspace);
    SecureZeroMemory(s, sizeof(*s));
}

UpdateCheckResult update_service_check(UpdateService* s, const UpdateTransport& transport, UpdateChannel channel, u64 now) noexcept {
    if (!s || !s->initialized || channel > UpdateChannel::preview) return failed(SE::state);
    if (!s->config.configured) return failed(SE::configuration);
    const u64 floor = s->config.own_version;
    const u64 staged = s->staged.file ? s->staged.version : 0;
    UpdateLog("check").field("channel", static_cast<u8>(channel)).field("own", floor).field("staged", staged).write();
    const Utf8View repository{s->config.repository, s->config.repository_bytes};
    UpdateLocation location{};
    UpdateManifest manifest{};
    UpdateManifestPolicy policy{};
    // 1. Channel manifest and detached signature from the Pages site.
    UpdateBufferSink document{reinterpret_cast<u8*>(s->document), sizeof(s->document), 0};
    if (!update_asset_location(repository, channel, UpdateAsset::manifest, 0, &location)) return report(failed(SE::configuration), "location");
    auto fetched = update_fetch(transport, location, kUpdateManifestMaximumBytes, 0, update_buffer_sink, &document, now);
    if (!fetched.ok()) return report(fetch_failed(fetched), "manifest");
    UpdateBufferSink signature{s->signature, sizeof(s->signature), 0};
    if (!update_asset_location(repository, channel, UpdateAsset::signature, 0, &location)) return report(failed(SE::configuration), "location");
    fetched = update_fetch(transport, location, 64, 64, update_buffer_sink, &signature, now);
    if (!fetched.ok()) return report(fetch_failed(fetched), "signature");
    s->document_bytes = document.used;
    const Utf8View text{s->document, s->document_bytes};
    // 2. Publisher signature and schema first: an older or equal signed
    //    version is "up to date" and never reaches durable state.
    auto verified = update_manifest_authenticate(text, {s->signature, 64}, s->key, s->workspace,
        s->config.maximum_artifact_bytes, kUpdateMaximumValiditySeconds, {s->scratch, 256}, &manifest);
    if (!verified.ok()) return report(manifest_failed(manifest_error(verified.error), verified.error), "verify");
    UpdateLog("offer").field("channel", static_cast<u8>(manifest.channel)).field("version", manifest.version)
        .field("bytes", manifest.artifact_bytes).field("issued", manifest.issued_unix_seconds).field("expires", manifest.expires_unix_seconds).write();
    // The stable channel never offers a preview build, whatever its manifest says.
    if (manifest.channel != channel || (channel == UpdateChannel::stable && update_version_channel(manifest.version) != UpdateChannel::stable))
        return report(manifest_failed(SE::manifest, UpdateManifestError::unsupported), "channel");
    if (manifest.version <= floor) return up_to_date(s, floor, manifest.version);
    offer_changelog(s, transport, repository, channel, manifest, now);
    if (staged >= manifest.version) {
        UpdateLog("ready").field("version", staged).field("staged", 1).write();
        return outcome(UpdateCheckOutcome::ready, staged);
    }
    // 3. Freshness and the durable rollback floor. A missing record means no
    //    version was admitted yet: there is no other install record, and
    //    deleting it takes the same local write access as replacing the
    //    executable. The installed version stays the floor either way.
    UpdateAcceptanceState accepted{};
    if (exists(s->paths.acceptance)) {
        const auto loaded = update_acceptance_load({s->paths.acceptance, s->paths.acceptance_count}, &accepted);
        if (!loaded.ok()) {
            UpdateLog("acceptance_load").field("error", static_cast<u8>(loaded.error)).field("native", loaded.native_error).write(LogLevel::warning);
            return report(failed(SE::acceptance), "acceptance");
        }
    }
    const auto& lane = accepted.channels[static_cast<u32>(channel)];
    policy = {floor, lane.present ? lane.highest_version : 0, now, s->config.maximum_artifact_bytes,
        kUpdateMaximumValiditySeconds, channel, {}, lane.present};
    if (lane.present) copy_bytes(policy.accepted_manifest_sha256, lane.manifest_sha256, 32);
    verified = update_manifest_verify(text, {s->signature, 64}, s->key, s->workspace, policy, {s->scratch, 256}, &manifest);
    if (verified.error == UpdateManifestError::rollback) return up_to_date(s, floor, manifest.version);
    if (!verified.ok()) return report(manifest_failed(manifest_error(verified.error), verified.error), "freshness");
    // 4. Durable admission before any package byte is fetched.
    if (!ensure_storage(s->paths)) { auto r = failed(SE::storage); r.native_error = GetLastError(); return report(r, "storage"); }
    UpdateAcceptanceTransaction acceptance{};
    auto admitted = update_acceptance_begin({s->paths.acceptance, s->paths.acceptance_count}, &acceptance, true);
    UpdateManifest approved{};
    if (admitted.ok()) admitted = update_acceptance_verify_commit(&acceptance, text, {s->signature, 64}, s->key, s->workspace,
        &policy, {s->scratch, 256}, &approved);
    update_acceptance_release(&acceptance);
    if (!admitted.ok()) {
        auto r = failed(admitted.error == UpdateAcceptanceError::manifest ? manifest_error(admitted.manifest_error) : SE::acceptance);
        r.manifest_error = admitted.manifest_error; r.native_error = admitted.native_error;
        UpdateLog("admission").field("error", static_cast<u8>(admitted.error)).field("manifest", static_cast<u8>(admitted.manifest_error))
            .field("native", admitted.native_error).write(LogLevel::warning);
        return report(r, "admission");
    }
    UpdateLog("admitted").field("version", manifest.version).write();
    // 5. Package from the version's immutable release, capped at the signed
    //    size. The package an earlier session staged and left uninstalled is
    //    used again while it still has the signed size and digest; step 6
    //    verifies it once more under the acceptance lock.
    if (!update_asset_location(repository, channel, UpdateAsset::package, manifest.version, &location))
        return report(manifest_failed(SE::manifest, UpdateManifestError::unsupported), "tag");
    UpdateArtifact kept{};
    const bool reused = update_artifact_verify({s->paths.download, s->paths.download_count}, manifest, &kept).ok();
    update_artifact_release(&kept);
    if (reused) UpdateLog("package").field("version", manifest.version).field("reused", 1).write();
    else {
        s->download = CreateFileW(s->paths.download, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s->download == INVALID_HANDLE_VALUE) {
            s->download = nullptr; auto r = failed(SE::storage); r.native_error = GetLastError(); return report(r, "download_file");
        }
        InterlockedExchange64(&s->progress.received, 0);
        InterlockedExchange64(&s->progress.total, static_cast<LONG64>(manifest.artifact_bytes));
        fetched = update_fetch(transport, location, manifest.artifact_bytes, manifest.artifact_bytes, file_sink, s, now);
        const bool flushed = FlushFileBuffers(s->download) != FALSE;
        const DWORD flush_error = flushed ? 0 : GetLastError();
        close(&s->download);
        if (!fetched.ok() || !flushed) {
            DeleteFileW(s->paths.download);
            // A 404 here means the signed version's release is gone, unpublished or private.
            auto r = !flushed ? failed(SE::storage) : fetched.error == UpdateFetchError::not_found ? failed(SE::unavailable) : fetch_failed(fetched);
            r.fetch_error = fetched.error; r.retry_at_unix = fetched.retry_at_unix;
            if (!flushed) r.native_error = flush_error;
            return report(r, "package");
        }
    }
    // 6. Admission again under the live acceptance lock, then the download's
    //    signed size and digest.
    const UpdatePreparationRequest request{{s->paths.acceptance, s->paths.acceptance_count},
        {s->paths.download, s->paths.download_count}, {s->document, s->document_bytes}, {s->signature, 64}, true};
    clear_bytes(&s->preparation, sizeof(s->preparation));
    const auto prepared = update_prepare_artifact(&s->preparation, request, s->key, s->workspace, &policy, {s->scratch, 256});
    {
        UpdateLog line("prepare");
        line.field("error", static_cast<u8>(prepared.error)).field("phase", static_cast<u8>(prepared.phase));
        if (!prepared.ok()) line.field("acceptance", static_cast<u8>(prepared.acceptance.error)).field("acceptance_native", prepared.acceptance.native_error)
            .field("artifact", static_cast<u8>(prepared.artifact.error)).field("artifact_native", prepared.artifact.native_error);
        line.write(prepared.ok() ? LogLevel::info : LogLevel::warning);
    }
    if (!prepared.ok()) { discard_work(s); return report(failed(SE::stage), "prepare"); }
    // 7. The package's executable, verified and staged beside the running one.
    //    An earlier stage is replaced only now; a stale file from an earlier
    //    session is never adopted.
    update_package_release(&s->staged, true);
    if (!s->staged.file) DeleteFileW(s->paths.staged);
    UpdatePackage package{};
    const UpdatePackageOffer offer{{s->document, s->document_bytes}, {s->signature, 64}};
    const auto extracted = update_package_extract(&s->preparation.artifact, offer, s->key, s->workspace, policy,
        s->preparation.acceptance, {s->scratch, 256}, {s->paths.staged, s->paths.staged_count}, &package);
    {
        UpdateLog line("stage");
        line.field("error", static_cast<u8>(extracted.error));
        if (!extracted.ok()) line.field("native", extracted.native_error).field("crypto", static_cast<u32>(extracted.crypto_status))
            .field("manifest", static_cast<u8>(extracted.manifest_error)).field("cleanup", extracted.cleanup_error);
        line.write(extracted.ok() ? LogLevel::info : LogLevel::warning);
    }
    if (!extracted.ok()) {
        discard_work(s);
        auto r = failed(SE::install); r.manifest_error = extracted.manifest_error; r.native_error = extracted.native_error;
        return report(r, "stage");
    }
    // The package stays until the update is installed or superseded, so a
    // later session stages it again without downloading it.
    release_work(s);
    s->staged = package;
    UpdateLog("ready").field("version", manifest.version).write();
    return outcome(UpdateCheckOutcome::ready, manifest.version);
}

UpdateSwapResult update_swap(const UpdatePaths& paths) noexcept {
    UpdateSwapResult r{};
    if (!MoveFileExW(paths.executable, paths.replaced, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        r.error = UpdateSwapError::replace; r.native_error = GetLastError(); return r;
    }
    if (MoveFileExW(paths.staged, paths.executable, MOVEFILE_WRITE_THROUGH)) return r;
    r.error = UpdateSwapError::install; r.native_error = GetLastError();
    // Put the running executable back; a copy is the last resort.
    if (!MoveFileExW(paths.replaced, paths.executable, MOVEFILE_WRITE_THROUGH)) {
        r.restore_error = GetLastError();
        if (CopyFileW(paths.replaced, paths.executable, TRUE)) r.restore_error = 0;
    }
    return r;
}
UpdateSwapResult update_service_swap(UpdateService* s) noexcept {
    UpdateSwapResult r{};
    if (!s || !s->initialized || !s->staged.file) { r.error = UpdateSwapError::nothing_staged; return r; }
    const u64 version = s->staged.version;
    // Close (not discard) the verified stage: the moves need it unopened.
    const auto released = update_package_release(&s->staged, false);
    if (!released.ok()) { r.error = UpdateSwapError::release; r.native_error = released.native_error; }
    else r = update_swap(s->paths);
    // Installed: the package it came from is not needed again.
    if (r.ok()) DeleteFileW(s->paths.download);
    UpdateLog("swap").field("version", version).field("error", static_cast<u8>(r.error)).field("native", r.native_error)
        .field("restore", r.restore_error).write(r.ok() ? LogLevel::info : LogLevel::warning);
    return r;
}

namespace {
u8 load_choice(const wchar_t* path, u8 maximum, u8 fallback) noexcept {
    HANDLE h = open_read(path); u64 bytes = 0; u8 value = 0xff; u32 native = 0;
    const bool ok = regular_size(h, &bytes) && bytes == 1 && file_persistence_read_complete(h, {&value, 1}, &native);
    close(&h);
    return ok && value <= maximum ? value : fallback;
}
bool save_choice(const UpdatePaths& paths, bool interval, u8 value) noexcept {
    if (!ensure_storage(paths)) return false;
    HANDLE h = CreateFileW(interval ? paths.interval : paths.channel, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = write_exact(h, &value, 1);
    CloseHandle(h);
    return ok;
}
} // namespace

UpdateChannel update_channel_load(const UpdatePaths& paths, UpdateChannel fallback) noexcept {
    return static_cast<UpdateChannel>(load_choice(paths.channel, static_cast<u8>(UpdateChannel::preview), static_cast<u8>(fallback)));
}
bool update_channel_save(const UpdatePaths& paths, UpdateChannel channel) noexcept {
    return channel <= UpdateChannel::preview && save_choice(paths, false, static_cast<u8>(channel));
}
UpdateCheckInterval update_interval_load(const UpdatePaths& paths, UpdateCheckInterval fallback) noexcept {
    return static_cast<UpdateCheckInterval>(load_choice(paths.interval, kUpdateCheckIntervalCount - 1, static_cast<u8>(fallback)));
}
bool update_interval_save(const UpdatePaths& paths, UpdateCheckInterval interval) noexcept {
    return static_cast<u32>(interval) < kUpdateCheckIntervalCount && save_choice(paths, true, static_cast<u8>(interval));
}

} // namespace maicord
