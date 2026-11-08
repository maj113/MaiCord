#include "plugins.hpp"
#include "maicord/app/plugins/dispatch.hpp"
#include "../shell/state.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/composer/reply_draft.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/protocol/messages/create.hpp"
#include "pipeline_host.hpp"
#include "maicord/model/chat/message_composer.hpp"
#include "../account/account_ui_runtime.hpp"

// Composer integration for plugins: the pre-send and pre-edit hooks, run before application commands, the
// ordinary send and the ordinary edit save, and the notice a stopped send leaves under the composer.
namespace maicord::app_detail {
namespace {
constexpr u64 kComposerNoticeMs = 8000;

struct ComposerNotice {
    char text[256];
    u32 bytes;
    u64 until;
};
ComposerNotice g_notice;

u64 channel_guild(u64 channel) noexcept {
    const auto* record = g_app.account_ui && channel ?
        chat_state_store_find_channel(g_app.account_ui->chat_state, channel) : nullptr;
    return record ? record->guild_id : 0;
}
void set_composer_notice(Utf8View text) noexcept {
    g_notice.bytes = utf8_copy_truncated(g_notice.text, text);
    g_notice.until = g_notice.bytes ? clock_monotonic_milliseconds() + kComposerNoticeMs : 0;
}

} // namespace

// The ABI-6 hooks rewrite, cancel or question the text in the composer (the editor's text while a
// message is being edited); the ordinary send or save then uses it. A send's flags travel as the
// draft's own state: "@silent " in front of the text, and the reply's mention.
bool application_plugins_compose(bool* handled) noexcept {
    *handled = false;
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
    PluginDraft draft{composer.channel_id, channel_guild(composer.channel_id),
                      editing ? g_app.message_edit.message_id : 0, reply.message_id, text, flags};
    if (const MessageRecord* replied = reply.message_id ? application_message_record(reply.message_id) : nullptr)
        draft.reply_author_id = replied->author_id;
    const PluginDraftResult result = plugin_dispatch_draft(hook, draft);
    if (result.verdict == PluginDraftVerdict::cancel) {
        *handled = true;
        set_composer_notice(editing ? utf8_literal("A plugin stopped this edit from being saved.")
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

Utf8View application_plugins_composer_notice() noexcept {
    return {g_notice.text, g_notice.bytes};
}

u32 application_plugins_composer_notice_wait(u64 now_ms) noexcept {
    if (!g_notice.bytes) return INFINITE;
    return g_notice.until <= now_ms ? 0 : static_cast<u32>(minimum<u64>(g_notice.until - now_ms, 0x7FFFFFFFu));
}

void application_plugins_composer_notice_poll(u64 now_ms) noexcept {
    if (!g_notice.bytes || now_ms < g_notice.until) return;
    g_notice.bytes = 0;
    if (g_app.account_ui) refresh_message_composer_ui();
}

} // namespace maicord::app_detail
