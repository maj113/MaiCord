#include "maicord/app/plugin_index_policy.hpp"
#include "maicord/app/plugin_sdk.hpp"

namespace maicord {
namespace {
bool listed(const PluginIndex& index, u32 plugin) noexcept {
    return plugin < index.plugin_count && index.plugins[plugin].first_version + index.plugins[plugin].version_count <= index.version_count;
}
const PluginIndexVersion& version_at(const PluginIndex& index, u32 plugin, u32 offset) noexcept {
    return index.versions[index.plugins[plugin].first_version + offset];
}
// Revoked by the bound provider or blocked officially, judged by the host-architecture build.
bool withdrawn(const PluginIndex& index, u64 owner, const PluginIndexVersion& version, const PluginHost& host,
    const PluginIndex* official) noexcept {
    const PluginIndexFile* file = plugin_index_file(version, host.arch);
    if (!file) return false;
    const auto match = plugin_index_revocation(&index, official, owner, file->sha256);
    return match.revoked || match.blocked;
}
} // namespace

PluginHost plugin_index_current_host() noexcept {
    return {kPluginSdkHostAbi, kPluginSdkOldestPluginAbi, PluginIndexArch::x64};
}

const PluginIndexFile* plugin_index_file(const PluginIndexVersion& version, PluginIndexArch arch) noexcept {
    for (u32 i = 0; i < version.file_count && i < kPluginIndexMaximumFiles; ++i)
        if (version.files[i].arch == arch) return &version.files[i];
    return nullptr;
}

PluginCompatibility plugin_index_compatibility(const PluginIndexVersion& version, const PluginHost& host) noexcept {
    if (version.abi < host.oldest_plugin_abi) return PluginCompatibility::plugin_too_old;
    if (version.abi > host.abi || version.min_host_abi > host.abi) return PluginCompatibility::needs_newer_host;
    if (version.max_host_abi && host.abi > version.max_host_abi) return PluginCompatibility::host_too_new;
    if (!plugin_index_file(version, host.arch)) return PluginCompatibility::architecture;
    return PluginCompatibility::compatible;
}

PluginPermissionDiff plugin_index_permission_diff(u64 approved, u64 permissions) noexcept {
    return {permissions & ~approved, approved & ~permissions, approved & permissions};
}

PluginRevocationMatch plugin_index_revocation(const PluginIndex* provider, const PluginIndex* official, u64 owner_id,
    const u8* sha256) noexcept {
    PluginRevocationMatch match{};
    if (!sha256) return match;
    if (provider) {
        for (u32 i = 0; i < provider->revoked_count && i < kPluginIndexMaximumRevoked; ++i) {
            const auto& entry = provider->revoked[i];
            if (entry.owner_id != owner_id || !bytes_equal(entry.sha256, sha256, 32)) continue;
            match.revoked = true; match.severity = entry.severity; match.entry = i;
            break;
        }
    }
    if (official)
        for (u32 i = 0; i < official->blocked_count && i < kPluginIndexMaximumBlocked && !match.blocked; ++i)
            match.blocked = bytes_equal(official->blocked[i].sha256, sha256, 32);
    return match;
}

bool plugin_index_owner_reserved(const PluginIndex& official, u64 owner_id) noexcept {
    if (owner_id <= 1) return true;
    for (u32 i = 0; i < official.plugin_count && i < kPluginIndexMaximumPlugins; ++i)
        if (official.plugins[i].owner_id == owner_id) return true;
    for (u32 i = 0; i < official.revoked_count && i < kPluginIndexMaximumRevoked; ++i)
        if (official.revoked[i].owner_id == owner_id) return true;
    return false;
}

bool plugin_index_build_conflicts(const PluginIndex& index, u32 plugin, const PluginKnownBuild& known, PluginIndexArch arch) noexcept {
    if (!known.version_code || !listed(index, plugin)) return false;
    for (u32 i = 0; i < index.plugins[plugin].version_count; ++i) {
        const PluginIndexVersion& version = version_at(index, plugin, i);
        if (version.version_code != known.version_code) continue;
        const PluginIndexFile* file = plugin_index_file(version, arch);
        return file && !bytes_equal(file->sha256, known.sha256, 32);
    }
    return false;
}

u32 plugin_index_newest_installable(const PluginIndex& index, u32 plugin, const PluginHost& host, const PluginIndex* official) noexcept {
    if (!listed(index, plugin)) return kPluginIndexNoVersion;
    const u64 owner = index.plugins[plugin].owner_id;
    for (u32 i = 0; i < index.plugins[plugin].version_count; ++i) {
        const PluginIndexVersion& version = version_at(index, plugin, i);
        if (!withdrawn(index, owner, version, host, official) && plugin_index_compatibility(version, host) == PluginCompatibility::compatible)
            return i;
    }
    return kPluginIndexNoVersion;
}

PluginUpdateSelection plugin_index_select_update(const PluginIndex& index, u32 plugin, const PluginInstalled& installed,
    const PluginHost& host, const PluginIndex* official) noexcept {
    PluginUpdateSelection selection{PluginUpdateOutcome::up_to_date, kPluginIndexNoVersion, PluginCompatibility::compatible, {}, false};
    if (!listed(index, plugin)) return selection;
    for (const auto& known : installed.known)
        if (plugin_index_build_conflicts(index, plugin, known, host.arch)) { selection.outcome = PluginUpdateOutcome::conflict; return selection; }
    const u64 owner = index.plugins[plugin].owner_id;
    bool newer = false;
    for (u32 i = 0; i < index.plugins[plugin].version_count; ++i) {
        const PluginIndexVersion& version = version_at(index, plugin, i);
        if (version.version_code <= installed.installed_code || version.version_code <= installed.held_code) break; // newest first
        if (withdrawn(index, owner, version, host, official)) continue;
        const PluginCompatibility compatibility = plugin_index_compatibility(version, host);
        if (!newer) { newer = true; selection.compatibility = compatibility; }
        if (compatibility != PluginCompatibility::compatible) continue;
        selection.version = i;
        selection.compatibility = compatibility;
        selection.permissions = plugin_index_permission_diff(installed.approved_permissions, version.permissions);
        const bool review = installed.trusted && selection.permissions.added;
        selection.outcome = review ? PluginUpdateOutcome::review : PluginUpdateOutcome::available;
        selection.carries_trust = installed.trusted && !review;
        return selection;
    }
    if (newer) selection.outcome = PluginUpdateOutcome::incompatible;
    return selection;
}

} // namespace maicord
