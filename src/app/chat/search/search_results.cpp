#include "search_internal.hpp"

namespace maicord::app_detail {
namespace {

// Compare normalized owning rich values, never scratch pointers or text arena
// offsets. Overlapping context groups may reuse an identity only when every
// retained field is the same.
bool rich_body_same(const MessageRichStore& store, const MessageRichRecord* rich,
                    const GatewayMessageEvent& m) noexcept {
    if (!rich)
        return !m.attachments.count && !m.embeds.count && !m.embed_fields.count && !m.stickers.count &&
               !m.components.count && !m.reactions.count &&
               m.reference.state == GatewayMessageReferenceState::none && !m.interaction.user_id &&
               !m.snapshot.present;
    if (rich->attachment_count != m.attachments.count || rich->embed_count != m.embeds.count ||
        rich->embed_field_count != m.embed_fields.count || rich->sticker_count != m.stickers.count ||
        rich->component_count != m.components.count || rich->reaction_count != m.reactions.count)
        return false;
    const auto str = [&](MessageRichString a, Utf8View v) noexcept {
        return utf8_equal(message_rich_store_string(store, a), v);
    };
    for (u32 i = 0; i < m.attachments.count; ++i) {
        const auto& a = rich->attachments[i];
        const auto& v = m.attachments[i];
        if (a.id != v.id || a.size != v.size || a.width != v.width || a.height != v.height ||
            a.flags != v.flags ||
            a.state != ((v.has_width ? 1 : 0) | (v.has_height ? 2 : 0) | (v.ephemeral ? 4 : 0)) ||
            !str(a.filename, v.filename) || !str(a.title, v.title) || !str(a.description, v.description) ||
            !str(a.content_type, v.content_type) || !str(a.url, v.url))
            return false;
    }
    const auto media = [&](const MessageRichEmbedMedia& a, const GatewayMessageEmbedMedia& v) noexcept {
        return a.width == v.width && a.height == v.height && a.flags == v.flags &&
               a.state == ((v.has_width ? 1 : 0) | (v.has_height ? 2 : 0)) && str(a.url, v.url) &&
               str(a.proxy_url, v.proxy_url) && str(a.content_type, v.content_type) &&
               str(a.description, v.description);
    };
    for (u32 i = 0; i < m.embeds.count; ++i) {
        const auto& a = rich->embeds[i];
        const auto& v = m.embeds[i];
        if (a.color != v.color || a.field_begin != v.field_begin || a.field_count != v.field_count ||
            a.type != v.type || a.has_color != v.has_color ||
            a.has_title != (v.has_title || v.title.bytes != 0) || a.has_author != v.has_author ||
            !str(a.title, v.title) || !str(a.description, v.description) || !str(a.url, v.url) ||
            !str(a.author_name, v.author_name) || !str(a.footer_text, v.footer_text) ||
            !str(a.author_url, v.author_url) || !str(a.author_icon, v.author_icon) ||
            !str(a.footer_icon, v.footer_icon) || !str(a.provider_name, v.provider_name) ||
            !str(a.provider_url, v.provider_url) || a.timestamp_ms != v.timestamp_ms ||
            !media(a.image, v.image) || !media(a.thumbnail, v.thumbnail) || !media(a.video, v.video))
            return false;
    }
    for (u32 i = 0; i < m.embed_fields.count; ++i) {
        const auto& a = rich->embed_fields[i];
        const auto& v = m.embed_fields[i];
        if (a.embed_index != v.embed_index || a.inline_field != v.inline_field || !str(a.name, v.name) ||
            !str(a.value, v.value))
            return false;
    }
    for (u32 i = 0; i < m.stickers.count; ++i) {
        const auto& a = rich->stickers[i];
        const auto& v = m.stickers[i];
        if (a.id != v.id || a.format_type != v.format_type || !str(a.name, v.name))
            return false;
    }
    for (u32 i = 0; i < m.components.count; ++i) {
        const auto& a = rich->components[i];
        const auto& v = m.components[i];
        if (a.id != v.id || a.accent_color != v.accent_color || a.parent != v.parent ||
            a.order != v.order || a.type != v.type || a.style != v.style || a.disabled != v.disabled ||
            a.has_accent_color != v.has_accent_color || !str(a.label, v.label) ||
            !str(a.content, v.content) || !str(a.custom_id, v.custom_id) || !str(a.url, v.url))
            return false;
    }
    for (u32 i = 0; i < m.reactions.count; ++i) {
        const auto& a = rich->reactions[i];
        const auto& v = m.reactions[i];
        if (a.emoji_id != v.emoji_id || a.count != v.count || a.normal_count != v.normal_count ||
            a.burst_count != v.burst_count ||
            a.flags != ((v.me ? 1 : 0) | (v.me_burst ? 2 : 0) | (v.animated ? 4 : 0)) ||
            !str(a.name, v.name))
            return false;
    }
    if (!rich->forwarded_snapshot_child) {
        const auto& a = rich->reference;
        const auto& v = m.reference;
        if (a.message_id != v.message_id || a.channel_id != v.channel_id || a.guild_id != v.guild_id ||
            a.author_id != v.author_id || a.type != v.type || a.state != v.state ||
            !str(a.author_name, v.author_name) || !str(a.author_avatar_hash, v.author_avatar_hash) ||
            !str(a.content, v.content))
            return false;
        const auto& x = rich->interaction;
        const auto& y = m.interaction;
        if (x.id != y.id || x.user_id != y.user_id || x.type != y.type || !str(x.user_name, y.user_name) ||
            !str(x.user_avatar_hash, y.user_avatar_hash) || !str(x.command_name, y.command_name))
            return false;
        if (rich->has_forwarded_snapshot != m.snapshot.present)
            return false;
        if (m.snapshot.present) {
            const auto* child = message_rich_store_snapshot(store, rich->message_id);
            if (!child)
                return false;
            const auto& c = child->snapshot;
            const auto& s = m.snapshot;
            if (c.timestamp_ms != s.timestamp_ms || c.edited_timestamp_ms != s.edited_timestamp_ms ||
                c.flags != s.flags || c.type != s.type || !str(c.content, s.content))
                return false;
            GatewayMessageEvent body{};
            body.attachments = s.attachments;
            body.embeds = s.embeds;
            body.embed_fields = s.embed_fields;
            body.stickers = s.stickers;
            body.components = s.components;
            if (!rich_body_same(store, child, body))
                return false;
        }
    }
    return true;
}

} // namespace
namespace search_detail {

bool bank_accept(void* opaque, u32 index, const MessageSearchMessage& item) noexcept {
    auto& b = *static_cast<SearchBank*>(opaque);
    const auto& m = item.message;
    if (index >= kMessageSearchMaximumMessages || m.rich_truncations || m.snapshot.rich_truncations)
        return false;
    const auto existing = message_store_find(b.store, m.id);
    const u8 poll_field = (m.fields & gateway_message_field_poll) ? (m.poll.present ? 2 : 1) : 0;
    for (u32 i = 0; existing.ok() && i < index; ++i)
        if (b.messages[i].message.id == m.id && b.poll_fields[i] != poll_field)
            return false;
    b.poll_fields[index] = poll_field;
    if (m.fields & gateway_message_field_poll) {
        // The sink is provisional until every group and scope check succeeds.
        // Its strings/descriptors must survive reuse of the per-message decoder.
        const auto staged = message_poll_store_apply(&b.staged_polls, m.id, m.channel_id, m.poll);
        if (!staged.ok() || (existing.ok() && staged.changed))
            return false;
    }
    if (existing.ok()) {
        const auto* r = message_store_resolve(b.store, existing.handle);
        const u64 timestamp = m.timestamp_ms ? m.timestamp_ms : snowflake_timestamp_ms(m.id);
        if (!r || r->channel_id != m.channel_id || r->author_id != m.author_id ||
            r->timestamp_ms != timestamp || r->edited_timestamp_ms != m.edited_timestamp_ms ||
            r->flags != m.flags || r->type != m.type ||
            !utf8_equal(message_store_content(b.store, existing.handle), m.content) ||
            !rich_body_same(b.rich, message_rich_store_find(b.rich, m.id), m))
            return false;
        for (u32 i = 0; i < index; ++i)
            if (b.result_handles[i].slot == existing.handle.slot) {
                if (!utf8_equal({b.author_names[i], b.author_bytes[i]}, m.author_name) ||
                    !utf8_equal({b.avatars[i], b.avatar_bytes[i]}, m.author_avatar_hash))
                    return false;
                break;
            }
        b.result_handles[index] = existing.handle;
    } else {
        if (!stable_storage_grow(&b.content_storage, maximum(1u, b.store.text_used + m.content.bytes)))
            return false;
        const MessageInput input{
            m.id,
            m.channel_id,
            m.author_id,
            m.timestamp_ms,
            m.edited_timestamp_ms,
            m.content,
            m.flags,
            MessageContentAvailability::available,
            MessageDeliveryState::authoritative,
            (m.pinned ? message_presentation_pinned : 0u) |
                (m.mentions_current_user ? message_presentation_mentions_current_user : 0u) |
                (m.poll.present ? message_presentation_has_poll : 0u),
            m.type};
        const auto result = message_store_upsert(&b.store, input);
        if (!result.ok())
            return false;
        b.result_handles[index] = result.handle;
        const u32 needed = b.rich.text_used + m.transient_bytes;
        if (!text_reserve_both(b.rich_text, needed))
            return false;
        const auto rich = message_rich_store_apply(&b.rich, m.id, 1, m);
        if (!rich.ok() || rich.storage_truncated)
            return false;
    }
    if (m.author_name.bytes > 128 || m.author_avatar_hash.bytes > 34)
        return false;
    copy_bytes(b.author_names[index], m.author_name.data, m.author_name.bytes);
    b.author_bytes[index] = static_cast<u8>(m.author_name.bytes);
    copy_bytes(b.avatars[index], m.author_avatar_hash.data, m.author_avatar_hash.bytes);
    b.avatar_bytes[index] = static_cast<u8>(m.author_avatar_hash.bytes);
    if (!bank_user(b, m.author_id, m.author_name, m.author_avatar_hash) ||
        !bank_user(b, m.reference.author_id, m.reference.author_name, m.reference.author_avatar_hash) ||
        !bank_user(b, m.interaction.user_id, m.interaction.user_name, m.interaction.user_avatar_hash))
        return false;
    if (!bank_mentions(b, m.mention_users) || !bank_mentions(b, m.snapshot.mention_users))
        return false;
    const auto* message_channel = chat_state_store_find_channel(g_app.account_ui->chat_state, m.channel_id);
    const u64 guild = item.guild_known ? item.guild_id : message_channel ? message_channel->guild_id : 0;
    if (guild && (m.fields & gateway_message_field_member)) {
        const auto member = guild_member_store_upsert(&b.members, g_app.account_ui->chat_state,
                                                              guild, m.author_id, m.member_nick, m.member_roles,
                                                              true, GuildMemberStatus::unknown);
        if (!member.ok() || member.evicted)
            return false;
    }
    return true;
}

} // namespace search_detail
namespace {

struct SearchPollPublication {
    MessagePollPageInput input;
    GatewayMessagePoll poll;
    MessagePollResultTicket ticket;
    GatewayMessagePollAnswer answers[kGatewayMessagePollMaximumAnswers];
    GatewayMessagePollAnswerCount counts[kGatewayMessagePollMaximumAnswers];
};
// The first result carrying a message's poll; later copies of the message are skipped.
bool poll_first(const SearchBank& bank, u32 index) noexcept {
    if (!bank.poll_fields[index])
        return false;
    for (u32 j = 0; j < index; ++j)
        if (bank.messages[j].message.id == bank.messages[index].message.id)
            return false;
    return true;
}

} // namespace
namespace search_detail {

bool publish_polls(ApplicationSearch& s, SearchBank& bank, u32 message_count) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < message_count; ++i)
        count += poll_first(bank, i);
    if (!count)
        return true;
    auto* publication = static_cast<SearchPollPublication*>(
        VirtualAlloc(nullptr, usize{count} * (sizeof(SearchPollPublication) + sizeof(MessagePollPageInput)),
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!publication)
        return false;
    static_assert(sizeof(SearchPollPublication) % alignof(MessagePollPageInput) == 0);
    auto* inputs = reinterpret_cast<MessagePollPageInput*>(publication + count);
    u32 at = 0;
    for (u32 i = 0; i < message_count; ++i) {
        if (!poll_first(bank, i))
            continue;
        const auto& message = bank.messages[i].message;
        auto& entry = publication[at];
        entry.ticket = poll_ticket(s, message.id);
        if (const auto* record = message_poll_store_find(bank.staged_polls, message.id)) {
            entry.poll = {message_poll_store_string(bank.staged_polls, record->question),
                          {entry.answers, record->answer_count},
                          {},
                          record->expiry_ms,
                          record->fields,
                          record->layout_type,
                          record->allow_multiselect,
                          record->expiry_null,
                          record->results_known,
                          record->is_finalized,
                          true};
            for (u32 j = 0; j < record->answer_count; ++j) {
                const auto& answer = record->answers[j];
                entry.answers[j] = {
                    answer.id, message_poll_store_string(bank.staged_polls, answer.text), answer.emoji_id,
                    message_poll_store_string(bank.staged_polls, answer.emoji_name), answer.emoji_animated};
                entry.counts[j] = {answer.id, answer.count, answer.me_voted};
            }
            if (record->fields & gateway_message_poll_field_results)
                entry.poll.answer_counts = {entry.counts, record->answer_count};
        }
        entry.input = {message.id, message.channel_id, &entry.poll, 0, &entry.ticket};
        inputs[at++] = entry.input;
    }
    const auto result =
        message_poll_store_apply_page(&g_app.account_ui->message_poll_store, {inputs, count});
    VirtualFree(publication, 0, MEM_RELEASE);
    return result.error == MessagePollStoreError::none;
}
MessageSearchRichScratch decode_scratch(SearchDecodeScratch& s) noexcept {
    return {{s.mention_roles, kGatewayMessageMentionRoleMaximumIds},
            {s.member_roles, kGatewayMessageMemberRoleMaximumIds},
            {s.mention_users, kGatewayMessageMentionUserMaximum},
            {{s.reactions, kGatewayMessageReactionMaximum},
             {s.attachments[0], kGatewayMessageAttachmentMaximum},
             {s.embeds[0], kGatewayMessageEmbedMaximum},
             {s.fields[0], kGatewayMessageEmbedFieldMaximum},
             {s.stickers[0], kGatewayMessageStickerMaximum},
             {s.components[0], kGatewayMessageComponentMaximum},
             {{s.attachments[1], kGatewayMessageAttachmentMaximum},
              {s.embeds[1], kGatewayMessageEmbedMaximum},
              {s.fields[1], kGatewayMessageEmbedFieldMaximum},
              {s.stickers[1], kGatewayMessageStickerMaximum},
              {s.components[1], kGatewayMessageComponentMaximum},
              {s.snapshot_users, kGatewayMessageMentionUserMaximum}},
             {{s.poll_answers, kGatewayMessagePollMaximumAnswers},
              {s.poll_counts, kGatewayMessagePollMaximumAnswers}}}};
}
bool available() noexcept {
    if (!g_app.account_ui || g_app.login.visible || application_friends_active())
        return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                           g_app.account_ui->active_channel_id);
    return channel && channel_is_message_capable(channel->type);
}
bool front() noexcept {
    return available() && !application_settings_active() && !g_app.text_form && !g_app.popup_scene.active &&
           !(g_app.profile && g_app.renderer.profile_scene && g_app.renderer.profile_scene->active) &&
           !g_app.image_viewer.scene.active && !g_app.reaction_viewer.active &&
           !application_downloads_active();
}
// The open thread browser is the focused pane: the field searches its thread
// names (results replace the browser's list) until the user widens this
// browsing session to the whole server.
bool thread_scope(const ApplicationSearch& s, ApplicationThreadBrowserSearchScope* out) noexcept {
    ApplicationThreadBrowserSearchScope scope{};
    if (!application_thread_browser_search_scope(&scope) || scope.session == s.widened_thread_session)
        return false;
    if (out)
        *out = scope;
    return true;
}

bool resolve(void* context, u32 index, UiSearchRow* out) noexcept {
    const auto& s = *static_cast<ApplicationSearch*>(context);
    if (!out || !s.bank || index >= s.bank->page.message_count)
        return false;
    const auto& item = s.bank->messages[index];
    if (row_unavailable(s, index)) {
        const bool access_check = row_access_check(s, index);
        *out = {item.message.channel_id,
                item.message.id,
                {},
                {},
                access_check
                    ? utf8_literal("Checking access to this message…")
                    : utf8_literal("You no longer have access to this message."),
                {},
                item.hit,
                false};
        out->unavailable = true;
        out->access_check = access_check;
        return true;
    }
    bool first = false;
    u32 group = 0;
    for (u32 i = 0; i < s.bank->page.group_count; ++i) {
        first |= s.bank->groups[i].first_message == index;
        if (index >= s.bank->groups[i].first_message)
            group = i;
    }
    auto body = item.card_excerpt.bytes ? item.card_excerpt : item.message.content;
    body.bytes = utf8_truncated_length(body, 1024);
    *out = {item.message.channel_id,
            item.message.id,
            s.bank->author_bytes[index] ? Utf8View{s.bank->author_names[index], s.bank->author_bytes[index]}
            : item.message.author_name.bytes ? item.message.author_name
                                             : utf8_literal("Unknown user"),
            {s.bank->channels[group], s.bank->channel_bytes[group]},
            body.bytes ? body : utf8_literal("Message has no text"),
            {s.bank->details[index], s.bank->detail_bytes[index]},
            item.hit,
            first,
            s.bank->body_heights[index],
            item.message.author_id,
            {s.bank->avatars[index], s.bank->avatar_bytes[index]}};
    return true;
}
bool measure_rows(ApplicationSearch& s) noexcept {
    if (!s.bank || s.source.rich_timeline || !s.panel || !ui_rect_has_area(s.scene.body))
        return false;
    auto system = renderer_text_system(&g_app.renderer, UiTextFace::normal);
    if (!system.native_text || !system.native_format)
        return false;
    auto& bank = *s.bank;
    const float width = ui_search_body_text_width(s.scene);
    if (bank.measured_width == width && bank.measured_em == system.em_size &&
        bank.measured_tracking == system.tracking && bank.measured_native_text == system.native_text &&
        bank.measured_native_format == system.native_format && bank.measured_dpi == g_app.renderer.dpi)
        return false;
    wchar_t scratch[1024];
    for (u32 i = 0; i < bank.page.message_count; ++i) {
        UiSearchRow row{};
        if (!resolve(&s, i, &row))
            return false;
        const auto measured = text_measure_utf8(&system, row.body, {}, width, 464.f, {scratch, 1024},
                                                TextWrapping::wrap);
        bank.body_heights[i] = measured.ok() ? maximum(24.f, minimum(464.f, measured.height + 2.f)) : 48.f;
    }
    bank.measured_width = width;
    bank.measured_em = system.em_size;
    bank.measured_tracking = system.tracking;
    bank.measured_native_text = system.native_text;
    bank.measured_native_format = system.native_format;
    bank.measured_dpi = g_app.renderer.dpi;
    ++s.source.revision;
    return true;
}
void cancel_submitted(ApplicationSearch& s) noexcept {
    if (s.submitted.request_id && !s.submitted_cancelled && g_app.account.instance &&
        g_app.account.api.message_search) {
        g_app.account.api.message_search(g_app.account.instance, AccountMessageSearchOperation::cancel,
                                         nullptr, s.submitted.request_id);
        s.submitted_cancelled = true;
    }
    // The shared worker queue keeps this request ID until its completion is consumed.
}
void focus(ApplicationSearch& s, bool value) noexcept {
    if (!value)
        member_query_withdraw(s, true);
    if (value)
        application_pinned_messages_close();
    if (s.focused != value)
        application_composer_text_input_suspend();
    s.focused = value;
    s.edit.focused = value;
    s.caret_visible = value;
    s.blink_at = value ? deadline_after(clock_monotonic_milliseconds(), kDefaultCaretBlinkMilliseconds) : 0;
    if (value) {
        g_app.message_composer.focused = false;
        g_app.context_focus = &s.scene.widgets;
        SetFocus(g_app.window);
        ui_widget_manager_focus(&s.scene.widgets, s.scene.edit);
    } else {
        const auto* rich = ui_search_scene_message_scene(s.scene);
        if (g_app.context_focus == &s.scene.widgets || (rich && g_app.context_focus == &rich->widgets))
            g_app.context_focus = nullptr;
    }
}
// Ends any press, drag or capture the panel holds.
void release_pointer(ApplicationSearch& s) noexcept {
    s.dragging = s.captured = s.scrollbar_dragging = false;
    reset_value(&s.pressed);
    ui_widget_manager_capture(&s.scene.widgets, ui_widget_invalid_handle());
    application_release_capture();
}
void focus_widget(ApplicationSearch& s, u32 index) noexcept {
    ui_widget_manager_focus(&s.scene.widgets, {static_cast<u16>(index), s.scene.widgets.generation});
}
// Focuses the first widget with this action (and source index, unless ~0u).
bool focus_target(ApplicationSearch& s, UiSearchAction action, u32 source_index) noexcept {
    for (u32 i = 1; i < s.scene.widgets.count; ++i)
        if (s.scene.targets[i].action == action &&
            (source_index == ~0u || s.scene.targets[i].source_index == source_index)) {
            focus_widget(s, i);
            return true;
        }
    return false;
}
bool repaint() noexcept {
    if (!application_search_synchronize() || !refresh_retained_projection())
        return false;
    application_composer_text_input_notify_layout();
    application_composer_text_input_synchronize();
    return true;
}
// Closes the open field, repaints and moves keyboard focus into the menu a toggle just opened.
bool menu_focus(ApplicationSearch& s, const bool& opened, UiSearchAction first) noexcept {
    focus(s, false);
    if (!repaint())
        return false;
    g_app.context_focus = &s.scene.widgets;
    if (opened)
        focus_target(s, first);
    application_request_frame();
    return true;
}
UiScrollbarGeometry body_scrollbar(const ApplicationSearch& s) noexcept {
    return ui_scrollbar_geometry(s.scene.body, s.scene.scroll_dips,
                                 ui_rect_height(s.scene.body) + ui_search_scene_scroll_maximum(s.scene));
}
bool selected_scope(MessageSearchScope* kind, u64* id) noexcept {
    if (!g_app.account_ui)
        return false;
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, g_app.account_ui->active_channel_id);
    if (!channel || !channel->occupied || !channel_is_message_capable(channel->type) ||
        (state.selected_channel_id && state.selected_channel_id != channel->id))
        return false;
    if (channel_is_private(channel->type)) {
        if (channel->guild_id || state.selected_guild_id)
            return false;
        *kind = MessageSearchScope::channel;
        *id = channel->id;
    } else {
        const auto* guild = chat_state_store_find_guild(state, channel->guild_id);
        if (!channel->guild_id || state.selected_guild_id != channel->guild_id || !guild ||
            !guild->occupied || guild->unavailable)
            return false;
        *kind = MessageSearchScope::guild;
        *id = channel->guild_id;
    }
    return *id != 0;
}
bool context_current(const ApplicationSearch& s) noexcept {
    MessageSearchScope kind{};
    u64 id = 0;
    return g_app.account_ui && s.account_scope && s.profile_id &&
           s.account_scope == g_app.plugin_account_scope_serial &&
           s.profile_id == g_app.account.status.profile_id &&
           s.account_id == g_app.account_ui->chat_state.self_user_id && selected_scope(&kind, &id) &&
           kind == s.scope && id == s.scope_id;
}
bool source_current(void* opaque) noexcept {
    return opaque && context_current(*static_cast<const ApplicationSearch*>(opaque));
}
void retire_actions(ApplicationSearch& s) noexcept {
    ++s.action_epoch;
    if (!s.action_epoch)
        ++s.action_epoch;
    reset_value(&s.validation);
    application_search_content_release();
    application_search_navigation_synchronize();
}

} // namespace search_detail
} // namespace maicord::app_detail
