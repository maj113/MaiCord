#include "application_mention_confirmation.hpp"

#include "application_message_send.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"

#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::app_detail {
namespace {

TextFormOwnerKey mention_confirmation;

bool mention_confirmation_valid(TextFormOwnerKey key) noexcept {
    const MessageComposer& composer = g_app.message_composer;
    return g_app.account_ui && key.scope == g_app.plugin_account_scope_serial &&
           key.scope == mention_confirmation.scope && key.collection == mention_confirmation.collection &&
           key.item == mention_confirmation.item && composer.channel_id == key.collection &&
           g_app.account_ui->active_channel_id == key.collection && composer.text_revision == key.item &&
           !g_app.message_edit.editor_active;
}

bool mention_confirmation_save(TextFormOwnerKey key, Utf8View) noexcept {
    // The draft changed or the channel went away: closing is the answer.
    if (!mention_confirmation_valid(key))
        return true;
    mention_confirmation = {};
    application_message_send_confirmed();
    return true;
}

MessageMentionWarningPlace mention_place(const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    if (channel.type != 10 && channel.type != 11 && channel.type != 12)
        return MessageMentionWarningPlace::channel;
    const ChannelRecord* parent = channel.parent_id ? chat_state_store_find_channel(state, channel.parent_id) : nullptr;
    return parent && (parent->type == 15 || parent->type == 16) ? MessageMentionWarningPlace::forum_post
                                                                : MessageMentionWarningPlace::thread;
}

} // namespace

bool application_mention_confirmation_request(bool* opened) noexcept {
    *opened = false;
    const MessageComposer& composer = g_app.message_composer;
    const AccountUiRuntime& ui = *g_app.account_ui;
    const ChannelRecord* channel = chat_state_store_find_channel(ui.chat_state, composer.channel_id);
    if (!channel || !channel->guild_id)
        return true;
    MessageMentionWarningInput input{};
    input.content = message_composer_text(composer);
    input.guild_channel = true;
    input.can_mention_everyone = (channel->actions & channel_action_mention_everyone) != 0;
    input.place = mention_place(ui.chat_state, *channel);
    if (input.place == MessageMentionWarningPlace::channel) {
        if (const auto* list = guild_member_store_list(ui.guild_member_store, channel->guild_id,
                                                       channel->member_list_id)) {
            input.member_count = list->member_count_known ? list->member_count : 0;
            input.online_count = list->online_count_known ? list->online_count : 0;
        }
    } else if (ui.thread_member_store.known && ui.thread_member_store.thread_id == channel->id) {
        input.thread_member_count = ui.thread_member_store.count;
    }
    const MessageMentionWarning warning = message_mention_warning_evaluate(input);
    if (!warning.required)
        return true;
    constexpr Utf8View footer = utf8_literal("\n\nMaybe try mentioning a role or individual users instead?");
    char context[kMessageMentionWarningBodyBytes + footer.bytes];
    const u32 body = message_mention_warning_body(warning, input.place, {context, kMessageMentionWarningBodyBytes});
    if (!body)
        return true;
    copy_bytes(context + body, footer.data, footer.bytes);
    mention_confirmation = {g_app.plugin_account_scope_serial, composer.channel_id, composer.text_revision};
    ApplicationTextFormRequest request{warning.here ? utf8_literal("Mention @here?") : utf8_literal("Mention @everyone?"),
                                       {context, body + footer.bytes},
                                       {},
                                       utf8_literal("Send now"),
                                       {},
                                       mention_confirmation,
                                       mention_confirmation_valid,
                                       mention_confirmation_save,
                                       0,
                                       true};
    request.accept_focused = true;
    if (!application_text_form_open(request)) {
        mention_confirmation = {};
        return true;
    }
    *opened = true;
    return true;
}

} // namespace maicord::app_detail
