#include "maicord/app/update_service.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/platform/random.hpp"

#include <Windows.h>

namespace maicord {
namespace {
using SE = UpdateServiceError;
constexpr u8 enrollment_magic[8]{'M', 'C', 'U', 'P', 'E', 'N', 'R', '1'};
constexpr u32 kEnrollmentBytes = 64;
constexpr u32 kHandoffWaitMs = 60000;

bool join(wchar_t* out, const wchar_t* base, u32 base_count, const wchar_t* leaf, u32* count = nullptr) noexcept {
    const u32 leaf_count = wide_length(leaf, kUpdatePathCapacity);
    if (base_count + 1 + leaf_count >= kUpdatePathCapacity) return false;
    copy_bytes(out, base, base_count * sizeof(wchar_t)); out[base_count] = L'\\';
    copy_bytes(out + base_count + 1, leaf, (leaf_count + 1) * sizeof(wchar_t));
    if (count) *count = base_count + 1 + leaf_count;
    return true;
}
bool same_path(const wchar_t* a, u32 a_count, const wchar_t* b, u32 b_count) noexcept {
    return a_count == b_count && CompareStringOrdinal(a, static_cast<int>(a_count), b, static_cast<int>(b_count), TRUE) == CSTR_EQUAL;
}
bool exists(const wchar_t* path) noexcept { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
bool read_exact(HANDLE h, void* out, u32 bytes) noexcept {
    u32 done = 0;
    while (done < bytes) {
        DWORD got = 0;
        if (!ReadFile(h, static_cast<u8*>(out) + done, bytes - done, &got, nullptr) || !got || got > bytes - done) return false;
        done += got;
    }
    return true;
}
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
// Directory identities of every component, drive root first (enrollment ancestry).
bool ancestry(const wchar_t* root, u32 count, UpdateGenerationDirectoryIdentity* ids, u32* id_count) noexcept {
    wchar_t path[kUpdatePathCapacity]{}; copy_bytes(path, root, (count + 1) * sizeof(wchar_t)); *id_count = 0;
    for (u32 i = 2; i <= count; ++i) if (i == 2 || i == count || path[i] == L'\\') {
        const u32 end = i == 2 ? 3 : i; const wchar_t saved = path[end]; path[end] = 0;
        const HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        path[end] = saved;
        if (h == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{}; const bool ok = GetFileInformationByHandle(h, &info) != FALSE;
        CloseHandle(h);
        if (!ok || *id_count == kUpdateGenerationMaximumAncestors) return false;
        ids[(*id_count)++] = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
    }
    return true;
}
// Rebuild paths from the final normalized name so the generation root's
// canonical-path check holds for short names or differing case.
bool canonicalize(UpdatePaths* paths) noexcept {
    const HANDLE h = CreateFileW(paths->base, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    wchar_t final[kUpdatePathCapacity + 8]{};
    const DWORD n = GetFinalPathNameByHandleW(h, final, kUpdatePathCapacity + 8, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= kUpdatePathCapacity + 8 || n < 7 || final[0] != L'\\' || final[1] != L'\\' || final[2] != L'?' || final[3] != L'\\' ||
        final[5] != L':') return false;
    const u32 count = wide_length(paths->base, kUpdatePathCapacity);
    if (n - 4 == count && bytes_equal(final + 4, paths->base, count * sizeof(wchar_t))) return true;
    UpdatePaths rebuilt{};
    if (!update_paths_build({final + 4, n - 4}, &rebuilt)) return false;
    *paths = rebuilt; return true;
}
bool ensure_storage(UpdatePaths* paths) noexcept {
    wchar_t partial[kUpdatePathCapacity]{};
    const u32 count = wide_length(paths->base, kUpdatePathCapacity);
    copy_bytes(partial, paths->base, (count + 1) * sizeof(wchar_t));
    for (u32 i = 3; i <= count; ++i) if (i == count || partial[i] == L'\\') {
        const wchar_t saved = partial[i]; partial[i] = 0;
        if (!CreateDirectoryW(partial, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        partial[i] = saved;
    }
    if (!canonicalize(paths)) return false;
    const wchar_t* directories[]{paths->root, paths->work};
    for (const wchar_t* dir : directories)
        if (!CreateDirectoryW(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    wchar_t drive[]{paths->base[0], L':', L'\\', 0};
    return GetDriveTypeW(drive) == DRIVE_FIXED;
}
bool read_enrollment(const UpdatePaths& paths, const u8* inventory, UpdateGenerationIdentity* installation) noexcept {
    HANDLE h = open_read(paths.enrollment); u64 bytes = 0; u8 record[kEnrollmentBytes]{};
    const bool ok = regular_size(h, &bytes) && bytes == kEnrollmentBytes && read_exact(h, record, kEnrollmentBytes) &&
        bytes_equal(record, enrollment_magic, 8) && bytes_equal(record + 32, inventory, 32);
    close(&h);
    u8 any = 0; for (u32 i = 0; i < 16; ++i) any |= record[16 + i];
    if (!ok || !any) return false;
    copy_bytes(installation->bytes, record + 16, 16); return true;
}
bool create_enrollment(const UpdatePaths& paths, const u8* inventory) noexcept {
    u8 record[kEnrollmentBytes]{};
    copy_bytes(record, enrollment_magic, 8);
    u8 any = 0;
    do { if (!secure_random_fill({record + 16, 16}).ok()) return false; any = 0; for (u32 i = 0; i < 16; ++i) any |= record[16 + i]; } while (!any);
    copy_bytes(record + 32, inventory, 32);
    HANDLE h = CreateFileW(paths.enrollment, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
    const bool ok = write_exact(h, record, kEnrollmentBytes) && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) DeleteFileW(paths.enrollment);
    return ok;
}
UpdateInstallLimits limits(const UpdateService& s) noexcept {
    return {s.config.maximum_artifact_bytes, kUpdateMaximumValiditySeconds, kUpdateInstallMaximumRetainedBytes, kUpdateFreeSpaceReserveBytes};
}
struct BufferSink { u8* data; u32 capacity, used; };
bool buffer_sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto* b = static_cast<BufferSink*>(context);
    if (bytes.count > b->capacity - b->used) return false;
    copy_bytes(b->data + b->used, bytes.data, bytes.count); b->used += bytes.count; return true;
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
UpdateCheckResult up_to_date(u64 floor, u64 offered) noexcept {
    UpdateLog("up_to_date").field("floor", floor).field("offered", offered).write();
    return outcome(UpdateCheckOutcome::up_to_date, floor);
}
void log_install(const char* event, const UpdateInstallResult& i) noexcept {
    UpdateLog line(event);
    line.field("error", static_cast<u8>(i.error));
    if (!i.ok()) line.field("native", i.native_error).field("manifest", static_cast<u8>(i.manifest_error))
        .field("generation", static_cast<u8>(i.generation.error)).field("generation_native", i.generation.native_error)
        .field("pair", static_cast<u8>(i.pair.error)).field("pair_native", i.pair.native_error)
        .field("pair_crypto", static_cast<u32>(i.pair.crypto_status)).field("pair_manifest", static_cast<u8>(i.pair.manifest_error));
    line.write(i.ok() ? LogLevel::info : LogLevel::warning);
}
void log_handoff(const char* event, const UpdateHandoffResult& h) noexcept {
    UpdateLog(event).field("error", static_cast<u8>(h.error)).field("native", h.native_error)
        .field("outcome", static_cast<u8>(h.outcome)).write(h.ok() ? LogLevel::info : LogLevel::warning);
}
void stage_path(const UpdatePaths& paths, const u8* digest, wchar_t* out) noexcept {
    wchar_t name[80]{}; constexpr wchar_t hex[] = L"0123456789abcdef";
    copy_bytes(name, L"maicord-", 8 * sizeof(wchar_t));
    for (u32 i = 0; i < 32; ++i) { name[8 + i * 2] = hex[digest[i] >> 4]; name[9 + i * 2] = hex[digest[i] & 15]; }
    copy_bytes(name + 72, L".stage", 7 * sizeof(wchar_t));
    join(out, paths.work, paths.work_count, name);
}
void release_work(UpdateService* s) noexcept {
    update_stage_cleanup_release(&s->preparation_result.stage);
    if (s->stage.file) update_stage_release(&s->stage, true);
    clear_bytes(&s->stage, sizeof(s->stage));
    update_preparation_release(&s->preparation);
    clear_bytes(&s->preparation, sizeof(s->preparation)); clear_bytes(&s->preparation_result, sizeof(s->preparation_result));
    close(&s->download);
    DeleteFileW(s->paths.download);
}
bool random_identity(UpdateGenerationIdentity* id) noexcept {
    for (u32 attempt = 0; attempt < 4; ++attempt) {
        if (!secure_random_fill({id->bytes, 16}).ok()) return false;
        u8 any = 0; for (auto b : id->bytes) any |= b;
        if (any) return true;
    }
    return false;
}
bool reverted(const UpdateGenerationState& s) noexcept {
    return s.phase == UpdateGenerationPhase::stable && s.previous.present && s.previous.pair.version > s.active.pair.version;
}
bool hex_name(const wchar_t* name, const u8* digest) noexcept {
    constexpr wchar_t hex[] = L"0123456789abcdef";
    if (name[0] != L'g' || name[1] != L'-') return false;
    for (u32 i = 0; i < 32; ++i)
        if (name[2 + i * 2] != hex[digest[i] >> 4] || name[3 + i * 2] != hex[digest[i] & 15]) return false;
    return true;
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

bool update_paths_build(ConstSlice<wchar_t> base, UpdatePaths* out) noexcept {
    // Generation paths add "\g-<64>\MaiCordAccount.dll" below a root of at most 511.
    if (!out || !base.data || base.count < 4 || base.count > 400 || base[1] != L':' || base[2] != L'\\' ||
        base[base.count - 1] == L'\\' || !((base[0] >= L'A' && base[0] <= L'Z') || (base[0] >= L'a' && base[0] <= L'z')))
        return false;
    for (u32 i = 0; i < base.count; ++i) if (!base[i] || base[i] == L'/' || base[i] < 32 || (i > 1 && base[i] == L':')) return false;
    UpdatePaths p{};
    copy_bytes(p.base, base.data, base.count * sizeof(wchar_t));
    const bool ok = join(p.root, p.base, base.count, L"generations", &p.root_count) &&
        join(p.work, p.base, base.count, L"work", &p.work_count) &&
        join(p.enrollment, p.base, base.count, L"enrollment.bin") && join(p.channel, p.base, base.count, L"channel") &&
        join(p.journal, p.root, p.root_count, L"install.state") &&
        join(p.acceptance, p.work, p.work_count, L"acceptance.bin", &p.acceptance_count) &&
        join(p.download, p.work, p.work_count, L"package.download", &p.download_count);
    if (!ok) return false;
    *out = p; return true;
}
bool update_paths_default(UpdatePaths* out) noexcept {
    wchar_t base[kUpdatePathCapacity]{};
    const DataPathResult built = data_path(base, kUpdatePathCapacity, L"updates");
    return built.ok() && update_paths_build({base, built.length}, out);
}

Utf8View update_service_error_text(UpdateServiceError e) noexcept {
    switch (e) {
    case SE::none: return {};
    case SE::configuration: return utf8_literal("Updates aren't configured for this build.");
    case SE::busy: return utf8_literal("Another MaiCord window is installing an update.");
    case SE::transport: return utf8_literal("Couldn't reach GitHub. Check your connection.");
    case SE::rate_limited: return utf8_literal("GitHub is limiting requests. MaiCord will try again later.");
    case SE::not_found: return utf8_literal("No update is published for this channel yet.");
    case SE::unavailable: return utf8_literal("The update's download isn't available yet. MaiCord will try again later.");
    case SE::redirect: case SE::oversize: case SE::size: return utf8_literal("The download didn't match the release.");
    case SE::signature: return utf8_literal("The update isn't signed by the MaiCord publisher.");
    case SE::expired: return utf8_literal("The update information has expired. Check your clock.");
    case SE::manifest: return utf8_literal("The update information isn't valid.");
    case SE::disk: return utf8_literal("Not enough disk space to install the update.");
    case SE::launch: case SE::startup: return utf8_literal("The new version didn't start. You're still on this version.");
    case SE::state: return utf8_literal("There's no update to apply.");
    default: return utf8_literal("Couldn't install the update.");
    }
}

UpdateServiceError update_service_initialize(UpdateService* s, const UpdateServiceConfig& config, const UpdatePaths& paths) noexcept {
    if (!s || s->initialized) return SE::state;
    clear_bytes(s, sizeof(*s));
    s->config = config; s->paths = paths;
    if (exists(s->paths.base)) canonicalize(&s->paths);
    if (config.configured) {
        if (!kdf_sha256_initialize(&s->workspace).ok() || !p256_import_public(&s->key, P256Kind::ecdsa, {config.public_key, 65}).ok() ||
            !update_generation_inventory_fingerprint(s->key, s->workspace, {s->inventory, 32}).ok()) {
            p256_release(&s->key); kdf_sha256_release(&s->workspace); clear_bytes(s, sizeof(*s));
            return SE::configuration;
        }
    }
    s->initialized = true;
    return SE::none;
}
void update_service_release(UpdateService* s) noexcept {
    if (!s || !s->initialized) return;
    release_work(s);
    update_service_close(s);
    p256_release(&s->key); kdf_sha256_release(&s->workspace);
    SecureZeroMemory(s, sizeof(*s));
}

UpdateServiceError update_service_open(UpdateService* s, bool provision) noexcept {
    if (!s || !s->initialized) return SE::state;
    if (!s->config.configured) return SE::configuration;
    if (s->open) return SE::none;
    if (!read_enrollment(s->paths, s->inventory, &s->installation)) { UpdateLog("open_failed enrollment").write(LogLevel::warning); return SE::enrollment; }
    UpdateGenerationDirectoryIdentity ids[kUpdateGenerationMaximumAncestors]{}; u32 count = 0;
    if (!ancestry(s->paths.root, s->paths.root_count, ids, &count)) {
        UpdateLog("open_failed ancestry").field("native", GetLastError()).write(LogLevel::warning); return SE::storage;
    }
    UpdateGenerationEnrollment enrollment{};
    enrollment.root = {s->paths.root, s->paths.root_count}; enrollment.installation = s->installation;
    enrollment.ancestors = {ids, count}; enrollment.explicitly_enrolled = true;
    copy_bytes(enrollment.publisher_inventory_sha256, s->inventory, 32);
    clear_bytes(&s->transaction, sizeof(s->transaction)); clear_bytes(&s->install, sizeof(s->install));
    const auto begun = update_generation_begin(enrollment, &s->transaction, provision);
    if (!begun.ok()) {
        UpdateLog("open_failed generation").field("error", static_cast<u8>(begun.error)).field("native", begun.native_error)
            .field("provision", provision).write(LogLevel::warning);
        update_generation_release(&s->transaction); clear_bytes(&s->transaction, sizeof(s->transaction));
        return begun.error == UpdateGenerationError::gate && begun.native_error == ERROR_SHARING_VIOLATION ? SE::busy : SE::storage;
    }
    const auto initialized = update_install_initialize(&s->install, &s->transaction, s->key, s->workspace, limits(*s));
    if (!initialized.ok()) {
        log_install("open_failed install", initialized);
        update_generation_release(&s->transaction); clear_bytes(&s->transaction, sizeof(s->transaction));
        return SE::enrollment;
    }
    s->open = true;
    return SE::none;
}
void update_service_close(UpdateService* s) noexcept {
    if (!s || !s->open) return;
    update_pair_release(&s->pair, false); clear_bytes(&s->pair, sizeof(s->pair));
    update_install_release(&s->install);
    update_generation_release(&s->transaction);
    // A failed release leaks its handles; never keep a half-released owner.
    clear_bytes(&s->install, sizeof(s->install)); clear_bytes(&s->transaction, sizeof(s->transaction)); s->open = false;
}

UpdateServiceError update_service_inspect(UpdateService* s, UpdateInstalledState* out) noexcept {
    if (!s || !out || !s->initialized) return SE::state;
    *out = {};
    if (!s->config.configured) return SE::configuration;
    if (!exists(s->paths.journal)) return SE::none;
    const SE opened = update_service_open(s, false); if (opened != SE::none) return opened;
    const auto& st = s->transaction.state;
    out->journal = true; out->phase = st.phase; out->active_version = st.active.pair.version;
    out->previous_version = st.previous.present ? st.previous.pair.version : 0;
    out->candidate_version = st.candidate.present ? st.candidate.pair.version : 0;
    update_service_close(s);
    return SE::none;
}

UpdateCheckResult update_service_check(UpdateService* s, const UpdateTransport& transport, UpdateChannel channel, u64 now) noexcept {
    if (!s || !s->initialized || channel > UpdateChannel::preview) return failed(SE::state);
    if (!s->config.configured) return failed(SE::configuration);
    UpdateInstalledState installed{};
    const SE inspected = update_service_inspect(s, &installed);
    if (inspected != SE::none) return report(failed(inspected), "inspect");
    const bool journal = installed.journal;
    const u64 floor = maximum(s->config.own_version, installed.active_version);
    const bool was_reverted = installed.phase == UpdateGenerationPhase::stable && installed.previous_version > installed.active_version;
    UpdateLog("check").field("channel", static_cast<u8>(channel)).field("own", s->config.own_version)
        .field("active", installed.active_version).field("phase", static_cast<u8>(installed.phase)).field("journal", journal).write();
    const Utf8View repository{s->config.repository, s->config.repository_bytes};
    UpdateLocation location{};
    UpdateManifest manifest{};
    UpdateManifestPolicy policy{};
    // 1. Channel manifest and detached signature from the Pages site.
    BufferSink document{reinterpret_cast<u8*>(s->document), sizeof(s->document), 0};
    if (!update_asset_location(repository, channel, UpdateAsset::manifest, 0, &location)) return report(failed(SE::configuration), "location");
    auto fetched = update_fetch(transport, location, kUpdateManifestMaximumBytes, 0, buffer_sink, &document, now);
    if (!fetched.ok()) return report(fetch_failed(fetched), "manifest");
    BufferSink signature{s->signature, sizeof(s->signature), 0};
    if (!update_asset_location(repository, channel, UpdateAsset::signature, 0, &location)) return report(failed(SE::configuration), "location");
    fetched = update_fetch(transport, location, 64, 64, buffer_sink, &signature, now);
    if (!fetched.ok()) return report(fetch_failed(fetched), "signature");
    s->document_bytes = document.used; s->signature_bytes = signature.used;
    const Utf8View text{s->document, s->document_bytes};
    // 2. Publisher signature and schema first: an older or equal signed
    //    version is "up to date" and never reaches durable state.
    auto verified = update_manifest_verify_installed(text, {s->signature, 64}, s->key, s->workspace,
        s->config.maximum_artifact_bytes, kUpdateMaximumValiditySeconds, {s->scratch, 256}, &manifest);
    if (!verified.ok()) { auto r = failed(manifest_error(verified.error)); r.manifest_error = verified.error; return report(r, "verify"); }
    UpdateLog("offer").field("channel", static_cast<u8>(manifest.channel)).field("version", manifest.version)
        .field("bytes", manifest.artifact_bytes).field("issued", manifest.issued_unix_seconds).field("expires", manifest.expires_unix_seconds).write();
    if (manifest.channel != channel) { auto r = failed(SE::manifest); r.manifest_error = UpdateManifestError::unsupported; return report(r, "channel"); }
    if (manifest.version <= floor) return up_to_date(floor, manifest.version);
    // An explicit rollback skips releases up to the version left behind.
    if (was_reverted && manifest.version <= installed.previous_version) return up_to_date(floor, manifest.version);
    if (installed.phase == UpdateGenerationPhase::prepared && installed.candidate_version >= manifest.version) {
        UpdateLog("ready").field("version", installed.candidate_version).field("prepared", 1).write();
        return outcome(UpdateCheckOutcome::ready, installed.candidate_version);
    }
    // 3. Freshness and the durable rollback floor.
    // Before the first install no record exists; afterwards it is required.
    UpdateAcceptanceState accepted{};
    if (journal || exists(s->paths.acceptance)) {
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
    if (verified.error == UpdateManifestError::rollback) return up_to_date(floor, manifest.version);
    if (!verified.ok()) { auto r = failed(manifest_error(verified.error)); r.manifest_error = verified.error; return report(r, "freshness"); }
    // 4. Durable admission before any package byte is fetched.
    if (!ensure_storage(&s->paths)) return report(failed(SE::storage), "storage");
    if (!exists(s->paths.enrollment) && (journal || !create_enrollment(s->paths, s->inventory))) return report(failed(SE::enrollment), "enrollment");
    UpdateAcceptanceTransaction acceptance{};
    auto admitted = update_acceptance_begin({s->paths.acceptance, s->paths.acceptance_count}, &acceptance, !journal);
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
    // 5. Package from the version's immutable release, capped at the signed size.
    if (!update_asset_location(repository, channel, UpdateAsset::package, manifest.version, &location)) {
        auto r = failed(SE::manifest); r.manifest_error = UpdateManifestError::unsupported; return report(r, "tag");
    }
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
    // 6. Stage under the live acceptance lock (re-verifies signature and size).
    wchar_t stale[kUpdatePathCapacity]{};
    stage_path(s->paths, manifest.manifest_sha256, stale);
    DeleteFileW(stale);
    const UpdatePreparationRequest request{{s->paths.acceptance, s->paths.acceptance_count}, {s->paths.download, s->paths.download_count},
        {s->paths.work, s->paths.work_count}, {s->document, s->document_bytes}, {s->signature, 64}, !journal};
    clear_bytes(&s->preparation, sizeof(s->preparation)); clear_bytes(&s->stage, sizeof(s->stage));
    s->preparation_result = update_prepare_stage(&s->preparation, request, s->key, s->workspace, &policy, {s->scratch, 256}, &s->stage);
    {
        const auto& p = s->preparation_result;
        UpdateLog line("stage");
        line.field("error", static_cast<u8>(p.error)).field("phase", static_cast<u8>(p.phase));
        if (!p.ok()) line.field("acceptance", static_cast<u8>(p.acceptance.error)).field("acceptance_native", p.acceptance.native_error)
            .field("artifact", static_cast<u8>(p.artifact.error)).field("artifact_native", p.artifact.native_error)
            .field("stage", static_cast<u8>(p.stage.error)).field("stage_native", p.stage.native_error);
        line.write(p.ok() ? LogLevel::info : LogLevel::warning);
    }
    if (!s->preparation_result.ok()) { release_work(s); return report(failed(SE::stage), "stage"); }
    // 7. Generation install: provision the first generation, else prepare.
    SE opened = update_service_open(s, !journal);
    if (opened != SE::none) { release_work(s); return report(failed(opened), "open"); }
    UpdateInstallResult installed_result{};
    const HANDLE retained[]{s->preparation.artifact.file, s->preparation.acceptance.lock};
    const UpdatePairOffer offer{{s->document, s->document_bytes}, {s->signature, 64}};
    if (journal) {
        installed_result = update_install_recover(&s->install);
        auto& st = s->transaction.state;
        if (installed_result.ok() && st.phase == UpdateGenerationPhase::prepared) installed_result = update_install_cancel(&s->install);
        if (installed_result.ok() && st.phase == UpdateGenerationPhase::stable && st.previous.present)
            installed_result = update_install_forget_previous(&s->install);
        UpdateGenerationIdentity id{};
        if (installed_result.ok()) installed_result = random_identity(&id) ?
            update_install_prepare(&s->install, &s->stage, offer, policy, s->preparation.acceptance, id, {retained, 2}) :
            UpdateInstallResult{UpdateInstallError::state};
    } else {
        installed_result = update_install_provision_current(&s->install, &s->stage, offer, policy, s->preparation.acceptance, {retained, 2});
    }
    log_install("install", installed_result);
    update_service_close(s);
    release_work(s);
    if (!installed_result.ok()) {
        auto r = failed(installed_result.error == UpdateInstallError::disk || installed_result.error == UpdateInstallError::budget ? SE::disk : SE::install);
        r.manifest_error = installed_result.manifest_error; r.native_error = installed_result.native_error; return report(r, "install");
    }
    UpdateLog("ready").field("version", manifest.version).write();
    return outcome(UpdateCheckOutcome::ready, manifest.version);
}

UpdateServiceError update_service_restart(UpdateService* s) noexcept {
    if (!s || !s->initialized) return SE::state;
    if (!s->config.configured) return SE::configuration;
    if (!exists(s->paths.journal)) return SE::state;
    SE opened = update_service_open(s, false); if (opened != SE::none) return opened;
    auto& st = s->transaction.state;
    UpdateLog("restart").field("phase", static_cast<u8>(st.phase)).field("active", st.active.pair.version)
        .field("candidate", st.candidate.present ? st.candidate.pair.version : 0).write();
    if (st.phase == UpdateGenerationPhase::pending || st.phase == UpdateGenerationPhase::acknowledged ||
        st.phase == UpdateGenerationPhase::rollback) {
        const auto recovered = update_install_recover(&s->install);
        if (!recovered.ok()) { log_install("restart_recover", recovered); update_service_close(s); return SE::install; }
    }
    UpdateGenerationLaunchAdmission admission{};
    bool pending = false;
    UpdateInstallResult selected{};
    if (st.phase == UpdateGenerationPhase::prepared) {
        selected = update_install_activate(&s->install);
        if (selected.ok()) selected = update_install_activation_admission(&s->install, &admission);
        if (selected.ok()) selected = update_install_select_activation_pair(&s->install, &s->pair);
        pending = st.phase == UpdateGenerationPhase::pending;
    } else if (st.phase == UpdateGenerationPhase::stable && st.active.pair.version != s->config.own_version) {
        selected = update_install_select_pair(&s->install, &s->pair);
    } else { update_service_close(s); return SE::state; }
    if (!selected.ok()) log_install("restart_select", selected);
    bool acknowledged = false;
    SE error = SE::launch;
    if (selected.ok()) {
        clear_bytes(&s->handoff, sizeof(s->handoff));
        const auto launched = update_handoff_launch(&s->transaction, &s->install.authentication, s->pair, true,
            pending ? &admission : nullptr, &s->handoff);
        log_handoff("handoff_launch", launched);
        if (launched.ok()) {
            AllowSetForegroundWindow(GetProcessId(s->handoff.process));
            const auto waited = update_handoff_wait(&s->handoff, kHandoffWaitMs);
            log_handoff("handoff_wait", waited);
            acknowledged = waited.ok() && waited.outcome == UpdateHandoffOutcome::acknowledged;
            if (!acknowledged) {
                // A child that never confirmed is not left running beside us.
                if (WaitForSingleObject(s->handoff.process, 0) == WAIT_TIMEOUT) TerminateProcess(s->handoff.process, 1);
                WaitForSingleObject(s->handoff.process, 5000);
                error = SE::startup;
            } else if (pending) {
                UpdateGenerationIdentity transaction = s->handoff.expected.transaction;
                u8 digest[32]{}; copy_bytes(digest, s->handoff.expected.document_sha256, 32);
                if (update_install_acknowledge(&s->install, transaction, {digest, 32}).ok()) update_install_recover(&s->install);
            }
        }
        update_handoff_release(&s->handoff); clear_bytes(&s->handoff, sizeof(s->handoff));
    }
    update_pair_release(&s->pair, false); clear_bytes(&s->pair, sizeof(s->pair));
    // Pending without confirmation rolls back and discards the candidate.
    if (!acknowledged && st.phase == UpdateGenerationPhase::pending) update_install_recover(&s->install);
    update_service_close(s);
    return acknowledged ? SE::none : error;
}

UpdateServiceError update_service_revert(UpdateService* s) noexcept {
    if (!s || !s->initialized) return SE::state;
    if (!s->config.configured) return SE::configuration;
    if (!exists(s->paths.journal)) return SE::state;
    SE opened = update_service_open(s, false); if (opened != SE::none) return opened;
    const auto& st = s->transaction.state;
    const bool allowed = st.phase == UpdateGenerationPhase::stable && st.previous.present &&
        st.previous.pair.version < st.active.pair.version && st.disposal.target == UpdateGenerationDisposalTarget::none;
    const auto reverted_result = allowed ? update_install_revert(&s->install) : UpdateInstallResult{UpdateInstallError::state};
    const bool ok = allowed && reverted_result.ok();
    log_install("revert", reverted_result);
    update_service_close(s);
    return !allowed ? SE::state : ok ? SE::none : SE::install;
}

bool update_service_dispatch(UpdateService* s) noexcept {
    if (!s || !s->initialized || !s->config.configured || !exists(s->paths.journal)) return false;
    if (update_service_open(s, false) != SE::none) return false;
    const auto recovered = update_install_recover(&s->install);
    if (!recovered.ok()) log_install("dispatch_recover", recovered);
    const auto& st = s->transaction.state;
    bool launched = false;
    UpdateLog("dispatch").field("phase", static_cast<u8>(st.phase)).field("active", st.active.pair.version)
        .field("own", s->config.own_version).write();
    UpdateInstallResult selected{UpdateInstallError::state};
    if (st.phase == UpdateGenerationPhase::stable && st.active.pair.version > s->config.own_version) {
        selected = update_install_select_pair(&s->install, &s->pair);
        if (!selected.ok()) log_install("dispatch_select", selected);
    }
    if (selected.ok()) {
        clear_bytes(&s->handoff, sizeof(s->handoff));
        const auto started = update_handoff_launch(&s->transaction, &s->install.authentication, s->pair, true, nullptr, &s->handoff);
        log_handoff("dispatch_launch", started);
        if (started.ok()) {
            AllowSetForegroundWindow(GetProcessId(s->handoff.process));
            const auto waited = update_handoff_wait(&s->handoff, kHandoffWaitMs);
            log_handoff("dispatch_wait", waited);
            launched = waited.ok() && waited.outcome == UpdateHandoffOutcome::acknowledged;
            if (!launched && WaitForSingleObject(s->handoff.process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(s->handoff.process, 1); WaitForSingleObject(s->handoff.process, 5000);
            }
        }
        update_handoff_release(&s->handoff); clear_bytes(&s->handoff, sizeof(s->handoff));
    }
    update_service_close(s);
    return launched;
}

UpdateServiceError update_service_child_open(const UpdateServiceConfig& config, const UpdatePaths& input_paths,
    ConstSlice<wchar_t> module, ConstSlice<wchar_t> command_line, UpdateHandoffChild* child) noexcept {
    if (!child || !config.configured) return SE::configuration;
    UpdatePaths paths = input_paths;
    if (!canonicalize(&paths)) return SE::storage;
    // <root>\g-<64 lowercase hex>\MaiCord.exe, root from trusted configuration.
    constexpr wchar_t exe[] = L"\\MaiCord.exe";
    constexpr u32 exe_count = sizeof(exe) / sizeof(wchar_t) - 1;
    if (!module.data || module.count != paths.root_count + 1 + 66 + exe_count ||
        !same_path(module.data, paths.root_count, paths.root, paths.root_count) || module[paths.root_count] != L'\\' ||
        !same_path(module.data + paths.root_count + 67, exe_count, exe, exe_count)) return SE::state;
    KdfSha256Workspace workspace{}; P256Key key{}; u8 inventory[32]{};
    struct Owners {
        KdfSha256Workspace& w; P256Key& k; HANDLE files[6]{}; UpdatePairResult verification{};
        ~Owners() noexcept { update_pair_cleanup_release(&verification); for (auto& h : files) close(&h); p256_release(&k); kdf_sha256_release(&w); }
    } owners{workspace, key};
    if (!kdf_sha256_initialize(&workspace).ok() || !p256_import_public(&key, P256Kind::ecdsa, {config.public_key, 65}).ok() ||
        !update_generation_inventory_fingerprint(key, workspace, {inventory, 32}).ok()) return SE::configuration;
    UpdateGenerationIdentity installation{};
    if (!read_enrollment(paths, inventory, &installation)) return SE::enrollment;
    u64 bytes = 0; u8 record[kUpdateGenerationRecordBytes]{}; UpdateGenerationState state{};
    owners.files[0] = open_read(paths.journal);
    if (!regular_size(owners.files[0], &bytes) || bytes != sizeof(record) || !read_exact(owners.files[0], record, sizeof(record)) ||
        !update_generation_decode({record, sizeof(record)}, &state).ok() || !state.active.present ||
        (state.phase != UpdateGenerationPhase::stable && state.phase != UpdateGenerationPhase::pending) ||
        !bytes_equal(state.installation.bytes, installation.bytes, 16)) return SE::state;
    if (!hex_name(module.data + paths.root_count + 1, state.active.pair.document_sha256)) return SE::state;
    wchar_t generation[kUpdatePathCapacity]{}, path[kUpdatePathCapacity]{};
    copy_bytes(generation, module.data, (paths.root_count + 67) * sizeof(wchar_t));
    constexpr const wchar_t* names[]{L"MaiCord.exe", L"MaiCordAccount.dll", L"offer.json", L"offer.sig", L"package.zip"};
    u64 sizes[5]{};
    for (u32 i = 0; i < 5; ++i) {
        if (!join(path, generation, paths.root_count + 67, names[i])) return SE::state;
        owners.files[i + 1] = open_read(path);
        if (!regular_size(owners.files[i + 1], &sizes[i])) return SE::state;
    }
    if (!sizes[2] || sizes[2] > kUpdateManifestMaximumBytes || sizes[3] != 64 || sizes[4] != state.active.pair.package_bytes ||
        sizes[4] > config.maximum_artifact_bytes) return SE::state;
    char document[kUpdateManifestMaximumBytes]{}; u8 signature[64]{}; u32 scratch[256]{};
    if (!read_exact(owners.files[3], document, static_cast<u32>(sizes[2])) || !read_exact(owners.files[4], signature, 64)) return SE::state;
    UpdateStage stage{}; stage.file = owners.files[5]; stage.bytes = sizes[4];
    UpdatePairDescriptor derived{};
    owners.verification = update_pair_package_verify_installed(&stage, {{document, static_cast<u32>(sizes[2])}, {signature, 64}},
        key, workspace, kUpdateMaximumValiditySeconds, {scratch, 256}, owners.files[1], owners.files[2], &derived);
    if (!owners.verification.ok() || !update_generation_pair_equal(derived, state.active.pair)) return SE::signature;
    // The parent replaces the journal when it records acknowledgement.
    close(&owners.files[0]);
    UpdateHandoffIdentity identity{installation, state.transaction, {}};
    copy_bytes(identity.document_sha256, derived.document_sha256, 32);
    return update_handoff_child_open(command_line, identity, true, child).ok() ? SE::none : SE::launch;
}

UpdateChannel update_channel_load(const UpdatePaths& paths, UpdateChannel fallback) noexcept {
    HANDLE h = open_read(paths.channel); u64 bytes = 0; u8 value = 0xff;
    const bool ok = regular_size(h, &bytes) && bytes == 1 && read_exact(h, &value, 1);
    close(&h);
    return ok && value <= static_cast<u8>(UpdateChannel::preview) ? static_cast<UpdateChannel>(value) : fallback;
}
bool update_channel_save(const UpdatePaths& input, UpdateChannel channel) noexcept {
    UpdatePaths paths = input;
    if (channel > UpdateChannel::preview || !ensure_storage(&paths)) return false;
    HANDLE h = CreateFileW(paths.channel, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const u8 value = static_cast<u8>(channel);
    const bool ok = write_exact(h, &value, 1);
    CloseHandle(h);
    return ok;
}

} // namespace maicord
