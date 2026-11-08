#pragma once

#include "maicord/app/plugins/binding.hpp"

// Shared by binding.cpp, api.cpp and store.cpp.
namespace maicord::plugin_runtime_detail {

// Store limits are the ABI's MC_STORE_*_MAX; settings persist in the same kind of table.
inline constexpr u32 kSettingsKeyBytes = MC_SETTING_KEY_MAX;
inline constexpr u32 kSettingsTotalBytes = 64u * 1024u;

// The level-1 host table every context points to (api.cpp).
extern const mc_api kApi;

// The binding of a context whose call has not returned (the context is the binding's first member), else null.
static_assert(offsetof(PluginBinding, ctx) == 0);
inline PluginBinding* active(mc_ctx* ctx) noexcept {
    return ctx && ctx->call ? reinterpret_cast<PluginBinding*>(ctx) : nullptr;
}
// The binding's persisted store, opened and read from disk on first use; null when it cannot be (binding.cpp).
PluginKeyValue* store_table(PluginBinding*) noexcept;
// Keys are 1..maximum bytes of printable ASCII (0x21..0x7E).
bool key_valid(mc_str key, u32 maximum_bytes) noexcept;

// ---- secrets.cpp: SECRET settings at rest --------------------------------------

// The DPAPI form of plain (1.. bytes), bound to the plugin and the setting; *sealed_bytes is the size
// it needs (also when sealed is too small, which fails).
bool plugin_secret_seal(u64 owner_id, mc_str key, ConstSlice<u8> plain, Slice<u8> sealed, u32* sealed_bytes) noexcept;
// The plaintext of a sealed value written for the same plugin and setting; false for any other.
bool plugin_secret_unseal(u64 owner_id, mc_str key, ConstSlice<u8> sealed, Slice<u8> plain, u32* plain_bytes) noexcept;

// ---- store.cpp: persisted key-value tables --------------------------------------

bool kv_open(PluginKeyValue*, u32 maximum_keys, u32 maximum_key_bytes, u32 maximum_value_bytes,
             u32 maximum_total_bytes) noexcept;
void kv_close(PluginKeyValue*) noexcept;
mc_status kv_get(const PluginKeyValue&, mc_str key, void* output, u32 capacity, u32* bytes) noexcept;
mc_status kv_set(PluginKeyValue*, const mc_str& key, const void* value, u32 bytes, u64 now_ms) noexcept;
mc_status kv_remove(PluginKeyValue*, mc_str key, u64 now_ms) noexcept;
// The value's bytes, or null when the key has none.
const u8* kv_find(const PluginKeyValue&, const mc_str& key, u32* bytes) noexcept;
// A missing file is an empty table; a damaged one empties the table and fails.
bool kv_load(PluginKeyValue*, const wchar_t* path) noexcept;
// Writes through a temporary file and a rename. Creates the data and owner directories.
bool kv_save(PluginKeyValue*, const wchar_t* path) noexcept;

} // namespace maicord::plugin_runtime_detail
