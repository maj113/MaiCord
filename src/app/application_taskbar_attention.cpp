#include "application_taskbar_attention.hpp"
#include "application_friends.hpp"
#include "application_state.hpp"
#include "maicord/platform/taskbar_attention.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
struct Owner {
    TaskbarAttention native;
    u64 notify_serial;
    u64 key;
    u64 recount_at_ms;
    u32 mentions;
    bool unread, initialized;
};
Owner g_owner{};

bool focused() noexcept {
    return g_app.window && GetForegroundWindow() == g_app.window && !IsIconic(g_app.window);
}
// The official client's badge: mentions plus incoming friend requests; unread alone is a dot.
void recount() noexcept {
    g_owner.mentions = 0;
    g_owner.unread = false;
    if (!g_app.account_ui || g_app.login.visible)
        return;
    const auto& ui = *g_app.account_ui;
    const auto& state = ui.chat_state;
    for (u32 order = 0; order < state.guild_count; ++order) {
        const u16 slot = state.guild_order[order];
        if (slot >= state.guild_capacity)
            continue;
        g_owner.mentions += state.guilds[slot].mention_count;
        g_owner.unread = g_owner.unread || state.guilds[slot].unread_channels != 0;
    }
    const auto& privates = ui.private_channel_store;
    for (u32 i = 0; i < privates.count; ++i) {
        const auto* channel = chat_state_store_find_channel(state, privates.records[i].id);
        if (!channel)
            continue;
        g_owner.mentions += channel->mention_count;
        g_owner.unread = g_owner.unread || chat_state_store_channel_unread(state, *channel);
    }
    if (const auto* relationships = application_friends_relationships())
        for (u32 i = 0; i < relationships->count; ++i)
            g_owner.mentions += relationships->records[i].type == RelationshipType::incoming;
}
u64 source_key() noexcept {
    if (!g_app.account_ui)
        return 0;
    const auto& ui = *g_app.account_ui;
    const auto* relationships = application_friends_relationships();
    return (static_cast<u64>(ui.chat_state.guild_mutations) << 40) ^
           (static_cast<u64>(ui.chat_state.channel_mutations) << 20) ^ ui.read_state_store.revision ^
           (static_cast<u64>(ui.navigation_view.revision) << 52) ^ ui.private_channel_store.revision ^
           (relationships ? static_cast<u64>(relationships->revision) << 32 : 0);
}
} // namespace

void application_taskbar_attention_tick(u64 now_ms) noexcept {
    if (!g_app.window)
        return;
    if (!g_owner.initialized) {
        taskbar_attention_initialize(&g_owner.native);
        // The button may predate registration of TaskbarButtonCreated.
        g_owner.native.button_ready = IsWindowVisible(g_app.window) != FALSE;
        g_owner.initialized = true;
    }
    // Counters cover most changes; a slow periodic recount catches the rest.
    const u64 key = source_key();
    if (key != g_owner.key || now_ms >= g_owner.recount_at_ms) {
        g_owner.key = key;
        g_owner.recount_at_ms = now_ms + 1000;
        recount();
    }
    const TaskbarAttentionInput input{g_owner.mentions, g_owner.notify_serial, g_owner.unread,
                                      g_app.preferences.unread_badge, g_app.preferences.taskbar_flash, focused()};
    const auto plan = taskbar_attention_plan(&g_owner.native.state, input);
    if (plan.set_badge || plan.start_flash || plan.stop_flash)
        taskbar_attention_apply(&g_owner.native, g_app.window, plan, g_app.renderer.dpi);
}

void application_taskbar_attention_notify() noexcept {
    if (!focused())
        ++g_owner.notify_serial;
}

bool application_taskbar_attention_window_message(UINT message) noexcept {
    return g_owner.initialized && taskbar_attention_window_message(&g_owner.native, g_app.window, message);
}

void application_taskbar_attention_counts(u32* mentions, bool* unread) noexcept {
    if (mentions)
        *mentions = g_owner.mentions;
    if (unread)
        *unread = g_owner.unread;
}

void application_taskbar_attention_release() noexcept {
    if (g_owner.initialized)
        taskbar_attention_release(&g_owner.native, g_app.window);
    g_owner = {};
}

} // namespace maicord::app_detail
