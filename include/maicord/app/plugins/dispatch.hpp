#pragma once

#include "maicord/core/slice.hpp"

// What the application calls to run plugin hooks. The lists behind it are built
// when a plugin is enabled (binding.hpp), so a hook nobody subscribes to costs
// the one load and branch of plugin_hook_subscribed. This header stays free of
// the plugin ABI header; hook points include it from ordinary application code.
namespace maicord {

enum class PluginHook : u8 { none, pre_send, pre_edit, count };

// Enabled plugins per hook, written only by plugin_dispatch_add/remove (UI thread).
extern u32 g_plugin_hook_subscribers[static_cast<u32>(PluginHook::count)];

inline bool plugin_hook_subscribed(PluginHook hook) noexcept {
    return g_plugin_hook_subscribers[static_cast<u32>(hook)] != 0;
}

// A message the user is about to send (pre_send) or save as an edit (pre_edit).
struct PluginDraft {
    u64 channel_id;
    u64 guild_id;   // 0 in direct messages
    u64 message_id; // pre_edit: the message being edited
    u64 reply_to;   // pre_send: the replied-to message, or 0
    Utf8View text;
};

enum class PluginDraftVerdict : u8 { keep, replace, cancel };

struct PluginDraftResult {
    PluginDraftVerdict verdict;
    // replace: the new text, valid until the next dispatch of any hook.
    Utf8View text;
    // cancel: the plugin that stopped the draft.
    u64 owner_id;
};

// Runs every subscribed plugin in enable order, each seeing the text the earlier
// ones produced. A plugin that fails, overruns its budget or returns invalid
// output leaves the draft as it was. Returns keep without a call when nobody
// subscribes or the text exceeds what the ABI can carry. Allocates nothing.
PluginDraftResult plugin_dispatch_draft(PluginHook hook, const PluginDraft& draft) noexcept;

// Hook calls made and strikes taken since process start; for tests and the dev console.
struct PluginDispatchStats {
    u32 calls;
    u32 strikes;
    // Times the dispatcher committed memory (when the first plugin subscribes).
    u32 allocations;
};
const PluginDispatchStats& plugin_dispatch_stats() noexcept;

} // namespace maicord
