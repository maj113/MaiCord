#pragma once

#include "maicord/app/plugin_index.hpp"

namespace maicord {

// Pure catalog policy over verified indexes: host compatibility, permission
// diffs, revocation and blocklist matching, owner reservation, version
// immutability and update selection. No allocation, IO or clock.
inline constexpr u32 kPluginIndexNoVersion = ~u32{0};

// The running client: its host ABI, the oldest plugin ABI it loads and its architecture.
struct PluginHost {
    u32 abi;
    u32 oldest_plugin_abi;
    PluginIndexArch arch;
};
// kPluginSdkHostAbi, kPluginSdkOldestPluginAbi and x64.
PluginHost plugin_index_current_host() noexcept;

// The first failing rule, in this order.
enum class PluginCompatibility : u8 {
    compatible,
    plugin_too_old,   // abi below the oldest plugin ABI the host loads
    needs_newer_host, // abi or min_host_abi above the host ABI ("Needs a newer MaiCord")
    host_too_new,     // a nonzero max_host_abi below the host ABI
    architecture,     // no file for the host architecture
};
// oldest <= abi <= host && min_host_abi <= host && (max_host_abi == 0 ||
// host <= max_host_abi) && a file for the host architecture.
PluginCompatibility plugin_index_compatibility(const PluginIndexVersion& version, const PluginHost& host) noexcept;
// The version's file for arch, or null.
const PluginIndexFile* plugin_index_file(const PluginIndexVersion& version, PluginIndexArch arch) noexcept;

// From the approved MC_PERM_* mask to a version's permissions.
struct PluginPermissionDiff {
    u64 added;   // permissions & ~approved: an update waits for a review prompt
    u64 removed; // approved & ~permissions: applied silently; the trust mask shrinks
    u64 kept;    // approved & permissions: the mask trust carries over with
};
PluginPermissionDiff plugin_index_permission_diff(u64 approved, u64 permissions) noexcept;

struct PluginRevocationMatch {
    bool revoked;                      // the bound provider revokes this owner and SHA-256
    bool blocked;                      // the official blocklist names this SHA-256
    PluginRevocationSeverity severity; // when revoked
    u32 entry;                         // revoked[] index when revoked
};
// provider is the index of the provider the plugin is bound to: its
// revocations apply only to its own plugins. official supplies the blocklist,
// which applies whichever provider installed the build. Either may be null
// (nothing cached) and both may be the official index.
PluginRevocationMatch plugin_index_revocation(const PluginIndex* provider, const PluginIndex* official, u64 owner_id,
    const u8* sha256) noexcept;

// True when an unofficial provider may not install owner_id: it is listed or
// revoked in the official index, or is 0 or 1 (the native settings owner).
// Developer Mode may override this; nothing here knows about it.
bool plugin_index_owner_reserved(const PluginIndex& official, u64 owner_id) noexcept;

// A build the client holds for one (provider, owner): installed or the
// retained previous version. version_code 0 is no build.
struct PluginKnownBuild {
    u64 version_code;
    u8 sha256[32];
};
// True when the index lists known.version_code for plugin with another
// SHA-256 for arch: (provider, owner, version code) -> SHA-256 is immutable.
bool plugin_index_build_conflicts(const PluginIndex& index, u32 plugin, const PluginKnownBuild& known, PluginIndexArch arch) noexcept;

// The newest listed version of plugin (an offset into its versions) that is
// compatible with host and neither revoked nor blocked; kPluginIndexNoVersion when none.
u32 plugin_index_newest_installable(const PluginIndex& index, u32 plugin, const PluginHost& host, const PluginIndex* official) noexcept;

// An installed catalog plugin bound to the provider of index.
struct PluginInstalled {
    u64 installed_code;
    u64 held_code;            // 0, or the version a rollback left or the user skipped
    u64 approved_permissions; // the plugins.bin mask when trusted
    bool trusted;             // plugins.bin trusts the installed identity
    PluginKnownBuild known[2]; // installed and retained previous builds
};
enum class PluginUpdateOutcome : u8 {
    up_to_date,   // nothing installable above the installed and held versions
    available,    // a compatible newer version within the approved mask, or an untrusted plugin
    review,       // a compatible newer version asks for more than the approved mask
    incompatible, // newer versions exist; none is compatible with this host
    conflict,     // the index re-lists a known version code with other bytes; nothing is offered
};
struct PluginUpdateSelection {
    PluginUpdateOutcome outcome;
    u32 version;                       // offset of the offered version (available, review)
    PluginCompatibility compatibility; // of the newest newer version (incompatible)
    PluginPermissionDiff permissions;  // offered version against approved_permissions
    bool carries_trust;                // trusted and within the mask: re-trust with permissions.kept, no prompt
};
// Offers only version_code > installed_code and > held_code, skips revoked
// and blocked builds, and selects the newest compatible one. An untrusted
// plugin is never loaded by an update, so its update needs no review; it
// prompts at Enable as usual.
PluginUpdateSelection plugin_index_select_update(const PluginIndex& index, u32 plugin, const PluginInstalled& installed,
    const PluginHost& host, const PluginIndex* official) noexcept;

} // namespace maicord
