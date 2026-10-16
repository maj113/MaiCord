#include "application_plugin_browser.hpp"
#include "maicord/app/update_https.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/random.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
using Kind = PluginBrowserCommandKind;
using Op = PluginJournalOperation;
constexpr u64 kBrowseRefreshSeconds = 3600;

// Per check or command; freed afterwards so the idle worker stays small.
struct Operation {
    PluginManagerPaths paths;
    KdfSha256Workspace workspace;
    PluginManagerWork manager;
    PluginProviders providers;
    PluginCatalogKeys keys;
    PluginCatalogSources sources;
    PluginEvaluation evaluation;
    PluginIndexWork* official; // the official index while custom providers are applied
    PluginIndexWork* scratch;
    UpdateHttpsTransport* https;
    UpdateTransport transport;
    bool workspace_ready;
};
struct Worker {
    PluginBrowserConfig config;
    PluginBrowserWorkerOptions options;
    wchar_t base[kPluginManagerBaseMaximumCharacters + 1];
    u32 base_count;
    SRWLOCK lock;
    HANDLE thread, wake, stop, notify;
    // Guarded by lock.
    PluginBrowserWorkerView view;
    PluginCatalogSnapshot* snapshot;
    PluginBrowserCommand command;
    u64 command_serial, next_serial, check_provider;
    bool command_pending, command_running, check_requested, manual_check, browse_opened, want_snapshot, snapshot_dirty;
    UpdateHttpsTransport* volatile active_https;
    // Worker thread only.
    u32 failures;
    u64 next_check;
    bool catalog_used;
    PluginManagerPrepared prepared; // the last prepare, settled by finish
    PluginIndexWork* probe_work;    // a probe's verified document until add, trust or cancel
    PluginProviderAddress probe_address;
    PluginProviderProbe probe;
};
static_assert(sizeof(Worker) <= 16 * 1024, "the idle catalog worker stays under 16 KiB");
Worker* g_worker;

u64 now_unix(const Worker* w) noexcept { return w->options.clock ? w->options.clock() : clock_unix_milliseconds() / 1000; }
u32 random32() noexcept { u32 value = 0; secure_random_u32(&value); return value; }
template <typename F> void publish(Worker* w, F&& change) noexcept {
    AcquireSRWLockExclusive(&w->lock);
    change(w->view);
    ++w->view.revision;
    ReleaseSRWLockExclusive(&w->lock);
    SetEvent(w->notify);
}
template <typename T> T* allocate() noexcept {
    return static_cast<T*>(VirtualAlloc(nullptr, sizeof(T), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
void release(void* memory) noexcept { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }

void close_operation(Worker* w, Operation* op) noexcept {
    if (!op) return;
    plugin_catalog_keys_release(&op->keys);
    if (op->workspace_ready) kdf_sha256_release(&op->workspace);
    if (op->https) {
        w->active_https = nullptr;
        update_https_release(op->https);
        release(op->https);
    }
    release(op->official);
    release(op->scratch);
    release(op);
}
Operation* open_operation(Worker* w, bool network) noexcept {
    auto* op = allocate<Operation>();
    if (!op) return nullptr;
    if (!plugin_manager_paths_build({w->base, w->base_count}, &op->paths) || !kdf_sha256_initialize(&op->workspace).ok()) {
        close_operation(w, op);
        return nullptr;
    }
    op->workspace_ready = true;
    const auto loaded = plugin_providers_load({op->paths.providers, op->paths.providers_count}, &op->providers);
    if (!loaded.ok()) op->providers = {}; // a missing or unreadable providers.bin leaves the official provider only
    const u8* keys[2]{w->config.keys[0], w->config.keys[1]};
    plugin_catalog_sources_build({w->config.repository, w->config.repository_bytes}, {keys, w->config.key_count}, op->providers,
        &op->keys, &op->sources);
    if (network) {
        if (w->options.transport.get) op->transport = w->options.transport;
        else {
            op->https = allocate<UpdateHttpsTransport>();
            if (!op->https || !update_https_initialize(op->https)) { close_operation(w, op); return nullptr; }
            w->active_https = op->https;
            op->transport = update_https_transport(op->https);
        }
    }
    return op;
}
bool ensure_work(PluginIndexWork*& work) noexcept { return work || (work = allocate<PluginIndexWork>()) != nullptr; }
const PluginCatalogSource* find_source(const Operation& op, u64 provider) noexcept {
    for (u32 i = 0; i < op.sources.count; ++i) if (op.sources.sources[i].provider == provider) return &op.sources.sources[i];
    return nullptr;
}
void forget_key_change(PluginBrowserWorkerView& v, u64 provider) noexcept {
    for (u32 i = 0; i < v.key_changed_count; ++i)
        if (v.key_changed[i] == provider) { v.key_changed[i] = v.key_changed[--v.key_changed_count]; v.key_changed[v.key_changed_count] = 0; return; }
}

void build_snapshot(Worker* w) noexcept {
    Operation* op = open_operation(w, false);
    PluginCatalogSnapshot* snapshot = nullptr;
    if (op && ensure_work(op->official) && ensure_work(op->scratch)) {
        PluginCatalogSnapshotInput input{};
        input.sources = &op->sources;
        input.providers = &op->providers;
        input.configured = w->config.key_count != 0;
        input.key_changed_count = w->view.key_changed_count;
        copy_bytes(input.key_changed, w->view.key_changed, sizeof(input.key_changed));
        snapshot = plugin_catalog_snapshot_build(op->paths, input, plugin_index_current_host(), op->workspace, op->official, op->scratch,
            &op->manager, now_unix(w));
    }
    close_operation(w, op);
    if (!snapshot) return;
    AcquireSRWLockExclusive(&w->lock);
    plugin_catalog_snapshot_release(w->snapshot);
    w->snapshot = snapshot;
    ++w->view.snapshot_serial;
    ++w->view.revision;
    ReleaseSRWLockExclusive(&w->lock);
    SetEvent(w->notify);
}

// Checks the providers due (or asked for), applies each index to installed
// plugins, stages one official automatic update, then publishes.
void run_check(Worker* w, bool manual, u64 only) noexcept {
    publish(w, [](PluginBrowserWorkerView& v) noexcept { v.phase = PluginBrowserPhase::checking; });
    Operation* op = open_operation(w, true);
    const u64 now = now_unix(w);
    if (!op || !ensure_work(op->official)) {
        close_operation(w, op);
        ++w->failures;
        w->next_check = w->catalog_used ? update_next_check(now, w->failures, 0, random32()) : 0;
        publish(w, [](PluginBrowserWorkerView& v) noexcept { v.phase = PluginBrowserPhase::idle; v.check_error = PluginIndexCheckError::storage; });
        return;
    }
    const PluginHost host = plugin_index_current_host();
    bool reached = false;
    u32 failed = 0;
    u64 retry_at = 0;
    PluginIndexCheckError first_error = PluginIndexCheckError::none;
    UpdateFetchError first_fetch = UpdateFetchError::none;
    u64 changed_keys[kPluginProviderMaximum]{};
    u32 changed_count = 0, cleared_count = 0;
    u64 cleared[kPluginProviderMaximum]{};
    const PluginIndex* official = nullptr;
    for (u32 i = 0; i < op->sources.count; ++i) {
        const PluginCatalogSource& source = op->sources.sources[i];
        const bool is_official = source.kind == PluginProviderKind::official;
        const PluginProviderRecord* record = op->sources.records[i];
        PluginIndexWork*& work = is_official ? op->official : op->scratch;
        if (!ensure_work(work)) break;
        const bool scheduled = is_official ? !(op->providers.official_flags & kPluginOfficialChecksOff)
                                           : source.kind == PluginProviderKind::custom_signed && record && (record->flags & kPluginProviderCheckDaily);
        bool have = false, installable = false;
        if ((scheduled || manual) && (!only || only == source.provider)) {
            const auto r = plugin_index_check(op->paths, source, op->transport, op->workspace, work, now, w->failures, random32());
            if (r.error == PluginIndexCheckError::none) {
                reached = have = installable = true;
                if (!is_official && cleared_count < kPluginProviderMaximum) cleared[cleared_count++] = source.provider;
            } else {
                ++failed;
                if (first_error == PluginIndexCheckError::none) { first_error = r.error; first_fetch = r.fetch_error; }
                retry_at = maximum(retry_at, r.retry_at_unix);
                if (r.error == PluginIndexCheckError::key_changed && changed_count < kPluginProviderMaximum) changed_keys[changed_count++] = source.provider;
            }
        }
        if (!have) {
            const auto cached = plugin_index_load_cache(op->paths, source, op->workspace, work, now);
            have = cached.error == PluginIndexCheckError::none;
            installable = cached.installable;
        }
        PluginCatalogPass pass{};
        pass.provider = source.provider;
        pass.kind = source.kind;
        pass.index = have ? &work->index : nullptr;
        pass.document = have ? plugin_index_work_document(*work) : Utf8View{};
        pass.installable = installable;
        pass.auto_update = is_official && !(op->providers.official_flags & kPluginOfficialAutoUpdateOff);
        plugin_manager_evaluate(op->paths, pass, official, host, &op->manager, &op->evaluation);
        if (is_official && have) official = &op->official->index;
    }
    // "Download now, apply at next start": one staged operation at a time.
    u32 staged = 0;
    for (u32 i = 0; official && i < op->evaluation.update_count && ensure_work(op->scratch); ++i) {
        const PluginAutomaticUpdate& update = op->evaluation.updates[i];
        const PluginManagerRequest request{Op::update, update.owner, update.version_code, update.approved_permissions, false};
        PluginManagerPrepared prepared{};
        const auto result = plugin_manager_prepare(op->paths, op->sources.sources[0], request, op->transport, op->workspace, op->scratch, nullptr,
            &op->manager, now, &prepared);
        if (result.ok()) { staged = 1; break; }
        if (result.error == PluginManagerError::busy) break;
    }
    if (reached) w->failures = 0;
    else if (failed) ++w->failures;
    w->next_check = w->catalog_used ? update_next_check(now, w->failures, retry_at, random32()) : 0;
    const PluginEvaluation& ev = op->evaluation;
    struct Facts {
        const PluginEvaluation& ev;
        bool reached;
        u64 now, retry_at;
        u32 staged, changed_count, cleared_count;
        PluginIndexCheckError error;
        UpdateFetchError fetch;
        const u64* changed;
        const u64* cleared;
    } facts{ev, reached, now, retry_at, staged, changed_count, cleared_count, first_error, first_fetch, changed_keys, cleared};
    publish(w, [&facts](PluginBrowserWorkerView& v) noexcept {
        v.phase = PluginBrowserPhase::idle;
        if (facts.reached) v.checked_unix = facts.now;
        v.check_error = facts.error;
        v.fetch_error = facts.fetch;
        v.retry_at_unix = facts.retry_at;
        v.offers = facts.ev.offers;
        v.reviews = facts.ev.reviews;
        v.staged = facts.staged;
        for (u32 i = 0; i < facts.cleared_count; ++i) forget_key_change(v, facts.cleared[i]);
        for (u32 i = 0; i < facts.changed_count; ++i) {
            bool known = false;
            for (u32 k = 0; k < v.key_changed_count; ++k) known = known || v.key_changed[k] == facts.changed[i];
            if (!known && v.key_changed_count < kPluginProviderMaximum) v.key_changed[v.key_changed_count++] = facts.changed[i];
        }
        if (facts.ev.revoked_count) {
            v.revoked_count = facts.ev.revoked_count;
            copy_bytes(v.revoked, facts.ev.revoked, facts.ev.revoked_count * static_cast<u32>(sizeof(PluginRevokedPlugin)));
            ++v.revoked_serial;
        }
    });
    UpdateLog("plugins", "check_done").field("manual", manual).field("reached", reached).field("failed", failed)
        .field("offers", ev.offers).field("staged", staged).field("next", w->next_check).write();
    close_operation(w, op);
    AcquireSRWLockShared(&w->lock);
    const bool want = w->want_snapshot;
    ReleaseSRWLockShared(&w->lock);
    if (want) build_snapshot(w);
}

// A staged automatic update for another plugin would block the user's
// operation until the next start: discard it (the next check stages it again).
void settle_unrelated(Worker* w, Operation* op, const PluginBrowserCommand& cmd, bool* reuse) noexcept {
    PluginJournal journal{};
    *reuse = false;
    if (!plugin_journal_load({op->paths.journal, op->paths.journal_count}, &journal).ok() || journal.operation == Op::none) return;
    if (journal.operation == Op::update && journal.phase == PluginJournalPhase::staged && cmd.kind == Kind::operation &&
        cmd.operation == Op::update && journal.owner == cmd.owner && journal.to_code == cmd.version_code && journal.provider == cmd.provider) {
        w->prepared = {};
        w->prepared.journal = journal;
        w->prepared.options.retain_previous = true;
        w->prepared.manifest_count = plugin_manager_id_path(op->paths.staging, op->paths.staging_count, journal.owner,
            kNativePluginManifestSuffix, w->prepared.manifest);
        *reuse = w->prepared.manifest_count && GetFileAttributesW(w->prepared.manifest) != INVALID_FILE_ATTRIBUTES;
        if (*reuse) return;
    }
    if (journal.phase == PluginJournalPhase::staged && journal.operation == Op::update && journal.provider == kPluginProviderOfficial) {
        PluginManagerSettled settled{};
        plugin_manager_recover(op->paths, &op->manager, &settled);
    }
}
void free_probe(Worker* w) noexcept {
    release(w->probe_work);
    w->probe_work = nullptr;
    w->probe = {};
}
void run_command(Worker* w, const PluginBrowserCommand& cmd, u64 serial) noexcept {
    publish(w, [](PluginBrowserWorkerView& v) noexcept { v.phase = PluginBrowserPhase::working; });
    const bool network = (cmd.kind == Kind::operation && cmd.operation != Op::rollback) || cmd.kind == Kind::probe || cmd.kind == Kind::probe_key;
    Operation* op = open_operation(w, network);
    const u64 now = now_unix(w);
    PluginManagerResult manager{};
    PluginManagerSettled settled{};
    AcceptanceRecordResult record{};
    PluginProviderAddressError address_error = PluginProviderAddressError::none;
    PluginProviderAddResult add{};
    bool changed = false;
    if (!op) {
        manager.error = PluginManagerError::storage;
        record.error = AcceptanceRecordError::open;
        address_error = PluginProviderAddressError::invalid_input;
        add.error = PluginProviderAddError::invalid_input;
    } else switch (cmd.kind) {
    case Kind::operation: {
        const PluginCatalogSource* source = find_source(*op, cmd.provider);
        bool reuse = false;
        settle_unrelated(w, op, cmd, &reuse);
        if (reuse) break;
        if (!source) { manager.error = PluginManagerError::provider; break; }
        const PluginIndex* official = nullptr;
        if (source->kind != PluginProviderKind::official && op->sources.official && ensure_work(op->official) &&
            plugin_index_load_cache(op->paths, op->sources.sources[0], op->workspace, op->official, now).error == PluginIndexCheckError::none)
            official = &op->official->index;
        if (!ensure_work(op->scratch)) { manager.error = PluginManagerError::storage; break; }
        const PluginManagerRequest request{cmd.operation, cmd.owner, cmd.version_code, cmd.approved, cmd.enable_after};
        PluginManagerPrepared prepared{};
        manager = plugin_manager_prepare(op->paths, *source, request, op->transport, op->workspace, op->scratch, official, &op->manager, now,
            &prepared);
        if (manager.ok()) w->prepared = prepared;
        break;
    }
    case Kind::uninstall: {
        bool reuse = false;
        settle_unrelated(w, op, cmd, &reuse);
        PluginManagerPrepared prepared{};
        manager = plugin_manager_uninstall_prepare(op->paths, cmd.owner, &op->manager, &prepared);
        if (manager.ok()) w->prepared = prepared;
        break;
    }
    case Kind::finish:
        manager = plugin_manager_finish(op->paths, w->prepared, cmd.delete_data, &op->manager, &settled);
        if (manager.ok()) w->prepared = {};
        changed = true;
        break;
    case Kind::probe:
    case Kind::probe_key: {
        free_probe(w);
        PluginProviderAddress address{};
        char text[kPluginProviderAddressMaximumBytes];
        Utf8View typed{cmd.text, minimum(cmd.text_bytes, kPluginProviderAddressMaximumBytes)};
        if (cmd.kind == Kind::probe_key) {
            const PluginProviderRecord* found = plugin_providers_find(op->providers, cmd.provider);
            u32 n = 0;
            const auto put = [&](const char* data, u32 bytes) noexcept {
                if (bytes <= sizeof(text) - n) { copy_bytes(text + n, data, bytes); n += bytes; }
            };
            if (found) { put("https://", 8); put(found->host, found->host_bytes); put(found->path, found->path_bytes); }
            typed = {text, n};
        }
        address_error = plugin_provider_address_parse(typed, op->workspace, &address);
        w->probe_address = address;
        if (address_error != PluginProviderAddressError::none) break;
        if (!ensure_work(w->probe_work)) { address_error = PluginProviderAddressError::invalid_input; break; }
        w->probe = plugin_provider_probe(address, op->transport, op->workspace, w->probe_work, cmd.allow_unsigned && cmd.kind == Kind::probe, now);
        if (w->probe.error != PluginProviderProbeError::none) {
            const PluginProviderProbe failed = w->probe;
            free_probe(w);
            w->probe = failed;
        }
        break;
    }
    case Kind::add_provider:
    case Kind::trust_key:
        if (!w->probe_work || w->probe.error != PluginProviderProbeError::none) { add.error = PluginProviderAddError::invalid_input; break; }
        add = cmd.kind == Kind::add_provider
            ? plugin_index_add_provider(op->paths, &op->providers, w->probe_address, w->probe, op->workspace, w->probe_work, now)
            : plugin_manager_replace_provider_key(op->paths, w->probe_address, w->probe, op->workspace, w->probe_work, now);
        if (add.error == PluginProviderAddError::none) {
            w->catalog_used = true;
            if (!w->next_check) w->next_check = update_next_check(now, 0, 0, random32());
            publish(w, [id = w->probe_address.id](PluginBrowserWorkerView& v) noexcept { forget_key_change(v, id); });
        }
        free_probe(w);
        changed = true;
        break;
    case Kind::cancel_probe: free_probe(w); break;
    case Kind::remove_provider:
        record = plugin_manager_remove_provider(op->paths, cmd.provider);
        publish(w, [id = cmd.provider](PluginBrowserWorkerView& v) noexcept { forget_key_change(v, id); });
        changed = true;
        break;
    case Kind::official_flags: record = plugin_manager_set_official_flags(op->paths, cmd.set, cmd.clear); changed = true; break;
    case Kind::provider_flags: record = plugin_manager_set_provider_flags(op->paths, cmd.provider, cmd.set, cmd.clear); changed = true; break;
    case Kind::plugin_flags: record = plugin_manager_set_plugin_flags(op->paths, cmd.owner, cmd.set, cmd.clear, cmd.held_code); changed = true; break;
    case Kind::none: break;
    }
    close_operation(w, op);
    struct Result {
        u64 serial;
        Kind kind;
        const PluginManagerResult& manager;
        const PluginManagerSettled& settled;
        const AcceptanceRecordResult& record;
        PluginProviderAddressError address_error;
        const PluginProviderAddResult& add;
        const Worker* w;
    } r{serial, cmd.kind, manager, settled, record, address_error, add, w};
    publish(w, [w, &r](PluginBrowserWorkerView& v) noexcept {
        w->command_running = false;
        v.phase = PluginBrowserPhase::idle;
        v.command_serial = r.serial;
        v.command = r.kind;
        v.manager = r.manager;
        v.prepared = r.w->prepared;
        v.settled = r.settled;
        v.record = r.record;
        v.address_error = r.address_error;
        v.address = r.w->probe_address;
        v.probe = r.w->probe;
        v.add = r.add;
    });
    UpdateLog("plugins", "command").field("kind", static_cast<u8>(cmd.kind)).field("op", static_cast<u8>(cmd.operation))
        .hex("owner", cmd.owner).field("manager", static_cast<u8>(manager.error)).field("record", static_cast<u8>(record.error))
        .field("address", static_cast<u8>(address_error)).field("probe", static_cast<u8>(w->probe.error))
        .field("add", static_cast<u8>(add.error)).write(manager.ok() && record.ok() ? LogLevel::info : LogLevel::warning);
    AcquireSRWLockShared(&w->lock);
    const bool want = w->want_snapshot;
    ReleaseSRWLockShared(&w->lock);
    if (want && changed) build_snapshot(w);
}

// The privacy rule: no scheduled contact unless a catalog plugin is installed.
bool catalog_installed(Worker* w) noexcept {
    auto* state = allocate<PluginManagerState>();
    auto* paths = allocate<PluginManagerPaths>();
    bool installed = false;
    if (state && paths && plugin_manager_paths_build({w->base, w->base_count}, paths) &&
        plugin_manager_state_load({paths->state, paths->state_count}, state).ok())
        for (u32 i = 0; i < state->count; ++i) installed = installed || state->entries[i].provider != kPluginProviderFile;
    release(state);
    release(paths);
    return installed;
}
DWORD WINAPI worker_main(void* parameter) noexcept {
    auto* w = static_cast<Worker*>(parameter);
    w->catalog_used = catalog_installed(w);
    const u64 jitter = w->options.first_check_jitter ? random32() % w->options.first_check_jitter : 0;
    w->next_check = w->catalog_used ? now_unix(w) + w->options.first_check_delay + jitter : 0;
    for (;;) {
        const u64 now = now_unix(w);
        const DWORD timeout = !w->next_check ? INFINITE : w->next_check <= now ? 0 : static_cast<DWORD>(minimum<u64>(w->next_check - now, 3600) * 1000);
        HANDLE handles[]{w->stop, w->wake};
        const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, timeout);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
        AcquireSRWLockExclusive(&w->lock);
        const bool command = w->command_pending;
        const PluginBrowserCommand cmd = w->command;
        const u64 serial = w->command_serial;
        w->command_pending = false;
        w->command_running = command;
        const bool check = w->check_requested, manual = w->manual_check, browse = w->browse_opened, snapshot = w->snapshot_dirty && w->want_snapshot;
        const u64 provider = w->check_provider;
        w->check_requested = w->manual_check = w->browse_opened = w->snapshot_dirty = false;
        w->check_provider = 0;
        const u64 checked = w->view.checked_unix;
        ReleaseSRWLockExclusive(&w->lock);
        if (command) run_command(w, cmd, serial);
        const u64 at = now_unix(w);
        bool due = check || (w->next_check && at >= w->next_check);
        if (browse || manual) w->catalog_used = true;
        if (browse && (!checked || at - checked >= kBrowseRefreshSeconds)) due = true;
        if (due) run_check(w, manual, manual ? provider : 0);
        else if (snapshot) build_snapshot(w);
    }
    return 0;
}
} // namespace

bool plugin_browser_worker_start(const PluginBrowserConfig& config, const PluginBrowserWorkerOptions& options) noexcept {
    if (g_worker) return true;
    auto* w = allocate<Worker>();
    if (!w) return false;
    w->config = config;
    w->options = options;
    InitializeSRWLock(&w->lock);
    if (options.base && options.base_count && options.base_count <= kPluginManagerBaseMaximumCharacters) {
        copy_bytes(w->base, options.base, options.base_count * static_cast<u32>(sizeof(wchar_t)));
        w->base_count = options.base_count;
    } else {
        const DataPathResult built = data_path(w->base, kPluginManagerBaseMaximumCharacters + 1, L"");
        w->base_count = built.ok() ? built.length : 0;
    }
    w->view.located = w->base_count != 0;
    w->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w->notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (w->view.located && w->wake && w->stop && w->notify)
        w->thread = CreateThread(nullptr, 256 * 1024, worker_main, w, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    w->view.started = w->thread != nullptr;
    UpdateLog("plugins", "worker").field("configured", config.key_count != 0).field("keys", config.key_count)
        .field("located", w->view.located).field("started", w->view.started).write();
    if (!w->thread) {
        const HANDLE events[]{w->wake, w->stop, w->notify};
        for (HANDLE h : events) if (h) CloseHandle(h);
        release(w);
        return false;
    }
    g_worker = w;
    return true;
}
void plugin_browser_worker_stop() noexcept {
    Worker* w = g_worker;
    if (!w) return;
    g_worker = nullptr;
    SetEvent(w->stop);
    if (UpdateHttpsTransport* https = w->active_https) update_https_cancel(https);
    // A journaled step in flight is recovered at the next start; never free
    // what the worker may still touch.
    if (WaitForSingleObject(w->thread, 3000) != WAIT_OBJECT_0) return;
    CloseHandle(w->thread);
    plugin_catalog_snapshot_release(w->snapshot);
    release(w->probe_work);
    const HANDLE events[]{w->wake, w->stop, w->notify};
    for (HANDLE h : events) if (h) CloseHandle(h);
    release(w);
}
bool plugin_browser_worker_running() noexcept { return g_worker != nullptr; }
HANDLE plugin_browser_worker_notification() noexcept { return g_worker ? g_worker->notify : nullptr; }
bool plugin_browser_worker_view(PluginBrowserWorkerView* view, u64 seen) noexcept {
    Worker* w = g_worker;
    if (!w || !view) return false;
    AcquireSRWLockShared(&w->lock);
    const bool changed = w->view.revision != seen;
    if (changed) *view = w->view;
    ReleaseSRWLockShared(&w->lock);
    return changed;
}
bool plugin_browser_worker_check(bool manual, u64 provider) noexcept {
    Worker* w = g_worker;
    if (!w) return false;
    AcquireSRWLockExclusive(&w->lock);
    w->check_requested = true;
    w->manual_check = w->manual_check || manual;
    w->check_provider = provider;
    ReleaseSRWLockExclusive(&w->lock);
    return SetEvent(w->wake) != FALSE;
}
void plugin_browser_worker_browse() noexcept {
    Worker* w = g_worker;
    if (!w) return;
    AcquireSRWLockExclusive(&w->lock);
    w->browse_opened = true;
    ReleaseSRWLockExclusive(&w->lock);
    SetEvent(w->wake);
}
void plugin_browser_worker_want_snapshot(bool want) noexcept {
    Worker* w = g_worker;
    if (!w) return;
    AcquireSRWLockExclusive(&w->lock);
    const bool request = want && !w->want_snapshot;
    w->want_snapshot = want;
    if (request) w->snapshot_dirty = true;
    PluginCatalogSnapshot* stale = want ? nullptr : w->snapshot;
    if (!want) w->snapshot = nullptr;
    ReleaseSRWLockExclusive(&w->lock);
    plugin_catalog_snapshot_release(stale);
    if (request) SetEvent(w->wake);
}
PluginCatalogSnapshot* plugin_browser_worker_take_snapshot() noexcept {
    Worker* w = g_worker;
    if (!w) return nullptr;
    AcquireSRWLockExclusive(&w->lock);
    PluginCatalogSnapshot* snapshot = w->snapshot;
    w->snapshot = nullptr;
    ReleaseSRWLockExclusive(&w->lock);
    return snapshot;
}
bool plugin_browser_worker_command(const PluginBrowserCommand& command, u64* serial) noexcept {
    Worker* w = g_worker;
    if (!w || command.kind == Kind::none) return false;
    AcquireSRWLockExclusive(&w->lock);
    const bool accepted = !w->command_pending && !w->command_running;
    if (accepted) {
        w->command = command;
        w->command_serial = ++w->next_serial;
        w->command_pending = true;
        if (serial) *serial = w->command_serial;
    }
    ReleaseSRWLockExclusive(&w->lock);
    if (accepted) SetEvent(w->wake);
    return accepted;
}
bool plugin_browser_worker_busy() noexcept {
    Worker* w = g_worker;
    if (!w) return false;
    AcquireSRWLockShared(&w->lock);
    const bool busy = w->command_pending || w->command_running || w->view.phase != PluginBrowserPhase::idle;
    ReleaseSRWLockShared(&w->lock);
    return busy;
}

} // namespace maicord::app_detail
