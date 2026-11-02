#include "catalog_service.hpp"
#include "catalog_worker.hpp"
#include "maicord/app/plugins/consent.hpp"
#include "maicord/app/plugins/http.hpp"
#include "maicord/app/update/log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sdk/plugin_abi.h"

namespace maicord::app_detail {
namespace {
constexpr char kLog[] = "catalog";
constexpr u64 kCheckIntervalSeconds = 86400;
constexpr u64 kBrowseRefreshSeconds = 3600;
constexpr u64 kRetrySeconds = 3600;
constexpr u64 kFirstCheckDelaySeconds = 180;
constexpr u32 kAssetSlots = 16;

struct Runtime {
    PluginRuntime runtime;
    PluginCallLedger ledger;
    bool open;
};
struct Operation {
    CatalogAction action;
    CatalogKind kind;
    u8 slug_bytes;
    char slug[kCatalogSlugMaximumBytes];
    u32 version;     // into index->versions
    u64 job;         // the worker job in flight
    u8 sha256[32];   // the file being installed
    bool update, enable_after;
};
struct AssetSlot {
    u8 sha256[32];
    u8* bytes;
    u32 count;
    u64 job;
    bool used, ready, failed;
};
struct Service {
    CatalogServiceHost host;
    wchar_t base[480];
    u32 base_characters;
    char repository[kUpdateRepositoryMaximumBytes];
    u32 repository_bytes;
    u8 keys[2][65];
    u32 key_count;
    UpdateTransport transport;
    u64 (*clock_unix)() noexcept;
    wchar_t state_path[600];
    u32 state_path_characters;
    CatalogState state;
    CatalogIndex* index;
    CatalogView view;
    Runtime* runtime;
    Operation op;
    CatalogConsent consent;
    PluginConsentRequest consent_request;
    PluginImage consent_image;
    Utf8View consent_hosts[MC_NETWORK_HOSTS_MAX];
    AssetSlot assets[kAssetSlots];
    u64 serial, check_job, cache_job, next_check_unix, attempted_sequence;
    u64 attempted[kCatalogStateMaximumEntries];
    u32 attempted_count;
    // Approved hosts each binding had when last compared (settings-page host switches).
    u32 host_snapshot[kPluginMaximumBindings];
    bool restored, browsing, worker;
    bool auto_due; // something changed that may make an automatic update due
};
Service* g;

template <typename T> T* allocate() noexcept {
    return static_cast<T*>(VirtualAlloc(nullptr, sizeof(T), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
u64 now_unix() noexcept { return g->clock_unix ? g->clock_unix() : clock_unix_milliseconds() / 1000; }
ConstSlice<wchar_t> base() noexcept { return {g->base, g->base_characters}; }
bool configured() noexcept { return g->key_count != 0 && g->repository_bytes != 0; }
void bump() noexcept { ++g->view.revision; }
Utf8View slug_of(const Operation& op) noexcept { return {op.slug, op.slug_bytes}; }
void toast(CatalogError error) noexcept {
    if (g->host.toast) g->host.toast(application_catalog_error_text(error), true);
}

// ---- state ----------------------------------------------------------------------------------------
bool save_state() noexcept {
    const auto saved = catalog_state_save(g->state_path, g->state_path_characters, g->state);
    if (!saved.ok()) UpdateLog(kLog, "state_save_failed").field("error", static_cast<u8>(saved.error)).field("native", saved.native_error).write(LogLevel::error);
    bump();
    return saved.ok();
}
bool catalog_item(const CatalogStateEntry& e) noexcept {
    return !(e.flags & (kCatalogEntryUnreviewed | kCatalogEntrySideloaded));
}
bool catalog_installed() noexcept {
    for (u32 i = 0; i < g->state.count; ++i) if (catalog_item(g->state.entries[i])) return true;
    return false;
}
u64 entry_code(const CatalogStateEntry& e) noexcept {
    u64 code = 0;
    return catalog_version_code(catalog_entry_version(e), &code) ? code : 0;
}

// ---- worker ---------------------------------------------------------------------------------------
bool ensure_worker() noexcept {
    if (g->worker) return true;
    const u8* keys[2]{g->keys[0], g->keys[1]};
    CatalogWorkerOptions options{base(), {g->repository, g->repository_bytes}, {keys, g->key_count}, g->transport,
                                 g->clock_unix, g->host.validate_package, g->host.library_changed};
    g->worker = catalog_worker_start(options);
    return g->worker;
}
u64 submit(CatalogJob job) noexcept {
    if (!ensure_worker()) return 0;
    job.serial = ++g->serial;
    job.floor_sequence = g->state.sequence;
    copy_bytes(job.floor_digest, g->state.digest, 32);
    return catalog_worker_submit(job) ? job.serial : 0;
}
void load_cache() noexcept {
    if (g->index || g->cache_job || !configured()) return;
    g->cache_job = submit({CatalogJobKind::load_cache});
}
bool check() noexcept {
    if (!configured()) return false;
    if (g->check_job) return true;
    g->check_job = submit({CatalogJobKind::check});
    g->view.fetching = g->check_job != 0;
    bump();
    return g->check_job != 0;
}

// ---- the runtime -------------------------------------------------------------------------------------
Runtime* runtime() noexcept {
    if (g->runtime && g->runtime->open) return g->runtime;
    if (!g->runtime && !(g->runtime = allocate<Runtime>())) return nullptr;
    wchar_t plugins[600], data[600], ledger[600];
    const u32 plugins_characters = catalog_path(base(), L"plugins", plugins);
    const u32 data_characters = catalog_path(base(), L"plugin-data", data);
    if (!plugins_characters || !data_characters || !catalog_path(base(), L"plugin-calls.bin", ledger) ||
        !directory_ensure(g->base) || !directory_ensure(plugins))
        return nullptr;
    Runtime& r = *g->runtime;
    // A marker left in a call by a process that died: that plugin starts off.
    PluginCallMarker leftover{};
    if (plugin_call_ledger_open(&r.ledger, ledger, &leftover)) plugin_call_watchdog_start(&r.ledger);
    if (leftover.present) {
        if (CatalogStateEntry* e = catalog_state_find_owner(g->state, leftover.owner_id)) {
            e->flags |= leftover.hung ? kCatalogEntryHung : kCatalogEntryCrashed;
            UpdateLog(kLog, "plugin_crashed").hex("owner", leftover.owner_id).field("hung", leftover.hung).write(LogLevel::warning);
            save_state();
        }
    }
    const auto opened = plugin_runtime_open(&r.runtime, {plugins, plugins_characters}, {data, data_characters}, &r.ledger);
    if (!opened.ok()) {
        UpdateLog(kLog, "runtime_open_failed").field("error", static_cast<u8>(opened.error)).field("native", opened.win32_error).write(LogLevel::error);
        plugin_call_ledger_close(&r.ledger);
        return nullptr;
    }
    r.runtime.toast = g->host.plugin_toast;
    r.open = true;
    return &r;
}
PluginBinding* binding_of(u64 owner) noexcept {
    if (!g->runtime || !g->runtime->open || !owner) return nullptr;
    PluginRuntime& r = g->runtime->runtime;
    for (u32 i = 0; i < r.binding_count; ++i) if (r.bindings[i]->owner_id == owner) return r.bindings[i];
    return nullptr;
}
// The host switches of the plugin's settings page are remembered here.
void sync_hosts(CatalogStateEntry& e) noexcept {
    if (const PluginBinding* b = binding_of(e.owner_id)) e.host_bits = static_cast<u8>(b->approved_hosts);
}
bool disable(CatalogStateEntry& e) noexcept {
    PluginBinding* b = binding_of(e.owner_id);
    if (!b) return true;
    sync_hosts(e);
    return plugin_runtime_disable(&g->runtime->runtime, b);
}
CatalogError enable(CatalogStateEntry& e) noexcept {
    if (e.kind != CatalogKind::plugin) return CatalogError::none;
    if (binding_of(e.owner_id)) return CatalogError::none;
    if (e.flags & kCatalogEntryRevoked) return CatalogError::revoked;
    Runtime* r = runtime();
    if (!r) return CatalogError::storage;
    wchar_t leaf[69];
    catalog_plugin_leaf(e.sha256, leaf);
    const auto enabled = plugin_runtime_enable(&r->runtime, {leaf, 68}, e.approved_permissions, e.host_bits, e.sha256);
    if (enabled.ok()) return CatalogError::none;
    UpdateLog(kLog, "plugin_enable_failed").hex("owner", e.owner_id).field("error", static_cast<u8>(enabled.error))
        .field("load", static_cast<u8>(enabled.load.error)).field("problem", static_cast<u8>(enabled.load.problem)).write(LogLevel::warning);
    return enabled.load.error == PluginLoadError::digest ? CatalogError::digest : CatalogError::enable_failed;
}
void delete_plugin_file(const u8 (&sha256)[32]) noexcept {
    // Content-addressed: another entry may still name the same build.
    for (u32 i = 0; i < g->state.count; ++i) {
        const CatalogStateEntry& e = g->state.entries[i];
        if (e.kind == CatalogKind::plugin && (bytes_equal(e.sha256, sha256, 32) || bytes_equal(e.previous_sha256, sha256, 32))) return;
    }
    wchar_t path[600], leaf[69];
    catalog_plugin_leaf(sha256, leaf);
    if (catalog_path(base(), L"plugins\\", path)) {
        u32 length = wide_length(path, 600);
        if (wide_append(path, 600, &length, leaf)) DeleteFileW(path);
    }
}
// Plugin files nothing names any more: a crash between writing a file and recording it leaves one behind.
void delete_unreferenced_plugins() noexcept {
    wchar_t pattern[600];
    u32 length = catalog_path(base(), L"plugins\\*.dll", pattern);
    if (!length) return;
    WIN32_FIND_DATAW found{};
    const HANDLE scan = FindFirstFileW(pattern, &found);
    if (scan == INVALID_HANDLE_VALUE) return;
    do {
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || wide_length(found.cFileName, 260) != 68)
            continue;
        u8 sha[32];
        bool valid = true;
        for (u32 i = 0; i < 64 && valid; ++i) {
            const wchar_t c = found.cFileName[i];
            const i32 nibble = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
            valid = nibble >= 0;
            sha[i / 2] = static_cast<u8>(i % 2 ? sha[i / 2] << 4 | nibble : nibble);
        }
        if (valid) delete_plugin_file(sha);
    } while (FindNextFileW(scan, &found));
    FindClose(scan);
}

// ---- the index ------------------------------------------------------------------------------------------
u32 item_of(CatalogKind kind, Utf8View slug) noexcept { return g->index ? catalog_index_find(*g->index, kind, slug) : kCatalogNone; }
ConstSlice<Utf8View> version_hosts(const CatalogVersion& v, Utf8View (&hosts)[MC_NETWORK_HOSTS_MAX]) noexcept {
    for (u32 i = 0; i < v.host_count && i < MC_NETWORK_HOSTS_MAX; ++i) hosts[i] = catalog_text(*g->index, g->index->hosts[v.first_host + i]);
    return {hosts, minimum<u32>(v.host_count, MC_NETWORK_HOSTS_MAX)};
}
bool needs_review(const CatalogStateEntry& e, const CatalogVersion& v) noexcept {
    if (e.kind != CatalogKind::plugin) return false;
    Utf8View hosts[MC_NETWORK_HOSTS_MAX];
    const auto list = version_hosts(v, hosts);
    const u32 all = list.count ? (1u << list.count) - 1 : 0;
    return (v.permissions & ~e.approved_permissions) || catalog_state_approved_hosts(e, list) != all;
}
// What revocations say about installed items: malicious builds never run again, broken ones are turned off.
void apply_revocations() noexcept {
    bool changed = false;
    for (u32 i = 0; i < g->state.count; ++i) {
        CatalogStateEntry& e = g->state.entries[i];
        const CatalogRevocation* entry = nullptr;
        const auto severity = catalog_index_revocation(*g->index, e.sha256, &entry);
        if (e.previous_version_bytes && catalog_index_revocation(*g->index, e.previous_sha256) != CatalogRevocationSeverity::none &&
            catalog_index_revocation(*g->index, e.previous_sha256) != CatalogRevocationSeverity::withdrawn) {
            clear_bytes(e.previous_sha256, 32);
            clear_bytes(e.previous_version, sizeof(e.previous_version));
            e.previous_version_bytes = 0;
            e.previous_host_bits = 0;
            changed = true;
        }
        if (severity != CatalogRevocationSeverity::malicious && severity != CatalogRevocationSeverity::broken) continue;
        const bool malicious = severity == CatalogRevocationSeverity::malicious;
        // Each revocation is acted on once; a broken build the user turned back on stays on.
        if (e.flags & (malicious ? kCatalogEntryRevoked : kCatalogEntryRevoked | kCatalogEntryBroken)) continue;
        if (e.kind == CatalogKind::plugin) {
            disable(e);
            e.flags &= static_cast<u16>(~kCatalogEntryEnabled);
        } else if (g->host.package_removed) {
            g->host.package_removed(e.kind, catalog_entry_slug(e), true);
        }
        e.flags |= malicious ? kCatalogEntryRevoked : kCatalogEntryBroken;
        changed = true;
        UpdateLog(kLog, "revoked").field("kind", static_cast<u8>(e.kind)).field("severity", static_cast<u8>(severity)).write(LogLevel::warning);
        if (g->host.toast) g->host.toast(malicious ? utf8_literal("MaiCord turned off an item that was found harmful.")
                                                   : utf8_literal("MaiCord turned off an item that stopped working. Update it to turn it back on."),
                                         true);
    }
    if (changed) save_state();
}
void set_index(CatalogIndex* index, bool fresh) noexcept {
    if (g->index) catalog_worker_free(g->index);
    g->index = index;
    g->view.index = index;
    g->view.installable = fresh && !index->expired;
    g->view.status = index->expired ? CatalogStatus::stale : CatalogStatus::ok;
    if (g->attempted_sequence != index->sequence) {
        g->attempted_sequence = index->sequence;
        g->attempted_count = 0;
    }
    apply_revocations();
    g->auto_due = true;
    bump();
}

// ---- operations --------------------------------------------------------------------------------------------
// Also before restore: nothing may write catalog-state.bin until it has been read.
bool busy() noexcept {
    const auto phase = g->view.operation.phase;
    return !g->restored || (phase != CatalogPhase::idle && phase != CatalogPhase::done && phase != CatalogPhase::failed);
}
void begin(CatalogAction action, CatalogKind kind, Utf8View slug) noexcept {
    reset_value(&g->op);
    g->op.action = action;
    g->op.kind = kind;
    g->op.slug_bytes = static_cast<u8>(minimum<u32>(slug.bytes, kCatalogSlugMaximumBytes));
    copy_bytes(g->op.slug, slug.data, g->op.slug_bytes);
    CatalogOperation& o = g->view.operation;
    reset_value(&o);
    o.action = action;
    o.kind = kind;
    o.slug_bytes = g->op.slug_bytes;
    copy_bytes(o.slug, g->op.slug, g->op.slug_bytes);
    o.phase = CatalogPhase::queued;
    bump();
}
bool finish(CatalogError error) noexcept {
    g->view.operation.phase = error == CatalogError::none ? CatalogPhase::done : CatalogPhase::failed;
    g->view.operation.error = error;
    g->auto_due = true;
    if (error != CatalogError::none && error != CatalogError::declined) {
        toast(error);
        UpdateLog(kLog, "operation_failed").field("action", static_cast<u8>(g->op.action)).field("kind", static_cast<u8>(g->op.kind))
            .field("error", static_cast<u8>(error)).write(LogLevel::warning);
    }
    bump();
    return error == CatalogError::none;
}
void unload_consent_image() noexcept {
    if (g->consent_image.module) plugin_image_unload(&g->consent_image);
    reset_value(&g->consent);
}
// Starts a download of a listed version.
bool download(CatalogAction action, CatalogKind kind, Utf8View slug, u32 item, u32 version) noexcept {
    if (busy()) return false;
    begin(action, kind, slug);
    g->op.version = version;
    g->op.update = action == CatalogAction::update;
    const CatalogVersion& v = g->index->versions[version];
    copy_bytes(g->op.sha256, v.file.sha256, 32);
    CatalogJob job{CatalogJobKind::download};
    job.item_kind = kind;
    job.slug_bytes = g->op.slug_bytes;
    copy_bytes(job.slug, g->op.slug, g->op.slug_bytes);
    job.blob = v.file;
    (void)item;
    g->op.job = submit(job);
    if (!g->op.job) return finish(CatalogError::busy);
    g->view.operation.phase = CatalogPhase::downloading;
    g->view.operation.total = v.file.bytes;
    bump();
    return true;
}
// Records an approved plugin build and runs it.
bool apply_plugin(u64 permissions, u32 host_bits, bool enable_now) noexcept {
    const u64 owner = g->consent_image.owner_id;
    Utf8View version = {};
    char version_text[kCatalogVersionTextMaximumBytes + 1];
    if (g->op.action == CatalogAction::sideload) {
        const u32 bytes = minimum<u32>(g->consent_image.info.version_bytes, kCatalogVersionTextMaximumBytes);
        copy_bytes(version_text, g->consent_image.info.version, bytes);
        u64 code = 0;
        version = catalog_version_code({version_text, bytes}, &code) ? Utf8View{version_text, bytes} : utf8_literal("0.0.0");
    } else {
        version = catalog_text(*g->index, g->index->versions[g->op.version].text);
    }
    // The descriptor's hosts live in the image: copy them before it is unloaded.
    char host_text[MC_NETWORK_HOSTS_MAX * MC_HOST_MAX];
    Utf8View hosts[MC_NETWORK_HOSTS_MAX];
    const mc_descriptor& d = *g->consent_image.descriptor;
    u32 used = 0;
    for (u32 i = 0; i < d.network_count; ++i) {
        copy_bytes(host_text + used, d.network[i].data, d.network[i].bytes);
        hosts[i] = {host_text + used, d.network[i].bytes};
        used += d.network[i].bytes;
    }
    const ConstSlice<Utf8View> host_list{hosts, d.network_count};
    unload_consent_image();
    g->view.operation.phase = CatalogPhase::applying;
    CatalogStateEntry* e = catalog_state_find(g->state, CatalogKind::plugin, slug_of(g->op));
    const bool existed = e != nullptr;
    if (!e && !(e = catalog_state_add(g->state, CatalogKind::plugin, slug_of(g->op)))) return finish(CatalogError::capacity);
    const bool was_running = existed && binding_of(e->owner_id);
    if (existed && !disable(*e)) return finish(CatalogError::busy);
    const bool keep_previous = existed && g->op.action == CatalogAction::update;
    catalog_state_install(e, version, g->op.sha256, now_unix(), keep_previous);
    e->owner_id = owner;
    catalog_state_approve(e, permissions, host_list, host_bits);
    e->host_bits = static_cast<u8>(host_bits);
    e->flags &= static_cast<u16>(~(kCatalogEntryOffFlags | kCatalogEntryBroken));
    if (g->op.action == CatalogAction::sideload) e->flags |= kCatalogEntrySideloaded;
    const bool run = g->op.update ? was_running || (e->flags & kCatalogEntryEnabled) : enable_now;
    if (run) e->flags |= kCatalogEntryEnabled;
    else e->flags &= static_cast<u16>(~kCatalogEntryEnabled);
    if (!save_state()) return finish(CatalogError::storage);
    if (!run) return finish(CatalogError::none);
    const CatalogError enabled = enable(*e);
    if (enabled != CatalogError::none && keep_previous && catalog_entry_has_previous(*e)) {
        // The new build would not start: go back to the one that did.
        catalog_state_swap_previous(e);
        save_state();
        enable(*e);
    }
    bump();
    return finish(enabled);
}
// Checks a downloaded plugin against its listing (owner, permissions, hosts, API level, version) and asks
// for consent when it needs anything the user has not approved.
bool review_plugin() noexcept {
    Runtime* r = runtime();
    if (!r) return finish(CatalogError::storage);
    wchar_t leaf[69];
    catalog_plugin_leaf(g->op.sha256, leaf);
    unload_consent_image();
    const auto loaded = plugin_image_load(&g->consent_image, r->runtime.directory, {leaf, 68}, ~0ull, g->op.sha256);
    if (!loaded.ok()) {
        UpdateLog(kLog, "plugin_refused").field("error", static_cast<u8>(loaded.error)).field("problem", static_cast<u8>(loaded.problem)).write(LogLevel::warning);
        reset_value(&g->consent_image);
        delete_plugin_file(g->op.sha256);
        return finish(loaded.error == PluginLoadError::digest ? CatalogError::digest : CatalogError::plugin_load);
    }
    const PluginImage& image = g->consent_image;
    const mc_descriptor& d = *image.descriptor;
    CatalogStateEntry* e = catalog_state_find(g->state, CatalogKind::plugin, slug_of(g->op));
    CatalogStateEntry* same_owner = catalog_state_find_owner(g->state, image.owner_id);
    bool matches = true;
    if (g->op.action != CatalogAction::sideload) {
        const CatalogVersion& v = g->index->versions[g->op.version];
        Utf8View hosts[MC_NETWORK_HOSTS_MAX];
        const auto listed = version_hosts(v, hosts);
        matches = image.permissions == v.permissions && d.api_level == v.api_level && d.network_count == listed.count &&
                  utf8_equal({d.version.data, d.version.bytes}, catalog_text(*g->index, v.text));
        for (u32 i = 0; matches && i < listed.count; ++i) matches = utf8_equal({d.network[i].data, d.network[i].bytes}, listed[i]);
    } else {
        // A sideloaded build is recorded under its owner.
        static constexpr char digits[] = "0123456789abcdef";
        char slug[20] = "dev-";
        for (u32 i = 0; i < 16; ++i) slug[4 + i] = digits[(image.owner_id >> (60 - i * 4)) & 15];
        g->op.slug_bytes = 20;
        copy_bytes(g->op.slug, slug, 20);
        g->view.operation.slug_bytes = 20;
        copy_bytes(g->view.operation.slug, slug, 20);
        e = catalog_state_find(g->state, CatalogKind::plugin, slug_of(g->op));
        g->op.update = e != nullptr;
    }
    if (!matches || (e && e->owner_id != image.owner_id) || (same_owner && same_owner != e)) {
        const bool conflict = matches;
        unload_consent_image();
        delete_plugin_file(g->op.sha256);
        return finish(conflict ? CatalogError::owner_conflict : CatalogError::plugin_mismatch);
    }
    PluginConsentRequest request = plugin_consent_request(image, g->consent_hosts);
    request.approved_permissions = e ? e->approved_permissions : 0;
    request.approved_hosts = e ? catalog_state_approved_hosts(*e, request.hosts) : 0;
    const u32 all = request.hosts.count ? (1u << request.hosts.count) - 1 : 0;
    // Updates within the approval apply at once; installs and sideloads always ask.
    if (g->op.update && g->op.action != CatalogAction::sideload && !(request.permissions & ~request.approved_permissions) &&
        request.approved_hosts == all)
        return apply_plugin(request.permissions, all, true);
    g->consent_request = request;
    g->consent.request = &g->consent_request;
    g->consent.sideload = g->op.action == CatalogAction::sideload;
    g->consent.update = g->op.update;
    g->consent.bytes = image.image_bytes;
    // The store's consent dialog shows while the phase is consent and answers through application_catalog_consent_answer.
    g->view.operation.phase = CatalogPhase::consent;
    bump();
    return true;
}
bool apply_data() noexcept {
    g->view.operation.phase = CatalogPhase::applying;
    CatalogStateEntry* e = catalog_state_find(g->state, g->op.kind, slug_of(g->op));
    if (e && !catalog_item(*e)) {
        // A catalog item replaces the unreviewed copy of the same slug.
        e->flags &= static_cast<u16>(~kCatalogEntryUnreviewed);
    }
    if (!e && !(e = catalog_state_add(g->state, g->op.kind, slug_of(g->op)))) return finish(CatalogError::capacity);
    catalog_state_install(e, catalog_text(*g->index, g->index->versions[g->op.version].text), g->op.sha256, now_unix(), false);
    e->flags &= static_cast<u16>(~(kCatalogEntryOffFlags | kCatalogEntryBroken));
    return finish(save_state() ? CatalogError::none : CatalogError::storage);
}
void downloaded(const CatalogJobResult& r) noexcept {
    if (r.serial != g->op.job) return;
    g->op.job = 0;
    if (r.error != CatalogError::none) {
        finish(r.error);
        return;
    }
    if (g->op.action == CatalogAction::sideload) copy_bytes(g->op.sha256, r.sha256, 32);
    g->view.operation.phase = CatalogPhase::verifying;
    bump();
    // The index may have changed while the file came in: the version must still be the one offered.
    if (g->op.action != CatalogAction::sideload &&
        (!g->index || g->op.version >= g->index->version_count || !bytes_equal(g->index->versions[g->op.version].file.sha256, g->op.sha256, 32))) {
        finish(CatalogError::not_listed);
        return;
    }
    if (g->op.kind == CatalogKind::plugin) review_plugin();
    else apply_data();
}

// The next automatic update, one at a time.
void auto_update() noexcept {
    if (!g->auto_due || busy() || !g->index || !g->view.installable) return;
    const bool plugins_off = g->state.flags & kCatalogStateAutoUpdateOff;
    for (u32 i = 0; i < g->state.count; ++i) {
        const CatalogStateEntry& e = g->state.entries[i];
        if (!catalog_item(e) || (e.flags & (kCatalogEntryAutoUpdateOff | kCatalogEntryRevoked)) ||
            (e.kind == CatalogKind::plugin && plugins_off))
            continue;
        const u64 key = hash64_mix(hash64_counted_bytes(kHash64Seed, e.slug, e.slug_bytes), static_cast<u64>(e.kind));
        bool tried = false;
        for (u32 j = 0; j < g->attempted_count && !tried; ++j) tried = g->attempted[j] == key;
        if (tried) continue;
        const auto st = application_catalog_item_state(e.kind, catalog_entry_slug(e));
        if (st.update == kCatalogNone || st.needs_review || g->index->versions[st.update].code == e.skipped_code) continue;
        if (g->attempted_count < kCatalogStateMaximumEntries) g->attempted[g->attempted_count++] = key;
        download(CatalogAction::update, e.kind, catalog_entry_slug(e), st.item, st.update);
        return;
    }
    g->auto_due = false;
}

void take_results() noexcept {
    CatalogJobResult r{};
    while (catalog_worker_take(&r)) {
        switch (r.kind) {
        case CatalogJobKind::load_cache:
            if (r.serial == g->cache_job) g->cache_job = 0;
            if (r.index && !g->index) set_index(r.index, true);
            else if (r.index) catalog_worker_free(r.index);
            break;
        case CatalogJobKind::check:
            if (r.serial == g->check_job) g->check_job = 0;
            g->view.fetching = false;
            g->view.error = r.error;
            if (r.index) {
                g->state.sequence = r.index->sequence;
                copy_bytes(g->state.digest, r.index->digest, 32);
                g->state.checked_unix = now_unix();
                g->view.checked_unix = g->view.fetched_unix = g->state.checked_unix;
                g->next_check_unix = g->state.checked_unix + kCheckIntervalSeconds;
                set_index(r.index, true);
                save_state();
            } else {
                const u64 retry = now_unix() + kRetrySeconds;
                g->next_check_unix = r.retry_at_unix > retry ? r.retry_at_unix : retry;
                g->view.status = r.error == CatalogError::unreachable || r.error == CatalogError::rate_limited ? CatalogStatus::unreachable
                               : CatalogStatus::invalid;
                UpdateLog(kLog, "check_failed").field("error", static_cast<u8>(r.error)).write(LogLevel::warning);
            }
            bump();
            break;
        case CatalogJobKind::download:
        case CatalogJobKind::copy: downloaded(r); break;
        case CatalogJobKind::asset: {
            bool kept = false;
            for (AssetSlot& slot : g->assets) {
                if (!slot.used || slot.job != r.serial) continue;
                slot.job = 0;
                slot.ready = r.error == CatalogError::none;
                slot.failed = !slot.ready;
                slot.bytes = r.bytes;
                slot.count = r.count;
                kept = true;
            }
            if (!kept) catalog_worker_free(r.bytes);
            bump();
            break;
        }
        default: break;
        }
    }
}
} // namespace

Utf8View application_catalog_error_text(CatalogError error) noexcept {
    switch (error) {
    case CatalogError::none: return {};
    case CatalogError::busy: return utf8_literal("Another change is still finishing. Try again in a moment.");
    case CatalogError::not_configured: return utf8_literal("The catalog isn't available in this build.");
    case CatalogError::unreachable: return utf8_literal("Can't reach the catalog. Check your connection and try again.");
    case CatalogError::rate_limited: return utf8_literal("The catalog asked MaiCord to wait. It will try again later.");
    case CatalogError::signature: return utf8_literal("The catalog's signature didn't verify. Nothing was changed.");
    case CatalogError::invalid_index: return utf8_literal("The catalog couldn't be read. Nothing was changed.");
    case CatalogError::replay: return utf8_literal("The catalog served an older list than MaiCord already has.");
    case CatalogError::expired: return utf8_literal("The catalog is out of date. Refresh it to install or update.");
    case CatalogError::not_listed: return utf8_literal("That item isn't in the catalog any more.");
    case CatalogError::incompatible: return utf8_literal("That item needs a newer MaiCord.");
    case CatalogError::revoked: return utf8_literal("MaiCord turned this off because it was found harmful.");
    case CatalogError::download: return utf8_literal("The download didn't finish. Try again.");
    case CatalogError::digest: return utf8_literal("The download didn't match the catalog. Nothing was installed.");
    case CatalogError::package: return utf8_literal("The file isn't a valid theme or effect. Nothing was installed.");
    case CatalogError::storage: return utf8_literal("MaiCord couldn't save the file. Check the disk and try again.");
    case CatalogError::plugin_load: return utf8_literal("The plugin couldn't be loaded. Nothing was installed.");
    case CatalogError::plugin_mismatch: return utf8_literal("The plugin isn't the build the catalog lists. Nothing was installed.");
    case CatalogError::owner_conflict: return utf8_literal("Another installed plugin uses the same identity.");
    case CatalogError::declined: return {};
    case CatalogError::capacity: return utf8_literal("Too many items are installed. Remove one first.");
    case CatalogError::enable_failed: return utf8_literal("The plugin didn't start. It's turned off.");
    case CatalogError::not_installed: return utf8_literal("That item isn't installed.");
    case CatalogError::developer_mode: return utf8_literal("Turn on Developer Mode to load your own plugins.");
    }
    return {};
}

const CatalogView& application_catalog_view() noexcept {
    static const CatalogView empty{};
    return g ? g->view : empty;
}

u32 application_catalog_installable_version(u32 item) noexcept {
    if (!g || !g->index || item >= g->index->item_count) return kCatalogNone;
    const CatalogItem& it = g->index->items[item];
    for (u32 i = 0; i < it.version_count; ++i) {
        const CatalogVersion& v = g->index->versions[it.first_version + i];
        if (it.kind == CatalogKind::plugin && v.api_level > MC_API_LEVEL) continue;
        const auto severity = catalog_index_revocation(*g->index, v.file.sha256);
        if (severity == CatalogRevocationSeverity::none || severity == CatalogRevocationSeverity::withdrawn) return it.first_version + i;
    }
    return kCatalogNone;
}

CatalogItemState application_catalog_item_state(CatalogKind kind, Utf8View slug) noexcept {
    CatalogItemState s{nullptr, kCatalogNone, kCatalogNone};
    if (!g) return s;
    CatalogStateEntry* e = catalog_state_find(g->state, kind, slug);
    s.installed = e;
    s.item = item_of(kind, slug);
    if (s.item != kCatalogNone) {
        const CatalogItem& it = g->index->items[s.item];
        bool any = false;
        for (u32 i = 0; i < it.version_count; ++i)
            any = any || kind != CatalogKind::plugin || g->index->versions[it.first_version + i].api_level <= MC_API_LEVEL;
        s.incompatible = !any;
    }
    if (!e) return s;
    s.enabled = e->flags & kCatalogEntryEnabled;
    s.running = kind == CatalogKind::plugin ? binding_of(e->owner_id) != nullptr : !(e->flags & kCatalogEntryRevoked);
    s.sideloaded = e->flags & kCatalogEntrySideloaded;
    s.unreviewed = e->flags & kCatalogEntryUnreviewed;
    s.auto_update = catalog_item(*e) && !(e->flags & kCatalogEntryAutoUpdateOff) &&
                    !(kind == CatalogKind::plugin && (g->state.flags & kCatalogStateAutoUpdateOff));
    s.can_roll_back = kind == CatalogKind::plugin && catalog_entry_has_previous(*e) && !(e->flags & kCatalogEntrySideloaded);
    if (g->index) {
        const CatalogRevocation* revocation = nullptr;
        s.revoked = catalog_index_revocation(*g->index, e->sha256, &revocation);
        if (revocation) s.reason = catalog_text(*g->index, revocation->reason);
    }
    if (s.revoked == CatalogRevocationSeverity::none && (e->flags & kCatalogEntryRevoked)) s.revoked = CatalogRevocationSeverity::malicious;
    if (s.item != kCatalogNone && catalog_item(*e)) {
        const u32 v = application_catalog_installable_version(s.item);
        if (v != kCatalogNone && g->index->versions[v].code > entry_code(*e)) {
            s.update = v;
            s.needs_review = needs_review(*e, g->index->versions[v]);
        }
    }
    return s;
}

void application_catalog_browse() noexcept {
    if (!g || !g->restored || !configured()) return;
    g->browsing = true;
    load_cache();
    if (now_unix() >= g->state.checked_unix + kBrowseRefreshSeconds) g->next_check_unix = now_unix();
}

bool application_catalog_check_now() noexcept {
    if (!g || !g->restored) return false;
    g->browsing = true;
    return check();
}

bool application_catalog_install(CatalogKind kind, Utf8View slug) noexcept {
    if (!g || busy()) return false;
    if (!g->view.installable) {
        begin(CatalogAction::install, kind, slug);
        return finish(g->view.status == CatalogStatus::not_configured ? CatalogError::not_configured : CatalogError::expired);
    }
    const u32 item = item_of(kind, slug);
    if (item == kCatalogNone) return false;
    const u32 version = application_catalog_installable_version(item);
    if (version == kCatalogNone) {
        begin(CatalogAction::install, kind, slug);
        return finish(CatalogError::incompatible);
    }
    const CatalogStateEntry* e = catalog_state_find(g->state, kind, slug);
    return download(e && catalog_item(*e) ? CatalogAction::update : CatalogAction::install, kind, slug, item, version);
}

bool application_catalog_update(CatalogKind kind, Utf8View slug) noexcept {
    if (!g || busy() || !g->view.installable) return false;
    const auto st = application_catalog_item_state(kind, slug);
    if (!st.installed || st.update == kCatalogNone) return false;
    return download(CatalogAction::update, kind, slug, st.item, st.update);
}

u32 application_catalog_update_all() noexcept {
    if (!g || busy() || !g->view.installable) return 0;
    // Updates run one after another; the ones that need review wait for the user.
    g->attempted_count = 0;
    u32 count = 0;
    for (u32 i = 0; i < g->state.count; ++i) {
        const auto st = application_catalog_item_state(g->state.entries[i].kind, catalog_entry_slug(g->state.entries[i]));
        count += st.update != kCatalogNone && !st.needs_review ? 1u : 0u;
    }
    for (u32 i = 0; i < g->state.count && !busy(); ++i) {
        const CatalogStateEntry& e = g->state.entries[i];
        const auto st = application_catalog_item_state(e.kind, catalog_entry_slug(e));
        if (st.update != kCatalogNone && !st.needs_review) download(CatalogAction::update, e.kind, catalog_entry_slug(e), st.item, st.update);
    }
    return count;
}

bool application_catalog_rollback(CatalogKind kind, Utf8View slug) noexcept {
    if (!g || busy()) return false;
    CatalogStateEntry* e = catalog_state_find(g->state, kind, slug);
    if (!e || kind != CatalogKind::plugin || !catalog_entry_has_previous(*e)) return false;
    begin(CatalogAction::rollback, kind, slug);
    if (g->index && catalog_index_revocation(*g->index, e->previous_sha256) == CatalogRevocationSeverity::malicious)
        return finish(CatalogError::revoked);
    const bool was_running = binding_of(e->owner_id) != nullptr;
    if (!disable(*e)) return finish(CatalogError::busy);
    // The version left behind is not reinstalled automatically.
    e->skipped_code = entry_code(*e);
    catalog_state_swap_previous(e);
    e->flags &= static_cast<u16>(~(kCatalogEntryOffFlags | kCatalogEntryBroken));
    if (!save_state()) return finish(CatalogError::storage);
    if (!was_running && !(e->flags & kCatalogEntryEnabled)) return finish(CatalogError::none);
    const CatalogError enabled = enable(*e);
    if (enabled != CatalogError::none) {
        catalog_state_swap_previous(e);
        save_state();
        enable(*e);
    }
    return finish(enabled);
}

bool application_catalog_remove(CatalogKind kind, Utf8View slug, bool delete_data) noexcept {
    if (!g || busy()) return false;
    CatalogStateEntry* e = catalog_state_find(g->state, kind, slug);
    if (!e) return false;
    begin(CatalogAction::remove, kind, slug);
    if (kind == CatalogKind::plugin) {
        if (!disable(*e)) return finish(CatalogError::busy);
        u8 current[32], previous[32];
        copy_bytes(current, e->sha256, 32);
        copy_bytes(previous, e->previous_sha256, 32);
        const u64 owner = e->owner_id;
        const bool had_previous = catalog_entry_has_previous(*e);
        catalog_state_remove(g->state, e);
        if (!save_state()) return finish(CatalogError::storage);
        delete_plugin_file(current);
        if (had_previous) delete_plugin_file(previous);
        if (delete_data) {
            CatalogJob job{CatalogJobKind::delete_data};
            job.owner = owner;
            submit(job);
        }
        return finish(CatalogError::none);
    }
    if (g->host.package_removed) g->host.package_removed(kind, slug, false);
    catalog_state_remove(g->state, e);
    return finish(save_state() ? CatalogError::none : CatalogError::storage);
}

bool application_catalog_set_enabled(CatalogKind kind, Utf8View slug, bool on) noexcept {
    if (!g || kind != CatalogKind::plugin || busy()) return false;
    CatalogStateEntry* e = catalog_state_find(g->state, kind, slug);
    if (!e) return false;
    if (on && (e->flags & kCatalogEntryRevoked)) {
        begin(CatalogAction::enable, kind, slug);
        return finish(CatalogError::revoked);
    }
    if (on && (e->flags & kCatalogEntrySideloaded) && !(g->host.developer_mode && g->host.developer_mode())) {
        begin(CatalogAction::enable, kind, slug);
        return finish(CatalogError::developer_mode);
    }
    if (!on && !disable(*e)) return false;
    if (on) e->flags = static_cast<u16>((e->flags & ~kCatalogEntryOffFlags) | kCatalogEntryEnabled);
    else e->flags &= static_cast<u16>(~kCatalogEntryEnabled);
    save_state();
    if (!on) return true;
    begin(CatalogAction::enable, kind, slug);
    return finish(enable(*e));
}

bool application_catalog_set_auto_update(CatalogKind kind, Utf8View slug, bool on) noexcept {
    CatalogStateEntry* e = g && g->restored ? catalog_state_find(g->state, kind, slug) : nullptr;
    if (!e) return false;
    if (on) e->flags &= static_cast<u16>(~kCatalogEntryAutoUpdateOff);
    else e->flags |= kCatalogEntryAutoUpdateOff;
    return save_state();
}

bool application_catalog_set_auto_update_all(bool on) noexcept {
    if (!g || !g->restored) return false;
    if (on) g->state.flags &= ~kCatalogStateAutoUpdateOff;
    else g->state.flags |= kCatalogStateAutoUpdateOff;
    return save_state();
}

bool application_catalog_skip_update(CatalogKind kind, Utf8View slug) noexcept {
    if (!g || !g->restored) return false;
    const auto st = application_catalog_item_state(kind, slug);
    CatalogStateEntry* e = catalog_state_find(g->state, kind, slug);
    if (!e || st.update == kCatalogNone) return false;
    e->skipped_code = g->index->versions[st.update].code;
    return save_state();
}

bool application_catalog_sideload(ConstSlice<wchar_t> path) noexcept {
    if (!g || busy() || !path.data || !path.count || path.count >= 520) return false;
    begin(CatalogAction::sideload, CatalogKind::plugin, {});
    if (!g->host.developer_mode || !g->host.developer_mode()) return finish(CatalogError::developer_mode);
    CatalogJob job{CatalogJobKind::copy};
    copy_bytes(job.path, path.data, path.count * 2);
    job.path_characters = path.count;
    g->op.job = submit(job);
    if (!g->op.job) return finish(CatalogError::busy);
    g->view.operation.phase = CatalogPhase::downloading;
    bump();
    return true;
}

const CatalogConsent* application_catalog_consent() noexcept {
    return g && g->view.operation.phase == CatalogPhase::consent ? &g->consent : nullptr;
}

bool application_catalog_consent_answer(bool accepted, bool enable_now) noexcept {
    if (!g || g->view.operation.phase != CatalogPhase::consent) return false;
    if (!accepted) {
        unload_consent_image();
        delete_plugin_file(g->op.sha256);
        return finish(CatalogError::declined);
    }
    const PluginConsentRequest& request = g->consent_request;
    const u32 all = request.hosts.count ? (1u << request.hosts.count) - 1 : 0;
    // Allowing allows everything the dialog listed.
    return apply_plugin(request.permissions, all, enable_now);
}

bool application_catalog_asset_request(CatalogAssetKind kind, const CatalogBlob& blob) noexcept {
    if (!g || !configured()) return false;
    AssetSlot* free_slot = nullptr;
    for (AssetSlot& slot : g->assets) {
        if (slot.used && bytes_equal(slot.sha256, blob.sha256, 32)) return slot.ready || slot.job;
        if (!slot.used && !free_slot) free_slot = &slot;
    }
    if (!free_slot) {
        // The oldest failed request makes room.
        for (AssetSlot& slot : g->assets)
            if (slot.failed) { free_slot = &slot; break; }
        if (!free_slot) return false;
    }
    CatalogJob job{CatalogJobKind::asset};
    job.asset = kind;
    job.blob = blob;
    const u64 serial = submit(job);
    if (!serial) return false;
    reset_value(free_slot);
    free_slot->used = true;
    free_slot->job = serial;
    copy_bytes(free_slot->sha256, blob.sha256, 32);
    return true;
}

bool application_catalog_asset_take(const u8 (&sha256)[32], CatalogAsset* out) noexcept {
    if (!g || !out) return false;
    for (AssetSlot& slot : g->assets) {
        if (!slot.used || !slot.ready || !bytes_equal(slot.sha256, sha256, 32)) continue;
        copy_bytes(out->sha256, sha256, 32);
        out->bytes = slot.bytes;
        out->count = slot.count;
        reset_value(&slot);
        return true;
    }
    return false;
}

void application_catalog_asset_release(CatalogAsset* asset) noexcept {
    if (!asset) return;
    catalog_worker_free(asset->bytes);
    reset_value(asset);
}

bool application_catalog_record_unreviewed(CatalogKind kind, Utf8View slug, const u8 (&sha256)[32], Utf8View version) noexcept {
    if (!g || !g->restored || kind == CatalogKind::plugin) return false;
    CatalogStateEntry* e = catalog_state_find(g->state, kind, slug);
    if (e && catalog_item(*e)) return false;
    if (!e && !(e = catalog_state_add(g->state, kind, slug))) return false;
    // A package version that isn't MAJOR.MINOR.PATCH is not kept: one bad entry must not block saving the rest.
    u64 code = 0;
    catalog_state_install(e, catalog_version_code(version, &code) ? version : Utf8View{}, sha256, now_unix(), false);
    e->flags = kCatalogEntryUnreviewed;
    if (g->index) apply_revocations();
    return save_state();
}

bool application_catalog_forget(CatalogKind kind, Utf8View slug) noexcept {
    CatalogStateEntry* e = g && g->restored ? catalog_state_find(g->state, kind, slug) : nullptr;
    if (!e || !(e->flags & kCatalogEntryUnreviewed)) return false;
    catalog_state_remove(g->state, e);
    return save_state();
}

bool application_catalog_initialize(const CatalogServiceOptions& options) noexcept {
    if (g) return true;
    if (!options.base.data || !options.base.count || options.base.count >= 480 || options.keys.count > 2 ||
        options.repository.bytes > kUpdateRepositoryMaximumBytes)
        return false;
    auto* s = allocate<Service>();
    if (!s) return false;
    s->host = options.host;
    copy_bytes(s->base, options.base.data, options.base.count * 2);
    s->base_characters = options.base.count;
    if (options.keys.count && options.repository.bytes && update_repository_valid(options.repository)) {
        copy_bytes(s->repository, options.repository.data, options.repository.bytes);
        s->repository_bytes = options.repository.bytes;
        for (u32 i = 0; i < options.keys.count; ++i) copy_bytes(s->keys[s->key_count++], options.keys[i], 65);
    }
    s->transport = options.transport;
    s->clock_unix = options.clock_unix;
    s->view.status = s->key_count ? CatalogStatus::none : CatalogStatus::not_configured;
    g = s;
    g->state_path_characters = catalog_path(base(), L"catalog-state.bin", g->state_path);
    return g->state_path_characters != 0;
}

void application_catalog_restore() noexcept {
    if (!g || g->restored) return;
    g->restored = true;
    const auto loaded = catalog_state_load(g->state_path, &g->state);
    if (!loaded.ok()) UpdateLog(kLog, "state_unreadable").field("error", static_cast<u8>(loaded.error)).write(LogLevel::error);
    g->view.state = &g->state;
    g->view.checked_unix = g->view.fetched_unix = g->state.checked_unix;
    delete_unreferenced_plugins();
    const bool developer = g->host.developer_mode && g->host.developer_mode();
    for (u32 i = 0; i < g->state.count; ++i) {
        CatalogStateEntry& e = g->state.entries[i];
        if (e.kind != CatalogKind::plugin || !(e.flags & kCatalogEntryEnabled) || ((e.flags & kCatalogEntrySideloaded) && !developer))
            continue;
        if (!runtime()) break;
        // runtime() may have flagged this plugin from a crash marker.
        if (e.flags & kCatalogEntryOffFlags) continue;
        if (enable(e) != CatalogError::none) UpdateLog(kLog, "restore_failed").hex("owner", e.owner_id).write(LogLevel::warning);
    }
    if (g->state.count) load_cache();
    if (catalog_installed()) {
        const u64 earliest = now_unix() + kFirstCheckDelaySeconds;
        const u64 due = g->state.checked_unix + kCheckIntervalSeconds;
        g->next_check_unix = due > earliest ? due : earliest;
    }
    bump();
}

void application_catalog_release() noexcept {
    if (!g) return;
    unload_consent_image();
    if (g->runtime && g->runtime->open) {
        bool changed = false;
        for (u32 i = 0; i < g->state.count; ++i) {
            CatalogStateEntry& e = g->state.entries[i];
            const u8 before = e.host_bits;
            sync_hosts(e);
            changed = changed || before != e.host_bits;
        }
        if (changed) save_state();
        plugin_runtime_close(&g->runtime->runtime);
        plugin_call_ledger_close(&g->runtime->ledger);
        g->runtime->open = false;
    }
    catalog_worker_stop();
    for (AssetSlot& slot : g->assets) catalog_worker_free(slot.bytes);
    catalog_worker_free(g->index);
    // A runtime whose DLL could not be freed keeps its memory.
    if (!g->runtime || !g->runtime->runtime.binding_count) catalog_worker_free(g->runtime);
    catalog_worker_free(g);
    g = nullptr;
}

HANDLE application_catalog_notification() noexcept { return g && g->worker ? catalog_worker_notification() : nullptr; }

HANDLE application_catalog_http_notification() noexcept {
    return g && g->runtime && g->runtime->open ? plugin_http_notification(&g->runtime->runtime) : nullptr;
}

void application_catalog_poll() noexcept {
    if (!g) return;
    if (g->worker) take_results();
    if (g->runtime && g->runtime->open) {
        PluginRuntime& r = g->runtime->runtime;
        plugin_http_deliver(&r);
        while (const u64 owner = plugin_runtime_poll(&r, clock_monotonic_milliseconds())) {
            if (CatalogStateEntry* e = catalog_state_find_owner(g->state, owner)) e->flags |= kCatalogEntryOverran;
            UpdateLog(kLog, "plugin_overran").hex("owner", owner).write(LogLevel::warning);
            if (g->host.toast) g->host.toast(utf8_literal("MaiCord turned off a plugin that kept running too long."), true);
            save_state();
        }
        // Host switches changed on the plugin's settings page.
        for (u32 i = 0; i < r.binding_count; ++i) {
            const u32 hosts = r.bindings[i]->approved_hosts;
            if (g->host_snapshot[i] == hosts) continue;
            g->host_snapshot[i] = hosts;
            CatalogStateEntry* e = catalog_state_find_owner(g->state, r.bindings[i]->owner_id);
            if (e && e->host_bits != static_cast<u8>(hosts)) {
                e->host_bits = static_cast<u8>(hosts);
                save_state();
            }
        }
    }
    if (g->view.operation.phase == CatalogPhase::downloading) {
        u64 bytes = 0, total = 0;
        catalog_worker_progress(&bytes, &total);
        if (total && bytes != g->view.operation.bytes) {
            g->view.operation.bytes = bytes;
            g->view.operation.total = total;
            bump();
        }
    }
    if (configured() && g->restored && g->next_check_unix && !g->check_job && now_unix() >= g->next_check_unix &&
        (g->browsing || catalog_installed())) {
        g->next_check_unix = now_unix() + kRetrySeconds;
        check();
    }
    auto_update();
}

u32 application_catalog_wait_timeout(u64 now_ms) noexcept {
    if (!g) return INFINITE;
    u64 wait = INFINITE;
    if (g->view.operation.phase == CatalogPhase::downloading) wait = 100;
    if (g->runtime && g->runtime->open) {
        const PluginRuntime& r = g->runtime->runtime;
        for (u32 i = 0; i < r.binding_count; ++i) {
            const PluginBinding& b = *r.bindings[i];
            const u64 since[]{b.store.dirty ? b.store.dirty_since : ~0ull, b.settings_dirty ? b.settings_dirty_since : ~0ull};
            for (u64 at : since) {
                if (at == ~0ull) continue;
                const u64 due = at + kPluginFlushDelayMilliseconds;
                wait = minimum<u64>(wait, due <= now_ms ? 0 : due - now_ms);
            }
        }
    }
    if (configured() && g->restored && g->next_check_unix && (g->browsing || catalog_installed())) {
        const u64 now = now_unix();
        wait = minimum<u64>(wait, g->next_check_unix <= now ? 0 : minimum<u64>(g->next_check_unix - now, 3600) * 1000);
    }
    return static_cast<u32>(minimum<u64>(wait, INFINITE));
}

PluginRuntime* application_catalog_runtime() noexcept { return g && g->runtime && g->runtime->open ? &g->runtime->runtime : nullptr; }

} // namespace maicord::app_detail
