#include "ui_internal.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/app/plugins/text.hpp"

// Context menu items: which of the enabled plugins' items a target shows, and running one.
namespace maicord {
using namespace plugin_ui_detail;
namespace {

static_assert(kPluginMenuTargetOwn == MC_MENU_OWN && kPluginMenuTargetPrivate == MC_MENU_PRIVATE &&
              kPluginMenuTargetAnimated == MC_MENU_ANIMATED && kPluginMenuLabelBytes == MC_MENU_LABEL_MAX);

constexpr u32 target_bit(PluginMenuKind kind) noexcept {
    constexpr u32 bits[] = {MC_MENU_USER, MC_MENU_MESSAGE, MC_MENU_CHANNEL, MC_MENU_GUILD,
                            MC_MENU_LINK, MC_MENU_IMAGE,   MC_MENU_EMOJI};
    return bits[static_cast<u32>(kind)];
}

mc_menu_target to_mc(const PluginMenuTarget& target) noexcept {
    return {target_bit(target.kind), target.flags,   target.user_id,      target.channel_id, target.guild_id,
            target.message_id,       target.item_id,  plugin_str(target.url),   plugin_str(target.text),
            plugin_str(target.content_type), target.attachment_index, target.width, target.height, 0};
}

// The argument of one call into a plugin's visible or select function.
struct Call {
    const mc_menu_def* def;
    const mc_menu_target* target;
    u32 shown;
};

mc_status invoke_visible(mc_ctx* context, void* argument) noexcept {
    Call& call = *static_cast<Call*>(argument);
    call.shown = call.def->visible(context, call.target);
    return MC_OK;
}

mc_status invoke_select(mc_ctx* context, void* argument) noexcept {
    Call& call = *static_cast<Call*>(argument);
    return call.def->select(context, call.target);
}

// The row for item when it shows for the target; false when the plugin hides it or fails.
bool make_row(PluginBinding& binding, u32 item, const mc_menu_target& target, PluginMenuRow* row) noexcept {
    const mc_menu_def& def = descriptor_of(binding).menus[item];
    u32 shown = MC_MENU_SHOW;
    if (def.visible) {
        Call call{&def, &target, 0};
        const PluginCallResult ran =
            plugin_binding_call(&binding, PluginCallKind::menu, PluginCallClass::pre, 0, invoke_visible, &call);
        if (ran.overran || ran.status != MC_OK) return false;
        shown = call.shown;
    }
    if (!(shown & (MC_MENU_SHOW | MC_MENU_DISABLED))) return false;
    row->owner_id = binding.owner_id;
    row->item = item;
    row->flags = ((def.flags & MC_MENU_DANGER) ? kPluginMenuRowDanger : 0u) |
                 ((shown & MC_MENU_DISABLED) ? kPluginMenuRowDisabled : 0u) |
                 (((def.flags & MC_MENU_CHECKBOX) && (shown & MC_MENU_CHECKED)) ? kPluginMenuRowChecked : 0u) |
                 ((def.flags & MC_MENU_SUBMENU) ? kPluginMenuRowSubmenu : 0u);
    row->label_bytes = utf8_copy_truncated(row->label, {def.label.data, def.label.bytes});
    return true;
}

bool has_child(const mc_descriptor& descriptor, u32 parent_item, u32 bit) noexcept {
    for (u32 i = 0; i < descriptor.menu_count; ++i)
        if (descriptor.menus[i].parent == parent_item + 1 && (descriptor.menus[i].targets & bit)) return true;
    return false;
}

} // namespace

bool plugin_menu_present(PluginMenuKind kind) noexcept {
    const u32 bit = target_bit(kind);
    for (u32 i = 0; i < g_slot_count; ++i) {
        const PluginBinding& binding = *g_slots[i].binding;
        if (!callable(binding)) continue;
        const mc_descriptor& descriptor = descriptor_of(binding);
        for (u32 item = 0; item < descriptor.menu_count; ++item)
            if (descriptor.menus[item].targets & bit) return true;
    }
    return false;
}

u32 plugin_menu_collect(const PluginMenuTarget& target, Slice<PluginMenuRow> output) noexcept {
    const mc_menu_target native = to_mc(target);
    const u32 bit = target_bit(target.kind);
    u32 count = 0;
    for (u32 slot = 0; slot < g_slot_count; ++slot) {
        PluginBinding& binding = *g_slots[slot].binding;
        if (!callable(binding)) continue;
        const mc_descriptor& descriptor = descriptor_of(binding);
        for (u32 item = 0; item < descriptor.menu_count && count < output.count; ++item) {
            const mc_menu_def& def = descriptor.menus[item];
            if (def.parent || !(def.targets & bit)) continue;
            if ((def.flags & MC_MENU_SUBMENU) && !has_child(descriptor, item, bit)) continue;
            if (make_row(binding, item, native, output.data + count)) ++count;
        }
    }
    return count;
}

u32 plugin_menu_children(const PluginMenuTarget& target, u64 owner_id, u32 parent_item,
                         Slice<PluginMenuRow> output) noexcept {
    Slot* slot = find_slot(owner_id);
    if (!slot || !callable(*slot->binding)) return 0;
    PluginBinding& binding = *slot->binding;
    const mc_descriptor& descriptor = descriptor_of(binding);
    if (parent_item >= descriptor.menu_count || !(descriptor.menus[parent_item].flags & MC_MENU_SUBMENU)) return 0;
    const mc_menu_target native = to_mc(target);
    const u32 bit = target_bit(target.kind);
    u32 count = 0;
    for (u32 item = 0; item < descriptor.menu_count && count < output.count; ++item) {
        const mc_menu_def& def = descriptor.menus[item];
        if (def.parent == parent_item + 1 && (def.targets & bit) && make_row(binding, item, native, output.data + count))
            ++count;
    }
    return count;
}

PluginMenuRun plugin_menu_activate(const PluginMenuTarget& target, u64 owner_id, u32 item) noexcept {
    Slot* slot = find_slot(owner_id);
    if (!slot || !callable(*slot->binding)) return PluginMenuRun::gone;
    PluginBinding& binding = *slot->binding;
    const mc_descriptor& descriptor = descriptor_of(binding);
    if (item >= descriptor.menu_count) return PluginMenuRun::gone;
    const mc_menu_def& def = descriptor.menus[item];
    if (!(def.targets & target_bit(target.kind)) || (def.flags & MC_MENU_SUBMENU) || !def.select)
        return PluginMenuRun::gone;
    const mc_menu_target native = to_mc(target);
    Call call{&def, &native, 0};
    const PluginCallResult ran = plugin_binding_call(&binding, PluginCallKind::menu, PluginCallClass::gesture,
                                                     MC_CTX_GESTURE, invoke_select, &call);
    return ran.overran || ran.status != MC_OK ? PluginMenuRun::failed : PluginMenuRun::ran;
}

} // namespace maicord
