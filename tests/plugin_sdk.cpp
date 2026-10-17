#include "maicord/app/plugin_module_loader.hpp"
#include "maicord/app/plugin_settings_projection.hpp"
#include "maicord/app/plugin_preferences.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/icon.hpp"
#include "../src/app/application_plugin_settings_projection.hpp"
#include "test_output.hpp"

namespace maicord::plugin_sdk_tests {
u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL plugin_sdk: ");
    test_write_line(label);
}
bool starts_with(Utf8View text, Utf8View prefix) noexcept {
    return text.bytes >= prefix.bytes && bytes_equal(text.data, prefix.data, prefix.bytes);
}
bool contains(Utf8View text, Utf8View part) noexcept {
    for (u32 i = 0; i + part.bytes <= text.bytes; ++i)
        if (bytes_equal(text.data + i, part.data, part.bytes)) return true;
    return false;
}

NativePluginSdkRuntime g_runtime;
PluginCallLedger g_ledger;
u64 g_clock = 1000;
u64 test_clock() noexcept { return g_clock; }
wchar_t g_directory[MAX_PATH];
u32 g_directory_characters;

struct HostFixture {
    UiPage page_bank[16];
    UiPageRegistry pages;
    NativePluginSlot slots[8];
    NativePluginHost host;
};
HostFixture g_host;
bool initialize_host() noexcept {
    g_host = {};
    const bool ready = ui_page_registry_initialize(&g_host.pages, {g_host.page_bank, 16}) &&
        native_plugin_host_initialize(&g_host.host, &g_host.pages, {g_host.slots, 8},
            {native_plugin_known_capabilities, 4, 64 * 1024 + kPluginSdkStateHeaderBytes, 256 * 1024, nullptr, nullptr, 1});
    g_host.host.sdk = &g_runtime;
    g_host.host.ledger = &g_ledger;
    return ready;
}
bool sibling(const wchar_t* leaf, wchar_t (&output)[1024], u32* characters) noexcept {
    const DWORD count = GetModuleFileNameW(nullptr, output, 1024);
    if (!count || count >= 1024) return false;
    u32 at = count;
    while (at && output[at - 1] != L'\\') --at;
    u32 length = 0;
    while (leaf[length]) ++length;
    if (at + length >= 1024) return false;
    for (u32 i = 0; i <= length; ++i) output[at + i] = leaf[i];
    *characters = at + length;
    return true;
}

// ---- In-process plugin for interface-level tests ---------------------------------

struct Inproc {
    u64 owner, permissions;
    u32 definition_size;
    void* binding;
    u64 generation;
    mc_status (MC_CALL* get)(void*, uint64_t, const char*, uint32_t, const void**);
    void (*during_start)() noexcept;
    u32 stops;
};
Inproc g_inproc;
alignas(64) u8 g_inproc_state[4096];
mc_status MC_CALL inproc_start(const mc_host* host, void*, uint32_t) {
    g_inproc.binding = host->binding;
    g_inproc.generation = host->generation;
    g_inproc.get = host->get_interface;
    if (g_inproc.during_start) g_inproc.during_start();
    return MC_OK;
}
void MC_CALL inproc_stop(void*, uint32_t) { ++g_inproc.stops; }
mc_status MC_CALL inproc_query(const mc_host_info* info, mc_plugin_definition* definition) {
    if (!info || info->abi_version < 5) return MC_E_UNSUPPORTED;
    struct {
        mc_plugin_definition base;
        u64 future[4];
    } local{};
    local.base.size = g_inproc.definition_size ? g_inproc.definition_size : static_cast<u32>(sizeof(local.base));
    local.base.abi_version = MC_ABI_VERSION;
    local.base.owner_id = g_inproc.owner;
    local.base.permissions = g_inproc.permissions;
    local.base.state_bytes = 256;
    local.base.state_alignment = 16;
    local.base.start = inproc_start;
    local.base.stop = inproc_stop;
    if (definition->size < local.base.size) {
        copy_bytes(definition, &local, definition->size);
        return MC_OK;
    }
    copy_bytes(definition, &local, local.base.size);
    return MC_OK;
}
struct InprocPlugin {
    NativePluginSdkBinding* binding;
    bool enabled;
    u64 owner;
};
InprocPlugin start_inproc(u64 owner, u64 permissions, u64 approved, void (*during_start)() noexcept = nullptr) noexcept {
    g_inproc.owner = owner;
    g_inproc.permissions = permissions;
    g_inproc.during_start = during_start;
    g_inproc.binding = nullptr;
    InprocPlugin plugin{native_plugin_sdk_create(&g_runtime, approved), false, owner};
    NativePluginDefinition adapter{};
    if (!plugin.binding || native_plugin_sdk_query(plugin.binding, inproc_query, owner, &adapter) != NativePluginSdkQueryError::none)
        return plugin;
    plugin.enabled = adapter.state_bytes <= sizeof(g_inproc_state) &&
        native_plugin_sdk_enable(plugin.binding, &g_host.host, adapter, {g_inproc_state, adapter.state_bytes}).ok();
    return plugin;
}
void stop_inproc(InprocPlugin& plugin) noexcept {
    if (plugin.enabled) native_plugin_host_disable(&g_host.host, plugin.owner);
    native_plugin_sdk_destroy(plugin.binding);
    plugin = {};
}
template <typename T> const T* table(const char* name, u32 version) noexcept {
    const void* found = nullptr;
    return g_inproc.get && g_inproc.get(g_inproc.binding, g_inproc.generation, name, version, &found) == MC_OK ?
        static_cast<const T*>(found) : nullptr;
}
mc_status lookup(const char* name, u32 version) noexcept {
    const void* found = nullptr;
    return g_inproc.get(g_inproc.binding, g_inproc.generation, name, version, &found);
}

void test_interfaces() noexcept {
    auto plugin = start_inproc(0x1001, MC_PERM_STORE | MC_PERM_TIMER, MC_PERM_ALL);
    check(plugin.enabled && g_inproc.binding, "in-process ABI-4 plugin starts through the ABI-3 host slot");
    const auto* store = table<mc_store_v1>("mc.store", 1);
    check(store && store->size == sizeof(mc_store_v1) && store->version == 1 && MC_HAS_FIELD(store, mc_store_v1, usage),
          "interface tables carry their size and version");
    check(lookup("mc.store", 2) == MC_E_UNSUPPORTED && lookup("mc.store", 0) == MC_E_UNSUPPORTED &&
              lookup("mc.nothing", 1) == MC_E_UNSUPPORTED,
          "unknown names and other versions are unsupported");
    check(lookup("mc.cmd", 1) == MC_E_DENIED && lookup("mc.ui.slots", 2) == MC_E_DENIED && lookup("mc.timer", 1) == MC_OK,
          "interfaces outside the definition's permissions are denied");
    const void* found = nullptr;
    check(g_inproc.get(g_inproc.binding, g_inproc.generation + 1, "mc.store", 1, &found) == MC_E_INACTIVE && !found,
          "a stale generation is refused");
    const auto generation = g_inproc.generation;
    const auto* timer = table<mc_timer_v1>("mc.timer", 1);
    check(timer && timer->start(g_inproc.binding, generation, 1, 100, 0) == MC_E_INVALID,
          "timers below the minimum interval are refused");
    stop_inproc(plugin);
    check(g_inproc.stops == 1 && g_inproc.get(g_inproc.binding, generation, "mc.store", 1, &found) == MC_E_INACTIVE,
          "a disabled binding refuses every call and stop ran once");

    auto denied = start_inproc(0x1002, MC_PERM_STORE | MC_PERM_COMMANDS, MC_PERM_STORE);
    check(!denied.enabled && !g_inproc.binding, "a definition asking beyond the approved permissions never starts");
    stop_inproc(denied);
}

// ---- Size-prefix growth -------------------------------------------------------------

bool g_page_ok, g_old_option_ok, g_menu_targets_checked, g_late_page;
void register_old_structs() noexcept {
    const auto* pages = table<mc_pages_v1>("mc.ui.pages", 1);
    const auto* settings = table<mc_settings_v3>("mc.settings", 3);
    const auto* commands = table<mc_cmd_v1>("mc.cmd", 1);
    const auto* menus = table<mc_menus_v2>("mc.ui.menus", 2);
    g_page_ok = pages && settings && pages->register_page(g_inproc.binding, g_inproc.generation, 1, {"Old", 3}) == MC_OK;
    // An older SDK: settings end before max_bytes; stride matches that size.
    constexpr u32 old_size = offsetof(mc_setting, max_bytes);
    u8 controls[2 * old_size]{};
    mc_setting first{};
    first.size = old_size;
    first.kind = MC_SETTING_TOGGLE;
    first.id = 1;
    first.label = {"Old toggle", 10};
    first.enabled = 1;
    first.checked = 1;
    mc_setting second = first;
    second.kind = MC_SETTING_TEXT;
    second.id = 2;
    second.label = {"Old text", 8};
    second.text = {"value", 5};
    copy_bytes(controls, &first, old_size);
    copy_bytes(controls + old_size, &second, old_size);
    mc_settings_page page{sizeof(mc_settings_page), 2, {"Old page", 8}, {}, reinterpret_cast<const mc_setting*>(controls),
                          old_size, 0};
    g_page_ok = g_page_ok && settings->set_page(g_inproc.binding, g_inproc.generation, 1, &page) == MC_OK;
    // A newer SDK: wider elements; the host reads the prefix it knows.
    struct Newer {
        mc_setting base;
        u64 future[2];
    } newer[1]{};
    newer[0].base = first;
    newer[0].base.size = sizeof(Newer);
    newer[0].future[0] = ~0ull;
    page.control_count = 1;
    page.controls = &newer[0].base;
    page.control_stride = sizeof(Newer);
    g_page_ok = g_page_ok && settings->set_page(g_inproc.binding, g_inproc.generation, 1, &page) == MC_OK;
    page.control_stride = 8;
    g_page_ok = g_page_ok && settings->set_page(g_inproc.binding, g_inproc.generation, 1, &page) == MC_E_INVALID;
    // Restore the two-control old page for the projection checks.
    page.control_count = 2;
    page.controls = reinterpret_cast<const mc_setting*>(controls);
    page.control_stride = old_size;
    g_page_ok = g_page_ok && settings->set_page(g_inproc.binding, g_inproc.generation, 1, &page) == MC_OK;
    constexpr u32 option_size = offsetof(mc_command_option, required);
    mc_command_option option{};
    option.size = option_size;
    option.type = MC_OPTION_STRING;
    option.name = {"word", 4};
    option.required = 1; // past the reported size: must read as zero
    mc_command command{};
    command.size = sizeof(command);
    command.command_id = 1;
    command.name = {"old", 3};
    command.option_count = 1;
    command.options = &option;
    command.option_stride = option_size;
    g_old_option_ok = commands && commands->register_command(g_inproc.binding, g_inproc.generation, &command) == MC_OK;
    g_menu_targets_checked = menus &&
        menus->register_item(g_inproc.binding, g_inproc.generation, 1, MC_MENU_TARGET_COUNT, {"x", 1}, 0) == MC_E_INVALID &&
        menus->register_item(g_inproc.binding, g_inproc.generation, 1, MC_MENU_USER, {"x", 1}, 8) == MC_E_INVALID &&
        menus->register_item(g_inproc.binding, g_inproc.generation, 2, MC_MENU_USER, {"Friend", 6}, 0) == MC_OK &&
        menus->register_item(g_inproc.binding, g_inproc.generation, 3, MC_MENU_EMOJI, {"Copy emoji", 10}, 0) == MC_OK &&
        menus->register_item(g_inproc.binding, g_inproc.generation, 3, MC_MENU_LINK, {"Dup", 3}, 0) == MC_E_INVALID;
}
bool read_context_source(void*, mc_context_info* info) noexcept {
    info->channel_id = 77;
    info->guild_id = 88;
    info->user_id = 99;
    info->channel_name_bytes = 7;
    copy_bytes(info->channel_name, "general", 7);
    info->user_name_bytes = 6;
    copy_bytes(info->user_name, "tester", 6);
    return true;
}

void test_size_prefix() noexcept {
    g_runtime.read = {};
    g_runtime.read.context_info = read_context_source;
    auto plugin = start_inproc(0x2001, MC_PERM_SETTINGS | MC_PERM_COMMANDS | MC_PERM_MENUS | MC_PERM_READ_CONTEXT,
                               MC_PERM_ALL, register_old_structs);
    check(plugin.enabled && g_page_ok, "older and newer mc_setting sizes and strides are read by prefix; tiny strides fail");
    check(g_old_option_ok, "an older mc_command_option registers with unknown fields read as zero");
    check(g_menu_targets_checked, "menu items validate target, flags and duplicate ids");
    const auto* descriptor = ui_page_registry_find(g_host.pages, {0x2001, 1});
    NativePluginSettingsSnapshot snapshot{};
    UiSettingsControl controls[4]{};
    UiSettingsPageContent content{};
    check(descriptor &&
              native_plugin_settings_snapshot(descriptor->context, &snapshot) == NativePluginSettingsError::none &&
              native_plugin_settings_project(snapshot, {controls, 4}, &content) == NativePluginSettingsError::none &&
              content.controls.count == 2 && controls[0].kind == UiSettingsControlKind::toggle && controls[0].checked &&
              controls[1].kind == UiSettingsControlKind::action && utf8_equal(controls[1].description, utf8_literal("value")),
          "an old-size page projects; a missing max_bytes falls back to the default");
    NativePluginSdkEdit edit{};
    check(native_plugin_sdk_settings_edit(g_runtime, {0x2001, 2}, 2, &edit) == false &&
              native_plugin_sdk_settings_edit(g_runtime, {0x2001, 1}, 2, &edit) && edit.max_bytes == 256,
          "edit lookup resolves only registered pages and text controls");
    const auto* read = table<mc_read_v2>("mc.read", 2);
    struct OldContext {
        u32 size, flags;
        u64 channel_id, guild_id;
        u64 sentinel;
    } old{};
    old.size = offsetof(mc_context_info, user_id);
    old.sentinel = 0xABCDABCDABCDABCDull;
    mc_context_info context{};
    context.size = sizeof(context);
    check(read && read->context(g_inproc.binding, g_inproc.generation, reinterpret_cast<mc_context_info*>(&old)) == MC_OK &&
              old.channel_id == 77 && old.guild_id == 88 && old.sentinel == 0xABCDABCDABCDABCDull &&
              old.size == offsetof(mc_context_info, user_id),
          "an older mc_context_info receives only the prefix it declared");
    check(read && read->context(g_inproc.binding, g_inproc.generation, &context) == MC_OK && context.user_id == 99 &&
              context.user_name_bytes == 6 && bytes_equal(context.user_name, "tester", 6) &&
              context.channel_name_bytes == 7,
          "mc.read copies the current context into the caller's struct");
    check(native_plugin_sdk_command_matches(g_runtime, utf8_literal("/old")), "the old-size command is listed");
    const auto result = native_plugin_sdk_run_command(&g_runtime, {1, 0, 0, utf8_literal("/old")});
    check(result.action == NativePluginSdkComposeAction::notice, "a command without a handler reports a notice instead of running");
    stop_inproc(plugin);
    g_runtime.read = {};
}

// ---- Storage caps ----------------------------------------------------------------------

void test_storage() noexcept {
    copy_bytes(g_runtime.data_directory, g_directory, g_directory_characters * 2);
    g_runtime.data_directory_characters = g_directory_characters;
    auto plugin = start_inproc(0x3001, MC_PERM_STORE, MC_PERM_ALL);
    const auto* store = table<mc_store_v1>("mc.store", 1);
    check(plugin.enabled && store, "store interface is available with the store permission");
    if (!store) {
        stop_inproc(plugin);
        return;
    }
    void* b = g_inproc.binding;
    const u64 g = g_inproc.generation;
    static u8 big[kPluginSdkStoreValueBytes + 1];
    check(store->set(b, g, {"big", 3}, big, kPluginSdkStoreValueBytes + 1) == MC_E_CAPACITY &&
              store->set(b, g, {"big", 3}, big, kPluginSdkStoreValueBytes) == MC_OK,
          "values above 2 KiB are refused; 2 KiB fits");
    u32 bytes = 0;
    u8 small[4];
    check(store->get(b, g, {"big", 3}, small, sizeof(small), &bytes) == MC_E_TRUNCATED && bytes == kPluginSdkStoreValueBytes,
          "a short read reports the needed size");
    check(store->set(b, g, {"", 0}, small, 1) == MC_E_INVALID && store->set(b, g, {"bad\n", 4}, small, 1) == MC_E_INVALID,
          "keys must be printable ASCII");
    u32 stored = 1;
    char key[8] = {'k', '0', '0'};
    for (u32 i = 0; i < 8; ++i) {
        key[1] = static_cast<char>('0' + i);
        stored = store->set(b, g, {key, 2}, big, kPluginSdkStoreValueBytes) == MC_OK ? stored : 0;
    }
    check(!stored && store->set(b, g, {"x", 1}, big, kPluginSdkStoreValueBytes) == MC_E_BUDGET,
          "the 16 KiB total is enforced");
    u32 used = 0, limit = 0;
    check(store->usage(b, g, &used, &limit) == MC_OK && used <= kPluginSdkStoreBytes && limit == kPluginSdkStoreBytes,
          "usage reports the bounded total");
    for (u32 i = 0; i < 8; ++i) {
        key[1] = static_cast<char>('0' + i);
        store->remove(b, g, {key, 2});
    }
    store->remove(b, g, {"big", 3});
    u32 accepted = 0;
    for (u32 i = 0; i < kPluginSdkStoreKeys + 1; ++i) {
        key[1] = static_cast<char>('A' + i / 10);
        key[2] = static_cast<char>('0' + i % 10);
        accepted += store->set(b, g, {key, 3}, &i, sizeof(i)) == MC_OK ? 1u : 0u;
    }
    check(accepted == kPluginSdkStoreKeys, "32 keys fit and the 33rd is refused");
    check(store->remove(b, g, {"zz", 2}) == MC_E_NOT_FOUND, "removing a missing key reports not found");
    const u32 marker = 0x12345678;
    store->remove(b, g, {"kA0", 3});
    check(store->set(b, g, {"persist", 7}, &marker, sizeof(marker)) == MC_OK, "a value is stored");
    native_plugin_sdk_poll(&g_runtime);
    stop_inproc(plugin);
    plugin = start_inproc(0x3001, MC_PERM_STORE, MC_PERM_ALL);
    store = table<mc_store_v1>("mc.store", 1);
    u32 restored = 0;
    check(plugin.enabled && store &&
              store->get(g_inproc.binding, g_inproc.generation, {"persist", 7}, &restored, sizeof(restored), &bytes) == MC_OK &&
              restored == marker && bytes == 4,
          "the store persists in the plugin's data folder across enables");
    stop_inproc(plugin);
}

// ---- Full DLL fixture -----------------------------------------------------------------------

constexpr u64 kFullOwner = 0x4D43344655554C4Cull;
constexpr u64 kSlowOwner = 0x4D433453304C4F57ull;
using Counter = u32(__cdecl*)(const void*, u32);

void test_full_fixture() noexcept {
    wchar_t path[1024];
    u32 characters = 0;
    check(sibling(L"MaiCordPluginSdkFullFixture.dll", path, &characters), "full fixture path");
    g_clock = 1000;
    NativePluginModule module{};
    const auto loaded = native_plugin_module_load(&module, &g_host.host, {path, characters}, MC_PERM_ALL);
    check(loaded.ok() && module.sdk && module.enabled, "the C fixture loads through maicord_plugin_query_v5");
    if (!loaded.ok()) return;
    const auto counter = reinterpret_cast<Counter>(GetProcAddress(module.module, "maicord_plugin_sdk_full_counter"));
    const void* state = static_cast<u8*>(module.state) + kPluginSdkStateHeaderBytes;
    check(counter && counter(state, 3) == 1, "start used the store (launch counter)");

    // Settings page with every control kind.
    const auto* descriptor = ui_page_registry_find(g_host.pages, {kFullOwner, 1});
    const auto* slot = native_plugin_host_find(g_host.host, kFullOwner);
    NativePluginSettingsSnapshot snapshot{};
    UiSettingsControl controls[12]{};
    UiSettingsPageContent content{};
    const bool projected = descriptor && slot &&
        native_plugin_settings_snapshot(descriptor->context, &snapshot) == NativePluginSettingsError::none &&
        native_plugin_settings_project(snapshot, {controls, 12}, &content) == NativePluginSettingsError::none;
    check(projected && content.controls.count == 9 && utf8_equal(content.title, utf8_literal("Dice settings")) &&
              controls[0].kind == UiSettingsControlKind::category && controls[1].kind == UiSettingsControlKind::toggle &&
              controls[2].kind == UiSettingsControlKind::choice && controls[2].choices.count == 3 &&
              controls[3].kind == UiSettingsControlKind::action && controls[4].kind == UiSettingsControlKind::action &&
              controls[5].kind == UiSettingsControlKind::slider && controls[5].choices.count == 21 &&
              controls[5].selected_choice == 10 && utf8_equal(controls[5].choices[10], utf8_literal("50")) &&
              controls[6].kind == UiSettingsControlKind::action && contains(controls[6].description, utf8_literal("#5865F2")) &&
              controls[7].kind == UiSettingsControlKind::action && controls[8].kind == UiSettingsControlKind::notice &&
              utf8_equal(controls[8].action_label, utf8_literal("Test")),
          "every control kind projects into the native settings vocabulary");
    NativePluginUiToken token{};
    const auto activate = [&](u64 control, UiSettingsControlKind kind, bool checked, u32 selected) noexcept {
        if (!native_plugin_host_settings_token(&g_host.host, {kFullOwner, 1}, &token).ok()) return false;
        return native_plugin_host_activate_settings(&g_host.host, token,
            {{UiSettingsTargetKind::control, {kFullOwner, 1}, control}, kind, checked, selected}).ok();
    };
    check(activate(2, UiSettingsControlKind::toggle, true, 0) && counter(state, 4) == 1,
          "a toggle reaches the plugin's setting handler");
    check(activate(6, UiSettingsControlKind::slider, false, 4) && counter(state, 5) == 20,
          "a slider step maps back to minimum + step * index");
    check(activate(9, UiSettingsControlKind::notice, false, 0) && counter(state, 8) == 1,
          "a notice button reaches the handler");
    check(!activate(5, UiSettingsControlKind::action, false, 0), "text controls are not activated as plain actions");
    NativePluginSdkEdit edit{};
    check(native_plugin_sdk_settings_edit(g_runtime, {kFullOwner, 1}, 5, &edit) && edit.kind == MC_SETTING_TEXT &&
              edit.max_bytes == 32 &&
              native_plugin_sdk_settings_submit(&g_runtime, {kFullOwner, 1}, 5, utf8_literal("Roll:")) == NativePluginError::none &&
              counter(state, 10) == 5,
          "text controls edit through the host form and reach the handler");
    check(native_plugin_sdk_settings_submit(&g_runtime, {kFullOwner, 1}, 7, utf8_literal("zz0000")) ==
                  NativePluginError::invalid_argument &&
              native_plugin_sdk_settings_submit(&g_runtime, {kFullOwner, 1}, 7, utf8_literal("#ff8800")) == NativePluginError::none &&
              counter(state, 6) == 0xFF8800,
          "colours are validated and parsed before the handler sees them");
    check(native_plugin_sdk_settings_submit(&g_runtime, {kFullOwner, 1}, 8, utf8_literal("shift + ctrl+k")) ==
                  NativePluginError::none &&
              counter(state, 9) == 12 &&
              native_plugin_sdk_settings_submit(&g_runtime, {kFullOwner, 1}, 8, utf8_literal("Ctrl+K+J")) ==
                  NativePluginError::invalid_argument,
          "keybinds are normalized to Ctrl+Shift+K and invalid chords refused");
    char normal[64];
    u32 normal_bytes = 0;
    check(native_plugin_sdk_keybind_normalize(utf8_literal("alt+f12"), normal, &normal_bytes) &&
              utf8_equal({normal, normal_bytes}, utf8_literal("Alt+F12")) &&
              !native_plugin_sdk_keybind_normalize(utf8_literal("ctrl+ctrl+a"), normal, &normal_bytes),
          "keybind normal form orders modifiers and rejects repeats");

    // Menu item (SDK-owned registry, typed context, runs as a gesture).
    NativePluginSdkMenuItem items[4]{};
    check(native_plugin_sdk_menu_items(g_runtime, MC_MENU_MESSAGE, {items, 4}) == 1 && items[0].owner_id == kFullOwner &&
              items[0].item_id == 1 && utf8_equal(items[0].label, utf8_literal("Roll for this message")) &&
              native_plugin_sdk_menu_items(g_runtime, MC_MENU_USER, {items, 4}) == 0,
          "message menu items are listed per target");
    NativePluginSdkMenuContext menu_context{};
    menu_context.target = MC_MENU_MESSAGE;
    menu_context.user_id = 5;
    menu_context.channel_id = 10;
    menu_context.message_id = 20;
    check(native_plugin_sdk_menu_activate(&g_runtime, kFullOwner, 1, menu_context) && counter(state, 7) == 1,
          "message menu items dispatch with their typed context");
    menu_context.target = MC_MENU_USER;
    check(!native_plugin_sdk_menu_activate(&g_runtime, kFullOwner, 1, menu_context) && counter(state, 7) == 1,
          "an item never runs for a target it wasn't registered for");

    // Local commands.
    NativePluginSdkCommandView views[4]{};
    check(native_plugin_sdk_commands(g_runtime, utf8_literal("r"), {views, 4}) == 2 &&
              utf8_equal(views[0].name, utf8_literal("roll")) && utf8_equal(views[1].name, utf8_literal("repeat")),
          "registered commands are listed by prefix for the command popup");
    check(native_plugin_sdk_command_matches(g_runtime, utf8_literal("/roll 2d6")) &&
              !native_plugin_sdk_command_matches(g_runtime, utf8_literal("/rollx")) &&
              !native_plugin_sdk_command_matches(g_runtime, utf8_literal("roll")),
          "only an exact /name prefix matches a local command");
    auto result = native_plugin_sdk_run_command(&g_runtime, {10, 0, 5, utf8_literal("/roll 2d6")});
    bool total_ok = false;
    if (result.action == NativePluginSdkComposeAction::send && starts_with(result.text, utf8_literal("rolled 2d6: "))) {
        u32 at = result.text.bytes, total = 0, scale = 1;
        while (at && result.text.data[at - 1] >= '0' && result.text.data[at - 1] <= '9') {
            total += static_cast<u32>(result.text.data[at - 1] - '0') * scale;
            scale *= 10;
            --at;
        }
        total_ok = total >= 2 && total <= 12;
    }
    check(total_ok, "/roll 2d6 sends a total between 2 and 12");
    result = native_plugin_sdk_run_command(&g_runtime, {10, 0, 5, utf8_literal("/repeat 2 yes hi there  ")});
    check(result.action == NativePluginSdkComposeAction::replace && utf8_equal(result.text, utf8_literal("HI THERE HI THERE")),
          "integer, boolean and rest-of-line string arguments parse positionally");
    result = native_plugin_sdk_run_command(&g_runtime, {10, 0, 5, utf8_literal("/repeat")});
    check(result.action == NativePluginSdkComposeAction::notice && utf8_equal(result.text, utf8_literal("Missing times.")),
          "missing required options produce a notice");
    result = native_plugin_sdk_run_command(&g_runtime, {10, 0, 5, utf8_literal("/repeat two hi")});
    check(result.action == NativePluginSdkComposeAction::notice &&
              utf8_equal(result.text, utf8_literal("times must be a whole number.")),
          "integer options are validated");
    result = native_plugin_sdk_run_command(&g_runtime, {10, 0, 5, utf8_literal("/roll 2x6")});
    check(result.action == NativePluginSdkComposeAction::notice, "plugin notices pass through");
    check(native_plugin_sdk_run_command(&g_runtime, {10, 0, 5, utf8_literal("/unknown")}).action ==
              NativePluginSdkComposeAction::none,
          "unknown commands are left for the server");

    // Pre-send transforms.
    result = native_plugin_sdk_pre_send(&g_runtime, {10, 0, 5, utf8_literal("!shout hello")});
    check(result.action == NativePluginSdkComposeAction::replace && utf8_equal(result.text, utf8_literal("HELLO")),
          "the pre-send transform replaces the draft");
    check(native_plugin_sdk_pre_send(&g_runtime, {10, 0, 5, utf8_literal("!cancel")}).action ==
              NativePluginSdkComposeAction::cancel,
          "the pre-send transform can cancel");
    check(native_plugin_sdk_pre_send(&g_runtime, {10, 0, 5, utf8_literal("!overflow")}).action ==
              NativePluginSdkComposeAction::none,
          "output claiming more than the host buffer is ignored");
    check(native_plugin_sdk_pre_send(&g_runtime, {10, 0, 5, utf8_literal("plain")}).action == NativePluginSdkComposeAction::none,
          "unchanged drafts stay unchanged");

    // Events and slots.
    NativePluginSdkEvent created{MC_EVENT_MESSAGE_CREATE, 0, 0, 10, 20, 6, 0, utf8_literal("let's roll"), utf8_literal("someone")};
    check((native_plugin_sdk_event_interest(g_runtime) & MC_EVENT_MASK(MC_EVENT_MESSAGE_CREATE)) &&
              native_plugin_sdk_event_push(&g_runtime, created) && counter(state, 0) == 0,
          "events queue without calling plugin code");
    native_plugin_sdk_poll(&g_runtime);
    check(counter(state, 0) == 1, "the next poll delivers queued events");
    NativePluginSdkSlotNode nodes[8]{};
    const NativePluginSdkSlotTarget accessory{MC_SLOT_MESSAGE_ACCESSORY, 10, 20, 0, 0};
    const u32 count = native_plugin_sdk_slots(g_runtime, accessory, {nodes, 8});
    check(count == 2 && nodes[0].kind == MC_NODE_BADGE && utf8_equal(nodes[0].text, utf8_literal("rolled")) &&
              nodes[0].icon == static_cast<u16>(UiIcon::favorite) && nodes[0].tone == MC_TONE_ACCENT &&
              utf8_equal(nodes[0].tooltip, utf8_literal("A dice roll")) && nodes[1].kind == MC_NODE_BUTTON &&
              nodes[1].icon == kPluginSdkNoIcon && nodes[1].tone == MC_TONE_NEUTRAL && nodes[1].action_id == 7,
          "slot projection resolves icons by name, maps unknown tones to neutral and keeps button actions");
    check(native_plugin_sdk_slots(g_runtime, {MC_SLOT_COMPOSER_TOOLBAR, 1, 2, 3, 4}, {nodes, 8}) == 1 &&
              nodes[0].kind == MC_NODE_BUTTON && nodes[0].action_id == 9,
          "global slots ignore target ids");
    check(native_plugin_sdk_slots(g_runtime, {MC_SLOT_MESSAGE_ACCESSORY, 10, 21, 0, 0}, {nodes, 8}) == 0,
          "slots are scoped to their message");
    const u64 popovers = g_runtime.popover_revision;
    check(native_plugin_sdk_slot_action(&g_runtime, kFullOwner, accessory, 7) && counter(state, 2) == 1 &&
              !native_plugin_sdk_slot_action(&g_runtime, kFullOwner, accessory, 8),
          "slot buttons dispatch only actions the plugin published");
    check(g_runtime.popover_revision != popovers && g_runtime.popover_count == 2 && g_runtime.popover_owner == kFullOwner &&
              g_runtime.popover[0].kind == MC_NODE_LABEL && g_runtime.popover[1].action_id == 8 &&
              g_runtime.popover_anchor.message_id == 20,
          "a slot action may answer with a popover anchored to its target");
    check(!native_plugin_sdk_popover_action(&g_runtime, g_runtime.popover_revision, 0) &&
              !native_plugin_sdk_popover_action(&g_runtime, g_runtime.popover_revision + 1, 1) &&
              native_plugin_sdk_popover_action(&g_runtime, g_runtime.popover_revision, 1) && counter(state, 11) == 1 &&
              !g_runtime.popover_count,
          "popover buttons run the handler again with their own action; stale revisions and labels are refused");

    // Timers on the fake clock.
    g_clock = 1100;
    native_plugin_sdk_poll(&g_runtime);
    const u32 before = counter(state, 1);
    g_clock = 1300;
    const u64 next = native_plugin_sdk_poll(&g_runtime).next_timer_ms;
    check(before == 0 && counter(state, 1) == 1 && next == 1550, "repeating timers fire when due and reschedule from now");

    const u64 revision = g_runtime.slot_revision;
    check(native_plugin_module_unload(&module).ok() && !module.sdk && g_runtime.slot_revision != revision &&
              !native_plugin_sdk_command_matches(g_runtime, utf8_literal("/roll")) &&
              !native_plugin_sdk_slots(g_runtime, accessory, {nodes, 8}) && !ui_page_registry_find(g_host.pages, {kFullOwner, 1}),
          "unload removes commands, slots and pages");
    const auto reloaded = native_plugin_module_load(&module, &g_host.host, {path, characters}, MC_PERM_ALL);
    const auto counter2 = reloaded.ok() ? reinterpret_cast<Counter>(GetProcAddress(module.module, "maicord_plugin_sdk_full_counter")) : nullptr;
    check(reloaded.ok() && counter2 && counter2(static_cast<u8*>(module.state) + kPluginSdkStateHeaderBytes, 3) == 2,
          "the store survives a reload (launch counter 2)");
    native_plugin_module_unload(&module);
    const auto restricted = native_plugin_module_load(&module, &g_host.host, {path, characters}, MC_PERM_COMMANDS);
    check(restricted.error == NativePluginModuleError::permissions && !module.module && !module.sdk,
          "loading with fewer approved permissions than requested is refused and fully released");
}

void test_missing_export() noexcept {
    // A DLL without maicord_plugin_query_v5 (any older-ABI plugin) is refused and released.
    wchar_t path[MAX_PATH];
    const UINT characters = GetSystemDirectoryW(path, MAX_PATH);
    constexpr wchar_t leaf[] = L"\\kernel32.dll";
    constexpr u32 leaf_characters = sizeof(leaf) / sizeof(leaf[0]);
    if (!characters || characters + leaf_characters > MAX_PATH) {
        check(false, "System32 path fits the fixed test buffer");
        return;
    }
    for (u32 i = 0; i < leaf_characters; ++i) path[characters + i] = leaf[i];
    NativePluginModule module{};
    const auto loaded = native_plugin_module_load(&module, &g_host.host, {path, characters + leaf_characters - 1},
                                                  MC_PERM_ALL);
    check(loaded.error == NativePluginModuleError::missing_export && !module.module && !module.sdk && !module.enabled,
          "a DLL without the ABI-4 export is refused and released");
}


void test_time_budget() noexcept {
    wchar_t path[1024];
    u32 characters = 0;
    check(sibling(L"MaiCordPluginSdkSlowFixture.dll", path, &characters), "slow fixture path");
    NativePluginModule module{};
    g_ledger.budget_us = 5'000;
    const auto loaded = native_plugin_module_load(&module, &g_host.host, {path, characters}, MC_PERM_ALL);
    check(loaded.ok() && module.sdk && native_plugin_sdk_permissions(module.sdk) == MC_PERM_COMMANDS,
          "a definition from a newer SDK (larger size) is accepted by prefix");
    if (!loaded.ok()) return;
    using Configure = void(__cdecl*)(unsigned long);
    const auto configure = reinterpret_cast<Configure>(GetProcAddress(module.module, "maicord_plugin_sdk_slow_configure"));
    if (configure) configure(12);
    for (u32 i = 0; i < 2; ++i) native_plugin_sdk_run_command(&g_runtime, {1, 0, 0, utf8_literal("/slow")});
    check(!plugin_call_ledger_disable_requested(&g_ledger, kSlowOwner), "two overruns are tolerated");
    native_plugin_sdk_run_command(&g_runtime, {1, 0, 0, utf8_literal("/slow")});
    const PluginCallLedgerEntry* entry = nullptr;
    for (const auto& candidate : g_ledger.entries)
        if (candidate.owner_id == kSlowOwner) entry = &candidate;
    check(plugin_call_ledger_disable_requested(&g_ledger, kSlowOwner) && entry && entry->overruns == 3 &&
              entry->worst_us >= 10'000,
          "the third overrun requests a disable after the call returned");
    check(!native_plugin_sdk_command_matches(g_runtime, utf8_literal("/slow")) &&
              native_plugin_sdk_run_command(&g_runtime, {1, 0, 0, utf8_literal("/slow")}).action ==
                  NativePluginSdkComposeAction::none,
          "a plugin waiting for its disable gets no further calls");
    check(plugin_call_ledger_take_disable(&g_ledger) == kSlowOwner && !plugin_call_ledger_take_disable(&g_ledger),
          "the pending disable is handed to the owner once");
    check(native_plugin_module_unload(&module).ok(), "the slow plugin is turned off");
    g_ledger.budget_us = kPluginCallBudgetUs;
}

// ---- Marker file ---------------------------------------------------------------------------

NativePluginManifest manifest_for(u64 owner, u64 permissions) noexcept {
    NativePluginManifest manifest{};
    manifest.owner_id = owner;
    manifest.abi_version = 5;
    manifest.permissions = permissions;
    copy_bytes(manifest.name, "Dice", 4);
    manifest.name_bytes = 4;
    manifest.version[0] = '1';
    manifest.version_bytes = 1;
    copy_bytes(manifest.dll, "dice0.dll", 9);
    manifest.dll[4] = static_cast<char>('0' + owner % 10);
    manifest.dll_bytes = 9;
    for (u32 i = 0; i < 32; ++i) manifest.sha256[i] = static_cast<u8>(i * 7);
    return manifest;
}

void test_marker(const wchar_t* marker) noexcept {
    PluginCallLedger ledger{};
    PluginCallMarker leftover{};
    check(plugin_call_ledger_open(&ledger, marker, &leftover) && !leftover.present, "a fresh marker has no leftover call");
    auto ticket = plugin_call_begin(&ledger, 0x5151, PluginCallKind::observe);
    plugin_call_end(&ledger, ticket);
    ticket = plugin_call_begin(&ledger, 0x7777, PluginCallKind::command);
    // Simulate MaiCord dying inside the call: drop the mapping without leaving it.
    UnmapViewOfFile(const_cast<u8*>(ledger.view));
    CloseHandle(ledger.mapping);
    CloseHandle(ledger.file);
    ledger = {};
    check(plugin_call_ledger_peek(marker, &leftover) && leftover.present && leftover.owner_id == 0x7777 &&
              leftover.kind == PluginCallKind::command,
          "startup reports the plugin whose call was in progress");
    PluginCallMarker again{};
    check(plugin_call_ledger_peek(marker, &again) && !again.present &&
              GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES,
          "the leftover is consumed once it has been reported");
    check(plugin_call_ledger_open(&ledger, marker, &again) && !again.present, "the marker reopens idle");
    plugin_call_ledger_close(&ledger);
    check(GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES, "a clean close removes the marker file");

    NativePluginPreferences preferences{};
    const auto manifest = manifest_for(0x7777, MC_PERM_COMMANDS | MC_PERM_STORE);
    check(native_plugin_preferences_enable(&preferences, manifest, MC_PERM_COMMANDS | MC_PERM_STORE).ok() &&
              native_plugin_preferences_restorable(preferences, manifest),
          "an approved ABI-4 plugin restores at startup");
    check(native_plugin_preferences_set_flags(&preferences, leftover.owner_id ? leftover.owner_id : 0x7777,
                                              native_plugin_preference_crashed) &&
              !native_plugin_preferences_restorable(preferences, manifest) &&
              native_plugin_preferences_find(preferences, manifest),
          "the crashed plugin stays remembered but off");
    check(native_plugin_preferences_enable(&preferences, manifest, MC_PERM_COMMANDS | MC_PERM_STORE).ok() &&
              preferences.entries[0].flags == 0 && native_plugin_preferences_restorable(preferences, manifest),
          "turning it back on clears the flag");
    auto wider = manifest;
    wider.permissions |= MC_PERM_READ_MESSAGES;
    check(!native_plugin_preferences_restorable(preferences, wider), "a manifest asking for more needs new approval");
    check(native_plugin_preferences_enable(&preferences, manifest, MC_PERM_TIMER).error == NativePluginPreferencesError::invalid_input,
          "approvals beyond the manifest are refused");
}

// ---- Manifest and plugins.bin -------------------------------------------------------------

u32 manifest_document(char (&out)[1024], const char* head, const char* extra) noexcept {
    u32 at = 0;
    const auto put = [&](const char* text) noexcept {
        while (*text) out[at++] = *text++;
    };
    put(head);
    put(",\"name\":\"Dice\",\"version\":\"1.0.0\",\"description\":\"Rolls\",\"dll\":\"dice.dll\"");
    put(extra);
    put(",\"sha256\":\"");
    for (u32 i = 0; i < 63; ++i) out[at++] = '0';
    put("1\"}");
    return at;
}
void test_manifest() noexcept {
    char document[1024];
    u32 bytes = manifest_document(document, "{\"schema\":3,\"owner_id\":77,\"abi\":5,\"min_host_abi\":5",
                                  ",\"publisher\":\"Example\",\"permissions\":[\"commands\",\"store\"]");
    NativePluginManifest manifest{};
    check(native_plugin_manifest_parse({document, bytes}, &manifest).ok() &&
              manifest.abi_version == 5 && manifest.min_host_abi == 5 &&
              manifest.permissions == (MC_PERM_COMMANDS | MC_PERM_STORE) &&
              utf8_equal({manifest.publisher, manifest.publisher_bytes}, utf8_literal("Example")) &&
              native_plugin_manifest_valid(manifest),
          "a manifest parses permissions, publisher and min_host_abi");
    char written[kNativePluginManifestMaximumBytes];
    u32 written_bytes = 0;
    NativePluginManifest round{};
    check(native_plugin_manifest_write(manifest, {written, sizeof(written)}, &written_bytes).ok() &&
              native_plugin_manifest_parse({written, written_bytes}, &round).ok() &&
              round.permissions == manifest.permissions && round.min_host_abi == 5 &&
              round.publisher_bytes == 7,
          "a manifest writes and parses back");
    const auto rejects = [&](const char* head, const char* extra, NativePluginManifestError expected) noexcept {
        NativePluginManifest out{};
        const u32 count = manifest_document(document, head, extra);
        return native_plugin_manifest_parse({document, count}, &out).error == expected;
    };
    check(rejects("{\"schema\":2,\"owner_id\":7,\"abi\":5", ",\"permissions\":[]", NativePluginManifestError::unsupported),
          "schema 2 manifests are rejected");
    check(rejects("{\"schema\":3,\"owner_id\":7,\"abi\":4", ",\"permissions\":[]", NativePluginManifestError::unsupported),
          "ABI 4 manifests are rejected");
    check(rejects("{\"schema\":3,\"owner_id\":7,\"abi\":5", "", NativePluginManifestError::missing_field) &&
              rejects("{\"schema\":3,\"owner_id\":7,\"abi\":5", ",\"permissions\":[\"everything\"]",
                      NativePluginManifestError::unsupported) &&
              rejects("{\"schema\":3,\"owner_id\":7,\"abi\":5", ",\"permissions\":[\"store\",\"store\"]",
                      NativePluginManifestError::duplicate_field),
          "a manifest requires a permission list of known, unique names");
    auto old_abi = manifest;
    old_abi.abi_version = 4;
    check(native_plugin_manifest_valid(manifest) && !native_plugin_manifest_valid(old_abi),
          "only ABI 5 records are valid");
    check(utf8_equal(native_plugin_permission_name(3), utf8_literal("commands")) &&
              utf8_equal(native_plugin_permission_name(21), utf8_literal("effects")) &&
              native_plugin_permission_label(13).bytes && !native_plugin_permission_name(22).bytes &&
              kNativePluginPermissionsAll == MC_PERM_ALL,
          "permission names follow MC_PERM_* bit order");
}

void test_preferences_file(const wchar_t* path) noexcept {
    // An older MaiCord's version 1 file (1440 bytes) is not read: the choices start empty.
    static u8 old_record[1440];
    clear_bytes(old_record, sizeof(old_record));
    copy_bytes(old_record, "MAIPLUGS", 8);
    byte_store_le32(old_record + 8, 1);
    byte_store_le32(old_record + 12, sizeof(old_record));
    byte_store_le32(old_record + 16, 1);
    const auto manifest = manifest_for(42, MC_PERM_COMMANDS);
    u8* row = old_record + 32;
    byte_store_le64(row, 42);
    byte_store_le32(row + 8, 3);
    byte_store_le32(row + 12, manifest.dll_bytes);
    copy_bytes(row + 16, manifest.sha256, 32);
    copy_bytes(row + 48, manifest.dll, manifest.dll_bytes);
    byte_store_le64(old_record + 24, hash64_bytes(hash64_bytes(kHash64Seed, old_record, 24), old_record + 32,
                                                  sizeof(old_record) - 32));
    NativePluginPreferences decoded{};
    check(native_plugin_preferences_decode({old_record, sizeof(old_record)}, &decoded).error ==
              NativePluginPreferencesError::size,
          "a version 1 plugins.bin does not decode");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    DWORD written = 0;
    const bool wrote = file != INVALID_HANDLE_VALUE && WriteFile(file, old_record, sizeof(old_record), &written, nullptr) &&
                       written == sizeof(old_record);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    NativePluginPreferencesIo io{};
    NativePluginPreferences loaded{};
    const auto old_load = native_plugin_preferences_load_file(&io, path, &loaded);
    check(wrote && old_load.ok() && old_load.missing && loaded.count == 0, "a version 1 file loads as absent");
    const auto approved = manifest_for(43, MC_PERM_UI_SLOTS);
    check(native_plugin_preferences_enable(&loaded, approved, MC_PERM_UI_SLOTS).ok() &&
              native_plugin_preferences_set_flags(&loaded, 43, native_plugin_preference_overran) &&
              native_plugin_preferences_save_file(&io, path, loaded).ok(),
          "saving replaces the old file with the current version");
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    NativePluginPreferences reread{};
    check(GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) &&
              attributes.nFileSizeLow == kNativePluginPreferencesRecordBytes &&
              native_plugin_preferences_load_file(&io, path, &reread).ok() && reread.count == 1 &&
              reread.entries[0].permissions == MC_PERM_UI_SLOTS && reread.entries[0].flags == native_plugin_preference_overran &&
              !native_plugin_preferences_restorable(reread, approved),
          "current records keep approved permissions and off flags");
    DeleteFileW(path);
}

void test_developer_gate() noexcept {
    using namespace app_detail;
    static UiSettingsControl controls[kApplicationPluginSettingsMaximumControls];
    static ApplicationPluginCatalogText text[8];
    static NativePluginCatalogEntry entries[1];
    entries[0] = {};
    entries[0].manifest = manifest_for(43, MC_PERM_COMMANDS | MC_PERM_STORE);
    NativePluginCatalog catalog{};
    catalog.entries = {entries, 1};
    catalog.count = 1;
    NativePluginPreferences preferences{};
    native_plugin_preferences_enable(&preferences, entries[0].manifest, MC_PERM_COMMANDS);
    native_plugin_preferences_set_flags(&preferences, 43, native_plugin_preference_crashed);
    ApplicationPluginSettingsState state{};
    state.status = utf8_literal("Only enable plugins you trust.");
    state.catalog = &catalog;
    state.discovered = true;
    state.preferences = &preferences;
    state.developer_mode = false;
    u32 count = application_plugins_settings_project(state, {controls, 21}, {text, 8});
    bool manual = false;
    for (u32 i = 0; i < count; ++i) manual = manual || controls[i].id == 1;
    check(count == 5 && !manual && controls[0].id == kApplicationPluginInstallAction &&
              utf8_equal(controls[0].description, state.status),
          "without Developer Mode the manifest-less DLL loader is not offered");
    const auto& row = controls[4];
    check(row.id == kApplicationPluginCatalogActionFirst &&
              contains(row.description, utf8_literal("Turned off because MaiCord closed while it was running.")) &&
              contains(row.description, utf8_literal("Permissions: commands, store.")),
          "catalog rows show the crash notice and requested permissions");
    state.developer_mode = true;
    count = application_plugins_settings_project(state, {controls, 21}, {text, 8});
    check(count == 6 && controls[0].id == 1 && utf8_equal(controls[0].action_label, utf8_literal("Choose DLL")),
          "Developer Mode offers loading a DLL without a manifest");
}

bool make_directory() noexcept {
    wchar_t temp[MAX_PATH];
    const DWORD count = GetTempPathW(MAX_PATH, temp);
    if (!count || count >= MAX_PATH || !GetTempFileNameW(temp, L"mc4", 0, g_directory) || !DeleteFileW(g_directory) ||
        !CreateDirectoryW(g_directory, nullptr)) return false;
    while (g_directory[g_directory_characters]) ++g_directory_characters;
    return true;
}
void join(const wchar_t* leaf, wchar_t (&output)[MAX_PATH + 64]) noexcept {
    u32 at = 0;
    for (; at < g_directory_characters; ++at) output[at] = g_directory[at];
    output[at++] = L'\\';
    for (u32 i = 0; leaf[i]; ++i) output[at++] = leaf[i];
    output[at] = 0;
}
void remove_owner_store(u64 owner) noexcept {
    wchar_t leaf[40], path[MAX_PATH + 64];
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (u32 i = 0; i < 16; ++i) leaf[i] = digits[(owner >> (60 - 4 * i)) & 15];
    leaf[16] = 0;
    join(leaf, path);
    wchar_t file[MAX_PATH + 64];
    u32 at = 0;
    while (path[at]) { file[at] = path[at]; ++at; }
    constexpr wchar_t store[] = L"\\store.bin";
    for (u32 i = 0; i < sizeof(store) / sizeof(store[0]); ++i) file[at + i] = store[i];
    DeleteFileW(file);
    RemoveDirectoryW(path);
}

#include "plugin_sdk_capabilities.inc"

void run() noexcept {
    if (!make_directory()) {
        check(false, "temporary directory");
        return;
    }
    g_runtime.ledger = &g_ledger;
    g_runtime.clock_ms = test_clock;
    check(initialize_host(), "host fixture initializes");
    test_interfaces();
    test_size_prefix();
    test_storage();
    copy_bytes(g_runtime.data_directory, g_directory, g_directory_characters * 2);
    g_runtime.data_directory_characters = g_directory_characters;
    test_full_fixture();
    test_missing_export();
    test_time_budget();
    wchar_t marker[MAX_PATH + 64], preferences[MAX_PATH + 64];
    join(L"plugin-call.bin", marker);
    join(L"plugins.bin", preferences);
    test_marker(marker);
    test_manifest();
    test_preferences_file(preferences);
    test_developer_gate();
    test_read();
    test_events();
    test_render();
    test_actions();
    test_keybinds();
    test_slots_v2();
    test_fx();
    test_showcase();
    DeleteFileW(marker);
    DeleteFileW(preferences);
    remove_owner_store(0x3001);
    remove_owner_store(kFullOwner);
    RemoveDirectoryW(g_directory);
    if (!failures) {
        constexpr char done[] = "plugin_sdk completed\n";
        DWORD written{};
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), done, sizeof(done) - 1, &written, nullptr);
    }
}
} // namespace maicord::plugin_sdk_tests

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_sdk_tests_entry() noexcept {
    maicord::plugin_sdk_tests::run();
    ExitProcess(maicord::plugin_sdk_tests::failures ? 1 : 0);
}
