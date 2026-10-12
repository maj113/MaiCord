// Plugin catalog policy: table-driven host compatibility, permission diffs,
// update selection (held, review, incompatible, revoked, blocked, conflict),
// revocation and blocklist matching, owner reservation and version
// immutability, over hand-built verified indexes.
#include "maicord/app/plugin_index_policy.hpp"
#include "maicord/app/plugin_sdk.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
using C = PluginCompatibility;
using O = PluginUpdateOutcome;
constexpr PluginIndexArch x64 = PluginIndexArch::x64, arm64 = PluginIndexArch::arm64;
constexpr u64 settings = 1ull << 0, store = 1ull << 5, timer = 1ull << 6, notify = 1ull << 19, clipboard = 1ull << 17;
constexpr u64 owner = 0x4242;

PluginIndexVersion make_version(u32 abi, u32 min_host, u32 max_host, bool has_x64, bool has_arm64, u64 code = 1999,
    u64 permissions = settings, u8 tag = 1) noexcept {
    PluginIndexVersion v{};
    v.abi = abi; v.min_host_abi = min_host; v.max_host_abi = max_host; v.version_code = code; v.permissions = permissions;
    if (has_x64) { v.files[v.file_count].arch = x64; v.files[v.file_count].sha256[0] = tag; v.files[v.file_count++].bytes = 4096; }
    if (has_arm64) { v.files[v.file_count].arch = arm64; v.files[v.file_count].sha256[0] = static_cast<u8>(tag | 0x80); v.files[v.file_count++].bytes = 4096; }
    return v;
}

void test_compatibility(Check check) noexcept {
    struct Row { PluginHost host; u32 abi, min_host, max_host; bool x, a; C expected; };
    constexpr PluginHost h5{5, 5, x64}, h6{6, 5, x64}, h6_6{6, 6, x64}, h6_arm{6, 5, arm64}, h7{7, 5, x64}, wide{0xFFFF, 5, x64};
    constexpr Row rows[]{
        {h5, 5, 5, 0, true, false, C::compatible},
        {h5, 5, 5, 5, true, false, C::compatible},
        {h5, 5, 5, 7, true, false, C::compatible},
        {h5, 4, 5, 0, true, false, C::plugin_too_old},
        {h5, 6, 6, 0, true, false, C::needs_newer_host},
        {h5, 5, 6, 0, true, false, C::needs_newer_host},
        {h5, 7, 7, 0, true, true, C::needs_newer_host},
        {h5, 5, 0xFFFF, 0, true, false, C::needs_newer_host},
        {h5, 5, 5, 4, true, false, C::host_too_new},
        {h5, 5, 5, 0, false, true, C::architecture},
        {h5, 5, 5, 0, true, true, C::compatible},
        {h6, 5, 5, 5, true, false, C::host_too_new},
        {h6, 5, 5, 0, true, false, C::compatible},
        {h6, 6, 6, 0, true, false, C::compatible},
        {h6_6, 5, 5, 0, true, false, C::plugin_too_old},
        {h6_arm, 5, 5, 0, true, true, C::compatible},
        {h6_arm, 5, 5, 0, true, false, C::architecture},
        {wide, 5, 5, 0, true, false, C::compatible},
        {h7, 5, 5, 6, true, false, C::host_too_new},
        {h7, 5, 5, 7, true, false, C::compatible},
        {h5, 4, 5, 0, false, true, C::plugin_too_old},   // the first failing rule wins
        {h5, 6, 6, 0, false, true, C::needs_newer_host},
        {h6, 5, 5, 5, false, true, C::host_too_new},
    };
    u32 passed = 0;
    for (const Row& row : rows)
        passed += plugin_index_compatibility(make_version(row.abi, row.min_host, row.max_host, row.x, row.a), row.host) == row.expected;
    check(passed == sizeof(rows) / sizeof(rows[0]), "plugin policy: every compatibility row gives its verdict");
    const PluginHost current = plugin_index_current_host();
    check(current.abi == kPluginSdkHostAbi && current.oldest_plugin_abi == kPluginSdkOldestPluginAbi && current.arch == x64 &&
        kPluginSdkOldestPluginAbi == 5 && kPluginSdkOldestPluginAbi <= kPluginSdkHostAbi,
        "plugin policy: the current host is the SDK's ABI range on x64");
    const PluginIndexVersion both = make_version(5, 5, 0, true, true, 1999, settings, 3);
    check(plugin_index_file(both, arm64) == &both.files[1] && plugin_index_file(both, x64) == &both.files[0] &&
        !plugin_index_file(make_version(5, 5, 0, true, false), arm64), "plugin policy: files are found by architecture");
}

void test_permission_diff(Check check) noexcept {
    struct Row { u64 approved, permissions, added, removed, kept; };
    constexpr Row rows[]{
        {0b1011, 0b0110, 0b0100, 0b1001, 0b0010},
        {settings | store, settings | store, 0, 0, settings | store},
        {0, settings | timer, settings | timer, 0, 0},
        {settings | store | timer, settings, 0, store | timer, settings},
        {settings, notify, notify, settings, 0},
        {~0ull >> 42, ~0ull >> 42, 0, 0, ~0ull >> 42},
    };
    u32 passed = 0;
    for (const Row& row : rows) {
        const auto diff = plugin_index_permission_diff(row.approved, row.permissions);
        passed += diff.added == row.added && diff.removed == row.removed && diff.kept == row.kept;
    }
    check(passed == sizeof(rows) / sizeof(rows[0]), "plugin policy: permission diffs split added, removed and kept masks");
}

// One provider listing owner with four versions, newest first:
//   0: 4.0.0 needs host ABI 6;  1: 3.0.0 adds notify;  2: 2.0.0 x64 + arm64;  3: 1.0.0
PluginIndex g_provider, g_official;
void build_indexes() noexcept {
    g_provider = {};
    g_provider.plugin_count = 1; g_provider.version_count = 4;
    g_provider.plugins[0].owner_id = owner; g_provider.plugins[0].first_version = 0; g_provider.plugins[0].version_count = 4;
    g_provider.versions[0] = make_version(6, 6, 0, true, false, 4000000999, settings | store, 4);
    g_provider.versions[1] = make_version(5, 5, 0, true, false, 3000000999, settings | store | notify, 3);
    g_provider.versions[2] = make_version(5, 5, 0, true, true, 2000000999, settings | store, 2);
    g_provider.versions[3] = make_version(5, 5, 0, true, false, 1000000999, settings | store, 1);
    g_official = {};
    g_official.plugin_count = 2;
    g_official.plugins[0].owner_id = 0x1000; g_official.plugins[1].owner_id = 0x2000;
    g_official.revoked_count = 1; g_official.revoked[0].owner_id = 0x3000;
}
PluginInstalled installed_at(u64 code, u64 approved = settings | store, bool trusted = true, u64 held = 0) noexcept {
    PluginInstalled state{};
    state.installed_code = code; state.held_code = held; state.approved_permissions = approved; state.trusted = trusted;
    return state;
}

void test_update_selection(Check check) noexcept {
    build_indexes();
    const PluginHost host{5, 5, x64};
    struct Row { PluginInstalled installed; PluginHost host; O outcome; u32 version; bool carries; u64 added, removed; C compatibility; };
    const Row rows[]{
        // Trusted 1.0.0: 3.0.0 is the newest compatible and adds notify.
        {installed_at(1000000999), host, O::review, 1, false, notify, 0, C::compatible},
        // Held at 3.0.0 (rolled back or skipped): only the incompatible 4.0.0 is above it.
        {installed_at(1000000999, settings | store, true, 3000000999), host, O::incompatible, kPluginIndexNoVersion, false, 0, 0, C::needs_newer_host},
        // Already approved notify: trust carries over without a prompt.
        {installed_at(2000000999, settings | store | notify), host, O::available, 1, true, 0, 0, C::compatible},
        // Approved more than 3.0.0 asks for: the mask shrinks silently.
        {installed_at(2000000999, settings | store | notify | timer), host, O::available, 1, true, 0, timer, C::compatible},
        {installed_at(3000000999), host, O::incompatible, kPluginIndexNoVersion, false, 0, 0, C::needs_newer_host},
        {installed_at(4000000999), host, O::up_to_date, kPluginIndexNoVersion, false, 0, 0, C::compatible},
        // Untrusted plugins are never loaded by an update: no review.
        {installed_at(1000000999, 0, false), host, O::available, 1, false, settings | store | notify, 0, C::compatible},
        // With host ABI 6, 4.0.0 is compatible and keeps the approved mask.
        {installed_at(1000000999), PluginHost{6, 5, x64}, O::available, 0, true, 0, 0, C::compatible},
        // On arm64 only 2.0.0 has a build.
        {installed_at(1000000999), PluginHost{5, 5, arm64}, O::available, 2, true, 0, 0, C::compatible},
        // The newest newer version names the reason: 4.0.0 needs host ABI 6.
        {installed_at(2000000999), PluginHost{5, 5, arm64}, O::incompatible, kPluginIndexNoVersion, false, 0, 0, C::needs_newer_host},
        // A held version below the installed one changes nothing.
        {installed_at(2000000999, settings | store | notify, true, 1000000999), host, O::available, 1, true, 0, 0, C::compatible},
    };
    u32 passed = 0;
    for (const Row& row : rows) {
        const auto s = plugin_index_select_update(g_provider, 0, row.installed, row.host, nullptr);
        passed += s.outcome == row.outcome && s.version == row.version && s.carries_trust == row.carries &&
            s.permissions.added == row.added && s.permissions.removed == row.removed && s.compatibility == row.compatibility;
    }
    check(passed == sizeof(rows) / sizeof(rows[0]), "plugin policy: update selection honours held versions, review, trust and host");

    // The provider revokes 3.0.0: 2.0.0 is offered instead.
    g_provider.revoked_count = 1;
    g_provider.revoked[0].owner_id = owner;
    g_provider.revoked[0].sha256[0] = 3;
    g_provider.revoked[0].severity = PluginRevocationSeverity::malicious;
    auto s = plugin_index_select_update(g_provider, 0, installed_at(1000000999), host, nullptr);
    check(s.outcome == O::available && s.version == 2 && s.carries_trust,
        "plugin policy: a revoked build is never offered and the next compatible version is");
    // Revocations name owner and build: another owner's identical bytes are not revoked.
    g_provider.revoked[0].owner_id = owner + 1;
    s = plugin_index_select_update(g_provider, 0, installed_at(1000000999), host, nullptr);
    check(s.outcome == O::review && s.version == 1, "plugin policy: a revocation applies only to its owner");
    g_provider.revoked_count = 0; g_provider.revoked[0] = {};
    // The official blocklist covers builds from any provider.
    g_official.blocked_count = 1; g_official.blocked[0].sha256[0] = 3;
    s = plugin_index_select_update(g_provider, 0, installed_at(1000000999), host, &g_official);
    check(s.outcome == O::available && s.version == 2, "plugin policy: an officially blocked build is skipped for any provider");
    check(plugin_index_newest_installable(g_provider, 0, host, &g_official) == 2 &&
        plugin_index_newest_installable(g_provider, 0, host, nullptr) == 1 &&
        plugin_index_newest_installable(g_provider, 0, PluginHost{6, 5, x64}, nullptr) == 0 &&
        plugin_index_newest_installable(g_provider, 0, PluginHost{5, 5, arm64}, nullptr) == 2 &&
        plugin_index_newest_installable(g_provider, 0, PluginHost{4, 4, x64}, nullptr) == kPluginIndexNoVersion &&
        plugin_index_newest_installable(g_provider, 1, host, nullptr) == kPluginIndexNoVersion,
        "plugin policy: the newest installable version skips incompatible and blocked builds");
    g_official.blocked_count = 0; g_official.blocked[0] = {};

    // (provider, owner, version code) -> SHA-256 is immutable.
    PluginInstalled conflicted = installed_at(1000000999);
    conflicted.known[0].version_code = 1000000999; conflicted.known[0].sha256[0] = 9;
    PluginInstalled consistent = installed_at(1000000999);
    consistent.known[0].version_code = 1000000999; consistent.known[0].sha256[0] = 1;
    consistent.known[1].version_code = 2000000999; consistent.known[1].sha256[0] = 2;
    check(plugin_index_select_update(g_provider, 0, conflicted, host, nullptr).outcome == O::conflict &&
        plugin_index_select_update(g_provider, 0, consistent, host, nullptr).outcome == O::review,
        "plugin policy: a re-listed version code with other bytes stops every offer");
}

void test_matching(Check check) noexcept {
    build_indexes();
    u8 sha[32]{};
    sha[0] = 7;
    g_provider.revoked_count = 2;
    g_provider.revoked[0].owner_id = owner + 1; g_provider.revoked[0].sha256[0] = 7;
    g_provider.revoked[1].owner_id = owner; g_provider.revoked[1].sha256[0] = 7;
    g_provider.revoked[1].severity = PluginRevocationSeverity::withdrawn;
    g_official.blocked_count = 1; g_official.blocked[0].sha256[0] = 8;
    g_official.revoked_count = 2; g_official.revoked[1].owner_id = owner; g_official.revoked[1].sha256[0] = 7;
    u8 blocked[32]{};
    blocked[0] = 8;
    const auto revoked = plugin_index_revocation(&g_provider, &g_official, owner, sha);
    const auto other = plugin_index_revocation(&g_provider, &g_official, owner + 2, sha);
    const auto kill = plugin_index_revocation(&g_provider, &g_official, owner + 2, blocked);
    const auto bound_elsewhere = plugin_index_revocation(&g_provider, nullptr, owner + 2, sha);
    const auto official_bound = plugin_index_revocation(&g_official, &g_official, owner, sha);
    check(revoked.revoked && !revoked.blocked && revoked.entry == 1 && revoked.severity == PluginRevocationSeverity::withdrawn &&
        !other.revoked && !other.blocked && kill.blocked && !kill.revoked && !bound_elsewhere.revoked &&
        official_bound.revoked && official_bound.entry == 1,
        "plugin policy: revocations match owner and build within the bound provider; the blocklist matches any build");
    const auto none = plugin_index_revocation(nullptr, nullptr, owner, sha);
    check(!none.revoked && !none.blocked && !plugin_index_revocation(&g_provider, &g_official, owner, nullptr).revoked,
        "plugin policy: nothing cached means nothing revoked");
    check(plugin_index_owner_reserved(g_official, 0x1000) && plugin_index_owner_reserved(g_official, 0x2000) &&
        plugin_index_owner_reserved(g_official, 0x3000) && plugin_index_owner_reserved(g_official, 1) &&
        plugin_index_owner_reserved(g_official, 0) && plugin_index_owner_reserved(g_official, owner) &&
        !plugin_index_owner_reserved(g_official, 0x7777),
        "plugin policy: owners listed or revoked officially (and 0 and 1) are reserved from unofficial providers");
    build_indexes();
    PluginKnownBuild known{1000000999, {}};
    known.sha256[0] = 1;
    PluginKnownBuild changed = known; changed.sha256[0] = 2;
    PluginKnownBuild unlisted{5000000999, {}};
    PluginKnownBuild arm{1000000999, {}};
    arm.sha256[0] = 0x55;
    check(!plugin_index_build_conflicts(g_provider, 0, known, x64) && plugin_index_build_conflicts(g_provider, 0, changed, x64) &&
        !plugin_index_build_conflicts(g_provider, 0, unlisted, x64) && !plugin_index_build_conflicts(g_provider, 0, arm, arm64) &&
        !plugin_index_build_conflicts(g_provider, 0, PluginKnownBuild{}, x64) && !plugin_index_build_conflicts(g_provider, 3, changed, x64),
        "plugin policy: a listed version code conflicts only when its build for the architecture has other bytes");
}
} // namespace

void test_plugin_index_policy(Check check) noexcept {
    test_compatibility(check);
    test_permission_diff(check);
    test_update_selection(check);
    test_matching(check);
}

} // namespace maicord
