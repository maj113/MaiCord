#include "binding_internal.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"

// mc_api level 1. Every function recovers its binding from the context, which is the
// binding's first member, and refuses a context whose call has returned.
namespace maicord::plugin_runtime_detail {
namespace {

static_assert(offsetof(PluginBinding, ctx) == 0);

PluginBinding* active(mc_ctx* ctx) noexcept {
    return ctx && ctx->call ? reinterpret_cast<PluginBinding*>(ctx) : nullptr;
}

// Short human text: UTF-8 with no control characters, 1..maximum bytes.
bool notice_text(mc_str text, u32 maximum) noexcept {
    if (!text.data || !text.bytes || text.bytes > maximum || !utf8_validate({text.data, text.bytes}).valid) return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (static_cast<u8>(text.data[i]) < 0x20 || static_cast<u8>(text.data[i]) == 0x7f) return false;
    return true;
}

void MC_CALL api_log(mc_ctx* ctx, uint32_t level, mc_str text) noexcept {
    PluginBinding* binding = active(ctx);
    const PluginRuntime* runtime = binding ? binding->runtime : nullptr;
    if (!runtime || !runtime->log || !text.data || level > MC_LOG_ERROR) return;
    // Longer text is cut on a character boundary.
    const Utf8View view{text.data, utf8_truncated_length({text.data, text.bytes}, MC_LOG_TEXT_MAX)};
    runtime->log(runtime->sink_context, binding->owner_id, level, view);
}

mc_status MC_CALL api_set_setting(mc_ctx* ctx, uint32_t index, const void* value, uint32_t bytes) noexcept {
    PluginBinding* binding = active(ctx);
    return binding ? plugin_binding_set_setting(binding, index, value, bytes) : MC_E_INACTIVE;
}

mc_status MC_CALL api_store_get(mc_ctx* ctx, mc_str key, void* output, uint32_t capacity, uint32_t* bytes) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (!bytes || !key_valid(key, MC_STORE_KEY_MAX) || (capacity && !output)) return MC_E_INVALID;
    const PluginKeyValue* store = store_table(binding);
    return store ? kv_get(*store, key, output, capacity, bytes) : MC_E_BUSY;
}

mc_status MC_CALL api_store_set(mc_ctx* ctx, mc_str key, const void* value, uint32_t bytes) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (!key_valid(key, MC_STORE_KEY_MAX) || (bytes && !value)) return MC_E_INVALID;
    PluginKeyValue* store = store_table(binding);
    return store ? kv_set(store, key, value, bytes, clock_monotonic_milliseconds()) : MC_E_BUSY;
}

mc_status MC_CALL api_store_remove(mc_ctx* ctx, mc_str key) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (!key_valid(key, MC_STORE_KEY_MAX)) return MC_E_INVALID;
    PluginKeyValue* store = store_table(binding);
    return store ? kv_remove(store, key, clock_monotonic_milliseconds()) : MC_E_BUSY;
}

mc_status MC_CALL api_toast(mc_ctx* ctx, mc_str text, uint32_t tone, uint32_t flags) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (flags || tone > MC_TONE_CRITICAL || !notice_text(text, MC_TOAST_TEXT_MAX)) return MC_E_INVALID;
    const PluginRuntime* runtime = binding->runtime;
    if (!runtime->toast) return MC_E_UNSUPPORTED;
    runtime->toast(runtime->sink_context, binding->owner_id, {text.data, text.bytes}, tone);
    return MC_OK;
}

} // namespace

const mc_api kApi{sizeof(mc_api), 1, api_log, api_set_setting, api_store_get, api_store_set, api_store_remove,
                  api_toast};

} // namespace maicord::plugin_runtime_detail
