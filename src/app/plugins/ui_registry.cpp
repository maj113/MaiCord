#include "ui_internal.hpp"

// The enabled plugins in enable order: menus, commands and banners walk this list instead of
// the runtime that owns the bindings, so application code needs no runtime pointer.
namespace maicord {
namespace plugin_ui_detail {

Slot g_slots[kPluginMaximumBindings];
u32 g_slot_count;
u32 g_revision;

Slot* find_slot(u64 owner_id) noexcept {
    for (u32 i = 0; i < g_slot_count; ++i)
        if (g_slots[i].binding->owner_id == owner_id) return g_slots + i;
    return nullptr;
}

} // namespace plugin_ui_detail

using namespace plugin_ui_detail;

bool plugin_ui_add(PluginBinding* binding) noexcept {
    if (g_slot_count == kPluginMaximumBindings) return false;
    g_slots[g_slot_count++] = {binding, {}};
    ++g_revision;
    return true;
}

void plugin_ui_remove(PluginBinding* binding) noexcept {
    u32 at = 0;
    while (at < g_slot_count && g_slots[at].binding != binding) ++at;
    if (at == g_slot_count) return;
    feedback_remove(g_slots + at);
    for (u32 i = at + 1; i < g_slot_count; ++i) g_slots[i - 1] = g_slots[i];
    g_slots[--g_slot_count] = {};
    ++g_revision;
}

u32 plugin_ui_revision() noexcept {
    return g_revision;
}

} // namespace maicord
