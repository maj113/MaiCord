#include "maicord/app/plugin_host.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/plugin_settings_projection.hpp"
#include "maicord/app/plugin_call_ledger.hpp"
#include "maicord/core/byte_range.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
namespace {

NativePluginResult result(NativePluginError error) noexcept {
    return {error};
}
NativePluginResult settings_result(NativePluginSettingsError error) noexcept {
    return result(error == NativePluginSettingsError::none ? NativePluginError::none :
        error == NativePluginSettingsError::unsupported ? NativePluginError::unsupported :
        error == NativePluginSettingsError::capacity ? NativePluginError::capacity : NativePluginError::invalid_argument);
}

NativePluginResult page_result(UiPageResult page) noexcept {
    return {page.ok() ? NativePluginError::none : NativePluginError::page};
}

NativePluginSlot* service_slot(void* binding, u64 generation) noexcept {
    auto* slot = static_cast<NativePluginSlot*>(binding);
    return slot && slot->host && !slot->service_busy && slot->generation == generation &&
        (!slot->host->busy || slot->phase == NativePluginPhase::starting) &&
        (slot->phase == NativePluginPhase::starting || slot->phase == NativePluginPhase::enabled) ?
        slot : nullptr;
}

NativePluginResult WINAPI register_page(void* binding, u64 generation, u64 page_id,
                                        Utf8View label, void* context) noexcept {
    NativePluginSlot* slot = service_slot(binding, generation);
    if (!slot) return result(NativePluginError::inactive);
    if (!(slot->definition.required_capabilities & native_plugin_capability_pages))
        return result(NativePluginError::capability_denied);
    if (slot->page_count >= slot->definition.page_limit) return result(NativePluginError::budget);
    const UiPageResult page = ui_page_registry_register(slot->host->pages,
        {{slot->definition.owner_id, page_id}, label, context});
    if (page.ok()) ++slot->page_count;
    return page_result(page);
}

NativePluginResult WINAPI unregister_page(void* binding, u64 generation, u64 page_id) noexcept {
    NativePluginSlot* slot = service_slot(binding, generation);
    if (!slot) return result(NativePluginError::inactive);
    if (!(slot->definition.required_capabilities & native_plugin_capability_pages))
        return result(NativePluginError::capability_denied);
    NativePluginHost* host = slot->host;
    const bool was_busy = host->busy;
    host->busy = true;
    slot->service_busy = true;
    // Any page can have a borrowing scene or popup, including unselected pages.
    if (ui_page_registry_find(*host->pages, {slot->definition.owner_id, page_id}) &&
        host->policy.detach_owner) {
        host->policy.detach_owner(host->policy.detach_context, slot->definition.owner_id);
    }
    const UiPageResult page = ui_page_registry_unregister(host->pages,
        {slot->definition.owner_id, page_id});
    if (page.ok() && slot->page_count) --slot->page_count;
    slot->service_busy = false;
    host->busy = was_busy;
    return page_result(page);
}

// Calls into DLL code are timed and marked; adapter trampolines are host code
// that times the plugin entry it makes on its own.
struct CallScope {
    PluginCallLedger* ledger;
    PluginCallTicket ticket;
    CallScope(NativePluginHost* host, const NativePluginDefinition& definition, PluginCallKind kind) noexcept
        : ledger(host->ledger), ticket(plugin_call_begin(host->ledger, definition.owner_id, kind)) {}
    ~CallScope() noexcept { plugin_call_end(ledger, ticket); }
};

void stop_slot(NativePluginHost* host, NativePluginSlot* slot) noexcept {
    slot->phase = NativePluginPhase::stopping;
    if (host->policy.detach_owner)
        host->policy.detach_owner(host->policy.detach_context, slot->definition.owner_id);
    ui_page_registry_unregister_owner(host->pages, slot->definition.owner_id);
    slot->page_count = 0;
    {
        CallScope call(host, slot->definition, PluginCallKind::stop);
        slot->definition.stop(slot->state, slot->definition.state_bytes);
    }
    clear_bytes(slot->state, slot->definition.state_bytes);
    host->state_bytes -= slot->definition.state_bytes;
    --host->active_count;
    // Dormant slots retain no plugin callbacks, contexts or owner metadata.
    reset_value(slot);
}

} // namespace

bool native_plugin_host_initialize(NativePluginHost* host, UiPageRegistry* pages,
    Slice<NativePluginSlot> slots, NativePluginPolicy policy) noexcept {
    if (!host || host->active_count || host->state_bytes || host->busy || !pages ||
        !pages->storage.data || !slots.data || !slots.count ||
        (policy.granted_capabilities & ~native_plugin_known_capabilities)) return false;
    const u64 generation = host->generation;
    auto* sdk = host->sdk;
    auto* ledger = host->ledger;
    *host = {slots, pages, policy, generation, 0, 0, false, sdk, ledger};
    for (u32 index = 0; index < slots.count; ++index) reset_value(&slots[index]);
    return true;
}

NativePluginResult native_plugin_host_enable(NativePluginHost* host,
    const NativePluginDefinition& definition, Slice<u8> state_storage) noexcept {
    if (!host || !host->slots.data || !host->pages) return result(NativePluginError::invalid_argument);
    if (host->busy) return result(NativePluginError::busy);
    if (!definition.owner_id || !definition.start || !definition.stop ||
        !byte_range_valid(state_storage.data, definition.state_bytes) ||
        !definition.state_alignment || (definition.state_alignment & (definition.state_alignment - 1)) ||
        definition.state_alignment > 64 ||
        (definition.state_bytes && (!state_storage.data ||
            (reinterpret_cast<usize>(state_storage.data) & (definition.state_alignment - 1)))))
        return result(NativePluginError::invalid_argument);
    if (definition.owner_id == host->policy.reserved_owner_id)
        return result(NativePluginError::duplicate_owner);
    if (definition.required_capabilities & (~host->policy.granted_capabilities | ~native_plugin_known_capabilities))
        return result(NativePluginError::capability_denied);
    if (definition.page_limit > host->policy.max_pages_per_plugin ||
        (definition.page_limit && !(definition.required_capabilities & native_plugin_capability_pages)) ||
        definition.state_bytes > state_storage.count ||
        definition.state_bytes > host->policy.max_state_bytes_per_plugin ||
        host->state_bytes > host->policy.max_total_state_bytes ||
        definition.state_bytes > host->policy.max_total_state_bytes - host->state_bytes)
        return result(NativePluginError::budget);
    if (byte_ranges_overlap(state_storage.data, definition.state_bytes, host, sizeof(*host)) ||
        byte_ranges_overlap(state_storage.data, definition.state_bytes, host->slots.data,
            static_cast<usize>(host->slots.count) * sizeof(NativePluginSlot)) ||
        byte_ranges_overlap(state_storage.data, definition.state_bytes, host->pages, sizeof(*host->pages)) ||
        byte_ranges_overlap(state_storage.data, definition.state_bytes, host->pages->storage.data,
            static_cast<usize>(host->pages->storage.count) * sizeof(UiPage)) ||
        byte_ranges_overlap(state_storage.data, definition.state_bytes, &definition, sizeof(definition)))
        return result(NativePluginError::invalid_argument);
    NativePluginSlot* free_slot = nullptr;
    for (u32 index = 0; index < host->slots.count; ++index) {
        NativePluginSlot& slot = host->slots[index];
        if (slot.phase == NativePluginPhase::empty) {
            if (!free_slot) free_slot = &slot;
        } else {
            if (slot.definition.owner_id == definition.owner_id) return result(NativePluginError::duplicate_owner);
            if (byte_ranges_overlap(slot.state, slot.definition.state_bytes, state_storage.data,
                                    definition.state_bytes))
                return result(NativePluginError::invalid_argument);
        }
    }
    // Refuse namespaces already owned by built-in pages or another host.
    for (u32 index = 0; index < host->pages->count; ++index) {
        if (ui_page_registry_at(*host->pages, index)->key.owner_id == definition.owner_id)
            return result(NativePluginError::duplicate_owner);
    }
    if (!free_slot || host->generation == ~u64{0}) return result(NativePluginError::capacity);
    host->busy = true;
    *free_slot = {definition, host, definition.state_bytes ? state_storage.data : nullptr,
        ++host->generation, 0, NativePluginPhase::starting, false};
    ++host->active_count;
    host->state_bytes += definition.state_bytes;
    clear_bytes(free_slot->state, definition.state_bytes);
    const NativePluginServices services{
        free_slot, free_slot->generation, register_page, unregister_page};
    bool started = false;
    {
        CallScope call(host, definition, PluginCallKind::start);
        started = definition.start(services, free_slot->state, definition.state_bytes);
    }
    if (started) free_slot->phase = NativePluginPhase::enabled;
    else stop_slot(host, free_slot);
    host->busy = false;
    return result(started ? NativePluginError::none : NativePluginError::start_failed);
}

NativePluginResult native_plugin_host_disable(NativePluginHost* host, u64 owner_id) noexcept {
    if (!host || !host->slots.data || !owner_id) return result(NativePluginError::invalid_argument);
    if (host->busy) return result(NativePluginError::busy);
    for (u32 index = 0; index < host->slots.count; ++index) {
        NativePluginSlot& slot = host->slots[index];
        if (slot.phase != NativePluginPhase::empty && slot.definition.owner_id == owner_id) {
            host->busy = true;
            stop_slot(host, &slot);
            host->busy = false;
            return result(NativePluginError::none);
        }
    }
    return result(NativePluginError::not_found);
}

NativePluginResult native_plugin_host_disable_all(NativePluginHost* host) noexcept {
    if (!host || !host->slots.data) return result(NativePluginError::invalid_argument);
    if (host->busy) return result(NativePluginError::busy);
    host->busy = true;
    for (u32 index = host->slots.count; index; --index) {
        if (host->slots[index - 1].phase != NativePluginPhase::empty)
            stop_slot(host, &host->slots[index - 1]);
    }
    host->busy = false;
    return result(NativePluginError::none);
}

NativePluginResult native_plugin_host_settings_token(const NativePluginHost* host,
    UiPageKey page, NativePluginUiToken* token) noexcept {
    if (token) reset_value(token);
    if (!host || !host->slots.data || !host->pages || !token || !page.owner_id || !page.page_id)
        return result(NativePluginError::invalid_argument);
    if (host->busy) return result(NativePluginError::busy);
    const auto* slot = native_plugin_host_find(*host, page.owner_id);
    if (!slot) return result(NativePluginError::inactive);
    if (!(slot->definition.required_capabilities & native_plugin_capability_pages) ||
        !slot->definition.activate_settings) return result(NativePluginError::unsupported);
    const auto* descriptor = ui_page_registry_find(*host->pages, page);
    if (!descriptor) return result(NativePluginError::not_found);
    if (!descriptor->context) return result(NativePluginError::invalid_argument);
    NativePluginSettingsSnapshot content{};
    const auto snapshot = native_plugin_settings_snapshot(descriptor->context, &content);
    if (snapshot != NativePluginSettingsError::none) return settings_result(snapshot);
    // Saturated revisions no longer distinguish removal/replacement or content
    // changes. Never issue an activation token that can survive that exhaustion.
    if (host->pages->revision == ~u32{0} || content.revision == ~u32{0})
        return result(NativePluginError::stale);
    *token = {page, slot->generation, host->pages->revision, content.revision};
    return result(NativePluginError::none);
}

NativePluginResult native_plugin_host_activate_settings(NativePluginHost* host,
    NativePluginUiToken token, UiSettingsActivation activation) noexcept {
    if (!host || !host->slots.data || !host->pages ||
        activation.target.kind != UiSettingsTargetKind::control ||
        !activation.target.page.owner_id || !activation.target.page.page_id ||
        !activation.target.control_id ||
        (activation.kind > UiSettingsControlKind::action && activation.kind != UiSettingsControlKind::slider &&
         activation.kind != UiSettingsControlKind::notice))
        return result(NativePluginError::invalid_argument);
    if (host->busy) return result(NativePluginError::busy);
    if (!token.generation || host->pages->revision == ~u32{0} ||
        !ui_page_key_equal(token.page, activation.target.page) ||
        token.registry_revision != host->pages->revision) return result(NativePluginError::stale);
    NativePluginSlot* slot = nullptr;
    for (u32 index = 0; index < host->slots.count; ++index) {
        auto& candidate = host->slots[index];
        if (candidate.phase == NativePluginPhase::enabled &&
            candidate.definition.owner_id == activation.target.page.owner_id) { slot = &candidate; break; }
    }
    if (!slot || slot->generation != token.generation) return result(NativePluginError::stale);
    if (!(slot->definition.required_capabilities & native_plugin_capability_pages) ||
        !slot->definition.activate_settings) return result(NativePluginError::unsupported);
    const auto* page = ui_page_registry_find(*host->pages, activation.target.page);
    if (!page || !page->context) return result(NativePluginError::stale);
    NativePluginSettingsSnapshot content{};
    const auto snapshot = native_plugin_settings_snapshot(page->context, &content);
    if (snapshot != NativePluginSettingsError::none) return settings_result(snapshot);
    if (content.revision == ~u32{0} || content.revision != token.content_revision)
        return result(NativePluginError::stale);
    NativePluginSettingsControl control{};
    bool found_control = false;
    for (u32 index = 0; index < content.controls.count; ++index) {
        NativePluginSettingsControl candidate{};
        const auto read = native_plugin_settings_control(content, index, &candidate);
        if (read != NativePluginSettingsError::none) return settings_result(read);
        if (candidate.id == activation.target.control_id) {
            if (found_control) return result(NativePluginError::invalid_argument);
            control = candidate;
            found_control = true;
        }
    }
    if (!found_control || !control.enabled || static_cast<UiSettingsControlKind>(control.kind) != activation.kind)
        return result(NativePluginError::stale);
    if ((control.kind == NativePluginSettingsKind::choice || control.kind == NativePluginSettingsKind::slider) &&
        activation.selected_choice >= control.choices.count)
        return result(NativePluginError::invalid_argument);
    host->busy = true;
    slot->service_busy = true;
    const NativePluginSettingsActivation copied{
        {NativePluginSettingsTargetKind::control,
            {activation.target.page.owner_id, activation.target.page.page_id}, activation.target.control_id},
        static_cast<NativePluginSettingsKind>(activation.kind), activation.checked, activation.selected_choice};
    NativePluginActivationResult outcome{};
    {
        CallScope call(host, slot->definition, PluginCallKind::settings);
        outcome = slot->definition.activate_settings(slot->state, slot->definition.state_bytes, copied);
    }
    // No borrowed page/control pointer is read after module code returns.
    slot->service_busy = false;
    host->busy = false;
    return {outcome.accepted ? NativePluginError::none : NativePluginError::activation_failed,
        outcome.accepted && outcome.changed};
}

const NativePluginSlot* native_plugin_host_find(const NativePluginHost& host, u64 owner_id) noexcept {
    if (!owner_id || !host.slots.data) return nullptr;
    for (u32 index = 0; index < host.slots.count; ++index) {
        if (host.slots[index].phase == NativePluginPhase::enabled &&
            host.slots[index].definition.owner_id == owner_id) return &host.slots[index];
    }
    return nullptr;
}

} // namespace maicord
