// Descriptor buttons (mc_button_def): chat bar, message hover bar, account panel, channel header and the two
// server list ends. The UI reads them as slot nodes; their visibility predicates run in the application's
// prepare pass and at enable, never while painting, and a click runs the plugin's handler as a gesture.
#include "surfaces_internal.hpp"
#include "maicord/app/plugins/call_ledger.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/plugins/slots.hpp"

namespace maicord {

namespace plugin_surfaces_detail {
namespace {

static_assert(MC_BUTTON_MAX <= 16 && sizeof(Plugin::buttons) / sizeof(ButtonState) >= MC_BUTTON_MAX);

// The context the message-independent predicates last ran for (the open channel).
u64 g_channel_id, g_guild_id;

u32 slot_surface(u32 surface) noexcept {
    switch (surface) {
    case MC_SURFACE_CHAT_BAR: return kUiPluginSlotComposerToolbar;
    case MC_SURFACE_MESSAGE_POPOVER: return kUiPluginSlotMessageHover;
    case MC_SURFACE_ACCOUNT_PANEL: return kUiPluginSlotAccountPanel;
    case MC_SURFACE_HEADER: return kUiPluginSlotHeaderToolbar;
    case MC_SURFACE_SERVER_LIST_TOP: return kUiPluginSlotServerTop;
    case MC_SURFACE_SERVER_LIST_BOTTOM: return kUiPluginSlotServerBottom;
    default: return ~0u;
    }
}

const mc_button_def& definition(const Plugin& plugin, u32 index) noexcept {
    return plugin.binding->image.descriptor->buttons[index];
}

struct VisibleCall {
    mc_button_visible_fn function;
    const mc_button_event* event;
    bool shown;
    static mc_status run(mc_ctx* ctx, void* argument) noexcept {
        auto* call = static_cast<VisibleCall*>(argument);
        call->shown = call->function(ctx, call->event) != 0;
        return MC_OK;
    }
};
// A predicate that overruns its budget counts as "hidden".
bool run_visible(Plugin& plugin, u32 index, const mc_message* message) noexcept {
    const mc_button_def& def = definition(plugin, index);
    if (plugin.binding->struck_out) return false;
    const mc_button_event event{g_channel_id, g_guild_id, message, index, 0};
    VisibleCall call{def.visible, &event, false};
    const PluginCallResult ran = plugin_binding_call(plugin.binding, PluginCallKind::decorate,
                                                     PluginCallClass::decorate, 0, VisibleCall::run, &call);
    return !ran.overran && call.shown;
}

void evaluate_static(Plugin& plugin) noexcept {
    plugin.shown = 0;
    for (u32 i = 0; i < plugin.button_count; ++i) {
        const mc_button_def& def = definition(plugin, i);
        if (def.surface == MC_SURFACE_MESSAGE_POPOVER) continue;
        if (!def.visible || run_visible(plugin, i, nullptr)) plugin.shown |= 1u << i;
    }
}

// The answer of the hover bar predicate for a message, from the plugin's memory of recent hovers.
const VisibleMemory* remembered(const Plugin& plugin, u64 message_id) noexcept {
    for (const VisibleMemory& memory : plugin.visible)
        if (memory.message_id == message_id) return &memory;
    return nullptr;
}
bool popover_shown(const Plugin& plugin, u32 index, u64 message_id) noexcept {
    if (!definition(plugin, index).visible) return true;
    const VisibleMemory* memory = remembered(plugin, message_id);
    return memory && (memory->bits >> index & 1u);
}

struct ClickCall {
    mc_button_fn function;
    const mc_button_event* event;
    static mc_status run(mc_ctx* ctx, void* argument) noexcept {
        const auto* call = static_cast<ClickCall*>(argument);
        return call->function(ctx, call->event);
    }
};

Plugin* by_owner(u64 owner_id) noexcept {
    for (u32 i = 0; i < g_plugin_count; ++i)
        if (g_plugins[i].binding->owner_id == owner_id) return &g_plugins[i];
    return nullptr;
}

} // namespace

// A plugin may set a button's state in start, before the host has published its buttons.
void buttons_init(Plugin& plugin) noexcept {
    const mc_descriptor& descriptor = *plugin.binding->image.descriptor;
    if (plugin.button_count) return;
    plugin.button_count = descriptor.button_count < MC_BUTTON_MAX ? descriptor.button_count : MC_BUTTON_MAX;
    for (u32 i = 0; i < plugin.button_count; ++i) {
        const mc_str icon = descriptor.buttons[i].icon;
        ButtonState& state = plugin.buttons[i];
        state.declared = state.icon = icon.bytes ? plugin_icon({icon.data, icon.bytes}) : kNoIcon;
    }
}

void buttons_add(Plugin& plugin) noexcept {
    buttons_init(plugin);
    evaluate_static(plugin);
}

void buttons_refresh(u64 channel_id, u64 guild_id) noexcept {
    g_channel_id = channel_id;
    g_guild_id = guild_id;
    for (u32 i = 0; i < g_plugin_count; ++i)
        if (g_plugins[i].button_count) evaluate_static(g_plugins[i]);
    changed(false);
}

void buttons_refresh_remembered() noexcept { buttons_refresh(g_channel_id, g_guild_id); }

mc_status buttons_set_state(PluginBinding* binding, u32 button, const mc_button_state& state) noexcept {
    if (button >= binding->image.descriptor->button_count) return MC_E_INVALID;
    Plugin* plugin = ensure_plugin(binding);
    if (!plugin) return MC_E_BUDGET;
    buttons_init(*plugin);
    if (button >= plugin->button_count) return MC_E_INVALID;
    if ((state.flags & ~(MC_BUTTON_ACTIVE | MC_BUTTON_DISABLED | MC_BUTTON_HIDDEN)) || state.badge_count > 1'000'000 ||
        !text_valid(state.tooltip, MC_NODE_TOOLTIP_MAX) || !text_valid(state.icon, 64))
        return MC_E_INVALID;
    ButtonState& target = plugin->buttons[button];
    ButtonState next{};
    next.flags = state.flags;
    next.badge = state.badge_count;
    next.declared = target.declared;
    next.icon = state.icon.bytes ? plugin_icon({state.icon.data, state.icon.bytes}) : target.declared;
    next.tooltip_bytes = static_cast<u8>(state.tooltip.bytes);
    copy_bytes(next.tooltip, state.tooltip.data, state.tooltip.bytes);
    if (next.flags == target.flags && next.badge == target.badge && next.icon == target.icon &&
        next.tooltip_bytes == target.tooltip_bytes && bytes_equal(next.tooltip, target.tooltip, next.tooltip_bytes))
        return MC_OK;
    target = next;
    changed(false);
    return MC_OK;
}

u32 button_surfaces() noexcept {
    u32 mask = 0;
    for (u32 p = 0; p < g_plugin_count; ++p)
        for (u32 i = 0; i < g_plugins[p].button_count; ++i) {
            const u32 surface = slot_surface(definition(g_plugins[p], i).surface);
            if (surface < 32) mask |= 1u << surface;
        }
    return mask;
}

u32 button_slots(u32 surface, u64 message_id, Slice<UiPluginSlotNode> output) noexcept {
    u32 count = 0;
    for (u32 p = 0; p < g_plugin_count; ++p) {
        Plugin& plugin = g_plugins[p];
        for (u32 i = 0; i < plugin.button_count; ++i) {
            const mc_button_def& def = definition(plugin, i);
            const ButtonState& state = plugin.buttons[i];
            if (slot_surface(def.surface) != surface || (state.flags & MC_BUTTON_HIDDEN)) continue;
            const bool popover = def.surface == MC_SURFACE_MESSAGE_POPOVER;
            if (popover ? !popover_shown(plugin, i, message_id) : !(plugin.shown >> i & 1u)) continue;
            if (count == output.count) return count;
            UiPluginSlotNode& node = output[count++];
            node = {plugin.binding->owner_id, kButtonAction | i, {def.label.data, def.label.bytes},
                    {state.tooltip, state.tooltip_bytes}, state.icon, ui_plugin_node_button,
                    static_cast<u8>((def.flags & MC_BUTTON_DANGER) ? MC_TONE_CRITICAL : MC_TONE_NEUTRAL), {}};
            node.badge = static_cast<u16>(state.badge < 0xffffu ? state.badge : 0xffffu);
            node.state = static_cast<u8>(((state.flags & MC_BUTTON_ACTIVE) ? kUiPluginSlotActive : 0) |
                                         ((state.flags & MC_BUTTON_DISABLED) ? kUiPluginSlotDisabled : 0));
        }
    }
    return count;
}

bool buttons_popover(PluginDecoratePass* pass, const PluginMessageInput& message) noexcept {
    if (!pass->budget_us) pass->budget_us = kPluginDecoratePassBudgetUs;
    bool complete = true;
    mc_message in{};
    bool built = false;
    for (u32 p = 0; p < g_plugin_count; ++p) {
        Plugin& plugin = g_plugins[p];
        u32 asking = 0;
        for (u32 i = 0; i < plugin.button_count; ++i)
            if (definition(plugin, i).surface == MC_SURFACE_MESSAGE_POPOVER && definition(plugin, i).visible)
                asking |= 1u << i;
        if (!asking) continue;
        VisibleMemory* memory = nullptr;
        for (VisibleMemory& candidate : plugin.visible)
            if (candidate.message_id == message.message_id) memory = &candidate;
        if (memory && memory->revision == message.revision) continue;
        if (pass->spent_us >= pass->budget_us) {
            pass->exhausted = true;
            complete = false;
            continue;
        }
        if (!built) {
            fill_message(message, &in);
            built = true;
        }
        u32 bits = 0;
        const i64 begin = clock_now();
        for (u32 i = 0; i < plugin.button_count; ++i)
            if ((asking >> i & 1u) && run_visible(plugin, i, &in)) bits |= 1u << i;
        pass->spent_us += elapsed_us(begin);
        ++pass->calls;
        if (!memory) memory = &plugin.visible[plugin.visible_next++ % kPopoverMemory];
        *memory = {message.message_id, message.revision, bits};
    }
    return complete;
}

u32 buttons_overflow(u64 message_id, u32 bar, Slice<PluginButtonRef> output) noexcept {
    u32 seen = 0, count = 0;
    for (u32 p = 0; p < g_plugin_count; ++p) {
        const Plugin& plugin = g_plugins[p];
        for (u32 i = 0; i < plugin.button_count; ++i) {
            const mc_button_def& def = definition(plugin, i);
            if (def.surface != MC_SURFACE_MESSAGE_POPOVER || (plugin.buttons[i].flags & MC_BUTTON_HIDDEN) ||
                !popover_shown(plugin, i, message_id) || seen++ < bar)
                continue;
            if (count == output.count) return count;
            const ButtonState& state = plugin.buttons[i];
            output[count++] = {plugin.binding->owner_id, kButtonAction | i,
                               state.tooltip_bytes ? Utf8View{state.tooltip, state.tooltip_bytes}
                                                   : Utf8View{def.label.data, def.label.bytes},
                               (def.flags & MC_BUTTON_DANGER) != 0};
        }
    }
    return count;
}

bool buttons_activate(u64 owner_id, const PluginSurfaceTarget& target, u64 action_id,
                      const PluginMessageInput* message) noexcept {
    Plugin* plugin = by_owner(owner_id);
    const u64 index = action_id & ~kButtonAction;
    if (!plugin || index >= plugin->button_count || plugin->binding->struck_out) return false;
    const mc_button_def& def = definition(*plugin, static_cast<u32>(index));
    const ButtonState& state = plugin->buttons[index];
    const bool popover = def.surface == MC_SURFACE_MESSAGE_POPOVER;
    if (slot_surface(def.surface) != target.surface || !def.click || (state.flags & (MC_BUTTON_HIDDEN | MC_BUTTON_DISABLED)) ||
        (popover && (!message || !popover_shown(*plugin, static_cast<u32>(index), message->message_id))))
        return false;
    mc_message in{};
    if (popover) fill_message(*message, &in);
    const mc_button_event event{target.channel_id, target.guild_id, popover ? &in : nullptr, static_cast<u32>(index), 0};
    ClickCall call{def.click, &event};
    plugin_binding_call(plugin->binding, PluginCallKind::slot_action, PluginCallClass::gesture, MC_CTX_GESTURE,
                        ClickCall::run, &call);
    return true;
}

} // namespace plugin_surfaces_detail
} // namespace maicord
