#include "maicord/app/plugin_index_service.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using CE = PluginIndexCheckError;
constexpr Utf8View kPagesSuffix = utf8_literal(".github.io");

struct Writer {
    char* out;
    u32 capacity, used;
    bool ok;
    void add(const char* text, u32 bytes) noexcept {
        if (!ok || bytes > capacity - used) { ok = false; return; }
        copy_bytes(out + used, text, bytes);
        used += bytes;
    }
    void add(Utf8View text) noexcept { add(text.data, text.bytes); }
};
Utf8View source_host(const PluginCatalogSource& source, char (&pages)[kUpdateHostNameCapacity]) noexcept {
    if (source.kind != PluginProviderKind::official) return {source.host, source.host_bytes};
    u32 slash = 0;
    while (slash < source.repository_bytes && source.repository[slash] != '/') ++slash;
    if (!slash || slash + kPagesSuffix.bytes > kUpdateHostNameCapacity) return {};
    for (u32 i = 0; i < slash; ++i) pages[i] = ascii_lower(source.repository[i]);
    copy_bytes(pages + slash, kPagesSuffix.data, kPagesSuffix.bytes);
    return {pages, slash + kPagesSuffix.bytes};
}
bool source_valid(const PluginCatalogSource& s) noexcept {
    switch (s.kind) {
    case PluginProviderKind::official:
        return s.provider == kPluginProviderOfficial && (s.key_count == 1 || s.key_count == 2) && s.keys[0] &&
            (s.key_count == 1 || s.keys[1]) && update_repository_valid({s.repository, s.repository_bytes});
    case PluginProviderKind::custom_signed:
        if (s.key_count != 1 || !s.keys[0] || s.pinned_key[0] != 4) return false;
        [[fallthrough]];
    case PluginProviderKind::custom_unsigned:
        return s.provider > kPluginProviderOfficial && (s.kind == PluginProviderKind::custom_signed || !s.key_count) &&
            update_origin_host_valid({s.host, s.host_bytes}) && s.path_bytes && s.path_bytes <= kPluginProviderPathCapacity &&
            s.path[0] == '/' && s.path[s.path_bytes - 1] == '/';
    default: return false;
    }
}
PluginIndexPolicy source_policy(const PluginCatalogSource& source, u64 now, bool accept_expired) noexcept {
    PluginIndexPolicy policy{};
    policy.now_unix = now;
    policy.kind = source.kind;
    policy.accept_expired = accept_expired;
    if (source.kind == PluginProviderKind::custom_signed) copy_bytes(policy.pinned_key, source.pinned_key, 65);
    return policy;
}
// Verifies work's record with the source's keys: the official provider may
// sign with its current or next key. *used receives the key that verified.
PluginIndexError verify(const PluginCatalogSource& source, KdfSha256Workspace& workspace, PluginIndexWork* work,
    const PluginIndexPolicy& policy, const P256Key** used) noexcept {
    const Utf8View document = plugin_index_work_document(*work);
    const Slice<u32> scratch{work->scratch, kPluginIndexStructuralCapacity};
    *used = nullptr;
    if (source.kind == PluginProviderKind::custom_unsigned)
        return plugin_index_parse_unsigned(document, workspace, policy, scratch, &work->index).error;
    PluginIndexError error = PluginIndexError::signature;
    for (u32 i = 0; i < source.key_count && error == PluginIndexError::signature; ++i) {
        error = plugin_index_verify(document, {work->record, 64}, *source.keys[i], workspace, policy, scratch, &work->index).error;
        if (error == PluginIndexError::none) *used = source.keys[i];
    }
    return error;
}
PluginIndexCheckResult finish(PluginIndexCheckResult r, CE error, const char* phase, u64 now, u32 failures, u32 random) noexcept {
    r.error = error;
    r.outcome = error == CE::none ? r.outcome : PluginIndexCheckOutcome::failed;
    r.next_check_unix = update_next_check(now, error == CE::none ? 0 : failures + 1, r.retry_at_unix, random);
    if (error == CE::none) return r;
    UpdateLog line("plugins", "index_check_failed");
    line.add(" phase=").add(phase);
    line.field("error", static_cast<u8>(error)).field("fetch", static_cast<u8>(r.fetch_error))
        .field("index", static_cast<u8>(r.index_error)).field("record", static_cast<u8>(r.record_error))
        .field("status", r.status).field("native", r.native_error);
    if (r.retry_at_unix) line.field("retry_at", r.retry_at_unix);
    line.write(LogLevel::warning);
    return r;
}
bool fetched(PluginIndexCheckResult* r, const UpdateFetchResult& f) noexcept {
    r->fetch_error = f.error; r->status = f.status; r->native_error = f.native_error; r->retry_at_unix = f.retry_at_unix;
    return f.ok();
}
u32 cache_path(const PluginManagerPaths& paths, u64 provider, wchar_t (&output)[kPluginManagerPathCapacity]) noexcept {
    return plugin_manager_id_path(paths.cache, paths.cache_count, provider, L".index", output);
}
// Reads a whole cache file of 65 .. 64 + 256 KiB bytes without following a
// final reparse point; `missing` when absent.
AcceptanceRecordResult read_cache(const PluginManagerPaths& paths, u64 provider, PluginIndexWork* work) noexcept {
    wchar_t path[kPluginManagerPathCapacity];
    const u32 count = cache_path(paths, provider, path);
    if (!count || !acceptance_record_path_valid({path, count})) return {AcceptanceRecordError::invalid_input, 0};
    auto result = acceptance_record_ancestors(path, count);
    if (!result.ok()) return result;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD native = GetLastError();
        return {native == ERROR_FILE_NOT_FOUND ? AcceptanceRecordError::missing : AcceptanceRecordError::open, native};
    }
    result = acceptance_record_disk_file(file);
    Win32LargeInteger size{};
    if (result.ok() && !GetFileSizeEx(file, size.native())) result = {AcceptanceRecordError::size, GetLastError()};
    if (result.ok() && (size.QuadPart <= 64 || size.QuadPart > 64 + kPluginIndexMaximumBytes)) result = {AcceptanceRecordError::size, 0};
    u32 native = 0;
    if (result.ok() && !file_persistence_read_complete(file, {work->record, static_cast<u32>(size.QuadPart)}, &native))
        result = {AcceptanceRecordError::read, native};
    CloseHandle(file);
    work->document_bytes = result.ok() ? static_cast<u32>(size.QuadPart) - 64 : 0;
    return result;
}
// True when the cache file holds exactly work's record.
bool cache_current(const PluginManagerPaths& paths, u64 provider, const PluginIndexWork& work) noexcept {
    wchar_t path[kPluginManagerPathCapacity];
    if (!cache_path(paths, provider, path)) return false;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    Win32LargeInteger size{};
    bool same = acceptance_record_disk_file(file).ok() && GetFileSizeEx(file, size.native()) && size.QuadPart == 64 + work.document_bytes;
    u8 chunk[4096];
    for (u32 at = 0; same && at < 64 + work.document_bytes;) {
        const u32 n = minimum<u32>(sizeof(chunk), 64 + work.document_bytes - at);
        DWORD got = 0;
        same = ReadFile(file, chunk, n, &got, nullptr) && got == n && bytes_equal(chunk, work.record + at, n);
        at += n;
    }
    CloseHandle(file);
    return same;
}
AcceptanceRecordResult write_cache(const PluginManagerPaths& paths, u64 provider, const PluginIndexWork& work) noexcept {
    wchar_t path[kPluginManagerPathCapacity];
    const u32 count = cache_path(paths, provider, path);
    if (!count) return {AcceptanceRecordError::invalid_input, 0};
    return plugin_record_write({path, count}, {work.record, 64 + work.document_bytes});
}
// Admission under the lock, then the cache. As in the updater, a missing
// acceptance.bin is provisioned on first use.
PluginIndexCheckResult admit(const PluginManagerPaths& paths, const PluginCatalogSource& source, const P256Key* key,
    const PluginIndexPolicy& policy, KdfSha256Workspace& workspace, PluginIndexWork* work, PluginIndexCheckResult r,
    CE* error) noexcept {
    u32 native = 0;
    if (!plugin_manager_paths_ensure(paths, &native)) { r.native_error = native; *error = CE::storage; return r; }
    PluginIndexAcceptanceTransaction transaction{};
    auto admitted = plugin_index_acceptance_begin({paths.acceptance, paths.acceptance_count}, &transaction, true);
    if (admitted.ok())
        admitted = plugin_index_acceptance_verify_commit(&transaction, source.provider, plugin_index_work_document(*work),
            {work->record, 64}, key, workspace, policy, {work->scratch, kPluginIndexStructuralCapacity}, &work->index);
    const auto released = plugin_index_acceptance_release(&transaction);
    if (admitted.ok() && !released.ok()) admitted = released;
    r.record_error = admitted.error; r.native_error = admitted.native_error; r.index_error = admitted.index_error;
    if (!admitted.ok()) {
        *error = admitted.error == AcceptanceRecordError::replay ? CE::replay
            : admitted.error == AcceptanceRecordError::document ? CE::document : CE::acceptance;
        return r;
    }
    r.sequence = work->index.sequence;
    r.outcome = admitted.written ? PluginIndexCheckOutcome::updated : PluginIndexCheckOutcome::up_to_date;
    if (!cache_current(paths, source.provider, *work)) {
        const auto written = write_cache(paths, source.provider, *work);
        r.record_error = written.error; r.native_error = written.native_error;
        if (!written.ok()) { *error = CE::cache; return r; }
        r.cache_written = true;
    }
    *error = CE::none;
    return r;
}
} // namespace

bool plugin_catalog_source_official(Utf8View repository, ConstSlice<const P256Key*> keys, PluginCatalogSource* output) noexcept {
    if (!output || !keys.data || !keys.count || keys.count > 2 || !update_repository_valid(repository)) return false;
    PluginCatalogSource s{};
    s.provider = kPluginProviderOfficial;
    s.kind = PluginProviderKind::official;
    s.key_count = keys.count;
    for (u32 i = 0; i < keys.count; ++i) s.keys[i] = keys[i];
    copy_bytes(s.repository, repository.data, repository.bytes);
    s.repository_bytes = repository.bytes;
    if (!source_valid(s)) return false;
    copy_value(output, s);
    return true;
}

bool plugin_catalog_source_custom(const PluginProviderRecord& record, const P256Key* pinned, PluginCatalogSource* output) noexcept {
    const bool signed_provider = record.flags & kPluginProviderSigned;
    if (!output || signed_provider != (pinned != nullptr) || record.host_bytes > kPluginProviderHostCapacity ||
        record.path_bytes > kPluginProviderPathCapacity) return false;
    PluginCatalogSource s{};
    s.provider = record.id;
    s.kind = signed_provider ? PluginProviderKind::custom_signed : PluginProviderKind::custom_unsigned;
    if (signed_provider) {
        s.key_count = 1;
        s.keys[0] = pinned;
        copy_bytes(s.pinned_key, record.key, 65);
    }
    copy_bytes(s.host, record.host, record.host_bytes);
    copy_bytes(s.path, record.path, record.path_bytes);
    s.host_bytes = record.host_bytes;
    s.path_bytes = record.path_bytes;
    if (!source_valid(s)) return false;
    copy_value(output, s);
    return true;
}

bool plugin_catalog_index_location(const PluginCatalogSource& source, bool signature, UpdateLocation* output) noexcept {
    if (!output || !source_valid(source)) return false;
    if (source.kind != PluginProviderKind::official)
        return plugin_provider_index_location({source.host, source.host_bytes}, {source.path, source.path_bytes}, signature, output);
    char pages[kUpdateHostNameCapacity];
    const Utf8View host = source_host(source, pages);
    u32 slash = 0;
    while (source.repository[slash] != '/') ++slash;
    UpdateLocation l{};
    Writer w{l.target, kUpdateTargetCapacity, 0, true};
    w.add("/", 1);
    w.add(source.repository + slash + 1, source.repository_bytes - slash - 1);
    w.add(signature ? utf8_literal("/v1/index.json.sig") : utf8_literal("/v1/index.json"));
    if (!w.ok || !host.bytes) return false;
    l.host = UpdateHost::pages;
    copy_bytes(l.origin, host.data, host.bytes);
    l.origin_bytes = host.bytes;
    l.target_bytes = w.used;
    *output = l;
    return true;
}

bool plugin_catalog_file_location(const PluginCatalogSource& source, const PluginIndex& index, Utf8View document, u32 plugin,
    u32 version, PluginIndexArch arch, UpdateLocation* output) noexcept {
    if (!output || !source_valid(source) || plugin >= index.plugin_count || version >= index.plugins[plugin].version_count ||
        index.plugins[plugin].first_version + version >= index.version_count) return false;
    const PluginIndexVersion& listed = index.versions[index.plugins[plugin].first_version + version];
    const PluginIndexFile* file = nullptr;
    for (u32 i = 0; i < listed.file_count && i < kPluginIndexMaximumFiles; ++i) if (listed.files[i].arch == arch) file = &listed.files[i];
    if (!file) return false;
    const Utf8View arch_text = arch == PluginIndexArch::x64 ? utf8_literal("x64") : utf8_literal("arm64");
    UpdateLocation l{};
    Writer w{l.target, kUpdateTargetCapacity, 0, true};
    char text[kPluginIndexAssetMaximumBytes + 1];
    const auto decoded = [&](PluginIndexText field) noexcept {
        const auto result = plugin_index_text_decode(document, field, {text, sizeof(text)});
        return result.ok() ? Utf8View{text, result.bytes} : Utf8View{};
    };
    if (source.kind == PluginProviderKind::official || file->source == PluginIndexFileSource::github_release) {
        l.host = UpdateHost::github;
        w.add("/", 1);
        if (source.kind == PluginProviderKind::official) {
            if (file->source != PluginIndexFileSource::derived) return false;
            char version_text[24];
            const u32 version_bytes = update_version_text(listed.version_code, {version_text, sizeof(version_text)});
            const Utf8View slug = decoded(index.plugins[plugin].slug);
            if (!version_bytes || !slug.bytes) return false;
            char slug_text[kPluginIndexSlugMaximumBytes];
            copy_bytes(slug_text, slug.data, slug.bytes);
            const Utf8View slug_copy{slug_text, slug.bytes};
            w.add(source.repository, source.repository_bytes);
            w.add(utf8_literal("/releases/download/")); w.add(slug_copy); w.add(utf8_literal("-v")); w.add(version_text, version_bytes);
            w.add("/", 1); w.add(slug_copy); w.add("-", 1); w.add(version_text, version_bytes); w.add("-", 1); w.add(arch_text);
            w.add(utf8_literal(".dll"));
        } else {
            w.add(decoded(file->repository)); w.add(utf8_literal("/releases/download/"));
            w.add(decoded(file->tag)); w.add("/", 1); w.add(decoded(file->asset));
        }
        if (!w.ok) return false;
        l.target_bytes = w.used;
        *output = l;
        return true;
    }
    if (file->source != PluginIndexFileSource::path) return false;
    w.add(source.path, source.path_bytes);
    w.add(decoded(file->path));
    return w.ok && update_provider_location({source.host, source.host_bytes}, {l.target, w.used}, output);
}

PluginIndexCheckResult plugin_index_check(const PluginManagerPaths& paths, const PluginCatalogSource& source,
    const UpdateTransport& transport, KdfSha256Workspace& workspace, PluginIndexWork* work, u64 now, u32 failures,
    u32 random) noexcept {
    PluginIndexCheckResult r{};
    UpdateLocation location{};
    if (!work || !transport.get || !now || !paths.manager_count || !plugin_catalog_index_location(source, false, &location)) {
        r.error = CE::invalid_input;
        return r;
    }
    UpdateTransport plugins = transport;
    plugins.log_category = "plugins";
    char pages[kUpdateHostNameCapacity];
    const Utf8View host = source_host(source, pages);
    UpdateLog("plugins", "index_check").field("kind", static_cast<u8>(source.kind)).host("host", host).write();
    u32 native = 0;
    if (!plugin_manager_paths_ensure(paths, &native)) { r.native_error = native; return finish(r, CE::storage, "storage", now, failures, random); }
    // A corrupt acceptance.bin fails closed.
    PluginIndexAcceptanceState floors{};
    const auto loaded = plugin_index_acceptance_load({paths.acceptance, paths.acceptance_count}, &floors);
    if (!loaded.ok() && loaded.error != AcceptanceRecordError::missing) {
        r.record_error = loaded.error; r.native_error = loaded.native_error;
        return finish(r, CE::acceptance, "floor", now, failures, random);
    }
    clear_bytes(work->record, 64);
    work->document_bytes = 0;
    UpdateBufferSink document{work->record + 64, kPluginIndexMaximumBytes, 0};
    if (!fetched(&r, update_fetch(plugins, location, kPluginIndexMaximumBytes, 0, update_buffer_sink, &document, now)))
        return finish(r, CE::fetch, "index", now, failures, random);
    work->document_bytes = document.used;
    if (source.kind == PluginProviderKind::custom_signed) {
        // A rotated key fails as key_changed before any signature work, so the
        // provider row can offer "Trust new key" with the served fingerprint.
        const auto peeked = plugin_index_unverified_key(plugin_index_work_document(*work), {work->scratch, kPluginIndexStructuralCapacity},
            r.served_key);
        if (peeked == PluginIndexKeyPeek::present && !bytes_equal(r.served_key, source.pinned_key, 65)) {
            plugin_provider_fingerprint(workspace, r.served_key, r.served_fingerprint);
            r.index_error = PluginIndexError::key_changed;
            return finish(r, CE::key_changed, "key", now, failures, random);
        }
        clear_bytes(r.served_key, 65);
    }
    if (source.kind != PluginProviderKind::custom_unsigned) {
        if (!plugin_catalog_index_location(source, true, &location)) return finish(r, CE::invalid_input, "location", now, failures, random);
        UpdateBufferSink signature{work->record, 64, 0};
        if (!fetched(&r, update_fetch(plugins, location, 64, 64, update_buffer_sink, &signature, now)))
            return finish(r, CE::fetch, "signature", now, failures, random);
    }
    const PluginIndexPolicy policy = source_policy(source, now, false);
    const P256Key* key = nullptr;
    r.index_error = verify(source, workspace, work, policy, &key);
    if (r.index_error != PluginIndexError::none)
        return finish(r, r.index_error == PluginIndexError::key_changed ? CE::key_changed : CE::document, "verify", now, failures, random);
    r.sequence = work->index.sequence;
    UpdateLog("plugins", "index_verified").field("sequence", work->index.sequence).field("issued", work->index.issued)
        .field("expires", work->index.expires).field("plugins", work->index.plugin_count).field("bytes", work->document_bytes).write();
    const auto verdict = plugin_index_floor_check(floors, source.provider, work->index.sequence, work->index.digest);
    if (verdict == PluginIndexFloorVerdict::replay) {
        const PluginIndexFloor* floor = plugin_index_floor(floors, source.provider);
        UpdateLog("plugins", "index_replay").field("floor", floor ? floor->sequence : 0).field("offered", work->index.sequence)
            .write(LogLevel::warning);
        return finish(r, CE::replay, "floor", now, failures, random);
    }
    CE error = CE::none;
    if (verdict == PluginIndexFloorVerdict::same) {
        // No floor write; the cache is rewritten only when lost or different.
        r.outcome = PluginIndexCheckOutcome::up_to_date;
        if (!cache_current(paths, source.provider, *work)) {
            const auto written = write_cache(paths, source.provider, *work);
            r.record_error = written.error; r.native_error = written.native_error;
            if (!written.ok()) return finish(r, CE::cache, "cache", now, failures, random);
            r.cache_written = true;
        }
        UpdateLog("plugins", "index_up_to_date").field("sequence", r.sequence).field("cache", r.cache_written).write();
        return finish(r, CE::none, "", now, failures, random);
    }
    r = admit(paths, source, key, policy, workspace, work, r, &error);
    if (error != CE::none) return finish(r, error, error == CE::cache ? "cache" : "admission", now, failures, random);
    UpdateLog("plugins", r.outcome == PluginIndexCheckOutcome::updated ? "index_admitted" : "index_up_to_date")
        .field("sequence", r.sequence).field("cache", r.cache_written).write();
    return finish(r, CE::none, "", now, failures, random);
}

PluginIndexCacheResult plugin_index_load_cache(const PluginManagerPaths& paths, const PluginCatalogSource& source,
    KdfSha256Workspace& workspace, PluginIndexWork* work, u64 now) noexcept {
    PluginIndexCacheResult r{};
    if (!work || !now || !paths.cache_count || !source_valid(source)) { r.error = CE::invalid_input; return r; }
    const auto read = read_cache(paths, source.provider, work);
    if (!read.ok()) {
        r.error = CE::cache; r.record_error = read.error; r.native_error = read.native_error;
        clear_bytes(&work->index, sizeof(work->index));
        return r;
    }
    const P256Key* key = nullptr;
    r.index_error = verify(source, workspace, work, source_policy(source, now, true), &key);
    if (r.index_error != PluginIndexError::none) {
        r.error = CE::document;
        UpdateLog("plugins", "cache_refused").field("index", static_cast<u8>(r.index_error)).write(LogLevel::warning);
        return r;
    }
    PluginIndexAcceptanceState floors{};
    const auto loaded = plugin_index_acceptance_load({paths.acceptance, paths.acceptance_count}, &floors);
    if (!loaded.ok() && loaded.error != AcceptanceRecordError::missing) {
        r.error = CE::acceptance; r.record_error = loaded.error; r.native_error = loaded.native_error;
        return r;
    }
    const PluginIndexFloor* floor = loaded.ok() ? plugin_index_floor(floors, source.provider) : nullptr;
    r.expired = work->index.expired;
    r.admitted = floor && floor->sequence == work->index.sequence && bytes_equal(floor->index_sha256, work->index.digest, 32);
    r.installable = r.admitted && !r.expired;
    UpdateLog("plugins", "cache_loaded").field("sequence", work->index.sequence).field("expired", r.expired)
        .field("admitted", r.admitted).write();
    return r;
}

PluginProviderAddResult plugin_index_add_provider(const PluginManagerPaths& paths, PluginProviders* providers,
    const PluginProviderAddress& address, const PluginProviderProbe& probe, KdfSha256Workspace& workspace,
    PluginIndexWork* work, u64 now) noexcept {
    PluginProviderAddResult r{};
    char name[kPluginProviderNameCapacity];
    const auto decoded = work ? plugin_index_text_decode(plugin_index_work_document(*work), work->index.provider_name, {name, sizeof(name)})
                              : JsonStringDecodeResult{0, JsonStringDecodeError::invalid_input};
    if (!providers || !work || !now || probe.error != PluginProviderProbeError::none || !decoded.ok() ||
        work->index.sequence != probe.sequence || !paths.manager_count) {
        r.error = PluginProviderAddError::invalid_input;
        return r;
    }
    PluginProviders next = *providers;
    r.insert_error = plugin_providers_insert(&next, address, probe, {name, decoded.bytes}, now);
    if (r.insert_error != PluginProviderInsertError::none) { r.error = PluginProviderAddError::insert; return r; }
    u32 native = 0;
    if (!plugin_manager_paths_ensure(paths, &native)) { r.error = PluginProviderAddError::providers; r.native_error = native; return r; }
    const auto saved = plugin_providers_save({paths.providers, paths.providers_count}, next);
    if (!saved.ok()) {
        r.error = PluginProviderAddError::providers; r.record_error = saved.error; r.native_error = saved.native_error;
        return r;
    }
    *providers = next;
    PluginCatalogSource source{};
    P256Key key{};
    const PluginProviderRecord* record = plugin_providers_find(next, address.id);
    const bool imported = !probe.signed_provider || p256_import_public(&key, P256Kind::ecdsa, {probe.key, 65}).ok();
    if (!record || !imported || !plugin_catalog_source_custom(*record, probe.signed_provider ? &key : nullptr, &source)) {
        p256_release(&key);
        r.error = PluginProviderAddError::invalid_input;
        return r;
    }
    CE error = CE::none;
    const auto admitted = admit(paths, source, probe.signed_provider ? &key : nullptr, source_policy(source, now, false), workspace,
        work, {}, &error);
    p256_release(&key);
    r.record_error = admitted.record_error; r.native_error = admitted.native_error; r.index_error = admitted.index_error;
    if (error != CE::none) r.error = error == CE::cache ? PluginProviderAddError::cache : PluginProviderAddError::acceptance;
    UpdateLog("plugins", "provider_added").host("host", {record->host, record->host_bytes}).field("signed", probe.signed_provider)
        .field("sequence", probe.sequence).field("error", static_cast<u8>(r.error)).write(r.error == PluginProviderAddError::none ? LogLevel::info : LogLevel::warning);
    return r;
}

} // namespace maicord
