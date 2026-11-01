#include "maicord/app/plugins/binding.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {

static_assert(static_cast<u32>(PluginHook::pre_send) == MC_HOOK_PRE_SEND &&
              static_cast<u32>(PluginHook::pre_edit) == MC_HOOK_PRE_EDIT &&
              static_cast<u32>(PluginHook::count) == MC_HOOK_LAST + 1);

u32 g_plugin_hook_subscribers[static_cast<u32>(PluginHook::count)];

namespace {

// Two output buffers a draft's text alternates between, so a hook's buffer never
// aliases the text it was given. Committed when the first plugin subscribes.
constexpr u32 kBufferBytes = 4096;
static_assert(kBufferBytes >= MC_DRAFT_TEXT_MAX);

struct Subscriber {
    PluginBinding* binding;
    mc_draft_fn function;
};
struct List {
    Subscriber entries[kPluginMaximumBindings];
    u32 count;
};

List g_lists[static_cast<u32>(PluginHook::count)];
u8* g_buffers;
u32 g_subscribed_plugins;
PluginDispatchStats g_stats;

void remove_from(List& list, const PluginBinding* binding) noexcept {
    u32 kept = 0;
    for (u32 i = 0; i < list.count; ++i)
        if (list.entries[i].binding != binding) list.entries[kept++] = list.entries[i];
    list.count = kept;
}

// The plugin's replacement is read from the host's buffer, never from a pointer the plugin could have changed.
bool replacement_valid(const char* buffer, u32 bytes) noexcept {
    return bytes && bytes <= MC_DRAFT_TEXT_MAX && utf8_validate({buffer, bytes}).valid;
}

} // namespace

bool plugin_dispatch_add(PluginBinding* binding) noexcept {
    const mc_descriptor& descriptor = *binding->image.descriptor;
    if (!descriptor.hook_count) return true;
    if (!g_buffers) {
        g_buffers = static_cast<u8*>(VirtualAlloc(nullptr, 2 * kBufferBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_buffers) return false;
        ++g_stats.allocations;
    }
    ++g_subscribed_plugins;
    for (u32 i = 0; i < descriptor.hook_count; ++i) {
        const mc_hook& hook = descriptor.hooks[i];
        List& list = g_lists[hook.kind];
        list.entries[list.count++] = {binding, hook.fn.draft};
        g_plugin_hook_subscribers[hook.kind] = list.count;
    }
    return true;
}

void plugin_dispatch_remove(PluginBinding* binding) noexcept {
    const mc_descriptor& descriptor = *binding->image.descriptor;
    if (!descriptor.hook_count) return;
    for (u32 kind = 1; kind < static_cast<u32>(PluginHook::count); ++kind) {
        remove_from(g_lists[kind], binding);
        g_plugin_hook_subscribers[kind] = g_lists[kind].count;
    }
    if (--g_subscribed_plugins == 0 && g_buffers) {
        VirtualFree(g_buffers, 0, MEM_RELEASE);
        g_buffers = nullptr;
    }
}

PluginDraftResult plugin_dispatch_draft(PluginHook hook, const PluginDraft& draft) noexcept {
    PluginDraftResult result{PluginDraftVerdict::keep, {}, 0};
    const List& list = g_lists[static_cast<u32>(hook)];
    if (!list.count || draft.text.bytes > MC_DRAFT_TEXT_MAX) return result;
    Utf8View current = draft.text;
    bool replaced = false;
    u32 next = 0;
    for (u32 i = 0; i < list.count; ++i) {
        const Subscriber& subscriber = list.entries[i];
        if (subscriber.binding->struck_out) continue;
        char* buffer = reinterpret_cast<char*>(g_buffers) + next * kBufferBytes;
        mc_draft call{draft.channel_id, draft.guild_id, draft.message_id, draft.reply_to,
                      {current.data, current.bytes, 0}, buffer, kBufferBytes, 0, 0, MC_DRAFT_KEEP};
        const PluginCallResult ran = plugin_binding_run_draft(subscriber.binding, static_cast<u32>(hook),
                                                              subscriber.function, &call);
        ++g_stats.calls;
        if (ran.overran) {
            ++g_stats.strikes;
            continue;
        }
        if (ran.status != MC_OK) continue;
        if (call.verdict == MC_DRAFT_CANCEL) return {PluginDraftVerdict::cancel, {}, subscriber.binding->owner_id};
        if (call.verdict == MC_DRAFT_REPLACE && replacement_valid(buffer, call.replaced_bytes)) {
            current = {buffer, call.replaced_bytes};
            replaced = true;
            next ^= 1;
        }
    }
    if (replaced) result = {PluginDraftVerdict::replace, current, 0};
    return result;
}

const PluginDispatchStats& plugin_dispatch_stats() noexcept {
    return g_stats;
}

} // namespace maicord
