#include "search_internal.hpp"
#include "../composer/composer.hpp"
#include "../composer/text_input.hpp"
#include "../../account/friends.hpp"
#include "maicord/ui/chat/search/query.hpp"
#include "maicord/model/user/friend_search.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/os/local_time.hpp"
#include "maicord/core/scratch_pages.hpp"

namespace maicord::app_detail {
namespace search_detail {

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
    reset_value(&s.source.rich_order);
    retire_actions(s);
    s.panel = !id && s.panel;
    focus(s, false);
    s.source.count = 0;
    ++s.source.revision;
    release_pointer(s);
    message_composer_clear(&s.edit);
    message_composer_refresh_borrowed(&s.edit, 0);
    s.scope = kind;
    s.scope_id = id;
    s.account_id = user;
    s.profile_id = profile;
    s.account_scope = epoch;
    s.admission_busy = false;
    menus_close(s);
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

} // namespace search_detail
namespace {

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

} // namespace
namespace search_detail {

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
bool channel_usable(const ChannelRecord& c) noexcept {
    return c.occupied && channel_is_message_capable(c.type) && !(c.flags & kChannelObfuscatedFlag);
}
// A guild channel whose permissions do not yet let this user view and read it.
bool channel_view_denied(const ChatStateStore& state, const ChannelRecord& c) noexcept {
    constexpr u32 required = channel_action_view | channel_action_read_history;
    return !c.permissions_resolved || (c.actions & required) != required ||
           !chat_state_store_channel_visible(state, c);
}

} // namespace search_detail
namespace {

bool choice_channel(const ApplicationSearch& s, const ChannelRecord& c) noexcept {
    // The record predicate already covers the guild, permissions and thread access.
    if (!application_channel_record_readable(g_app.account_ui->chat_state, c))
        return false;
    return s.scope == MessageSearchScope::channel ? c.id == s.scope_id : c.guild_id && c.guild_id == s.scope_id;
}
bool choice_user_allowed(u64 id) noexcept {
    const auto* relationships = application_friends_relationships();
    const auto* record = relationships ? relationship_store_find(*relationships, id) : nullptr;
    return id && (!record || (record->type != RelationshipType::blocked && !record->ignored));
}
Utf8View choice_copy(SearchFilterChoices& b, char* output, u32 capacity, const Utf8View& text) noexcept {
    if (!utf8_validate(text).valid)
        return {};
    const u32 bytes = utf8_truncated_length(text, capacity);
    if (bytes) {
        copy_bytes(output, text.data, bytes);
        ++b.copies;
    }
    return {output, bytes};
}

bool choice_capacity(SearchFilterChoices& b, const Utf8View& value) noexcept {
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
void choice_add(SearchFilterChoices& b, u64 id, u8 rank, const Utf8View& label, const Utf8View& detail,
                const Utf8View& value, u32 semantic = 0, const Utf8View& avatar = {}) noexcept {
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
    const u32 shifted = count - position - 1;
    if (shifted) {
        copy_bytes(b.rows + position + 1, b.rows + position, shifted * sizeof(b.rows[0]));
        copy_bytes(b.ranks + position + 1, b.ranks + position, shifted * sizeof(b.ranks[0]));
        copy_bytes(b.labels + position + 1, b.labels + position, shifted * sizeof(b.labels[0]));
        copy_bytes(b.details + position + 1, b.details + position, shifted * sizeof(b.details[0]));
        copy_bytes(b.values + position + 1, b.values + position, shifted * sizeof(b.values[0]));
        copy_bytes(b.avatars + position + 1, b.avatars + position, shifted * sizeof(b.avatars[0]));
        b.copies += shifted;
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
bool choice_match(const Utf8View& prefix, const Utf8View& name, const Utf8View& alias,
                  const Utf8View& value) noexcept {
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
    const auto prefix = without_prefix(b.filter.prefix(), '@');
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
// A new source revision; when the counter is exhausted the chooser publishes no rows.
void choices_publish(SearchFilterChoices& b) noexcept {
    if (filter_publication_counter != ~u32{0}) {
        b.source.revision = ++filter_publication_counter;
        return;
    }
    b.source.revision = ~u32{0};
    reset_value(&b.source.rows);
    b.source.selected = kUiSearchFilterNoSelection;
    b.source.hint = utf8_literal("Suggestions unavailable. Type a value.");
}

} // namespace
namespace search_detail {

bool choices_synchronize(ApplicationSearch& s) noexcept {
    if (!s.focused || !s.suggestions || !front() || !context_current(s) ||
        application_composer_text_input_composing()) {
        member_query_withdraw(s, true);
        return false;
    }
    auto stamp = choices_stamp(s);
    if (s.choices && s.choices->initialized && stamp == s.choices->stamp)
        return false;
    const auto& filter =
        ui_search_filter_context(message_composer_text(s.edit), s.edit.anchor, s.edit.caret);
    if (filter.kind == UiSearchFilterKind::none) {
        member_query_withdraw(s);
        if (s.choices) {
            copy_value(&s.choices->filter, filter);
            s.choices->stamp = stamp;
        }
        return false;
    }
    if (!s.choices)
        s.choices = commit_pages<SearchFilterChoices>();
    if (!s.choices)
        return false;
    auto& b = *s.choices;
    // Selection/caret changes still invalidate every captured row, while
    // identical matching inputs keep the existing choices.
    const u32 stamp_anchor = stamp.anchor, stamp_caret = stamp.caret;
    stamp.anchor = b.stamp.anchor;
    stamp.caret = b.stamp.caret;
    const bool same_inputs =
        b.initialized && stamp == b.stamp && b.filter.kind == filter.kind &&
        b.filter.token_begin == filter.token_begin && b.filter.token_end == filter.token_end &&
        b.filter.value_begin == filter.value_begin && b.filter.value_end == filter.value_end &&
        utf8_equal(b.filter.prefix(), filter.prefix());
    stamp.anchor = stamp_anchor;
    stamp.caret = stamp_caret;
    const bool preserve_selection =
        b.initialized && b.source.selected < b.source.rows.count && b.stamp.document == stamp.document &&
        b.stamp.text == stamp.text && b.stamp.anchor == stamp.anchor && b.stamp.caret == stamp.caret &&
        b.filter.kind == filter.kind && utf8_equal(b.filter.prefix(), filter.prefix());
    const u64 selected_id = preserve_selection ? b.rows[b.source.selected].id : 0;
    const u32 selected_semantic = preserve_selection ? b.rows[b.source.selected].semantic : 0;
    copy_value(&b.stamp, stamp);
    copy_value(&b.filter, filter);
    if (same_inputs) {
        choices_publish(b);
        return true;
    }
    b.initialized = true;
    b.capacity_limited = false;
    ++b.rebuilds;
    reset_value(&b.source);
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
            prefix = without_prefix(prefix, '-');
        if (kind == UiSearchFilterKind::file_type)
            prefix = without_prefix(prefix, '.');
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
        const auto prefix = without_prefix(filter.prefix(), '#');
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
            const auto query = without_prefix(b.filter.prefix(), '@');
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
    choices_publish(b);
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
    if (!edit_replace(s, b.filter.value_begin, b.filter.value_end, {replacement, bytes}))
        return false;
    reset_value(&s.validation);
    menus_close(s);
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
    name = without_prefix(name, '@');
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
    name = without_prefix(name, '#');
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
    CalendarTime local{};
    for (u32 i = 0; i < 4; ++i)
        local.wYear = static_cast<u16>(local.wYear * 10 + date.data[i] - '0');
    local.wMonth = static_cast<u16>((date.data[5] - '0') * 10 + date.data[6] - '0');
    local.wDay = static_cast<u16>((date.data[8] - '0') * 10 + date.data[9] - '0');
    // The day's first instant, and the next day's: the next day is found at local noon, which a daylight shift
    // never moves across midnight.
    i64 start_seconds = 0, noon_seconds = 0, end_seconds = 0;
    if (!local_time_to_unix(local, &start_seconds))
        return false;
    local.wHour = 12;
    CalendarTime next{};
    if (!local_time_to_unix(local, &noon_seconds) || !local_time_from_unix(noon_seconds + 86'400, &next))
        return false;
    next.wHour = next.wMinute = next.wSecond = next.wMilliseconds = 0;
    if (!local_time_to_unix(next, &end_seconds))
        return false;
    constexpr i64 snowflake_epoch_ms = static_cast<i64>(kSnowflakeEpochMilliseconds);
    const i64 start = start_seconds * 1000, end = end_seconds * 1000;
    if (end <= snowflake_epoch_ms)
        return false;
    *first = start > snowflake_epoch_ms ? static_cast<u64>(start - snowflake_epoch_ms) : 0;
    *last = static_cast<u64>(end - snowflake_epoch_ms);
    return *last > *first;
}
void recent_load(ApplicationSearch& s) noexcept {
    if (!s.profile_id || s.recent_profile == s.profile_id)
        return;
    if (!s.recent)
        s.recent = commit_pages<SearchRecentList>();
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

} // namespace search_detail
} // namespace maicord::app_detail
