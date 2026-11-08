#include "maicord/app/plugins/http.hpp"
#include "binding_internal.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"


namespace maicord {
using namespace plugin_runtime_detail;
namespace {

constexpr u32 kPageBytes = 4096;
constexpr u32 kAlignment = 64;
constexpr u32 kItemTableBytes = MC_SETTING_ITEM_MAX * sizeof(mc_str);
// A list value's bytes: its items, and a separator between each.
constexpr u32 kListValueBytes = MC_SETTING_LIST_BYTES_MAX + MC_SETTING_ITEM_MAX;
// A secret in the clear is at most MC_SETTING_TEXT_MAX bytes; its DPAPI form adds a header.
constexpr u32 kSecretBytes = MC_SETTING_TEXT_MAX + 1024;

static_assert(kPluginKeyControl == MC_KEY_CTRL && kPluginKeyShift == MC_KEY_SHIFT && kPluginKeyAlt == MC_KEY_ALT);

constexpr u32 align_up(u32 value, u32 alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Every binding holds a validated descriptor from load to unload.
const mc_setting_def* setting_defs(const PluginBinding& binding) noexcept {
    return binding.image.descriptor->settings;
}
u32 setting_total(const PluginBinding& binding) noexcept {
    return binding.image.descriptor->setting_count;
}
u32 text_capacity(const mc_setting_def& setting) noexcept {
    return setting.max_bytes ? setting.max_bytes : MC_SETTING_TEXT_MAX;
}
u8* settings_block(const PluginBinding& binding) noexcept {
    return const_cast<u8*>(static_cast<const u8*>(binding.ctx.settings));
}

// ---- Setting values ------------------------------------------------------------
// A value travels as bytes in the encoding mc_api.set_setting documents: TOGGLE one
// byte, INT/SLIDER/CHOICE four little-endian bytes, TEXT UTF-8, TEXT_LIST UTF-8
// items separated by '\n'. Validation and the write share one function.

// Whether text is storable: UTF-8 without NUL, and without line breaks unless multi-line.
bool text_acceptable(const u8* text, u32 bytes, bool multiline) noexcept {
    if (!utf8_validate({reinterpret_cast<const char*>(text), bytes}).valid) return false;
    for (u32 i = 0; i < bytes; ++i)
        if (!text[i] || (!multiline && (text[i] == '\n' || text[i] == '\r'))) return false;
    return true;
}

mc_status write_setting(PluginBinding& binding, u32 index, const u8* value, u32 bytes, bool commit) noexcept {
    const mc_setting_def& setting = setting_defs(binding)[index];
    u8* at = settings_block(binding) + setting.offset;
    switch (setting.type) {
    case MC_SET_TOGGLE:
        if (bytes != 1 || value[0] > 1) return MC_E_INVALID;
        if (commit) *at = value[0];
        return MC_OK;
    case MC_SET_INT:
    case MC_SET_SLIDER: {
        if (bytes != 4) return MC_E_INVALID;
        const i32 number = static_cast<i32>(byte_load_le32(value));
        if (number < setting.minimum || number > setting.maximum) return MC_E_INVALID;
        if (commit) copy_bytes(at, &number, 4);
        return MC_OK;
    }
    case MC_SET_CHOICE: {
        const u32 choice = bytes == 4 ? byte_load_le32(value) : ~0u;
        if (choice >= setting.item_count) return MC_E_INVALID;
        if (commit) copy_bytes(at, &choice, 4);
        return MC_OK;
    }
    case MC_SET_TEXT:
    case MC_SET_SECRET: {
        if (bytes > text_capacity(setting)) return MC_E_CAPACITY;
        if (!text_acceptable(value, bytes, setting.type == MC_SET_TEXT && (setting.flags & MC_SET_MULTILINE)))
            return MC_E_INVALID;
        if (commit) {
            char* text = reinterpret_cast<char*>(binding.data + binding.text_offsets[index]);
            copy_bytes(text, value, bytes);
            const mc_str stored{text, bytes, 0};
            copy_bytes(at, &stored, sizeof(stored));
        }
        return MC_OK;
    }
    case MC_SET_TEXT_LIST: {
        if (bytes > kListValueBytes) return MC_E_CAPACITY;
        if (!text_acceptable(value, bytes, true)) return MC_E_INVALID;
        // Items are the non-empty lines.
        mc_str* items = reinterpret_cast<mc_str*>(binding.data + binding.text_offsets[index]);
        char* characters = reinterpret_cast<char*>(items + MC_SETTING_ITEM_MAX);
        u32 count = 0, used = 0;
        for (u32 first = 0; first < bytes;) {
            u32 end = first;
            while (end < bytes && value[end] != '\n') ++end;
            const u32 length = end - first;
            if (length) {
                if (length > text_capacity(setting) || count == MC_SETTING_ITEM_MAX ||
                    used + length > MC_SETTING_LIST_BYTES_MAX) return MC_E_CAPACITY;
                if (commit) {
                    copy_bytes(characters + used, value + first, length);
                    items[count] = {characters + used, length, 0};
                }
                ++count;
                used += length;
            }
            first = end + 1;
        }
        if (commit) {
            const mc_list list{items, count, 0};
            copy_bytes(at, &list, sizeof(list));
        }
        return MC_OK;
    }
    case MC_SET_COLOUR: {
        const u32 colour = bytes == 4 ? byte_load_le32(value) : ~0u;
        if (colour > 0xFFFFFFu) return MC_E_INVALID;
        if (commit) copy_bytes(at, &colour, 4);
        return MC_OK;
    }
    case MC_SET_KEYBIND: {
        // A virtual-key code with the modifier bits, or 0 for unbound.
        const u32 key = bytes == 4 ? byte_load_le32(value) : ~0u;
        if (!plugin_keybind_valid(key)) return MC_E_INVALID;
        if (commit) copy_bytes(at, &key, 4);
        return MC_OK;
    }
    case MC_SET_CHANNEL:
        if (bytes != 8) return MC_E_INVALID;
        if (commit) copy_bytes(at, value, 8);
        return MC_OK;
    case MC_SET_USER_LIST: {
        if (bytes % 8) return MC_E_INVALID;
        if (bytes / 8 > MC_SETTING_ITEM_MAX) return MC_E_CAPACITY;
        for (u32 i = 0; i < bytes; i += 8) {
            u64 id;
            copy_bytes(&id, value + i, 8);
            if (!id) return MC_E_INVALID;
        }
        if (commit) {
            u8* ids = binding.data + binding.text_offsets[index];
            copy_bytes(ids, value, bytes);
            const mc_ids list{reinterpret_cast<const u64*>(ids), bytes / 8, 0};
            copy_bytes(at, &list, sizeof(list));
        }
        return MC_OK;
    }
    default: return MC_E_INVALID; // HEADER holds no value
    }
}

// The bytes write_setting accepts for the value the block holds now. Returns the size.
u32 encode_setting(const PluginBinding& binding, u32 index, u8* output) noexcept {
    const mc_setting_def& setting = setting_defs(binding)[index];
    const u8* at = settings_block(binding) + setting.offset;
    switch (setting.type) {
    case MC_SET_TOGGLE: output[0] = *at; return 1;
    case MC_SET_INT:
    case MC_SET_SLIDER:
    case MC_SET_CHOICE: copy_bytes(output, at, 4); return 4;
    case MC_SET_TEXT:
    case MC_SET_SECRET: {
        mc_str text;
        copy_bytes(&text, at, sizeof(text));
        copy_bytes(output, text.data, text.bytes);
        return text.bytes;
    }
    case MC_SET_COLOUR:
    case MC_SET_KEYBIND: copy_bytes(output, at, 4); return 4;
    case MC_SET_CHANNEL: copy_bytes(output, at, 8); return 8;
    case MC_SET_USER_LIST: {
        mc_ids ids;
        copy_bytes(&ids, at, sizeof(ids));
        if (ids.count) copy_bytes(output, ids.items, ids.count * 8);
        return ids.count * 8;
    }
    default: { // TEXT_LIST
        mc_list list;
        copy_bytes(&list, at, sizeof(list));
        u32 used = 0;
        for (u32 i = 0; i < list.count; ++i) {
            if (i) output[used++] = '\n';
            copy_bytes(output + used, list.items[i].data, list.items[i].bytes);
            used += list.items[i].bytes;
        }
        return used;
    }
    }
}

bool has_value(u32 type) noexcept {
    return type != MC_SET_HEADER;
}

void apply_defaults(PluginBinding& binding) noexcept {
    const mc_setting_def* defs = setting_defs(binding);
    u8 buffer[kListValueBytes + 8];
    for (u32 i = 0; i < setting_total(binding); ++i) {
        const mc_setting_def& setting = defs[i];
        u32 bytes = 0;
        switch (setting.type) {
        case MC_SET_TOGGLE: buffer[0] = static_cast<u8>(setting.default_int); bytes = 1; break;
        case MC_SET_INT:
        case MC_SET_SLIDER:
        case MC_SET_CHOICE:
        case MC_SET_COLOUR:
        case MC_SET_KEYBIND: byte_store_le32(buffer, static_cast<u32>(setting.default_int)); bytes = 4; break;
        case MC_SET_USER_LIST: break;
        case MC_SET_SECRET:
        case MC_SET_TEXT:
            copy_bytes(buffer, setting.default_text.data, setting.default_text.bytes);
            bytes = setting.default_text.bytes;
            break;
        case MC_SET_TEXT_LIST:
            for (u32 item = 0; item < setting.item_count; ++item) {
                if (item) buffer[bytes++] = '\n';
                copy_bytes(buffer + bytes, setting.items[item].data, setting.items[item].bytes);
                bytes += setting.items[item].bytes;
            }
            break;
        default: continue;
        }
        // The loader validated every default, so this cannot refuse one.
        write_setting(binding, i, buffer, bytes, true);
    }
}

// ---- Persistence ------------------------------------------------------------------

bool owner_path(const PluginRuntime& runtime, u64 owner, const wchar_t* leaf, wchar_t (&path)[640]) noexcept {
    u32 length = runtime.data_directory_characters;
    if (!length) return false;
    copy_bytes(path, runtime.data_directory, length * 2);
    path[length] = 0;
    return wide_append(path, 640, &length, L"\\") && wide_append_hex(path, 640, &length, owner) &&
           wide_append(path, 640, &length, L"\\") && wide_append(path, 640, &length, leaf);
}

void mark_settings_dirty(PluginBinding& binding) noexcept {
    if (!binding.settings_dirty) binding.settings_dirty_since = clock_monotonic_milliseconds();
    binding.settings_dirty = true;
}

void load_settings(PluginBinding& binding) noexcept {
    wchar_t path[640];
    PluginKeyValue table;
    if (!setting_total(binding) || !owner_path(*binding.runtime, binding.owner_id, L"settings.bin", path) ||
        !kv_open(&table, MC_SETTING_MAX, kSettingsKeyBytes, kListValueBytes, kSettingsTotalBytes))
        return;
    kv_load(&table, path);
    const mc_setting_def* defs = setting_defs(binding);
    for (u32 i = 0; i < setting_total(binding); ++i) {
        u32 bytes = 0;
        const u8* value = has_value(defs[i].type) ? kv_find(table, defs[i].key, &bytes) : nullptr;
        if (!value) continue;
        u8 plain[kSecretBytes];
        if (defs[i].type == MC_SET_SECRET && bytes) {
            // A secret that cannot be unsealed (another user, another machine) keeps its default.
            if (!plugin_secret_unseal(binding.owner_id, defs[i].key, {value, bytes}, {plain, sizeof(plain)}, &bytes)) continue;
            value = plain;
        }
        // A value the schema no longer accepts keeps its default.
        write_setting(binding, i, value, bytes, true);
        secure_clear_bytes(plain, sizeof(plain));
    }
    kv_close(&table);
}

bool save_settings(PluginBinding& binding) noexcept {
    wchar_t path[640];
    PluginKeyValue table;
    if (!owner_path(*binding.runtime, binding.owner_id, L"settings.bin", path) ||
        !kv_open(&table, MC_SETTING_MAX, kSettingsKeyBytes, kListValueBytes, kSettingsTotalBytes))
        return false;
    u8 buffer[kListValueBytes + 8];
    const mc_setting_def* defs = setting_defs(binding);
    bool ok = true;
    for (u32 i = 0; ok && i < setting_total(binding); ++i) {
        if (!has_value(defs[i].type)) continue;
        u32 bytes = encode_setting(binding, i, buffer);
        if (defs[i].type == MC_SET_SECRET && bytes) {
            u8 sealed[kSecretBytes];
            u32 sealed_bytes = 0;
            ok = plugin_secret_seal(binding.owner_id, defs[i].key, {buffer, bytes}, {sealed, sizeof(sealed)}, &sealed_bytes);
            ok = ok && kv_set(&table, defs[i].key, sealed, sealed_bytes, 0) == MC_OK;
        } else {
            ok = kv_set(&table, defs[i].key, buffer, bytes, 0) == MC_OK;
        }
    }
    secure_clear_bytes(buffer, sizeof(buffer));
    ok = ok && kv_save(&table, path);
    kv_close(&table);
    return ok;
}

// A change reaches disk after the delay, or at once when forced.
void flush(PluginBinding& binding, u64 now_ms, bool force) noexcept {
    if (binding.settings_dirty && (force || now_ms - binding.settings_dirty_since >= kPluginFlushDelayMilliseconds)) {
        if (save_settings(binding)) binding.settings_dirty = false;
        else binding.settings_dirty_since = now_ms; // try again after another delay
    }
    PluginKeyValue& store = binding.store;
    if (store.memory && store.dirty && (force || now_ms - store.dirty_since >= kPluginFlushDelayMilliseconds)) {
        wchar_t path[640];
        if (!owner_path(*binding.runtime, binding.owner_id, L"store.bin", path) || !kv_save(&store, path))
            store.dirty_since = now_ms;
    }
}

// ---- Calls ------------------------------------------------------------------------

void apply_staged(PluginBinding& binding) noexcept {
    for (u32 i = 0; i < binding.staged_count; ++i) {
        const PluginStagedSetting& staged = binding.staged[i];
        if (write_setting(binding, staged.index, binding.staging + staged.offset, staged.bytes, true) == MC_OK)
            mark_settings_dirty(binding);
    }
    binding.staged_count = 0;
    binding.staged_used = 0;
}

// One timed call into the plugin. The plugin sees a context that is valid only until
// finish; settings it changed meanwhile apply then.
struct Call {
    PluginBinding& binding;
    PluginCallTicket ticket;

    Call(PluginBinding& target, PluginCallKind kind, PluginCallClass call_class, u32 flags) noexcept : binding(target) {
        PluginRuntime& runtime = *binding.runtime;
        if (++runtime.call_serial == 0) ++runtime.call_serial;
        binding.ctx.call = runtime.call_serial;
        binding.ctx.flags = flags;
        // A gesture counts for a few seconds; a completion keeps the deadline of the request that began it.
        binding.gesture_until_ms = !(flags & MC_CTX_GESTURE)          ? 0
                                   : binding.inherit_gesture_until_ms ? binding.inherit_gesture_until_ms
                                                                      : clock_monotonic_milliseconds() + kPluginGestureMilliseconds;
        ticket = plugin_call_begin(runtime.ledger, binding.owner_id, kind, call_class);
    }
    PluginCallOutcome finish() noexcept {
        const PluginCallOutcome outcome = plugin_call_finish(binding.runtime->ledger, ticket);
        apply_staged(binding);
        binding.ctx.call = 0;
        binding.ctx.flags = 0;
        binding.gesture_until_ms = 0;
        ++binding.calls;
        binding.last_call_us = outcome.elapsed_us;
        if (outcome.overran) {
            ++binding.strikes;
            const PluginRuntime& runtime = *binding.runtime;
            if (runtime.strike) runtime.strike(runtime.sink_context, binding.owner_id, ticket.kind, binding.strikes, outcome.elapsed_us);
        }
        if (outcome.disable) binding.struck_out = true;
        return outcome;
    }
};

bool start_plugin(PluginBinding& binding, u32 flags) noexcept {
    const auto start = binding.image.descriptor->start;
    if (!start) return true;
    Call call(binding, PluginCallKind::start, PluginCallClass::lifecycle, flags);
    const mc_status status = start(&binding.ctx);
    return !call.finish().overran && status == MC_OK;
}

void stop_plugin(PluginBinding& binding) noexcept {
    const auto stop = binding.image.descriptor->stop;
    if (!stop) return;
    Call call(binding, PluginCallKind::stop, PluginCallClass::lifecycle, 0);
    stop(&binding.ctx);
    call.finish();
}

// ---- Allocation ---------------------------------------------------------------------

u32 reserve_text(PluginBinding& binding, u32 cursor) noexcept {
    const mc_setting_def* defs = setting_defs(binding);
    for (u32 i = 0; i < setting_total(binding); ++i) {
        u32 bytes = 0;
        if (defs[i].type == MC_SET_TEXT || defs[i].type == MC_SET_SECRET) bytes = text_capacity(defs[i]);
        else if (defs[i].type == MC_SET_TEXT_LIST) bytes = kItemTableBytes + MC_SETTING_LIST_BYTES_MAX;
        else if (defs[i].type == MC_SET_USER_LIST) bytes = MC_SETTING_ITEM_MAX * 8;
        binding.text_offsets[i] = cursor;
        cursor += align_up(bytes, 8);
    }
    return cursor;
}

void destroy(PluginBinding* binding) noexcept {
    kv_close(&binding->store);
    if (binding->data) pages_release(binding->data);
    pages_release(binding);
}

bool allocate_data(PluginBinding& binding) noexcept {
    const mc_descriptor& descriptor = *binding.image.descriptor;
    const u32 settings_at = align_up(descriptor.state_bytes, kAlignment);
    const u32 text_at = settings_at + align_up(descriptor.settings_bytes, kAlignment);
    // The text offsets are relative to data, which starts on a page: every state alignment holds.
    const u32 staging_at = align_up(reserve_text(binding, text_at), kAlignment);
    binding.data = commit_pages<u8>(align_up(staging_at + kPluginStagedBytes, kPageBytes));
    if (!binding.data) return false;
    binding.staging = binding.data + staging_at;
    binding.ctx.api = &kApi;
    binding.ctx.state = descriptor.state_bytes ? binding.data : nullptr;
    binding.ctx.settings = descriptor.settings_bytes ? binding.data + settings_at : nullptr;
    return true;
}

PluginEnableResult enable_failed(PluginEnableError error, PluginLoadResult load = {}) noexcept {
    return {error, load, nullptr};
}

using Carry = decltype(PluginRuntime::carry);

void release_carry(PluginRuntime& runtime) noexcept {
    if (runtime.carry.state) pages_release(runtime.carry.state);
    runtime.carry = {};
}

// A reload's state belongs to the next enable, whatever its outcome: it is taken at the start and freed at the end.
struct CarryHold {
    Carry carry;
    explicit CarryHold(PluginRuntime* runtime) noexcept : carry(runtime->carry) { runtime->carry = {}; }
    ~CarryHold() {
        if (carry.state) pages_release(carry.state);
    }
};

} // namespace

namespace plugin_runtime_detail {

PluginKeyValue* store_table(PluginBinding* binding) noexcept {
    PluginKeyValue& store = binding->store;
    if (store.loaded) return store.memory ? &store : nullptr;
    if (!kv_open(&store, MC_STORE_KEYS_MAX, MC_STORE_KEY_MAX, MC_STORE_VALUE_MAX, MC_STORE_TOTAL_MAX)) {
        store.loaded = true; // not retried until the next enable
        return nullptr;
    }
    wchar_t path[640];
    if (owner_path(*binding->runtime, binding->owner_id, L"store.bin", path)) kv_load(&store, path);
    store.loaded = true;
    return &store;
}

} // namespace plugin_runtime_detail

mc_status plugin_binding_set_setting(PluginBinding* binding, u32 index, const void* value, u32 bytes) noexcept {
    if (index >= setting_total(*binding) || (bytes && !value)) return MC_E_INVALID;
    const u8 none = 0;
    const u8* data = bytes ? static_cast<const u8*>(value) : &none;
    const mc_status status = write_setting(*binding, index, data, bytes, false);
    if (status != MC_OK) return status;
    if (!binding->ctx.call) {
        write_setting(*binding, index, data, bytes, true);
        mark_settings_dirty(*binding);
        return MC_OK;
    }
    // The plugin may hold pointers into the setting it is replacing; the new value waits for the call's end.
    if (binding->staged_count == kPluginStagedSettings || bytes > kPluginStagedBytes - binding->staged_used)
        return MC_E_BUDGET;
    copy_bytes(binding->staging + binding->staged_used, data, bytes);
    binding->staged[binding->staged_count++] = {index, binding->staged_used, bytes};
    binding->staged_used += bytes;
    return MC_OK;
}

PluginCallResult plugin_binding_run_draft(PluginBinding* binding, u32 hook_kind, mc_draft_fn function,
                                          mc_draft* draft) noexcept {
    const PluginCallKind kind = hook_kind == MC_HOOK_PRE_EDIT ? PluginCallKind::pre_edit : PluginCallKind::pre_send;
    // The user's send or save is the gesture.
    Call call(*binding, kind, PluginCallClass::pre, MC_CTX_GESTURE);
    const mc_status status = function(&binding->ctx, draft);
    return {status, call.finish().overran};
}

PluginCallResult plugin_binding_call(PluginBinding* binding, PluginCallKind kind, PluginCallClass call_class,
                                     u32 context_flags, PluginInvoke invoke, void* argument) noexcept {
    Call call(*binding, kind, call_class, context_flags);
    const mc_status status = invoke(&binding->ctx, argument);
    return {status, call.finish().overran};
}

namespace {

// The one runtime of the process (plugin_runtime_open); completions from other modules find their plugin here.
PluginRuntime* g_runtime;

struct CompletionArgument {
    mc_completion_fn function;
    const mc_completion* completion;
};
mc_status invoke_completion(mc_ctx* ctx, void* argument) noexcept {
    const auto& call = *static_cast<const CompletionArgument*>(argument);
    call.function(ctx, call.completion);
    return MC_OK;
}

} // namespace

bool plugin_binding_complete(PluginBinding* binding, const PluginCompletion& completion) noexcept {
    const mc_descriptor& descriptor = *binding->image.descriptor;
    mc_completion_fn function = nullptr;
    for (u32 i = 0; i < descriptor.hook_count; ++i)
        if (descriptor.hooks[i].kind == MC_HOOK_COMPLETION) function = descriptor.hooks[i].fn.completion;
    if (!function || binding->struck_out) return false;
    mc_completion output{};
    output.tag = completion.tag;
    switch (completion.kind) {
    case PluginCompletionKind::http:
        output.kind = MC_COMPLETION_HTTP;
        output.error = completion.http_error;
        output.status = completion.http_status;
        output.body = completion.body.count ? completion.body.data : nullptr;
        output.body_bytes = completion.body.count;
        break;
    case PluginCompletionKind::banner:
        output.kind = MC_COMPLETION_BANNER;
        output.result = completion.which;
        break;
    case PluginCompletionKind::modal:
        output.kind = MC_COMPLETION_MODAL;
        output.result = completion.accepted;
        output.toggle = completion.toggle;
        output.select_index = completion.select_index;
        output.text = {completion.text.data, completion.text.bytes, 0};
        break;
    default: return false;
    }
    // A user gesture begins a window of its own; an http result continues the window of its request.
    const u32 flags = completion.gesture || completion.inherited_gesture_until_ms > clock_monotonic_milliseconds()
                          ? MC_CTX_GESTURE : 0u;
    binding->inherit_gesture_until_ms = completion.gesture ? 0 : completion.inherited_gesture_until_ms;
    CompletionArgument argument{function, &output};
    const PluginCallResult ran = plugin_binding_call(binding, PluginCallKind::event, PluginCallClass::event, flags,
                                                     invoke_completion, &argument);
    binding->inherit_gesture_until_ms = 0;
    return !ran.overran;
}

bool plugin_dispatch_completion(u64 owner_id, const PluginCompletion& completion) noexcept {
    PluginRuntime* runtime = g_runtime;
    if (!runtime) return false;
    for (u32 i = 0; i < runtime->binding_count; ++i)
        if (runtime->bindings[i]->owner_id == owner_id) return plugin_binding_complete(runtime->bindings[i], completion);
    return false;
}

namespace {

mc_setting_fn setting_hook(const PluginBinding& binding, u32 kind) noexcept {
    const mc_descriptor& descriptor = *binding.image.descriptor;
    for (u32 i = 0; i < descriptor.hook_count; ++i)
        if (descriptor.hooks[i].kind == kind) return descriptor.hooks[i].fn.setting;
    return nullptr;
}

struct SettingArgument {
    mc_setting_fn function;
    u32 index;
};
mc_status invoke_setting(mc_ctx* ctx, void* argument) noexcept {
    const auto& call = *static_cast<const SettingArgument*>(argument);
    call.function(ctx, call.index);
    return MC_OK;
}

} // namespace

mc_status plugin_binding_change_setting(PluginBinding* binding, u32 index, const void* value, u32 bytes) noexcept {
    if (binding->ctx.call) return MC_E_BUSY;
    const mc_status status = plugin_binding_set_setting(binding, index, value, bytes);
    const mc_setting_fn hook = status == MC_OK && !binding->struck_out ? setting_hook(*binding, MC_HOOK_SETTING_CHANGED)
                                                                     : nullptr;
    if (hook) {
        SettingArgument argument{hook, index};
        plugin_binding_call(binding, PluginCallKind::settings, PluginCallClass::event, MC_CTX_GESTURE, invoke_setting,
                            &argument);
    }
    return status;
}

bool plugin_dispatch_keybind(u32 key) noexcept {
    PluginRuntime* runtime = g_runtime;
    if (!key || !runtime) return false;
    bool handled = false;
    for (u32 b = 0; b < runtime->binding_count; ++b) {
        PluginBinding* binding = runtime->bindings[b];
        const mc_descriptor& descriptor = *binding->image.descriptor;
        const mc_setting_fn hook = binding->struck_out ? nullptr : setting_hook(*binding, MC_HOOK_KEYBIND);
        if (!hook || binding->ctx.call) continue;
        for (u32 i = 0; i < descriptor.setting_count; ++i) {
            const mc_setting_def& setting = descriptor.settings[i];
            u32 bound = 0;
            if (setting.type == MC_SET_KEYBIND) copy_bytes(&bound, settings_block(*binding) + setting.offset, 4);
            if (bound != key) continue;
            SettingArgument argument{hook, i};
            plugin_binding_call(binding, PluginCallKind::keybind, PluginCallClass::gesture, MC_CTX_GESTURE,
                                invoke_setting, &argument);
            handled = true;
        }
    }
    return handled;
}

bool plugin_binding_set_host(PluginBinding* binding, u32 index, bool approved) noexcept {
    if (index >= binding->image.descriptor->network_count) return false;
    const bool was = binding->approved_hosts & (1u << index);
    if (was == approved) return true;
    binding->approved_hosts ^= 1u << index;
    // Requests still waiting for a host the user just took away do not run.
    if (!approved) plugin_http_cancel(binding->runtime, binding, false);
    return true;
}

PluginLoadResult plugin_runtime_open(PluginRuntime* runtime, ConstSlice<wchar_t> plugins_directory,
                                     ConstSlice<wchar_t> data_directory, PluginCallLedger* ledger) noexcept {
    if (!runtime || !data_directory.data || !data_directory.count || data_directory.count > 480)
        return {PluginLoadError::invalid_argument, PluginDescriptorProblem::none, 0, 0};
    const PluginLoadResult pinned = plugin_directory_open(&runtime->directory, plugins_directory);
    if (!pinned.ok()) return pinned;
    runtime->ledger = ledger;
    g_runtime = runtime;
    copy_bytes(runtime->data_directory, data_directory.data, data_directory.count * 2);
    runtime->data_directory[data_directory.count] = 0;
    runtime->data_directory_characters = data_directory.count;
    return {};
}

PluginLoadResult plugin_runtime_close(PluginRuntime* runtime) noexcept {
    if (!runtime) return {PluginLoadError::invalid_argument, PluginDescriptorProblem::none, 0, 0};
    while (runtime->binding_count) {
        const u32 before = runtime->binding_count;
        plugin_runtime_disable(runtime, runtime->bindings[before - 1]);
        if (runtime->binding_count == before) return {PluginLoadError::release, PluginDescriptorProblem::none, 0, 0};
    }
    release_carry(*runtime);
    plugin_http_close(runtime);
    if (g_runtime == runtime) g_runtime = nullptr;
    return plugin_directory_close(&runtime->directory);
}

PluginEnableResult plugin_runtime_enable(PluginRuntime* runtime, ConstSlice<wchar_t> leaf, u64 approved_permissions,
                                         u32 approved_hosts, const u8* sha256) noexcept {
    if (!runtime || !runtime->directory.ancestor_count) return enable_failed(PluginEnableError::invalid_argument);
    const CarryHold hold(runtime);
    const Carry& carry = hold.carry;
    if (runtime->binding_count == kPluginMaximumBindings) return enable_failed(PluginEnableError::capacity);
    auto* binding = commit_pages<PluginBinding>(align_up(sizeof(PluginBinding), kPageBytes));
    if (!binding) return enable_failed(PluginEnableError::allocation);
    binding->runtime = runtime;
    const PluginLoadResult loaded = plugin_image_load(&binding->image, runtime->directory, leaf, approved_permissions, sha256);
    if (!loaded.ok()) {
        // A DLL that could not be freed stays owned by the image, so the record is kept for it.
        if (!binding->image.module) pages_release(binding);
        return enable_failed(loaded.error == PluginLoadError::release ? PluginEnableError::release
                                                                       : PluginEnableError::load, loaded);
    }
    binding->owner_id = binding->image.owner_id;
    binding->permissions = binding->image.permissions;
    // Only the hosts the descriptor declares can be approved (the loader limited them to MC_NETWORK_HOSTS_MAX).
    binding->approved_hosts = approved_hosts & ((1u << binding->image.descriptor->network_count) - 1);
    if (++runtime->generation_serial == 0) ++runtime->generation_serial;
    binding->generation = runtime->generation_serial;
    for (u32 i = 0; i < runtime->binding_count; ++i) {
        if (runtime->bindings[i]->owner_id != binding->owner_id) continue;
        const bool freed = plugin_image_unload(&binding->image).ok();
        if (freed) pages_release(binding);
        return enable_failed(freed ? PluginEnableError::duplicate_owner : PluginEnableError::release);
    }
    plugin_call_ledger_forget(runtime->ledger, binding->owner_id);
    if (!allocate_data(*binding)) {
        const bool freed = plugin_image_unload(&binding->image).ok();
        destroy(binding);
        return enable_failed(freed ? PluginEnableError::allocation : PluginEnableError::release);
    }
    apply_defaults(*binding);
    load_settings(*binding);
    // A reload hands the replaced build's state over when the layout is the one the new descriptor declares.
    u32 start_flags = MC_CTX_FIRST_START;
    PluginStateCarry carried = PluginStateCarry::none;
    if (carry.owner_id == binding->owner_id) {
        const mc_descriptor& d = *binding->image.descriptor;
        carried = PluginStateCarry::reset;
        if (d.state_layout == carry.layout && d.state_bytes == carry.bytes && d.state_align == carry.alignment) {
            if (carry.bytes) copy_bytes(binding->data, carry.state, carry.bytes);
            carried = PluginStateCarry::kept;
            start_flags = MC_CTX_RELOADED;
        }
    }
    runtime->bindings[runtime->binding_count++] = binding;
    PluginEnableError failure = PluginEnableError::none;
    if (!start_plugin(*binding, start_flags)) {
        failure = PluginEnableError::start_failed;
    } else {
        binding->started = true;
        binding->hooks_published = plugin_dispatch_add(binding);
        if (!binding->hooks_published) {
            failure = PluginEnableError::allocation;
        } else {
            plugin_ui_add(binding);
            if (!plugin_surfaces_add(binding)) failure = PluginEnableError::allocation;
            plugin_pipeline_add(binding);
        }
    }
    if (failure != PluginEnableError::none) {
        // Undo everything, exactly as a disable does.
        return enable_failed(plugin_runtime_disable(runtime, binding) ? failure : PluginEnableError::release);
    }
    return {PluginEnableError::none, {}, binding, carried};
}

bool plugin_runtime_disable(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    if (!runtime || !binding) return false;
    u32 slot = 0;
    while (slot < runtime->binding_count && runtime->bindings[slot] != binding) ++slot;
    if (slot == runtime->binding_count) return false;
    if (binding->hooks_published) {
        plugin_dispatch_remove(binding);
        plugin_ui_remove(binding);
        binding->hooks_published = false;
    }
    if (binding->started) {
        stop_plugin(*binding);
        binding->started = false;
    }
    plugin_pipeline_remove(binding);
    // After stop, which may still push; also when the plugin never published hooks (a start that failed).
    plugin_surfaces_remove(binding);
    plugin_http_cancel(runtime, binding, true);
    flush(*binding, clock_monotonic_milliseconds(), true);
    kv_close(&binding->store);
    if (binding->image.module && !plugin_image_unload(&binding->image).ok()) return false;
    for (u32 i = slot + 1; i < runtime->binding_count; ++i) runtime->bindings[i - 1] = runtime->bindings[i];
    runtime->bindings[--runtime->binding_count] = nullptr;
    destroy(binding);
    return true;
}

bool plugin_runtime_keep_state(PluginRuntime* runtime, const PluginBinding* binding) noexcept {
    if (!runtime || !binding) return false;
    release_carry(*runtime);
    const mc_descriptor& d = *binding->image.descriptor;
    u8* copy = nullptr;
    if (d.state_bytes) {
        copy = commit_pages<u8>(d.state_bytes);
        if (!copy) return false;
        copy_bytes(copy, binding->data, d.state_bytes);
    }
    runtime->carry = {binding->owner_id, d.state_layout, d.state_bytes, d.state_align, copy};
    return true;
}

u64 plugin_runtime_poll(PluginRuntime* runtime, u64 now_ms) noexcept {
    if (!runtime) return 0;
    for (u32 i = 0; i < runtime->binding_count; ++i) flush(*runtime->bindings[i], now_ms, false);
    for (u32 i = 0; i < runtime->binding_count; ++i) {
        PluginBinding* binding = runtime->bindings[i];
        if (!binding->struck_out && !plugin_call_ledger_disable_requested(runtime->ledger, binding->owner_id)) continue;
        const u64 owner = binding->owner_id;
        if (!plugin_runtime_disable(runtime, binding)) continue;
        plugin_call_ledger_forget(runtime->ledger, owner);
        return owner;
    }
    return 0;
}

} // namespace maicord
