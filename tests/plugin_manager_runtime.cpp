// The plugin catalog at runtime: startup recovery and the roll-forward of a
// staged automatic update, the startup gate, applying indexes to installed
// plugins (adoption, revocation, the blocklist, pruning, update offers), the
// browser snapshot, and the catalog worker against a fake server. Versions
// are MaiCordPluginFixture with zero bytes appended; nothing is loaded.
// Isolated TEMP files; test-only scalar-one key.
#include "application_plugin_browser.hpp"
#include "maicord/app/update_log.hpp"
#include "plugin_manager_fixture.hpp"
#include "plugin_fake_transport.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using namespace app_detail;
using namespace plugin_manager_fixture;
using Op = PluginJournalOperation;
using Action = PluginRecoveryAction;
constexpr u64 kSequence = 1760227200;
constexpr u64 kManual = 0x004d414e55414cull;   // "MANUAL"

u32 g_failures;
void check(bool passed, const char* name) noexcept { test_check(&g_failures, passed, name); }
u32 g_log_paths, g_log_lines;
HANDLE g_probe_log_entered, g_probe_log_release;
volatile LONG g_probe_log_armed, g_probe_log_released;
// Log lines carry hosts and numbers only: a '/' would be a URL path.
void capture_log(LogLevel, const char*, const char* line) noexcept {
    ++g_log_lines;
    for (const char* c = line; *c; ++c) if (*c == '/') { ++g_log_paths; break; }
    if (InterlockedCompareExchange(&g_probe_log_armed, 0, 0)) {
        PluginBrowserWorkerView view{};
        if (plugin_browser_worker_view(&view, ~u64{0}) && view.phase == PluginBrowserPhase::idle &&
            view.address_error == PluginProviderAddressError::none &&
            view.probe.error == PluginProviderProbeError::none && view.probe.signed_provider &&
            InterlockedCompareExchange(&g_probe_log_armed, 0, 1) == 1) {
            // Hold the completed command before its worker function returns.
            SetEvent(g_probe_log_entered);
            InterlockedExchange(&g_probe_log_released,
                WaitForSingleObject(g_probe_log_release, kTimeoutMs) == WAIT_OBJECT_0 ? 1 : 0);
        }
    }
}

struct Fixture {
    KdfSha256Workspace workspace;
    P256Key signer, trusted;
    wchar_t root[MAX_PATH], base[MAX_PATH];
    u32 base_count;
    PluginManagerPaths paths;
    PluginCatalogSource source;
    plugin_fake::Server server;
    u8* dll;
    u32 dll_bytes;
    u8 sha[3][32], manual_sha[32], blob_sha[32];
    u8 blob[512];
    char index[8192];
    u32 index_bytes;
    u8 signature[64];
    u8* custom;
    u32 custom_bytes;
    u8 custom_signature[64];
    PluginIndexWork index_work, official_work;
    PluginManagerWork work;
    PluginManagerPrepared prepared;
    PluginManagerSettled settled;
    NativePluginBundleInstall installer;
    PluginStartupGate gate;
    u64 menus;
};
Fixture g;

bool digest(const u8* data, u32 bytes, u8* out) noexcept {
    const ConstSlice<u8> part{data, bytes};
    return sha256(g.workspace, {&part, 1}, {out, 32}).ok();
}
struct Spec {
    u64 sequence;
    bool revoke_installed; // malicious revocation of (FIXTURE, revoked_sha)
    const u8* revoked_sha;
    bool block_manual;     // the official blocklist names the manual install's bytes
    bool v2_settings_only; // 1.2.0 asks for no more than 1.1.0
};

bool build_index(const Spec& spec) noexcept {
    Text t{g.index, sizeof(g.index), 0};
    t.add(R"({"format":1,"provider":{"id":"maicord","name":"MaiCord plugins","homepage":"https://plugins.example.com/maicord"},)");
    t.add(R"("sequence":)"); t.number(spec.sequence); t.add(R"(,"issued":1760226600,"expires":1762818600,"plugins":[)");
    plugin(t, "fixture", kOwner, "Fixture");
    version(t, "1.2.0", kCode[2], spec.v2_settings_only ? R"("settings")" : R"("settings","menus")", g.sha[2], g.dll_bytes + 32, false);
    version(t, "1.1.0", kCode[1], R"("settings")", g.sha[1], g.dll_bytes + 16, false);
    version(t, "1.0.0", kCode[0], R"("settings")", g.sha[0], g.dll_bytes, true);
    t.add("]},");
    plugin(t, "notdll", kNotDll, "Not a DLL");
    version(t, "1.0.0", kCode[0], "", g.blob_sha, sizeof(g.blob), true);
    t.add("]},");
    plugin(t, "manual", kManual, "Manual");
    version(t, "1.0.0", kCode[0], R"("settings")", g.manual_sha, g.dll_bytes + 48, true);
    t.add(R"(]}],"revoked":[)");
    if (spec.revoke_installed) {
        t.add(R"({"owner_id":)"); t.number(kOwner); t.add(R"(,"sha256":")"); t.hex(spec.revoked_sha, 32);
        t.add(R"(","severity":"malicious","reason":"Sent messages without asking.","since":1760000000})");
    }
    t.add("]");
    if (spec.block_manual) {
        t.add(R"(,"blocklist":[{"sha256":")"); t.hex(g.manual_sha, 32); t.add(R"(","reason":"Steals data.","since":1760000000}])");
    }
    t.add("}");
    g.index_bytes = t.used;
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kPluginIndexSignatureDomain.data), kPluginIndexSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(g.index), g.index_bytes}};
    u8 hash[32]{};
    return t.used < t.capacity && sha256(g.workspace, {parts, 2}, {hash, 32}).ok() && p256_sign_digest(g.signer, {hash, 32}, {g.signature, 64}).ok();
}

void serve() noexcept {
    plugin_fake::reset(g.server, "example.github.io");
    plugin_fake::serve(g.server, UpdateHost::pages, "/plugins/v1/index.json", 200, g.index, g.index_bytes);
    plugin_fake::serve(g.server, UpdateHost::pages, "/plugins/v1/index.json.sig", 200, g.signature, 64);
    for (u32 i = 0; i < 3; ++i) plugin_fake::serve(g.server, UpdateHost::github, kTargets[i], 200, g.dll, g.dll_bytes + 16 * i);
    plugin_fake::serve(g.server, UpdateHost::provider, "/custom/v1/index.json", 200, g.custom, g.custom_bytes);
    plugin_fake::serve(g.server, UpdateHost::provider, "/custom/v1/index.json.sig", 200, g.custom_signature, 64);
}
bool publish(const Spec& spec) noexcept {
    if (!build_index(spec)) return false;
    serve();
    return plugin_index_check(g.paths, g.source, plugin_fake::transport(g.server), g.workspace, &g.index_work, kNow).error ==
        PluginIndexCheckError::none;
}

// ---- files and records -----------------------------------------------------------------------
bool manifest_is(u32 v) noexcept {
    NativePluginManifest m{};
    return read_manifest(g.paths.plugins, g.paths.plugins_count, kOwner, &m) && bytes_equal(m.sha256, g.sha[v], 32);
}
bool manifest_present(u64 owner) noexcept {
    wchar_t path[kPluginManagerPathCapacity]{};
    return plugin_manager_id_path(g.paths.plugins, g.paths.plugins_count, owner, L".maicord-plugin.json", path) &&
        GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
// A manual install: a manifest the user copied in, naming bytes the catalog lists.
bool write_manual() noexcept {
    NativePluginManifest m{};
    m.owner_id = kManual;
    m.abi_version = 5;
    constexpr char name[] = "Manual", version[] = "1.0.0";
    copy_bytes(m.name, name, sizeof(name) - 1); m.name_bytes = sizeof(name) - 1;
    copy_bytes(m.version, version, sizeof(version) - 1); m.version_bytes = sizeof(version) - 1;
    constexpr char dll[] = "manual.dll";
    copy_bytes(m.dll, dll, sizeof(dll) - 1); m.dll_bytes = sizeof(dll) - 1;
    copy_bytes(m.sha256, g.manual_sha, 32);
    m.permissions = kSettings;
    char record[kNativePluginManifestMaximumBytes];
    u32 bytes = 0;
    wchar_t path[kPluginManagerPathCapacity]{};
    if (!native_plugin_manifest_write(m, {record, sizeof(record)}, &bytes).ok() ||
        !plugin_manager_id_path(g.paths.plugins, g.paths.plugins_count, kManual, L".maicord-plugin.json", path)) return false;
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, record, bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    return ok;
}
bool delete_manual() noexcept {
    wchar_t path[kPluginManagerPathCapacity]{};
    return plugin_manager_id_path(g.paths.plugins, g.paths.plugins_count, kManual, L".maicord-plugin.json", path) && DeleteFileW(path);
}
PluginManagerState g_state;
const PluginManagerEntry* entry(u64 owner) noexcept {
    g_state = {};
    if (!plugin_manager_state_load({g.paths.state, g.paths.state_count}, &g_state).ok()) return nullptr;
    for (u32 i = 0; i < g_state.count; ++i) if (g_state.entries[i].owner == owner) return &g_state.entries[i];
    return nullptr;
}
NativePluginPreferences g_trust;
u32 trust_count() noexcept {
    NativePluginPreferencesIo io{};
    g_trust = {};
    const auto loaded = native_plugin_preferences_load_file(&io, g.paths.trust, &g_trust);
    native_plugin_preferences_release(&io);
    return loaded.ok() ? g_trust.count : ~u32{0};
}
bool trust_is(u32 v) noexcept {
    return trust_count() == 1 && g_trust.entries[0].owner_id == kOwner && bytes_equal(g_trust.entries[0].sha256, g.sha[v], 32);
}
Op journal_operation() noexcept {
    PluginJournal journal{};
    const auto loaded = plugin_journal_load({g.paths.journal, g.paths.journal_count}, &journal);
    return loaded.ok() ? journal.operation : loaded.error == AcceptanceRecordError::missing ? Op::none : static_cast<Op>(0xFF);
}
PluginManagerResult prepare(Op operation, u32 v, u64 approved, bool enable_after = false) noexcept {
    serve();
    const PluginManagerRequest request{operation, kOwner, kCode[v], approved, enable_after};
    return plugin_manager_prepare(g.paths, g.source, request, plugin_fake::transport(g.server), g.workspace, &g.index_work, nullptr, &g.work,
        kNow, &g.prepared);
}
NativePluginBundleInstallResult install(const PluginManagerPrepared& prepared) noexcept {
    const auto started = native_plugin_bundle_install_start(&g.installer, {prepared.manifest, prepared.manifest_count},
        {g.paths.plugins, g.paths.plugins_count}, prepared.options);
    NativePluginBundleInstallResult result{};
    result.error = started;
    if (started == NativePluginBundleInstallError::none) result = plugin_manager_run_installer(&g.installer, kTimeoutMs);
    return result;
}
PluginStartupResult startup() noexcept { return plugin_manager_startup(g.paths, plugin_index_current_host(), &g.work, &g.gate); }
PluginEvaluation g_evaluation;
PluginManagerResult evaluate() noexcept {
    g_evaluation = {};
    const auto cached = plugin_index_load_cache(g.paths, g.source, g.workspace, &g.index_work, kNow);
    PluginCatalogPass pass{};
    pass.provider = kPluginProviderOfficial;
    pass.kind = PluginProviderKind::official;
    pass.index = cached.error == PluginIndexCheckError::none ? &g.index_work.index : nullptr;
    pass.document = plugin_index_work_document(g.index_work);
    pass.installable = cached.installable;
    pass.auto_update = true;
    return plugin_manager_evaluate(g.paths, pass, nullptr, plugin_index_current_host(), &g.work, &g_evaluation);
}

// ---- startup ---------------------------------------------------------------------------------
void test_startup() noexcept {
    auto r = startup();
    check(r.manager.ok() && !r.catalog_installed && !g.gate.count && !r.rolled_forward,
        "plugin startup: a client that never used the catalog has nothing to recover or gate");
    check(publish({kSequence, false, nullptr, false, false}), "plugin startup: the official fixture index is admitted");
    const bool installed = prepare(Op::install, 0, kSettings, true).ok() && install(g.prepared).committed &&
        plugin_manager_finish(g.paths, g.prepared, false, &g.work, &g.settled).ok() && manifest_is(0) && trust_is(0);
    check(installed, "plugin startup: 1.0.0 installed from the catalog and trusted");
    r = startup();
    check(r.manager.ok() && r.catalog_installed && !g.gate.count && r.settled.action == Action::none,
        "plugin startup: a catalog plugin makes scheduled checks allowed; a clear journal does nothing");
    // Download now, apply at next start.
    const bool staged = prepare(Op::update, 1, kSettings).ok() && journal_operation() == Op::update && manifest_is(0);
    r = startup();
    const auto* e = entry(kOwner);
    check(staged && r.rolled_forward && r.manager.ok() && r.settled.action == Action::finish && manifest_is(1) && trust_is(1) &&
        journal_operation() == Op::none && e && e->installed_code == kCode[1] && e->previous_code == kCode[0],
        "plugin startup: a staged official update is committed before anything loads, with its trust carried over");
    // Automatic updates turned off after staging: discarded instead.
    const bool staged_again = prepare(Op::update, 2, kSettings | g.menus).ok() &&
        plugin_manager_set_official_flags(g.paths, kPluginOfficialAutoUpdateOff, 0).ok();
    r = startup();
    check(staged_again && !r.rolled_forward && r.settled.action == Action::discard && manifest_is(1) && journal_operation() == Op::none,
        "plugin startup: with automatic updates off the staged update is discarded and 1.1.0 stays");
    check(plugin_manager_set_official_flags(g.paths, 0, kPluginOfficialAutoUpdateOff).ok(), "plugin startup: automatic updates on again");
}

// ---- evaluation ------------------------------------------------------------------------------
void test_evaluate() noexcept {
    check(write_manual() && publish({kSequence + 1, false, nullptr, false, true}), "plugin evaluation: a manual install and a newer index");
    auto r = evaluate();
    const auto* manual = entry(kManual);
    check(r.ok() && g_evaluation.adopted == 1 && manual && manual->provider == kPluginProviderOfficial && manual->installed_code == kCode[0] &&
        manual->slug[0] == 'm' && g_evaluation.state_written,
        "plugin evaluation: a manual install whose bytes the official index lists is adopted");
    check(g_evaluation.offers == 1 && g_evaluation.update_count == 1 && g_evaluation.updates[0].owner == kOwner &&
        g_evaluation.updates[0].version_code == kCode[2] && g_evaluation.updates[0].approved_permissions == kSettings && !g_evaluation.reviews,
        "plugin evaluation: 1.2.0 within the approved permissions is an automatic update candidate");
    check(publish({kSequence + 2, false, nullptr, false, false}) && evaluate().ok() && g_evaluation.reviews == 1 && !g_evaluation.update_count &&
        entry(kOwner) && (entry(kOwner)->flags & kPluginStateReviewPending),
        "plugin evaluation: growing permissions never update automatically and mark the plugin for review");
    // Malicious revocation of the installed build.
    check(trust_count() == 1, "plugin revocation: the installed plugin is trusted before");
    u64 loaded[1]{kOwner};
    u32 loaded_count = 1;
    check(publish({kSequence + 3, true, g.sha[1], false, false}) && evaluate().ok(), "plugin revocation: the revoking index is admitted");
    for (u32 i = 0; i < g_evaluation.revoked_count; ++i)
        for (u32 j = 0; j < loaded_count; ++j) if (loaded[j] == g_evaluation.revoked[i].owner) loaded[j] = loaded[--loaded_count];
    const auto* revoked = entry(kOwner);
    check(g_evaluation.revoked_count == 1 && g_evaluation.revoked[0].owner == kOwner && g_evaluation.revoked[0].name_bytes == 7 &&
        loaded_count == 0 && trust_count() == 0 && g_evaluation.trust_written && revoked && (revoked->flags & kPluginStateRevokedMalicious),
        "plugin revocation: the revoked plugin is listed for unloading (loaded 1 -> 0) and loses its trust (1 -> 0)");
    auto s = startup();
    check(s.manager.ok() && plugin_startup_gate_reason(g.gate, kOwner) == kPluginGateRevoked,
        "plugin revocation: the next start skips the revoked plugin");
    g_evaluation = {};
    PluginCatalogPass offline{};
    offline.provider = kPluginProviderOfficial;
    offline.kind = PluginProviderKind::official;
    check(plugin_manager_evaluate(g.paths, offline, nullptr, plugin_index_current_host(), &g.work, &g_evaluation).ok() &&
        entry(kOwner) && (entry(kOwner)->flags & kPluginStateRevokedMalicious),
        "plugin revocation: losing the cache never lifts a revocation");
    check(publish({kSequence + 4, false, nullptr, false, false}) && evaluate().ok() && entry(kOwner) &&
        !(entry(kOwner)->flags & kPluginStateRevokedMalicious) && !g_evaluation.revoked_count,
        "plugin revocation: a newer admitted index without it lifts the flag (trust stays removed)");
    // The blocklist applies to an adopted manual install.
    check(publish({kSequence + 5, false, nullptr, true, false}) && evaluate().ok() && g_evaluation.revoked_count == 1 &&
        g_evaluation.revoked[0].owner == kManual && entry(kManual) && (entry(kManual)->flags & kPluginStateRevokedMalicious),
        "plugin blocklist: blocked bytes are flagged malicious whichever provider installed them");
    // A host bound tightened by the catalog gates the next start.
    PluginManagerState state{};
    bool bounded = plugin_manager_state_load({g.paths.state, g.paths.state_count}, &state).ok();
    for (u32 i = 0; bounded && i < state.count; ++i) if (state.entries[i].owner == kOwner) state.entries[i].max_host_abi = 4;
    bounded = bounded && plugin_manager_state_save({g.paths.state, g.paths.state_count}, state).ok();
    s = startup();
    check(bounded && plugin_startup_gate_reason(g.gate, kOwner) == kPluginGateIncompatible &&
        plugin_startup_gate_reason(g.gate, kManual) == kPluginGateRevoked && g.gate.count == 2,
        "plugin startup gate: a stored max_host_abi below the host and a blocked build are skipped");
    check(evaluate().ok() && entry(kOwner) && entry(kOwner)->max_host_abi == 0, "plugin evaluation: the index restores max_host_abi");
    check(delete_manual() && evaluate().ok() && g_evaluation.pruned == 1 && !entry(kManual),
        "plugin evaluation: an entry whose manifest was deleted by hand is pruned");
}

// ---- snapshot --------------------------------------------------------------------------------
void test_snapshot() noexcept {
    PluginProviders providers{};
    PluginCatalogKeys keys{};
    PluginCatalogSources sources{};
    const u8* official[]{kGenerator};
    plugin_catalog_sources_build(utf8_literal("example/plugins"), {official, 1}, providers, &keys, &sources);
    PluginCatalogSnapshotInput input{};
    input.sources = &sources;
    input.providers = &providers;
    input.configured = true;
    PluginCatalogSnapshot* s = plugin_catalog_snapshot_build(g.paths, input, plugin_index_current_host(), g.workspace, &g.official_work,
        &g.index_work, &g.work, kNow);
    const PluginCatalogEntryView* fixture = nullptr;
    for (u32 i = 0; s && i < s->entry_count; ++i) if (s->entries[i].owner == kOwner) fixture = &s->entries[i];
    char name[32]{};
    const auto decoded = s && fixture ? plugin_index_text_decode(plugin_catalog_document(*s, fixture->provider), fixture->name, {name, 32})
                                      : JsonStringDecodeResult{};
    check(sources.official && sources.count == 1 && s && s->provider_count == 1 && s->providers[0].status == PluginCatalogStatus::ok &&
        s->providers[0].installable && s->providers[0].host_bytes == 17 && s->entry_count == 3 && s->version_count == 5 &&
        s->installed_count == 1 && fixture && fixture->bound && fixture->installed_index == 0 && s->installed[0].has_state &&
        s->installed[0].installed_code == kCode[1] && decoded.ok() && decoded.bytes == 7 &&
        fixture->update.outcome == PluginUpdateOutcome::available && s->installed[0].entry != ~0u,
        "plugin snapshot: providers, entries, versions and the installed plugin merge with its update offer");
    plugin_catalog_snapshot_release(s);
    input.key_changed_count = 1;
    input.key_changed[0] = kPluginProviderOfficial;
    s = plugin_catalog_snapshot_build(g.paths, input, plugin_index_current_host(), g.workspace, &g.official_work, &g.index_work, &g.work, kNow);
    check(s && s->providers[0].status == PluginCatalogStatus::key_changed, "plugin snapshot: a provider with a changed key says so");
    plugin_catalog_snapshot_release(s);
    plugin_catalog_keys_release(&keys);
}

// ---- the worker ------------------------------------------------------------------------------
u64 test_clock() noexcept { return kNow; }
PluginBrowserWorkerView g_view;
template <typename Ready> bool wait_view(Ready ready) noexcept {
    for (u32 attempt = 0; attempt < 64; ++attempt) {
        plugin_browser_worker_view(&g_view, ~u64{0});
        if (ready(g_view)) return true;
        if (WaitForSingleObject(plugin_browser_worker_notification(), kTimeoutMs) != WAIT_OBJECT_0) return false;
    }
    return false;
}
bool run_command(const PluginBrowserCommand& cmd) noexcept {
    u64 serial = 0;
    return plugin_browser_worker_command(cmd, &serial) &&
        wait_view([serial](const PluginBrowserWorkerView& v) noexcept { return v.command_serial == serial && v.phase == PluginBrowserPhase::idle; });
}
void test_worker() noexcept {
    g_probe_log_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_probe_log_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool gate_ready = g_probe_log_entered && g_probe_log_release;
    check(gate_ready, "plugin worker: completion log gate is available");
    if (!gate_ready) {
        if (g_probe_log_entered) CloseHandle(g_probe_log_entered);
        if (g_probe_log_release) CloseHandle(g_probe_log_release);
        g_probe_log_entered = g_probe_log_release = nullptr;
        return;
    }
    check(publish({kSequence + 6, false, nullptr, false, true}), "plugin worker: a fresh index to serve");
    PluginBrowserConfig config{};
    constexpr Utf8View repository = utf8_literal("example/plugins");
    copy_bytes(config.repository, repository.data, repository.bytes);
    config.repository_bytes = repository.bytes;
    copy_bytes(config.keys[0], kGenerator, 65);
    config.key_count = 1;
    PluginBrowserWorkerOptions options{};
    options.transport = plugin_fake::transport(g.server);
    options.base = g.base;
    options.base_count = g.base_count;
    options.clock = test_clock;
    const u32 requests = g.server.requests;
    check(plugin_browser_worker_start(config, options), "plugin worker: starts with an injected transport");
    // A catalog plugin is installed, so the first check is due at once.
    check(wait_view([](const PluginBrowserWorkerView& v) noexcept { return v.checked_unix == kNow && v.phase == PluginBrowserPhase::idle; }) &&
        g_view.revision >= 2 && g_view.offers == 1 && g_view.staged == 1 && g.server.requests >= requests + 2 && !g.server.wrong_host,
        "plugin worker: the scheduled check publishes checking, then idle with one offer, and stages the automatic update");
    check(journal_operation() == Op::update, "plugin worker: the automatic update waits in the journal for the next start");
    plugin_browser_worker_want_snapshot(true);
    PluginCatalogSnapshot* snapshot = nullptr;
    check(wait_view([](const PluginBrowserWorkerView& v) noexcept { return v.snapshot_serial >= 1; }) &&
        (snapshot = plugin_browser_worker_take_snapshot()) != nullptr && snapshot->entry_count == 3 && snapshot->installed_count == 1,
        "plugin worker: a wanted snapshot is built from the caches and handed over");
    plugin_catalog_snapshot_release(snapshot);
    // A user update of the same version reuses the staged files.
    PluginBrowserCommand cmd{};
    cmd.kind = PluginBrowserCommandKind::operation;
    cmd.operation = Op::update;
    cmd.owner = kOwner;
    cmd.provider = kPluginProviderOfficial;
    cmd.version_code = kCode[2];
    cmd.approved = kSettings;
    const u32 hits = g.server.routes[4].hits;
    check(run_command(cmd) && g_view.manager.ok() && g_view.prepared.journal.to_code == kCode[2] && g.server.routes[4].hits == hits,
        "plugin worker: an update of the staged version reuses its files without downloading again");
    const bool committed = install(g_view.prepared).committed;
    cmd = {};
    cmd.kind = PluginBrowserCommandKind::finish;
    check(committed && run_command(cmd) && g_view.manager.ok() && g_view.settled.action == Action::finish && manifest_is(2) &&
        journal_operation() == Op::none, "plugin worker: finish settles the operation after the installer commits");
    // Uninstall keeps the data unless asked.
    cmd = {};
    cmd.kind = PluginBrowserCommandKind::uninstall;
    cmd.owner = kOwner;
    const bool journaled = run_command(cmd) && g_view.manager.ok();
    const auto started = native_plugin_bundle_uninstall_start(&g.installer, kOwner, {g.paths.plugins, g.paths.plugins_count});
    const bool removed = started == NativePluginBundleInstallError::none && plugin_manager_run_installer(&g.installer, kTimeoutMs).committed;
    cmd = {};
    cmd.kind = PluginBrowserCommandKind::finish;
    check(journaled && removed && run_command(cmd) && g_view.settled.action == PluginRecoveryAction::finish_uninstall &&
        !manifest_present(kOwner) && !entry(kOwner), "plugin worker: uninstall, the installer, then finish remove plugin and records");
    // Install with enable_after: trusted when settled.
    cmd = {};
    cmd.kind = PluginBrowserCommandKind::operation;
    cmd.operation = Op::install;
    cmd.owner = kOwner;
    cmd.provider = kPluginProviderOfficial;
    cmd.version_code = kCode[2];
    cmd.approved = kSettings;
    cmd.enable_after = true;
    const bool fetched = run_command(cmd) && g_view.manager.ok();
    const bool installed = fetched && install(g_view.prepared).committed;
    cmd = {};
    cmd.kind = PluginBrowserCommandKind::finish;
    check(installed && run_command(cmd) && g_view.settled.trusted && trust_is(2), "plugin worker: install with enable_after trusts the plugin");
    // Add provider: a bad address, then a signed custom provider.
    cmd = {};
    cmd.kind = PluginBrowserCommandKind::probe;
    constexpr char bad[] = "http://example.github.io/custom/";
    copy_bytes(cmd.text, bad, sizeof(bad) - 1);
    cmd.text_bytes = sizeof(bad) - 1;
    check(run_command(cmd) && g_view.address_error == PluginProviderAddressError::scheme, "plugin worker: a non-https address is refused");
    constexpr char good[] = "https://example.github.io/custom";
    copy_bytes(cmd.text, good, sizeof(good) - 1);
    cmd.text_bytes = sizeof(good) - 1;
    InterlockedExchange(&g_probe_log_armed, 1);
    const bool probed = run_command(cmd) && g_view.address_error == PluginProviderAddressError::none &&
        g_view.probe.error == PluginProviderProbeError::none && g_view.probe.signed_provider && g_view.probe.requests == 2 &&
        g_view.address.path_bytes == 8;
    const bool entered = WaitForSingleObject(g_probe_log_entered, kTimeoutMs) == WAIT_OBJECT_0;
    cmd = {};
    cmd.kind = PluginBrowserCommandKind::add_provider;
    u64 add_serial = 0;
    const bool admitted = plugin_browser_worker_command(cmd, &add_serial);
    SetEvent(g_probe_log_release);
    check(probed, "plugin worker: the probe fetches the signed custom index without admitting it");
    const bool added = admitted && wait_view([add_serial](const PluginBrowserWorkerView& v) noexcept {
        return v.command_serial == add_serial && v.phase == PluginBrowserPhase::idle;
    });
    check(entered && added && InterlockedCompareExchange(&g_probe_log_released, 0, 0) == 1,
        "plugin worker: a published completion admits the next command before returning");
    PluginProviders providers{};
    check(added && g_view.add.error == PluginProviderAddError::none &&
        plugin_providers_load({g.paths.providers, g.paths.providers_count}, &providers).ok() && providers.count == 1,
        "plugin worker: the confirmed probe is saved, admitted and cached");
    const u64 provider = providers.entries[0].id;
    cmd = {};
    cmd.kind = PluginBrowserCommandKind::remove_provider;
    cmd.provider = provider;
    providers = {};
    check(run_command(cmd) && g_view.record.ok() && plugin_providers_load({g.paths.providers, g.paths.providers_count}, &providers).ok() &&
        providers.count == 0, "plugin worker: removing the provider drops its entry");
    // A revocation reaches the UI through the view.
    const u64 revoked_serial = g_view.revoked_serial;
    check(publish({kSequence + 7, true, g.sha[2], false, true}) && plugin_browser_worker_check() &&
        wait_view([revoked_serial](const PluginBrowserWorkerView& v) noexcept { return v.revoked_serial > revoked_serial && v.phase == PluginBrowserPhase::idle; }) &&
        g_view.revoked_count == 1 && g_view.revoked[0].owner == kOwner && trust_count() == 0,
        "plugin worker: a manual check publishes the revocation and removes the trust");
    plugin_browser_worker_stop();
    check(!plugin_browser_worker_running(), "plugin worker: stops and frees its state");
    InterlockedExchange(&g_probe_log_armed, 0);
    CloseHandle(g_probe_log_entered);
    CloseHandle(g_probe_log_release);
    g_probe_log_entered = g_probe_log_release = nullptr;
}

bool setup() noexcept {
    g.base_count = make_root(L"MPR", g.root, g.base);
    if (!g.base_count || !plugin_manager_paths_build({g.base, g.base_count}, &g.paths)) return false;
    if (!read_dll(&g.dll, &g.dll_bytes, 1)) return false;
    // The signed custom fixture shared with tests/plugin_providers.cpp.
    wchar_t path[MAX_PATH]{};
    const auto read = [&](const wchar_t* leaf, u8** out, u32* bytes, u32 capacity) noexcept {
        u32 at = 0;
        for (const wchar_t* c = MAICORD_PLUGIN_INDEX_FIXTURES; *c && at < MAX_PATH - 40; ++c) path[at++] = *c == L'/' ? L'\\' : *c;
        path[at++] = L'\\';
        for (const wchar_t* c = leaf; *c; ++c) path[at++] = *c;
        path[at] = 0;
        const HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER s{};
        bool done = GetFileSizeEx(f, &s) && s.QuadPart > 0 && s.QuadPart <= capacity;
        if (done && !*out) *out = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD n = 0;
        done = done && *out && ReadFile(f, *out, static_cast<DWORD>(s.QuadPart), &n, nullptr) && n == s.QuadPart;
        CloseHandle(f);
        *bytes = static_cast<u32>(s.QuadPart);
        return done;
    };
    u8* signature = g.custom_signature;
    u32 signature_bytes = 0;
    if (!read(L"custom.index.json", &g.custom, &g.custom_bytes, kPluginIndexMaximumBytes) ||
        !read(L"custom.index.json.sig", &signature, &signature_bytes, 64) || signature_bytes != 64) return false;
    for (u32 i = 0; i < sizeof(g.blob); ++i) g.blob[i] = 'A';
    u8 scalar[32]{};
    scalar[31] = 1; // test-only private key; its public point is the generator
    const P256Key* keys[]{&g.trusted};
    g.menus = 1ull << native_plugin_permission_bit(utf8_literal("menus"));
    u8* padded = static_cast<u8*>(VirtualAlloc(nullptr, g.dll_bytes + 64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    bool ok = padded != nullptr;
    if (ok) {
        copy_bytes(padded, g.dll, g.dll_bytes);
        ok = kdf_sha256_initialize(&g.workspace).ok() && digest(padded, g.dll_bytes + 48, g.manual_sha);
        VirtualFree(padded, 0, MEM_RELEASE);
    }
    return ok && p256_import_private(&g.signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_import_public(&g.trusted, P256Kind::ecdsa, {kGenerator, 65}).ok() && digest(g.dll, g.dll_bytes, g.sha[0]) &&
        digest(g.dll, g.dll_bytes + 16, g.sha[1]) && digest(g.dll, g.dll_bytes + 32, g.sha[2]) && digest(g.blob, sizeof(g.blob), g.blob_sha) &&
        plugin_catalog_source_official(utf8_literal("example/plugins"), {keys, 1}, &g.source);
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_runtime_tests_entry() noexcept {
    using namespace maicord;
    g_update_log_sink = capture_log;
    const bool ready = setup();
    check(ready, "plugin runtime: temporary root, the fixture DLL, its versions and the signed fixtures");
    if (ready) {
        test_startup();
        test_evaluate();
        test_snapshot();
        test_worker();
    }
    check(g_log_lines > 0 && g_log_paths == 0, "plugin runtime: log lines carry hosts and numbers, never a URL path");
    check(teardown(g.paths, g.base, g.root, true), "plugin runtime: every test file and directory was removed");
    p256_release(&g.trusted); p256_release(&g.signer);
    kdf_sha256_release(&g.workspace);
    if (g.dll) VirtualFree(g.dll, 0, MEM_RELEASE);
    if (g.custom) VirtualFree(g.custom, 0, MEM_RELEASE);
    ExitProcess(g_failures ? 1 : 0);
}
