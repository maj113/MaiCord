#pragma once

#include "maicord/core/slice.hpp"

// What the application calls to run plugin hooks. The lists behind it are built
// when a plugin is enabled (binding.hpp), so a hook nobody subscribes to costs
// the one load and branch of plugin_hook_subscribed. This header stays free of
// the plugin ABI header; hook points include it from ordinary application code.
namespace maicord {

// One value per MC_HOOK_* kind, in ABI order (dispatch.cpp asserts it).
enum class PluginHook : u8 {
    none,
    pre_send,
    pre_edit,
    message_click,
    message_deleted,
    message_edited,
    events,
    decorate_message,
    decorate_user,
    decorate_channel,
    decorate_guild,
    decorate_header,
    node_action,
    pre_typing,
    setting_changed,
    keybind,
    timer,
    completion,
    count
};

// Enabled plugins per hook, written only by plugin_dispatch_add/remove (UI thread).
extern u32 g_plugin_hook_subscribers[static_cast<u32>(PluginHook::count)];

inline bool plugin_hook_subscribed(PluginHook hook) noexcept {
    return g_plugin_hook_subscribers[static_cast<u32>(hook)] != 0;
}

// PluginDraft::flags (the values of MC_DRAFT_*; dispatch.cpp asserts it).
inline constexpr u32 kPluginDraftSilent = 1;
inline constexpr u32 kPluginDraftSuppressEmbeds = 2;
inline constexpr u32 kPluginDraftReplyPing = 4;
inline constexpr u32 kPluginDraftTextMax = 4000;

// A message the user is about to send (pre_send) or save as an edit (pre_edit).
struct PluginDraft {
    u64 channel_id;
    u64 guild_id;   // 0 in direct messages
    u64 message_id; // pre_edit: the message being edited
    u64 reply_to;   // pre_send: the replied-to message, or 0
    Utf8View text;
    u32 flags = 0;  // pre_send: MC_DRAFT_SILENT, MC_DRAFT_SUPPRESS_EMBEDS, MC_DRAFT_REPLY_PING
    u64 reply_author_id = 0; // pre_send: the author of reply_to, or 0 when unknown
};

enum class PluginDraftVerdict : u8 { keep, replace, cancel, confirm };

struct PluginDraftResult {
    PluginDraftVerdict verdict;
    // replace: the new text; confirm: the new text when a plugin also replaced it, else empty.
    // Valid until the next dispatch of any hook.
    Utf8View text;
    // cancel: the plugin that stopped the draft; confirm: the plugin that asks.
    u64 owner_id;
    // pre_send: the flags after every plugin ran (the input flags when none changed them).
    u32 flags = 0;
    // confirm: what to ask the user, valid until the next dispatch of any hook.
    Utf8View question;
};

// Runs every subscribed plugin in enable order, each seeing the text the earlier
// ones produced. A plugin that fails, overruns its budget or returns invalid
// output leaves the draft as it was. Returns keep without a call when nobody
// subscribes or the text exceeds what the ABI can carry. Allocates nothing.
PluginDraftResult plugin_dispatch_draft(PluginHook hook, const PluginDraft& draft) noexcept;

// Hook calls made and strikes taken since process start; for tests.
struct PluginDispatchStats {
    u32 calls;
    u32 strikes;
    // Times the dispatcher committed memory (when the first plugin subscribes).
    u32 allocations;
};
const PluginDispatchStats& plugin_dispatch_stats() noexcept;

// The result of an operation a plugin started (an HTTP request, a modal, a banner), delivered to its
// completion hook. Pointers in it are read during the call only.
enum class PluginCompletionKind : u8 { none, http, modal, banner };
struct PluginCompletion {
    PluginCompletionKind kind;
    u64 tag; // what the plugin passed when it started the operation
    // A user gesture caused this (a modal accepted, a banner clicked): the call counts as one for kPluginGestureMilliseconds.
    bool gesture;
    // http: error is an MC_HTTP_E_* value (0 = a response arrived), body the response bytes.
    u32 http_error, http_status;
    ConstSlice<u8> body;
    // http: when the gesture of the request that started this stops counting (0 = none); the completion
    // call counts as a gesture until then, and requests it makes keep the same deadline.
    u64 inherited_gesture_until_ms;
    // modal
    u32 accepted, toggle, select_index;
    Utf8View text; // the input, valid for the call
    // banner: 1 = the action was clicked, 2 = dismissed
    u32 which;
};
inline constexpr u64 kPluginGestureMilliseconds = 5000;
// The modifier bits of a keybind value (MC_KEY_CTRL, MC_KEY_SHIFT, MC_KEY_ALT) with the virtual-key code in the low byte.
inline constexpr u32 kPluginKeyControl = 0x10000, kPluginKeyShift = 0x20000, kPluginKeyAlt = 0x40000;
// The user pressed key: calls MC_HOOK_KEYBIND, with a gesture, for every setting of an enabled plugin bound to
// exactly that key. True when one was called. Keys need Ctrl or Alt, or are function keys, so typing is never taken.
bool plugin_dispatch_keybind(u32 key) noexcept;
// Runs the owner's completion hook. False when the plugin is not enabled, has no completion hook, or
// overran its budget.
bool plugin_dispatch_completion(u64 owner_id, const PluginCompletion&) noexcept;
// Same, for a binding the caller already holds.
struct PluginBinding;
bool plugin_binding_complete(PluginBinding*, const PluginCompletion&) noexcept;

} // namespace maicord
