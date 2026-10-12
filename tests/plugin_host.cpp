#include "maicord/app/plugin_host.hpp"

namespace maicord {
namespace {

struct PluginFixture {
    NativePluginHost* host;
    UiPageRegistry* pages;
    NativePluginServices services;
    u32 starts;
    u32 stops;
    u32 detaches;
    bool fail_start;
    bool state_was_zero;
    bool stop_saw_no_pages;
    bool detach_saw_live_page;
    bool lifecycle_reentry_blocked;
    bool detach_service_reentry_blocked;
    NativePluginServices peer_services;
    NativePluginSettingsControl controls[4];
    NativePluginSettingsPage content;
    NativePluginUiToken token;
    UiSettingsActivation activation;
    u32 activations;
    bool reject_activation;
    bool activation_reentry_blocked;
    bool peer_service_reentry_blocked;
};

PluginFixture fixture;
constexpr Utf8View plugin_choices[] = {utf8_literal("One"), utf8_literal("Two")};

bool WINAPI start_idle(NativePluginServices services, void* state, u32 bytes) noexcept {
    ++fixture.starts;
    fixture.services = services;
    fixture.state_was_zero = true;
    auto* data = static_cast<u8*>(state);
    for (u32 index = 0; index < bytes; ++index) fixture.state_was_zero &= !data[index];
    if (bytes) data[0] = 0xAB;
    return !fixture.fail_start;
}

bool WINAPI start_page(NativePluginServices services, void* state, u32 bytes) noexcept {
    const bool started = start_idle(services, state, bytes);
    const NativePluginResult page = services.register_page(services.binding, services.generation,
        1, utf8_literal("Plugin settings"), state);
    return started && page.ok();
}

bool WINAPI start_settings(NativePluginServices services, void* state, u32 bytes) noexcept {
    const bool started = start_idle(services, state, bytes);
    fixture.controls[0] = {1, utf8_literal("Toggle"), {}, NativePluginSettingsKind::toggle, true, false};
    fixture.controls[1] = {2, utf8_literal("Choice"), {}, NativePluginSettingsKind::choice,
        true, false, {plugin_choices, 2}, 0};
    fixture.controls[2] = {3, utf8_literal("Action"), {}, NativePluginSettingsKind::action,
        true, false, {}, 0, utf8_literal("Apply")};
    fixture.controls[3] = {4, utf8_literal("Disabled"), {}, NativePluginSettingsKind::toggle, false, false};
    fixture.content = {utf8_literal("Settings"), {}, {fixture.controls, 4}, 1};
    const auto page = services.register_page(services.binding, services.generation,
        1, utf8_literal("Settings"), &fixture.content);
    return started && page.ok();
}

NativePluginActivationResult WINAPI activate_settings(void*, u32, NativePluginSettingsActivation activation) noexcept {
    ++fixture.activations;
    const auto own = fixture.services;
    const auto peer = fixture.peer_services;
    NativePluginUiToken ignored{};
    fixture.activation_reentry_blocked =
        native_plugin_host_disable(fixture.host, activation.target.page.owner_id).error == NativePluginError::busy &&
        native_plugin_host_disable_all(fixture.host).error == NativePluginError::busy &&
        native_plugin_host_activate_settings(fixture.host, fixture.token, fixture.activation).error == NativePluginError::busy &&
        native_plugin_host_settings_token(fixture.host,
            {activation.target.page.owner_id, activation.target.page.page_id}, &ignored).error == NativePluginError::busy &&
        own.register_page(own.binding, own.generation, 2, utf8_literal("Reentry"), nullptr).error == NativePluginError::inactive &&
        own.unregister_page(own.binding, own.generation, 1).error == NativePluginError::inactive;
    fixture.peer_service_reentry_blocked =
        peer.register_page(peer.binding, peer.generation, 2, utf8_literal("Peer reentry"), nullptr).error == NativePluginError::inactive &&
        peer.unregister_page(peer.binding, peer.generation, 1).error == NativePluginError::inactive;
    if (fixture.reject_activation) return {false, true};
    bool changed = activation.kind == NativePluginSettingsKind::action;
    if (activation.kind == NativePluginSettingsKind::toggle) {
        changed = fixture.controls[0].checked != activation.checked;
        fixture.controls[0].checked = activation.checked;
    } else if (activation.kind == NativePluginSettingsKind::choice) {
        changed = fixture.controls[1].selected_choice != activation.selected_choice;
        fixture.controls[1].selected_choice = activation.selected_choice;
    }
    if (changed) ++fixture.content.revision;
    return {true, changed};
}

void WINAPI stop_plugin(void*, u32) noexcept {
    ++fixture.stops;
    fixture.stop_saw_no_pages = fixture.pages->count == 1;
}

void detach_owner(void*, u64 owner_id) noexcept {
    ++fixture.detaches;
    fixture.detach_saw_live_page =
        ui_page_registry_find(*fixture.pages, {owner_id, 1}) != nullptr;
    fixture.lifecycle_reentry_blocked =
        native_plugin_host_disable(fixture.host, owner_id).error == NativePluginError::busy;
    const auto services = fixture.services;
    fixture.detach_service_reentry_blocked = services.register_page(services.binding,
        services.generation, 100, utf8_literal("Reentry"), nullptr).error == NativePluginError::inactive;
    if (fixture.peer_services.binding) {
        const auto peer = fixture.peer_services;
        fixture.peer_service_reentry_blocked = peer.register_page(peer.binding,
            peer.generation, 100, utf8_literal("Peer reentry"), nullptr).error == NativePluginError::inactive;
    }
}

NativePluginDefinition definition(u64 owner_id, bool pages = true) noexcept {
    return {owner_id,
        pages ? native_plugin_capability_pages : 0, pages ? 2u : 0u,
        32, 16, pages ? start_page : start_idle, stop_plugin};
}

bool zeroed(ConstSlice<u8> data) noexcept {
    for (u32 index = 0; index < data.count; ++index) if (data[index]) return false;
    return true;
}

void test_settings_dispatch(void (*check)(bool, const char*) noexcept) noexcept {
    UiPage page_storage[8]{};
    UiPageRegistry pages{};
    NativePluginSlot slots[2]{};
    NativePluginHost host{};
    alignas(64) u8 state_a[64]{}, state_b[64]{};
    NativePluginPolicy policy{native_plugin_capability_pages, 2, 64, 80, detach_owner, nullptr, 1};
    fixture = {&host, &pages};
    check(ui_page_registry_initialize(&pages, {page_storage, 8}) &&
        native_plugin_host_initialize(&host, &pages, {slots, 2}, policy), "settings dispatch host initializes");
    auto def = definition(1);
    check(native_plugin_host_enable(&host, def, {state_a, 64}).error == NativePluginError::duplicate_owner &&
        !fixture.starts, "reserved native owner is rejected before built-in registration or plugin start");
    check(ui_page_registry_register(&pages, {{10, 1}, utf8_literal("Built-in"), nullptr}).ok(),
        "settings dispatch preserves independent native pages");
    def = definition(20);
    def.start = start_settings;
    def.activate_settings = activate_settings;
    check(native_plugin_host_enable(&host, def, {state_a, 64}).ok() &&
        ui_page_registry_find(pages, {20, 1})->context == &fixture.content,
        "settings descriptors can remain borrowed from the live module rather than host state");
    const auto own = fixture.services;
    auto peer_def = definition(21);
    check(native_plugin_host_enable(&host, peer_def, {state_b, 64}).ok(), "dispatch peer enables independently");
    fixture.peer_services = fixture.services;
    fixture.services = own;
    fixture.activation = {{UiSettingsTargetKind::control, {20, 1}, 1}, UiSettingsControlKind::toggle, true, 0};
    auto& request = fixture.activation;
    auto& token = fixture.token;
    check(native_plugin_host_settings_token(&host, {20, 1}, &token).ok() && token.generation &&
        native_plugin_host_settings_token(&host, {21, 1}, nullptr).error == NativePluginError::invalid_argument,
        "dispatch snapshot captures the live owner generation and descriptor revisions");
    NativePluginUiToken ignored{};
    check(native_plugin_host_settings_token(&host, {21, 1}, &ignored).error == NativePluginError::unsupported &&
        native_plugin_host_settings_token(&host, {999, 1}, &ignored).error == NativePluginError::inactive &&
        native_plugin_host_settings_token(&host, {20, 999}, &ignored).error == NativePluginError::not_found,
        "optional callbacks and unowned or missing pages never receive settings requests");
    auto bad_request = request;
    bad_request.target.kind = UiSettingsTargetKind::page;
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::invalid_argument,
        "dispatch only accepts semantic control activations");
    bad_request = request;
    bad_request.target.page.owner_id = 21;
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::stale,
        "dispatch token cannot cross owner namespaces");
    bad_request = request;
    bad_request.target.page.page_id = 999;
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::stale,
        "dispatch token cannot cross page IDs");
    auto bad_token = token;
    bad_token.generation = 0;
    check(native_plugin_host_activate_settings(&host, bad_token, request).error == NativePluginError::stale,
        "dispatch refuses unobserved generations");
    bad_token = token;
    ++bad_token.generation;
    check(native_plugin_host_activate_settings(&host, bad_token, request).error == NativePluginError::stale,
        "dispatch refuses stale slot generations");
    bad_request = request;
    bad_request.target.control_id = 999;
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::stale,
        "dispatch rejects removed or unknown controls");
    bad_request.target.control_id = 4;
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::stale,
        "dispatch rejects currently disabled controls");
    bad_request = request;
    bad_request.kind = UiSettingsControlKind::choice;
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::stale,
        "dispatch revalidates current control kind");
    bad_request.kind = static_cast<UiSettingsControlKind>(255);
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::invalid_argument,
        "dispatch rejects malformed control kinds");
    bad_request = {{UiSettingsTargetKind::control, {20, 1}, 2}, UiSettingsControlKind::choice, false, 2};
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::invalid_argument,
        "dispatch validates desired choice bounds");
    fixture.controls[1].selected_choice = 2;
    bad_request.selected_choice = 1;
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::invalid_argument,
        "dispatch validates the current choice projection");
    fixture.controls[1].selected_choice = 0;
    fixture.controls[1].choices.data = nullptr;
    check(native_plugin_host_activate_settings(&host, token, bad_request).error == NativePluginError::invalid_argument,
        "dispatch rejects malformed choice storage");
    fixture.controls[1].choices = {plugin_choices, 2};
    fixture.content.format = 0;
    check(native_plugin_host_activate_settings(&host, token, request).error == NativePluginError::unsupported &&
        native_plugin_host_settings_token(&host, {20, 1}, &ignored).error == NativePluginError::unsupported,
        "dispatch rejects contexts with an incompatible settings format");
    fixture.content.format = kNativePluginSettingsFormatHost;
    fixture.content.controls.count = 8193;
    check(native_plugin_host_activate_settings(&host, token, request).error == NativePluginError::invalid_argument,
        "dispatch bounds descriptor scans");
    fixture.content.controls.count = 4;
    fixture.content.controls.data = nullptr;
    check(native_plugin_host_activate_settings(&host, token, request).error == NativePluginError::invalid_argument,
        "dispatch rejects missing control storage");
    fixture.content.controls.data = fixture.controls;
    fixture.controls[3].id = 1;
    check(native_plugin_host_activate_settings(&host, token, request).error == NativePluginError::invalid_argument,
        "dispatch rejects ambiguous target IDs");
    fixture.controls[3].id = 4;
    ++fixture.content.revision;
    check(native_plugin_host_activate_settings(&host, token, request).error == NativePluginError::stale &&
        !fixture.activations, "descriptor revisions invalidate pending requests before any callback runs");
    check(native_plugin_host_settings_token(&host, {20, 1}, &token).ok(), "dispatch snapshot refreshes explicitly");
    const auto changed = native_plugin_host_activate_settings(&host, token, request);
    check(changed.ok() && changed.changed && fixture.controls[0].checked && fixture.activations == 1 &&
        fixture.activation_reentry_blocked && fixture.peer_service_reentry_blocked && !host.busy && !slots[0].service_busy,
        "toggle dispatch applies desired value while lifecycle, own and peer service reentry remain blocked");
    check(native_plugin_host_activate_settings(&host, token, request).error == NativePluginError::stale,
        "callback revision change invalidates the prior projection token");
    check(native_plugin_host_settings_token(&host, {20, 1}, &token).ok() &&
        !native_plugin_host_activate_settings(&host, token, request).changed && fixture.activations == 2,
        "accepted unchanged values report no change");
    request = {{UiSettingsTargetKind::control, {20, 1}, 2}, UiSettingsControlKind::choice, false, 1};
    check(native_plugin_host_activate_settings(&host, token, request).changed && fixture.controls[1].selected_choice == 1,
        "choice dispatch uses the requested bounded value");
    request = {{UiSettingsTargetKind::control, {20, 1}, 3}, UiSettingsControlKind::action, false, 0};
    check(native_plugin_host_settings_token(&host, {20, 1}, &token).ok() &&
        native_plugin_host_activate_settings(&host, token, request).changed,
        "action dispatch returns the cooperative module change result");
    fixture.reject_activation = true;
    check(native_plugin_host_settings_token(&host, {20, 1}, &token).ok(), "rejected action uses a fresh snapshot");
    const auto rejected = native_plugin_host_activate_settings(&host, token, request);
    check(rejected.error == NativePluginError::activation_failed && !rejected.changed && !host.busy,
        "callback rejection reports failure and releases dispatch guards");
    fixture.reject_activation = false;
    const auto old_token = token;
    check(own.unregister_page(own.binding, own.generation, 1).ok() && fixture.peer_service_reentry_blocked &&
        native_plugin_host_activate_settings(&host, old_token, request).error == NativePluginError::stale,
        "page detach blocks peer services and expires pending actions before removing borrowed descriptors");
    check(own.register_page(own.binding, own.generation, 1, utf8_literal("Settings"), &fixture.content).ok() &&
        native_plugin_host_activate_settings(&host, old_token, request).error == NativePluginError::stale,
        "same-key page replacement cannot revive a removed-page token");
    check(native_plugin_host_settings_token(&host, {20, 1}, &token).ok() &&
        native_plugin_host_disable(&host, 20).ok() && !slots[0].definition.activate_settings &&
        native_plugin_host_activate_settings(&host, token, request).error == NativePluginError::stale,
        "disable removes the callback and rejects module requests after descriptor teardown");
    fixture.fail_start = true;
    check(native_plugin_host_enable(&host, def, {state_a, 64}).error == NativePluginError::start_failed &&
        !ui_page_registry_find(pages, {20, 1}) && !slots[0].definition.activate_settings,
        "failed settings start rolls back borrowed pages and activation callbacks");
    fixture.fail_start = false;
    check(native_plugin_host_enable(&host, def, {state_a, 64}).ok() &&
        native_plugin_host_activate_settings(&host, token, request).error == NativePluginError::stale &&
        native_plugin_host_disable_all(&host).ok() && pages.count == 1,
        "same-owner reload never revives old activation generations and cleanup preserves built-in pages");
}

void test_settings_revision_exhaustion(void (*check)(bool, const char*) noexcept) noexcept {
    UiPage page_storage[8]{};
    UiPageRegistry pages{};
    NativePluginSlot slots[2]{};
    NativePluginHost host{};
    alignas(64) u8 state[64]{};
    const NativePluginPolicy policy{native_plugin_capability_pages, 2, 64, 128, detach_owner, nullptr};
    fixture = {&host, &pages};
    auto def = definition(20);
    def.start = start_settings;
    def.activate_settings = activate_settings;
    check(ui_page_registry_initialize(&pages, {page_storage, 8}) &&
        ui_page_registry_register(&pages, {{10, 1}, utf8_literal("Built-in"), nullptr}).ok() &&
        native_plugin_host_initialize(&host, &pages, {slots, 2}, policy) &&
        native_plugin_host_enable(&host, def, {state, 64}).ok(),
        "settings revision exhaustion fixture enables an owned settings page");
    const auto services = fixture.services;
    fixture.peer_services = services;
    fixture.activation = {{UiSettingsTargetKind::control, {20, 1}, 1}, UiSettingsControlKind::toggle, false, 0};
    auto& token = fixture.token;
    const auto request = fixture.activation;
    pages.revision = ~u32{0} - 1;
    fixture.content.revision = ~u32{0} - 1;
    check(native_plugin_host_settings_token(&host, {20, 1}, &token).ok() &&
        native_plugin_host_activate_settings(&host, token, request).ok() && fixture.activations == 1,
        "the last distinguishable settings registry and content revisions still dispatch");

    NativePluginUiToken saturated = token;
    pages.revision = ~u32{0};
    saturated.registry_revision = pages.revision;
    NativePluginUiToken rejected = token;
    check(native_plugin_host_settings_token(&host, {20, 1}, &rejected).error == NativePluginError::stale &&
        !rejected.generation && !rejected.page.owner_id &&
        native_plugin_host_activate_settings(&host, token, request).error == NativePluginError::stale &&
        native_plugin_host_activate_settings(&host, saturated, request).error == NativePluginError::stale &&
        fixture.activations == 1,
        "exhausted registry refuses new, pending and synthesized matching settings tokens before callback entry");
    check(services.unregister_page(services.binding, services.generation, 1).ok() &&
        services.register_page(services.binding, services.generation, 1,
            utf8_literal("Replacement"), &fixture.content).ok() && pages.revision == ~u32{0} &&
        native_plugin_host_activate_settings(&host, saturated, request).error == NativePluginError::stale &&
        fixture.activations == 1,
        "same-key settings replacement at registry exhaustion cannot revive an old matching token");

    // Isolate the independently exhausted module content counter.
    pages.revision = 100;
    fixture.content.revision = ~u32{0};
    saturated.registry_revision = pages.revision;
    saturated.content_revision = fixture.content.revision;
    rejected = saturated;
    check(native_plugin_host_settings_token(&host, {20, 1}, &rejected).error == NativePluginError::stale &&
        !rejected.generation && !rejected.content_revision &&
        native_plugin_host_activate_settings(&host, saturated, request).error == NativePluginError::stale &&
        fixture.activations == 1,
        "exhausted content refuses capture and matching token dispatch without invoking module code");
    pages.revision = ~u32{0};
    check(native_plugin_host_disable(&host, 20).ok() && !host.active_count && pages.count == 1 &&
        !slots[0].definition.activate_settings && fixture.stops == 1 && fixture.stop_saw_no_pages &&
        zeroed({state, 32}),
        "registry and content exhaustion still permit detach, stop and erasure of plugin state");
}

} // namespace

void test_plugin_host(void (*check)(bool, const char*) noexcept) noexcept {
    UiPage page_storage[8]{};
    UiPageRegistry pages{};
    NativePluginSlot slots[2]{};
    NativePluginHost host{};
    alignas(64) u8 state_a[64]{};
    alignas(64) u8 state_b[64]{};
    const NativePluginPolicy policy{native_plugin_capability_pages, 2, 64, 80, detach_owner, nullptr};
    fixture = {&host, &pages};
    check(ui_page_registry_initialize(&pages, {page_storage, 8}) &&
        ui_page_registry_register(&pages, {{10, 1}, utf8_literal("Built-in"), nullptr}).ok() &&
        native_plugin_host_initialize(&host, &pages, {slots, 2}, policy), "plugin host fixed storage initializes");

    auto def = definition(20);
    auto invalid = def;
    invalid.required_capabilities |= 1u << 31;
    check(native_plugin_host_enable(&host, invalid, {state_a, 64}).error == NativePluginError::capability_denied,
        "plugin host rejects unknown and ungranted capabilities");
    invalid = def;
    invalid.page_limit = 3;
    check(native_plugin_host_enable(&host, invalid, {state_a, 64}).error == NativePluginError::budget,
        "plugin host rejects excessive page declaration");
    invalid = def;
    invalid.state_bytes = 65;
    check(native_plugin_host_enable(&host, invalid, {state_a, 64}).error == NativePluginError::budget,
        "plugin host rejects excessive runtime memory declaration");
    check(native_plugin_host_enable(&host, def, {state_a + 1, 63}).error == NativePluginError::invalid_argument &&
        native_plugin_host_enable(&host, def, {state_a, 31}).error == NativePluginError::budget && !fixture.starts,
        "plugin host rejects misaligned or insufficient state before startup");
    check(native_plugin_host_enable(&host, def, {reinterpret_cast<u8*>(~usize{0} - 15), 32}).error ==
        NativePluginError::invalid_argument, "plugin host refuses wrapping caller state address range");
    invalid = def;
    invalid.state_alignment = 1;
    check(native_plugin_host_enable(&host, invalid, {reinterpret_cast<u8*>(&host), 32}).error ==
        NativePluginError::invalid_argument &&
        native_plugin_host_enable(&host, invalid, {reinterpret_cast<u8*>(slots), 32}).error ==
        NativePluginError::invalid_argument &&
        native_plugin_host_enable(&host, invalid, {reinterpret_cast<u8*>(&pages), 32}).error ==
        NativePluginError::invalid_argument &&
        native_plugin_host_enable(&host, invalid, {reinterpret_cast<u8*>(page_storage), 32}).error ==
        NativePluginError::invalid_argument &&
        native_plugin_host_enable(&host, invalid, {reinterpret_cast<u8*>(&invalid), 32}).error ==
        NativePluginError::invalid_argument, "plugin state cannot alias host, registry, slots or definition");
    invalid = definition(10);
    check(native_plugin_host_enable(&host, invalid, {state_a, 64}).error == NativePluginError::duplicate_owner,
        "plugin host refuses an existing built-in owner namespace");

    for (u8& byte : state_a) byte = 0xCC;
    check(native_plugin_host_enable(&host, def, {state_a, 64}).ok() && fixture.state_was_zero &&
        host.active_count == 1 && host.state_bytes == 32 && native_plugin_host_find(host, 20),
        "plugin startup receives zeroed budgeted caller memory");
    check(!native_plugin_host_initialize(&host, &pages, {slots, 2}, policy) && host.active_count == 1 &&
        native_plugin_host_find(host, 20), "reinitialization cannot erase an enabled plugin");
    check(state_a[32] == 0xCC && ui_page_registry_find(pages, {20, 1})->context == state_a,
        "plugin page borrows context and host touches only declared state bytes");
    const NativePluginServices original = fixture.services;
    check(native_plugin_host_enable(&host, def, {state_b, 64}).error == NativePluginError::duplicate_owner,
        "plugin host rejects an already enabled owner");
    invalid = definition(21);
    check(native_plugin_host_enable(&host, invalid, {state_a + 16, 48}).error == NativePluginError::invalid_argument,
        "plugin host rejects overlapping caller state across plugins");
    check(original.register_page(original.binding, original.generation, 2, utf8_literal("Second"), nullptr).ok() &&
        original.register_page(original.binding, original.generation, 3, utf8_literal("Third"), nullptr).error ==
            NativePluginError::budget, "plugin page budget is enforced on service calls");
    check(original.unregister_page(original.binding, original.generation, 2).ok() &&
        !ui_page_registry_find(pages, {10, 2}) && fixture.detaches == 1 &&
        fixture.lifecycle_reentry_blocked && fixture.detach_service_reentry_blocked,
        "unselected plugin page removal detaches borrowing popups and keeps owner namespace");
    check(ui_page_registry_select(&pages, {20, 1}).ok() &&
        original.unregister_page(original.binding, original.generation, 1).ok() &&
        fixture.detach_saw_live_page && fixture.lifecycle_reentry_blocked && fixture.detach_service_reentry_blocked,
        "selected plugin removal detaches borrowed UI and blocks lifecycle and service reentry");
    check(original.register_page(original.binding, original.generation, 1,
        utf8_literal("Plugin settings"), state_a).ok(), "plugin can register again after page removal");
    check(native_plugin_host_disable(&host, 20).ok() && fixture.stop_saw_no_pages &&
        fixture.detach_saw_live_page && fixture.lifecycle_reentry_blocked &&
        !host.active_count && !host.state_bytes && pages.count == 1 && zeroed({state_a, 32}),
        "plugin stop removes UI before callback and clears caller state and budget");
    check(!slots[0].definition.start && !slots[0].host && !slots[0].state &&
        original.register_page(original.binding, original.generation, 1, utf8_literal("Stale"), nullptr).error ==
            NativePluginError::inactive, "disabled slot retains no plugin pointers and rejects stale services");

    fixture.fail_start = true;
    const u32 stops_before_failure = fixture.stops;
    check(native_plugin_host_enable(&host, def, {state_a, 64}).error == NativePluginError::start_failed &&
        fixture.stops == stops_before_failure + 1 && !host.active_count && pages.count == 1 &&
        zeroed({state_a, 32}), "failed plugin startup rolls back pages, stop, state and accounting");
    fixture.fail_start = false;
    check(native_plugin_host_enable(&host, def, {state_a, 64}).ok() &&
        original.unregister_page(original.binding, original.generation, 1).error == NativePluginError::inactive &&
        ui_page_registry_find(pages, {20, 1}), "slot reuse rejects service copies from previous generation");
    invalid = definition(21, false);
    invalid.state_bytes = 64;
    check(native_plugin_host_enable(&host, invalid, {state_b, 64}).error == NativePluginError::budget,
        "plugin host enforces aggregate runtime memory budget");
    invalid.state_bytes = 32;
    check(native_plugin_host_enable(&host, invalid, {state_b, 64}).ok(), "second plugin has independent state");
    const auto no_pages = fixture.services;
    check(no_pages.register_page(no_pages.binding, no_pages.generation, 1, utf8_literal("Denied"), nullptr).error ==
        NativePluginError::capability_denied, "plugin without page capability cannot contribute pages");
    invalid = definition(22, false);
    invalid.state_bytes = 0;
    check(native_plugin_host_enable(&host, invalid, {}).error == NativePluginError::capacity,
        "plugin host honors caller slot capacity");
    check(native_plugin_host_disable_all(&host).ok() && !host.active_count && !host.state_bytes &&
        pages.count == 1 && zeroed({state_a, 32}) && zeroed({state_b, 32}),
        "disable all preserves built-in pages and releases all plugin budgets");

    const u64 last_generation = host.generation;
    check(native_plugin_host_initialize(&host, &pages, {slots, 2}, policy) && host.generation == last_generation &&
        native_plugin_host_enable(&host, def, {state_a, 64}).ok() &&
        original.register_page(original.binding, original.generation, 2, utf8_literal("Stale"), nullptr).error ==
            NativePluginError::inactive, "host reinitialization cannot resurrect old service generation");
    check(native_plugin_host_disable_all(&host).ok() &&
        native_plugin_host_disable(&host, 20).error == NativePluginError::not_found,
        "plugin teardown is complete and inactive owner reports not found");
    test_settings_dispatch(check);
    test_settings_revision_exhaustion(check);
}

} // namespace maicord
