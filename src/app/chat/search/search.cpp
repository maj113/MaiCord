#include "search_internal.hpp"
#include "../../shell/overlay_input.hpp"
#include "../../voice/call.hpp"
#include "../composer/composer.hpp"
#include "../composer/text_input.hpp"
#include "../messages/components.hpp"
#include "content.hpp"
#include "navigation.hpp"
#include "maicord/ui/chat/messages/text_coordinator.hpp"
#include "maicord/model/chat/forwarded_input.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/app/chat/search/account_message_search.hpp"
#include "../polls/poll.hpp"
#include "maicord/ui/chat/polls/controls.hpp"

namespace maicord::app_detail {
namespace {

constexpr u32 search_bank_text_limit = kRestClientMaximumResponseBytes;
constexpr u32 search_user_limit =
    kMessageSearchMaximumMessages * (2 * kGatewayMessageMentionUserMaximum + 3);

bool text_reserve(SearchTextBanks& storage, u32 bank, u32 bytes) noexcept {
    if (bytes > search_bank_text_limit)
        return false;
    if (!storage.data)
        storage.data = static_cast<u8*>(
            pages_reserve(2ull * search_bank_text_limit));
    if (!storage.data)
        return false;
    const u32 rounded = (bytes + 4095u) & ~4095u;
    if (rounded > storage.committed[bank]) {
        if (!pages_commit_at(storage.data + bank * search_bank_text_limit + storage.committed[bank], rounded - storage.committed[bank]))
            return false;
        storage.committed[bank] = rounded;
    }
    return true;
}
u32 search_publication_counter{};

} // namespace

static_assert(sizeof(SearchFilterChoices) <= 4096);
static_assert(sizeof(ApplicationSearch) <= 8192);

namespace search_detail {

bool text_reserve_both(SearchTextBanks& storage, u32 bytes) noexcept {
    return text_reserve(storage, 0, bytes) && text_reserve(storage, 1, bytes);
}
u32 filter_publication_counter{};
Utf8View without_prefix(const Utf8View& value, char prefix) noexcept {
    return value.bytes && value.data[0] == prefix ? Utf8View{value.data + 1, value.bytes - 1} : value;
}
__declspec(noinline) void menus_close(ApplicationSearch& s) noexcept {
    s.filters = s.suggestions = s.sort_menu = false;
}
__declspec(noinline) void suggestions_open(ApplicationSearch& s) noexcept {
    s.filters = s.sort_menu = false;
    s.suggestions = true;
}
// Replaces a range of the query as one undoable edit.
bool edit_replace(ApplicationSearch& s, u32 begin, u32 end, const Utf8View& text) noexcept {
    if (!s.edit.history && !message_composer_enable_history(&s.edit, 4096))
        return false;
    message_composer_break_history_run(&s.edit);
    if (message_composer_replace_range(&s.edit, begin, end, text) != MessageComposerResult::changed)
        return false;
    message_composer_break_history_run(&s.edit);
    return true;
}
void member_query_withdraw(ApplicationSearch& s, bool invalidate_choices) noexcept {
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
        pages_release(s.choices);
    s.choices = nullptr;
}

} // namespace search_detail
namespace {

void clear_poll_tickets(ApplicationSearch& s) noexcept {
    if (s.submitted_poll_tickets)
        pages_release(s.submitted_poll_tickets);
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
        commit_pages(usize{store.count} * sizeof(MessagePollResultTicket)));
    if (!s.submitted_poll_tickets)
        return false;
    s.submitted_poll_count = store.count;
    for (u32 i = 0; i < store.count; ++i)
        s.submitted_poll_tickets[i] = {store.records[i].message_id, s.submitted_poll_gateway_serial,
                                       store.records[i].content_revision};
    return true;
}

} // namespace
namespace search_detail {

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
        pages_release(bank->rich_text.data);
    if (bank->user_names.data)
        pages_release(bank->user_names.data);
    pages_release(bank);
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
        !text_reserve_both(b.rich_text, 1) ||
        !text_reserve(b.user_names, 0, 1) ||
        !stable_storage_initialize(&b.user_records, search_user_limit, 16, sizeof(UserRecord),
                                   alignof(UserRecord), true) ||
        !stable_storage_initialize(&b.user_order, search_user_limit, 16, sizeof(u32), alignof(u32)) ||
        !message_store_initialize(&b.store, {b.records, kMessageSearchMaximumMessages},
                                  {b.order, kMessageSearchMaximumMessages},
                                  {stable_storage_data<u8>(b.content_storage), search_bank_text_limit}) ||
        !message_rich_store_initialize_owned_sparse(&b.rich, &b.rich_storage, kMessageSearchMaximumMessages,
                                                    {b.rich_order, kMessageSearchMaximumMessages},
                                                    {b.rich_text.data, 2 * search_bank_text_limit}) ||
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
bool bank_user(SearchBank& b, u64 id, const Utf8View& name, const Utf8View& avatar) noexcept {
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
    if (!text_reserve_both(b.user_names, needed))
        return false;
    const auto result = user_store_upsert(&b.users, id, name);
    return result.ok() && !result.evicted && user_store_pin(&b.users, id) &&
           (!avatar.bytes || user_store_set_avatar(&b.users, id, avatar));
}
bool bank_mentions(SearchBank& b, ConstSlice<GatewayMessageNamedUser> users) noexcept {
    for (u32 i = 0; i < users.count; ++i)
        if (!bank_user(b, users[i].id, users[i].name))
            return false;
    return true;
}

} // namespace search_detail
namespace {

bool edit_ready(const TextInputTarget& t) noexcept {
    return application_search_edit_focused() && t.context == application_search_edit_model();
}
bool edit_forbidden(u32 unit) noexcept {
    return unit < 32 || unit == 0x2028 || unit == 0x2029;
}
using EditInput = ForwardedInput<edit_ready, edit_forbidden, true>;

} // namespace
namespace search_detail {

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
    // Scrolling replaces rich-row widget handles. Publish their text before
    // the scroll host or video pump can rebuild renderer overlays this turn.
    if (*changed && !application_search_synchronize())
        return false;
    if (*changed)
        application_request_frame();
    return true;
}

} // namespace search_detail

bool application_search_active() noexcept {
    return g_app.search && g_app.search->panel && available();
}
bool application_search_close_panel() noexcept {
    auto* s = g_app.search;
    if (!s)
        return true;
    close_panel(*s);
    release_pointer(*s);
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
    if (!channel_usable(*channel) ||
        (s.scope == MessageSearchScope::guild ? channel->guild_id != s.scope_id
                                                     : channel->guild_id || channel->id != s.scope_id ||
                                                           !channel_is_private(channel->type)))
        return false;
    if (b.channel_generations[index] && b.channel_generations[index] != channel->generation)
        return false;
    if (channel->guild_id) {
        const auto* guild = chat_state_store_find_guild(state, channel->guild_id);
        if (!guild || guild->unavailable ||
            (channel_view_denied(state, *channel) && !(permit_unknown && row_access_check(s, index))))
            return false;
    }
    // A guild channel not seen before can get its first model generation once
    // navigation validates its metadata. It never gets another generation after
    // it is deleted or recreated.
    if (!b.channel_generations[index])
        b.channel_generations[index] = channel->generation;
    return true;
}

} // namespace
namespace search_detail {

bool row_unavailable(const ApplicationSearch& s, u32 index) noexcept {
    if (!s.bank || !g_app.account_ui || index >= s.bank->page.message_count)
        return true;
    const auto& b = *s.bank;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                           b.messages[index].message.channel_id);
    if (!channel)
        return true;
    return !channel_usable(*channel) ||
           (b.channel_generations[index] && b.channel_generations[index] != channel->generation) ||
           (channel->guild_id && channel_view_denied(g_app.account_ui->chat_state, *channel));
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
        !channel_is_thread(channel->type) ||
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

} // namespace search_detail
namespace {

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
    reset_value(&s.pressed);
    s.captured = s.dragging = false;
    ui_widget_manager_capture(&s.scene.widgets, ui_widget_invalid_handle());
    application_release_capture();
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
    if (!s || !accepted_current(*s) || index >= s->bank->page.message_count)
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
    if (!s || !ticket_current(*s, ticket) || !channel_authorized(*s, ticket.index, true))
        return false;
    const auto& item = s->bank->messages[ticket.index];
    *out = {ticket, item.message.channel_id,
            item.guild_known ? item.guild_id
                             : (s->scope == MessageSearchScope::guild ? s->scope_id : 0)};
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
            commit_pages(sizeof(SearchTextOwner)));
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
            pages_release(s->text_owner);
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

MessageComposer* application_search_edit_model() noexcept {
    return g_app.search ? &g_app.search->edit : nullptr;
}
bool application_search_edit_focused() noexcept {
    return g_app.search && g_app.search->focused && front();
}
TextInputTarget application_search_edit_target() noexcept {
    auto* model = application_search_edit_model();
    return {model, &EditInput::operations, model ? model->document_identity : 0};
}
UiRect application_search_edit_bounds() noexcept {
    return g_app.search ? g_app.search->scene.text : UiRect{};
}
void application_search_edit_cancel_native_group(const TextInputTarget& t) noexcept {
    if (t.operations != &EditInput::operations || !g_app.search || t.context != &g_app.search->edit ||
        t.identity != g_app.search->edit.document_identity)
        return;
    EditInput::abandon(t);
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
            commit_pages(sizeof(ApplicationSearch)));
        if (!s)
            return false;
        if (!message_composer_initialize(&s->edit, {s->query_text, sizeof(s->query_text)})) {
            pages_release(s);
            return false;
        }
        g_app.search = s;
        s->account_id = g_app.account_ui->chat_state.self_user_id;
        s->source = {s, 0, 1, resolve, source_current};
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
        menus_close(*s);
    if (!front() && (s->focused || s->captured || s->suggestions || s->filters || s->sort_menu)) {
        menus_close(*s);
        focus(*s, false);
        release_pointer(*s);
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
        ui_scene_node(g_app.shell_scene, UiSemantic::title_bar);
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
    ui_search_scene_set_media_policy(&s->scene, application_message_media_policy());
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
        const bool changed = !presentation->active || message_scene_pending(*scene) ||
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
    if (s->captured)
        application_release_capture();
    ui_search_scene_release(&s->scene);
    choices_release(*s);
    clear_poll_tickets(*s);
    if (s->recent)
        pages_release(s->recent);
    message_composer_release(&s->edit);
    bank_release(s->bank);
    bank_release(s->candidate);
    if (s->text_owner)
        pages_release(s->text_owner);
    pages_release(s);
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
    if (!g_app.account.instance || !application_search_pending(now))
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
    const auto admission = account_module_message_search(
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
    reset_value(&s->submitted);
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
            commit_pages(sizeof(SearchBank)));
        auto* scratch = static_cast<SearchDecodeScratch*>(
            commit_pages(sizeof(SearchDecodeScratch)));
        auto* text = body.bytes && body.bytes <= search_bank_text_limit
                         ? static_cast<char*>(
                               commit_pages(body.bytes))
                         : nullptr;
        if (s->candidate && scratch && text && bank_initialize(*s->candidate)) {
            decoded = decode_message_search_page(
                body, s->controller.query,
                {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount},
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
            pages_release(scratch);
        if (text)
            pages_release(text);
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
    auto retry =
        maximum(response.rate.retry_after_ms, response.retry_at_ms > now ? response.retry_at_ms - now : 0);
    if (response.transport_ok() && response.status_code == 202) {
        const auto indexing = decode_search_indexing_body(
            body, {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount},
            64);
        // Index readiness is local to this accepted search ticket. Recognized
        // replies with missing/zero retry_after permit a short bounded delay.
        if (indexing.ok())
            retry = maximum(retry, indexing.retry_after_ms ? indexing.retry_after_ms : 5000);
    }
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

} // namespace maicord::app_detail
