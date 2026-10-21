// Plugin SDK host, ABI 5: context menus (mc.ui.menus@2), slots and popovers
// (mc.ui.slots@2) and message render decorators (mc.msg.render@1).
#include "plugin_sdk_internal.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {
namespace {
using namespace plugin_sdk;

constexpr char kIconNames[] =
#define MAICORD_ICON(name, source) source "\0"
#include "maicord/ui/icon_ids.inc"
#undef MAICORD_ICON
;
struct IconName { u16 offset, bytes, id; };
static_assert(sizeof(IconName)==6);
struct IconNames { IconName entries[static_cast<u32>(UiIcon::count)]; };
consteval IconNames icon_names() noexcept {
    IconNames names{};u32 at=0,id=0;
    for(auto& entry:names.entries) {
        entry.offset=static_cast<u16>(at);entry.id=static_cast<u16>(id++);
        while(kIconNames[at])++at;
        entry.bytes=static_cast<u16>(at-entry.offset);
        ++at;
    }
    // Stable length ordering preserves manifest order among possible duplicates.
    for(u32 i=1;i<static_cast<u32>(UiIcon::count);++i) {
        const auto value=names.entries[i];u32 j=i;
        while(j&&names.entries[j-1].bytes>value.bytes){names.entries[j]=names.entries[j-1];--j;}
        names.entries[j]=value;
    }
    return names;
}
static_assert(sizeof(kIconNames)<65536);
constexpr auto kIconNameIndex=icon_names();

constexpr u32 kind_bit(u32 kind) noexcept { return 1u << kind; }

// ---- mc.ui.menus@2 ----------------------------------------------------------------

mc_status MC_CALL menus_register(void* handle, uint64_t generation, uint64_t id, uint32_t target, mc_str label,
                                 uint32_t flags) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_MENUS)) return MC_E_DENIED;
    if (!id || target >= MC_MENU_TARGET_COUNT || (flags & ~(MC_MENU_ITEM_DANGER | MC_MENU_ITEM_DISABLED)) ||
        !text_ok(label, kPluginSdkMenuLabelBytes, true)) return MC_E_INVALID;
    Menu* menu = nullptr;
    for (auto& candidate : binding->menus) {
        if (candidate.registered && candidate.id == id) return MC_E_INVALID;
        if (!menu && !candidate.registered) menu = &candidate;
    }
    if (!menu) return MC_E_BUDGET;
    *menu = {};
    menu->id = id;
    menu->target = target;
    menu->flags = flags;
    menu->label_bytes = copy_text(menu->label, kPluginSdkMenuLabelBytes, view(label));
    menu->registered = true;
    ++binding->runtime->menu_revision;
    return MC_OK;
}
mc_status MC_CALL menus_unregister(void* handle, uint64_t generation, uint64_t id) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_MENUS)) return MC_E_DENIED;
    for (auto& menu : binding->menus)
        if (menu.registered && menu.id == id) {
            menu = {};
            ++binding->runtime->menu_revision;
            return MC_OK;
        }
    return MC_E_NOT_FOUND;
}
mc_status MC_CALL menus_set_handler(void* handle, uint64_t generation, mc_menu_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_MENUS)) return MC_E_DENIED;
    binding->menu_handler = handler;
    ++binding->runtime->menu_revision;
    return MC_OK;
}

// ---- mc.ui.slots@2 ----------------------------------------------------------------

bool global_surface(u32 surface) noexcept {
    return surface == MC_SLOT_COMPOSER_TOOLBAR || surface == MC_SLOT_HEADER_TOOLBAR || surface == MC_SLOT_ACCOUNT_PANEL ||
           surface == MC_SLOT_MESSAGE_HOVER;
}
bool normalize_target(const NativePluginSdkSlotTarget& source, NativePluginSdkSlotTarget* target) noexcept {
    *target = {source.surface, 0, 0, 0, 0};
    switch (source.surface) {
    case MC_SLOT_MESSAGE_ACCESSORY:
        target->channel_id = source.channel_id;
        target->message_id = source.message_id;
        return source.channel_id && source.message_id;
    case MC_SLOT_MEMBER_BADGE:
    case MC_SLOT_PROFILE_SECTION:
        target->user_id = source.user_id;
        return source.user_id != 0;
    case MC_SLOT_CHANNEL_ROW:
        target->channel_id = source.channel_id;
        return source.channel_id != 0;
    case MC_SLOT_GUILD_RAIL:
        target->guild_id = source.guild_id;
        return source.guild_id != 0;
    default: return global_surface(source.surface);
    }
}
u32 surface_kinds(u32 surface) noexcept {
    switch (surface) {
    case MC_SLOT_COMPOSER_TOOLBAR:
    case MC_SLOT_ACCOUNT_PANEL:
    case MC_SLOT_MESSAGE_HOVER: return kind_bit(MC_NODE_BUTTON) | kind_bit(MC_NODE_ICON);
    case MC_SLOT_PROFILE_SECTION:
        return kind_bit(MC_NODE_LABEL) | kind_bit(MC_NODE_ICON) | kind_bit(MC_NODE_BADGE) | kind_bit(MC_NODE_BUTTON) |
               kind_bit(MC_NODE_LINK);
    default: return kind_bit(MC_NODE_LABEL) | kind_bit(MC_NODE_ICON) | kind_bit(MC_NODE_BADGE) | kind_bit(MC_NODE_BUTTON);
    }
}
bool same_target(const SlotEntry& entry, const NativePluginSdkSlotTarget& target) noexcept {
    return entry.node_count && entry.surface == target.surface && entry.channel_id == target.channel_id &&
           entry.message_id == target.message_id && entry.user_id == target.user_id && entry.guild_id == target.guild_id;
}
bool read_target(const mc_slot_target* source, NativePluginSdkSlotTarget* target) noexcept {
    mc_slot_target copy{};
    if (!read_prefix(source, &copy, offsetof(mc_slot_target, channel_id))) return false;
    return normalize_target({copy.surface, copy.channel_id, copy.message_id, copy.user_id, copy.guild_id}, target);
}
void put_node(SlotNode& out, const mc_slot_node& node) noexcept {
    out.kind = static_cast<u8>(node.kind);
    out.tone = static_cast<u8>(node.tone);
    out.icon = node.icon.bytes ? native_plugin_sdk_icon(view(node.icon)) : kPluginSdkNoIcon;
    out.action_id = node.action_id;
    out.text_bytes = static_cast<u8>(copy_text(out.text, kSlotTextBytes, view(node.text)));
    out.tooltip_bytes = static_cast<u8>(copy_text(out.tooltip, kSlotTooltipBytes, view(node.tooltip)));
    out.value_bytes = static_cast<u16>(copy_text(out.value, kSlotValueBytes, view(node.value)));
}
mc_status MC_CALL slots_set(void* handle, uint64_t generation, const mc_slot_target* source_target,
                            const mc_slot_content* source_content) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_UI_SLOTS)) return MC_E_DENIED;
    NativePluginSdkSlotTarget target{};
    mc_slot_content content{};
    if (!read_target(source_target, &target) || !read_prefix(source_content, &content, offsetof(mc_slot_content, reserved)))
        return MC_E_INVALID;
    if (content.node_count > kPluginSdkSlotNodes) return MC_E_BUDGET;
    if (content.node_count && (!content.nodes || content.node_stride < offsetof(mc_slot_node, tooltip)))
        return MC_E_INVALID;
    SlotEntry candidate{};
    candidate.surface = target.surface;
    candidate.channel_id = target.channel_id;
    candidate.message_id = target.message_id;
    candidate.user_id = target.user_id;
    candidate.guild_id = target.guild_id;
    const NodeLimits limits{kSlotTextBytes, kSlotValueBytes, surface_kinds(target.surface)};
    for (u32 i = 0; i < content.node_count; ++i) {
        mc_slot_node node{};
        if (!read_node(content.nodes, content.node_stride, i, limits, &node)) return MC_E_INVALID;
        put_node(candidate.nodes[i], node);
    }
    candidate.node_count = content.node_count;
    // Same target first, then a free entry, then evict the oldest.
    SlotEntry* entry = nullptr;
    for (auto& existing : binding->slots)
        if (same_target(existing, target)) entry = &existing;
    if (!entry)
        for (auto& existing : binding->slots)
            if (!existing.node_count) { entry = &existing; break; }
    if (!entry) {
        entry = &binding->slots[0];
        for (auto& existing : binding->slots)
            if (existing.stamp < entry->stamp) entry = &existing;
    }
    auto& runtime = *binding->runtime;
    candidate.stamp = ++runtime.slot_stamp;
    *entry = candidate.node_count ? candidate : SlotEntry{};
    const u32 index = static_cast<u32>(entry - binding->slots) + 1;
    if (index > binding->slot_high_water) binding->slot_high_water = index;
    ++runtime.slot_revision;
    return MC_OK;
}
mc_status MC_CALL slots_clear(void* handle, uint64_t generation, const mc_slot_target* source_target) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_UI_SLOTS)) return MC_E_DENIED;
    NativePluginSdkSlotTarget target{};
    if (!read_target(source_target, &target)) return MC_E_INVALID;
    for (auto& entry : binding->slots)
        if (same_target(entry, target)) {
            entry = {};
            ++binding->runtime->slot_revision;
            return MC_OK;
        }
    return MC_E_NOT_FOUND;
}
mc_status MC_CALL slots_clear_surface(void* handle, uint64_t generation, uint32_t surface) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_UI_SLOTS)) return MC_E_DENIED;
    if (surface > MC_SLOT_SURFACE_COUNT) return MC_E_INVALID;
    bool changed = false;
    for (u32 i = 0; i < binding->slot_high_water; ++i) {
        auto& entry = binding->slots[i];
        if (entry.node_count && (surface == MC_SLOT_SURFACE_COUNT || entry.surface == surface)) {
            entry = {};
            changed = true;
        }
    }
    if (changed) ++binding->runtime->slot_revision;
    return MC_OK;
}
mc_status MC_CALL slots_set_handler(void* handle, uint64_t generation, mc_slot_action_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_UI_SLOTS)) return MC_E_DENIED;
    binding->slot_handler = handler;
    return MC_OK;
}

// ---- Popovers -------------------------------------------------------------------------

// Runs an action handler (slot or accessory) inside a gesture and keeps the
// popover it returns. from_render selects which handler the popover rows use.
bool run_action(NativePluginSdkBinding& binding, mc_slot_action_fn handler, const mc_slot_event& event,
                bool from_render) noexcept {
    auto& runtime = *binding.runtime;
    mc_popover popover{};
    popover.size = sizeof(popover);
    mc_slot_event copy = event;
    mc_status status{};
    {
        Call call(binding, PluginCallKind::slot_action);
        Gesture gesture(&runtime);
        status = handler(binding.plugin_state, binding.plugin_state_bytes, &copy, &popover);
    }
    runtime.popover_count = 0;
    runtime.popover_owner = 0;
    ++runtime.popover_revision;
    if (status != MC_OK) return false;
    mc_popover checked{};
    if (!read_prefix(&popover, &checked, offsetof(mc_popover, reserved)) || !checked.node_count) return true;
    if (checked.node_count > kPluginSdkPopoverNodes || !checked.nodes ||
        checked.node_stride < offsetof(mc_slot_node, tooltip)) return true;
    const NodeLimits limits{kNodeLabelBytes, kNodeValueBytes,
                            kind_bit(MC_NODE_LABEL) | kind_bit(MC_NODE_BUTTON) | kind_bit(MC_NODE_LINK) |
                                kind_bit(MC_NODE_SEPARATOR) | kind_bit(MC_NODE_ICON)};
    NativePluginSdkPopoverNode nodes[kPluginSdkPopoverNodes]{};
    for (u32 i = 0; i < checked.node_count; ++i) {
        mc_slot_node node{};
        if (!read_node(checked.nodes, checked.node_stride, i, limits, &node)) return true;
        auto& out = nodes[i];
        out.kind = static_cast<u8>(node.kind);
        out.tone = static_cast<u8>(node.tone);
        out.icon = node.icon.bytes ? native_plugin_sdk_icon(view(node.icon)) : kPluginSdkNoIcon;
        out.action_id = node.action_id;
        out.text_bytes = utf8_copy_truncated(out.text, view(node.text.bytes ? node.text : node.tooltip));
        out.value_bytes = utf8_copy_truncated(out.value, view(node.value));
    }
    copy_bytes(runtime.popover, nodes, sizeof(nodes));
    runtime.popover_count = checked.node_count;
    runtime.popover_owner = binding.owner_id;
    runtime.popover_anchor = event;
    runtime.popover_from_render = from_render;
    return true;
}

// ---- mc.msg.render@1 ------------------------------------------------------------------

mc_status MC_CALL render_set_decorator(void* handle, uint64_t generation, mc_render_fn decorator) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_MESSAGE_RENDER)) return MC_E_DENIED;
    binding->decorator = decorator;
    auto& runtime = *binding->runtime;
    ++runtime.render_epoch;
    ++runtime.render_revision;
    return MC_OK;
}
mc_status MC_CALL render_invalidate(void* handle, uint64_t generation, uint64_t channel_id, uint64_t message_id) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_MESSAGE_RENDER)) return MC_E_DENIED;
    auto& runtime = *binding->runtime;
    if (!message_id) {
        ++runtime.render_epoch;
        ++runtime.render_revision;
        return MC_OK;
    }
    for (auto& entry : runtime.render)
        if (entry.used && entry.message_id == message_id && (!channel_id || entry.channel_id == channel_id)) {
            entry.stale = true;
            ++runtime.render_revision;
            return MC_OK;
        }
    return MC_OK; // not decorated yet: the first evaluation will ask
}
mc_status MC_CALL render_set_action_handler(void* handle, uint64_t generation, mc_slot_action_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_MESSAGE_RENDER)) return MC_E_DENIED;
    binding->render_action_handler = handler;
    return MC_OK;
}

NativePluginSdkRenderEntry* render_find(NativePluginSdkRuntime& runtime, u64 message_id) noexcept {
    for (auto& entry : runtime.render)
        if (entry.used && entry.message_id == message_id) return &entry;
    return nullptr;
}
const NativePluginSdkRenderEntry* render_find(const NativePluginSdkRuntime& runtime, u64 message_id) noexcept {
    return render_find(const_cast<NativePluginSdkRuntime&>(runtime), message_id);
}
bool render_current(const NativePluginSdkRuntime& runtime, const NativePluginSdkRenderEntry& entry, u32 revision) noexcept {
    return entry.revision == revision && entry.epoch == runtime.render_epoch && !entry.stale;
}
u32 arena_put(NativePluginSdkRuntime& runtime, Utf8View text) noexcept {
    const u32 at = runtime.render_arena_used;
    copy_bytes(runtime.render_arena + at, text.data, text.bytes);
    runtime.render_arena_used += text.bytes;
    return at;
}
Utf8View arena_view(const NativePluginSdkRuntime& runtime, u32 offset, u32 bytes) noexcept {
    return bytes ? Utf8View{runtime.render_arena + offset, bytes} : Utf8View{};
}
// Worst case one evaluation can add to the arena for every plugin.
// (merge_text stores each joined prefix/suffix again, up to 96 bytes each.)
constexpr u32 kRenderPluginBytes = 64 + 2 * 96 + kPluginSdkRenderContentBytes +
                                   kPluginSdkRenderBadges * (kSlotTextBytes + kSlotTooltipBytes) +
                                   kPluginSdkRenderAccessories * (kNodeLabelBytes + kSlotTooltipBytes + kNodeValueBytes);
void put_arena_node(NativePluginSdkRuntime& runtime, NativePluginSdkRenderArenaNode& out, u64 owner,
                    const mc_slot_node& node) noexcept {
    out = {};
    out.owner_id = owner;
    out.action_id = node.action_id;
    out.kind = static_cast<u8>(node.kind);
    out.tone = static_cast<u8>(node.tone);
    out.icon = node.icon.bytes ? native_plugin_sdk_icon(view(node.icon)) : kPluginSdkNoIcon;
    out.text_bytes = static_cast<u16>(node.text.bytes);
    out.text_offset = arena_put(runtime, view(node.text));
    out.tooltip_bytes = static_cast<u16>(node.tooltip.bytes);
    out.tooltip_offset = arena_put(runtime, view(node.tooltip));
    out.value_bytes = static_cast<u16>(node.value.bytes);
    out.value_offset = arena_put(runtime, view(node.value));
}
NativePluginSdkSlotNode resolve_arena_node(const NativePluginSdkRuntime& runtime,
                                           const NativePluginSdkRenderArenaNode& node) noexcept {
    return {node.owner_id, node.action_id, arena_view(runtime, node.text_offset, node.text_bytes),
            arena_view(runtime, node.tooltip_offset, node.tooltip_bytes),
            arena_view(runtime, node.value_offset, node.value_bytes), node.icon, node.kind, node.tone};
}
// Validates one plugin's verdict without writing anything.
bool decoration_ok(const mc_render_decoration& out) noexcept {
    if (out.visibility > MC_RENDER_HIDDEN || out.media > MC_MEDIA_HIDE || out.name_tone > MC_TONE_ROLE) return false;
    if (!text_ok(out.collapsed_label, 64, false) || !text_ok(out.name_prefix, 32, false) ||
        !text_ok(out.name_suffix, 32, false) || !text_ok(out.content, kPluginSdkRenderContentBytes, false, true))
        return false;
    if (out.badge_count > kPluginSdkRenderBadges || out.accessory_count > kPluginSdkRenderAccessories) return false;
    if (out.badge_count && (!out.badges || out.badge_stride < offsetof(mc_slot_node, tooltip))) return false;
    if (out.accessory_count && (!out.accessories || out.accessory_stride < offsetof(mc_slot_node, tooltip))) return false;
    const NodeLimits badge{kSlotTextBytes, 0, kind_bit(MC_NODE_BADGE) | kind_bit(MC_NODE_ICON)};
    const NodeLimits accessory{kNodeLabelBytes, kNodeValueBytes,
                               kind_bit(MC_NODE_LABEL) | kind_bit(MC_NODE_BUTTON) | kind_bit(MC_NODE_LINK)};
    for (u32 i = 0; i < out.badge_count; ++i) {
        mc_slot_node node{};
        if (!read_node(out.badges, out.badge_stride, i, badge, &node)) return false;
    }
    for (u32 i = 0; i < out.accessory_count; ++i) {
        mc_slot_node node{};
        if (!read_node(out.accessories, out.accessory_stride, i, accessory, &node)) return false;
    }
    return true;
}
// Appends text to a merged prefix/suffix (joined with a space, <= 96 bytes).
void merge_text(NativePluginSdkRuntime& runtime, u32* offset, u16* bytes, Utf8View text) noexcept {
    if (!text.bytes) return;
    char joined[160];
    u32 at = 0;
    if (*bytes) {
        at = copy_text(joined, 96, arena_view(runtime, *offset, *bytes));
        joined[at++] = ' ';
    }
    at += copy_text(joined + at, static_cast<u32>(sizeof(joined)) - at, {text.data, utf8_truncated_length(text, 96 - minimum(at, 96u))});
    *offset = arena_put(runtime, {joined, at});
    *bytes = static_cast<u16>(at);
}
void fill_output(const NativePluginSdkRuntime& runtime, const NativePluginSdkRenderEntry& entry,
                 NativePluginSdkDecoration* output) noexcept {
    *output = {};
    output->visibility = entry.visibility;
    output->media = entry.media;
    output->name_tone = entry.name_tone;
    output->replace_content = entry.replace_content;
    output->collapsed_label = arena_view(runtime, entry.label_offset, entry.label_bytes);
    output->name_prefix = arena_view(runtime, entry.prefix_offset, entry.prefix_bytes);
    output->name_suffix = arena_view(runtime, entry.suffix_offset, entry.suffix_bytes);
    output->content = arena_view(runtime, entry.content_offset, entry.content_bytes);
    output->badge_count = entry.badge_count;
    output->accessory_count = entry.accessory_count;
    for (u32 i = 0; i < entry.badge_count; ++i) output->badges[i] = resolve_arena_node(runtime, entry.badges[i]);
    for (u32 i = 0; i < entry.accessory_count; ++i) output->accessories[i] = resolve_arena_node(runtime, entry.accessories[i]);
}
bool decorates(const NativePluginSdkBinding* binding, const NativePluginSdkRuntime& runtime) noexcept {
    return usable(binding, runtime) && binding->decorator && permitted(*binding, MC_PERM_MESSAGE_RENDER);
}
bool entry_decorated(const NativePluginSdkRenderEntry& entry) noexcept {
    return entry.visibility || entry.media || entry.replace_content || entry.prefix_bytes || entry.suffix_bytes ||
           entry.badge_count || entry.accessory_count;
}

} // namespace

namespace plugin_sdk {
const mc_menus_v2 kMenus{sizeof(mc_menus_v2), 2, menus_register, menus_unregister, menus_set_handler};
const mc_slots_v2 kSlots{sizeof(mc_slots_v2), 2, slots_set, slots_clear, slots_clear_surface, slots_set_handler};
const mc_render_v1 kRender{sizeof(mc_render_v1), 1, render_set_decorator, render_invalidate, render_set_action_handler};

void surfaces_reset(NativePluginSdkBinding& binding) noexcept {
    auto& runtime = *binding.runtime;
    bool menus = false;
    for (const auto& menu : binding.menus) menus = menus || menu.registered;
    if (menus || binding.menu_handler) ++runtime.menu_revision;
    bool slots = false;
    for (u32 i = 0; i < binding.slot_high_water; ++i) slots = slots || binding.slots[i].node_count;
    if (slots) ++runtime.slot_revision;
    if (binding.decorator) {
        ++runtime.render_epoch;
        ++runtime.render_revision;
    }
    if (runtime.popover_owner == binding.owner_id) {
        runtime.popover_owner = 0;
        runtime.popover_count = 0;
        ++runtime.popover_revision;
    }
    binding.menu_handler = nullptr;
    binding.slot_handler = nullptr;
    binding.render_action_handler = nullptr;
    binding.decorator = nullptr;
    clear_bytes(binding.menus, sizeof(binding.menus));
    clear_bytes(binding.slots, binding.slot_high_water * static_cast<u32>(sizeof(SlotEntry)));
    binding.slot_high_water = 0;
}
} // namespace plugin_sdk

// ---- Public API ---------------------------------------------------------------------------

u32 native_plugin_sdk_menu_items(const NativePluginSdkRuntime& runtime, u32 target,
                                 Slice<NativePluginSdkMenuItem> output) noexcept {
    u32 count = 0;
    for (const auto* binding : g_live) {
        if (!usable(binding, runtime) || !permitted(*binding, MC_PERM_MENUS) || !binding->menu_handler) continue;
        for (const auto& menu : binding->menus)
            if (menu.registered && menu.target == target && count < output.count)
                output[count++] = {binding->owner_id, menu.id, {menu.label, menu.label_bytes}, menu.flags};
    }
    return count;
}

bool native_plugin_sdk_menu_activate(NativePluginSdkRuntime* runtime, u64 owner_id, u64 item_id,
                                     const NativePluginSdkMenuContext& context) noexcept {
    auto* binding = runtime ? binding_for(*runtime, owner_id) : nullptr;
    if (!binding || !permitted(*binding, MC_PERM_MENUS) ||
        !callable(*binding, reinterpret_cast<const void*>(binding->menu_handler))) return false;
    const Menu* menu = nullptr;
    for (const auto& candidate : binding->menus)
        if (candidate.registered && candidate.id == item_id && candidate.target == context.target) menu = &candidate;
    if (!menu || (menu->flags & MC_MENU_ITEM_DISABLED)) return false;
    // Copy-only: strings point into the runtime input buffer, not host data.
    const u32 url_bytes = utf8_truncated_length(context.url, kPluginSdkUrlBytes);
    const u32 text_bytes = utf8_truncated_length(context.text, 1024);
    copy_bytes(runtime->input, context.url.data, url_bytes);
    copy_bytes(runtime->input + url_bytes, context.text.data, text_bytes);
    mc_menu_context copy{};
    copy.size = sizeof(copy);
    copy.target = context.target;
    copy.item_id = item_id;
    copy.user_id = context.user_id;
    copy.channel_id = context.channel_id;
    copy.message_id = context.message_id;
    copy.guild_id = context.guild_id;
    copy.emoji_id = context.emoji_id;
    copy.attachment_index = context.attachment_index;
    copy.flags = context.flags & (MC_MENU_OWN | MC_MENU_PRIVATE | MC_MENU_ANIMATED);
    copy.url = {runtime->input, url_bytes};
    copy.text = {runtime->input + url_bytes, text_bytes};
    mc_status status{};
    {
        Call call(*binding, PluginCallKind::menu);
        Gesture gesture(runtime);
        status = binding->menu_handler(binding->plugin_state, binding->plugin_state_bytes, &copy);
    }
    return status == MC_OK;
}

u32 native_plugin_sdk_slots(const NativePluginSdkRuntime& runtime, NativePluginSdkSlotTarget target,
                           Slice<NativePluginSdkSlotNode> output) noexcept {
    NativePluginSdkSlotTarget normal{};
    if (!normalize_target(target, &normal)) return 0;
    u32 count = 0;
    for (const auto* binding : g_live) {
        if (!usable(binding, runtime)) continue;
        for (u32 e = 0; e < binding->slot_high_water; ++e) {
            const auto& entry = binding->slots[e];
            if (!same_target(entry, normal)) continue;
            for (u32 i = 0; i < entry.node_count && count < output.count; ++i) {
                const auto& node = entry.nodes[i];
                output[count++] = {binding->owner_id, node.action_id, {node.text, node.text_bytes},
                                   {node.tooltip, node.tooltip_bytes}, {node.value, node.value_bytes}, node.icon,
                                   node.kind, node.tone};
            }
        }
    }
    return count;
}

bool native_plugin_sdk_slot_action(NativePluginSdkRuntime* runtime, u64 owner, NativePluginSdkSlotTarget target,
                                  u64 action_id) noexcept {
    auto* binding = runtime && action_id ? binding_for(*runtime, owner) : nullptr;
    if (!binding || !permitted(*binding, MC_PERM_UI_SLOTS) ||
        !callable(*binding, reinterpret_cast<const void*>(binding->slot_handler))) return false;
    NativePluginSdkSlotTarget normal{};
    if (!normalize_target(target, &normal)) return false;
    bool found = false;
    for (u32 e = 0; e < binding->slot_high_water; ++e) {
        const auto& entry = binding->slots[e];
        if (same_target(entry, normal))
            for (u32 i = 0; i < entry.node_count; ++i)
                found = found || (entry.nodes[i].kind == MC_NODE_BUTTON && entry.nodes[i].action_id == action_id);
    }
    if (!found) return false;
    // Global surfaces carry their click context (the hovered message).
    const auto& ids = global_surface(target.surface) ? target : normal;
    const mc_slot_event event{sizeof(mc_slot_event), target.surface, ids.channel_id, ids.message_id, ids.user_id,
                              ids.guild_id, action_id};
    return run_action(*binding, binding->slot_handler, event, false);
}

bool native_plugin_sdk_popover_action(NativePluginSdkRuntime* runtime, u64 revision, u32 index) noexcept {
    if (!runtime || revision != runtime->popover_revision || index >= runtime->popover_count) return false;
    const auto node = runtime->popover[index];
    auto* binding = binding_for(*runtime, runtime->popover_owner);
    if (!binding || node.kind != MC_NODE_BUTTON || !node.action_id) return false;
    const bool from_render = runtime->popover_from_render;
    const auto handler = from_render ? binding->render_action_handler : binding->slot_handler;
    const u64 permission = from_render ? MC_PERM_MESSAGE_RENDER : MC_PERM_UI_SLOTS;
    if (!permitted(*binding, permission) || !callable(*binding, reinterpret_cast<const void*>(handler))) return false;
    mc_slot_event event = runtime->popover_anchor;
    event.action_id = node.action_id;
    return run_action(*binding, handler, event, from_render);
}

void native_plugin_sdk_popover_clear(NativePluginSdkRuntime* runtime) noexcept {
    if (!runtime || !runtime->popover_count) return;
    runtime->popover_count = 0;
    runtime->popover_owner = 0;
    ++runtime->popover_revision;
}

u16 native_plugin_sdk_icon(Utf8View name) noexcept {
    u32 first=0,last=static_cast<u32>(UiIcon::count);
    while(first<last){const u32 middle=first+(last-first)/2;
        if(kIconNameIndex.entries[middle].bytes<name.bytes)first=middle+1;else last=middle;}
    for(u32 i=first;i<static_cast<u32>(UiIcon::count);++i) {
        const auto& entry=kIconNameIndex.entries[i];
        if(entry.bytes!=name.bytes)break;
        if(bytes_equal(kIconNames+entry.offset,name.data,name.bytes))return entry.id;
    }
    return kPluginSdkNoIcon;
}

bool native_plugin_sdk_has_decorators(const NativePluginSdkRuntime& runtime) noexcept {
    for (const auto* binding : g_live)
        if (decorates(binding, runtime)) return true;
    return false;
}

bool native_plugin_sdk_decoration_current(const NativePluginSdkRuntime& runtime, u64 message_id, u32 revision) noexcept {
    if (!native_plugin_sdk_has_decorators(runtime)) return true;
    const auto* entry = render_find(runtime, message_id);
    return entry && render_current(runtime, *entry, revision);
}

bool native_plugin_sdk_decorate(NativePluginSdkRuntime* runtime, const NativePluginSdkRenderMessage& message, bool evaluate,
                                NativePluginSdkDecoration* output) noexcept {
    if (output) *output = {};
    if (!runtime || !output || !message.message_id) return false;
    auto* entry = render_find(*runtime, message.message_id);
    if (entry && render_current(*runtime, *entry, message.revision)) {
        if (!entry_decorated(*entry)) return false;
        fill_output(*runtime, *entry, output);
        return true;
    }
    if (!evaluate || !native_plugin_sdk_has_decorators(*runtime)) return false;
    u32 decorators = 0;
    for (const auto* binding : g_live)
        decorators += decorates(binding, *runtime);
    if (runtime->render_arena_used + decorators * kRenderPluginBytes > kPluginSdkRenderArenaBytes) {
        // Arena exhausted: start over; every message is decorated again on demand.
        for (auto& existing : runtime->render) existing.used = false;
        runtime->render_arena_used = 0;
        ++runtime->render_revision;
        entry = nullptr;
    }
    if (!entry)
        for (auto& candidate : runtime->render)
            if (!candidate.used) { entry = &candidate; break; }
    if (!entry) {
        entry = &runtime->render[0];
        for (auto& candidate : runtime->render)
            if (candidate.stamp < entry->stamp) entry = &candidate;
    }
    NativePluginSdkRenderEntry merged{};
    merged.used = true;
    merged.message_id = message.message_id;
    merged.channel_id = message.channel_id;
    merged.revision = message.revision;
    merged.epoch = runtime->render_epoch;
    const u32 content_bytes = utf8_truncated_length(message.content, kPluginSdkInputBytes - 512);
    const u32 author_bytes = utf8_truncated_length(message.author_name, 128);
    for (auto* binding : g_live) {
        if (!decorates(binding, *runtime)) continue;
        // Recopy for each decorator so one plugin can't alter what the next one sees.
        copy_bytes(runtime->input, message.content.data, content_bytes);
        copy_bytes(runtime->input + content_bytes, message.author_name.data, author_bytes);
        mc_render_message in{};
        in.size = sizeof(in);
        in.flags = message.flags;
        in.channel_id = message.channel_id;
        in.message_id = message.message_id;
        in.author_id = message.author_id;
        in.guild_id = message.guild_id;
        in.timestamp_ms = message.timestamp_ms;
        in.edited_ms = message.edited_ms;
        in.revision = message.revision;
        in.relationship = message.relationship;
        in.attachment_count = message.attachment_count;
        in.embed_count = message.embed_count;
        in.content = {runtime->input, content_bytes};
        in.author_name = {runtime->input + content_bytes, author_bytes};
        mc_render_decoration out{};
        out.size = sizeof(out);
        mc_status status{};
        {
            Call call(*binding, PluginCallKind::decorate);
            status = binding->decorator(binding->plugin_state, binding->plugin_state_bytes, &in, &out);
        }
        mc_render_decoration checked{};
        if (status != MC_OK || !read_prefix(&out, &checked, offsetof(mc_render_decoration, badges)) ||
            !decoration_ok(checked)) continue;
        if (checked.visibility > merged.visibility) {
            merged.visibility = static_cast<u8>(checked.visibility);
            if (checked.collapsed_label.bytes) {
                merged.label_bytes = static_cast<u16>(checked.collapsed_label.bytes);
                merged.label_offset = arena_put(*runtime, view(checked.collapsed_label));
            }
        }
        if (checked.media > merged.media) merged.media = static_cast<u8>(checked.media);
        if ((checked.name_prefix.bytes || checked.name_suffix.bytes) && !merged.prefix_bytes && !merged.suffix_bytes)
            merged.name_tone = static_cast<u8>(checked.name_tone);
        merge_text(*runtime, &merged.prefix_offset, &merged.prefix_bytes, view(checked.name_prefix));
        merge_text(*runtime, &merged.suffix_offset, &merged.suffix_bytes, view(checked.name_suffix));
        if (checked.replace_content && !merged.replace_content) {
            merged.replace_content = true;
            merged.content_bytes = static_cast<u16>(checked.content.bytes);
            merged.content_offset = arena_put(*runtime, view(checked.content));
        }
        const NodeLimits badge{kSlotTextBytes, 0, kind_bit(MC_NODE_BADGE) | kind_bit(MC_NODE_ICON)};
        for (u32 i = 0; i < checked.badge_count && merged.badge_count < kPluginSdkRenderMergedBadges; ++i) {
            mc_slot_node node{};
            read_node(checked.badges, checked.badge_stride, i, badge, &node);
            put_arena_node(*runtime, merged.badges[merged.badge_count++], binding->owner_id, node);
        }
        const NodeLimits accessory{kNodeLabelBytes, kNodeValueBytes,
                                   kind_bit(MC_NODE_LABEL) | kind_bit(MC_NODE_BUTTON) | kind_bit(MC_NODE_LINK)};
        for (u32 i = 0; i < checked.accessory_count && merged.accessory_count < kPluginSdkRenderMergedAccessories; ++i) {
            mc_slot_node node{};
            read_node(checked.accessories, checked.accessory_stride, i, accessory, &node);
            put_arena_node(*runtime, merged.accessories[merged.accessory_count++], binding->owner_id, node);
        }
    }
    merged.stamp = ++runtime->render_stamp;
    *entry = merged;
    ++runtime->render_revision;
    if (!entry_decorated(*entry)) return false;
    fill_output(*runtime, *entry, output);
    return true;
}

bool native_plugin_sdk_render_action(NativePluginSdkRuntime* runtime, u64 owner, u64 channel_id, u64 message_id,
                                     u64 action_id) noexcept {
    auto* binding = runtime && action_id ? binding_for(*runtime, owner) : nullptr;
    if (!binding || !permitted(*binding, MC_PERM_MESSAGE_RENDER) ||
        !callable(*binding, reinterpret_cast<const void*>(binding->render_action_handler))) return false;
    const auto* entry = render_find(*runtime, message_id);
    if (!entry || entry->epoch != runtime->render_epoch || (channel_id && entry->channel_id != channel_id)) return false;
    bool found = false;
    for (u32 i = 0; i < entry->accessory_count; ++i)
        found = found || (entry->accessories[i].owner_id == owner && entry->accessories[i].kind == MC_NODE_BUTTON &&
                          entry->accessories[i].action_id == action_id);
    if (!found) return false;
    const mc_slot_event event{sizeof(mc_slot_event), MC_SLOT_MESSAGE_ACCESSORY, entry->channel_id, message_id, 0, 0,
                              action_id};
    return run_action(*binding, binding->render_action_handler, event, true);
}

} // namespace maicord
