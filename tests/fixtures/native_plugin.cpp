#include "native_plugin_fixture.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace {
using namespace maicord;

using native_plugin_fixture::FixtureAudit;

// Everything the plugin keeps lives in its host-owned state.
struct FixtureState {
    void* binding;
    u64 generation;
    const mc_pages_v1* pages;
    const mc_settings_v3* settings;
    const mc_menus_v2* menus;
    const mc_actions_v1* actions;
    mc_setting control;
    mc_settings_page page;
};
FixtureAudit* audit = nullptr;
FixtureState* current = nullptr;
u32 mode = 0;
u64 owner = native_plugin_fixture::owner_id;
bool active = false;

template <u32 N> mc_str literal(const char (&data)[N]) noexcept { return {data, N - 1}; }

mc_status publish(FixtureState* state) noexcept {
    state->page.size = sizeof(state->page);
    state->page.control_count = 1;
    state->page.title = literal("Native plugin fixture");
    state->page.description = literal("Explicitly loaded ABI 5 module.");
    state->page.controls = &state->control;
    state->page.control_stride = sizeof(mc_setting);
    return state->settings->set_page(state->binding, state->generation, 1, &state->page);
}


bool remove_menu(u64 extension_id) noexcept {
    return current && current->menus &&
           current->menus->unregister_item(current->binding, current->generation, extension_id) == MC_OK;
}

mc_status MC_CALL setting_changed(void* memory, uint32_t bytes, const mc_setting_value* value) noexcept {
    if (!memory || bytes != sizeof(FixtureState) || !value || value->page_id != 1 || value->control_id != 1 ||
        value->kind != MC_SETTING_TOGGLE) return MC_E_INVALID;
    auto* state = static_cast<FixtureState*>(memory);
    const u32 checked = value->checked ? 1u : 0u;
    if (state->control.checked == checked) return MC_OK;
    state->control.checked = checked;
    return publish(state);
}

mc_status MC_CALL menu_activated(void* memory, uint32_t bytes, const mc_menu_context* target) noexcept {
    if (!memory || bytes != sizeof(FixtureState) || !target ||
        !((target->item_id == native_plugin_fixture::friends_extension_id && target->target == MC_MENU_USER &&
           target->user_id && !target->message_id) ||
          (target->item_id == native_plugin_fixture::chat_extension_id && target->target == MC_MENU_MESSAGE &&
           target->channel_id && target->message_id)))
        return MC_E_INVALID;
    if (audit) {
        ++audit->menu_activations;
        audit->last_menu_activation = {target->item_id, target->user_id, target->channel_id, target->message_id,
                                       target->guild_id, target->target, target->flags};
        audit->menu_callback_popup_dismissed = !audit->popup || !audit->popup->active;
        if (audit->menu_callback_hook) audit->menu_callback_hook();
    }
    return mode == 9 ? MC_E_INVALID : MC_OK;
}

template <class Table> bool interface_table(const mc_host* host, const char* name, u32 version,
                                            const Table** table) noexcept {
    const void* found = nullptr;
    if (host->get_interface(host->binding, host->generation, name, version, &found) != MC_OK || !found) return false;
    *table = static_cast<const Table*>(found);
    return true;
}

mc_status MC_CALL start(const mc_host* host, void* memory, uint32_t bytes) noexcept {
    if (audit) {
        ++audit->starts;
        if (audit->start_callback_hook) audit->start_callback_hook();
    }
    if (!host || !memory || bytes != sizeof(FixtureState)) return MC_E_INVALID;
    const auto* raw = static_cast<const volatile u8*>(memory);
    bool zero = true;
    for (u32 index = 0; index < bytes; ++index) if (raw[index]) zero = false;
    const bool aligned = (reinterpret_cast<usize>(memory) & 63u) == 0;
    if (audit) {
        audit->state_zero = zero;
        audit->state_aligned = aligned;
    }
    if (!zero || !aligned) return MC_E_INVALID;
    auto* state = static_cast<FixtureState*>(memory);
    state->binding = host->binding;
    state->generation = host->generation;
    active = true;
    if (!interface_table(host, "mc.ui.pages", 1, &state->pages) ||
        !interface_table(host, "mc.settings", 3, &state->settings) ||
        !interface_table(host, "mc.ui.menus", 2, &state->menus)) return MC_E_INVALID;
    if (!interface_table(host, "mc.actions", 1, &state->actions)) state->actions = nullptr;
    state->control.size = sizeof(state->control);
    state->control.kind = MC_SETTING_TOGGLE;
    state->control.id = 1;
    state->control.label = literal("Enable fixture feature");
    state->control.description = literal("A trusted native plugin toggle.");
    state->control.enabled = 1;
    if (state->pages->register_page(state->binding, state->generation, 1, literal("Native fixture")) != MC_OK ||
        publish(state) != MC_OK ||
        state->settings->set_handler(state->binding, state->generation, setting_changed) != MC_OK ||
        state->menus->set_handler(state->binding, state->generation, menu_activated) != MC_OK ||
        state->menus->register_item(state->binding, state->generation, native_plugin_fixture::friends_extension_id,
                                    MC_MENU_USER, literal("Open native fixture for friend"), 0) != MC_OK ||
        state->menus->register_item(state->binding, state->generation, native_plugin_fixture::chat_extension_id,
                                    MC_MENU_MESSAGE, literal("Open native fixture for message"), 0) != MC_OK)
        return MC_E_INVALID;
    current = state;
    if (audit) audit->remove_menu = remove_menu;
    return mode == 3 ? MC_E_INVALID : MC_OK;
}

void MC_CALL stop(void* memory, uint32_t bytes) noexcept {
    if (audit) {
        ++audit->stops;
        if (audit->stop_callback_hook) audit->stop_callback_hook();
        audit->stop_after_detach = audit->host_detaches >= audit->stops;
        bool no_pages = true;
        if (audit->pages) {
            for (u32 index = 0; index < audit->pages->count; ++index)
                if (audit->pages->storage[index].key.owner_id == owner) no_pages = false;
        }
        audit->stop_without_pages = no_pages;
        audit->remove_menu = nullptr;
    }
    current = nullptr;
    if (memory && bytes == sizeof(FixtureState)) {
        auto* state = static_cast<FixtureState*>(memory);
        if (state->pages) {
            const auto removed = state->pages->unregister_page(state->binding, state->generation, 1);
            if (audit) audit->stop_services_inactive = removed == MC_E_INACTIVE;
        }
        if (state->menus) {
            const auto removed = state->menus->unregister_item(state->binding, state->generation,
                                                               native_plugin_fixture::friends_extension_id);
            if (audit) audit->stop_menu_services_inactive = removed == MC_E_INACTIVE;
        }
    }
    active = false;
}
} // namespace

extern "C" __declspec(dllexport) void WINAPI maicord_plugin_fixture_configure(
    maicord::u32 requested_mode, maicord::u64 requested_owner, void* observations) noexcept {
    mode = requested_mode;
    owner = requested_owner;
    audit = static_cast<FixtureAudit*>(observations);
}

// Modes: 1 query fails, 2 wrong ABI, 3 start fails, 4 unapproved permission, 5 no owner,
// 6 truncated definition, 7 state too large, 8 settings permission only, 9 menu action
// rejected.
extern "C" __declspec(dllexport) mc_status MC_CALL maicord_plugin_query_v5(const mc_host_info* host,
    mc_plugin_definition* definition) noexcept {
    if (audit) ++audit->queries;
    if (mode == 1 || !host || !definition) return MC_E_INVALID;
    mc_plugin_definition local{};
    local.size = mode == 6 ? offsetof(mc_plugin_definition, start) - 1 : sizeof(local);
    local.abi_version = mode == 2 ? 4 : MC_ABI_VERSION;
    local.owner_id = mode == 5 ? 0 : owner;
    local.permissions = mode == 4 ? 1ull << 40 : mode == 8 ? MC_PERM_SETTINGS : MC_PERM_SETTINGS | MC_PERM_MENUS;
    local.state_bytes = mode == 7 ? 1u << 20 : sizeof(FixtureState);
    local.state_alignment = 64;
    local.min_host_abi = 5;
    local.start = start;
    local.stop = stop;
    return mc_definition_publish(definition, &local);
}

extern "C" BOOL WINAPI maicord_plugin_fixture_entry(HINSTANCE instance, DWORD reason, void*) noexcept {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    if (reason == DLL_PROCESS_DETACH && audit) {
        ++audit->dll_detaches;
        audit->dll_after_stop = !active;
    }
    return TRUE;
}
