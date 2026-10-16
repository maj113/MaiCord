#include "application_click_count.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "maicord/ui/timestamp_format.hpp"
#include "application_search.hpp"
#include "application_call.hpp"
#include "application_cursor.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_settings.hpp"
#include "application_friends.hpp"
#include "application_retained_projection.hpp"
#include "application_pinned_messages.hpp"
#include "application_downloads.hpp"
#include "application_image_viewer.hpp"
#include "application_video.hpp"
#include "application_search_content.hpp"
#include "application_search_navigation.hpp"
#include "application_thread_browser.hpp"
#include "application_poll.hpp"
#include "application_channel_authority.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/model/message_search.hpp"
#include "maicord/model/message_poll_store.hpp"
#include "maicord/ui/search_query.hpp"
#include "maicord/ui/search_filter_context.hpp"
#include "maicord/model/friend_search.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/app/search_recent.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/utf.hpp"

#include <limits.h>

namespace maicord::app_detail {
namespace {
constexpr u32 search_bank_text_limit = kRestClientMaximumResponseBytes;
constexpr u32 search_user_limit =
    kMessageSearchMaximumMessages * (2 * kGatewayMessageMentionUserMaximum + 3);
// Two fixed-address banks with independently committed prefixes. Switching a
// model text bank never commits the unused gap between the two prefixes.
struct SearchTextBanks {
    u8* data{};
    u32 committed[2]{};
};
bool text_reserve(SearchTextBanks& storage, u32 bank, u32 bytes) noexcept {
    if (bank > 1 || bytes > search_bank_text_limit)
        return false;
    if (!storage.data)
        storage.data = static_cast<u8*>(
            VirtualAlloc(nullptr, 2ull * search_bank_text_limit, MEM_RESERVE, PAGE_READWRITE));
    if (!storage.data)
        return false;
    const u32 rounded = (bytes + 4095u) & ~4095u;
    if (rounded > storage.committed[bank]) {
        if (!VirtualAlloc(storage.data + bank * search_bank_text_limit + storage.committed[bank],
                          rounded - storage.committed[bank], MEM_COMMIT, PAGE_READWRITE))
            return false;
        storage.committed[bank] = rounded;
    }
    return true;
}
struct SearchTextOwner {
    MessageTextPresentation presentation;
    MessageTextCommand commands[kMaximumMessageTextCommands];
    char utf8[kMaximumMessageTextUtf8Bytes];
    TextStyleSpan styles[kMaximumMessageTextStyleSpans];
    MessageMarkdownSemantic semantics[kMaximumMessageTextSemantics];
    MessageTextMeasurement measurements[kMaximumMessageTextMeasurements];
    MessageTextSpoilerReveal reveals[kMaximumMessageTextSpoilerReveals];
    u32 byte_styles[kMessageTextByteStyleScratchCapacity];
    u64 input_signature;
};
struct SearchBank {
    MessageSearchPage page;
    MessageSearchGroup groups[kMessageSearchMaximumGroups];
    MessageSearchMessage messages[kMessageSearchMaximumMessages];
    char channels[kMessageSearchMaximumGroups][128];
    u8 channel_bytes[kMessageSearchMaximumGroups];
    char details[kMessageSearchMaximumMessages][32];
    u8 detail_bytes[kMessageSearchMaximumMessages];
    MessageStore store;
    MessageRecord records[kMessageSearchMaximumMessages];
    u32 order[kMessageSearchMaximumMessages];
    MessageHandle result_handles[kMessageSearchMaximumMessages];
    MessageRichStore rich;
    MessagePollStore staged_polls;
    MessageScenePollBinding polls;
    u8 poll_fields[kMessageSearchMaximumMessages]; // 0 omitted, 1 null, 2 present
    StableStorage content_storage, rich_storage, user_records, user_order;
    SearchTextBanks rich_text, user_names;
    u16 rich_order[kMessageSearchMaximumMessages];
    UserStore users;
    GuildMemberStore members;
    GuildMemberRecord member_records[kMessageSearchMaximumMessages];
    u32 member_order[kMessageSearchMaximumMessages];
    UiVariableList list;
    u32 heights[kMessageSearchMaximumMessages];
    u64 fenwick[kMessageSearchMaximumMessages];
    MessageTimeline timeline;
    char author_names[kMessageSearchMaximumMessages][128];
    char avatars[kMessageSearchMaximumMessages][34];
    u8 author_bytes[kMessageSearchMaximumMessages],
        avatar_bytes[kMessageSearchMaximumMessages];
    u16 channel_generations[kMessageSearchMaximumMessages];
    bool unavailable[kMessageSearchMaximumMessages];
    bool access_checks[kMessageSearchMaximumMessages];
    u64 action_epoch, account_scope, profile_id, self_id;
    const void* account_instance;
    const AccountUiRuntime* account_ui;
    u32 publication;
    bool initialized;
    float body_heights[kMessageSearchMaximumMessages];
    float measured_width, measured_em, measured_tracking;
    const void* measured_native_text;
    u32 measured_dpi, measured_native_format;
};
struct SearchFilterStamp {
    const void *actor{}, *ui{}, *relationships{}, *private_channels{};
    u64 account{}, profile{}, epoch{}, channel_epoch{}, active{}, document{};
    u32 text{}, anchor{}, caret{}, guilds{}, channels{}, permissions{}, names{}, users{}, members{},
        relationship_revision{}, private_revision{}, created{}, updated{}, deleted{}, compacted{},
        publication{};
    bool operator==(const SearchFilterStamp&) const noexcept = default;
};
struct SearchFilterChoices {
    UiSearchFilterChoiceView rows[kUiSearchFilterChoiceMaximum];
    char labels[kUiSearchFilterChoiceMaximum][128], details[kUiSearchFilterChoiceMaximum][64],
        values[kUiSearchFilterChoiceMaximum][24], avatars[kUiSearchFilterChoiceMaximum][kUserAvatarHashMaximumBytes];
    u8 ranks[kUiSearchFilterChoiceMaximum];
    UiSearchFilterContext filter;
    UiSearchFilterChoiceSource source;
    SearchFilterStamp stamp;
    u64 scans, copies, rebuilds;
    bool initialized, capacity_limited;
};
static_assert(sizeof(SearchFilterChoices) <= 4096);
} // namespace
struct ApplicationSearch {
    MessageComposer edit;
    char query_text[kMessageSearchContentBytes];
    MessageSearchController controller;
    MessageSearchToken submitted;
    MessagePollResultTicket* submitted_poll_tickets;
    u32 submitted_poll_count;
    u64 submitted_poll_gateway_serial;
    SearchBank* bank;
    SearchBank* candidate;
    SearchFilterChoices* choices;
    SearchTextOwner* text_owner;
    UiSearchScene scene;
    UiSearchSource source;
    UiSearchContent content;
    UiSearchTarget pressed;
    u64 scope_id, blink_at, account_id, account_scope, profile_id, submitted_account_scope,
        submitted_profile_id, action_epoch;
    u32 held_key, pressed_revision, edit_revision, committed_edit_revision;
    MessageSearchScope scope;
    MessageSearchSort sort;
    Utf8View validation;
    char scope_heading[160];
    u16 heading_bytes;
    float thumb_grab;
    bool panel, focused, caret_visible, dragging, filters, suggestions, sort_menu, admission_busy, captured,
        scrollbar_dragging, submitted_cancelled, member_query_active;
    // Selected tab, the tab of the published bank, and per-tab totals from the tabs endpoint.
    MessageSearchTab tab, result_tab;
    u8 tab_known;
    bool tabs_unavailable;
    u64 tab_totals[kMessageSearchTabCount];
    SearchRecentList* recent;
    u64 recent_profile;
    Utf8View recent_views[kSearchRecentCapacity];
    u32 recent_published;
    // While the thread browser is open the field searches its thread names.
    // The browser session the user widened to the whole server, if any.
    u64 widened_thread_session;
};
static_assert(sizeof(ApplicationSearch) <= 8192);
namespace {
u32 search_publication_counter{};
u32 filter_publication_counter{};
void member_query_withdraw(ApplicationSearch& s, bool invalidate_choices = false) noexcept {
    if (!s.member_query_active)
        return;
    s.member_query_active = false;
    if (invalidate_choices && s.choices)
        s.choices->initialized = false;
    application_member_query_cancel();
}
void choices_release(ApplicationSearch& s) noexcept {
    member_query_withdraw(s);
    s.content.filter_choices = nullptr;
    if (s.choices)
        VirtualFree(s.choices, 0, MEM_RELEASE);
    s.choices = nullptr;
}
void clear_poll_tickets(ApplicationSearch& s) noexcept {
    if (s.submitted_poll_tickets)
        VirtualFree(s.submitted_poll_tickets, 0, MEM_RELEASE);
    s.submitted_poll_tickets = nullptr;
    s.submitted_poll_count = 0;
    s.submitted_poll_gateway_serial = 0;
}
bool capture_poll_tickets(ApplicationSearch& s) noexcept {
    clear_poll_tickets(s);
    if (!g_app.account_ui)
        return false;
    const auto& store = g_app.account_ui->message_poll_store;
    s.submitted_poll_gateway_serial = g_app.account_ui->poll_gateway_serial;
    if (!store.count)
        return true;
    if (!store.records || store.count > store.capacity || store.count > 65535)
        return false;
    s.submitted_poll_tickets = static_cast<MessagePollResultTicket*>(
        VirtualAlloc(nullptr, usize{store.count} * sizeof(MessagePollResultTicket),
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!s.submitted_poll_tickets)
        return false;
    s.submitted_poll_count = store.count;
    for (u32 i = 0; i < store.count; ++i)
        s.submitted_poll_tickets[i] = {store.records[i].message_id, s.submitted_poll_gateway_serial,
                                       store.records[i].content_revision};
    return true;
}
MessagePollResultTicket poll_ticket(const ApplicationSearch& s, u64 id) noexcept {
    for (u32 i = 0; i < s.submitted_poll_count; ++i)
        if (s.submitted_poll_tickets[i].message_id == id)
            return s.submitted_poll_tickets[i];
    return {id, s.submitted_poll_gateway_serial, 0};
}
void bank_release(SearchBank* bank) noexcept {
    if (!bank)
        return;
    message_rich_store_release(&bank->rich);
    message_poll_store_release(&bank->staged_polls);
    stable_storage_release(&bank->content_storage);
    stable_storage_release(&bank->user_records);
    stable_storage_release(&bank->user_order);
    if (bank->rich_text.data)
        VirtualFree(bank->rich_text.data, 0, MEM_RELEASE);
    if (bank->user_names.data)
        VirtualFree(bank->user_names.data, 0, MEM_RELEASE);
    VirtualFree(bank, 0, MEM_RELEASE);
}
bool bank_rich_initialize(SearchBank& b) noexcept {
    return message_rich_store_initialize_owned_sparse(&b.rich, &b.rich_storage,
                                                      kMessageSearchMaximumMessages,
                                                      {b.rich_order, kMessageSearchMaximumMessages},
                                                      {b.rich_text.data, 2 * search_bank_text_limit});
}
bool bank_initialize(SearchBank& b) noexcept {
    constexpr u32 poll_text_limit =
        kMessageSearchMaximumMessages *
        (kGatewayMessagePollQuestionMaximumBytes +
         kGatewayMessagePollMaximumAnswers *
             (kGatewayMessagePollAnswerMaximumBytes + kGatewayMessagePollEmojiMaximumBytes));
    if (!message_poll_store_initialize_owned(&b.staged_polls, kMessageSearchMaximumMessages,
                                             poll_text_limit) ||
        !stable_storage_initialize(&b.content_storage, search_bank_text_limit, 0, 1, 1) ||
        !text_reserve(b.rich_text, 0, 1) || !text_reserve(b.rich_text, 1, 1) ||
        !text_reserve(b.user_names, 0, 1) ||
        !stable_storage_initialize(&b.user_records, search_user_limit, 16, sizeof(UserRecord),
                                   alignof(UserRecord), true) ||
        !stable_storage_initialize(&b.user_order, search_user_limit, 16, sizeof(u32), alignof(u32)) ||
        !message_store_initialize(&b.store, {b.records, kMessageSearchMaximumMessages},
                                  {b.order, kMessageSearchMaximumMessages},
                                  {stable_storage_data<u8>(b.content_storage), search_bank_text_limit}) ||
        !bank_rich_initialize(b) ||
        !user_store_initialize(&b.users,
                                       {stable_storage_data<UserRecord>(b.user_records), 16},
                                       {stable_storage_data<u32>(b.user_order), 16},
                                       {b.user_names.data, 2 * search_bank_text_limit}) ||
        !guild_member_store_initialize(&b.members,
                                               {b.member_records, kMessageSearchMaximumMessages},
                                               {b.member_order, kMessageSearchMaximumMessages}) ||
        !ui_variable_list_initialize(&b.list, {b.heights, kMessageSearchMaximumMessages},
                                     {b.fenwick, kMessageSearchMaximumMessages}, 0,
                                     ui_extent_units(80)) ||
        !message_timeline_initialize(&b.timeline, &b.store, &b.list, ui_extent_units(80),
                                     ui_extent_units(480), false) ||
        !message_timeline_bind_rich_store(&b.timeline, &b.rich))
        return false;
    b.initialized = true;
    return true;
}
bool bank_user(SearchBank& b, u64 id, Utf8View name, Utf8View avatar = {}) noexcept {
    if (!id || !name.bytes)
        return true;
    if (!user_store_find(b.users, id) && b.users.count == b.users.capacity) {
        const u32 old = b.users.capacity, next = minimum(search_user_limit, maximum(old + 1, old * 2));
        if (!stable_storage_grow(&b.user_records, next) || !stable_storage_grow(&b.user_order, next))
            return false;
        b.users.capacity = minimum(b.user_records.capacity, b.user_order.capacity);
        for (u32 i = old; i < b.users.capacity; ++i)
            b.users.order[i] = ~u32{0};
    }
    // Accepted identities are pinned; no replacement may evict a previous
    // card's labels. Commit both possible compaction prefixes before mutation.
    const u32 needed = b.users.name_used + name.bytes;
    if (!text_reserve(b.user_names, 0, needed) || !text_reserve(b.user_names, 1, needed))
        return false;
    const auto result = user_store_upsert(&b.users, id, name);
    return result.ok() && !result.evicted && user_store_pin(&b.users, id) &&
           (!avatar.bytes || user_store_set_avatar(&b.users, id, avatar));
}
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
        if (!text_reserve(b.rich_text, 0, needed) || !text_reserve(b.rich_text, 1, needed))
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
    for (u32 i = 0; i < m.mention_users.count; ++i) {
        const auto& u = m.mention_users[i];
        if (!bank_user(b, u.id, u.name))
            return false;
    }
    for (u32 i = 0; i < m.snapshot.mention_users.count; ++i) {
        const auto& u = m.snapshot.mention_users[i];
        if (!bank_user(b, u.id, u.name))
            return false;
    }
    const u64 guild =
        item.guild_known
            ? item.guild_id
            : (chat_state_store_find_channel(g_app.account_ui->chat_state, m.channel_id)
                   ? chat_state_store_find_channel(g_app.account_ui->chat_state, m.channel_id)
                         ->guild_id
                   : 0);
    if (guild && (m.fields & gateway_message_field_member)) {
        const auto member = guild_member_store_upsert(&b.members, g_app.account_ui->chat_state,
                                                              guild, m.author_id, m.member_nick, m.member_roles,
                                                              true, GuildMemberStatus::unknown);
        if (!member.ok() || member.evicted)
            return false;
    }
    return true;
}
struct SearchDecodeScratch {
    GatewayMessagePollAnswer poll_answers[kGatewayMessagePollMaximumAnswers];
    GatewayMessagePollAnswerCount poll_counts[kGatewayMessagePollMaximumAnswers];
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds],
        member_roles[kGatewayMessageMemberRoleMaximumIds];
    GatewayMessageNamedUser mention_users[kGatewayMessageMentionUserMaximum],
        snapshot_users[kGatewayMessageMentionUserMaximum];
    GatewayMessageReaction reactions[kGatewayMessageReactionMaximum];
    GatewayMessageAttachment attachments[2][kGatewayMessageAttachmentMaximum];
    GatewayMessageEmbed embeds[2][kGatewayMessageEmbedMaximum];
    GatewayMessageEmbedField fields[2][kGatewayMessageEmbedFieldMaximum];
    GatewayMessageSticker stickers[2][kGatewayMessageStickerMaximum];
    GatewayMessageComponent components[2][kGatewayMessageComponentMaximum];
};
struct SearchPollPublication {
    MessagePollPageInput input;
    GatewayMessagePoll poll;
    MessagePollResultTicket ticket;
    GatewayMessagePollAnswer answers[kGatewayMessagePollMaximumAnswers];
    GatewayMessagePollAnswerCount counts[kGatewayMessagePollMaximumAnswers];
};
bool publish_polls(ApplicationSearch& s, SearchBank& bank, u32 message_count) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < message_count; ++i) {
        const auto& message = bank.messages[i].message;
        if (!bank.poll_fields[i])
            continue;
        bool duplicate = false;
        for (u32 j = 0; j < i; ++j)
            duplicate |= bank.messages[j].message.id == message.id;
        if (!duplicate)
            ++count;
    }
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
        const auto& message = bank.messages[i].message;
        if (!bank.poll_fields[i])
            continue;
        bool duplicate = false;
        for (u32 j = 0; j < i; ++j)
            duplicate |= bank.messages[j].message.id == message.id;
        if (duplicate)
            continue;
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
bool row_unavailable(const ApplicationSearch&, u32) noexcept;
bool row_access_check(const ApplicationSearch&, u32) noexcept;
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
    u32 bytes = minimum(body.bytes, 1024u);
    while (bytes < body.bytes && bytes && static_cast<u8>(body.data[bytes]) >= 0x80 &&
           static_cast<u8>(body.data[bytes]) < 0xc0)
        --bytes;
    body.bytes = bytes;
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
        const auto measured = text_measure_utf8_styled(&system, row.body, {}, width, 464.f, {scratch, 1024},
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
bool repaint() noexcept {
    if (!application_search_synchronize() || !refresh_retained_projection())
        return false;
    application_composer_text_input_notify_layout();
    application_composer_text_input_synchronize();
    return true;
}
bool selected_scope(MessageSearchScope* kind, u64* id) noexcept {
    if (!g_app.account_ui || !kind || !id)
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
    s.validation = {};
    application_search_content_release();
    application_search_navigation_synchronize();
}
void current_scope(ApplicationSearch& s) noexcept {
    MessageSearchScope kind{};
    u64 id = 0;
    selected_scope(&kind, &id);
    const u64 user = g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0;
    const u64 profile = g_app.account.status.profile_id, epoch = g_app.plugin_account_scope_serial;
    if (s.scope == kind && s.scope_id == id && s.account_id == user && s.profile_id == profile &&
        s.account_scope == epoch)
        return;
    if (s.panel || s.focused || s.captured)
        g_app.context_focus = nullptr;
    application_composer_text_input_suspend();
    cancel_submitted(s);
    message_search_cancel(&s.controller);
    g_app.renderer.search_scene = nullptr;
    renderer_unbind_search_message_text_presentation(&g_app.renderer);
    bank_release(s.bank);
    bank_release(s.candidate);
    choices_release(s);
    s.bank = s.candidate = nullptr;
    s.source.rich_timeline = nullptr;
    s.source.polls = nullptr;
    s.source.rich_order = {};
    retire_actions(s);
    s.panel = !id && s.panel;
    focus(s, false);
    s.source.count = 0;
    ++s.source.revision;
    s.pressed = {};
    s.dragging = s.captured = s.scrollbar_dragging = false;
    ui_widget_manager_capture(&s.scene.widgets, ui_widget_invalid_handle());
    if (GetCapture() == g_app.window)
        ReleaseCapture();
    message_composer_clear(&s.edit);
    message_composer_refresh_borrowed(&s.edit, 0);
    s.scope = kind;
    s.scope_id = id;
    s.account_id = user;
    s.profile_id = profile;
    s.account_scope = epoch;
    s.admission_busy = false;
    s.filters = false;
    s.suggestions = s.sort_menu = false;
    s.tab = s.result_tab = MessageSearchTab::messages;
    s.tab_known = 0;
    s.validation = id && profile && epoch
                       ? Utf8View{}
                       : utf8_literal("Select a channel to search.");
    s.scene.scroll_dips = 0;
}
bool heading(ApplicationSearch& s) noexcept {
    Utf8View label = utf8_literal("Search");
    Utf8View prefix = utf8_literal("Search ");
    ApplicationThreadBrowserSearchScope threads{};
    if (thread_scope(s, &threads)) {
        prefix = threads.forum ? utf8_literal("Search posts in ") : utf8_literal("Search threads in ");
        label = threads.parent_name;
    } else if (g_app.account_ui && s.scope_id) {
        const auto& state = g_app.account_ui->chat_state;
        if (s.scope == MessageSearchScope::guild) {
            const auto* guild = chat_state_store_find_guild(state, s.scope_id);
            if (guild)
                label = chat_state_store_guild_name(state, *guild);
        } else {
            const auto* channel = chat_state_store_find_channel(state, s.scope_id);
            if (channel)
                label = chat_state_store_channel_name(state, *channel);
        }
    }
    const u32 bytes = utf8_truncated_length(label, static_cast<u32>(sizeof(s.scope_heading) - prefix.bytes));
    const u32 total = prefix.bytes + bytes;
    if (s.heading_bytes == total && bytes_equal(s.scope_heading, prefix.data, prefix.bytes) &&
        bytes_equal(s.scope_heading + prefix.bytes, label.data, bytes))
        return false;
    copy_bytes(s.scope_heading, prefix.data, prefix.bytes);
    copy_bytes(s.scope_heading + prefix.bytes, label.data, bytes);
    s.heading_bytes = static_cast<u16>(total);
    return true;
}
SearchFilterStamp choices_stamp(const ApplicationSearch& s) noexcept {
    SearchFilterStamp v{};
    if (!g_app.account_ui)
        return v;
    const auto& ui = *g_app.account_ui;
    const auto& state = ui.chat_state;
    const auto* relationships = application_friends_relationships();
    v.actor = g_app.account.instance;
    v.ui = &ui;
    v.relationships = relationships;
    v.private_channels = state.private_channels;
    v.account = g_app.plugin_account_scope_serial;
    v.profile = g_app.account.status.profile_id;
    v.epoch = s.action_epoch;
    v.channel_epoch = g_app.load_trace.channel_epoch;
    v.active = ui.active_channel_id;
    v.document = s.edit.document_identity;
    v.text = s.edit.text_revision;
    v.anchor = s.edit.anchor;
    v.caret = s.edit.caret;
    v.guilds = state.guild_mutations;
    v.channels = state.channel_mutations;
    v.permissions = state.permission_mutations;
    v.names = state.compactions;
    v.users = ui.user_store.revision;
    v.members = ui.guild_member_store.revision;
    v.relationship_revision = relationships ? relationships->revision : 0;
    v.private_revision = state.private_channels ? state.private_channels->revision : 0;
    const auto& store = g_app.message_view.store;
    v.created = store.created;
    v.updated = store.updated;
    v.deleted = store.deleted;
    v.compacted = store.compactions;
    v.publication = s.bank ? s.bank->publication : 0;
    return v;
}
bool choice_channel(const ApplicationSearch&, const ChannelRecord&) noexcept;
bool choices_current(void* opaque) noexcept {
    const auto* s = static_cast<const ApplicationSearch*>(opaque);
    if (!s || !s->choices || !s->focused || !s->suggestions || !front() || !context_current(*s) ||
        application_composer_text_input_composing())
        return false;
    const auto stamp = choices_stamp(*s);
    const auto* active = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                          g_app.account_ui->active_channel_id);
    return active && choice_channel(*s, *active) && stamp == s->choices->stamp;
}
bool choice_channel(const ApplicationSearch& s, const ChannelRecord& c) noexcept {
    const auto& state = g_app.account_ui->chat_state;
    constexpr u32 required = channel_action_view | channel_action_read_history;
    if (!application_channel_readable(g_app, c.id) || !c.occupied ||
        !channel_is_message_capable(c.type) || (c.flags & kChannelObfuscatedFlag))
        return false;
    if (s.scope == MessageSearchScope::channel)
        return c.id == s.scope_id;
    const auto* guild = chat_state_store_find_guild(state, c.guild_id);
    return c.guild_id == s.scope_id && guild && !guild->unavailable && c.permissions_resolved &&
           (c.actions & required) == required && chat_state_store_channel_visible(state, c) &&
           (!channel_is_thread(c.type) || chat_state_store_thread_accessible(state, c));
}
bool choice_user_allowed(u64 id) noexcept {
    const auto* relationships = application_friends_relationships();
    const auto* record = relationships ? relationship_store_find(*relationships, id) : nullptr;
    return id && (!record || (record->type != RelationshipType::blocked && !record->ignored));
}
Utf8View choice_copy(SearchFilterChoices& b, char* output, u32 capacity, Utf8View text) noexcept {
    if (!utf8_validate(text).valid)
        return {};
    const u32 bytes = utf8_truncated_length(text, capacity);
    if (bytes) {
        copy_bytes(output, text.data, bytes);
        ++b.copies;
    }
    return {output, bytes};
}
bool resolve_user(void*, Utf8View, u64*) noexcept;
bool resolve_channel(void*, Utf8View, u64*) noexcept;
bool choice_capacity(SearchFilterChoices& b, Utf8View value) noexcept {
    if (b.filter.other_value_count < 4 ||
        (b.filter.kind != UiSearchFilterKind::from && b.filter.kind != UiSearchFilterKind::mentions &&
         b.filter.kind != UiSearchFilterKind::in))
        return true;
    const auto& s = *static_cast<ApplicationSearch*>(b.source.context);
    const auto query = message_composer_text(s.edit);
    char isolated[kMessageSearchContentBytes];
    u32 used = 0;
    // Validate only the active identity domain. An unfinished unrelated filter
    // must not prevent inserting a value here; the full parser runs on submit.
    const auto key =
        Utf8View{query.data + b.filter.token_begin, b.filter.value_begin - b.filter.token_begin};
    for (u32 at = 0; at < query.bytes;) {
        while (at < query.bytes && query.data[at] == ' ')
            ++at;
        const u32 first = at;
        bool quoted = false;
        while (at < query.bytes) {
            const char c = query.data[at];
            if (c == '\\' && quoted && at + 1 < query.bytes) {
                at += 2;
                continue;
            }
            if (c == '"')
                quoted = !quoted;
            if (c == ' ' && !quoted)
                break;
            ++at;
        }
        if (first != b.filter.token_begin && at - first >= key.bytes &&
            bytes_equal(query.data + first, key.data, key.bytes)) {
            if (used + at - first + 1 > sizeof(isolated))
                return false;
            copy_bytes(isolated + used, query.data + first, at - first);
            used += at - first;
            isolated[used++] = ' ';
        }
    }
    const u32 last_token = used;
    if (used + key.bytes + value.bytes > sizeof(isolated))
        return false;
    copy_bytes(isolated + used, key.data, key.bytes);
    used += key.bytes;
    copy_bytes(isolated + used, value.data, value.bytes);
    used += value.bytes;
    const UiSearchQueryContext context{b.source.context, resolve_user, resolve_channel, nullptr};
    const auto result = ui_search_query_parse({isolated, used}, s.scope, s.scope_id, s.sort, &context);
    const bool allowed = result.ok() || result.error_offset != last_token;
    b.capacity_limited |= !allowed;
    return allowed;
}
void choice_add(SearchFilterChoices& b, u64 id, u8 rank, Utf8View label, Utf8View detail, Utf8View value,
                u32 semantic = 0, Utf8View avatar = {}) noexcept {
    if (!label.bytes || !value.bytes || value.bytes > sizeof(b.values[0]))
        return;
    if (!choice_capacity(b, value))
        return;
    for (u32 i = 0; i < b.source.rows.count; ++i)
        if (b.rows[i].id == id && b.rows[i].semantic == semantic)
            return;
    u32 position = 0;
    while (position < b.source.rows.count &&
           (b.ranks[position] < rank ||
            (b.ranks[position] == rank && (rank == 2 || rank == 3 || b.rows[position].id < id))))
        ++position;
    if (position >= kUiSearchFilterChoiceMaximum)
        return;
    const u32 count = minimum(b.source.rows.count + 1, kUiSearchFilterChoiceMaximum);
    for (u32 i = count - 1; i > position; --i) {
        b.rows[i] = b.rows[i - 1];
        b.ranks[i] = b.ranks[i - 1];
        copy_bytes(b.labels[i], b.labels[i - 1], sizeof(b.labels[i]));
        copy_bytes(b.details[i], b.details[i - 1], sizeof(b.details[i]));
        copy_bytes(b.values[i], b.values[i - 1], sizeof(b.values[i]));
        copy_bytes(b.avatars[i], b.avatars[i - 1], sizeof(b.avatars[i]));
        ++b.copies;
    }
    // An avatar hash is ASCII; one that does not fit is dropped, not cut.
    const Utf8View avatar_hash =
        avatar.bytes <= sizeof(b.avatars[0]) ? choice_copy(b, b.avatars[position], sizeof(b.avatars[0]), avatar)
                                             : Utf8View{};
    b.rows[position] = {id, semantic, choice_copy(b, b.labels[position], 128, label),
                        choice_copy(b, b.details[position], 64, detail),
                        choice_copy(b, b.values[position], 24, value), avatar_hash};
    b.ranks[position] = rank;
    b.source.rows = {b.rows, count};
    for (u32 i = 0; i < count; ++i) {
        b.rows[i].label.data = b.labels[i];
        b.rows[i].detail.data = b.details[i];
        b.rows[i].value.data = b.values[i];
        b.rows[i].avatar_hash.data = b.avatars[i];
    }
}
bool choice_match(Utf8View prefix, Utf8View name, Utf8View alias, Utf8View value) noexcept {
    const auto result = friend_search(prefix, name, alias, value);
    return result.ok() && result.matched;
}
void choice_user(ApplicationSearch& s, SearchFilterChoices& b, u64 id, u8 rank,
                 const UserStore& users, Utf8View nickname = {}) noexcept {
    ++b.scans;
    if (!choice_user_allowed(id))
        return;
    const auto* user = user_store_find(users, id);
    auto label = nickname.bytes ? nickname : user ? user_store_name(users, *user) : Utf8View{};
    const auto username = user ? user_store_username(users, *user) : Utf8View{};
    char digits[24];
    const auto bytes = decimal_write(digits, id);
    const bool self = id == s.account_id;
    if (self && !label.bytes)
        label = utf8_literal("You");
    auto prefix = b.filter.prefix();
    if (prefix.bytes && prefix.data[0] == '@')
        prefix = {prefix.data + 1, prefix.bytes - 1};
    const auto value = self ? utf8_literal("@me") : Utf8View{digits, bytes};
    // A server nickname, the profile's display name or the username matches,
    // as in the official client's people search.
    const auto display = user && nickname.bytes ? user_store_name(users, *user) : Utf8View{};
    if (choice_match(prefix, label, username, {digits, bytes}) ||
        (display.bytes && choice_match(prefix, display, {}, {})) ||
        (self && choice_match(prefix, utf8_literal("me"), {}, {})))
        choice_add(b, id, rank, label, self ? utf8_literal("You") : username, value, 0,
                   user ? user_store_avatar_hash(*user) : Utf8View{});
}
bool choices_synchronize(ApplicationSearch& s) noexcept {
    if (!s.focused || !s.suggestions || !front() || !context_current(s) ||
        application_composer_text_input_composing()) {
        member_query_withdraw(s, true);
        return false;
    }
    const auto stamp = choices_stamp(s);
    if (s.choices && s.choices->initialized && stamp == s.choices->stamp)
        return false;
    const auto filter =
        ui_search_filter_context(message_composer_text(s.edit), s.edit.anchor, s.edit.caret);
    if (filter.kind == UiSearchFilterKind::none) {
        member_query_withdraw(s);
        if (s.choices) {
            s.choices->filter = filter;
            s.choices->stamp = stamp;
        }
        return false;
    }
    if (!s.choices)
        s.choices = static_cast<SearchFilterChoices*>(
            VirtualAlloc(nullptr, sizeof(SearchFilterChoices), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!s.choices)
        return false;
    auto& b = *s.choices;
    auto matching_stamp = stamp;
    matching_stamp.anchor = b.stamp.anchor;
    matching_stamp.caret = b.stamp.caret;
    if (b.initialized && matching_stamp == b.stamp && b.filter.kind == filter.kind &&
        b.filter.token_begin == filter.token_begin && b.filter.token_end == filter.token_end &&
        b.filter.value_begin == filter.value_begin && b.filter.value_end == filter.value_end &&
        utf8_equal(b.filter.prefix(), filter.prefix())) {
        // Selection/caret changes still invalidate every captured row,
        // while identical matching inputs keep the existing choices.
        b.stamp = stamp;
        b.filter = filter;
        if (filter_publication_counter != ~u32{0})
            b.source.revision = ++filter_publication_counter;
        else {
            b.source.revision = ~u32{0};
            b.source.rows = {};
            b.source.selected = kUiSearchFilterNoSelection;
            b.source.hint = utf8_literal("Suggestions unavailable. Type a value.");
        }
        return true;
    }
    const bool preserve_selection =
        b.initialized && b.source.selected < b.source.rows.count && b.stamp.document == stamp.document &&
        b.stamp.text == stamp.text && b.stamp.anchor == stamp.anchor && b.stamp.caret == stamp.caret &&
        b.filter.kind == filter.kind && utf8_equal(b.filter.prefix(), filter.prefix());
    const u64 selected_id = preserve_selection ? b.rows[b.source.selected].id : 0;
    const u32 selected_semantic = preserve_selection ? b.rows[b.source.selected].semantic : 0;
    b.stamp = stamp;
    b.filter = filter;
    b.initialized = true;
    b.capacity_limited = false;
    ++b.rebuilds;
    b.source = {};
    b.source.context = &s;
    b.source.current = choices_current;
    b.source.kind = filter.kind;
    const auto kind = filter.kind;
    b.source.heading = kind == UiSearchFilterKind::in            ? utf8_literal("Channels")
                       : kind == UiSearchFilterKind::has         ? utf8_literal("Message contains")
                       : kind == UiSearchFilterKind::pinned      ? utf8_literal("Pinned messages")
                       : kind == UiSearchFilterKind::author_type ? utf8_literal("Author type")
                       : kind == UiSearchFilterKind::file_type   ? utf8_literal("File type")
                       : kind == UiSearchFilterKind::link_from   ? utf8_literal("Links from")
                       : kind == UiSearchFilterKind::file_name   ? utf8_literal("File name")
                                                                 : utf8_literal("People");
    b.source.hint =
        kind == UiSearchFilterKind::in ? utf8_literal("Channels")
        : kind == UiSearchFilterKind::from || kind == UiSearchFilterKind::mentions ? utf8_literal("People")
        : kind == UiSearchFilterKind::link_from ? utf8_literal("Type a website, like youtube.com.")
        : kind == UiSearchFilterKind::file_name ? utf8_literal("Type part of a file name.")
                                                : utf8_literal("Press Enter or Tab to insert.");
    bool requested_members = false;
    if (kind == UiSearchFilterKind::has || kind == UiSearchFilterKind::pinned ||
        kind == UiSearchFilterKind::author_type || kind == UiSearchFilterKind::file_type) {
        auto prefix = filter.prefix();
        // has: and authorType: accept "-" to exclude a value.
        const bool negated = prefix.bytes && prefix.data[0] == '-' &&
                             (kind == UiSearchFilterKind::has || kind == UiSearchFilterKind::author_type);
        if (negated)
            prefix = {prefix.data + 1, prefix.bytes - 1};
        if (kind == UiSearchFilterKind::file_type && prefix.bytes && prefix.data[0] == '.')
            prefix = {prefix.data + 1, prefix.bytes - 1};
        const auto fixed = ui_search_filter_fixed_choices(kind);
        for (u32 i = 0; i < fixed.count; ++i) {
            const auto& row = fixed[i];
            ++b.scans;
            if (!choice_match(prefix, row.label, {}, row.value))
                continue;
            char value[24];
            u32 bytes = 0;
            if (negated)
                value[bytes++] = '-';
            copy_bytes(value + bytes, row.value.data, row.value.bytes);
            bytes += row.value.bytes;
            choice_add(b, row.id, kind == UiSearchFilterKind::file_type ? 0 : static_cast<u8>(row.semantic),
                       row.label, negated ? utf8_literal("Exclude") : row.detail, {value, bytes},
                       row.semantic);
        }
    } else if (kind == UiSearchFilterKind::link_from || kind == UiSearchFilterKind::file_name) {
        // Free text: the hint explains the expected value.
    } else if (filter.kind == UiSearchFilterKind::in) {
        auto prefix = filter.prefix();
        if (prefix.bytes && prefix.data[0] == '#')
            prefix = {prefix.data + 1, prefix.bytes - 1};
        const auto& state = g_app.account_ui->chat_state;
        for (u32 i = 0; i < state.channel_count; ++i) {
            const auto& c = state.channels[state.channel_order[i]];
            ++b.scans;
            if (!choice_channel(s, c))
                continue;
            const auto label = chat_state_store_channel_name(state, c);
            char digits[24];
            const auto n = decimal_write(digits, c.id);
            if (choice_match(prefix, label, {}, {digits, n}))
                choice_add(b, c.id, c.id == g_app.account_ui->active_channel_id ? 0 : 1, label,
                           utf8_literal("Channel"), {digits, n});
        }
    } else if (const auto* active = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                                     g_app.account_ui->active_channel_id);
               active && choice_channel(s, *active)) {
        const auto& ui = *g_app.account_ui;
        choice_user(s, b, s.account_id, 0, ui.user_store);
        if (s.scope == MessageSearchScope::channel && ui.chat_state.private_channels) {
            const auto& private_store = *ui.chat_state.private_channels;
            const auto* record = private_channel_store_find(private_store, s.scope_id);
            if (record) {
                const auto recipients = private_channel_store_recipients(private_store, *record);
                for (u32 i = 0; i < recipients.count; ++i)
                    choice_user(s, b, recipients[i], 1, ui.user_store);
            }
        }
        const auto& store = g_app.message_view.store;
        // Suggestions use recent loaded authors, never an unbounded scan of
        // the conversation history on each edit.
        const u32 recent_first = store.count - minimum(store.count, 128u);
        for (u32 i = store.count; i > recent_first; --i) {
            const auto& message = store.records[store.order[i - 1]];
            const auto* c = chat_state_store_find_channel(ui.chat_state, message.channel_id);
            if (c && choice_channel(s, *c) && message.delivery == MessageDeliveryState::authoritative)
                choice_user(s, b, message.author_id, 2, ui.user_store);
        }
        if (s.bank)
            for (u32 i = 0; i < s.bank->page.message_count; ++i)
                if (!row_unavailable(s, i) && !row_access_check(s, i))
                    choice_user(s, b, s.bank->messages[i].message.author_id, 3, s.bank->users);
        // Server members: the cached ones, and for a typed name the same gateway
        // member search the composer's '@' runs; its results rebuild this list.
        const u64 member_guild = s.scope == MessageSearchScope::guild ? s.scope_id : active->guild_id;
        if (member_guild) {
            auto query = b.filter.prefix();
            if (query.bytes && query.data[0] == '@')
                query = {query.data + 1, query.bytes - 1};
            if (query.bytes) {
                application_member_query_note(member_guild, active->id, query);
                requested_members = s.member_query_active = true;
            }
            for (u32 i = 0; i < ui.guild_member_store.count; ++i) {
                const auto& member = ui.guild_member_store.records[ui.guild_member_store.order[i]];
                if (member.occupied && member.guild_id == member_guild)
                    choice_user(s, b, member.user_id, 4, ui.user_store,
                                {member.nickname, member.nickname_bytes});
            }
        }
    }
    if (!requested_members)
        member_query_withdraw(s);
    if (!b.source.rows.count && kind != UiSearchFilterKind::link_from &&
        kind != UiSearchFilterKind::file_name)
        b.source.hint = b.capacity_limited ? utf8_literal("Up to four values per filter.")
                                           : utf8_literal("No matches. Keep typing.");
    if (filter_publication_counter != ~u32{0})
        b.source.revision = ++filter_publication_counter;
    else {
        b.source.revision = ~u32{0};
        b.source.rows = {};
        b.source.hint = utf8_literal("Suggestions unavailable. Type a value.");
    }
    b.source.selected = b.source.rows.count ? 0 : kUiSearchFilterNoSelection;
    if (preserve_selection)
        for (u32 i = 0; i < b.source.rows.count; ++i)
            if (b.rows[i].id == selected_id && b.rows[i].semantic == selected_semantic) {
                b.source.selected = static_cast<u16>(i);
                break;
            }
    return true;
}
bool choice_accept(ApplicationSearch& s, u32 revision, u32 index) noexcept {
    if (!choices_current(&s) || !s.choices || revision != s.choices->source.revision ||
        index >= s.choices->source.rows.count)
        return false;
    auto& b = *s.choices;
    const auto row = b.rows[index];
    const auto* active = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                          g_app.account_ui->active_channel_id);
    if (!active || !choice_channel(s, *active))
        return false;
    if (b.filter.kind == UiSearchFilterKind::in) {
        const auto* c = chat_state_store_find_channel(g_app.account_ui->chat_state, row.id);
        if (!c || !choice_channel(s, *c))
            return false;
    } else if ((b.filter.kind == UiSearchFilterKind::from ||
                b.filter.kind == UiSearchFilterKind::mentions) &&
               !choice_user_allowed(row.id))
        return false;
    char replacement[25];
    copy_bytes(replacement, row.value.data, row.value.bytes);
    u32 bytes = row.value.bytes;
    const auto text = message_composer_text(s.edit);
    if (b.filter.value_end == text.bytes || text.data[b.filter.value_end] != ' ')
        replacement[bytes++] = ' ';
    if (!s.edit.history && !message_composer_enable_history(&s.edit, 4096))
        return false;
    message_composer_break_history_run(&s.edit);
    if (message_composer_replace_range(&s.edit, b.filter.value_begin, b.filter.value_end,
                                       {replacement, bytes}) != MessageComposerResult::changed)
        return false;
    message_composer_break_history_run(&s.edit);
    s.validation = {};
    s.suggestions = false;
    s.filters = s.sort_menu = false;
    focus(s, true);
    return repaint();
}
bool choice_move(ApplicationSearch& s, bool previous) noexcept {
    if (!choices_current(&s) || !s.choices->source.rows.count)
        return false;
    auto& source = s.choices->source;
    source.selected =
        static_cast<u16>(previous ? (source.selected ? source.selected - 1 : source.rows.count - 1)
                                  : (source.selected + 1) % source.rows.count);
    ++s.content.revision;
    return repaint();
}
bool resolve_user(void*, Utf8View name, u64* id) noexcept {
    if (!g_app.account_ui || !id)
        return false;
    const auto& state = g_app.account_ui->chat_state;
    if (name.bytes && name.data[0] == '@')
        name = {name.data + 1, name.bytes - 1};
    if (utf8_equal(name, utf8_literal("me"))) {
        *id = state.self_user_id;
        return *id != 0;
    }
    const auto& users = g_app.account_ui->user_store;
    u64 found = 0;
    for (u32 i = 0; i < users.count; ++i) {
        const auto& user = users.records[users.order[i]];
        if (!utf8_equal(name, user_store_username(users, user)) &&
            !utf8_equal(name, user_store_name(users, user)))
            continue;
        if (found && found != user.id)
            return false;
        found = user.id;
    }
    *id = found;
    return found != 0;
}
bool resolve_channel(void* opaque, Utf8View name, u64* id) noexcept {
    const auto& s = *static_cast<ApplicationSearch*>(opaque);
    if (!g_app.account_ui || !id)
        return false;
    if (name.bytes && name.data[0] == '#')
        name = {name.data + 1, name.bytes - 1};
    const auto& state = g_app.account_ui->chat_state;
    u64 found = 0;
    for (u32 i = 0; i < state.channel_count; ++i) {
        const auto& channel = state.channels[state.channel_order[i]];
        if (s.scope == MessageSearchScope::guild && channel.guild_id != s.scope_id)
            continue;
        if (s.scope == MessageSearchScope::channel && channel.id != s.scope_id)
            continue;
        if (!channel_is_message_capable(channel.type) ||
            !utf8_equal(name, chat_state_store_channel_name(state, channel)))
            continue;
        if (found && found != channel.id)
            return false;
        found = channel.id;
    }
    *id = found;
    return found != 0;
}
bool local_date(void*, Utf8View date, u64* first, u64* last) noexcept {
    if (date.bytes != 10 || !first || !last)
        return false;
    SYSTEMTIME local{};
    for (u32 i = 0; i < 4; ++i)
        local.wYear = static_cast<WORD>(local.wYear * 10 + date.data[i] - '0');
    local.wMonth = static_cast<WORD>((date.data[5] - '0') * 10 + date.data[6] - '0');
    local.wDay = static_cast<WORD>((date.data[8] - '0') * 10 + date.data[9] - '0');
    SYSTEMTIME utc{}, next{}, utc_next{};
    FILETIME local_file{}, utc_file{}, utc_next_file{};
    if (!SystemTimeToFileTime(&local, &local_file) ||
        !TzSpecificLocalTimeToSystemTimeEx(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &utc_file))
        return false;
    u64 ticks = (static_cast<u64>(local_file.dwHighDateTime) << 32) | local_file.dwLowDateTime;
    ticks += 864000000000ull;
    local_file = {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    if (!FileTimeToSystemTime(&local_file, &next) ||
        !TzSpecificLocalTimeToSystemTimeEx(nullptr, &next, &utc_next) ||
        !SystemTimeToFileTime(&utc_next, &utc_next_file))
        return false;
    const u64 start = (static_cast<u64>(utc_file.dwHighDateTime) << 32) | utc_file.dwLowDateTime,
              end = (static_cast<u64>(utc_next_file.dwHighDateTime) << 32) | utc_next_file.dwLowDateTime;
    constexpr u64 snowflake_epoch_ticks = 130645440000000000ull;
    if (end <= snowflake_epoch_ticks)
        return false;
    *first = start > snowflake_epoch_ticks ? (start - snowflake_epoch_ticks) / 10000 : 0;
    *last = (end - snowflake_epoch_ticks) / 10000;
    return *last > *first;
}
void recent_load(ApplicationSearch& s) noexcept {
    if (!s.profile_id || s.recent_profile == s.profile_id)
        return;
    if (!s.recent)
        s.recent = static_cast<SearchRecentList*>(
            VirtualAlloc(nullptr, sizeof(SearchRecentList), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    wchar_t path[512];
    if (!s.recent || !search_recent_path(s.profile_id, path, 512))
        return;
    search_recent_load_file(path, s.recent);
    s.recent_profile = s.profile_id;
}
void recent_store(ApplicationSearch& s) noexcept {
    wchar_t path[512];
    if (s.recent && s.recent_profile && s.recent_profile == s.profile_id &&
        search_recent_path(s.profile_id, path, 512))
        search_recent_save_file(path, *s.recent);
}
bool commit(ApplicationSearch& s, bool tab_switch = false) noexcept {
    if (thread_scope(s, nullptr)) {
        // Thread names, not messages: the browser lists the matches in place
        // and opens the chosen thread. The field keeps the query and focus.
        s.filters = s.suggestions = s.sort_menu = false;
        s.validation = {};
        s.committed_edit_revision = s.edit.text_revision;
        application_thread_browser_search(message_composer_text(s.edit));
        return repaint();
    }
    // A widened search leaves the thread browser for the results pane.
    if (application_thread_browser_active())
        application_thread_browser_close();
    application_pinned_messages_close();
    current_scope(s);
    s.panel = true;
    if (!context_current(s))
        return repaint();
    cancel_submitted(s);
    message_search_cancel(&s.controller);
    s.admission_busy = false;
    const bool keep_editor_focus = s.focused;
    const UiSearchQueryContext context{&s, resolve_user, resolve_channel, local_date};
    auto parsed =
        ui_search_query_parse(message_composer_text(s.edit), s.scope, s.scope_id, s.sort, &context);
    parsed.query.tab = s.tab;
    parsed.query.endpoint =
        s.tabs_unavailable ? MessageSearchEndpoint::legacy : MessageSearchEndpoint::tabs;
    parsed.query.tab_counts = !tab_switch;
    s.panel = true;
    s.filters = false;
    s.suggestions = s.sort_menu = false;
    if (!parsed.ok()) {
        s.validation = parsed.error == UiSearchQueryError::invalid_date
                           ? utf8_literal("Use a valid YYYY-MM-DD date (2015-2100).")
                       : parsed.error == UiSearchQueryError::invalid_filter
                           ? utf8_literal("Couldn't find a match. Try an ID.")
                           : utf8_literal("Search text is invalid or too long.");
        return repaint();
    }
    if (!message_search_query_valid(parsed.query)) {
        s.validation = utf8_literal("Enter search text or choose a filter.");
        return repaint();
    }
    for (u8 i = 0; i < parsed.query.channel_count; ++i) {
        const auto* channel =
            chat_state_store_find_channel(g_app.account_ui->chat_state, parsed.query.channel_ids[i]);
        if (!channel || !channel->occupied || !channel_is_message_capable(channel->type) ||
            (s.scope == MessageSearchScope::guild ? channel->guild_id != s.scope_id
                                                         : channel->id != s.scope_id)) {
            s.validation = utf8_literal("Choose a channel in the current search scope.");
            return repaint();
        }
    }
    s.validation = {};
    if (!message_search_begin(&s.controller, s.account_id, parsed.query)) {
        s.validation = utf8_literal("Search could not be queued.");
        return repaint();
    }
    retire_actions(s);
    s.admission_busy = false;
    s.committed_edit_revision = s.edit.text_revision;
    if (!tab_switch)
        s.tab_known = 0;
    recent_load(s);
    if (s.recent && search_recent_push(s.recent, message_composer_text(s.edit)))
        recent_store(s);
    if (tab_switch) {
        if (!repaint())
            return false;
        for (u32 i = 1; i < s.scene.widgets.count; ++i)
            if (s.scene.targets[i].action == UiSearchAction::tab &&
                s.scene.targets[i].source_index == static_cast<u32>(s.tab)) {
                g_app.context_focus = &s.scene.widgets;
                ui_widget_manager_focus(&s.scene.widgets,
                                        {static_cast<u16>(i), s.scene.widgets.generation});
                break;
            }
        application_request_frame();
        return true;
    }
    if (keep_editor_focus)
        return repaint();
    focus(s, false);
    if (!repaint())
        return false;
    g_app.context_focus = &s.scene.widgets;
    ui_widget_manager_focus(&s.scene.widgets, s.scene.buttons[0]);
    application_request_frame();
    return true;
}
void decorate(SearchBank& bank) noexcept {
    bank.measured_native_text = nullptr;
    bank.measured_width = 0;
    clear_bytes(bank.body_heights, sizeof(bank.body_heights));
    for (u32 i = 0; i < bank.page.message_count; ++i) {
        u32 group = 0;
        for (u32 g = 0; g < bank.page.group_count; ++g)
            if (i >= bank.groups[g].first_message)
                group = g;
        const auto& m = bank.messages[i].message;
        const auto* channel =
            chat_state_store_find_channel(g_app.account_ui->chat_state, m.channel_id);
        const auto name = channel
                              ? chat_state_store_channel_name(g_app.account_ui->chat_state, *channel)
                              : Utf8View{};
        auto* label = bank.channels[group];
        u32 used = 0;
        label[used++] = '#';
        if (name.bytes) {
            const u32 bytes = minimum<u32>(name.bytes, sizeof(bank.channels[group]) - 1);
            u32 last = bytes;
            while (last < name.bytes && last && static_cast<u8>(name.data[last]) >= 0x80 &&
                   static_cast<u8>(name.data[last]) < 0xc0)
                --last;
            CopyMemory(label + used, name.data, last);
            used += last;
        } else
            used += decimal_write(label + used, m.channel_id);
        bank.channel_bytes[group] = static_cast<u8>(used);
        auto* detail = bank.details[i];
        used = 0;
        if (m.timestamp_ms) {
            // The official client's calendar label ("Today at 3:45 PM", "10/5/2026 3:45 PM");
            // the short date alone when a long locale format overflows the slot.
            const u64 now_seconds = clock_unix_milliseconds() / 1000u;
            auto formatted = format_timestamp_calendar(m.timestamp_ms / 1000u, now_seconds,
                                                       Slice<char>{detail, sizeof(bank.details[i])});
            if (!formatted.ok)
                formatted = format_timestamp_local(m.timestamp_ms / 1000u, 'd', now_seconds,
                                                   Slice<char>{detail, sizeof(bank.details[i])});
            used = formatted.ok ? formatted.bytes : 0;
        }
        bank.detail_bytes[i] = static_cast<u8>(used);
    }
}
bool activate(ApplicationSearch& s, UiSearchTarget target) noexcept {
    switch (target.action) {
    case UiSearchAction::filter_value:
        return choice_accept(s, target.source_revision, target.source_index);
    case UiSearchAction::edit:
        s.suggestions = true;
        s.filters = s.sort_menu = false;
        focus(s, true);
        return repaint();
    case UiSearchAction::submit:
        return commit(s);
    case UiSearchAction::scope: {
        // Removing the chip widens this browsing session to the whole server.
        ApplicationThreadBrowserSearchScope threads{};
        if (application_thread_browser_search_scope(&threads))
            s.widened_thread_session = threads.session;
        s.suggestions = true;
        s.filters = s.sort_menu = false;
        focus(s, true);
        return repaint();
    }
    case UiSearchAction::tab:
        if (target.source_index >= kMessageSearchTabCount)
            return true;
        s.filters = s.suggestions = s.sort_menu = false;
        s.tab = static_cast<MessageSearchTab>(target.source_index);
        focus(s, false);
        return commit(s, true);
    case UiSearchAction::recent: {
        const auto value = s.recent ? search_recent_entry(*s.recent, target.source_index) : Utf8View{};
        if (!value.bytes || application_composer_text_input_composing())
            return true;
        char text[kSearchRecentTextBytes];
        copy_bytes(text, value.data, value.bytes);
        message_composer_restore(&s.edit, 0, {text, value.bytes}, value.bytes, value.bytes);
        s.suggestions = false;
        return commit(s);
    }
    case UiSearchAction::clear_recent:
        if (s.recent && s.recent->count) {
            search_recent_clear(s.recent);
            recent_store(s);
        }
        s.suggestions = true;
        focus(s, true);
        return repaint();
    case UiSearchAction::close:
        cancel_submitted(s);
        message_search_cancel(&s.controller);
        s.panel = false;
        retire_actions(s);
        choices_release(s);
        s.filters = false;
        s.suggestions = s.sort_menu = false;
        focus(s, false);
        return repaint();
    case UiSearchAction::filters:
    case UiSearchAction::more_filters:
        s.filters = !s.filters;
        s.suggestions = s.filters;
        s.sort_menu = false;
        focus(s, false);
        if (!repaint())
            return false;
        g_app.context_focus = &s.scene.widgets;
        if (s.filters)
            for (u32 i = 1; i < s.scene.widgets.count; ++i)
                if (s.scene.targets[i].action == UiSearchAction::filter_from) {
                    ui_widget_manager_focus(&s.scene.widgets,
                                            {static_cast<u16>(i), s.scene.widgets.generation});
                    break;
                }
        application_request_frame();
        return true;
    case UiSearchAction::sort:
        s.sort_menu = !s.sort_menu;
        s.filters = s.suggestions = false;
        focus(s, false);
        if (!repaint())
            return false;
        g_app.context_focus = &s.scene.widgets;
        if (s.sort_menu)
            for (u32 i = 1; i < s.scene.widgets.count; ++i)
                if (s.scene.targets[i].action == UiSearchAction::newest) {
                    ui_widget_manager_focus(&s.scene.widgets,
                                            {static_cast<u16>(i), s.scene.widgets.generation});
                    break;
                }
        application_request_frame();
        return true;
    case UiSearchAction::result:
        if (s.filters || s.suggestions || s.sort_menu || !context_current(s))
            return true;
        focus(s, false);
        s.filters = false;
        s.suggestions = s.sort_menu = false;
        {
            ApplicationSearchSourceTicket ticket{};
            if (!ui_search_scene_target_current(s.scene, target) ||
                !application_search_source_ticket(target.source_index, &ticket) ||
                ticket.message_id != target.message_id)
                return true;
            return application_search_navigation_begin(ticket) && repaint();
        }
    case UiSearchAction::rich_item: {
        if (s.filters || s.suggestions || s.sort_menu || !ui_search_scene_target_current(s.scene, target))
            return true;
        ApplicationSearchSourceTicket ticket{};
        ApplicationSearchSourceView view{};
        if (!application_search_source_ticket(target.source_index, &ticket) ||
            ticket.message_id != target.message_id ||
            !application_search_source_resolve(ticket, target.snapshot, &view))
            return true;
        if (target.save) {
            if (target.rich_kind != MessageSceneRichItemKind::attachment || !view.rich ||
                target.rich_index >= view.rich->attachment_count)
                return true;
            return application_downloads_begin_search(ticket, view.rich->attachments[target.rich_index].id,
                                                      target.snapshot);
        }
        if (application_video_open_search(target))
            return true;
        UiRect rectangle{}, clip{};
        if (ui_search_scene_rich_preview_geometry(s.scene, target, &rectangle, &clip) &&
            ui_rect_has_area(clip) &&
            application_image_viewer_open_search(ticket, target.rich_kind, target.rich_index,
                                                 target.snapshot, rectangle, target.resource))
            return true;
        return application_search_content_activate(ticket, target);
    }
    case UiSearchAction::previous:
    case UiSearchAction::next:
    case UiSearchAction::page: {
        if (!context_current(s))
            return repaint();
        const auto offset = s.controller.query.offset;
        const auto next = target.action == UiSearchAction::page       ? target.page_offset
                          : target.action == UiSearchAction::previous ? (offset >= 25 ? offset - 25 : 0)
                                                                      : offset + 25;
        if (message_search_page(&s.controller, next)) {
            retire_actions(s);
            return repaint();
        }
        return true;
    }
    case UiSearchAction::newest:
    case UiSearchAction::oldest:
    case UiSearchAction::relevance:
        s.sort_menu = false;
        s.sort = target.action == UiSearchAction::newest   ? MessageSearchSort::newest
                 : target.action == UiSearchAction::oldest ? MessageSearchSort::oldest
                                                           : MessageSearchSort::relevance;
        return commit(s);
    default:
        break;
    }
    if (ui_search_action_filter(target.action)) {
        const Utf8View tokens[] = {
            utf8_literal("from:"),     utf8_literal("in:"),       utf8_literal("has:"),
            utf8_literal("mentions:"), utf8_literal("on:"),       utf8_literal("before:"),
            utf8_literal("after:"),    utf8_literal("pinned:"),   utf8_literal("linkFrom:"),
            utf8_literal("fileType:"), utf8_literal("fileName:"), utf8_literal("authorType:")};
        if (application_composer_text_input_composing())
            return false;
        const auto token =
            tokens[static_cast<u8>(target.action) - static_cast<u8>(UiSearchAction::filter_from)];
        const auto text = message_composer_text(s.edit);
        const u32 first = minimum(s.edit.anchor, s.edit.caret), last = maximum(s.edit.anchor, s.edit.caret);
        char inserted[16];
        u32 used = 0;
        if (first && text.data[first - 1] != ' ')
            inserted[used++] = ' ';
        copy_bytes(inserted + used, token.data, token.bytes);
        used += token.bytes;
        const u32 caret = first + used;
        if (last < text.bytes && text.data[last] != ' ')
            inserted[used++] = ' ';
        if (!s.edit.history && !message_composer_enable_history(&s.edit, 4096))
            return false;
        message_composer_break_history_run(&s.edit);
        if (message_composer_replace_range(&s.edit, first, last, {inserted, used}) !=
            MessageComposerResult::changed)
            return false;
        message_composer_break_history_run(&s.edit);
        message_composer_set_selection(&s.edit, caret, caret);
        s.filters = false;
        s.suggestions = true;
        s.sort_menu = false;
        focus(s, true);
        return application_search_edit_changed(true);
    }
    return true;
}
TextInputTarget delegate(const TextInputTarget& target) noexcept {
    auto result = message_composer_input_target(static_cast<MessageComposer*>(target.context));
    result.identity = target.identity;
    return result;
}
bool edit_ready(const TextInputTarget& t) noexcept {
    return application_search_edit_focused() && t.context == application_search_edit_model() &&
           text_input_ready(delegate(t));
}
u32 edit_revision(const TextInputTarget& t) noexcept {
    return text_input_revision(delegate(t));
}
TextInputResult edit_length(const TextInputTarget& t, u32* n) noexcept {
    return text_input_length(delegate(t), n);
}
TextInputResult edit_selection(const TextInputTarget& t, TextInputSelection* s) noexcept {
    return text_input_selection(delegate(t), s);
}
TextInputResult edit_select(const TextInputTarget& t, TextInputSelection s) noexcept {
    return text_input_select(delegate(t), s);
}
TextInputResult edit_copy(const TextInputTarget& t, u32 a, u32 b, Slice<u16> out, u32* n,
                          u32* next) noexcept {
    return text_input_copy(delegate(t), a, b, out, n, next);
}
TextInputResult edit_replace(const TextInputTarget& t, u32 a, u32 b, ConstSlice<u16> input,
                             TextInputChange* change) noexcept {
    if (!change || (!input.data && input.count))
        return TextInputResult::invalid_input;
    for (u32 i = 0; i < input.count; ++i)
        if (input[i] < 32 || input[i] == 0x2028 || input[i] == 0x2029)
            return TextInputResult::invalid_input;
    return text_input_replace(delegate(t), a, b, input, change);
}
void edit_cancel(const TextInputTarget& t) noexcept {
    text_input_cancel(delegate(t));
}
bool edit_begin(const TextInputTarget& t) noexcept {
    auto* m = static_cast<MessageComposer*>(t.context);
    return (m->history || message_composer_enable_history(m, 2048)) && text_input_begin_group(delegate(t));
}
void edit_end(const TextInputTarget& t) noexcept {
    text_input_end_group(delegate(t));
}
constexpr TextInputOperations edit_operations{edit_ready,  edit_revision, edit_length,  edit_selection,
                                              edit_select, edit_copy,     edit_replace, edit_cancel,
                                              edit_begin,  edit_end};
bool scroll_read(void* opaque, u64 key, UiScrollRange* out) noexcept {
    if (opaque != g_app.search || !out || !front())
        return false;
    const auto& s = *g_app.search;
    if (!s.panel || key != s.edit.document_identity)
        return false;
    *out = {s.scene.body,     s.scene.scroll_dips, ui_search_scene_scroll_maximum(s.scene), 40, 1, 0,
            s.source.revision};
    return true;
}
bool scroll_write(void* opaque, u64 key, double offset, bool* changed) noexcept {
    UiScrollRange range{};
    if (!changed || !scroll_read(opaque, key, &range))
        return false;
    auto& s = *g_app.search;
    const float old = s.scene.scroll_dips;
    if (!ui_search_scene_scroll(&s.scene, static_cast<float>(offset)))
        return false;
    *changed = old != s.scene.scroll_dips;
    if (*changed)
        application_request_frame();
    return true;
}
} // namespace
bool application_search_active() noexcept {
    return g_app.search && g_app.search->panel && available();
}
bool application_search_close_panel() noexcept {
    auto* s = g_app.search;
    if (!s)
        return true;
    cancel_submitted(*s);
    message_search_cancel(&s->controller);
    s->panel = s->filters = s->suggestions = s->sort_menu = false;
    retire_actions(*s);
    choices_release(*s);
    focus(*s, false);
    s->dragging = s->captured = s->scrollbar_dragging = false;
    s->pressed = {};
    ui_widget_manager_capture(&s->scene.widgets, ui_widget_invalid_handle());
    if (GetCapture() == g_app.window)
        ReleaseCapture();
    return repaint();
}
bool application_search_read_blocked() noexcept {
    return g_app.search && available() &&
           (g_app.search->focused || g_app.search->panel || g_app.search->suggestions ||
            g_app.search->sort_menu);
}
namespace {
bool accepted_current(const ApplicationSearch& s) noexcept {
    const auto* b = s.bank;
    return b && b->initialized && b->publication && s.panel && context_current(s) &&
           b->action_epoch == s.action_epoch && b->account_scope == g_app.plugin_account_scope_serial &&
           b->profile_id == g_app.account.status.profile_id &&
           b->self_id == g_app.account_ui->chat_state.self_user_id &&
           b->account_instance == g_app.account.instance && b->account_ui == g_app.account_ui &&
           g_app.account.instance && !g_app.account.stop_failed && !g_app.login.visible;
}
bool ticket_current(const ApplicationSearch& s, const ApplicationSearchSourceTicket& ticket) noexcept {
    return accepted_current(s) && ticket.publication_revision == s.bank->publication &&
           ticket.index < s.bank->page.message_count &&
           s.bank->messages[ticket.index].message.id == ticket.message_id &&
           message_store_resolve(s.bank->store, s.bank->result_handles[ticket.index]);
}
bool channel_authorized(ApplicationSearch& s, u32 index, bool permit_unknown) noexcept {
    auto& b = *s.bank;
    const auto& item = b.messages[index];
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, item.message.channel_id);
    if (!channel)
        return permit_unknown && !b.channel_generations[index] &&
               s.scope == MessageSearchScope::guild && item.guild_known &&
               item.guild_id == s.scope_id;
    if (!channel->occupied || !channel_is_message_capable(channel->type) ||
        (channel->flags & kChannelObfuscatedFlag) ||
        (s.scope == MessageSearchScope::guild ? channel->guild_id != s.scope_id
                                                     : channel->guild_id || channel->id != s.scope_id ||
                                                           !channel_is_private(channel->type)))
        return false;
    if (b.channel_generations[index] && b.channel_generations[index] != channel->generation)
        return false;
    if (channel->guild_id) {
        const auto* guild = chat_state_store_find_guild(state, channel->guild_id);
        constexpr u32 required = channel_action_view | channel_action_read_history;
        if (!guild || guild->unavailable ||
            ((!channel->permissions_resolved || (channel->actions & required) != required ||
              !chat_state_store_channel_visible(state, *channel)) &&
             !(permit_unknown && row_access_check(s, index))))
            return false;
    }
    // A guild channel not seen before can get its first model generation once
    // navigation validates its metadata. It never gets another generation after
    // it is deleted or recreated.
    if (!b.channel_generations[index])
        b.channel_generations[index] = channel->generation;
    return true;
}
bool row_unavailable(const ApplicationSearch& s, u32 index) noexcept {
    if (!s.bank || !g_app.account_ui || index >= s.bank->page.message_count)
        return true;
    const auto& b = *s.bank;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                           b.messages[index].message.channel_id);
    if (!channel)
        return true;
    constexpr u32 required = channel_action_view | channel_action_read_history;
    return !channel->occupied || !channel_is_message_capable(channel->type) ||
           (channel->flags & kChannelObfuscatedFlag) ||
           (b.channel_generations[index] && b.channel_generations[index] != channel->generation) ||
           (channel->guild_id &&
            (!channel->permissions_resolved || (channel->actions & required) != required ||
             !chat_state_store_channel_visible(g_app.account_ui->chat_state, *channel)));
}
bool row_access_check(const ApplicationSearch& s, u32 index) noexcept {
    if (!s.bank || !g_app.account_ui || index >= s.bank->page.message_count)
        return false;
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel =
        chat_state_store_find_channel(state, s.bank->messages[index].message.channel_id);
    if (!channel) {
        const auto& item = s.bank->messages[index];
        const auto* guild = chat_state_store_find_guild(state, s.scope_id);
        return !s.bank->channel_generations[index] && s.scope == MessageSearchScope::guild &&
               item.guild_known && item.guild_id == s.scope_id && guild && !guild->unavailable;
    }
    if (!channel->occupied || (channel->flags & kChannelObfuscatedFlag) ||
        (channel->type != 10 && channel->type != 11 && channel->type != 12) ||
        (s.bank->channel_generations[index] && s.bank->channel_generations[index] != channel->generation))
        return false;
    if (channel->type == 12 && channel->thread_self_joined_known && !channel->thread_self_joined)
        return false;
    const auto* guild = chat_state_store_find_guild(state, channel->guild_id);
    if (!guild || guild->unavailable)
        return false;
    const auto* parent = chat_state_store_find_channel(state, channel->parent_id);
    constexpr u32 required = channel_action_view | channel_action_read_history;
    if (parent && parent->permissions_resolved && (parent->actions & required) != required)
        return false;
    return !channel->permissions_resolved || (channel->type == 12 && !channel->thread_self_joined_known &&
                                              !chat_state_store_thread_accessible(state, *channel));
}
void synchronize_privacy(ApplicationSearch& s) noexcept {
    if (!s.bank)
        return;
    bool changed = false;
    for (u32 i = 0; i < s.bank->page.message_count; ++i) {
        const bool unavailable = row_unavailable(s, i);
        const bool check = row_access_check(s, i);
        changed |= unavailable != s.bank->unavailable[i] || check != s.bank->access_checks[i];
        s.bank->unavailable[i] = unavailable;
        s.bank->access_checks[i] = check;
    }
    if (!changed)
        return;
    if (auto* rich = const_cast<MessageScene*>(ui_search_scene_message_scene(s.scene))) {
        ui_widget_manager_capture(&rich->widgets, ui_widget_invalid_handle());
        ui_widget_manager_focus(&rich->widgets, ui_widget_invalid_handle());
        if (g_app.context_focus == &rich->widgets)
            g_app.context_focus = &s.scene.widgets;
    }
    ++s.source.revision;
    s.pressed = {};
    s.captured = s.dragging = false;
    ui_widget_manager_capture(&s.scene.widgets, ui_widget_invalid_handle());
    if (GetCapture() == g_app.window)
        ReleaseCapture();
    if (s.text_owner) {
        s.text_owner->presentation.active = false;
        s.text_owner->input_signature = 0;
        s.text_owner->presentation.command_count = 0;
        s.text_owner->presentation.semantic_count = 0;
    }
    renderer_unbind_search_message_text_presentation(&g_app.renderer);
    application_request_frame();
}
} // namespace
bool application_search_source_ticket(u32 index, ApplicationSearchSourceTicket* out) noexcept {
    auto* s = g_app.search;
    if (!out || !s || !accepted_current(*s) || index >= s->bank->page.message_count)
        return false;
    *out = {s->bank->messages[index].message.id, s->bank->publication, index};
    return true;
}
bool application_search_source_resolve(const ApplicationSearchSourceTicket& ticket, bool snapshot,
                                       ApplicationSearchSourceView* out) noexcept {
    auto* s = g_app.search;
    if (!out || !s || !ticket_current(*s, ticket) || !channel_authorized(*s, ticket.index, false))
        return false;
    auto& b = *s->bank;
    const auto handle = b.result_handles[ticket.index];
    const auto* message = message_store_resolve(b.store, handle);
    const auto* outer = message_rich_store_find(b.rich, ticket.message_id);
    const auto* rich = snapshot ? message_rich_store_snapshot(b.rich, ticket.message_id) : outer;
    if (!message || message->revision != ticket.publication_revision || (snapshot && (!outer || !rich)) ||
        (rich && rich->message_revision != ticket.publication_revision))
        return false;
    const auto& item = b.messages[ticket.index];
    *out = {ticket,
            &b.store,
            message,
            handle,
            &b.rich,
            rich,
            {b.author_names[ticket.index], b.author_bytes[ticket.index]},
            {b.avatars[ticket.index], b.avatar_bytes[ticket.index]},
            snapshot && outer ? outer->reference.channel_id : 0,
            item.guild_known ? item.guild_id
                             : (s->scope == MessageSearchScope::guild ? s->scope_id : 0),
            snapshot};
    return true;
}
bool application_search_source_resolve(u64 id, u32 publication, bool snapshot,
                                       ApplicationSearchSourceView* out) noexcept {
    auto* s = g_app.search;
    if (!s || !s->bank || s->bank->publication != publication)
        return false;
    for (u32 i = 0; i < s->bank->page.message_count; ++i)
        if (s->bank->messages[i].message.id == id)
            return application_search_source_resolve({id, publication, i}, snapshot, out);
    return false;
}
bool application_search_navigation_source(const ApplicationSearchSourceTicket& ticket,
                                          ApplicationSearchNavigationSource* out) noexcept {
    auto* s = g_app.search;
    if (!out || !s || !ticket_current(*s, ticket) || !channel_authorized(*s, ticket.index, true))
        return false;
    const auto& item = s->bank->messages[ticket.index];
    const auto* channel =
        chat_state_store_find_channel(g_app.account_ui->chat_state, item.message.channel_id);
    *out = {ticket, item.message.channel_id,
            item.guild_known ? item.guild_id
                             : (s->scope == MessageSearchScope::guild ? s->scope_id : 0),
            channel != nullptr};
    return true;
}
bool application_search_source_notice(const ApplicationSearchSourceTicket& ticket,
                                      Utf8View notice) noexcept {
    auto* s = g_app.search;
    if (!s || !ticket_current(*s, ticket))
        return false;
    s->validation = notice;
    application_request_frame();
    return true;
}
MessageTimeline* application_search_rich_timeline() noexcept {
    auto* s = g_app.search;
    return s && s->bank && s->panel && context_current(*s) ? &s->bank->timeline : nullptr;
}
UiSearchScene* application_search_scene() noexcept {
    return g_app.search ? &g_app.search->scene : nullptr;
}
MessageTextPresentation* application_search_message_text_presentation() noexcept {
    auto* s = g_app.search;
    if (!s || !application_search_rich_timeline())
        return nullptr;
    if (!s->text_owner) {
        s->text_owner = static_cast<SearchTextOwner*>(
            VirtualAlloc(nullptr, sizeof(SearchTextOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!s->text_owner)
            return nullptr;
        auto& o = *s->text_owner;
        if (!message_text_presentation_initialize(
                &o.presentation, {o.commands, kMaximumMessageTextCommands},
                {o.utf8, kMaximumMessageTextUtf8Bytes}, {o.styles, kMaximumMessageTextStyleSpans},
                {o.semantics, kMaximumMessageTextSemantics},
                {o.measurements, kMaximumMessageTextMeasurements},
                {o.reveals, kMaximumMessageTextSpoilerReveals},
                {o.byte_styles, kMessageTextByteStyleScratchCapacity})) {
            VirtualFree(s->text_owner, 0, MEM_RELEASE);
            s->text_owner = nullptr;
            return nullptr;
        }
    }
    message_text_presentation_bind_guild_members(&s->text_owner->presentation, &s->bank->members);
    return &s->text_owner->presentation;
}
void application_search_theme_changed() noexcept {
    auto* s = g_app.search;
    if (!s)
        return;
    ui_search_scene_bind_theme_styles(&s->scene, application_theme().styles);
    if (s->bank) {
        s->bank->measured_width = -1;
        s->bank->measured_dpi = 0;
        s->bank->polls.backend_revision = renderer_text_measure_revision(g_app.renderer);
    }
    if (s->text_owner) {
        auto& p = s->text_owner->presentation;
        p.measurement_history_count = 0;
        p.measurement_history_offset = 0;
        p.measurement_count = 0;
        s->text_owner->input_signature = 0;
    }
    application_request_frame();
}
void application_search_media_dimensions_changed() noexcept {
    auto* s = g_app.search;
    if (!s || !s->bank)
        return;
    ++s->source.revision;
    application_request_frame();
}
namespace {
u64 search_text_signature(const ApplicationSearch& s, const MessageScene& scene) noexcept {
    const auto& state = g_app.account_ui->chat_state;
    const auto& p = s.text_owner->presentation;
    u64 hash = hash64_bytes(kHash64Seed, &application_theme(), sizeof(Theme));
    const u64 values[]{s.bank->publication,
                       s.source.revision,
                       scene.widgets.generation,
                       scene.first,
                       scene.count,
                       scene.layout_passes,
                       scene.rebuilds,
                       scene.data_updates,
                       scene.measurement_updates,
                       scene.observed_scroll_units,
                       s.bank->rich.updated,
                       s.bank->users.revision,
                       s.bank->members.revision,
                       state.guild_mutations,
                       state.channel_mutations,
                       state.permission_mutations,
                       state.emoji_mutations,
                       state.compactions,
                       g_app.account_ui->navigation_view.revision,
                       g_app.account_ui->navigation_view.guild_revision,
                       g_app.account_ui->text_measure_cache.generation,
                       g_app.renderer.dpi,
                       p.hovered_message_id,
                       p.hovered_source_offset,
                       static_cast<u64>(p.hovered_kind),
                       p.has_hover,
                       p.spoiler_reveal_count};
    hash = hash64_bytes(hash, values, sizeof(values));
    return hash64_bytes(hash, p.spoiler_reveals.data,
                        p.spoiler_reveal_count * static_cast<u32>(sizeof(MessageTextSpoilerReveal)));
}
} // namespace
bool application_search_source_link_current(const ApplicationSearchSourceTicket& ticket,
                                            const MessageTextHit& hit) noexcept {
    if (!front() || !application_search_synchronize())
        return false;
    ApplicationSearchSourceView view{};
    if (!hit.valid || hit.message_id != ticket.message_id ||
        hit.message_revision != ticket.publication_revision ||
        !application_search_source_resolve(ticket, (hit.source_offset & 0x40000000u) != 0, &view))
        return false;
    auto* s = g_app.search;
    if (!s->text_owner || !s->text_owner->presentation.active || !ui_search_scene_current(s->scene))
        return false;
    const auto& p = s->text_owner->presentation;
    for (u32 i = 0; i < p.command_count; ++i) {
        const auto& c = p.commands[i];
        if (c.message_id != hit.message_id || c.message_revision != hit.message_revision ||
            c.source_id != (hit.source_offset >> 16) ||
            !ui_rect_has_area(ui_search_scene_content_clip(s->scene, c.clip)))
            continue;
        for (u32 j = 0; j < c.semantics.count; ++j) {
            const auto& semantic = c.semantics[j];
            if (semantic.kind == hit.kind && semantic.target == hit.target &&
                semantic.source_offset == hit.source_offset && semantic.source_bytes == hit.source_bytes)
                return true;
        }
    }
    return false;
}
MessageComposer* application_search_edit_model() noexcept {
    return g_app.search ? &g_app.search->edit : nullptr;
}
bool application_search_edit_focused() noexcept {
    return g_app.search && g_app.search->focused && front();
}
TextInputTarget application_search_edit_target() noexcept {
    auto* model = application_search_edit_model();
    return {model, &edit_operations, model ? model->document_identity : 0};
}
UiRect application_search_edit_bounds() noexcept {
    return g_app.search ? g_app.search->scene.text : UiRect{};
}
void application_search_edit_cancel_native_group(const TextInputTarget& t) noexcept {
    if (t.operations != &edit_operations || !g_app.search || t.context != &g_app.search->edit ||
        t.identity != g_app.search->edit.document_identity)
        return;
    text_input_end_group(delegate(t));
    text_input_cancel(delegate(t));
}
bool application_search_edit_changed(bool) noexcept {
    auto* s = g_app.search;
    if (!s)
        return false;
    s->caret_visible = s->focused;
    s->blink_at =
        s->focused ? deadline_after(clock_monotonic_milliseconds(), kDefaultCaretBlinkMilliseconds) : 0;
    if (s->focused && s->edit.text_revision != s->committed_edit_revision) {
        s->suggestions = true;
        s->sort_menu = false;
    }
    return repaint();
}
bool application_search_synchronize() noexcept {
    if (!available()) {
        g_app.renderer.search_scene = nullptr;
        renderer_unbind_search_message_text_presentation(&g_app.renderer);
        if (g_app.search) {
            member_query_withdraw(*g_app.search, true);
            current_scope(*g_app.search);
        }
        if (g_app.search && g_app.search->focused)
            focus(*g_app.search, false);
        return true;
    }
    auto* s = g_app.search;
    if (!s) {
        s = static_cast<ApplicationSearch*>(
            VirtualAlloc(nullptr, sizeof(ApplicationSearch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!s)
            return false;
        if (!message_composer_initialize(&s->edit, {s->query_text, sizeof(s->query_text)})) {
            VirtualFree(s, 0, MEM_RELEASE);
            return false;
        }
        g_app.search = s;
        s->account_id = g_app.account_ui->chat_state.self_user_id;
        s->source = {s, 0, 1, resolve, source_current};
        current_scope(*s);
    }
    current_scope(*s);
    synchronize_privacy(*s);
    recent_load(*s);
    const bool heading_changed = heading(*s);
    ApplicationThreadBrowserSearchScope threads{};
    const bool thread_scoped = thread_scope(*s, &threads);
    // Message filters, recent message searches and the results pane do not
    // apply to thread names; the browser's own list shows those results.
    if (thread_scoped)
        s->suggestions = s->filters = s->sort_menu = false;
    if (!front() && (s->focused || s->captured || s->suggestions || s->filters || s->sort_menu)) {
        s->suggestions = s->filters = s->sort_menu = false;
        focus(*s, false);
        s->dragging = false;
        s->captured = false;
        s->scrollbar_dragging = false;
        s->pressed = {};
        ui_widget_manager_capture(&s->scene.widgets, ui_widget_invalid_handle());
        if (GetCapture() == g_app.window)
            ReleaseCapture();
    }
    const bool choices_changed = choices_synchronize(*s);
    const auto state = s->controller.state;
    const bool busy = state == MessageSearchState::fetching || state == MessageSearchState::ready_to_submit;
    UiSearchContent content{};
    content.edit = {
        message_composer_text(s->edit), s->edit.anchor, s->edit.caret, true, s->focused, s->caret_visible};
    content.panel = s->panel && !application_thread_browser_active();
    content.filters = s->filters;
    content.suggestions = s->suggestions && !application_composer_text_input_composing();
    content.filter_choices =
        choices_current(s) && s->choices && s->choices->filter.kind != UiSearchFilterKind::none
            ? &s->choices->source
            : nullptr;
    content.sort_menu = s->sort_menu;
    content.total = s->bank ? s->bank->page.total_results : 0;
    content.offset = s->controller.query.offset;
    content.sort = static_cast<u8>(s->sort);
    content.heading = content.placeholder = {s->scope_heading, s->heading_bytes};
    content.scope_chip = !thread_scoped ? Utf8View{}
                         : threads.forum ? utf8_literal("Posts")
                                         : utf8_literal("Threads");
    content.page_size = s->controller.query.limit ? s->controller.query.limit : 25;
    const auto& q = s->controller.query;
    content.filter_summary = q.author_count || q.mention_count || q.channel_count || q.has || q.min_id ||
                                     q.max_id || q.pinned != MessageSearchPinned::any ||
                                     q.has_excluded || q.author_types || q.author_types_excluded ||
                                     q.hostname_bytes || q.extension_bytes || q.filename_bytes
                                 ? utf8_literal("Filtered search")
                                 : Utf8View{};
    content.tabs = content.panel;
    content.tab = static_cast<u8>(s->tab);
    content.result_tab = static_cast<u8>(s->result_tab);
    content.tab_known = s->tab_known;
    for (u32 i = 0; i < kMessageSearchTabCount; ++i)
        content.tab_totals[i] = s->tab_totals[i];
    const u32 recent_count = s->recent ? s->recent->count : 0;
    for (u32 i = 0; i < recent_count; ++i)
        s->recent_views[i] = search_recent_entry(*s->recent, i);
    content.recent = recent_count && !thread_scoped ? s->recent_views : nullptr;
    content.recent_count = static_cast<u8>(thread_scoped ? 0 : recent_count);
    const bool recent_changed = s->recent && s->recent->revision != s->recent_published;
    if (s->recent)
        s->recent_published = s->recent->revision;
    content.phase = busy                                    ? UiSearchPhase::loading
                    : state == MessageSearchState::indexing ? UiSearchPhase::indexing
                    : state == MessageSearchState::failed   ? UiSearchPhase::failed
                    : state == MessageSearchState::ready    ? UiSearchPhase::ready
                                                            : UiSearchPhase::empty;
    const bool draft_changed = s->edit.text_revision != s->committed_edit_revision;
    content.stale = s->bank && (state != MessageSearchState::ready || draft_changed);
    content.previous = state == MessageSearchState::ready && content.offset != 0;
    content.next = state == MessageSearchState::ready && s->controller.loaded_groups &&
                   content.offset + 25 < s->controller.total_results &&
                   content.offset + 25 <= kMessageSearchMaximumOffset;
    content.status =
        s->validation.bytes ? s->validation
        : state == MessageSearchState::indexing
            ? (s->bank ? utf8_literal("Indexing messages. Showing earlier results.")
                       : utf8_literal("Indexing messages…"))
        : state == MessageSearchState::rate_limited
            ? (s->bank ? utf8_literal("Too many searches. Showing earlier results.")
                       : utf8_literal("Too many searches. Retrying…"))
        : busy ? (s->bank ? utf8_literal("Updating results…")
                          : utf8_literal("Searching…"))
        : state == MessageSearchState::failed
            ? (s->bank ? utf8_literal("Search failed. Showing earlier results.")
                       : utf8_literal("Search failed. Press Enter to try again."))
        : state == MessageSearchState::ready
            ? (draft_changed   ? utf8_literal("Press Enter to update results.")
               : content.total ? Utf8View{}
                               : utf8_literal("No messages matched this search."))
            : utf8_literal("Enter a query or choose Filters.");
    const u32 old = s->content.revision;
    content.revision = 0;
    const auto& prior = s->content;
    const auto view_equal = [](Utf8View left, Utf8View right) noexcept {
        return left.data == right.data && left.bytes == right.bytes;
    };
    // Aggregate padding (notably UiEditView's five trailing bytes) is not a
    // presentation change. Compare semantic fields so unchanged frames are reused.
    const bool content_equal =
        view_equal(content.edit.text, prior.edit.text) && content.edit.anchor == prior.edit.anchor &&
        content.edit.caret == prior.edit.caret && content.edit.active == prior.edit.active &&
        content.edit.focused == prior.edit.focused &&
        content.edit.caret_visible == prior.edit.caret_visible &&
        view_equal(content.status, prior.status) && content.total == prior.total &&
        content.offset == prior.offset && content.phase == prior.phase && content.sort == prior.sort &&
        content.panel == prior.panel && content.stale == prior.stale &&
        content.previous == prior.previous && content.next == prior.next &&
        content.filters == prior.filters && view_equal(content.heading, prior.heading) &&
        view_equal(content.placeholder, prior.placeholder) && view_equal(content.scope_chip, prior.scope_chip) &&
        view_equal(content.filter_summary, prior.filter_summary) && content.page_size == prior.page_size &&
        content.total_estimated == prior.total_estimated && content.suggestions == prior.suggestions &&
        content.sort_menu == prior.sort_menu && content.filter_choices == prior.filter_choices &&
        content.tabs == prior.tabs && content.tab == prior.tab && content.result_tab == prior.result_tab &&
        content.tab_known == prior.tab_known &&
        bytes_equal(content.tab_totals, prior.tab_totals, sizeof(content.tab_totals)) &&
        content.recent_count == prior.recent_count && !recent_changed;
    if (heading_changed || choices_changed || !content_equal || s->edit_revision != s->edit.revision)
        content.revision = old + 1;
    else
        content.revision = old;
    s->content = content;
    s->edit_revision = s->edit.revision;
    s->source.count = s->bank ? s->bank->page.message_count : 0;
    s->source.rich_timeline = s->bank ? &s->bank->timeline : nullptr;
    if (s->bank && g_app.account_ui) {
        s->bank->polls = g_app.account_ui->message_poll_binding;
        s->bank->polls.store = &g_app.account_ui->message_poll_store;
        s->bank->polls.measure = &g_app.account_ui->text_measure_cache;
        s->bank->polls.now_ms = clock_unix_milliseconds();
        s->bank->polls.project_state =
            +[](void* context, u64 id, Slice<UiPollAnswer> answers, UiPollState* state) noexcept {
                const auto* bank = static_cast<const SearchBank*>(context);
                return bank && application_poll_search_state(id, bank->publication, answers, state);
            };
        s->bank->polls.context = s->bank;
        if (!s->bank->polls.backend_revision)
            s->bank->polls.backend_revision = 1;
        s->source.polls = g_app.account_ui->message_poll_store.capacity ? &s->bank->polls : nullptr;
    } else
        s->source.polls = nullptr;
    s->source.rich_order =
        s->bank ? ConstSlice<MessageHandle>{s->bank->result_handles, s->bank->page.message_count}
                : ConstSlice<MessageHandle>{};
    const auto* title_node =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::title_bar));
    const auto title = title_node ? title_node->rectangle : UiRect{};
    auto header = ui_scene_header_geometry(g_app.shell_scene);
    // Over the call view the field takes its own slot in the call header (left
    // of close, right of the chat toggle); with no room, or fullscreen, it hides.
    UiRect call_field{};
    if (application_call_header_search(ui_rect_width(header.search_field), &call_field)) {
        if (!ui_rect_has_area(call_field)) {
            if (s->focused)
                focus(*s, false);
            g_app.renderer.search_scene = nullptr;
            return true;
        }
        header.search_field = call_field;
    }
    auto viewport = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::content);
    // The members pane is a sibling of content. Search overlays that same
    // right edge without altering the user's member-pane width or visibility.
    viewport.right = static_cast<float>(g_app.renderer.width) * 96.f / maximum(g_app.renderer.dpi, 1u);
    // A guild-selection handoff can temporarily have no matching channel.
    // current_scope has released its bank; keep the empty field/status usable
    // without lending the stale result source or failing the main frame.
    const UiSearchSource* scene_source = context_current(*s) ? &s->source : nullptr;
    if (!scene_source && s->bank)
        return false;
    ui_search_scene_bind_theme_styles(&s->scene, application_theme().styles);
    if (!ui_search_scene_update(&s->scene, scene_source, &s->content, viewport, title, &header))
        return false;
    if (measure_rows(*s) &&
        !ui_search_scene_update(&s->scene, scene_source, &s->content, viewport, title, &header))
        return false;
    g_app.renderer.search_scene = &s->scene;
    if (s->panel && s->bank) {
        auto* presentation = application_search_message_text_presentation();
        auto* scene = ui_search_scene_message_scene(&s->scene);
        if (!presentation || !scene)
            return false;
        const u64 now_seconds = clock_unix_milliseconds() / 1000;
        const bool changed = !presentation->active || scene->dirty ||
                             s->text_owner->input_signature != search_text_signature(*s, *scene) ||
                             (presentation->next_timestamp_update_unix_seconds &&
                              now_seconds >= presentation->next_timestamp_update_unix_seconds);
        if (changed && g_app.account_ui->text_measure_cache.backend) {
            const auto result = message_text_coordinator_converge(
                {scene, presentation, &g_app.account_ui->text_measure_cache,
                 &g_app.account_ui->chat_state, &s->bank->users, &g_app.account_ui->navigation_view,
                 &application_theme(), +[](void*, const Theme&) noexcept { return true; }, nullptr,
                 now_seconds, 1, nullptr});
            if (!result.ok() || !ui_search_scene_refresh_rich(&s->scene))
                return false;
        } else if (changed && !message_text_presentation_build(
                                   presentation, *scene, g_app.account_ui->chat_state, &s->bank->users,
                                   g_app.account_ui->navigation_view, application_theme(), now_seconds)
                                   .ok())
            return false;
        s->text_owner->input_signature = search_text_signature(*s, *scene);
        if (!renderer_bind_search_message_text_presentation(&g_app.renderer, presentation, &s->bank->users,
                                                            &g_app.account_ui->text_measure_cache))
            return false;
    } else
        renderer_unbind_search_message_text_presentation(&g_app.renderer);
    application_search_navigation_synchronize();
    if (s->panel && g_app.message_action_bar.active &&
        ui_rect_has_area(ui_rect_intersection(g_app.message_action_bar.rectangle, s->scene.panel)))
        message_action_bar_close(&g_app.message_action_bar);
    return true;
}
void application_search_release() noexcept {
    application_search_content_release();
    application_search_navigation_release();
    auto* s = g_app.search;
    if (!s)
        return;
    application_composer_text_input_suspend();
    cancel_submitted(*s);
    g_app.renderer.search_scene = nullptr;
    renderer_unbind_search_message_text_presentation(&g_app.renderer);
    const auto* rich_scene = ui_search_scene_message_scene(s->scene);
    if (g_app.context_focus == &s->scene.widgets ||
        (rich_scene && g_app.context_focus == &rich_scene->widgets))
        g_app.context_focus = nullptr;
    g_app.search = nullptr;
    if (s->captured && GetCapture() == g_app.window)
        ReleaseCapture();
    ui_search_scene_release(&s->scene);
    choices_release(*s);
    clear_poll_tickets(*s);
    if (s->recent)
        VirtualFree(s->recent, 0, MEM_RELEASE);
    message_composer_release(&s->edit);
    bank_release(s->bank);
    bank_release(s->candidate);
    if (s->text_owner)
        VirtualFree(s->text_owner, 0, MEM_RELEASE);
    VirtualFree(s, 0, MEM_RELEASE);
}
bool application_search_pending(u64 now) noexcept {
    const auto* s = g_app.search;
    return s && g_app.account_ui && s->panel && !s->submitted.request_id && context_current(*s) &&
           (s->controller.state == MessageSearchState::ready_to_submit ||
            ((s->controller.state == MessageSearchState::indexing ||
              s->controller.state == MessageSearchState::rate_limited) &&
             s->controller.retry_at_ms <= now));
}
bool application_search_submit(u64 id, u64 now) noexcept {
    if (!g_app.account.instance || !g_app.account.api.message_search || !application_search_pending(now))
        return false;
    auto& s = *g_app.search;
    MessageSearchToken token{};
    if (!message_search_submit(&s.controller, id, now, &token))
        return false;
    if (!capture_poll_tickets(s)) {
        message_search_submission_failed(&s.controller, token, false);
        clear_poll_tickets(s);
        repaint();
        return false;
    }
    const auto admission = g_app.account.api.message_search(
        g_app.account.instance, AccountMessageSearchOperation::submit, &s.controller.query, id);
    if (!admission.ok()) {
        clear_poll_tickets(s);
        s.admission_busy = admission.error == RestClientSubmitError::busy;
        message_search_submission_failed(&s.controller, token, s.admission_busy);
        repaint();
        return false;
    }
    s.admission_busy = false;
    s.submitted = token;
    s.submitted_cancelled = false;
    s.submitted_account_scope = s.account_scope;
    s.submitted_profile_id = s.profile_id;
    repaint();
    return true;
}
bool application_search_response(const RestClientResponse& response, Utf8View body) noexcept {
    auto* s = g_app.search;
    if (!s || !s->submitted.request_id || response.request_id != s->submitted.request_id)
        return false;
    if (!application_search_synchronize())
        return false;
    const auto token = s->submitted;
    s->submitted = {};
    s->submitted_cancelled = false;
    if (!context_current(*s) || s->submitted_account_scope != s->account_scope ||
        s->submitted_profile_id != s->profile_id || token.scope_id != s->scope_id ||
        s->controller.query.scope != s->scope || !g_app.account_ui || token.account_id != s->account_id ||
        token.account_id != g_app.account_ui->chat_state.self_user_id ||
        !message_search_accepts(&s->controller, token)) {
        clear_poll_tickets(*s);
        return true;
    }
    MessageSearchDecodeResult decoded{};
    bool valid = false;
    if (response.transport_ok() && response.status_code == 200) {
        bank_release(s->candidate);
        s->candidate = static_cast<SearchBank*>(
            VirtualAlloc(nullptr, sizeof(SearchBank), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        auto* scratch = static_cast<SearchDecodeScratch*>(
            VirtualAlloc(nullptr, sizeof(SearchDecodeScratch), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        auto* text = body.bytes && body.bytes <= search_bank_text_limit
                         ? static_cast<char*>(
                               VirtualAlloc(nullptr, body.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                         : nullptr;
        if (s->candidate && scratch && text && bank_initialize(*s->candidate)) {
            decoded = decode_message_search_page(
                body, s->controller.query,
                {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount},
                {g_app.account_ui->message_history_object_structurals,
                 kMessageHistoryObjectStructuralCount},
                64, {text, body.bytes}, {s->candidate->groups, kMessageSearchMaximumGroups},
                {s->candidate->messages, kMessageSearchMaximumMessages}, decode_scratch(*scratch),
                {s->candidate, bank_accept});
            valid = decoded.ok();
            for (u32 i = 0; valid && i < decoded.page.message_count; ++i) {
                const auto& item = s->candidate->messages[i];
                const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                                       item.message.channel_id);
                valid = s->scope == MessageSearchScope::guild
                            ? (!item.guild_known || item.guild_id == s->scope_id) &&
                                  (channel ? channel->occupied && channel->guild_id == s->scope_id
                                           : item.guild_known)
                            : (!item.guild_known || item.guild_id == 0) && channel && channel->occupied &&
                                  channel->id == s->scope_id && channel_is_private(channel->type);
                if (!valid)
                    decoded.error = MessageSearchDecodeError::guild_mismatch;
            }
            if (!valid && (decoded.error == MessageSearchDecodeError::guild_mismatch ||
                           decoded.error == MessageSearchDecodeError::channel_mismatch))
                s->validation =
                    utf8_literal("Results are out of date. Search again.");
        }
        if (scratch)
            VirtualFree(scratch, 0, MEM_RELEASE);
        if (text)
            VirtualFree(text, 0, MEM_RELEASE);
        if (valid && search_publication_counter == 0x03ffffffu) {
            valid = false;
            s->validation =
                utf8_literal("Search is unavailable. Restart MaiCord to search again.");
        }
        if (valid) {
            auto& b = *s->candidate;
            b.publication = ++search_publication_counter;
            for (u32 i = 0; i < b.store.count; ++i) {
                auto& r = b.records[b.order[i]];
                if (message_rich_store_find(b.rich, r.id))
                    message_rich_store_advance_revision(&b.rich, r.id, r.revision, b.publication);
                r.revision = b.publication;
            }
            for (u32 i = 0; i < decoded.page.message_count; ++i) {
                const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                                       b.messages[i].message.channel_id);
                b.channel_generations[i] = channel ? channel->generation : 0;
            }
            b.action_epoch = s->action_epoch;
            b.account_scope = s->account_scope;
            b.profile_id = s->profile_id;
            b.self_id = s->account_id;
            b.account_instance = g_app.account.instance;
            b.account_ui = g_app.account_ui;
            valid = ui_variable_list_initialize(&b.list, {b.heights, kMessageSearchMaximumMessages},
                                                {b.fenwick, kMessageSearchMaximumMessages},
                                                decoded.page.message_count, ui_extent_units(80));
        }
    }
    const auto now = clock_monotonic_milliseconds();
    const auto retry =
        maximum(response.rate.retry_after_ms, response.retry_at_ms > now ? response.retry_at_ms - now : 0);
    if (valid && !publish_polls(*s, *s->candidate, decoded.page.message_count)) {
        valid = false;
        s->validation = utf8_literal("Couldn't load poll results. Search again.");
    }
    if (s->candidate)
        message_poll_store_release(&s->candidate->staged_polls);
    clear_poll_tickets(*s);
    // A tabs reply whose envelope cannot be read is treated like a missing route.
    const auto envelope = decoded.error;
    const bool unreadable = !valid && response.transport_ok() && response.status_code == 200 &&
                            (envelope == MessageSearchDecodeError::scan_failed ||
                             envelope == MessageSearchDecodeError::invalid_json ||
                             envelope == MessageSearchDecodeError::duplicate_field ||
                             envelope == MessageSearchDecodeError::missing_field);
    if (!unreadable || !message_search_fallback(&s->controller, token))
        message_search_complete(&s->controller, token, response.status_code, response.transport_ok(), valid,
                                valid ? &decoded.page : nullptr, now, retry);
    s->tabs_unavailable |= s->controller.endpoint_fallback;
    if (valid && s->controller.state == MessageSearchState::ready) {
        const auto& query = s->controller.query;
        if (query.tab_counts && !query.offset) {
            s->tab_known = decoded.page.tab_known;
            for (u32 i = 0; i < kMessageSearchTabCount; ++i)
                s->tab_totals[i] = decoded.page.tab_totals[i];
        }
        s->result_tab = query.tab;
        s->tab_known |= static_cast<u8>(1u << static_cast<u8>(query.tab));
        s->tab_totals[static_cast<u8>(query.tab)] = decoded.page.total_results;
        s->candidate->page = decoded.page;
        decorate(*s->candidate);
        auto* old = s->bank;
        s->bank = s->candidate;
        s->candidate = old;
        application_search_content_release();
        application_search_navigation_synchronize();
        ++s->source.revision;
        s->scene.scroll_dips = 0;
    }
    return repaint();
}
u64 application_search_next_wake() noexcept {
    const auto* s = g_app.search;
    if (!s || !s->panel || !context_current(*s) || s->submitted.request_id || s->admission_busy ||
        g_app.message_history_request_owner != MessageHistoryRequestOwner::none)
        return 0;
    return s->controller.state == MessageSearchState::ready_to_submit
               ? 1
               : message_search_next_wake(&s->controller);
}
u64 application_search_edit_next_wake() noexcept {
    return application_search_edit_focused() ? g_app.search->blink_at : 0;
}
void application_search_tick(u64 now) noexcept {
    auto* s = g_app.search;
    if (s && application_search_edit_focused() && s->blink_at && now >= s->blink_at) {
        s->caret_visible = !s->caret_visible;
        s->blink_at = deadline_after(now, kDefaultCaretBlinkMilliseconds);
        repaint();
    }
}
bool application_search_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto* s = g_app.search;
    if (s && ui_input_consume_key_tail(&s->held_key, {kind, key, repeated}))
        return true;
    if (!front())
        return false;
    const bool raw = (GetKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0,
               control = raw && !alt, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (kind == UiInputKind::key_down && shortcut_matches(ShortcutAction::search, shortcut_chord_from_keyboard(key))) {
        if (!s) {
            if (!application_search_synchronize())
                return true;
            s = g_app.search;
        }
        current_scope(*s);
        ApplicationThreadBrowserSearchScope threads{};
        if (application_thread_browser_search_scope(&threads)) {
            // The open thread browser is the focused pane: the shortcut
            // scopes the field to its threads and selects the last query so
            // typing replaces it. Shift widens this session to the server.
            s->widened_thread_session = shift ? threads.session : 0;
            if (shift)
                message_composer_clear(&s->edit);
            else
                message_composer_select_all(&s->edit);
            s->suggestions = shift;
            s->filters = s->sort_menu = false;
            focus(*s, true);
            s->held_key = key;
            repaint();
            return true;
        }
        if (!shift && s->scope == MessageSearchScope::guild) {
            char prefill[32] = {'i', 'n', ':'};
            const auto n = decimal_write(prefill + 3, g_app.account_ui->active_channel_id);
            prefill[n + 3] = ' ';
            message_composer_restore(&s->edit, 0, {prefill, n + 4}, n + 4, n + 4);
        } else if (shift)
            message_composer_clear(&s->edit);
        s->panel = true;
        s->suggestions = true;
        s->filters = s->sort_menu = false;
        focus(*s, true);
        s->held_key = key;
        repaint();
        return true;
    }
    auto* rich_scene = s ? ui_search_scene_message_scene(&s->scene) : nullptr;
    const bool rich_focused = rich_scene && g_app.context_focus == &rich_scene->widgets;
    if (!s || (!s->focused && g_app.context_focus != &s->scene.widgets && !rich_focused))
        return false;
    if (kind == UiInputKind::character) {
        if (!s->focused || control || (alt && !raw) || key < 32 || key == 127 || key == 0x2028 ||
            key == 0x2029)
            return true;
        if (message_composer_insert_utf16(&s->edit, static_cast<u16>(key),
                                          clock_monotonic_milliseconds()) == MessageComposerResult::changed)
            application_search_edit_changed(true);
        return true;
    }
    if (kind == UiInputKind::key_up)
        return true;
    if (kind != UiInputKind::key_down)
        return false;
    if (s->focused && !raw && !alt && !shift && application_composer_text_input_composing() &&
        (key == VK_UP || key == VK_DOWN || key == VK_RETURN || key == VK_TAB))
        return true;
    if (s->focused && !raw && !alt && !shift && s->suggestions && s->choices &&
        s->choices->filter.kind != UiSearchFilterKind::none) {
        if (application_composer_text_input_composing() &&
            (key == VK_UP || key == VK_DOWN || key == VK_RETURN || key == VK_TAB))
            return true;
        const bool current_choices = choices_current(s);
        if (current_choices) {
            if (key == VK_UP || key == VK_DOWN) {
                choice_move(*s, key == VK_UP);
                return true;
            }
            if ((key == VK_RETURN || key == VK_TAB) &&
                s->choices->source.selected < s->choices->source.rows.count) {
                s->held_key = key;
                choice_accept(*s, s->choices->source.revision, s->choices->source.selected);
                return true;
            }
            if (key == VK_TAB) {
                // An empty/unselected chooser has no completion to accept.
                // Dismiss it before the ordinary editor-to-control focus move.
                s->suggestions = false;
                repaint();
            }
        }
        if (!current_choices && (key == VK_UP || key == VK_DOWN || key == VK_RETURN || key == VK_TAB))
            return true;
    }
    if (control && key == 'S' && rich_focused) {
        auto target = ui_search_scene_rich_widget_target(s->scene, rich_scene->widgets.focused);
        if (target.action == UiSearchAction::rich_item &&
            target.rich_kind == MessageSceneRichItemKind::attachment) {
            target.save = true;
            activate(*s, target);
        }
        s->held_key = key;
        return true;
    }
    if (key == VK_ESCAPE) {
        s->held_key = key;
        if (s->filters || s->suggestions || s->sort_menu) {
            s->filters = s->suggestions = s->sort_menu = false;
            repaint();
        } else
            activate(*s, {UiSearchAction::close, 0, 0});
        return true;
    }
    if (key == VK_RETURN) {
        s->held_key = key;
        if (s->focused)
            commit(*s);
        else {
            const auto h = s->scene.widgets.focused;
            if (rich_focused)
                activate(*s, ui_search_scene_rich_widget_target(s->scene, rich_scene->widgets.focused));
            else if (h.generation && h.slot < s->scene.storage.widgets.count)
                activate(*s, s->scene.targets[h.slot]);
        }
        return true;
    }
    if (key == VK_SPACE && !s->focused) {
        s->held_key = key;
        const auto h = s->scene.widgets.focused;
        if (rich_focused)
            activate(*s, ui_search_scene_rich_widget_target(s->scene, rich_scene->widgets.focused));
        else if (h.generation && h.slot < s->scene.storage.widgets.count)
            activate(*s, s->scene.targets[h.slot]);
        return true;
    }
    if (key == VK_TAB) {
        if (shift && s->focused && s->choices && s->suggestions &&
            s->choices->filter.kind != UiSearchFilterKind::none) {
            s->suggestions = false;
            repaint();
        }
        if (rich_focused) {
            const auto before = rich_scene->widgets.focused;
            const auto target = ui_search_scene_rich_widget_target(s->scene, before);
            const auto after = ui_widget_manager_move_focus(&rich_scene->widgets, shift);
            if (after.generation && before.generation &&
                (shift ? after.slot >= before.slot : after.slot <= before.slot)) {
                if (target.source_index < s->scene.measured_count) {
                    ui_search_scene_scroll(
                        &s->scene,
                        maximum(s->scene.scroll_dips, s->scene.row_offsets[target.source_index + 1] -
                                                          ui_rect_height(s->scene.body)));
                    repaint();
                }
                for (u32 i = 0; i < s->scene.widgets.count; ++i)
                    if (s->scene.targets[i].action == UiSearchAction::result &&
                        s->scene.targets[i].source_index == target.source_index) {
                        ui_widget_manager_focus(&s->scene.widgets,
                                                {static_cast<u16>(i), s->scene.widgets.generation});
                        g_app.context_focus = &s->scene.widgets;
                        break;
                    }
            }
            application_request_frame();
            return true;
        }
        if (s->filters || s->suggestions || s->sort_menu) {
            const auto start = s->scene.widgets.focused.slot;
            u32 found = start;
            for (u32 attempt = 0; attempt < s->scene.widgets.count; ++attempt) {
                found = shift ? (found ? found - 1 : s->scene.widgets.count - 1)
                              : (found + 1) % s->scene.widgets.count;
                const auto action = s->scene.targets[found].action;
                const bool filter =
                    ui_search_action_filter(action) || action == UiSearchAction::more_filters ||
                    action == UiSearchAction::clear_recent || action == UiSearchAction::recent;
                const bool sort = action == UiSearchAction::newest || action == UiSearchAction::oldest ||
                                  action == UiSearchAction::relevance;
                if ((s->sort_menu && sort) || (!s->sort_menu && filter)) {
                    focus(*s, false);
                    g_app.context_focus = &s->scene.widgets;
                    ui_widget_manager_focus(&s->scene.widgets,
                                            {static_cast<u16>(found), s->scene.widgets.generation});
                    repaint();
                    break;
                }
            }
            return true;
        }
        focus(*s, false);
        const auto h = ui_widget_manager_move_focus(&s->scene.widgets, shift);
        if (h.generation && s->scene.targets[h.slot].action == UiSearchAction::edit)
            focus(*s, true);
        g_app.context_focus = &s->scene.widgets;
        repaint();
        return true;
    }
    if (!s->focused && (key == VK_LEFT || key == VK_RIGHT)) {
        const auto h = s->scene.widgets.focused;
        if (h.generation && h.slot < s->scene.storage.widgets.count &&
            s->scene.targets[h.slot].action == UiSearchAction::tab) {
            auto target = s->scene.targets[h.slot];
            target.source_index =
                (target.source_index + (key == VK_LEFT ? kMessageSearchTabCount - 1 : 1)) %
                kMessageSearchTabCount;
            activate(*s, target);
        }
        return true;
    }
    if (!s->focused)
        return true;
    const u32 revision = s->edit.text_revision;
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (control && key == 'A')
        result = message_composer_select_all(&s->edit);
    else if (control && (key == 'C' || key == 'X')) {
        const auto first = minimum(s->edit.anchor, s->edit.caret),
                   last = maximum(s->edit.anchor, s->edit.caret);
        if (last > first && clipboard_write_utf8(g_app.window, {s->edit.text + first, last - first}) &&
            key == 'X')
            result = message_composer_replace_range(&s->edit, first, last, {});
    } else if (control && key == 'V') {
        char text[kMessageSearchContentBytes];
        const auto read = clipboard_read_utf8(g_app.window, {text, sizeof(text)});
        bool line = read.ok();
        for (u32 i = 0; line && i < read.bytes; ++i)
            line = static_cast<u8>(text[i]) >= 32;
        if (line)
            result = message_composer_insert_utf8(&s->edit, {text, read.bytes});
    } else if (control && key == 'Z')
        result = shift ? message_composer_redo(&s->edit) : message_composer_undo(&s->edit);
    else if (control && key == 'Y')
        result = message_composer_redo(&s->edit);
    else if (key == VK_BACK) {
        s->held_key = key;
        // Backspace in the empty field removes the scope chip, as with
        // other chip inputs; a held key never reaches past the query.
        if (!repeated && !s->edit.bytes && thread_scope(*s, nullptr))
            return activate(*s, {UiSearchAction::scope, 0, 0});
        result =
            control ? message_composer_delete_word(&s->edit, false) : message_composer_backspace(&s->edit);
    } else if (key == VK_DELETE)
        result = control ? message_composer_delete_word(&s->edit, true)
                         : message_composer_delete_forward(&s->edit);
    else if (key == VK_LEFT || key == VK_RIGHT)
        result = message_composer_move(
            &s->edit,
            control ? (key == VK_LEFT ? MessageComposerMove::previous_word : MessageComposerMove::next_word)
                    : (key == VK_LEFT ? MessageComposerMove::previous : MessageComposerMove::next),
            shift);
    else if (key == VK_HOME || key == VK_END)
        result = message_composer_move(
            &s->edit,
            key == VK_HOME ? MessageComposerMove::document_home : MessageComposerMove::document_end, shift);
    if (result == MessageComposerResult::changed)
        application_search_edit_changed(revision != s->edit.text_revision);
    return true;
}
bool application_search_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* s = g_app.search;
    if (!s)
        return false;
    if (message == WM_KILLFOCUS) {
        s->suggestions = s->filters = s->sort_menu = false;
        focus(*s, false);
        s->dragging = false;
        s->captured = false;
        s->scrollbar_dragging = false;
        s->pressed = {};
        ui_widget_manager_capture(&s->scene.widgets, ui_widget_invalid_handle());
        if (GetCapture() == g_app.window)
            ReleaseCapture();
        return false;
    }
    if (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lparam) != g_app.window) {
        s->dragging = false;
        s->captured = false;
        s->scrollbar_dragging = false;
        s->pressed = {};
        ui_widget_manager_capture(&s->scene.widgets, ui_widget_invalid_handle());
        return false;
    }
    if (!front())
        return false;
    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (message == WM_SETCURSOR) {
        if (LOWORD(lparam) != HTCLIENT || !GetCursorPos(&point) || !ScreenToClient(g_app.window, &point))
            return false;
    }
    if (message == WM_MOUSEWHEEL || message == WM_CONTEXTMENU) {
        if (message == WM_CONTEXTMENU && lparam == -1)
            return g_app.context_focus == &s->scene.widgets;
        ScreenToClient(g_app.window, &point);
    }
    const float scale = g_app.renderer.dpi ? 96.f / g_app.renderer.dpi : 1;
    const float x = point.x * scale, y = point.y * scale;
    const bool inside = ui_search_scene_contains(s->scene, x, y);
    const auto hit = ui_search_scene_hit_test(s->scene, x, y);
    if (message == WM_LBUTTONDBLCLK && hit.action != UiSearchAction::edit)
        message = WM_LBUTTONDOWN;
    if (message == WM_SETCURSOR && inside) {
        application_set_cursor(ui_search_scene_cursor(s->scene, x, y));
        return true;
    }
    if (message == WM_MOUSELEAVE) {
        if (ui_widget_manager_hover(&s->scene.widgets, ui_widget_invalid_handle()))
            application_request_frame();
        return false;
    }
    if (message == WM_MOUSEWHEEL && inside) {
        if (choices_current(s)) {
            choice_move(*s, static_cast<short>(HIWORD(wparam)) > 0);
            return true;
        }
        if (s->panel && ui_rect_contains(s->scene.body, x, y) && !s->filters && !s->suggestions &&
            !s->sort_menu) {
            const i32 delta = static_cast<short>(HIWORD(wparam));
            u32 lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            ui_scroll_host_wheel(&g_app.scroll_host,
                                 {s, s->edit.document_identity, scroll_read, scroll_write}, {x, y}, delta,
                                 lines, lines == WHEEL_PAGESCROLL,
                                 clock_microseconds(g_app.clock, 0, clock_now()), scale);
            application_request_frame();
        }
        return true;
    }
    if (message == WM_LBUTTONDOWN) {
        if (!inside) {
            const bool popup = s->suggestions || s->filters || s->sort_menu;
            s->suggestions = s->filters = s->sort_menu = false;
            if (s->focused) {
                focus(*s, false);
                repaint();
            }
            if (popup) {
                repaint();
                return true;
            }
            return false;
        }
        if (hit.action == UiSearchAction::none && (s->suggestions || s->filters || s->sort_menu)) {
            s->suggestions = s->filters = s->sort_menu = false;
            focus(*s, false);
            s->pressed = {};
            repaint();
            return true;
        }
        SetFocus(g_app.window);
        if (hit.action == UiSearchAction::scope) {
            // The chip acts on press, like the field it sits in: the press
            // itself would blur the field and retire a release target.
            s->pressed = {};
            activate(*s, hit);
            return true;
        }
        s->pressed = hit;
        s->pressed_revision = s->scene.revision;
        const auto scrollbar =
            ui_scrollbar_geometry(s->scene.body, s->scene.scroll_dips,
                                  ui_rect_height(s->scene.body) + ui_search_scene_scroll_maximum(s->scene));
        if (s->panel && !s->filters && !s->suggestions && !s->sort_menu &&
            ui_scrollbar_track_hit(scrollbar, x, y)) {
            focus(*s, false);
            s->pressed = {};
            s->scrollbar_dragging = ui_scrollbar_thumb_hit(scrollbar, x, y);
            s->thumb_grab = y - scrollbar.thumb.top;
            if (!s->scrollbar_dragging)
                ui_search_scene_scroll(&s->scene,
                                       s->scene.scroll_dips + (y < scrollbar.thumb.top ? -1 : 1) *
                                                                  ui_rect_height(s->scene.body));
            s->captured = true;
            SetCapture(g_app.window);
            repaint();
            return true;
        }
        if (hit.action == UiSearchAction::edit) {
            s->filters = false;
            s->suggestions = true;
            s->sort_menu = false;
            focus(*s, true);
            // A third press after a double click selects the whole query.
            const bool triple = application_click_count(&s->edit, x, y, false) == 3;
            u32 position = 0;
            if (triple)
                message_composer_select_all(&s->edit);
            else if (text_edit_layout_hit_test(g_app.renderer.search_layout, x, y, &position)) {
                u32 byte = 0;
                if (utf8_byte_offset_for_utf16_position(message_composer_text(s->edit), position, &byte))
                    message_composer_set_selection(&s->edit, byte, byte);
            }
            s->dragging = !triple;
            application_search_edit_changed(false);
        } else if (hit.action == UiSearchAction::filter_value) {
            // The chooser is part of the current text edit; preserve its TSF
            // target and selection throughout the pointer capture.
            focus(*s, true);
            if (choices_current(s) && hit.source_revision == s->choices->source.revision &&
                hit.source_index < s->choices->source.rows.count) {
                s->choices->source.selected = static_cast<u16>(hit.source_index);
                ++s->content.revision;
                repaint();
            }
        } else {
            focus(*s, false);
            const auto h = ui_widget_manager_hit_test(s->scene.widgets, x, y);
            if (h.generation) {
                ui_widget_manager_focus(&s->scene.widgets, h);
                ui_widget_manager_capture(&s->scene.widgets, h);
            }
            auto* rich = ui_search_scene_message_scene(&s->scene);
            if (hit.action == UiSearchAction::rich_item && !hit.save && rich) {
                const auto native = ui_widget_manager_hit_test(rich->widgets, x, y);
                if (native.generation) {
                    ui_widget_manager_focus(&rich->widgets, native);
                    g_app.context_focus = &rich->widgets;
                } else
                    g_app.context_focus = &s->scene.widgets;
            } else
                g_app.context_focus = &s->scene.widgets;
            application_request_frame();
        }
        s->pressed_revision = s->scene.revision;
        s->captured = true;
        SetCapture(g_app.window);
        return true;
    }
    if (message == WM_MOUSEMOVE && s->scrollbar_dragging) {
        const auto geometry =
            ui_scrollbar_geometry(s->scene.body, s->scene.scroll_dips,
                                  ui_rect_height(s->scene.body) + ui_search_scene_scroll_maximum(s->scene));
        ui_search_scene_scroll(&s->scene, ui_scrollbar_scroll_from_pointer(geometry, y, s->thumb_grab));
        repaint();
        return true;
    }
    if (message == WM_MOUSEMOVE && s->dragging) {
        u32 position = 0, byte = 0;
        if (text_edit_layout_hit_test(g_app.renderer.search_layout, x, y, &position) &&
            utf8_byte_offset_for_utf16_position(message_composer_text(s->edit), position, &byte)) {
            message_composer_set_selection(&s->edit, s->edit.anchor, byte);
            application_search_edit_changed(false);
        }
        return true;
    }
    if (message == WM_MOUSEMOVE) {
        const auto h =
            inside ? ui_widget_manager_hit_test(s->scene.widgets, x, y) : ui_widget_invalid_handle();
        if (ui_widget_manager_hover(&s->scene.widgets, h))
            application_request_frame();
    }
    if (message == WM_LBUTTONUP && (s->captured || inside)) {
        const auto pressed = s->pressed;
        const bool drag = s->dragging || s->scrollbar_dragging;
        s->dragging = false;
        s->captured = false;
        s->scrollbar_dragging = false;
        s->pressed = {};
        ui_widget_manager_capture(&s->scene.widgets, ui_widget_invalid_handle());
        if (GetCapture() == g_app.window)
            ReleaseCapture();
        if (!drag && s->pressed_revision == s->scene.revision && ui_search_target_equal(pressed, hit))
            activate(*s, pressed);
        return true;
    }
    if (message == WM_LBUTTONDBLCLK && hit.action == UiSearchAction::edit) {
        focus(*s, true);
        application_click_count(&s->edit, x, y, true);
        message_composer_select_word(&s->edit, s->edit.caret);
        application_search_edit_changed(false);
        return true;
    }
    return inside && (message == WM_MOUSEMOVE || message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN ||
                      message == WM_RBUTTONUP || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
                      message == WM_LBUTTONDBLCLK);
}
} // namespace maicord::app_detail
