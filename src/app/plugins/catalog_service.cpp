#include "catalog_service.hpp"
#include "maicord/os/text.hpp"
#include "catalog_worker.hpp"
#include "dev_console.hpp"
#include "dev_watch.hpp"
#include "maicord/app/packages/mcfx.hpp"
#include "maicord/app/settings/client_theme_document.hpp"
#include "maicord/app/plugins/consent.hpp"
#include "maicord/app/plugins/http.hpp"
#include "maicord/app/plugins/pipeline.hpp"
#include "maicord/app/update/log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/file.hpp"
#include "maicord/sdk/maicord_plugin.h"
#include "maicord/core/utf.hpp"

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
    bool update;
    // A Developer Mode reload (a build the dev folder watch found); dev_file indexes Service::dev_files.
    bool reload;
    u8 dev_file;
    char dev_name[kDevLineNameBytes]; // the plugin's name once known, else the file's
    u8 dev_name_bytes;
    u64 dev_started_ms;
};
// A DLL seen in the dev folder.
struct DevFile {
    wchar_t name[64];
    u64 modified;
    u32 bytes;
    bool pending;
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
    // Developer Mode: the dev folder (plugins\dev or %MAICORD_PLUGIN_DEV%), its watch and the DLLs seen in it.
    DevWatch dev_watch, effect_watch; // the plugin dev folder, and the effects one (a change there rescans the effect library)
    wchar_t dev_folder[480];
    u32 dev_folder_characters;
    char dev_folder_text[1440], effect_folder_text[1440]; // UTF-8, for the Developer page
    u32 dev_folder_text_bytes, effect_folder_text_bytes;
    DevFile dev_files[8];
    u32 dev_count;
    bool dev_scan, dev_force, dev_failed; // scan the folder at the next poll (force: every DLL again); the folder can't be watched
    char dev_detail[kDevLineTextBytes]; // why the current reload failed, when known
    u32 dev_detail_bytes;
    PluginEnableResult last_enable;
};
Service* g;

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
    if (!g->runtime && !(g->runtime = commit_pages<Runtime>())) return nullptr;
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
    plugin_pipeline_set_sink(g->host.pipeline);
    // Log lines and budget strikes reach the Developer console (which holds them only in Developer Mode).
    r.runtime.sink_context = &r.runtime;
    r.runtime.log = application_dev_console_log;
    r.runtime.strike = application_dev_console_strike;
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
    g->last_enable = {};
    if (e.kind != CatalogKind::plugin) return CatalogError::none;
    if (binding_of(e.owner_id)) return CatalogError::none;
    if (e.flags & kCatalogEntryRevoked) return CatalogError::revoked;
    Runtime* r = runtime();
    if (!r) return CatalogError::storage;
    wchar_t leaf[69];
    catalog_plugin_leaf(e.sha256, leaf);
    const auto enabled = plugin_runtime_enable(&r->runtime, {leaf, kCatalogPluginLeafCharacters}, e.approved_permissions, e.host_bits, e.sha256);
    g->last_enable = enabled;
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
        if (wide_append(path, 600, &length, leaf)) file_remove(path);
    }
}
// Plugin files nothing names any more: a crash between writing a file and recording it leaves one behind.
void delete_unreferenced_plugins() noexcept {
    wchar_t directory[600];
    if (!catalog_path(base(), L"plugins\\", directory)) return;
    DirIter scan{};
    if (!dir_open(&scan, directory)) return;
    DirEntry found{};
    while (dir_next(&scan, &found)) {
        if (found.kind != FileKind::regular || wide_length(found.name, 260) != kCatalogPluginLeafCharacters) continue;
        bool dll = true;
        for (u32 i = 0; i < kCatalogPluginExtensionCharacters; ++i)
            dll = dll && (found.name[64 + i] | 0x20) == kCatalogPluginExtension[i];
        if (!dll) continue;
        u8 sha[32];
        bool valid = true;
        for (u32 i = 0; i < 64 && valid; ++i) {
            const wchar_t c = found.name[i];
            const i32 nibble = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
            valid = nibble >= 0;
            sha[i / 2] = static_cast<u8>(i % 2 ? sha[i / 2] << 4 | nibble : nibble);
        }
        if (valid) delete_plugin_file(sha);
    }
    dir_close(&scan);
}

// ---- the index ------------------------------------------------------------------------------------------
u32 item_of(CatalogKind kind, const Utf8View& slug) noexcept {
    return g->index ? catalog_index_find(*g->index, kind, slug) : kCatalogNone;
}
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
        const auto severity = catalog_index_revocation(*g->index, e.sha256);
        const auto previous = e.previous_version_bytes ? catalog_index_revocation(*g->index, e.previous_sha256)
                                                        : CatalogRevocationSeverity::none;
        if (previous != CatalogRevocationSeverity::none && previous != CatalogRevocationSeverity::withdrawn) {
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
void begin(CatalogAction action, CatalogKind kind, const Utf8View& slug) noexcept {
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
    g->dev_detail_bytes = 0;
    bump();
}
// ---- Developer Mode: the dev folder -----------------------------------------------------------------------------
bool developer() noexcept { return g->host.developer_mode && g->host.developer_mode(); }

void dev_line(DevLineKind kind, const Utf8View& text) noexcept {
    application_dev_console_add(kind, {g->op.dev_name, g->op.dev_name_bytes}, text);
}
// Why a reload didn't happen: the details the step recorded, else the error's own sentence.
void dev_problem(CatalogError error) noexcept {
    DevText t{};
    t.add("Not reloaded: ");
    if (g->dev_detail_bytes) t.add({g->dev_detail, g->dev_detail_bytes});
    else if (error == CatalogError::declined) t.add("you declined it.");
    else t.add(application_catalog_error_text(error));
    dev_line(DevLineKind::problem, t.view());
}
void dev_detail_set(const DevText& text) noexcept {
    g->dev_detail_bytes = text.used;
    copy_bytes(g->dev_detail, text.data, text.used);
}
// "Reloaded in 12 ms. State kept." after a rebuild replaced a running build; "Loaded" when none was running.
void dev_reloaded(bool was_running) noexcept {
    DevText t{};
    t.add(was_running ? utf8_literal("Reloaded in ") : utf8_literal("Loaded in "));
    t.number(clock_monotonic_milliseconds() - g->op.dev_started_ms);
    t.add(" ms.");
    if (g->last_enable.carried == PluginStateCarry::kept) t.add(" State kept.");
    else if (g->last_enable.carried == PluginStateCarry::reset) t.add(" State reset: its layout changed.");
    dev_line(DevLineKind::reload, t.view());
}
// The new build was refused or didn't start; the old one is already gone, so the plugin stays off.
void dev_enable_failed() noexcept {
    DevText t{};
    application_dev_enable_error_text(g->last_enable, &t);
    t.add(" The plugin stays off until the next build.");
    dev_detail_set(t);
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
    if (g->op.reload && error != CatalogError::none) {
        dev_problem(error);
        // The next change to the folder (or its Reload button) tries this file again; a decline stays declined.
        if (error != CatalogError::declined && g->op.dev_file < g->dev_count) g->dev_files[g->op.dev_file].modified = 0;
    }
    bump();
    return error == CatalogError::none;
}
void unload_consent_image() noexcept {
    if (g->consent_image.module) plugin_image_unload(&g->consent_image);
    reset_value(&g->consent);
}
// Starts a download of a listed version.
bool download(CatalogAction action, CatalogKind kind, const Utf8View& slug, u32 version) noexcept {
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
    // A reload carries the running build's state over to its replacement (when the layout allows).
    if (g->op.reload && was_running) plugin_runtime_keep_state(&g->runtime->runtime, binding_of(e->owner_id));
    if (existed && !disable(*e)) return finish(CatalogError::busy);
    u8 replaced[32];
    copy_bytes(replaced, e->sha256, 32);
    const bool keep_previous = existed && g->op.action == CatalogAction::update;
    catalog_state_install(e, version, g->op.sha256, now_unix(), keep_previous);
    // Every rebuild is a new file; the replaced one is of no use any more.
    if (existed && g->op.reload && !bytes_equal(replaced, g->op.sha256, 32)) delete_plugin_file(replaced);
    e->owner_id = owner;
    catalog_state_approve(e, permissions, host_list, host_bits);
    e->host_bits = static_cast<u8>(host_bits);
    e->flags &= static_cast<u16>(~(kCatalogEntryOffFlags | kCatalogEntryBroken));
    if (g->op.action == CatalogAction::sideload) e->flags |= kCatalogEntrySideloaded;
    const bool run = g->op.update ? was_running || (e->flags & kCatalogEntryEnabled) : enable_now;
    if (run) e->flags |= kCatalogEntryEnabled;
    else e->flags &= static_cast<u16>(~kCatalogEntryEnabled);
    if (!save_state()) return finish(CatalogError::storage);
    if (!run) {
        if (g->op.reload) dev_line(DevLineKind::reload, utf8_literal("Updated. The plugin is off; turn it on to run this build."));
        return finish(CatalogError::none);
    }
    const CatalogError enabled = enable(*e);
    if (enabled != CatalogError::none && keep_previous && catalog_entry_has_previous(*e)) {
        // The new build would not start: go back to the one that did.
        catalog_state_swap_previous(e);
        save_state();
        enable(*e);
    }
    bump();
    if (g->op.reload) {
        if (enabled == CatalogError::none) dev_reloaded(was_running);
        else dev_enable_failed();
    }
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
    const auto loaded = plugin_image_load(&g->consent_image, r->runtime.directory, {leaf, kCatalogPluginLeafCharacters}, ~0ull, g->op.sha256);
    if (!loaded.ok()) {
        UpdateLog(kLog, "plugin_refused").field("error", static_cast<u8>(loaded.error)).field("problem", static_cast<u8>(loaded.problem)).write(LogLevel::warning);
        if (g->op.reload) {
            DevText why{};
            application_dev_load_error_text(loaded, &why);
            dev_detail_set(why);
        }
        reset_value(&g->consent_image);
        delete_plugin_file(g->op.sha256);
        return finish(loaded.error == PluginLoadError::digest ? CatalogError::digest : CatalogError::plugin_load);
    }
    const PluginImage& image = g->consent_image;
    if (g->op.reload) {
        g->op.dev_name_bytes = static_cast<u8>(utf8_copy_truncated(g->op.dev_name, {image.info.name, image.info.name_bytes}));
    }
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
        // The build this folder already has running: nothing to do.
        if (g->op.reload && e && bytes_equal(e->sha256, g->op.sha256, 32)) {
            unload_consent_image();
            return finish(CatalogError::none);
        }
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
    // Updates within the approval apply at once, and so does a rebuild of a plugin the user already allowed
    // (Developer Mode); installs and first sideloads always ask.
    const bool within = !(request.permissions & ~request.approved_permissions) && request.approved_hosts == all;
    if (within && ((g->op.update && g->op.action != CatalogAction::sideload) || (g->op.reload && e)))
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
        download(CatalogAction::update, e.kind, catalog_entry_slug(e), st.update);
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

// ---- Developer Mode: watching the dev folder -----------------------------------------------------------------------
constexpr u32 kNoDevFile = 0xFFFFFFFFu;

// Copies a DLL into plugins\ and reviews it. file is the dev folder entry for a reload (a plugin the user
// allowed before then swaps without a question, and its state carries over), else kNoDevFile.
bool sideload_file(const wchar_t* path, u32 characters, u32 file) noexcept {
    begin(CatalogAction::sideload, CatalogKind::plugin, {});
    if (!developer()) return finish(CatalogError::developer_mode);
    if (file != kNoDevFile) {
        g->op.reload = true;
        g->op.dev_file = static_cast<u8>(file);
        g->op.dev_started_ms = clock_monotonic_milliseconds();
        const wchar_t* name = g->dev_files[file].name;
        const u32 stem = wide_length(name, 64) - kCatalogPluginExtensionCharacters; // without ".dll" (".so" on Linux)
        for (u32 i = 0; i < stem && g->op.dev_name_bytes < kDevLineNameBytes; ++i)
            g->op.dev_name[g->op.dev_name_bytes++] = name[i] < 0x80 ? static_cast<char>(name[i]) : '?';
    }
    CatalogJob job{CatalogJobKind::copy};
    copy_bytes(job.path, path, characters * 2);
    job.path_characters = characters;
    g->op.job = submit(job);
    if (!g->op.job) return finish(CatalogError::busy);
    g->view.operation.phase = CatalogPhase::downloading;
    bump();
    return true;
}

// A dev folder: the environment variable's folder, else <base>\<relative> (whose parent is made first); created
// when it is missing. path receives it.
bool dev_folder_path(const wchar_t* variable, const wchar_t* parent, const wchar_t* relative, wchar_t (&path)[600], u32* length) noexcept {
    *length = environment_variable(variable, path, 480);
    if (!*length || *length >= 480) {
        wchar_t above[600];
        if (!catalog_path(base(), parent, above) || !directory_ensure(g->base) || !directory_ensure(above)) return false;
        *length = catalog_path(base(), relative, path);
    }
    return *length && *length < 480 && directory_ensure(path);
}

u32 dev_utf8(const wchar_t* path, u32 length, char (&text)[1440]) noexcept {
    const int bytes = utf16_to_utf8(path, static_cast<int>(length), text, static_cast<int>(sizeof(text)), false);
    return bytes > 0 ? static_cast<u32>(bytes) : 0;
}

// plugins\dev, or %MAICORD_PLUGIN_DEV%.
bool dev_folder_resolve() noexcept {
    wchar_t path[600];
    u32 length = 0;
    if (!dev_folder_path(L"MAICORD_PLUGIN_DEV", L"plugins", L"plugins\\dev", path, &length)) return false;
    copy_bytes(g->dev_folder, path, (length + 1) * 2);
    g->dev_folder_characters = length;
    g->dev_folder_text_bytes = dev_utf8(path, length, g->dev_folder_text);
    return true;
}

// effects\dev, or %MAICORD_EFFECT_DEV%: the effects module lists its .mcfx files; a settled change only has to
// tell it to scan again (a rebuilt effect keeps its selection and its knob values there).
bool dev_effects_watch() noexcept {
    wchar_t path[600];
    u32 length = 0;
    if (!dev_folder_path(L"MAICORD_EFFECT_DEV", L"effects", L"effects\\dev", path, &length)) return false;
    g->effect_folder_text_bytes = dev_utf8(path, length, g->effect_folder_text);
    return dev_watch_open(&g->effect_watch, path);
}

bool dev_dll(const wchar_t* name, u32 length) noexcept {
    constexpr u32 kExtension = kCatalogPluginExtensionCharacters;
    if (length <= kExtension || length >= 64) return false;
    for (u32 i = 0; i < kExtension; ++i) {
        const wchar_t c = name[length - kExtension + i];
        if ((c | 0x20) != kCatalogPluginExtension[i]) return false;
    }
    return true;
}

// The DLLs in the folder: new ones and ones whose time or size changed are pending (everything when force).
void dev_scan(bool force) noexcept {
    u32 length = g->dev_folder_characters;
    if (!length || length >= 600) return;
    DevFile seen[8]{};
    u32 count = 0;
    DirIter scan{};
    if (!dir_open(&scan, g->dev_folder)) {
        g->dev_count = 0; // an empty folder: a DLL that comes back is new
        return;
    }
    DirEntry found{};
    while (dir_next(&scan, &found)) {
        const u32 name_length = wide_length(found.name, 64);
        if (found.kind != FileKind::regular || !dev_dll(found.name, name_length) || count == 8)
            continue;
        DevFile& file = seen[count++];
        copy_bytes(file.name, found.name, (name_length + 1) * 2);
        file.modified = found.last_write;
        file.bytes = static_cast<u32>(found.bytes);
        file.pending = true;
        for (u32 i = 0; i < g->dev_count && !force; ++i) {
            const DevFile& before = g->dev_files[i];
            if (wide_length(before.name, 64) == name_length && bytes_equal(before.name, file.name, name_length * 2))
                file.pending = before.pending || before.modified != file.modified || before.bytes != file.bytes;
        }
    }
    dir_close(&scan);
    copy_bytes(g->dev_files, seen, sizeof(seen));
    g->dev_count = count;
}

// The next pending build, once nothing else is in progress.
void dev_next() noexcept {
    for (u32 i = 0; i < g->dev_count && !busy(); ++i) {
        DevFile& file = g->dev_files[i];
        if (!file.pending) continue;
        file.pending = false;
        wchar_t path[600];
        u32 length = g->dev_folder_characters;
        copy_bytes(path, g->dev_folder, (length + 1) * 2);
        if (wide_append(path, 600, &length, L"\\") && wide_append(path, 600, &length, file.name) && length < 520)
            sideload_file(path, length, i);
    }
}

void dev_poll() noexcept {
    const bool on = g->restored && developer();
    application_dev_console_enable(on);
    if (!on) {
        dev_watch_close(&g->dev_watch);
        dev_watch_close(&g->effect_watch);
        g->dev_count = 0;
        g->dev_failed = false;
        return;
    }
    const u64 now = clock_monotonic_milliseconds();
    if (!g->dev_failed && (!g->dev_watch.handle || !g->effect_watch.handle)) {
        g->dev_failed = !dev_folder_resolve() || !dev_watch_open(&g->dev_watch, g->dev_folder) || !dev_effects_watch();
        g->dev_scan = true;
    }
    if (dev_watch_settled(&g->dev_watch, now)) g->dev_scan = true;
    if (dev_watch_settled(&g->effect_watch, now) && g->host.library_changed) g->host.library_changed();
    if (g->dev_scan && g->dev_watch.handle) {
        dev_scan(g->dev_force);
        g->dev_scan = g->dev_force = false;
    }
    dev_next();
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

namespace {
// The item's interface (a plugin entry's, a theme document's or an effect's .mcfx interface) and a plugin version's
// API level are ones this client has.
bool version_runs(const CatalogItem& item, const CatalogVersion& v) noexcept {
    constexpr u32 kInterfaces[kCatalogKindCount]{1, kClientThemeInterfaceMaximum, kMcfxInterfaceMaximum};
    return item.interface_level <= kInterfaces[static_cast<u32>(item.kind)] &&
           (item.kind != CatalogKind::plugin || v.api_level <= MC_API_LEVEL);
}
} // namespace

u32 application_catalog_installable_version(u32 item) noexcept {
    if (!g || !g->index || item >= g->index->item_count) return kCatalogNone;
    const CatalogItem& it = g->index->items[item];
    for (u32 i = 0; i < it.version_count; ++i) {
        const CatalogVersion& v = g->index->versions[it.first_version + i];
        if (!version_runs(it, v)) continue;
        const auto severity = catalog_index_revocation(*g->index, v.file.sha256);
        if (severity == CatalogRevocationSeverity::none || severity == CatalogRevocationSeverity::withdrawn) return it.first_version + i;
    }
    return kCatalogNone;
}

CatalogItemState application_catalog_item_state(CatalogKind kind, const Utf8View& slug) noexcept {
    CatalogItemState s{nullptr, kCatalogNone, kCatalogNone};
    if (!g) return s;
    CatalogStateEntry* e = catalog_state_find(g->state, kind, slug);
    s.installed = e;
    s.item = item_of(kind, slug);
    if (s.item != kCatalogNone) {
        const CatalogItem& it = g->index->items[s.item];
        bool any = false;
        for (u32 i = 0; i < it.version_count; ++i)
            any = any || version_runs(it, g->index->versions[it.first_version + i]);
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
    if (!g) return;
    // The shell restores the catalog when the account session is ready; the store pages open before that (signed out,
    // or while the session is restoring) and must list the catalog all the same.
    if (!g->restored) application_catalog_restore();
    if (!configured()) return;
    g->browsing = true;
    load_cache();
    if (now_unix() >= g->state.checked_unix + kBrowseRefreshSeconds) g->next_check_unix = now_unix();
}

bool application_catalog_check_now() noexcept {
    if (!g || !g->restored) return false;
    g->browsing = true;
    return check();
}

bool application_catalog_install(CatalogKind kind, const Utf8View& slug) noexcept {
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
    return download(e && catalog_item(*e) ? CatalogAction::update : CatalogAction::install, kind, slug, version);
}

bool application_catalog_update(CatalogKind kind, const Utf8View& slug) noexcept {
    if (!g || busy() || !g->view.installable) return false;
    const auto st = application_catalog_item_state(kind, slug);
    if (!st.installed || st.update == kCatalogNone) return false;
    return download(CatalogAction::update, kind, slug, st.update);
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
        if (st.update != kCatalogNone && !st.needs_review) download(CatalogAction::update, e.kind, catalog_entry_slug(e), st.update);
    }
    return count;
}

bool application_catalog_rollback(CatalogKind kind, const Utf8View& slug) noexcept {
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

bool application_catalog_remove(CatalogKind kind, const Utf8View& slug, bool delete_data) noexcept {
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

bool application_catalog_set_enabled(CatalogKind kind, const Utf8View& slug, bool on) noexcept {
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

bool application_catalog_set_auto_update(CatalogKind kind, const Utf8View& slug, bool on) noexcept {
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

bool application_catalog_skip_update(CatalogKind kind, const Utf8View& slug) noexcept {
    if (!g || !g->restored) return false;
    const auto st = application_catalog_item_state(kind, slug);
    CatalogStateEntry* e = catalog_state_find(g->state, kind, slug);
    if (!e || st.update == kCatalogNone) return false;
    e->skipped_code = g->index->versions[st.update].code;
    return save_state();
}

bool application_catalog_sideload(ConstSlice<wchar_t> path) noexcept {
    if (!g || busy() || !path.data || !path.count || path.count >= 520) return false;
    return sideload_file(path.data, path.count, kNoDevFile);
}

HANDLE application_catalog_dev_notification(u32 folder) noexcept {
    return g ? (folder == 0 ? g->dev_watch.handle : folder == 1 ? g->effect_watch.handle : nullptr) : nullptr;
}

bool application_catalog_dev_reload() noexcept {
    if (!g || !g->restored || !developer()) return false;
    g->dev_scan = g->dev_force = true;
    return true;
}

Utf8View application_catalog_dev_folder(u32 folder) noexcept {
    if (!g) return {};
    return folder == 0 ? Utf8View{g->dev_folder_text, g->dev_folder_text_bytes} : Utf8View{g->effect_folder_text, g->effect_folder_text_bytes};
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

bool application_catalog_record_unreviewed(CatalogKind kind, const Utf8View& slug, const u8 (&sha256)[32], const Utf8View& version) noexcept {
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

bool application_catalog_forget(CatalogKind kind, const Utf8View& slug) noexcept {
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
    auto* s = commit_pages<Service>();
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
    dev_watch_close(&g->dev_watch);
    dev_watch_close(&g->effect_watch);
    application_dev_console_enable(false);
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
    dev_poll();
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
    if (g->dev_watch.handle) {
        wait = minimum<u64>(wait, minimum<u64>(dev_watch_timeout(g->dev_watch, now_ms), dev_watch_timeout(g->effect_watch, now_ms)));
        bool ready = g->dev_scan;
        for (u32 i = 0; i < g->dev_count; ++i) ready = ready || g->dev_files[i].pending;
        if (ready && !busy()) wait = 0; // a build is waiting for its turn (a consent in progress holds it)
    }
    return static_cast<u32>(minimum<u64>(wait, INFINITE));
}

PluginRuntime* application_catalog_runtime() noexcept { return g && g->runtime && g->runtime->open ? &g->runtime->runtime : nullptr; }

} // namespace maicord::app_detail
