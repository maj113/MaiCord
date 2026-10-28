#include "plugin_manager_common.hpp"
#include "maicord/app/plugin_manager_runtime.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using namespace plugin_manager_detail;
using ME = PluginManagerError;
using Op = PluginJournalOperation;

AcceptanceRecordResult load_providers(const PluginManagerPaths& paths, PluginProviders* providers) noexcept {
    reset_value(providers);
    const auto loaded = plugin_providers_load({paths.providers, paths.providers_count}, providers);
    return loaded.error == AcceptanceRecordError::missing ? AcceptanceRecordResult{} : loaded;
}
// The local inventory in work. false only when the directory exists and
// cannot be read; a missing directory is an empty inventory.
bool scan_inventory(const PluginManagerPaths& paths, PluginManagerWork* work) noexcept {
    clear_bytes(&work->catalog, sizeof(work->catalog));
    auto scanned = native_plugin_catalog_initialize(&work->catalog, {paths.plugins, paths.plugins_count},
        {work->entries, kNativePluginCatalogMaximumEntries});
    if (scanned.ok()) scanned = native_plugin_catalog_refresh(&work->catalog);
    const bool missing = scanned.win32_error == ERROR_FILE_NOT_FOUND || scanned.win32_error == ERROR_PATH_NOT_FOUND;
    const bool released = native_plugin_catalog_release(&work->catalog).ok();
    if (!scanned.ok()) work->catalog.count = 0;
    return (scanned.ok() || missing) && released;
}
const NativePluginManifest* inventory_manifest(const PluginManagerWork& work, u64 owner) noexcept {
    for (u32 i = 0; i < work.catalog.count && i < kNativePluginCatalogMaximumEntries; ++i) {
        const auto& entry = work.entries[i];
        if (entry.status == NativePluginCatalogError::none && entry.manifest.owner_id == owner) return &entry.manifest;
    }
    return nullptr;
}
u32 index_plugin(const PluginIndex& index, u64 owner) noexcept {
    for (u32 i = 0; i < index.plugin_count; ++i) if (index.plugins[i].owner_id == owner) return i;
    return kPluginIndexNoVersion;
}
// The listed version of plugin whose host-architecture file is sha256.
u32 index_build(const PluginIndex& index, u32 plugin, const u8* sha256, PluginIndexArch arch) noexcept {
    const auto& p = index.plugins[plugin];
    for (u32 i = 0; i < p.version_count; ++i) {
        const PluginIndexFile* file = plugin_index_file(index.versions[p.first_version + i], arch);
        if (file && bytes_equal(file->sha256, sha256, 32)) return i;
    }
    return kPluginIndexNoVersion;
}
u32 index_code(const PluginIndex& index, u32 plugin, u64 code) noexcept {
    const auto& p = index.plugins[plugin];
    for (u32 i = 0; i < p.version_count; ++i) if (index.versions[p.first_version + i].version_code == code) return i;
    return kPluginIndexNoVersion;
}
void remember_revoked(PluginEvaluation* ev, u64 owner, const NativePluginManifest* manifest) noexcept {
    for (u32 i = 0; i < ev->revoked_count; ++i) if (ev->revoked[i].owner == owner) return;
    if (ev->revoked_count >= kPluginManagerStateMaximumEntries) return;
    auto& r = ev->revoked[ev->revoked_count++];
    reset_value(&r);
    r.owner = owner;
    if (manifest) {
        r.name_bytes = minimum(manifest->name_bytes, kNativePluginNameCapacity);
        copy_bytes(r.name, manifest->name, r.name_bytes);
    }
}
} // namespace

// ---- sources ----------------------------------------------------------------------------------
void plugin_catalog_keys_release(PluginCatalogKeys* keys) noexcept {
    if (!keys) return;
    for (auto& key : keys->official) p256_release(&key);
    for (auto& key : keys->custom) p256_release(&key);
}

void plugin_catalog_sources_build(Utf8View repository, ConstSlice<const u8*> official_keys, const PluginProviders& providers,
    PluginCatalogKeys* keys, PluginCatalogSources* output) noexcept {
    if (!keys || !output) return;
    reset_value(output);
    if (repository.bytes && official_keys.data && official_keys.count) {
        const P256Key* imported[2]{};
        u32 count = 0;
        for (u32 i = 0; i < official_keys.count && i < 2; ++i)
            if (official_keys[i] && p256_import_public(&keys->official[count], P256Kind::ecdsa, {official_keys[i], 65}).ok())
                imported[count] = &keys->official[count], ++count;
        if (count && plugin_catalog_source_official(repository, {imported, count}, &output->sources[0])) {
            output->official = true;
            output->count = 1;
        }
    }
    for (u32 i = 0; i < providers.count && i < kPluginProviderMaximum; ++i) {
        const PluginProviderRecord& record = providers.entries[i];
        if (!(record.flags & kPluginProviderEnabled)) continue;
        const P256Key* pinned = nullptr;
        if (record.flags & kPluginProviderSigned) {
            if (!p256_import_public(&keys->custom[i], P256Kind::ecdsa, {record.key, 65}).ok()) continue;
            pinned = &keys->custom[i];
        }
        if (plugin_catalog_source_custom(record, pinned, &output->sources[output->count])) output->records[output->count++] = &record;
    }
}

// ---- startup ----------------------------------------------------------------------------------
u8 plugin_startup_gate_reason(const PluginStartupGate& gate, u64 owner) noexcept {
    for (u32 i = 0; i < gate.count && i < kPluginManagerStateMaximumEntries; ++i) if (gate.owners[i] == owner) return gate.reasons[i];
    return 0;
}
u8 plugin_manager_entry_gate(const PluginManagerEntry& entry, const PluginHost& host) noexcept {
    u8 reasons = 0;
    if (entry.flags & kPluginStateRevokedMalicious) reasons |= kPluginGateRevoked;
    // Only the stored bound: the flag was computed for the host of the last check.
    if (entry.max_host_abi && host.abi > entry.max_host_abi) reasons |= kPluginGateIncompatible;
    return reasons;
}

PluginStartupResult plugin_manager_startup(const PluginManagerPaths& paths, const PluginHost& host, PluginManagerWork* work,
    PluginStartupGate* gate) noexcept {
    PluginStartupResult r{};
    if (!work || !gate || !paths.manager_count) { r.manager = failure(ME::invalid_input); return r; }
    reset_value(gate);
    if (GetFileAttributesW(paths.manager) == INVALID_FILE_ATTRIBUTES) return r; // the catalog was never used
    PluginJournal journal{};
    const auto journaled = plugin_journal_load({paths.journal, paths.journal_count}, &journal);
    if (!journaled.ok() && journaled.error != AcceptanceRecordError::missing) { r.manager = record_failure(journaled); return r; }
    if (journaled.ok() && journal.operation == Op::update && journal.phase == PluginJournalPhase::staged &&
        journal.provider == kPluginProviderOfficial) {
        PluginProviders providers{};
        const bool switches = load_providers(paths, &providers).ok() && !(providers.official_flags & kPluginOfficialAutoUpdateOff);
        const bool states = load_state(paths, &work->state).ok();
        const PluginManagerEntry* entry = states ? state_entry(work->state, journal.owner) : nullptr;
        PluginManagerPrepared prepared{};
        prepared.journal = journal;
        prepared.options.retain_previous = true;
        prepared.manifest_count = plugin_manager_id_path(paths.staging, paths.staging_count, journal.owner, kNativePluginManifestSuffix,
            prepared.manifest);
        if (switches && entry && !(entry->flags & kPluginStateAutoUpdateOff) && prepared.manifest_count &&
            GetFileAttributesW(prepared.manifest) != INVALID_FILE_ATTRIBUTES) {
            NativePluginBundleInstall installer{};
            const auto started = native_plugin_bundle_install_start(&installer, {prepared.manifest, prepared.manifest_count},
                {paths.plugins, paths.plugins_count}, prepared.options);
            const auto ran = started == NativePluginBundleInstallError::none ? plugin_manager_run_installer(&installer, kInstallerTimeoutMs)
                                                                              : NativePluginBundleInstallResult{};
            if (ran.committed) {
                r.manager = plugin_manager_finish(paths, prepared, false, work, &r.settled);
                r.rolled_forward = r.manager.ok();
            }
            UpdateLog("plugins", "roll_forward").hex("owner", journal.owner).field("to", journal.to_code).field("committed", ran.committed)
                .field("install", static_cast<u8>(started != NativePluginBundleInstallError::none ? started : ran.error))
                .write(r.rolled_forward ? LogLevel::info : LogLevel::warning);
        }
    }
    if (!r.rolled_forward && journaled.ok() && journal.operation != Op::none) r.manager = plugin_manager_recover(paths, work, &r.settled);
    const auto states = load_state(paths, &work->state);
    if (!states.ok()) { if (r.manager.ok()) r.manager = record_failure(states); return r; }
    for (u32 i = 0; i < work->state.count; ++i) {
        const PluginManagerEntry& entry = work->state.entries[i];
        r.catalog_installed = r.catalog_installed || entry.provider != kPluginProviderFile;
        const u8 reasons = plugin_manager_entry_gate(entry, host);
        if (reasons) { gate->owners[gate->count] = entry.owner; gate->reasons[gate->count++] = reasons; }
    }
    UpdateLog("plugins", "startup").field("action", static_cast<u8>(r.settled.action)).field("rolled", r.rolled_forward)
        .field("gated", gate->count).field("error", static_cast<u8>(r.manager.error)).write(r.manager.ok() ? LogLevel::info : LogLevel::warning);
    return r;
}

// ---- evaluation -------------------------------------------------------------------------------
PluginManagerResult plugin_manager_evaluate(const PluginManagerPaths& paths, const PluginCatalogPass& pass,
    const PluginIndex* official, const PluginHost& host, PluginManagerWork* work, PluginEvaluation* ev) noexcept {
    if (!work || !ev || !paths.manager_count || (pass.index && !pass.document.data)) return failure(ME::invalid_input);
    const bool official_pass = pass.kind == PluginProviderKind::official;
    const PluginIndex* off = official_pass ? pass.index : official;
    const bool inventory = scan_inventory(paths, work);
    const auto states = load_state(paths, &work->state);
    if (!states.ok()) return record_failure(states);
    const auto trusted = load_trust(paths, &work->trust);
    if (!trusted.ok()) return failure(ME::trust);
    PluginJournal journal{};
    const auto journaled = plugin_journal_load({paths.journal, paths.journal_count}, &journal);
    const bool settled = journaled.error == AcceptanceRecordError::missing || (journaled.ok() && journal.operation == Op::none);
    PluginManagerState& state = work->state;
    bool state_changed = false, trust_changed = false;
    // Entries whose manifest was deleted by hand.
    if (official_pass && inventory && settled) {
        for (u32 i = 0; i < state.count;) {
            if (inventory_manifest(*work, state.entries[i].owner)) { ++i; continue; }
            for (u32 j = i + 1; j < state.count; ++j) state.entries[j - 1] = state.entries[j];
            reset_value(&state.entries[--state.count]);
            ++ev->pruned;
            state_changed = true;
        }
    }
    // Adoption of manual installs whose bytes this provider lists.
    for (u32 i = 0; pass.index && inventory && i < work->catalog.count; ++i) {
        const auto& item = work->entries[i];
        if (item.status != NativePluginCatalogError::none) continue;
        const NativePluginManifest& m = item.manifest;
        PluginManagerEntry* entry = state_entry(state, m.owner_id);
        if (entry && entry->provider != kPluginProviderFile) continue;
        if (!official_pass && off && plugin_index_owner_reserved(*off, m.owner_id)) continue;
        const u32 plugin = index_plugin(*pass.index, m.owner_id);
        const u32 version = plugin == kPluginIndexNoVersion ? kPluginIndexNoVersion : index_build(*pass.index, plugin, m.sha256, host.arch);
        if (version == kPluginIndexNoVersion) continue;
        if (!entry) {
            if (state.count >= kPluginManagerStateMaximumEntries) continue;
            entry = &state.entries[state.count++];
            reset_value(entry);
            entry->owner = m.owner_id;
        }
        const PluginIndexVersion& listed = pass.index->versions[pass.index->plugins[plugin].first_version + version];
        entry->provider = pass.provider;
        entry->installed_code = listed.version_code;
        copy_bytes(entry->installed_sha256, m.sha256, 32);
        entry->max_host_abi = listed.max_host_abi;
        entry->admitted_sequence = pass.index->sequence;
        clear_bytes(entry->slug, kPluginManagerSlugCapacity);
        const auto slug = plugin_index_text_decode(pass.document, pass.index->plugins[plugin].slug, {entry->slug, kPluginManagerSlugCapacity});
        if (!slug.ok()) clear_bytes(entry->slug, kPluginManagerSlugCapacity);
        ++ev->adopted;
        state_changed = true;
    }
    for (u32 i = 0; i < state.count; ++i) {
        PluginManagerEntry& e = state.entries[i];
        const NativePluginManifest* manifest = inventory_manifest(*work, e.owner);
        const u8* sha = manifest ? manifest->sha256 : e.installed_sha256;
        const bool bound = e.provider == pass.provider;
        const bool unbound_file = official_pass && e.provider == kPluginProviderFile;
        if (!bound && !(official_pass && off)) continue;
        u32 flags = e.flags;
        if (bound || unbound_file) {
            const PluginRevocationMatch match = plugin_index_revocation(bound ? pass.index : nullptr, off, e.owner, sha);
            const bool malicious = match.blocked || (match.revoked && match.severity == PluginRevocationSeverity::malicious);
            // Cache loss or expiry never lifts a revocation; a newer admitted index may.
            if (malicious) flags |= kPluginStateRevokedMalicious;
            else if (pass.installable) flags &= ~kPluginStateRevokedMalicious;
            if (pass.index) {
                flags &= ~(kPluginStateRevokedBroken | kPluginStateWithdrawn);
                if (match.revoked && match.severity == PluginRevocationSeverity::broken) flags |= kPluginStateRevokedBroken;
                if (match.revoked && match.severity == PluginRevocationSeverity::withdrawn) flags |= kPluginStateWithdrawn;
            }
        } else if (plugin_index_revocation(nullptr, off, e.owner, sha).blocked) {
            flags |= kPluginStateRevokedMalicious; // the blocklist applies whichever provider installed it
        }
        const u32 plugin = bound && pass.index ? index_plugin(*pass.index, e.owner) : kPluginIndexNoVersion;
        if (plugin != kPluginIndexNoVersion) {
            flags &= ~(kPluginStateIncompatibleHost | kPluginStateReviewPending);
            const u32 version = index_code(*pass.index, plugin, e.installed_code);
            if (version != kPluginIndexNoVersion) {
                const PluginIndexVersion& listed = pass.index->versions[pass.index->plugins[plugin].first_version + version];
                if (e.max_host_abi != listed.max_host_abi) { e.max_host_abi = listed.max_host_abi; state_changed = true; }
                if (plugin_index_compatibility(listed, host) != PluginCompatibility::compatible)
                    flags |= kPluginStateIncompatibleHost;
            }
            if (pass.installable && !(flags & kPluginStateRevokedMalicious)) {
                const NativePluginPreference* trust = manifest ? native_plugin_preferences_find(work->trust, *manifest) : nullptr;
                PluginInstalled installed{};
                installed.installed_code = e.installed_code;
                installed.held_code = e.held_code;
                installed.trusted = trust != nullptr;
                installed.approved_permissions = trust ? trust->permissions : 0;
                installed.known[0].version_code = e.installed_code;
                copy_bytes(installed.known[0].sha256, e.installed_sha256, 32);
                installed.known[1].version_code = e.previous_code;
                copy_bytes(installed.known[1].sha256, e.previous_sha256, 32);
                const auto selection = plugin_index_select_update(*pass.index, plugin, installed, host, off);
                if (selection.outcome == PluginUpdateOutcome::review) {
                    flags |= kPluginStateReviewPending;
                    ++ev->reviews;
                    ++ev->offers;
                } else if (selection.outcome == PluginUpdateOutcome::available) {
                    ++ev->offers;
                    if (official_pass && pass.auto_update && !(flags & kPluginStateAutoUpdateOff) &&
                        (selection.carries_trust || !installed.trusted) && ev->update_count < kPluginManagerStateMaximumEntries) {
                        auto& update = ev->updates[ev->update_count++];
                        update.owner = e.owner;
                        update.provider = e.provider;
                        update.version_code = pass.index->versions[pass.index->plugins[plugin].first_version + selection.version].version_code;
                        update.approved_permissions = installed.approved_permissions;
                    }
                }
            }
        }
        if ((flags & kPluginStateRevokedMalicious) && !(e.flags & kPluginStateRevokedMalicious)) remember_revoked(ev, e.owner, manifest);
        if ((flags & kPluginStateRevokedMalicious) && trusted_owner(work->trust, e.owner)) {
            native_plugin_preferences_remove_owner(&work->trust, e.owner);
            remember_revoked(ev, e.owner, manifest);
            trust_changed = true;
        }
        if (flags != e.flags) { e.flags = flags; state_changed = true; }
    }
    // Blocked manual installs the state does not list yet.
    for (u32 i = 0; official_pass && off && inventory && i < work->catalog.count; ++i) {
        const auto& item = work->entries[i];
        if (item.status != NativePluginCatalogError::none || state_entry(state, item.manifest.owner_id)) continue;
        if (!plugin_index_revocation(nullptr, off, item.manifest.owner_id, item.manifest.sha256).blocked) continue;
        if (state.count >= kPluginManagerStateMaximumEntries) continue;
        PluginManagerEntry& e = state.entries[state.count++];
        reset_value(&e);
        e.owner = item.manifest.owner_id;
        e.flags = kPluginStateRevokedMalicious;
        copy_bytes(e.installed_sha256, item.manifest.sha256, 32);
        state_changed = true;
        remember_revoked(ev, e.owner, &item.manifest);
        if (native_plugin_preferences_remove_owner(&work->trust, e.owner)) trust_changed = true;
    }
    PluginManagerResult r = failure(ME::none);
    if (state_changed) {
        u32 native = 0;
        const auto saved = plugin_manager_paths_ensure(paths, &native) ? plugin_manager_state_save({paths.state, paths.state_count}, state)
                                                                       : AcceptanceRecordResult{AcceptanceRecordError::open, native};
        if (!saved.ok()) r = record_failure(saved);
        else ev->state_written = true;
    }
    if (trust_changed) {
        const auto saved = save_trust(paths, work->trust);
        if (!saved.ok() && r.ok()) { r = failure(ME::trust); r.native_error = saved.win32_error; }
        else if (saved.ok()) ev->trust_written = true;
    }
    UpdateLog("plugins", "evaluated").field("kind", static_cast<u8>(pass.kind)).field("index", pass.index != nullptr)
        .field("adopted", ev->adopted).field("pruned", ev->pruned).field("offers", ev->offers).field("reviews", ev->reviews)
        .field("revoked", ev->revoked_count).field("updates", ev->update_count).field("error", static_cast<u8>(r.error))
        .write(r.ok() ? LogLevel::info : LogLevel::warning);
    return r;
}

// ---- settings -----------------------------------------------------------------------------------
AcceptanceRecordResult plugin_manager_set_official_flags(const PluginManagerPaths& paths, u32 set, u32 clear) noexcept {
    if (((set | clear) & ~kPluginOfficialKnownFlags) || !paths.manager_count) return {AcceptanceRecordError::invalid_input, 0};
    u32 native = 0;
    if (!plugin_manager_paths_ensure(paths, &native)) return {AcceptanceRecordError::open, native};
    PluginProviders providers{};
    const auto loaded = load_providers(paths, &providers);
    if (!loaded.ok()) return loaded;
    const u32 flags = (providers.official_flags & ~clear) | set;
    if (flags == providers.official_flags && GetFileAttributesW(paths.providers) != INVALID_FILE_ATTRIBUTES) return {};
    providers.official_flags = flags;
    return plugin_providers_save({paths.providers, paths.providers_count}, providers);
}
AcceptanceRecordResult plugin_manager_set_provider_flags(const PluginManagerPaths& paths, u64 provider, u32 set, u32 clear) noexcept {
    if (((set | clear) & ~(kPluginProviderEnabled | kPluginProviderCheckDaily)) || !paths.manager_count)
        return {AcceptanceRecordError::invalid_input, 0};
    PluginProviders providers{};
    const auto loaded = load_providers(paths, &providers);
    if (!loaded.ok()) return loaded;
    for (u32 i = 0; i < providers.count; ++i) {
        auto& record = providers.entries[i];
        if (record.id != provider) continue;
        const u32 flags = (record.flags & ~clear) | set;
        if (flags == record.flags) return {};
        record.flags = flags;
        return plugin_providers_save({paths.providers, paths.providers_count}, providers);
    }
    return {AcceptanceRecordError::missing, 0};
}
AcceptanceRecordResult plugin_manager_set_plugin_flags(const PluginManagerPaths& paths, u64 owner, u32 set, u32 clear, u64 held_code) noexcept {
    if (((set | clear) & ~(kPluginStateAutoUpdateOff | kPluginStateHeld)) || !paths.manager_count || owner <= 1)
        return {AcceptanceRecordError::invalid_input, 0};
    PluginManagerState state{};
    const auto loaded = load_state(paths, &state);
    if (!loaded.ok()) return loaded;
    PluginManagerEntry* entry = state_entry(state, owner);
    if (!entry) return {AcceptanceRecordError::missing, 0};
    entry->flags = (entry->flags & ~clear) | set;
    if (set & kPluginStateHeld) entry->held_code = held_code;
    if (clear & kPluginStateHeld) entry->held_code = 0;
    if (set & kPluginStateHeld) entry->flags &= ~kPluginStateReviewPending;
    return plugin_manager_state_save({paths.state, paths.state_count}, state);
}
namespace {
AcceptanceRecordResult forget_floor(const PluginManagerPaths& paths, u64 provider) noexcept {
    if (GetFileAttributesW(paths.acceptance) == INVALID_FILE_ATTRIBUTES) return {};
    PluginIndexAcceptanceTransaction transaction{};
    auto result = plugin_index_acceptance_begin({paths.acceptance, paths.acceptance_count}, &transaction);
    if (result.ok()) result = plugin_index_acceptance_forget(&transaction, provider);
    const auto released = plugin_index_acceptance_release(&transaction);
    if (result.ok() && !released.ok()) result = released;
    return {result.error, result.native_error};
}
bool remove_record(PluginProviders* providers, u64 provider) noexcept {
    for (u32 i = 0; i < providers->count; ++i) {
        if (providers->entries[i].id != provider) continue;
        for (u32 j = i + 1; j < providers->count; ++j) providers->entries[j - 1] = providers->entries[j];
        reset_value(&providers->entries[--providers->count]);
        return true;
    }
    return false;
}
} // namespace
AcceptanceRecordResult plugin_manager_remove_provider(const PluginManagerPaths& paths, u64 provider) noexcept {
    if (provider <= kPluginProviderOfficial || !paths.manager_count) return {AcceptanceRecordError::invalid_input, 0};
    PluginProviders providers{};
    auto result = load_providers(paths, &providers);
    if (!result.ok()) return result;
    if (!remove_record(&providers, provider)) return {AcceptanceRecordError::missing, 0};
    result = plugin_providers_save({paths.providers, paths.providers_count}, providers);
    if (!result.ok()) return result;
    result = forget_floor(paths, provider);
    wchar_t cache[kPluginManagerPathCapacity];
    if (plugin_manager_id_path(paths.cache, paths.cache_count, provider, L".index", cache)) DeleteFileW(cache);
    UpdateLog("plugins", "provider_removed").field("error", static_cast<u8>(result.error)).write();
    return result;
}
PluginProviderAddResult plugin_manager_replace_provider_key(const PluginManagerPaths& paths, const PluginProviderAddress& address,
    const PluginProviderProbe& probe, KdfSha256Workspace& workspace, PluginIndexWork* work, u64 now) noexcept {
    PluginProviderAddResult r{};
    PluginProviders providers{};
    const auto loaded = load_providers(paths, &providers);
    if (!loaded.ok() || !remove_record(&providers, address.id)) {
        r.error = PluginProviderAddError::providers;
        r.record_error = loaded.ok() ? AcceptanceRecordError::missing : loaded.error;
        return r;
    }
    const auto forgotten = forget_floor(paths, address.id);
    if (!forgotten.ok()) {
        r.error = PluginProviderAddError::acceptance;
        r.record_error = forgotten.error;
        r.native_error = forgotten.native_error;
        return r;
    }
    return plugin_index_add_provider(paths, &providers, address, probe, workspace, work, now);
}

// ---- snapshot ---------------------------------------------------------------------------------
namespace {
constexpr u64 kPage = 4096;
constexpr u64 align_up(u64 value, u64 to) noexcept { return (value + to - 1) / to * to; }
constexpr u64 kHeaderBytes = align_up(sizeof(PluginCatalogSnapshot), 64);
constexpr u64 kEntriesBytes = align_up(u64{kPluginCatalogSnapshotEntries} * sizeof(PluginCatalogEntryView), kPage);
constexpr u64 kVersionsBytes =
    align_up(u64{kPluginCatalogSnapshotEntries} * kPluginIndexMaximumVersions * sizeof(PluginCatalogVersionView), kPage);
constexpr u64 kDocumentsBytes = u64{kPluginIndexAcceptanceSlots} * kPluginIndexMaximumBytes;
constexpr u64 kReserveBytes = align_up(kHeaderBytes, kPage) + kEntriesBytes + kVersionsBytes + kDocumentsBytes;

// Commits [base, base + bytes) of one region on demand.
bool commit(u8* base, u64* committed, u64 bytes) noexcept {
    if (bytes <= *committed) return true;
    const u64 next = align_up(bytes, 64 * 1024);
    if (!VirtualAlloc(base, next, MEM_COMMIT, PAGE_READWRITE)) return false;
    *committed = next;
    return true;
}
struct Builder {
    PluginCatalogSnapshot* s;
    u64 entries_committed, versions_committed, documents_committed;
};
PluginCatalogStatus cache_status(const PluginIndexCacheResult& cached) noexcept {
    if (cached.error == PluginIndexCheckError::cache && cached.record_error == AcceptanceRecordError::missing) return PluginCatalogStatus::none;
    if (cached.error != PluginIndexCheckError::none) return PluginCatalogStatus::failed;
    if (cached.expired) return PluginCatalogStatus::expired;
    return cached.admitted ? PluginCatalogStatus::ok : PluginCatalogStatus::unadmitted;
}
bool same_name(Utf8View a_document, PluginIndexText a, Utf8View b_document, PluginIndexText b) noexcept {
    char left[kNativePluginNameCapacity], right[kNativePluginNameCapacity];
    const auto l = plugin_index_text_decode(a_document, a, {left, sizeof(left)});
    const auto r = plugin_index_text_decode(b_document, b, {right, sizeof(right)});
    return l.ok() && r.ok() && l.bytes == r.bytes && bytes_equal(left, right, l.bytes);
}
bool official_named(const PluginIndex* official, Utf8View official_document, Utf8View document, PluginIndexText name) noexcept {
    for (u32 i = 0; official && i < official->plugin_count; ++i)
        if (same_name(official_document, official->plugins[i].name, document, name)) return true;
    return false;
}
// Appends one provider's verified index: its document, versions and entries.
bool append_index(Builder& b, u32 slot, const PluginIndex& index, Utf8View document, const PluginIndex* official,
    Utf8View official_document, const PluginHost& host, bool installable) noexcept {
    PluginCatalogSnapshot& s = *b.s;
    PluginCatalogProviderView& provider = s.providers[slot];
    if (!commit(reinterpret_cast<u8*>(s.documents), &b.documents_committed, u64{s.documents_bytes} + document.bytes)) return false;
    provider.document_offset = s.documents_bytes;
    provider.document_bytes = document.bytes;
    copy_bytes(s.documents + s.documents_bytes, document.data, document.bytes);
    s.documents_bytes += document.bytes;
    provider.plugin_count = index.plugin_count;
    const Utf8View copied{s.documents + provider.document_offset, document.bytes};
    const bool custom = provider.kind != PluginProviderKind::official;
    for (u32 p = 0; p < index.plugin_count; ++p) {
        const PluginIndexPlugin& plugin = index.plugins[p];
        if (s.entry_count >= kPluginCatalogSnapshotEntries ||
            !commit(reinterpret_cast<u8*>(s.entries), &b.entries_committed, u64{s.entry_count + 1} * sizeof(PluginCatalogEntryView)) ||
            !commit(reinterpret_cast<u8*>(s.versions), &b.versions_committed,
                u64{s.version_count + plugin.version_count} * sizeof(PluginCatalogVersionView)))
            return false;
        PluginCatalogEntryView& e = s.entries[s.entry_count];
        e.owner = plugin.owner_id;
        e.provider = slot;
        e.slug = plugin.slug; e.name = plugin.name; e.summary = plugin.summary; e.publisher = plugin.publisher;
        e.license = plugin.license; e.homepage = plugin.homepage;
        e.author = plugin.author_count ? plugin.authors[0].name : PluginIndexText{};
        e.author_count = plugin.author_count;
        e.categories = plugin.categories;
        e.disclosures = plugin.disclosures;
        e.first_version = s.version_count;
        e.version_count = plugin.version_count;
        e.newest = plugin_index_newest_installable(index, p, host, official);
        e.installed_index = ~0u;
        e.reserved = custom && official && plugin_index_owner_reserved(*official, plugin.owner_id);
        e.official_name = custom && official_named(official, official_document, copied, plugin.name);
        for (u32 v = 0; v < plugin.version_count; ++v) {
            const PluginIndexVersion& listed = index.versions[plugin.first_version + v];
            PluginCatalogVersionView& view = s.versions[s.version_count++];
            view.code = listed.version_code;
            view.released = listed.released;
            view.permissions = listed.permissions;
            view.abi = listed.abi; view.min_host_abi = listed.min_host_abi; view.max_host_abi = listed.max_host_abi;
            view.changelog = listed.changelog;
            view.compatibility = plugin_index_compatibility(listed, host);
            if (const PluginIndexFile* file = plugin_index_file(listed, host.arch)) {
                view.bytes = file->bytes;
                copy_bytes(view.sha256, file->sha256, 32);
                const auto match = plugin_index_revocation(&index, official, plugin.owner_id, file->sha256);
                view.revoked = match.revoked;
                view.blocked = match.blocked;
                view.severity = match.severity;
            }
        }
        for (u32 i = 0; i < s.installed_count; ++i) {
            PluginCatalogInstalledView& installed = s.installed[i];
            if (installed.owner != plugin.owner_id) continue;
            e.installed_index = i;
            e.bound = installed.has_state && installed.provider == provider.id;
            e.revocation = plugin_index_revocation(e.bound ? &index : nullptr, official, plugin.owner_id, installed.sha256);
            if (e.revocation.revoked && e.revocation.entry < index.revoked_count) {
                e.revocation_reason = index.revoked[e.revocation.entry].reason;
                e.revocation_provider = slot;
            } else if (e.revocation.blocked && official) {
                for (u32 k = 0; k < official->blocked_count; ++k)
                    if (bytes_equal(official->blocked[k].sha256, installed.sha256, 32)) e.revocation_reason = official->blocked[k].reason;
                e.revocation_provider = 0; // the official provider is always slot 0
            }
            if (!e.bound) break;
            installed.entry = s.entry_count;
            if (!installable) break;
            PluginInstalled facts{};
            facts.installed_code = installed.installed_code;
            facts.held_code = installed.held_code;
            facts.trusted = installed.trusted;
            facts.approved_permissions = installed.approved_permissions;
            facts.known[0].version_code = installed.installed_code;
            copy_bytes(facts.known[0].sha256, installed.sha256, 32);
            e.update = plugin_index_select_update(index, p, facts, host, official);
            break;
        }
        ++s.entry_count;
    }
    return true;
}
} // namespace

PluginCatalogSnapshot* plugin_catalog_snapshot_build(const PluginManagerPaths& paths, const PluginCatalogSnapshotInput& input,
    const PluginHost& host, KdfSha256Workspace& workspace, PluginIndexWork* official_work, PluginIndexWork* work,
    PluginManagerWork* manager_work, u64 now) noexcept {
    if (!input.sources || !input.providers || !official_work || !work || !manager_work || !now || !paths.manager_count) return nullptr;
    u8* base = static_cast<u8*>(VirtualAlloc(nullptr, kReserveBytes, MEM_RESERVE, PAGE_NOACCESS));
    if (!base) return nullptr;
    if (!VirtualAlloc(base, align_up(kHeaderBytes, kPage), MEM_COMMIT, PAGE_READWRITE)) { VirtualFree(base, 0, MEM_RELEASE); return nullptr; }
    auto* s = reinterpret_cast<PluginCatalogSnapshot*>(base);
    s->reserved_bytes = kReserveBytes;
    s->entries = reinterpret_cast<PluginCatalogEntryView*>(base + align_up(kHeaderBytes, kPage));
    s->versions = reinterpret_cast<PluginCatalogVersionView*>(base + align_up(kHeaderBytes, kPage) + kEntriesBytes);
    s->documents = reinterpret_cast<char*>(base + align_up(kHeaderBytes, kPage) + kEntriesBytes + kVersionsBytes);
    s->configured = input.configured;
    s->official_flags = input.providers->official_flags;
    Builder b{s, 0, 0, 0};
    // Installed plugins first, so catalog entries can point at them.
    scan_inventory(paths, manager_work);
    const bool states = load_state(paths, &manager_work->state).ok();
    const bool trusted = load_trust(paths, &manager_work->trust).ok();
    for (u32 i = 0; i < manager_work->catalog.count && s->installed_count < kNativePluginCatalogMaximumEntries; ++i) {
        const auto& item = manager_work->entries[i];
        if (item.status != NativePluginCatalogError::none || !native_plugin_manifest_valid(item.manifest)) continue;
        const NativePluginManifest& m = item.manifest;
        PluginCatalogInstalledView& v = s->installed[s->installed_count++];
        v.owner = m.owner_id;
        v.entry = ~0u;
        copy_bytes(v.sha256, m.sha256, 32);
        v.name_bytes = minimum(m.name_bytes, kNativePluginNameCapacity);
        copy_bytes(v.name, m.name, v.name_bytes);
        v.version_bytes = minimum(m.version_bytes, kNativePluginVersionCapacity);
        copy_bytes(v.version, m.version, v.version_bytes);
        const NativePluginPreference* trust = trusted ? native_plugin_preferences_find(manager_work->trust, m) : nullptr;
        v.trusted = trust != nullptr;
        v.approved_permissions = trust ? trust->permissions : 0;
        const PluginManagerEntry* entry = states ? state_entry(manager_work->state, m.owner_id) : nullptr;
        if (entry && bytes_equal(entry->installed_sha256, m.sha256, 32)) {
            v.has_state = true;
            v.provider = entry->provider;
            v.state_flags = entry->flags;
            v.installed_code = entry->installed_code;
            v.previous_code = entry->previous_code;
            v.held_code = entry->held_code;
        } else if (entry) {
            v.state_flags = entry->flags & kPluginStateRevokedMalicious;
        }
    }
    const PluginCatalogSources& sources = *input.sources;
    const PluginIndex* official = nullptr;
    Utf8View official_document{};
    bool ok = true;
    for (u32 i = 0; ok && i < sources.count; ++i) {
        const PluginCatalogSource& source = sources.sources[i];
        const bool is_official = source.kind == PluginProviderKind::official;
        PluginCatalogProviderView& provider = s->providers[s->provider_count];
        provider.id = source.provider;
        provider.kind = source.kind;
        provider.enabled = true;
        provider.check_daily = is_official ? !(s->official_flags & kPluginOfficialChecksOff) : false;
        if (is_official) {
            // <owner>.github.io and /<repo>/, as the index location derives them.
            u32 slash = 0;
            while (slash < source.repository_bytes && source.repository[slash] != '/') ++slash;
            constexpr Utf8View pages = utf8_literal(".github.io");
            if (slash + pages.bytes <= kPluginProviderHostCapacity && source.repository_bytes - slash + 1 <= kPluginProviderPathCapacity) {
                for (u32 c = 0; c < slash; ++c) provider.host[c] = ascii_lower(source.repository[c]);
                copy_bytes(provider.host + slash, pages.data, pages.bytes);
                provider.host_bytes = slash + pages.bytes;
                copy_bytes(provider.path, source.repository + slash, source.repository_bytes - slash);
                provider.path[source.repository_bytes - slash] = '/';
                provider.path_bytes = source.repository_bytes - slash + 1;
            }
        }
        if (const PluginProviderRecord* record = sources.records[i]) {
            provider.enabled = (record->flags & kPluginProviderEnabled) != 0;
            provider.check_daily = (record->flags & kPluginProviderCheckDaily) != 0;
            provider.added = record->added;
            provider.host_bytes = record->host_bytes; copy_bytes(provider.host, record->host, record->host_bytes);
            provider.path_bytes = record->path_bytes; copy_bytes(provider.path, record->path, record->path_bytes);
            provider.name_bytes = record->name_bytes; copy_bytes(provider.name, record->name, record->name_bytes);
            copy_bytes(provider.fingerprint, record->fingerprint, 32);
        }
        PluginIndexWork* target = is_official ? official_work : work;
        const auto cached = plugin_index_load_cache(paths, source, workspace, target, now);
        provider.status = cache_status(cached);
        provider.installable = cached.installable;
        for (u32 k = 0; k < input.key_changed_count; ++k)
            if (input.key_changed[k] == source.provider) provider.status = PluginCatalogStatus::key_changed;
        const u32 slot = s->provider_count++;
        if (cached.error != PluginIndexCheckError::none) continue;
        const PluginIndex& index = target->index;
        provider.sequence = index.sequence;
        provider.issued = index.issued;
        provider.expires = index.expires;
        if (is_official) {
            official = &official_work->index;
            official_document = plugin_index_work_document(*official_work);
            provider.name_bytes = 0;
            const auto name = plugin_index_text_decode(official_document, index.provider_name, {provider.name, kPluginProviderNameCapacity});
            if (name.ok()) provider.name_bytes = name.bytes;
        }
        ok = append_index(b, slot, index, plugin_index_work_document(*target), official, official_document, host, cached.installable);
        if (is_official && ok) official_document = plugin_catalog_document(*s, slot);
    }
    if (!ok) {
        UpdateLog("plugins", "snapshot_failed").field("entries", s->entry_count).write(LogLevel::warning);
        VirtualFree(base, 0, MEM_RELEASE);
        return nullptr;
    }
    UpdateLog("plugins", "snapshot").field("providers", s->provider_count).field("entries", s->entry_count)
        .field("installed", s->installed_count).field("bytes", s->documents_bytes).write();
    return s;
}
void plugin_catalog_snapshot_release(PluginCatalogSnapshot* snapshot) noexcept {
    if (snapshot) VirtualFree(snapshot, 0, MEM_RELEASE);
}

} // namespace maicord
