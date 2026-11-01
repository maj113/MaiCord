#include "binding_internal.hpp"
#include "ui_internal.hpp"
#include "pipeline_internal.hpp"
#include "surfaces_internal.hpp"
#include "maicord/app/plugins/http.hpp"
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
    if (runtime->toast) {
        runtime->toast(runtime->sink_context, binding->owner_id, {text.data, text.bytes}, tone);
        return MC_OK;
    }
    // Without a runtime sink the application's host shows it.
    const PluginUiHost* host = plugin_ui_host();
    if (!host || !host->toast) return MC_E_UNSUPPORTED;
    host->toast(binding->owner_id, {text.data, text.bytes}, tone);
    return MC_OK;
}
mc_status MC_CALL api_http(mc_ctx* ctx, const mc_http_request* request, uint64_t tag) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    return request ? plugin_http_submit(binding, *request, tag) : MC_E_INVALID;
}

mc_status MC_CALL api_banner(mc_ctx* ctx, const mc_banner* banner) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    return banner ? plugin_ui_detail::banner_set(*binding, *banner) : MC_E_INVALID;
}
mc_status MC_CALL api_banner_clear(mc_ctx* ctx, uint64_t tag) noexcept {
    PluginBinding* binding = active(ctx);
    return binding ? plugin_ui_detail::banner_clear(*binding, tag) : MC_E_INACTIVE;
}
mc_status MC_CALL api_modal(mc_ctx* ctx, const mc_modal* modal, uint64_t tag) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    return modal ? plugin_ui_detail::modal_open(*binding, *modal, tag) : MC_E_INVALID;
}
mc_status MC_CALL api_open_url(mc_ctx* ctx, mc_str url) noexcept {
    PluginBinding* binding = active(ctx);
    return binding ? plugin_ui_detail::open_url(*binding, url) : MC_E_INACTIVE;
}
mc_status MC_CALL api_copy_text(mc_ctx* ctx, mc_str text) noexcept {
    PluginBinding* binding = active(ctx);
    return binding ? plugin_ui_detail::copy_text(*binding, text) : MC_E_INACTIVE;
}
mc_status MC_CALL api_open(mc_ctx* ctx, uint32_t what, uint64_t id, uint64_t sub) noexcept {
    PluginBinding* binding = active(ctx);
    return binding ? plugin_ui_detail::open_target(*binding, what, id, sub) : MC_E_INACTIVE;
}

mc_status MC_CALL api_invalidate(mc_ctx* ctx, uint32_t what, uint64_t id) noexcept {
    PluginBinding* binding = active(ctx);
    return binding ? plugin_surfaces_detail::plugin_surfaces_invalidate(binding, what, id) : MC_E_INACTIVE;
}
mc_status MC_CALL api_decorate_push(mc_ctx* ctx, uint32_t kind, uint64_t id, uint64_t scope,
                                    const mc_decoration* decoration) noexcept {
    PluginBinding* binding = active(ctx);
    return binding ? plugin_surfaces_detail::plugin_surfaces_push(binding, kind, id, scope, decoration) : MC_E_INACTIVE;
}
mc_status MC_CALL api_button_state(mc_ctx* ctx, uint32_t button, const mc_button_state* state) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    return state ? plugin_surfaces_detail::buttons_set_state(binding, button, *state) : MC_E_INVALID;
}
mc_status MC_CALL api_read_user(mc_ctx* ctx, uint64_t user, uint64_t guild, mc_user* out) noexcept {
    return active(ctx) ? plugin_surfaces_detail::read_user(user, guild, out) : MC_E_INACTIVE;
}
mc_status MC_CALL api_read_channel(mc_ctx* ctx, uint64_t channel, mc_channel* out) noexcept {
    return active(ctx) ? plugin_surfaces_detail::read_channel(channel, out) : MC_E_INACTIVE;
}
mc_status MC_CALL api_read_guild(mc_ctx* ctx, uint64_t guild, mc_guild* out) noexcept {
    return active(ctx) ? plugin_surfaces_detail::read_guild(guild, out) : MC_E_INACTIVE;
}
mc_status MC_CALL api_read_permissions(mc_ctx* ctx, uint64_t guild, uint64_t channel, uint64_t user,
                                       uint64_t* bits) noexcept {
    return active(ctx) ? plugin_surfaces_detail::read_permissions(guild, channel, user, bits) : MC_E_INACTIVE;
}

} // namespace

// Designated, one line per entry: each runtime file adds its own entries here.
const mc_api kApi{
    .size = sizeof(mc_api),
    .level = MC_API_LEVEL,
    .log = api_log,
    .set_setting = api_set_setting,
    .store_get = api_store_get,
    .store_set = api_store_set,
    .store_remove = api_store_remove,
    .toast = api_toast,
    .invalidate = api_invalidate,
    .decorate_push = api_decorate_push,
    .button_state = api_button_state,
    .read_user = api_read_user,
    .read_channel = api_read_channel,
    .read_guild = api_read_guild,
    .read_permissions = api_read_permissions,
    .banner = api_banner,
    .banner_clear = api_banner_clear,
    .modal = api_modal,
    .open_url = api_open_url,
    .copy_text = api_copy_text,
    .open = api_open,
    .http = api_http,
    .local_message = api_local_message,
    .local_message_remove = api_local_message_remove,
    .composer_text = api_composer_text,
    .composer_insert = api_composer_insert,
    .composer_replace = api_composer_replace,
    .composer_focus = api_composer_focus,
    .composer_reply = api_composer_reply,
    .composer_edit = api_composer_edit,
    .send_message = api_send_message,
    .edit_message = api_edit_message,
    .react = api_react,
    .delete_own = api_delete_own,
    .mark_read = api_mark_read,
    .set_activity = api_set_activity,
    .timer = api_timer,
    .cancel_timer = api_cancel_timer,
    .keep_info = api_keep_info,
    .keep_text = api_keep_text,
    .keep_restore = api_keep_restore,
};

} // namespace maicord::plugin_runtime_detail
