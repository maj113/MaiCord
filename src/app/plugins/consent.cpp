#include "maicord/app/plugins/consent.hpp"
#include "binding_internal.hpp"
#include "maicord/app/plugins/text.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

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
} // namespace

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

bool plugin_approval_save(const PluginBinding& binding) noexcept {
    using namespace plugin_runtime_detail;
    const mc_descriptor& d = *binding.image.descriptor;
    wchar_t path[640];
    PluginKeyValue table;
    if (!approval_path(*binding.runtime, binding.owner_id, path) || !open_table(&table)) return false;
    u8 permissions[8];
    byte_store_le64(permissions, binding.permissions);
    char names[MC_NETWORK_HOSTS_MAX * (MC_HOST_MAX + 1)];
    u32 used = 0;
    for (u32 i = 0; i < d.network_count; ++i) {
        if (!(binding.approved_hosts & (1u << i))) continue;
        if (used) names[used++] = '\n';
        copy_bytes(names + used, d.network[i].data, d.network[i].bytes);
        used += d.network[i].bytes;
    }
    const bool ok = kv_set(&table, plugin_str(kPermissionsKey), permissions, 8, 0) == MC_OK &&
                    kv_set(&table, plugin_str(kHostsKey), names, used, 0) == MC_OK && kv_save(&table, path);
    kv_close(&table);
    return ok;
}

PluginApproval plugin_approval_load(const PluginRuntime& runtime, u64 owner_id, const mc_descriptor& d) noexcept {
    using namespace plugin_runtime_detail;
    wchar_t path[640];
    PluginKeyValue table;
    PluginApproval approval{};
    if (!approval_path(runtime, owner_id, path) || !open_table(&table)) return approval;
    if (kv_load(&table, path)) {
        u32 bytes = 0;
        const u8* permissions = kv_find(table, plugin_str(kPermissionsKey), &bytes);
        if (permissions && bytes == 8) approval.permissions = byte_load_le64(permissions) & MC_PERM_KNOWN;
        const u8* names = kv_find(table, plugin_str(kHostsKey), &bytes);
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
