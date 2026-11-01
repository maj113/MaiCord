#include "runtime.hpp"
#include "maicord/app/plugins/dispatch.hpp"
#include "../shell/state.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/composer/reply_draft.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/messages/create.hpp"
#include "pipeline_host.hpp"
#include "maicord/model/chat/message_composer.hpp"
#include "../account/account_ui_runtime.hpp"

// Composer integration for plugins: ABI-5 local slash commands and the ABI-5
// pre-send transform, then the ABI-6 pre-send and pre-edit hooks, run before
// application commands, the ordinary send and the ordinary edit save.
namespace maicord::app_detail {
namespace {
constexpr u64 kComposerNoticeMs = 8000;

u64 channel_guild(u64 channel) noexcept {
    const auto* record = g_app.account_ui && channel ?
        chat_state_store_find_channel(g_app.account_ui->chat_state, channel) : nullptr;
    return record ? record->guild_id : 0;
}
void set_composer_notice(ApplicationPluginsSdk& sdk, Utf8View text) noexcept {
    const u32 bytes = utf8_copy_truncated(sdk.composer_notice, text);
    sdk.composer_notice_bytes = bytes;
    sdk.composer_notice_until = bytes ? clock_monotonic_milliseconds() + kComposerNoticeMs : 0;
    sdk.composer_notice_tone = 0;
    sdk.composer_refresh = refresh_message_composer_ui;
}

bool compose_abi5(bool* handled) noexcept {
    *handled = false;
    auto* sdk = g_app.plugins ? g_app.plugins->sdk : nullptr;
    if (!sdk || !sdk->runtime.enabled || !g_app.account_ui || g_app.message_edit.editor_active) return true;
    auto& composer = g_app.message_composer;
    const Utf8View text = message_composer_text(composer);
    const NativePluginSdkDraft draft{composer.channel_id, channel_guild(composer.channel_id),
                                    g_app.account_ui->chat_state.self_user_id, text};
    auto result = native_plugin_sdk_run_command(&sdk->runtime, draft);
    if (result.action == NativePluginSdkComposeAction::none) {
        // Other slash text belongs to application commands; transforms see messages only.
        if (!text.bytes || text.data[0] == '/') return true;
        result = native_plugin_sdk_pre_send(&sdk->runtime, draft);
        if (result.action == NativePluginSdkComposeAction::cancel) {
            *handled = true;
            set_composer_notice(*sdk, utf8_literal("A plugin stopped this message from being sent."));
            return true;
        }
        if (result.action != NativePluginSdkComposeAction::replace) return true;
        if (!result.text.bytes) {
            *handled = true;
            return true;
        }
        return message_composer_replace_range(&composer, 0, composer.bytes, result.text) !=
               MessageComposerResult::invalid_input;
    }
    switch (result.action) {
    case NativePluginSdkComposeAction::send:
        // The normal send path sends the command's text.
        return message_composer_replace_range(&composer, 0, composer.bytes, result.text) !=
               MessageComposerResult::invalid_input;
    case NativePluginSdkComposeAction::replace:
        *handled = true;
        return composer_apply_result(message_composer_replace_range(&composer, 0, composer.bytes, result.text), true);
    case NativePluginSdkComposeAction::notice:
        *handled = true;
        set_composer_notice(*sdk, result.text);
        return true;
    default:
        *handled = true;
        return composer_apply_result(message_composer_replace_range(&composer, 0, composer.bytes, {}), true);
    }
}

// The ABI-6 hooks rewrite, cancel or question the text in the composer (the editor's text while a
// message is being edited); the ordinary send or save then uses it. A send's flags travel as the
// draft's own state: "@silent " in front of the text, and the reply's mention.
bool compose_hooks(bool* handled) noexcept {
    const bool editing = g_app.message_edit.editor_active;
    const PluginHook hook = editing ? PluginHook::pre_edit : PluginHook::pre_send;
    if (!plugin_hook_subscribed(hook) || !g_app.account_ui) return true;
    auto& composer = g_app.message_composer;
    const Utf8View text = message_composer_text(composer);
    // Slash text belongs to application commands; hooks see messages only.
    if (!text.bytes || (!editing && text.data[0] == '/')) return true;
    const MessageComposerReplySnapshot reply = editing ? MessageComposerReplySnapshot{} : application_reply_draft();
    Utf8View body{};
    u32 flags = 0;
    if (!editing) {
        if (message_silent_prefix(text, &body)) flags |= kPluginDraftSilent;
        if (reply.message_id && reply.mention_reply_author) flags |= kPluginDraftReplyPing;
    }
    const PluginDraft draft{composer.channel_id, channel_guild(composer.channel_id),
                            editing ? g_app.message_edit.message_id : 0, reply.message_id, text, flags};
    const PluginDraftResult result = plugin_dispatch_draft(hook, draft);
    if (result.verdict == PluginDraftVerdict::cancel) {
        *handled = true;
        if (auto* sdk = g_app.plugins ? application_plugins_sdk_acquire(*g_app.plugins) : nullptr)
            set_composer_notice(*sdk, editing ? utf8_literal("A plugin stopped this edit from being saved.")
                                              : utf8_literal("A plugin stopped this message from being sent."));
        return true;
    }
    const bool replaced = result.verdict == PluginDraftVerdict::replace ||
                          (result.verdict == PluginDraftVerdict::confirm && result.text.bytes);
    // Changed flags: the silent prefix is added or removed on the text that goes on.
    char storage[kPluginDraftTextMax + 16];
    Utf8View next = replaced ? result.text : text;
    bool changed = replaced;
    if (!editing && ((result.flags ^ flags) & kPluginDraftSilent)) {
        Utf8View rest = next;
        const bool prefixed = message_silent_prefix(next, &rest);
        const Utf8View silent = utf8_literal("@silent ");
        const Utf8View core = prefixed ? rest : next;
        if ((result.flags & kPluginDraftSilent) && core.bytes + silent.bytes <= sizeof(storage)) {
            copy_bytes(storage, silent.data, silent.bytes);
            copy_bytes(storage + silent.bytes, core.data, core.bytes);
            next = {storage, silent.bytes + core.bytes};
        } else if (!(result.flags & kPluginDraftSilent)) {
            next = core;
        }
        changed = true;
    }
    if (!editing && reply.message_id && ((result.flags ^ flags) & kPluginDraftReplyPing))
        application_reply_draft_set(reply.message_id, (result.flags & kPluginDraftReplyPing) != 0);
    if (!editing) application_plugins_suppress_embeds_next((result.flags & kPluginDraftSuppressEmbeds) != 0);
    if (changed && message_composer_replace_range(&composer, 0, composer.bytes, next) ==
                       MessageComposerResult::invalid_input)
        return false;
    if (result.verdict == PluginDraftVerdict::confirm) {
        // The send continues from the dialog's accept; until then the composer keeps the text.
        *handled = true;
        application_plugins_send_confirm(result.question);
    }
    return true;
}
} // namespace

bool application_plugins_compose(bool* handled) noexcept {
    if (!compose_abi5(handled)) return false;
    return *handled || compose_hooks(handled);
}

} // namespace maicord::app_detail
