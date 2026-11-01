#include "maicord/app/plugins/consent.hpp"
#include "binding_internal.hpp"
#include "maicord/app/update/fetch.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

struct PermissionText {
    u64 bit;
    Utf8View label;
};
constexpr PermissionText kPermissions[]{
    {MC_PERM_MESSAGE_SEND, utf8_literal("Change the messages you send or edit before they go out")},
    {MC_PERM_READ_MESSAGES, utf8_literal("Read your messages and who you talk to")},
    {MC_PERM_MESSAGE_HISTORY, utf8_literal("Keep deleted and edited messages visible while MaiCord is open")},
    {MC_PERM_READ_ACTIVITY, utf8_literal("See typing, presence, voice, server and friend activity")},
    {MC_PERM_NETWORK, utf8_literal("Connect to the servers listed below")},
    {MC_PERM_COMPOSER, utf8_literal("Read and change what you are typing")},
    {MC_PERM_ACT_MESSAGES, utf8_literal("Send, edit and delete your messages, add reactions and mark channels read")},
    {MC_PERM_ACT_PRESENCE, utf8_literal("Set your activity and custom status")},
    {MC_PERM_CLIPBOARD, utf8_literal("Copy text to your clipboard")},
    {MC_PERM_AUTOMATED_ACTIONS, utf8_literal("Act for you on its own, without you asking")},
    {MC_PERM_PERSISTENT_LOG, utf8_literal("Save deleted and edited messages on this computer")},
};
constexpr u64 covered_permissions() noexcept {
    u64 all = 0;
    for (const PermissionText& entry : kPermissions) all |= entry.bit;
    return all;
}
static_assert(covered_permissions() == MC_PERM_KNOWN, "every permission has a label");

constexpr Utf8View kBullet = utf8_literal("\n\xE2\x80\xA2 ");

void bullets(UpdateTextWriter& w, u64 permissions, bool risky) noexcept {
    for (const PermissionText& entry : kPermissions) {
        if (!(permissions & entry.bit) || !!(kPluginHighRiskPermissions & entry.bit) != risky) continue;
        w.add(kBullet);
        w.add(entry.label);
    }
}

bool approval_path(const PluginRuntime& runtime, u64 owner, wchar_t (&path)[640]) noexcept {
    u32 length = runtime.data_directory_characters;
    if (!length) return false;
    copy_bytes(path, runtime.data_directory, length * 2);
    path[length] = 0;
    return wide_append(path, 640, &length, L"\\") && wide_append_hex(path, 640, &length, owner) &&
           wide_append(path, 640, &length, L"\\approval.bin");
}

// permissions as eight little-endian bytes; hosts as their names separated by '\n'.
constexpr Utf8View kPermissionsKey = utf8_literal("permissions"), kHostsKey = utf8_literal("hosts");
bool open_table(PluginKeyValue* table) noexcept {
    return plugin_runtime_detail::kv_open(table, 4, 16, MC_NETWORK_HOSTS_MAX * (MC_HOST_MAX + 1), 4096);
}
mc_str key_of(Utf8View text) noexcept {
    return {text.data, text.bytes, 0};
}

} // namespace

Utf8View plugin_permission_label(u32 bit) noexcept {
    for (const PermissionText& entry : kPermissions)
        if (entry.bit == (u64{1} << bit)) return entry.label;
    return {};
}

u32 plugin_consent_text(const PluginConsentRequest& request, Slice<char> output) noexcept {
    UpdateTextWriter w{output, 0, true};
    if (request.author.bytes) {
        w.add(utf8_literal("Published by "));
        w.add(request.author);
        w.add(utf8_literal(".\n"));
    }
    const u64 added = request.permissions & ~request.approved_permissions;
    u32 new_hosts = 0, shown_hosts = 0;
    for (u32 i = 0; i < request.hosts.count && i < 32; ++i) new_hosts += !(request.approved_hosts & (1u << i));
    const bool repeat = request.approved_permissions || request.approved_hosts;
    if (repeat && (added || new_hosts)) {
        w.add(utf8_literal("This version asks for more than you approved before:"));
        bullets(w, added & ~kPluginHighRiskPermissions, false);
        bullets(w, added, true);
        for (u32 i = 0; i < request.hosts.count && i < 32; ++i) {
            if (request.approved_hosts & (1u << i)) continue;
            w.add(kBullet);
            w.add(utf8_literal("Connect to "));
            w.add(request.hosts[i]);
        }
        w.add(utf8_literal("\n\n"));
    }
    w.add(utf8_literal("This plugin will be able to:"));
    const u64 ordinary = request.permissions & ~kPluginHighRiskPermissions & ~MC_PERM_NETWORK;
    bullets(w, ordinary, false);
    if (request.permissions & MC_PERM_NETWORK) {
        w.add(kBullet);
        w.add(plugin_permission_label(4));
    }
    if (!(request.permissions & ~kPluginHighRiskPermissions)) w.add(utf8_literal("\nNothing beyond running its own code."));
    if (request.permissions & kPluginHighRiskPermissions) {
        w.add(utf8_literal("\n\nHigh risk, and not allowed in the official catalog:"));
        bullets(w, request.permissions, true);
    }
    for (u32 i = 0; i < request.hosts.count; ++i) {
        w.add(shown_hosts++ ? kBullet : utf8_literal("\n\nIt can reach only these servers, over HTTPS:\n\xE2\x80\xA2 "));
        w.add(request.hosts[i]);
    }
    w.add(utf8_literal("\n\nPlugins run with full access to your computer. Only enable plugins you trust."));
    return w.ok ? w.used : 0;
}

PluginConsentRequest plugin_consent_request(const PluginImage& image, Utf8View (&hosts)[MC_NETWORK_HOSTS_MAX]) noexcept {
    const mc_descriptor& d = *image.descriptor;
    for (u32 i = 0; i < d.network_count; ++i) hosts[i] = {d.network[i].data, d.network[i].bytes};
    return {{image.info.name, image.info.name_bytes},
            {image.info.version, image.info.version_bytes},
            {image.info.author, image.info.author_bytes},
            image.permissions,
            {hosts, d.network_count},
            0,
            0};
}

bool plugin_approval_store(const PluginRuntime& runtime, u64 owner_id, const mc_descriptor& d,
                           const PluginApproval& approval) noexcept {
    using namespace plugin_runtime_detail;
    wchar_t path[640];
    PluginKeyValue table;
    if (!approval_path(runtime, owner_id, path) || !open_table(&table)) return false;
    u8 permissions[8];
    byte_store_le64(permissions, approval.permissions);
    char names[MC_NETWORK_HOSTS_MAX * (MC_HOST_MAX + 1)];
    u32 used = 0;
    for (u32 i = 0; i < d.network_count; ++i) {
        if (!(approval.hosts & (1u << i))) continue;
        if (used) names[used++] = '\n';
        copy_bytes(names + used, d.network[i].data, d.network[i].bytes);
        used += d.network[i].bytes;
    }
    const bool ok = kv_set(&table, key_of(kPermissionsKey), permissions, 8, 0) == MC_OK &&
                    kv_set(&table, key_of(kHostsKey), names, used, 0) == MC_OK && kv_save(&table, path);
    kv_close(&table);
    return ok;
}

bool plugin_approval_save(const PluginBinding& binding) noexcept {
    return plugin_approval_store(*binding.runtime, binding.owner_id, *binding.image.descriptor,
                                 {binding.permissions, binding.approved_hosts});
}

PluginApproval plugin_approval_load(const PluginRuntime& runtime, u64 owner_id, const mc_descriptor& d) noexcept {
    using namespace plugin_runtime_detail;
    wchar_t path[640];
    PluginKeyValue table;
    PluginApproval approval{};
    if (!approval_path(runtime, owner_id, path) || !open_table(&table)) return approval;
    if (kv_load(&table, path)) {
        u32 bytes = 0;
        const u8* permissions = kv_find(table, key_of(kPermissionsKey), &bytes);
        if (permissions && bytes == 8) approval.permissions = byte_load_le64(permissions) & MC_PERM_KNOWN;
        const u8* names = kv_find(table, key_of(kHostsKey), &bytes);
        // Hosts are matched by name, so a plugin that reorders or drops hosts keeps only what it still declares.
        for (u32 first = 0; names && first <= bytes;) {
            u32 end = first;
            while (end < bytes && names[end] != '\n') ++end;
            for (u32 i = 0; i < d.network_count; ++i)
                if (d.network[i].bytes == end - first && bytes_equal(d.network[i].data, names + first, end - first))
                    approval.hosts |= 1u << i;
            first = end + 1;
        }
    }
    kv_close(&table);
    return approval;
}

} // namespace maicord
