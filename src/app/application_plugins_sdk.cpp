#include "application_plugins.hpp"
#include "application_plugins_runtime.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/log.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/plugin_slots.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {

ApplicationPluginsSdk* owner() noexcept { return g_app.plugins ? g_app.plugins->sdk : nullptr; }
NativePluginSdkRuntime* runtime() noexcept {
    auto* sdk = owner();
    return sdk ? &sdk->runtime : nullptr;
}
u64 monotonic() noexcept { return clock_monotonic_milliseconds(); }
// Turns a plugin off after a call returned over budget: unload it and keep it
// off across restarts until the user enables it again.
void turn_off(ApplicationPlugins& plugins, u64 owner_id, u32 flag) noexcept {
    for (u32 i = 0; i < 8; ++i) {
        auto& module = plugins.modules[i];
        if (!module.enabled || module.owner_id != owner_id) continue;
        char name[96];
        const auto label = application_plugins_label(i);
        const u32 name_bytes = minimum(label.bytes, static_cast<u32>(sizeof(name)));
        copy_bytes(name, label.data, name_bytes);
        NativePluginPreferences flagged = plugins.preferences;
        if (native_plugin_preferences_set_flags(&flagged, owner_id, flag) &&
            !application_plugins_save_choices(plugins, flagged))
            plugins.preferences_dirty = true;
        plugins.activating = true;
        const auto unloaded = native_plugin_module_unload(&module);
        plugins.activating = false;
        if (unloaded.ok()) {
            plugins.label_bytes[i] = 0;
            plugins.loaded_identities[i] = {};
            plugins.loaded_identity_generations[i] = 0;
        } else {
            log_u64(LogLevel::warning, "plugins", "budget_unload_error", static_cast<u64>(unloaded.error));
        }
        application_plugins_set_notice(plugins, {name, name_bytes},
            utf8_literal(" was turned off because it kept MaiCord waiting. Turn it on again from the list."));
        log_u64(LogLevel::warning, "plugins", "budget_disabled_owner", owner_id);
        application_settings_refresh_pages();
        return;
    }
}
u32 slot_query(u32 surface, u64 channel_id, u64 message_id, u64 user_id, u64 guild_id,
               Slice<UiPluginSlotNode> output) noexcept {
    const auto* rt = runtime();
    if (!rt || !output.data) return 0;
    NativePluginSdkSlotNode nodes[16]{};
    const u32 count = native_plugin_sdk_slots(*rt, {surface, channel_id, message_id, user_id, guild_id},
                                             {nodes, minimum(output.count, 16u)});
    for (u32 i = 0; i < count; ++i)
        output[i] = {nodes[i].owner_id, nodes[i].action_id, nodes[i].text, nodes[i].tooltip, nodes[i].icon,
                     nodes[i].kind, nodes[i].tone, nodes[i].value};
    return count;
}
bool edit_valid(TextFormOwnerKey key) noexcept {
    auto* sdk = owner();
    NativePluginSdkEdit edit{};
    return sdk && key.scope == sdk->edit_serial && key.collection == sdk->edit_page.owner_id && key.item == sdk->edit_control &&
           native_plugin_sdk_settings_edit(sdk->runtime, sdk->edit_page, sdk->edit_control, &edit);
}
bool edit_save(TextFormOwnerKey key, Utf8View text) noexcept {
    auto* sdk = owner();
    if (!edit_valid(key)) return false;
    const auto result = native_plugin_sdk_settings_submit(&sdk->runtime, sdk->edit_page, sdk->edit_control, text);
    application_settings_refresh_pages();
    return result == NativePluginError::none;
}
} // namespace

bool application_plugins_sdk_open(ApplicationPlugins& plugins, PluginCallMarker* leftover) noexcept {
    if (leftover) *leftover = {};
    if (plugins.sdk) return true;
    auto* sdk = static_cast<ApplicationPluginsSdk*>(
        VirtualAlloc(nullptr, sizeof(ApplicationPluginsSdk), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!sdk) return false;
    sdk->runtime.ledger = &sdk->ledger;
    application_plugins_read_install(sdk->runtime);
    application_plugins_actions_install(sdk->runtime);
    sdk->runtime.unix_ms = clock_unix_milliseconds;
    sdk->runtime.clock_ms = monotonic;
    // %LOCALAPPDATA%\MaiCord\plugin-data, leaving room for each plugin's
    // "\<owner hex>\store.bin" below it.
    sdk->runtime.data_directory_characters = data_path(sdk->runtime.data_directory, 512 - 40, L"plugin-data").length;
    // Only read a marker here; the marker file exists only while plugins run.
    wchar_t marker[1024];
    if (plugin_call_ledger_default_path(marker)) plugin_call_ledger_peek(marker, leftover);
    plugins.sdk = sdk;
    g_ui_plugin_slot_query = slot_query;
    return true;
}

void application_plugins_sdk_close(ApplicationPlugins& plugins) noexcept {
    auto* sdk = plugins.sdk;
    if (!sdk) return;
    native_plugin_sdk_flush(&sdk->runtime);
    if (auto* host = application_settings_plugin_host_current(); host && host->sdk == &sdk->runtime) {
        host->sdk = nullptr;
        host->ledger = nullptr;
    }
    plugin_call_ledger_close(&sdk->ledger);
    g_ui_plugin_slot_query = nullptr;
    plugins.sdk = nullptr;
    VirtualFree(sdk, 0, MEM_RELEASE);
}

void application_plugins_sdk_bind_host(ApplicationPlugins& plugins, NativePluginHost* host) noexcept {
    if (!host || !plugins.sdk) return;
    if (!plugins.sdk->ledger_open) plugins.sdk->ledger_open = plugin_call_ledger_open_default(&plugins.sdk->ledger, nullptr);
    host->sdk = &plugins.sdk->runtime;
    host->ledger = &plugins.sdk->ledger;
}

void application_plugins_set_notice(ApplicationPlugins& plugins, Utf8View first, Utf8View second, Utf8View third) noexcept {
    auto* sdk = plugins.sdk;
    if (!sdk) return;
    u32 at = 0;
    const Utf8View parts[]{first, second, third};
    for (auto part : parts)
        at += utf8_copy_truncated(sdk->notice + at, static_cast<u32>(sizeof(sdk->notice)) - at, part);
    plugins.status = {sdk->notice, at};
}

void application_plugins_sdk_poll(ApplicationPlugins& plugins) noexcept {
    auto* sdk = plugins.sdk;
    if (!sdk) return;
    if (!application_plugins_busy())
        while (const u64 owner_id = plugin_call_ledger_take_disable(&sdk->ledger))
            turn_off(plugins, owner_id, native_plugin_preference_overran);
    if (!g_app.plugins || g_app.plugins->sdk != sdk) return;
    const auto polled = native_plugin_sdk_poll(&sdk->runtime);
    sdk->next_timer = polled.next_timer_ms;
    sdk->events_pending = polled.events_pending;
    if (sdk->slot_revision != sdk->runtime.slot_revision) {
        sdk->slot_revision = sdk->runtime.slot_revision;
        application_plugins_surfaces_changed();
        application_request_frame();
    }
    application_plugins_fx_poll(*sdk);
    application_plugins_popover_poll();
    if (sdk->composer_notice_until && monotonic() >= sdk->composer_notice_until) {
        sdk->composer_notice_until = 0;
        sdk->composer_notice_bytes = 0;
        if (g_app.account_ui && sdk->composer_refresh) sdk->composer_refresh();
    }
}

bool application_plugins_command_pending() noexcept {
    const auto* rt = runtime();
    return rt && native_plugin_sdk_command_matches(*rt, message_composer_text(g_app.message_composer));
}

u32 application_plugins_commands(Utf8View prefix, Slice<NativePluginSdkCommandView> output) noexcept {
    const auto* rt = runtime();
    return rt ? native_plugin_sdk_commands(*rt, prefix, output) : 0;
}

Utf8View application_plugins_composer_notice() noexcept {
    const auto* sdk = owner();
    return sdk && sdk->composer_notice_bytes ? Utf8View{sdk->composer_notice, sdk->composer_notice_bytes} : Utf8View{};
}

u32 application_plugins_wait_timeout(u64 now_ms) noexcept {
    const auto* sdk = owner();
    if (!sdk) return INFINITE;
    if (sdk->events_pending) return 0;
    u64 due = sdk->composer_notice_until;
    const u64 timer = sdk->next_timer;
    if (timer && (!due || timer < due)) due = timer;
    if (!due) return INFINITE;
    return due <= now_ms ? 0u : static_cast<u32>(minimum<u64>(due - now_ms, INFINITE - 1u));
}

bool application_plugins_settings_edit(UiPageKey page, u64 control_id) noexcept {
    auto* sdk = owner();
    NativePluginSdkEdit edit{};
    if (!sdk || !native_plugin_sdk_settings_edit(sdk->runtime, page, control_id, &edit)) return false;
    sdk->edit_page = page;
    sdk->edit_control = control_id;
    sdk->edit_serial = g_app.plugin_account_scope_serial;
    ApplicationTextFormRequest request{};
    request.title = edit.label;
    request.context = edit.kind == MC_SETTING_COLOUR ? utf8_literal("Enter a colour such as #5865F2.") :
                      edit.kind == MC_SETTING_KEYBIND ? utf8_literal("Enter a shortcut such as Ctrl+Shift+K.") :
                      Utf8View{};
    request.label = edit.label;
    request.accept = utf8_literal("Save");
    request.initial = edit.value;
    request.owner = {sdk->edit_serial, page.owner_id, control_id};
    request.valid = edit_valid;
    request.save = edit_save;
    request.maximum_bytes = maximum(edit.max_bytes, 1u);
    request.single_line = true;
    if (edit.kind == MC_SETTING_COLOUR) {
        request.placeholder = utf8_literal("#RRGGBB");
        request.check = application_text_form_color_check;
        request.rejected = utf8_literal("That isn't a hex colour. Use #RGB or #RRGGBB.");
        request.swatch = true;
    }
    return application_text_form_open(request);
}

bool application_plugins_slots_stale() noexcept {
    const auto* sdk = owner();
    return sdk && sdk->rendered_slot_revision != sdk->runtime.slot_revision;
}
void application_plugins_slots_rendered() noexcept {
    if (auto* sdk = owner()) sdk->rendered_slot_revision = sdk->runtime.slot_revision;
}

u64 application_plugins_slot_revision() noexcept {
    const auto* rt = runtime();
    return rt ? rt->slot_revision : 0;
}

bool application_plugins_slot_action(u64 owner_id, NativePluginSdkSlotTarget target, u64 action_id) noexcept {
    auto* rt = runtime();
    return rt && !application_plugins_busy() && native_plugin_sdk_slot_action(rt, owner_id, target, action_id);
}

} // namespace maicord::app_detail
