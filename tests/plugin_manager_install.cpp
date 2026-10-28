// Catalog install, update, review, rollback and uninstall through the bundle
// installer, download failures, and a crash at each journal phase followed by
// recovery. Versions are MaiCordPluginFixture with zero bytes appended; nothing
// is loaded. Isolated TEMP files; test-only scalar-one key.
#include "maicord/app/plugin_manager_install.hpp"
#include "maicord/app/plugin_bundle_install.hpp"
#include "plugin_manager_fixture.hpp"
#include "plugin_fake_transport.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using ME = PluginManagerError;
using namespace plugin_manager_fixture;
using Op = PluginJournalOperation;
using Ph = PluginJournalPhase;
using Action = PluginRecoveryAction;

u32 g_failures;
void check(bool passed, const char* name) noexcept { test_check(&g_failures, passed, name); }

struct Fixture {
    KdfSha256Workspace workspace;
    P256Key signer, trusted;
    wchar_t root[MAX_PATH], base[MAX_PATH];
    PluginManagerPaths paths;
    PluginCatalogSource source;
    plugin_fake::Server server;
    u8* dll;           // the fixture DLL followed by 64 zero bytes
    u8* bad;           // the same with one appended byte changed
    u32 dll_bytes;
    u8 sha[3][32], blob_sha[32];
    u8 blob[512];
    char index[8192];
    u32 index_bytes;
    u8 signature[64];
    PluginIndexWork index_work;
    PluginManagerWork work;
    PluginManagerPrepared prepared;
    PluginManagerSettled settled;
    NativePluginBundleInstall installer;
    u64 permissions_menus;
};
Fixture g;

bool digest(const u8* data, u32 bytes, u8* out) noexcept {
    const ConstSlice<u8> part{data, bytes};
    return sha256(g.workspace, {&part, 1}, {out, 32}).ok();
}
// ---- the signed official index -------------------------------------------------------------
bool build_index() noexcept {
    Text t{g.index, sizeof(g.index), 0};
    t.add(R"({"format":1,"provider":{"id":"maicord","name":"MaiCord plugins","homepage":"https://plugins.example.com/maicord"},)");
    t.add(R"("sequence":1760227200,"issued":1760226600,"expires":1762818600,"plugins":[)");
    plugin(t, "fixture", kOwner, "Fixture");
    version(t, "1.2.0", kCode[2], R"("settings","menus")", g.sha[2], g.dll_bytes + 32, false);
    version(t, "1.1.0", kCode[1], R"("settings")", g.sha[1], g.dll_bytes + 16, false);
    version(t, "1.0.0", kCode[0], R"("settings")", g.sha[0], g.dll_bytes, true);
    t.add("]},");
    plugin(t, "notdll", kNotDll, "Fixture");
    version(t, "1.0.0", kCode[0], "", g.blob_sha, sizeof(g.blob), true);
    t.add(R"(]}],"revoked":[]})");
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
    plugin_fake::serve(g.server, UpdateHost::github, "/example/plugins/releases/download/notdll-v1.0.0/notdll-1.0.0-x64.dll", 200, g.blob,
        sizeof(g.blob));
}

// ---- counting what is on disk ---------------------------------------------------------------
struct Files { u32 manifests, dlls, other; };
Files files(const wchar_t* directory, u32 count, u64 owner) noexcept {
    Files f{};
    wchar_t pattern[kPluginManagerPathCapacity]{}, prefix[17]{};
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (u32 i = 0; i < 16; ++i) prefix[i] = digits[(owner >> ((15 - i) * 4)) & 15];
    copy_bytes(pattern, directory, count * 2);
    copy_bytes(pattern + count, L"\\*", 6);
    WIN32_FIND_DATAW found{};
    const HANDLE scan = FindFirstFileW(pattern, &found);
    if (scan == INVALID_HANDLE_VALUE) return f;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        u32 n = 0;
        while (found.cFileName[n]) ++n;
        bool mine = n >= 16;
        for (u32 i = 0; mine && i < 16; ++i) mine = found.cFileName[i] == prefix[i];
        if (!mine) { ++f.other; continue; }
        if (n == 85) ++f.dlls;
        else if (n == 36) ++f.manifests;
        else ++f.other;
    } while (FindNextFileW(scan, &found));
    FindClose(scan);
    return f;
}
bool plugins_are(u32 manifests, u32 dlls) noexcept {
    const Files f = files(g.paths.plugins, g.paths.plugins_count, kOwner);
    return f.manifests == manifests && f.dlls == dlls;
}
u32 staging_files() noexcept {
    const Files f = files(g.paths.staging, g.paths.staging_count, kOwner);
    return f.manifests + f.dlls + f.other;
}
bool dll_present(u32 v) noexcept {
    wchar_t path[kPluginManagerPathCapacity]{};
    constexpr wchar_t digits[] = L"0123456789abcdef";
    u32 n = g.paths.plugins_count;
    copy_bytes(path, g.paths.plugins, n * 2);
    path[n++] = L'\\';
    for (u32 i = 0; i < 16; ++i) path[n++] = digits[(kOwner >> ((15 - i) * 4)) & 15];
    path[n++] = L'-';
    for (u32 i = 0; i < 32; ++i) { path[n++] = digits[g.sha[v][i] >> 4]; path[n++] = digits[g.sha[v][i] & 15]; }
    copy_bytes(path + n, L".dll", 10);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
// The installed manifest's digest is version v's.
bool manifest_is(u32 v) noexcept {
    NativePluginManifest m{};
    return read_manifest(g.paths.plugins, g.paths.plugins_count, kOwner, &m) && bytes_equal(m.sha256, g.sha[v], 32);
}
// The rollback snapshot is version v's; ~0 means no snapshot.
bool snapshot_is(u32 v) noexcept {
    NativePluginManifest m{};
    bool present = false;
    const bool parsed = read_manifest(g.paths.rollback, g.paths.rollback_count, kOwner, &m, &present);
    return v == ~u32{0} ? !present : parsed && bytes_equal(m.sha256, g.sha[v], 32);
}
PluginManagerState g_state;
NativePluginPreferences g_trust;
u32 state_count() noexcept {
    g_state = {};
    const auto loaded = plugin_manager_state_load({g.paths.state, g.paths.state_count}, &g_state);
    return loaded.ok() ? g_state.count : loaded.error == AcceptanceRecordError::missing ? 0 : ~u32{0};
}
const PluginManagerEntry* entry() noexcept {
    state_count();
    for (u32 i = 0; i < g_state.count; ++i) if (g_state.entries[i].owner == kOwner) return &g_state.entries[i];
    return nullptr;
}
bool state_is(u32 installed, u32 previous, u64 held = 0) noexcept {
    const PluginManagerEntry* e = entry();
    return e && e->provider == kPluginProviderOfficial && e->installed_code == kCode[installed] && bytes_equal(e->installed_sha256, g.sha[installed], 32) &&
        (previous == ~u32{0} ? !e->previous_code : e->previous_code == kCode[previous] && bytes_equal(e->previous_sha256, g.sha[previous], 32)) &&
        e->held_code == held && ((e->flags & kPluginStateHeld) != 0) == (held != 0) && e->slug[0] == 'f' && e->admitted_sequence == 1760227200;
}
u32 trust_count() noexcept {
    NativePluginPreferencesIo io{};
    g_trust = {};
    const bool ok = native_plugin_preferences_load_file(&io, g.paths.trust, &g_trust).ok();
    native_plugin_preferences_release(&io);
    return ok ? g_trust.count : ~u32{0};
}
bool trust_is(u32 v, u64 permissions) noexcept {
    if (trust_count() != 1) return false;
    return g_trust.entries[0].owner_id == kOwner && bytes_equal(g_trust.entries[0].sha256, g.sha[v], 32) && g_trust.entries[0].permissions == permissions;
}
Op journal_operation() noexcept {
    PluginJournal journal{};
    const auto loaded = plugin_journal_load({g.paths.journal, g.paths.journal_count}, &journal);
    return loaded.ok() ? journal.operation : loaded.error == AcceptanceRecordError::missing ? Op::none : static_cast<Op>(0xFF);
}
bool settled_clean() noexcept { return journal_operation() == Op::none && staging_files() == 0; }

// ---- driving one operation -------------------------------------------------------------------
// Routes: 0 index, 1 signature, 2-4 versions 1.0.0-1.2.0, 5 the non-DLL blob.
PluginManagerResult prepare_served(const PluginManagerRequest& request) noexcept {
    return plugin_manager_prepare(g.paths, g.source, request, plugin_fake::transport(g.server), g.workspace, &g.index_work, nullptr, &g.work,
        kNow, &g.prepared);
}
PluginManagerResult prepare(Op operation, u32 v, u64 approved, bool enable_after = false) noexcept {
    serve();
    return prepare_served({operation, kOwner, operation == Op::rollback ? 0 : kCode[v], approved, enable_after});
}
// The installer handshake the UI performs; nothing is loaded, so it commits at prepared.
NativePluginBundleInstallResult install(u32 fault = 0) noexcept {
    native_plugin_bundle_install_fixture_failures(fault);
    const auto started = native_plugin_bundle_install_start(&g.installer, {g.prepared.manifest, g.prepared.manifest_count},
        {g.paths.plugins, g.paths.plugins_count}, g.prepared.options);
    NativePluginBundleInstallResult result{};
    result.error = started;
    if (started == NativePluginBundleInstallError::none) result = plugin_manager_run_installer(&g.installer, kTimeoutMs);
    native_plugin_bundle_install_fixture_failures(0);
    return result;
}
NativePluginBundleInstallResult uninstall() noexcept {
    const auto started = native_plugin_bundle_uninstall_start(&g.installer, kOwner, {g.paths.plugins, g.paths.plugins_count});
    NativePluginBundleInstallResult result{};
    result.error = started;
    if (started == NativePluginBundleInstallError::none) result = plugin_manager_run_installer(&g.installer, kTimeoutMs);
    return result;
}
PluginManagerResult finish(bool delete_data = false) noexcept {
    return plugin_manager_finish(g.paths, g.prepared, delete_data, &g.work, &g.settled);
}
PluginManagerResult recover() noexcept { return plugin_manager_recover(g.paths, &g.work, &g.settled); }
// A complete operation: prepare, the installer, finish.
bool run(Op operation, u32 v, u64 approved, bool enable_after = false) noexcept {
    return prepare(operation, v, approved, enable_after).ok() && install().committed && finish().ok() && g.settled.action == Action::finish;
}

void test_lifecycle() noexcept {
    serve();
    const auto checked = plugin_index_check(g.paths, g.source, plugin_fake::transport(g.server), g.workspace, &g.index_work, kNow);
    check(checked.error == PluginIndexCheckError::none && checked.outcome == PluginIndexCheckOutcome::updated && g.index_work.index.plugin_count == 2,
        "plugin manager: the official fixture index is checked and admitted");
    // Install 1.0.0 and enable it.
    auto r = prepare(Op::install, 0, kSettings, true);
    check(r.ok() && g.prepared.journal.phase == Ph::staged && journal_operation() == Op::install && staging_files() == 2 &&
        plugins_are(0, 0) && g.server.routes[2].hits == 1, "plugin install: prepare downloads and stages the DLL and manifest, journal staged");
    const auto installed = install();
    r = finish();
    check(installed.committed && r.ok() && g.settled.action == Action::finish && g.settled.trusted && state_count() == 1 && state_is(0, ~u32{0}) &&
        trust_is(0, kSettings) && plugins_are(1, 1) && manifest_is(0) && settled_clean() && snapshot_is(~u32{0}),
        "plugin install: state 1, trust 1, files 2 (manifest and DLL), journal clear and staging empty");
    // A build flagged by an earlier check: the next build starts unflagged.
    PluginManagerState flagged{};
    bool marked = plugin_manager_state_load({g.paths.state, g.paths.state_count}, &flagged).ok() && flagged.count == 1;
    if (marked) flagged.entries[0].flags |= kPluginStateRevokedBroken | kPluginStateWithdrawn;
    marked = marked && plugin_manager_state_save({g.paths.state, g.paths.state_count}, flagged).ok();
    // Same permissions: the trust follows the new digest; the previous version is retained.
    check(run(Op::update, 1, kSettings) && state_is(1, 0) && trust_is(1, kSettings) && plugins_are(1, 2) && dll_present(0) && dll_present(1) &&
        manifest_is(1) && snapshot_is(0) && settled_clean(),
        "plugin update: same permissions keep 2 DLLs, the trust SHA follows 1.1.0 and the snapshot names 1.0.0");
    check(marked && entry() && !(entry()->flags & (kPluginStateRevokedBroken | kPluginStateWithdrawn)),
        "plugin update: revocation flags of the replaced build do not carry over");
    // More permissions: review, nothing written.
    r = prepare(Op::update, 2, kSettings);
    check(r.error == ME::review && r.permissions.added == g.permissions_menus && r.permissions.kept == kSettings && plugins_are(1, 2) &&
        manifest_is(1) && settled_clean() && g.server.routes[4].hits == 0 && state_is(1, 0),
        "plugin update: more permissions stop at needs_review with files, state and journal unchanged and nothing downloaded");
    // Roll back to the retained 1.0.0: held at 1.1.0.
    check(run(Op::rollback, 0, kSettings) && manifest_is(0) && state_is(0, 1, kCode[1]) && trust_is(0, kSettings) && plugins_are(1, 2) &&
        snapshot_is(1) && settled_clean(), "plugin rollback: manifest SHA = previous, held = 1.1.0, both DLLs and the snapshot kept");
    r = prepare(Op::update, 0, kSettings);
    check(r.error == ME::current && settled_clean(), "plugin update: the installed version is current");
    // Uninstall with the data option.
    wchar_t data[kPluginManagerPathCapacity]{}, store[kPluginManagerPathCapacity]{};
    const u32 data_count = plugin_manager_id_path(g.paths.data, g.paths.data_count, kOwner, L"", data);
    const bool data_ready = data_count && CreateDirectoryW(g.paths.data, nullptr) && CreateDirectoryW(data, nullptr) &&
        plugin_manager_id_path(data, data_count, 0x73746f7265ull, L".bin", store);
    const HANDLE file = data_ready ? CreateFileW(store, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr) : INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    r = plugin_manager_uninstall_prepare(g.paths, kOwner, &g.work, &g.prepared);
    check(r.ok() && journal_operation() == Op::uninstall && trust_count() == 0 && plugins_are(1, 2),
        "plugin uninstall: the intent is journaled and the trust removed before the installer runs");
    const auto removed = uninstall();
    r = finish(true);
    check(file != INVALID_HANDLE_VALUE && removed.committed && removed.removed == 2 && r.ok() && g.settled.action == Action::finish_uninstall &&
        g.settled.data_removed && plugins_are(0, 0) && state_count() == 0 && trust_count() == 0 && snapshot_is(~u32{0}) &&
        GetFileAttributesW(data) == INVALID_FILE_ATTRIBUTES && settled_clean(),
        "plugin uninstall: 0 files, 0 state, 0 trust, no snapshot and plugin-data removed when asked");
    RemoveDirectoryW(g.paths.data);
}

void test_download_failures() noexcept {
    const PluginManagerRequest request{Op::install, kOwner, kCode[1], kSettings, false};
    // Same size, another digest: nothing reaches plugins\.
    serve();
    g.server.routes[3].body = g.bad;
    auto r = prepare_served(request);
    check(r.error == ME::integrity && g.server.routes[3].hits == 1 && plugins_are(0, 0) && settled_clean() && state_count() == 0,
        "plugin download: a hash mismatch leaves the plugins directory untouched and staging empty");
    serve();
    g.server.routes[3].body_bytes = g.dll_bytes;
    r = prepare_served(request);
    check(r.error == ME::fetch && r.fetch_error == UpdateFetchError::size && plugins_are(0, 0) && settled_clean(),
        "plugin download: a size mismatch is refused at the exact signed size");
    serve();
    r = prepare_served({Op::install, kNotDll, kCode[0], 0, false});
    check(r.error == ME::image && files(g.paths.plugins, g.paths.plugins_count, kNotDll).dlls == 0 && settled_clean(),
        "plugin download: correctly signed bytes that are not an AMD64 DLL are refused");
    serve();
    g.server.routes[3].status = 404;
    r = prepare_served(request);
    check(r.error == ME::fetch && r.fetch_error == UpdateFetchError::not_found && settled_clean(), "plugin download: a missing release asset fails cleanly");
    serve();
    r = prepare_served({Op::install, kOwner, 1003000999, kSettings, false});
    check(r.error == ME::not_listed && settled_clean(), "plugin install: an unlisted version is refused");
    r = prepare_served({Op::install, kOwner, kCode[2], kSettings, true});
    check(r.error == ME::review && r.permissions.added == g.permissions_menus && settled_clean(),
        "plugin install: enabling with less than the version's permissions asks for review");
    r = prepare_served({Op::update, kOwner, kCode[1], kSettings, false});
    check(r.error == ME::not_installed && settled_clean(), "plugin update: an owner that is not installed cannot be updated");
    PluginJournal busy{};
    busy.operation = Op::install; busy.phase = Ph::intent; busy.owner = kNotDll;
    const bool journaled = plugin_journal_save({g.paths.journal, g.paths.journal_count}, busy).ok();
    r = prepare_served(request);
    const bool refused = r.error == ME::busy && journal_operation() == Op::install;
    check(journaled && refused && recover().ok() && g.settled.action == Action::discard && settled_clean(),
        "plugin manager: a journaled operation blocks the next until recovery settles it");
}

// Each phase of the journal, interrupted, then recovered.
void test_recovery() noexcept {
    check(run(Op::install, 0, kSettings, true) && plugins_are(1, 1) && trust_is(0, kSettings), "plugin recovery: 1.0.0 installed and trusted");
    // intent: the process died while downloading.
    PluginJournal journal{};
    journal.operation = Op::update; journal.phase = Ph::intent; journal.owner = kOwner; journal.provider = kPluginProviderOfficial;
    journal.to_code = kCode[1]; journal.approved_permissions = kSettings; journal.had_trust = true;
    copy_bytes(journal.from_sha256, g.sha[0], 32); copy_bytes(journal.to_sha256, g.sha[1], 32);
    wchar_t partial[kPluginManagerPathCapacity]{};
    const bool staged = plugin_journal_save({g.paths.journal, g.paths.journal_count}, journal).ok() &&
        plugin_manager_id_path(g.paths.staging, g.paths.staging_count, kOwner, L"-partial.dll.download", partial);
    const HANDLE file = staged ? CreateFileW(partial, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr) : INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) { DWORD written = 0; WriteFile(file, g.dll, 100, &written, nullptr); CloseHandle(file); }
    auto r = recover();
    check(file != INVALID_HANDLE_VALUE && r.ok() && g.settled.action == Action::discard && g.settled.staging_removed == 1 && settled_clean() &&
        plugins_are(1, 1) && manifest_is(0) && state_is(0, ~u32{0}), "plugin recovery: intent discards the partial download and clears the journal");
    // staged: prepared, then the process died before the installer ran.
    r = prepare(Op::update, 1, kSettings);
    const u32 staged_files = staging_files();
    const auto discarded = recover();
    check(r.ok() && staged_files == 3 && discarded.ok() && g.settled.action == Action::discard && settled_clean() && plugins_are(1, 1) &&
        manifest_is(0) && !dll_present(1) && state_is(0, ~u32{0}) && trust_is(0, kSettings),
        "plugin recovery: staged with the old manifest discards staging and keeps 1.0.0");
    // staged: the installer crashed just before its commit (orphan DLL and staged manifest).
    r = prepare(Op::update, 1, kSettings);
    const auto crashed = install(8);
    const bool orphaned = plugins_are(1, 2) && files(g.paths.plugins, g.paths.plugins_count, kOwner).other == 1;
    const auto cleaned = recover();
    check(r.ok() && !crashed.committed && orphaned && cleaned.ok() && g.settled.action == Action::discard && g.settled.removed_dlls == 1 &&
        g.settled.removed_manifests == 1 && plugins_are(1, 1) && files(g.paths.plugins, g.paths.plugins_count, kOwner).other == 0 &&
        manifest_is(0) && settled_clean(), "plugin recovery: a crash before commit removes exactly the orphan DLL and the staged manifest");
    // staged: the installer committed, the process died before finish.
    r = prepare(Op::update, 1, kSettings);
    const auto committed = install();
    const bool between = journal_operation() == Op::update && manifest_is(1) && state_is(0, ~u32{0});
    const auto finished = recover();
    check(r.ok() && committed.committed && between && finished.ok() && g.settled.action == Action::finish && g.settled.trusted &&
        state_is(1, 0) && trust_is(1, kSettings) && snapshot_is(0) && plugins_are(1, 2) && settled_clean(),
        "plugin recovery: staged with the new manifest finishes state, trust and snapshot");
    // committed: finish wrote its first record, then the process died.
    r = prepare(Op::update, 2, kSettings | g.permissions_menus);
    const auto third = install();
    journal = g.prepared.journal;
    journal.phase = Ph::committed;
    const bool marked = plugin_journal_save({g.paths.journal, g.paths.journal_count}, journal).ok();
    const auto resumed = recover();
    check(r.ok() && third.committed && third.removed == 1 && marked && resumed.ok() && g.settled.action == Action::finish && state_is(2, 1) &&
        trust_is(2, kSettings | g.permissions_menus) && plugins_are(1, 2) && dll_present(1) && dll_present(2) && snapshot_is(1) && settled_clean(),
        "plugin recovery: committed finishes idempotently with the approved larger mask");
    const auto again = recover();
    check(again.ok() && g.settled.action == Action::none && state_is(2, 1) && plugins_are(1, 2), "plugin recovery: a clear journal does nothing");
    // uninstall: the process died before the installer ran (manifest present).
    r = plugin_manager_uninstall_prepare(g.paths, kOwner, &g.work, &g.prepared);
    const auto restarted = recover();
    check(r.ok() && restarted.ok() && g.settled.action == Action::finish_uninstall && plugins_are(0, 0) && state_count() == 0 &&
        trust_count() == 0 && snapshot_is(~u32{0}) && settled_clean(), "plugin recovery: an uninstall before its commit is run again to the end");
    // uninstall: the installer deleted the manifest, the process died before finish.
    check(run(Op::install, 0, kSettings, true), "plugin recovery: 1.0.0 installed again");
    r = plugin_manager_uninstall_prepare(g.paths, kOwner, &g.work, &g.prepared);
    const auto deleted = uninstall();
    const auto completed = recover();
    check(r.ok() && deleted.committed && completed.ok() && g.settled.action == Action::finish_uninstall && plugins_are(0, 0) &&
        state_count() == 0 && trust_count() == 0 && settled_clean(), "plugin recovery: an uninstall after its commit removes the records");
}

void test_plan() noexcept {
    PluginJournal j{};
    u8 from[32]{}, to[32]{}, other[32]{};
    from[0] = 1; to[0] = 2; other[0] = 3;
    struct Row { Op op; Ph phase; bool fresh; const u8* manifest; Action expected; };
    const Row rows[]{
        {Op::none, Ph::none, false, nullptr, Action::none},
        {Op::install, Ph::intent, true, nullptr, Action::discard},
        {Op::update, Ph::intent, false, to, Action::discard},
        {Op::install, Ph::staged, true, nullptr, Action::discard},
        {Op::update, Ph::staged, false, from, Action::discard},
        {Op::update, Ph::staged, false, to, Action::finish},
        {Op::rollback, Ph::staged, false, from, Action::discard},
        {Op::update, Ph::committed, false, to, Action::finish},
        {Op::update, Ph::committed, false, from, Action::clear},
        {Op::update, Ph::staged, false, other, Action::clear},
        {Op::update, Ph::staged, false, nullptr, Action::clear},
        {Op::install, Ph::staged, true, other, Action::clear},
        {Op::uninstall, Ph::intent, false, from, Action::restart_uninstall},
        {Op::uninstall, Ph::committed, false, nullptr, Action::finish_uninstall},
    };
    u32 matched = 0;
    for (const Row& row : rows) {
        j = {};
        j.operation = row.op; j.phase = row.phase; j.owner = row.op == Op::none ? 0 : kOwner;
        if (!row.fresh && row.op != Op::none) copy_bytes(j.from_sha256, from, 32);
        if (row.op != Op::none && row.op != Op::uninstall) copy_bytes(j.to_sha256, to, 32);
        matched += plugin_manager_recovery_plan(j, row.manifest) == row.expected;
    }
    check(matched == sizeof(rows) / sizeof(rows[0]), "plugin recovery plan: 14 journal and manifest combinations");
}

bool setup() noexcept {
    const u32 count = make_root(L"MPM", g.root, g.base);
    if (!count || !plugin_manager_paths_build({g.base, count}, &g.paths) || !plugin_manager_paths_ensure(g.paths))
        return false;
    // The fixture DLL next to this executable.
    if (!read_dll(&g.dll, &g.dll_bytes, 2)) return false;
    g.bad = g.dll + g.dll_bytes + 64;
    copy_bytes(g.bad, g.dll, g.dll_bytes + 64);
    g.bad[g.dll_bytes + 3] = 0x5A;
    for (u32 i = 0; i < sizeof(g.blob); ++i) g.blob[i] = 'A';
    u8 scalar[32]{};
    scalar[31] = 1; // test-only private key; its public point is the generator
    const P256Key* keys[]{&g.trusted};
    g.permissions_menus = 1ull << native_plugin_permission_bit(utf8_literal("menus"));
    return kdf_sha256_initialize(&g.workspace).ok() && p256_import_private(&g.signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_import_public(&g.trusted, P256Kind::ecdsa, {kGenerator, 65}).ok() && digest(g.dll, g.dll_bytes, g.sha[0]) &&
        digest(g.dll, g.dll_bytes + 16, g.sha[1]) && digest(g.dll, g.dll_bytes + 32, g.sha[2]) &&
        digest(g.blob, sizeof(g.blob), g.blob_sha) && build_index() &&
        plugin_catalog_source_official(utf8_literal("example/plugins"), {keys, 1}, &g.source);
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_manager_tests_entry() noexcept {
    using namespace maicord;
    const bool ready = setup();
    check(ready, "plugin manager: temporary root, the fixture DLL, its versions and the signed index");
    if (ready) {
        test_plan();
        test_lifecycle();
        test_download_failures();
        test_recovery();
    }
    check(teardown(g.paths, g.base, g.root, false), "plugin manager: every test file and directory was removed");
    p256_release(&g.trusted); p256_release(&g.signer);
    kdf_sha256_release(&g.workspace);
    if (g.dll) VirtualFree(g.dll, 0, MEM_RELEASE);
    ExitProcess(g_failures ? 1 : 0);
}
