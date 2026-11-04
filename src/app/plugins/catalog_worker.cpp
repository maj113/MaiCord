#include "catalog_worker.hpp"
#include "maicord/app/plugins/loader.hpp"
#include "maicord/app/packages/install.hpp"
#include "maicord/app/update/https.hpp"
#include "maicord/app/update/log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_records.hpp"

namespace maicord::app_detail {
namespace {
constexpr u32 kQueue = 16;
constexpr u32 kSignatureBytes = 64;
constexpr u64 kAssetCacheBytes = 16ull * 1024 * 1024;
constexpr char kLog[] = "catalog";

struct Worker {
    wchar_t base[480];
    u32 base_characters;
    char repository[kUpdateRepositoryMaximumBytes];
    u32 repository_bytes;
    u8 keys[2][65];
    u32 key_count;
    UpdateTransport transport;
    u64 (*clock_unix)() noexcept;
    bool (*validate_package)(CatalogKind, ConstSlice<u8>, Utf8View) noexcept;
    void (*library_changed)() noexcept;
    HANDLE thread, wake, stop, notify;
    SrwLock lock;
    // Guarded by lock.
    CatalogJob jobs[kQueue];
    u32 job_first, job_count;
    CatalogJobResult results[kQueue];
    u32 result_first, result_count;
    UpdateHttpsTransport* volatile https;
    volatile LONG64 progress_bytes, progress_total;
};
Worker* g_worker;

void release(void* memory) noexcept {
    if (memory) pages_release(memory);
}
u64 now_unix(const Worker& w) noexcept { return w.clock_unix ? w.clock_unix() : clock_unix_milliseconds() / 1000; }
ConstSlice<wchar_t> base(const Worker& w) noexcept { return {w.base, w.base_characters}; }

void hex(const u8* bytes, u32 count, char* out) noexcept {
    static constexpr char digits[] = "0123456789abcdef";
    for (u32 i = 0; i < count; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
}
void hex_wide(const u8* bytes, u32 count, wchar_t* out) noexcept {
    char text[64];
    hex(bytes, count, text);
    for (u32 i = 0; i < count * 2; ++i) out[i] = static_cast<wchar_t>(text[i]);
}
u32 append(wchar_t (&path)[600], u32 length, const wchar_t* text) noexcept {
    return wide_append(path, 600, &length, text) ? length : 0;
}

// ---- files --------------------------------------------------------------------------------------
// Reads a whole regular file of at most maximum bytes (not following a final reparse point) into a new
// allocation; null when it is missing, larger or unreadable.
u8* read_file(const wchar_t* path, u32 maximum, u32* bytes) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    u8* data = nullptr;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && static_cast<u64>(size.QuadPart) <= maximum) {
        data = commit_pages<u8>(static_cast<u64>(size.QuadPart));
        DWORD read = 0;
        if (data && (!ReadFile(file, data, static_cast<DWORD>(size.QuadPart), &read, nullptr) || read != size.QuadPart)) {
            release(data);
            data = nullptr;
        }
        *bytes = static_cast<u32>(size.QuadPart);
    }
    CloseHandle(file);
    return data;
}
// Writes bytes to "<path>.new" and moves it over path, flushed before the rename.
bool write_file(const wchar_t* path, const ConstSlice<u8>& bytes) noexcept {
    wchar_t staged[604];
    u32 length = wide_length(path, 600);
    copy_bytes(staged, path, length * 2);
    staged[length] = 0;
    if (!wide_append(staged, 604, &length, L".new")) return false;
    const HANDLE file = CreateFileW(staged, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data, bytes.count, &written, nullptr) && written == bytes.count && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok && MoveFileExW(staged, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(staged);
    return false;
}
// <base>\<directory> exists.
bool ensure(const Worker& w, const wchar_t* directory, wchar_t (&path)[600]) noexcept {
    return directory_ensure(w.base) && catalog_path(base(w), directory, path) && directory_ensure(path);
}

// ---- hashing and keys -----------------------------------------------------------------------------
struct Crypto {
    KdfSha256Workspace workspace;
    bool ready;
    ~Crypto() { if (ready) kdf_sha256_release(&workspace); }
    bool open() noexcept { return ready || (ready = kdf_sha256_initialize(&workspace).ok()); }
    bool digest(const ConstSlice<u8>& bytes, u8 (&out)[32]) noexcept {
        const ConstSlice<u8> parts[]{bytes};
        return open() && sha256(workspace, {parts, 1}, {out, 32}).ok();
    }
    bool matches(const ConstSlice<u8>& bytes, const u8* expected) noexcept {
        u8 actual[32];
        return digest(bytes, actual) && bytes_equal(actual, expected, 32);
    }
};

// ---- network --------------------------------------------------------------------------------------
struct Network {
    Worker* w;
    UpdateHttpsTransport* https;
    UpdateTransport transport;
    ~Network() {
        if (!https) return;
        w->https = nullptr;
        update_https_release(https);
        release(https);
    }
    bool open() noexcept {
        if (transport.get) return true;
        if (w->transport.get) {
            transport = w->transport;
            return true;
        }
        https = commit_pages<UpdateHttpsTransport>();
        if (!https || !update_https_initialize(https)) return false;
        w->https = https;
        transport = update_https_transport(https);
        transport.log_category = kLog;
        return true;
    }
};
// https://<owner>.github.io/<repo>/<relative>
bool location(const Worker& w, Utf8View relative, UpdateLocation* out) noexcept {
    const Utf8View repository{w.repository, w.repository_bytes};
    u32 slash = 0;
    while (slash < repository.bytes && repository.data[slash] != '/') ++slash;
    constexpr Utf8View pages = utf8_literal(".github.io");
    if (!slash || slash + pages.bytes > kUpdateHostNameCapacity) return false;
    UpdateLocation l{};
    l.host = UpdateHost::pages;
    for (u32 i = 0; i < slash; ++i) l.origin[i] = ascii_lower(repository.data[i]);
    copy_bytes(l.origin + slash, pages.data, pages.bytes);
    l.origin_bytes = slash + pages.bytes;
    TextWriter t{{l.target, kUpdateTargetCapacity}, 0, true};
    t.add("/", 1);
    t.add({repository.data + slash + 1, repository.bytes - slash - 1});
    t.add("/", 1);
    t.add(relative);
    if (!t.ok) return false;
    l.target_bytes = t.used;
    *out = l;
    return true;
}
struct ProgressSink {
    UpdateBufferSink buffer;
    Worker* w;
};
bool progress_sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto& s = *static_cast<ProgressSink*>(context);
    if (!update_buffer_sink(&s.buffer, bytes)) return false;
    InterlockedExchange64(&s.w->progress_bytes, s.buffer.used);
    return true;
}
CatalogError fetch_error(const UpdateFetchResult& r) noexcept {
    switch (r.error) {
    case UpdateFetchError::rate_limited: return CatalogError::rate_limited;
    case UpdateFetchError::oversize:
    case UpdateFetchError::size: return CatalogError::download;
    default: return CatalogError::unreachable;
    }
}
// Fetches relative into a new allocation of exactly `exact` bytes (or at most `maximum` when exact is 0).
CatalogError fetch(Worker& w, Network& net, const Utf8View& relative, u32 maximum, u32 exact, u8** out,
                   u32* bytes, u64* retry_at = nullptr) noexcept {
    UpdateLocation start{};
    if (!net.open() || !location(w, relative, &start)) return CatalogError::unreachable;
    ProgressSink sink{{commit_pages<u8>(exact ? exact : maximum), exact ? exact : maximum, 0}, &w};
    if (!sink.buffer.data) return CatalogError::storage;
    InterlockedExchange64(&w.progress_bytes, 0);
    InterlockedExchange64(&w.progress_total, exact);
    const auto result = update_fetch(net.transport, start, sink.buffer.capacity, exact, progress_sink, &sink, now_unix(w));
    if (!result.ok()) {
        release(sink.buffer.data);
        if (retry_at) *retry_at = result.retry_at_unix;
        UpdateLog(kLog, "fetch").field("error", static_cast<u8>(result.error)).field("status", result.status).write(LogLevel::warning);
        return fetch_error(result);
    }
    *out = sink.buffer.data;
    *bytes = sink.buffer.used;
    return CatalogError::none;
}

// ---- the index -----------------------------------------------------------------------------------
struct Keys {
    P256Key keys[2];
    const P256Key* list[2];
    u32 count;
    ~Keys() { for (P256Key& key : keys) p256_release(&key); }
    bool open(const Worker& w) noexcept {
        for (u32 i = 0; i < w.key_count; ++i)
            if (p256_import_public(&keys[count], P256Kind::ecdsa, {w.keys[i], 65}).ok()) list[count] = &keys[count], ++count;
        return count != 0;
    }
};
// Verifies sig || document against the compiled keys and the replay floor.
CatalogError verify(Worker& w, Crypto& crypto, const CatalogJob& job, const ConstSlice<u8>& signature,
                    const Utf8View& document, bool accept_expired, CatalogIndex** out) noexcept {
    Keys keys{};
    if (!keys.open(w) || !crypto.open()) return CatalogError::not_configured;
    auto* index = commit_pages<CatalogIndex>();
    auto* scratch = commit_pages<u32>(kCatalogIndexStructuralCapacity * sizeof(u32));
    CatalogError error = CatalogError::none;
    if (!index || !scratch) {
        error = CatalogError::storage;
    } else {
        const auto verified = catalog_index_verify(document, signature, {keys.list, keys.count}, crypto.workspace, now_unix(w),
                                                   accept_expired, {scratch, kCatalogIndexStructuralCapacity}, index);
        if (!verified.ok()) {
            UpdateLog(kLog, "index_refused").field("error", static_cast<u8>(verified.error)).write(LogLevel::warning);
            error = verified.error == CatalogIndexError::signature ? CatalogError::signature
                  : verified.error == CatalogIndexError::expired ? CatalogError::expired
                                                                : CatalogError::invalid_index;
        } else if (index->sequence < job.floor_sequence ||
                   (index->sequence == job.floor_sequence && job.floor_sequence &&
                    !bytes_equal(index->digest, job.floor_digest, 32))) {
            UpdateLog(kLog, "index_replay").field("sequence", index->sequence).field("floor", job.floor_sequence).write(LogLevel::warning);
            error = CatalogError::replay;
        }
    }
    release(scratch);
    if (error != CatalogError::none) {
        release(index);
        return error;
    }
    *out = index;
    return CatalogError::none;
}
void load_cache(Worker& w, const CatalogJob& job, CatalogJobResult& r) noexcept {
    wchar_t path[600];
    u32 bytes = 0;
    u8* record = catalog_path(base(w), L"catalog.bin", path) ? read_file(path, kSignatureBytes + kCatalogIndexMaximumBytes, &bytes) : nullptr;
    if (!record || bytes <= kSignatureBytes) {
        release(record);
        r.error = CatalogError::not_listed;
        return;
    }
    Crypto crypto{};
    r.error = verify(w, crypto, job, {record, kSignatureBytes},
                     {reinterpret_cast<const char*>(record + kSignatureBytes), bytes - kSignatureBytes}, true, &r.index);
    release(record);
}
void check(Worker& w, const CatalogJob& job, CatalogJobResult& r) noexcept {
    Network net{&w};
    u8 *document = nullptr, *signature = nullptr;
    u32 document_bytes = 0, signature_bytes = 0;
    // index-3.json lists every item with the interface it needs (catalog_index.hpp). index-full.json (0.6.3) and
    // index.json (0.6.0 to 0.6.2) leave out what those can't read and are signed one and two sequences lower, so
    // each update moves forward.
    r.error = fetch(w, net, utf8_literal("index-3.json"), kCatalogIndexMaximumBytes, 0, &document, &document_bytes, &r.retry_at_unix);
    if (r.error == CatalogError::none)
        r.error = fetch(w, net, utf8_literal("index-3.json.sig"), kSignatureBytes, kSignatureBytes, &signature, &signature_bytes,
                        &r.retry_at_unix);
    Crypto crypto{};
    if (r.error == CatalogError::none)
        r.error = verify(w, crypto, job, {signature, signature_bytes}, {reinterpret_cast<const char*>(document), document_bytes},
                         false, &r.index);
    if (r.error == CatalogError::none) {
        // The cache is the signature, then the document: one file, so the pair can't tear.
        auto* record = commit_pages<u8>(kSignatureBytes + document_bytes);
        wchar_t path[600];
        if (record && catalog_path(base(w), L"catalog.bin", path) && directory_ensure(w.base)) {
            copy_bytes(record, signature, kSignatureBytes);
            copy_bytes(record + kSignatureBytes, document, document_bytes);
            if (!write_file(path, {record, kSignatureBytes + document_bytes}))
                UpdateLog(kLog, "cache_write_failed").field("error", GetLastError()).write(LogLevel::warning);
        }
        release(record);
        UpdateLog(kLog, "index_accepted").field("sequence", r.index->sequence).field("items", r.index->item_count).write();
    }
    release(document);
    release(signature);
}

// ---- files the index lists -------------------------------------------------------------------------
static_assert(static_cast<u8>(CatalogKind::theme) == static_cast<u8>(PackageLinkKind::theme) &&
              static_cast<u8>(CatalogKind::effect) == static_cast<u8>(PackageLinkKind::effect));
u32 file_path(const Worker& w, CatalogKind kind, Utf8View slug, const u8 (&sha256)[32], wchar_t (&path)[600]) noexcept {
    if (kind == CatalogKind::plugin) {
        wchar_t dll[69];
        catalog_plugin_leaf(sha256, dll);
        return catalog_path(base(w), L"plugins\\", path) ? append(path, wide_length(path, 600), dll) : 0;
    }
    // The same files the package library installs and prunes: themes\<slug>.maicord-theme.json and
    // effects\<slug>-<sha16>.mcfx (PackageLinkKind has the catalog's order).
    u32 units = 0;
    return package_file_path(w.base, static_cast<PackageLinkKind>(kind), slug, sha256, {path, 600}, &units) ? units : 0;
}
void download(Worker& w, const CatalogJob& job, CatalogJobResult& r) noexcept {
    char relative[96];
    const u32 relative_bytes = catalog_file_name(job.item_kind, job.blob.sha256, {relative, sizeof(relative)});
    wchar_t path[600];
    const Utf8View slug{job.slug, job.slug_bytes};
    const wchar_t* directory = job.item_kind == CatalogKind::plugin ? L"plugins" : job.item_kind == CatalogKind::theme ? L"themes" : L"effects";
    copy_bytes(r.sha256, job.blob.sha256, 32);
    if (!relative_bytes || !ensure(w, directory, path) || !file_path(w, job.item_kind, slug, job.blob.sha256, path)) {
        r.error = CatalogError::storage;
        return;
    }
    Crypto crypto{};
    // Content-addressed: a file already there with the right bytes is kept (it may be loaded).
    u32 existing_bytes = 0;
    if (u8* existing = read_file(path, job.blob.bytes, &existing_bytes)) {
        const bool same = existing_bytes == job.blob.bytes && crypto.matches({existing, existing_bytes}, job.blob.sha256);
        release(existing);
        if (same) return;
    }
    Network net{&w};
    u8* bytes = nullptr;
    u32 count = 0;
    r.error = fetch(w, net, {relative, relative_bytes}, job.blob.bytes, job.blob.bytes, &bytes, &count);
    if (r.error != CatalogError::none) return;
    if (!crypto.matches({bytes, count}, job.blob.sha256)) r.error = CatalogError::digest;
    else if (job.item_kind != CatalogKind::plugin && (!w.validate_package || !w.validate_package(job.item_kind, {bytes, count}, slug)))
        r.error = CatalogError::package;
    else if (!write_file(path, {bytes, count})) r.error = CatalogError::storage;
    release(bytes);
    UpdateLog(kLog, "download").field("kind", static_cast<u8>(job.item_kind)).field("bytes", count)
        .field("error", static_cast<u8>(r.error)).write(r.error == CatalogError::none ? LogLevel::info : LogLevel::warning);
    if (r.error != CatalogError::none || job.item_kind == CatalogKind::plugin) return;
    if (job.item_kind == CatalogKind::effect) package_prune(w.base, slug, job.blob.sha256);
    if (w.library_changed) w.library_changed();
}

// ---- details and previews -----------------------------------------------------------------------------
void trim_assets(const Worker& w) noexcept {
    wchar_t pattern[600];
    u32 length = catalog_path(base(w), L"catalog-cache\\", pattern);
    if (!length || !wide_append(pattern, 600, &length, L"*")) return;
    // Oldest first until the cache is under its cap; at most a few hundred files.
    for (u32 round = 0; round < 64; ++round) {
        WIN32_FIND_DATAW found{}, oldest{};
        u64 total = 0;
        bool any = false;
        const HANDLE scan = FindFirstFileW(pattern, &found);
        if (scan == INVALID_HANDLE_VALUE) return;
        do {
            if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            total += (static_cast<u64>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
            const u64 when = (static_cast<u64>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
            const u64 best = (static_cast<u64>(oldest.ftLastWriteTime.dwHighDateTime) << 32) | oldest.ftLastWriteTime.dwLowDateTime;
            if (!any || when < best) oldest = found, any = true;
        } while (FindNextFileW(scan, &found));
        FindClose(scan);
        if (total <= kAssetCacheBytes || !any) return;
        wchar_t path[600];
        if (!catalog_path(base(w), L"catalog-cache\\", path) || !append(path, wide_length(path, 600), oldest.cFileName) ||
            !DeleteFileW(path))
            return;
    }
}
void asset(Worker& w, const CatalogJob& job, CatalogJobResult& r) noexcept {
    copy_bytes(r.sha256, job.blob.sha256, 32);
    wchar_t path[600], leaf[65];
    hex_wide(job.blob.sha256, 32, leaf);
    leaf[64] = 0;
    Crypto crypto{};
    u32 count = 0;
    u8* bytes = nullptr;
    if (ensure(w, L"catalog-cache", path) && catalog_path(base(w), L"catalog-cache\\", path) &&
        append(path, wide_length(path, 600), leaf)) {
        bytes = read_file(path, job.blob.bytes, &count);
        if (bytes && (count != job.blob.bytes || !crypto.matches({bytes, count}, job.blob.sha256))) {
            release(bytes);
            bytes = nullptr;
        }
    }
    if (!bytes) {
        char relative[96];
        char digest[64];
        hex(job.blob.sha256, 32, digest);
        TextWriter t{{relative, sizeof(relative)}, 0, true};
        t.add(job.asset == CatalogAssetKind::details ? utf8_literal("details/") : utf8_literal("previews/"));
        t.add(digest, 64);
        t.add(job.asset == CatalogAssetKind::details ? utf8_literal(".md") : utf8_literal(".png"));
        Network net{&w};
        r.error = fetch(w, net, {relative, t.used}, job.blob.bytes, job.blob.bytes, &bytes, &count);
        if (r.error != CatalogError::none) return;
        if (!crypto.matches({bytes, count}, job.blob.sha256)) {
            release(bytes);
            r.error = CatalogError::digest;
            return;
        }
        if (path[0] && write_file(path, {bytes, count})) trim_assets(w);
    }
    r.bytes = bytes;
    r.count = count;
}

// ---- plugin data and sideloads -------------------------------------------------------------------------
void delete_data(const Worker& w, const CatalogJob& job) noexcept {
    wchar_t directory[600], pattern[600];
    u32 length = catalog_path(base(w), L"plugin-data\\", directory);
    if (!length || !wide_append_hex(directory, 600, &length, job.owner)) return;
    copy_bytes(pattern, directory, (length + 1) * 2);
    u32 pattern_length = length;
    if (!wide_append(pattern, 600, &pattern_length, L"\\*")) return;
    WIN32_FIND_DATAW found{};
    const HANDLE scan = FindFirstFileW(pattern, &found);
    if (scan != INVALID_HANDLE_VALUE) {
        do {
            if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            wchar_t path[600];
            copy_bytes(path, directory, (length + 1) * 2);
            if (append(path, length, L"\\") && append(path, length + 1, found.cFileName)) DeleteFileW(path);
        } while (FindNextFileW(scan, &found));
        FindClose(scan);
    }
    RemoveDirectoryW(directory);
}
void copy(Worker& w, const CatalogJob& job, CatalogJobResult& r) noexcept {
    wchar_t source[521];
    copy_bytes(source, job.path, job.path_characters * 2);
    source[job.path_characters] = 0;
    u32 count = 0;
    u8* bytes = read_file(source, kPluginDllMaximumBytes, &count);
    Crypto crypto{};
    wchar_t path[600], leaf[69];
    if (!bytes || !crypto.digest({bytes, count}, r.sha256)) r.error = CatalogError::plugin_load;
    else {
        catalog_plugin_leaf(r.sha256, leaf);
        // Content-addressed: a file of this name is this build (the loader checks the digest), and one that is
        // loaded right now (a dev folder build that did not change) can't be written over.
        if (!ensure(w, L"plugins", path) || !catalog_path(base(w), L"plugins\\", path) ||
            !append(path, wide_length(path, 600), leaf) ||
            (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && !write_file(path, {bytes, count})))
            r.error = CatalogError::storage;
    }
    release(bytes);
}

void run(Worker& w, const CatalogJob& job, CatalogJobResult& r) noexcept {
    switch (job.kind) {
    case CatalogJobKind::load_cache: load_cache(w, job, r); break;
    case CatalogJobKind::check: check(w, job, r); break;
    case CatalogJobKind::download: download(w, job, r); break;
    case CatalogJobKind::asset: asset(w, job, r); break;
    case CatalogJobKind::delete_data: delete_data(w, job); break;
    case CatalogJobKind::copy: copy(w, job, r); break;
    default: break;
    }
}
void free_result(CatalogJobResult& r) noexcept {
    release(r.index);
    release(r.bytes);
}

DWORD WINAPI worker_main(void* parameter) noexcept {
    auto& w = *static_cast<Worker*>(parameter);
    for (;;) {
        AcquireSRWLockExclusive(w.lock.native());
        const bool have = w.job_count != 0;
        CatalogJob job{};
        if (have) {
            job = w.jobs[w.job_first];
            w.job_first = (w.job_first + 1) % kQueue;
            --w.job_count;
        }
        ReleaseSRWLockExclusive(w.lock.native());
        if (!have) {
            HANDLE handles[]{w.stop, w.wake};
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            continue;
        }
        if (WaitForSingleObject(w.stop, 0) == WAIT_OBJECT_0) break;
        CatalogJobResult result{};
        result.kind = job.kind;
        result.serial = job.serial;
        run(w, job, result);
        InterlockedExchange64(&w.progress_total, 0);
        AcquireSRWLockExclusive(w.lock.native());
        // The UI takes results every pass; a full queue only happens while it is gone.
        if (w.result_count == kQueue) {
            free_result(w.results[w.result_first]);
            w.result_first = (w.result_first + 1) % kQueue;
            --w.result_count;
        }
        w.results[(w.result_first + w.result_count++) % kQueue] = result;
        ReleaseSRWLockExclusive(w.lock.native());
        SetEvent(w.notify);
    }
    return 0;
}
} // namespace

u32 catalog_path(const ConstSlice<wchar_t>& base, const wchar_t* relative,
                 wchar_t (&output)[600]) noexcept {
    if (!base.data || !base.count || base.count >= 500) return 0;
    copy_bytes(output, base.data, base.count * 2);
    output[base.count] = 0;
    u32 length = base.count;
    return wide_append(output, 600, &length, L"\\") && wide_append(output, 600, &length, relative) ? length : 0;
}

void catalog_plugin_leaf(const u8 (&sha256)[32], wchar_t (&leaf)[69]) noexcept {
    hex_wide(sha256, 32, leaf);
    copy_bytes(leaf + 64, L".dll", 5 * sizeof(wchar_t));
}

bool catalog_worker_start(const CatalogWorkerOptions& options) noexcept {
    if (g_worker) return true;
    if (!options.base.count || options.base.count >= 480 || options.repository.bytes > kUpdateRepositoryMaximumBytes ||
        options.keys.count > 2)
        return false;
    auto* w = commit_pages<Worker>();
    if (!w) return false;
    copy_bytes(w->base, options.base.data, options.base.count * 2);
    w->base_characters = options.base.count;
    copy_bytes(w->repository, options.repository.data, options.repository.bytes);
    w->repository_bytes = options.repository.bytes;
    for (u32 i = 0; i < options.keys.count; ++i) copy_bytes(w->keys[w->key_count++], options.keys[i], 65);
    w->transport = options.transport;
    w->clock_unix = options.clock_unix;
    w->validate_package = options.validate_package;
    w->library_changed = options.library_changed;
    w->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (w->wake && w->stop && w->notify)
        w->thread = CreateThread(nullptr, 256 * 1024, worker_main, w, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!w->thread) {
        const HANDLE events[]{w->wake, w->stop, w->notify};
        for (HANDLE h : events) if (h) CloseHandle(h);
        release(w);
        return false;
    }
    g_worker = w;
    return true;
}

void catalog_worker_stop() noexcept {
    Worker* w = g_worker;
    if (!w) return;
    g_worker = nullptr;
    SetEvent(w->stop);
    if (UpdateHttpsTransport* https = w->https) update_https_cancel(https);
    // A worker that does not stop keeps its memory: it may still be writing to it.
    if (WaitForSingleObject(w->thread, 5000) != WAIT_OBJECT_0) return;
    CloseHandle(w->thread);
    for (u32 i = 0; i < w->result_count; ++i) free_result(w->results[(w->result_first + i) % kQueue]);
    const HANDLE events[]{w->wake, w->stop, w->notify};
    for (HANDLE h : events) CloseHandle(h);
    release(w);
}

HANDLE catalog_worker_notification() noexcept { return g_worker ? g_worker->notify : nullptr; }

bool catalog_worker_submit(const CatalogJob& job) noexcept {
    Worker* w = g_worker;
    if (!w) return false;
    AcquireSRWLockExclusive(w->lock.native());
    const bool room = w->job_count < kQueue;
    if (room) w->jobs[(w->job_first + w->job_count++) % kQueue] = job;
    ReleaseSRWLockExclusive(w->lock.native());
    if (room) SetEvent(w->wake);
    return room;
}

bool catalog_worker_take(CatalogJobResult* result) noexcept {
    Worker* w = g_worker;
    if (!w) return false;
    AcquireSRWLockExclusive(w->lock.native());
    const bool have = w->result_count != 0;
    if (have) {
        *result = w->results[w->result_first];
        w->result_first = (w->result_first + 1) % kQueue;
        --w->result_count;
    }
    ReleaseSRWLockExclusive(w->lock.native());
    return have;
}

void catalog_worker_progress(u64* bytes, u64* total) noexcept {
    Worker* w = g_worker;
    *bytes = w ? static_cast<u64>(InterlockedCompareExchange64(&w->progress_bytes, 0, 0)) : 0;
    *total = w ? static_cast<u64>(InterlockedCompareExchange64(&w->progress_total, 0, 0)) : 0;
}

void catalog_worker_free(void* memory) noexcept { release(memory); }

} // namespace maicord::app_detail
