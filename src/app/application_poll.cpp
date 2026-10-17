#include "application_poll.hpp"
#include "application_cursor.hpp"
#include "application_account_logout.hpp"
#include "application_account_presentation.hpp"
#include "application_profile.hpp"
#include "application_search.hpp"
#include "application_state.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/model/message_poll_store.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/protocol/message_poll.hpp"
#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include <shellapi.h>

namespace maicord::app_detail {
namespace {
constexpr u32 page_limit = 25, cursor_capacity = 256;
constexpr u32 page_text_bytes =
    page_limit * (2 * kReactionUserMaximumNameBytes + kReactionUserMaximumAvatarHashBytes);
struct PollPage {
    ReactionUser users[page_limit];
    char text[page_text_bytes];
};
struct PollOwner {
    PollController controller;
    ApplicationPollSource source;
    MessagePollResultTicket result_ticket;
    PollRequest request;
    UiPollAnswer answers[kUiPollMaximumAnswers];
    UiPollVoter voters[page_limit];
    UiPollVotersScene scene;
    UiPollScene card;
    UiWidgetManager widgets;
    UiWidget widget_storage[1];
    UiLayoutNode layout_storage[1];
    UiWidgetReference root;
    UiPollVotersTarget pressed;
    PollPage* pages[2];
    u64 cursors[cursor_capacity], publication, pressed_publication, pressed_revision;
    u32 revision, user_count, cursor_first, cursor_count, page, requested_page, held_key;
    u16 selection;
    u8 bank;
    Utf8View status;
    bool results, voters_active, captured, cancel_requested, release_pending;
    u64 projected_theme;
    u32 projected_dpi;
    u64 card_theme;
    u32 card_dpi;
    UiRect card_node_bounds;
    char question[kUiPollQuestionMaximumBytes];
    char answer_text[kUiPollMaximumAnswers][kUiPollAnswerMaximumBytes];
    char emoji_text[kUiPollMaximumAnswers][kUiPollEmojiMaximumBytes];
    u32 label_publication, observed_model_publication;
    u64 uncertain_messages[8];
    bool reconcile_all, reconcile_requested, source_checked;
    u32 loaded_before[cursor_capacity];
    bool voters_more;
    bool aggregates_known;
};
PollOwner* owner{};
u64 publication{};
u64 visible_expiry{};

PollControllerActor actor() noexcept {
    if (application_account_logout_blocked() || application_account_logout_host_owned() ||
        !g_app.account_ui || g_app.login.visible || !g_app.account.instance ||
        !g_app.plugin_account_scope_serial || !g_app.account.status.profile_id ||
        !g_app.account_ui->chat_state.self_user_id)
        return {};
    return {g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
            g_app.account_ui->chat_state.self_user_id, reinterpret_cast<usize>(g_app.account.instance)};
}
bool actor_current(const ApplicationPollSource& s) noexcept {
    const auto current = actor();
    return current.instance && s.ui == g_app.account_ui && current.instance == s.source.actor.instance &&
           current.account_scope == s.source.actor.account_scope &&
           current.profile_id == s.source.actor.profile_id &&
           current.self_user_id == s.source.actor.self_user_id;
}
const ChannelRecord* channel(u64 id) noexcept {
    return g_app.account_ui ? chat_state_store_find_channel(g_app.account_ui->chat_state, id)
                            : nullptr;
}
bool readable(const ChannelRecord& c) noexcept {
    constexpr u32 required = channel_action_view | channel_action_read_history;
    if (!c.occupied || !channel_is_message_capable(c.type) ||
        (c.flags & kChannelObfuscatedFlag) || (c.actions & required) != required ||
        !chat_state_store_channel_visible(g_app.account_ui->chat_state, c))
        return false;
    if (!c.guild_id)
        return c.type == 1 || c.type == 3;
    const auto* guild = chat_state_store_find_guild(g_app.account_ui->chat_state, c.guild_id);
    if (!guild || guild->unavailable || !c.permissions_resolved)
        return false;
    if (!channel_is_thread(c.type))
        return true;
    const auto* parent = channel(c.parent_id);
    return parent && parent->guild_id == c.guild_id && parent->permissions_resolved &&
           !(parent->flags & kChannelObfuscatedFlag) && (parent->actions & required) == required &&
           chat_state_store_channel_visible(g_app.account_ui->chat_state, *parent) &&
           chat_state_store_thread_accessible(g_app.account_ui->chat_state, c);
}
const MessagePollRecord* poll(const ApplicationPollSource& s) noexcept {
    if (!actor_current(s) || s.source.channel_epoch != g_app.load_trace.channel_epoch || !s.store)
        return nullptr;
    if (s.surface == UiPollSourceSurface::timeline) {
        if (s.store != &g_app.message_view.store ||
            g_app.account_ui->active_channel_id != s.source.channel_id ||
            g_app.account_ui->chat_state.selected_channel_id != s.source.channel_id ||
            g_app.account_ui->chat_state.selected_guild_id != s.guild_id)
            return nullptr;
    } else if (s.surface == UiPollSourceSurface::search) {
        ApplicationSearchSourceView view{};
        if (!application_search_source_resolve({s.source.message_id, s.source_publication, s.source_index},
                                               false, &view) ||
            view.store != s.store || view.handle.slot != s.source.handle.slot ||
            view.handle.generation != s.source.handle.generation)
            return nullptr;
    } else
        return nullptr;
    const auto* c = channel(s.source.channel_id);
    const auto* message = message_store_resolve(*s.store, s.source.handle);
    if (!c || !readable(*c) || c->generation != s.source.channel_generation || c->guild_id != s.guild_id ||
        c->parent_id != s.parent_id || c->type != s.channel_type || !message ||
        message->id != s.source.message_id || message->channel_id != s.source.channel_id ||
        message->author_id != s.author_id || message->type != s.message_type ||
        message->delivery != MessageDeliveryState::authoritative ||
        message->content_availability != MessageContentAvailability::available)
        return nullptr;
    if (s.parent_id && channel_is_thread(c->type)) {
        const auto* parent = channel(s.parent_id);
        if (!parent || parent->generation != s.parent_generation)
            return nullptr;
    }
    const auto* p = message_poll_store_find(g_app.account_ui->message_poll_store, message->id);
    return p && p->channel_id == c->id && p->content_revision == s.source.poll_content_revision ? p
                                                                                                : nullptr;
}
bool capture(u64 id, ApplicationPollSource* out) noexcept {
    const auto a = actor();
    if (!a.instance || !g_app.load_trace.channel_epoch)
        return false;
    const auto found = message_store_find(g_app.message_view.store, id);
    const auto* m = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    const auto* c = m ? channel(m->channel_id) : nullptr;
    const auto* p = m ? message_poll_store_find(g_app.account_ui->message_poll_store, id) : nullptr;
    if (!m || !c || !p || !readable(*c) || p->channel_id != c->id || !p->content_revision ||
        m->delivery != MessageDeliveryState::authoritative ||
        m->content_availability != MessageContentAvailability::available ||
        g_app.account_ui->active_channel_id != c->id ||
        g_app.account_ui->chat_state.selected_channel_id != c->id ||
        g_app.account_ui->chat_state.selected_guild_id != c->guild_id)
        return false;
    const auto* parent = channel_is_thread(c->type) ? channel(c->parent_id) : nullptr;
    *out = {
        {a, c->id, id, g_app.load_trace.channel_epoch, found.handle, p->content_revision, c->generation},
        g_app.account_ui,
        &g_app.message_view.store,
        c->guild_id,
        c->parent_id,
        m->author_id,
        parent ? parent->generation : u16{0},
        m->type,
        c->type,
        p->result_revision,
        0,
        0,
        UiPollSourceSurface::timeline};
    return true;
}
bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.running && g_app.account.status.gateway_ready &&
           !g_app.account.stop_failed &&
           g_app.account.status.required_action == AccountRequirement::none &&
           g_app.account.api.poll;
}
PollControllerSnapshot snapshot(const ApplicationPollSource& source, const MessagePollRecord& p) noexcept {
    PollControllerSnapshot s{};
    s.source = source.source;
    s.expiry_ms = p.expiry_null ? 0 : p.expiry_ms;
    s.answer_count = p.answer_count;
    s.multiple = p.allow_multiselect;
    s.finalized = p.is_finalized;
    s.can_inspect = p.layout_type == 1;
    s.can_vote = s.can_inspect && connected() && !p.expiry_null && p.expiry_ms > clock_unix_milliseconds();
    s.mine_known = p.results_known;
    for (u32 i = 0; i < p.answer_count; ++i) {
        s.answer_ids[i] = p.answers[i].id;
        s.mine_known &= p.answers[i].me_known;
        if (p.answers[i].me_voted)
            s.mine_mask |= static_cast<u16>(1u << i);
    }
    if (!s.mine_known)
        s.mine_mask = 0;
    return s;
}
void changed() noexcept {
    if (!owner)
        return;
    ++owner->revision;
    application_request_frame();
}
void clear_capture() noexcept {
    if (!owner)
        return;
    const bool captured = owner->captured;
    owner->captured = false;
    owner->pressed = {};
    ui_poll_voters_scene_pointer(&owner->scene, UiPollPointerEvent::cancel, {});
    ui_poll_scene_pointer(&owner->card, UiPollPointerEvent::cancel, {});
    if (captured && GetCapture() == g_app.window)
        ReleaseCapture();
}
bool needs_reconcile(u64 id) noexcept {
    if (!owner || owner->source_checked)
        return false;
    if (owner->reconcile_all)
        return true;
    for (u64 value : owner->uncertain_messages)
        if (value == id)
            return true;
    return false;
}
void uncertain(u64 id) noexcept {
    if (id == owner->source.source.message_id) {
        owner->source_checked = false;
        owner->reconcile_requested = false;
    }
    for (u64 value : owner->uncertain_messages)
        if (value == id)
            return;
    for (auto& value : owner->uncertain_messages)
        if (!value) {
            value = id;
            return;
        }
    owner->reconcile_all = true;
}
void cancel_retired() noexcept {
    if (!owner || !owner->controller.flight.active || !owner->controller.flight.retired ||
        owner->cancel_requested)
        return;
    const auto& flight = owner->controller.flight;
    if (flight.command.kind == PollControllerWorkKind::vote)
        uncertain(flight.command.snapshot.source.message_id);
    if (g_app.account.instance &&
        reinterpret_cast<usize>(g_app.account.instance) == flight.command.snapshot.source.actor.instance &&
        !application_account_logout_host_owned() && g_app.account.api.poll) {
        g_app.account.api.poll(g_app.account.instance, nullptr, flight.ticket.request_id);
        owner->cancel_requested = true;
    }
}
bool ensure_owner() noexcept {
    if (owner)
        return !owner->release_pending;
    owner = static_cast<PollOwner*>(
        VirtualAlloc(nullptr, sizeof(PollOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner)
        return false;
    poll_controller_initialize(&owner->controller);
    if (!ui_widget_manager_initialize(&owner->widgets, {owner->widget_storage, 1},
                                      {owner->layout_storage, 1})) {
        VirtualFree(owner, 0, MEM_RELEASE);
        owner = nullptr;
        return false;
    }
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::overlay;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.flags = ui_layout_node_visible | ui_layout_node_allow_overlap;
    const auto root = ui_widget_manager_add(&owner->widgets, ui_widget_invalid_handle(),
                                            {1,
                                             UiWidgetKind::container,
                                             ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable,
                                             {utf8_literal("PollVoters"), style, {}, 0, 1}});
    if (!root.ok()) {
        VirtualFree(owner, 0, MEM_RELEASE);
        owner = nullptr;
        return false;
    }
    owner->root = {&owner->widgets, root.handle};
    return true;
}
void free_owner() noexcept {
    if (!owner)
        return;
    clear_capture();
    if (g_app.context_focus == &owner->widgets)
        g_app.context_focus = nullptr;
    renderer_bind_poll_scene(&g_app.renderer, nullptr);
    renderer_bind_poll_voters_scene(&g_app.renderer, nullptr);
    for (auto* page : owner->pages)
        if (page)
            VirtualFree(page, 0, MEM_RELEASE);
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}
bool page_storage(u8 bank) noexcept {
    if (!owner->pages[bank])
        owner->pages[bank] = static_cast<PollPage*>(
            VirtualAlloc(nullptr, sizeof(PollPage), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return owner->pages[bank] != nullptr;
}
Utf8View error_text(PollControllerError e) noexcept {
    switch (e) {
    case PollControllerError::forbidden:
        return utf8_literal("You cannot vote in this poll right now.");
    case PollControllerError::expired:
    case PollControllerError::finalized:
        return utf8_literal("This poll has closed.");
    case PollControllerError::selection:
        return utf8_literal("Choose an answer before voting.");
    case PollControllerError::uncertain:
        return utf8_literal("Your vote could not be confirmed. Refresh before trying again.");
    case PollControllerError::transport:
    case PollControllerError::http:
    case PollControllerError::decode:
        // The official client's vote failure alert falls back to this body text.
        return utf8_literal("Seems like something's gone wrong. Sorry about that.");
    case PollControllerError::busy:
        return utf8_literal("Your vote is still being sent.");
    default:
        return {};
    }
}
bool source_same(const ApplicationPollSource& a, const ApplicationPollSource& b) noexcept {
    return a.ui == b.ui && a.store == b.store && a.surface == b.surface &&
           a.source_publication == b.source_publication && a.source_index == b.source_index &&
           a.guild_id == b.guild_id && a.parent_id == b.parent_id &&
           a.parent_generation == b.parent_generation && a.author_id == b.author_id &&
           a.message_type == b.message_type && a.channel_type == b.channel_type &&
           poll_controller_source_equal(a.source, b.source);
}
bool project_state(const ApplicationPollSource& source, const MessagePollRecord& p,
                   Slice<UiPollAnswer> answers, UiPollState* out) noexcept {
    if (!out || !answers.data || answers.count < p.answer_count || p.answer_count > kUiPollMaximumAnswers)
        return false;
    auto& store = g_app.account_ui->message_poll_store;
    const bool selected = owner && !owner->release_pending && owner->controller.source_active &&
                          source_same(owner->source, source);
    if (selected && owner->label_publication != store.content_revision) {
        const auto question = message_poll_store_string(store, p.question);
        if (question.bytes > sizeof(owner->question))
            return false;
        copy_bytes(owner->question, question.data, question.bytes);
        for (u32 i = 0; i < p.answer_count; ++i) {
            const auto text = message_poll_store_string(store, p.answers[i].text);
            const auto emoji = message_poll_store_string(store, p.answers[i].emoji_name);
            if (text.bytes > sizeof(owner->answer_text[i]) || emoji.bytes > sizeof(owner->emoji_text[i]))
                return false;
            copy_bytes(owner->answer_text[i], text.data, text.bytes);
            copy_bytes(owner->emoji_text[i], emoji.data, emoji.bytes);
        }
        owner->label_publication = store.content_revision;
    }
    const auto s = snapshot(source, p);
    UiPollState state{};
    state.owner = source.source.message_id;
    // Read-only cards and selected poll controllers have disjoint response domains. A
    // closed controller never lends its semantic action to a later lifetime.
    state.generation =
        selected ? (owner->publication << 1) | 1u : u64{source.source.poll_content_revision} << 1;
    state.revision = (u64{p.result_revision} << 32) | (selected ? owner->revision : 1u);
    state.content_revision = p.content_revision;
    state.result_revision = p.result_revision;
    state.question = selected ? Utf8View{owner->question, p.question.bytes}
                              : message_poll_store_string(store, p.question);
    state.answers = {answers.data, p.answer_count};
    const u64 now = clock_unix_milliseconds();
    state.now_ms = p.expiry_ms && now >= p.expiry_ms ? p.expiry_ms : now / 60000 * 60000;
    state.expiry_ms = p.expiry_ms;
    state.expiry_known = (p.fields & gateway_message_poll_field_expiry) && !p.expiry_null;
    state.finalized = p.is_finalized;
    state.finalized_known = p.results_known;
    state.multiselect = p.allow_multiselect;
    state.results_known = p.results_known;
    state.results = selected ? owner->results : p.is_finalized || (s.mine_known && s.mine_mask);
    state.status = selected ? owner->status : Utf8View{};
    state.supported = p.layout_type == 1;
    state.can_view_results = state.supported;
    state.can_view_voters = state.supported && state.results;
    const bool working = selected && (owner->controller.pending_ready || owner->controller.flight.active ||
                                      owner->controller.desired_active);
    state.loading = working;
    state.error = selected && owner->controller.last_error != PollControllerError::none;
    const bool barrier = selected && (owner->controller.vote_barrier != PollControllerError::none ||
                                      needs_reconcile(source.source.message_id));
    state.can_vote = s.can_vote && !s.finalized && s.mine_known && !s.mine_mask && !working && !barrier;
    state.can_remove_vote =
        s.can_vote && !s.finalized && s.mine_known && s.mine_mask && !working && !barrier;
    // The official client has no retry control: a failed vote reports and the poll stays
    // votable. Only unknown results or an uncertain write offer a reload.
    state.can_retry = state.supported && connected() && !working && (!p.results_known || barrier);
    state.total_votes_known = p.results_known;
    // The official client applies the vote optimistically before its PUT: the
    // pending vote already shows its counts, own marks and the results view.
    const bool optimistic = selected && owner->controller.desired_active;
    const u16 desired = optimistic ? owner->controller.desired_mask : u16{0};
    if (optimistic)
        state.results = desired != 0;
    for (u32 i = 0; i < p.answer_count; ++i) {
        const auto& a = p.answers[i];
        u32 count = a.count;
        bool me_voted = a.me_voted, me_known = a.me_known;
        if (optimistic) {
            const bool want = ((desired >> i) & 1u) != 0;
            if (a.me_known && a.count_known && a.me_voted != want)
                count = want ? (count == ~u32{0} ? count : count + 1u) : (count ? count - 1u : 0u);
            me_voted = want;
            me_known = true;
        }
        answers[i] = {a.id,
                      selected ? Utf8View{owner->answer_text[i], a.text.bytes}
                               : message_poll_store_string(store, a.text),
                      selected ? Utf8View{owner->emoji_text[i], a.emoji_name.bytes}
                               : message_poll_store_string(store, a.emoji_name),
                      a.emoji_id,
                      count,
                      a.emoji_animated,
                      a.count_known,
                      me_voted,
                      me_known,
                      selected ? (owner->selection & (1u << i)) != 0 : a.me_known && a.me_voted};
        state.total_votes_known &= a.count_known;
        if (a.count_known)
            state.total_votes += count;
    }
    *out = state;
    return true;
}
bool project_voters() noexcept {
    if (!owner || !owner->voters_active)
        return true;
    UiPollState state{};
    const auto* p = poll(owner->source);
    if (!p || !project_state(owner->source, *p, {owner->answers, kUiPollMaximumAnswers}, &state))
        return false;
    for (u32 i = 0; i < owner->user_count; ++i) {
        const auto& user = owner->pages[owner->bank]->users[i];
        const u32 variant = user_avatar_variant(user.avatar_hash);
        owner->voters[i] = {user.id, user.display_name.bytes ? user.display_name : user.username, variant,
                            variant && asset_hash_animated(user.avatar_hash),
                            variant ? user.avatar_hash : Utf8View{}};
    }
    const bool loading = (owner->controller.pending_ready &&
                          owner->controller.pending.kind == PollControllerWorkKind::voters) ||
                         (owner->controller.flight.active && !owner->controller.flight.retired &&
                          owner->controller.flight.command.kind == PollControllerWorkKind::voters);
    const UiPollVotersState view{owner->source.source.message_id,
                                 owner->publication,
                                 owner->revision,
                                 p->content_revision,
                                 owner->controller.voter_answer_id,
                                 owner->page,
                                 state.question,
                                 owner->status,
                                 state.answers,
                                 {owner->voters, owner->user_count},
                                 loading,
                                 owner->controller.voters_failed,
                                 owner->page > owner->cursor_first,
                                 owner->voters_more && !loading,
                                 owner->controller.voters_failed && !loading && connected()};
    const auto viewport = application_viewport_dips();
    const auto theme = hash64_counted_bytes(kHash64Seed, &application_theme(), sizeof(Theme));
    const auto& previous = owner->scene.geometry.viewport;
    if (owner->scene.active && owner->scene.state.revision == owner->revision &&
        previous.left == viewport.left && previous.top == viewport.top &&
        previous.right == viewport.right && previous.bottom == viewport.bottom &&
        owner->projected_theme == theme && owner->projected_dpi == g_app.renderer.dpi)
        return true;
    const auto laid = ui_widget_manager_layout_rectangle(&owner->widgets, viewport);
    const bool projected =
        laid.ok() &&
        ui_poll_voters_scene_project(&owner->scene, view, viewport, owner->root,
                                     ui_theme_poll_voters_style(application_theme()), application_theme(),
                                     &g_app.account_ui->text_measure_cache,
                                     renderer_text_measure_revision(g_app.renderer), owner->scene.scroll);
    if (projected) {
        owner->projected_theme = theme;
        owner->projected_dpi = g_app.renderer.dpi;
    }
    return projected;
}
bool same_rectangle(UiRect a, UiRect b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
MessageScene* source_scene(const ApplicationPollSource& source) noexcept {
    if (source.surface == UiPollSourceSurface::timeline)
        return &g_app.message_scene;
    auto* search = application_search_scene();
    return source.surface == UiPollSourceSurface::search && search ? ui_search_scene_message_scene(search)
                                                                   : nullptr;
}
bool project_card() noexcept {
    if (!owner || !owner->controller.source_active)
        return true;
    auto* scene = source_scene(owner->source);
    const MessageSceneRowWidget* row{};
    for (u32 i = 0; scene && i < scene->count; ++i) {
        const auto& candidate = scene->rows[i];
        if (!candidate.redacted && !candidate.forwarded_snapshot &&
            candidate.message_id == owner->source.source.message_id &&
            candidate.message.slot == owner->source.source.handle.slot &&
            candidate.message.generation == owner->source.source.handle.generation) {
            row = &candidate;
            break;
        }
    }
    if (!row) {
        owner->card.active = false;
        renderer_bind_poll_scene(&g_app.renderer, nullptr);
        return true;
    }
    const auto handle = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::poll, 0);
    const auto* node = message_scene_rich_item_layout_node(*scene, *row, MessageSceneRichItemKind::poll, 0);
    if (!node || !handle.generation || !ui_widget_manager_input_enabled(scene->widgets, handle)) {
        owner->card.active = false;
        renderer_bind_poll_scene(&g_app.renderer, nullptr);
        return true;
    }
    UiPollState state{};
    const auto* p = poll(owner->source);
    if (!p || !project_state(owner->source, *p, {owner->answers, kUiPollMaximumAnswers}, &state))
        return false;
    const auto clip = ui_rect_intersection(node->clip_rectangle, scene->viewport);
    const auto theme = hash64_counted_bytes(kHash64Seed, &application_theme(), sizeof(Theme));
    const UiWidgetReference root{&scene->widgets, handle};
    const bool same =
        owner->card.active && owner->card.projection.state.revision == state.revision &&
        owner->card.owner.manager == root.manager && owner->card.owner.handle.slot == handle.slot &&
        owner->card.owner.handle.generation == handle.generation &&
        same_rectangle(owner->card_node_bounds, node->content_rectangle) &&
        same_rectangle(owner->card.clip, clip) && owner->card.projection.state.now_ms == state.now_ms &&
        owner->card_theme == theme && owner->card_dpi == g_app.renderer.dpi;
    if (!same) {
        UiPollProjection projection{};
        if (!message_scene_poll_projection(*scene, *row, application_theme(),
                                           {owner->answers, kUiPollMaximumAnswers}, &projection) ||
            !ui_poll_scene_project(&owner->card, state, projection.geometry.bounds, clip, root,
                                   owner->source.surface, ui_theme_poll_style(application_theme()),
                                   application_theme(), &g_app.account_ui->text_measure_cache,
                                   renderer_text_measure_revision(g_app.renderer)))
            return false;
    }
    owner->card_node_bounds = node->content_rectangle;
    owner->card_theme = theme;
    owner->card_dpi = g_app.renderer.dpi;
    return renderer_bind_poll_scene(&g_app.renderer, &owner->card);
}
bool queue_page(u32 answer, u32 page, u64 after) noexcept {
    if (!owner || !application_poll_synchronize() || !owner->controller.source_active)
        return false;
    const auto queued = poll_controller_queue_voter_page(&owner->controller, answer, after, page_limit,
                                                         clock_monotonic_milliseconds());
    if (!queued.ok()) {
        owner->status = error_text(queued.error);
        changed();
        return false;
    }
    cancel_retired();
    clear_capture();
    owner->requested_page = page;
    owner->user_count = 0;
    owner->status = {};
    owner->voters_active = true;
    owner->scene.scroll = 0;
    changed();
    g_app.context_focus = &owner->widgets;
    return project_voters();
}
bool front() noexcept {
    return !g_app.text_form && !g_app.popup_scene.active && !g_app.image_viewer.scene.active &&
           !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
bool choose_card(UiPoint point, bool keyboard) noexcept {
    MessageScene* scene = &g_app.message_scene;
    if (application_search_active()) {
        auto* search = application_search_scene();
        scene = search ? ui_search_scene_message_scene(search) : nullptr;
    }
    if (!scene || !scene->active)
        return false;
    for (u32 i = 0; i < scene->count; ++i) {
        const auto& row = scene->rows[i];
        if (row.redacted || row.forwarded_snapshot)
            continue;
        const auto root = message_scene_rich_item_widget(row, MessageSceneRichItemKind::poll, 0);
        if (!root.generation || !ui_widget_manager_input_enabled(scene->widgets, root))
            continue;
        if (keyboard &&
            (g_app.context_focus != &scene->widgets || scene->widgets.focused.slot != root.slot ||
             scene->widgets.focused.generation != root.generation))
            continue;
        UiPollAnswer answers[kUiPollMaximumAnswers]{};
        UiPollProjection projection{};
        if (!message_scene_poll_projection(*scene, row, application_theme(),
                                           {answers, kUiPollMaximumAnswers}, &projection))
            continue;
        if (!keyboard && ui_poll_controls_hit(projection, point).control == UiPollControl::none)
            continue;
        ApplicationPollSource source{};
        UiPollState state{};
        bool found{};
        if (scene == &g_app.message_scene)
            found =
                application_poll_state(row.message_id, {answers, kUiPollMaximumAnswers}, &state, &source);
        else {
            ApplicationSearchSourceTicket ticket{};
            found = application_search_source_ticket(row.source_index, &ticket) &&
                    application_poll_search_state(row.message_id, ticket.publication_revision,
                                                  {answers, kUiPollMaximumAnswers}, &state, &source);
        }
        if (!found || !application_poll_open(source) || !application_poll_synchronize() ||
            !owner->card.active)
            continue;
        if (keyboard) {
            ui_poll_scene_key(&owner->card, UiPollKey::first);
            owner->card.keyboard_focus = true;
        }
        return true;
    }
    return false;
}
} // namespace

bool application_poll_source_current(const ApplicationPollSource& source) noexcept {
    return poll(source) != nullptr;
}
bool application_poll_state(u64 id, Slice<UiPollAnswer> answers, UiPollState* out,
                            ApplicationPollSource* source) noexcept {
    ApplicationPollSource s{};
    if (!capture(id, &s))
        return false;
    const auto* p = poll(s);
    if (!p || !project_state(s, *p, answers, out))
        return false;
    if (source)
        *source = s;
    return true;
}
bool application_poll_search_state(u64 id, u32 source_publication, Slice<UiPollAnswer> answers,
                                   UiPollState* out, ApplicationPollSource* source) noexcept {
    ApplicationSearchSourceView view{};
    const auto a = actor();
    if (!a.instance || !g_app.load_trace.channel_epoch ||
        !application_search_source_resolve(id, source_publication, false, &view) || !view.store ||
        !view.message || view.message->delivery != MessageDeliveryState::authoritative ||
        view.message->content_availability != MessageContentAvailability::available)
        return false;
    const auto* c = channel(view.message->channel_id);
    const auto* p = message_poll_store_find(g_app.account_ui->message_poll_store, id);
    if (!c || !readable(*c) || !p || p->channel_id != c->id || view.message->channel_id != c->id)
        return false;
    const auto* parent = channel_is_thread(c->type) ? channel(c->parent_id) : nullptr;
    const ApplicationPollSource s{
        {a, c->id, id, g_app.load_trace.channel_epoch, view.handle, p->content_revision, c->generation},
        g_app.account_ui,
        view.store,
        c->guild_id,
        c->parent_id,
        view.message->author_id,
        parent ? parent->generation : u16{0},
        view.message->type,
        c->type,
        p->result_revision,
        view.ticket.publication_revision,
        view.ticket.index,
        UiPollSourceSurface::search};
    if (!poll(s) || !project_state(s, *p, answers, out))
        return false;
    if (source)
        *source = s;
    return true;
}
bool application_poll_open(const ApplicationPollSource& source) noexcept {
    const auto* p = poll(source);
    if (!p || !ensure_owner() || publication >= (~u64{0} / 2))
        return false;
    if (owner->controller.source_active && source_same(owner->source, source))
        return true;
    clear_capture();
    if (owner->source.source.actor.account_scope &&
        (owner->source.source.actor.account_scope != source.source.actor.account_scope ||
         owner->source.source.actor.instance != source.source.actor.instance)) {
        for (auto& value : owner->uncertain_messages)
            value = 0;
        owner->reconcile_all = false;
    }
    owner->source = source;
    owner->publication = ++publication;
    owner->reconcile_requested = owner->source_checked = false;
    owner->aggregates_known = p->results_known;
    for (u32 i = 0; i < p->answer_count; ++i)
        owner->aggregates_known &= p->answers[i].count_known;
    owner->label_publication = owner->observed_model_publication = 0;
    const auto initial = snapshot(source, *p);
    owner->results = p->is_finalized || (initial.mine_known && initial.mine_mask);
    owner->voters_active = false;
    owner->user_count = owner->cursor_count = owner->cursor_first = owner->page = 0;
    owner->status = {};
    owner->scene.active = false;
    const auto observed = poll_controller_observe(&owner->controller, snapshot(source, *p));
    cancel_retired();
    owner->selection = owner->controller.snapshot.mine_mask;
    if (!observed.ok())
        return false;
    changed();
    return true;
}
bool application_poll_active() noexcept {
    return owner && owner->controller.source_active && poll(owner->source);
}
ApplicationPollView application_poll_view() noexcept {
    if (!application_poll_active())
        return {};
    auto& value = *owner;
    const auto* p = poll(value.source);
    UiPollState state{};
    if (!project_state(value.source, *p, {value.answers, kUiPollMaximumAnswers}, &state))
        return {};
    return {value.source,
            state,
            value.status,
            value.pages[value.bank]
                ? ConstSlice<ReactionUser>{value.pages[value.bank]->users, value.user_count}
                : ConstSlice<ReactionUser>{},
            value.publication,
            value.revision,
            value.controller.voter_answer_id,
            value.page,
            true,
            value.voters_active,
            value.scene.state.loading,
            value.controller.voters_failed,
            value.voters_more};
}
bool application_poll_action(const ApplicationPollSource& source, const UiPollAction& action) noexcept {
    const auto* p = poll(source);
    UiPollAnswer answers[kUiPollMaximumAnswers]{};
    UiPollState state{};
    if (!p || source.result_revision != p->result_revision ||
        !project_state(source, *p, {answers, kUiPollMaximumAnswers}, &state) ||
        !ui_poll_action_current(state, action))
        return false;
    if (!application_poll_open(source))
        return false;
    auto& c = owner->controller;
    const auto now = clock_monotonic_milliseconds(), unix_now = clock_unix_milliseconds();
    PollControllerResult queued{};
    switch (action.kind) {
    case UiPollActionKind::toggle_answer: {
        if (!state.can_vote || state.results)
            return false;
        u32 index = p->answer_count;
        for (u32 i = 0; i < p->answer_count; ++i)
            if (p->answers[i].id == action.answer_id)
                index = i;
        if (index == p->answer_count)
            return false;
        // As in the official client, tapping a selected answer clears it (single-select too);
        // otherwise single-select replaces the selection.
        const u16 bit = static_cast<u16>(1u << index);
        owner->selection = (owner->selection & bit) ? static_cast<u16>(owner->selection & ~bit)
                           : p->allow_multiselect ? static_cast<u16>(owner->selection | bit)
                                                  : bit;
        owner->status = {};
        changed();
        return true;
    }
    case UiPollActionKind::vote:
        if (!state.can_vote || !owner->selection)
            return false;
        queued = poll_controller_queue_vote(&c, owner->selection, now, unix_now);
        break;
    case UiPollActionKind::remove_vote:
        if (!state.can_remove_vote)
            return false;
        queued = poll_controller_queue_vote(&c, 0, now, unix_now);
        break;
    case UiPollActionKind::results:
        if (!state.can_view_results)
            return false;
        // "Show results" / "Go back to vote" flip the local view and clear the
        // selection; nothing is sent.
        owner->results = !state.results;
        owner->selection = 0;
        if (!p->results_known)
            queued = poll_controller_queue_refresh(&c, now);
        changed();
        return queued.ok();
    case UiPollActionKind::voters:
        if (!state.can_view_voters)
            return false;
        return application_poll_voters(action.answer_id);
    case UiPollActionKind::retry:
        if (!state.can_retry)
            return false;
        queued = poll_controller_queue_refresh(&c, now);
        if (queued.ok())
            owner->reconcile_requested = true;
        break;
    default:
        return false;
    }
    owner->status = queued.ok() ? Utf8View{} : error_text(queued.error);
    changed();
    return queued.ok();
}
bool application_poll_voters(u32 answer, bool more) noexcept {
    if (!application_poll_active())
        return false;
    auto& value = *owner;
    if (more) {
        if (!value.voters_active || value.controller.voter_answer_id != answer || !value.voters_more ||
            value.controller.flight.active || value.controller.pending_ready || value.page == ~u32{0})
            return false;
        const u32 next = value.page + 1;
        if (next - value.cursor_first >= cursor_capacity) {
            for (u32 i = 1; i < cursor_capacity; ++i) {
                value.cursors[i - 1] = value.cursors[i];
                value.loaded_before[i - 1] = value.loaded_before[i];
            }
            ++value.cursor_first;
            --value.cursor_count;
        }
        value.cursors[next - value.cursor_first] = value.controller.after_user_id;
        const u64 loaded = u64{value.loaded_before[value.page - value.cursor_first]} + value.user_count;
        value.loaded_before[next - value.cursor_first] = static_cast<u32>(minimum(loaded, u64{~u32{0}}));
        value.cursor_count = next - value.cursor_first + 1;
        return queue_page(answer, next, value.controller.after_user_id);
    }
    value.cursor_first = value.cursor_count = value.page = 0;
    value.cursors[0] = 0;
    value.loaded_before[0] = 0;
    value.cursor_count = 1;
    return queue_page(answer, 0, 0);
}
bool application_poll_voters_previous() noexcept {
    if (!application_poll_voters_active() || owner->page <= owner->cursor_first ||
        owner->controller.flight.active || owner->controller.pending_ready)
        return false;
    const u32 previous = owner->page - 1;
    return queue_page(owner->controller.voter_answer_id, previous,
                      owner->cursors[previous - owner->cursor_first]);
}
void application_poll_voters_close() noexcept {
    if (!owner || !owner->voters_active)
        return;
    clear_capture();
    owner->voters_active = owner->scene.active = false;
    renderer_bind_poll_voters_scene(&g_app.renderer, nullptr);
    auto& c = owner->controller;
    if (c.pending_ready && c.pending.kind == PollControllerWorkKind::voters) {
        c.pending = {};
        c.pending_ready = false;
    }
    if (c.flight.active && c.flight.command.kind == PollControllerWorkKind::voters)
        c.flight.retired = true;
    cancel_retired();
    if (g_app.context_focus == &owner->widgets)
        g_app.context_focus = nullptr;
    changed();
}
bool application_poll_voters_active() noexcept {
    return application_poll_active() && owner->voters_active;
}
void application_poll_close() noexcept {
    if (!owner)
        return;
    application_poll_voters_close();
    poll_controller_close(&owner->controller);
    owner->card.active = false;
    renderer_bind_poll_scene(&g_app.renderer, nullptr);
    cancel_retired();
    changed();
}
bool application_poll_synchronize() noexcept {
    if (!actor().instance)
        visible_expiry = 0;
    else if (visible_expiry && clock_unix_milliseconds() >= visible_expiry) {
        visible_expiry = 0;
        application_request_frame();
    }
    if (!owner)
        return true;
    if (owner->release_pending) {
        cancel_retired();
        if (!owner->controller.flight.active || !g_app.account.instance)
            free_owner();
        return true;
    }
    const auto* p = owner->controller.source_active ? poll(owner->source) : nullptr;
    if (!p) {
        if (owner->controller.source_active)
            application_poll_close();
        return true;
    }
    const auto before = owner->controller;
    bool aggregates = p->results_known;
    for (u32 i = 0; i < p->answer_count; ++i)
        aggregates &= p->answers[i].count_known;
    if (owner->aggregates_known && !aggregates)
        owner->reconcile_requested = false;
    owner->aggregates_known = aggregates;
    poll_controller_observe(&owner->controller, snapshot(owner->source, *p));
    poll_controller_expire(&owner->controller, clock_unix_milliseconds());
    cancel_retired();
    if (owner->observed_model_publication != g_app.account_ui->message_poll_store.content_revision ||
        owner->source.result_revision != p->result_revision ||
        before.vote_phase != owner->controller.vote_phase ||
        before.snapshot.can_vote != owner->controller.snapshot.can_vote ||
        before.desired_active != owner->controller.desired_active) {
        owner->source.result_revision = p->result_revision;
        owner->observed_model_publication = g_app.account_ui->message_poll_store.content_revision;
        if (!owner->controller.desired_active && before.desired_active) {
            owner->selection = owner->controller.snapshot.mine_mask;
            owner->results = owner->controller.snapshot.mine_known && owner->controller.snapshot.mine_mask;
        }
        owner->status = error_text(owner->controller.last_error);
        changed();
    }
    if ((owner->controller.refresh_required ||
         (!owner->reconcile_requested && (needs_reconcile(owner->source.source.message_id) ||
                                          !owner->controller.snapshot.mine_known || !aggregates))) &&
        !owner->controller.pending_ready && !owner->controller.flight.active && connected()) {
        const auto queued =
            poll_controller_queue_refresh(&owner->controller, clock_monotonic_milliseconds());
        if (queued.ok()) {
            owner->reconcile_requested = true;
            changed();
        }
    }
    return project_card() && project_voters() &&
           renderer_bind_poll_voters_scene(&g_app.renderer, owner->voters_active ? &owner->scene : nullptr);
}
bool application_poll_pending(u64 now) noexcept {
    if (!application_poll_synchronize() || !application_poll_active() || !connected())
        return false;
    PollControllerCommand command{};
    return poll_controller_prepare(owner->controller, now, clock_unix_milliseconds(), &command);
}
bool application_poll_submit(u64 id, u64 now) noexcept {
    if (!id || !application_poll_pending(now))
        return false;
    auto& value = *owner;
    PollControllerCommand command{};
    if (!poll_controller_prepare(value.controller, now, clock_unix_milliseconds(), &command))
        return false;
    PollRequest request{};
    request.channel_id = command.snapshot.source.channel_id;
    request.message_id = command.snapshot.source.message_id;
    if (command.kind == PollControllerWorkKind::vote) {
        request.operation =
            command.desired_mask ? PollOperation::vote : PollOperation::remove_vote;
        for (u32 i = 0; i < command.snapshot.answer_count; ++i)
            if (command.desired_mask & (1u << i))
                request.answer_ids[request.answer_count++] = command.snapshot.answer_ids[i];
    } else if (command.kind == PollControllerWorkKind::voters) {
        request.operation = PollOperation::voters;
        request.answer_id = command.answer_id;
        request.after_user_id = command.after_user_id;
        request.limit = command.limit;
    } else
        request.operation = PollOperation::refresh;
    const auto result_ticket = message_poll_store_result_ticket(
        g_app.account_ui->message_poll_store, request.message_id, g_app.account_ui->poll_gateway_serial);
    const auto result = g_app.account.api.poll(g_app.account.instance, &request, id);
    if (!result.ok()) {
        if (result.error == RestClientSubmitError::busy)
            poll_controller_submit_failed(&value.controller, deadline_after(now, 50));
        else {
            value.controller.pending_ready = false;
            value.controller.pending = {};
            value.controller.desired_active = false;
            if (command.kind == PollControllerWorkKind::vote)
                value.controller.vote_phase = PollControllerVotePhase::failed;
            if (command.kind == PollControllerWorkKind::voters)
                value.controller.voters_failed = true;
            value.controller.last_error = PollControllerError::transport;
            value.status = error_text(value.controller.last_error);
        }
        changed();
        return false;
    }
    PollControllerTicket ticket{};
    if (!poll_controller_admit(&value.controller, command, id, now, clock_unix_milliseconds(), &ticket)) {
        // Synchronous module validation copied the values. Preserve this
        // accepted result even if a server-state change raced the request.
        value.controller.flight = {command, {id, command.epoch}, true, true};
    }
    value.request = request;
    value.cancel_requested = false;
    const auto* p = poll(value.source);
    value.result_ticket = result_ticket;
    if (!p)
        value.controller.flight.retired = true;
    cancel_retired();
    value.status = {};
    changed();
    return true;
}
bool application_poll_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!owner || !owner->controller.flight.active || !response.request_id ||
        owner->controller.flight.ticket.request_id != response.request_id)
        return false;
    auto& value = *owner;
    const auto flight = value.controller.flight;
    const bool fresh = !flight.retired && !value.release_pending && poll(value.source) &&
                       poll_controller_source_equal(flight.command.snapshot.source, value.source.source);
    const bool successful = response.transport_ok() && response.status_code == 200;
    bool valid = true;
    ConstSlice<ReactionUser> users{};
    const u8 candidate_bank = value.bank ^ 1;
    if (fresh && flight.command.kind != PollControllerWorkKind::vote && successful) {
        valid = body.data && body.bytes == response.body_bytes && body.bytes <= kPollMaximumResponseBytes;
        if (valid && flight.command.kind == PollControllerWorkKind::voters) {
            valid = page_storage(candidate_bank);
            if (valid) {
                auto& page = *value.pages[candidate_bank];
                u32 object_scratch[256]{};
                const auto decoded = decode_poll_voters(
                    body,
                    {g_app.account_ui->message_history_page_structurals,
                     kMessageHistoryPageStructuralCount},
                    {object_scratch, 256}, 64, {page.users, page_limit}, {page.text, page_text_bytes});
                valid = decoded.ok();
                if (valid)
                    users = {page.users, decoded.users.user_count};
            }
        } else if (valid) {
            GatewayMessagePollAnswer answers[kGatewayMessagePollMaximumAnswers]{};
            GatewayMessagePollAnswerCount counts[kGatewayMessagePollMaximumAnswers]{};
            char text[kGatewayMessagePollQuestionMaximumBytes +
                      kGatewayMessagePollMaximumAnswers *
                          (kGatewayMessagePollAnswerMaximumBytes + kGatewayMessagePollEmojiMaximumBytes)]{};
            GatewayMessagePoll dto{};
            const auto decoded = decode_poll_message(
                body, value.source.source.channel_id, value.source.source.message_id,
                {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount},
                64, {text, sizeof(text)},
                {{answers, kGatewayMessagePollMaximumAnswers}, {counts, kGatewayMessagePollMaximumAnswers}},
                &dto);
            valid = decoded.ok();
            if (valid) {
                const auto applied = message_poll_store_apply(
                    &g_app.account_ui->message_poll_store, value.source.source.message_id,
                    value.source.source.channel_id, dto, 0, &value.result_ticket);
                valid = applied.ok();
                const auto* p = poll(value.source);
                if (valid && p) {
                    const auto authority = snapshot(value.source, *p);
                    poll_controller_observe(&value.controller, authority, !applied.results_ignored);
                    if (!applied.results_ignored && authority.mine_known) {
                        for (auto& id : value.uncertain_messages)
                            if (id == p->message_id)
                                id = 0;
                        value.source_checked = true;
                    }
                } else if (!p)
                    application_poll_close();
            }
        }
    }
    // The official client applies an accepted vote locally and waits for the
    // gateway echo instead of refetching the message; the store update is
    // idempotent against that echo.
    const bool vote_accepted = fresh && flight.command.kind == PollControllerWorkKind::vote &&
                               value.controller.desired_active && response.transport_ok() &&
                               response.status_code >= 200 && response.status_code < 300;
    if (vote_accepted)
        message_poll_store_own_vote(&g_app.account_ui->message_poll_store, value.source.source.message_id,
                                    value.source.source.channel_id, value.source.source.actor.self_user_id,
                                    flight.command.desired_mask);
    if (!fresh)
        value.controller.flight.retired = true;
    const auto result =
        poll_controller_complete(&value.controller, flight.ticket, response, clock_monotonic_milliseconds(),
                                 clock_unix_milliseconds(), valid, users);
    if (vote_accepted) {
        if (const auto* p = poll(value.source)) {
            const auto accepted = snapshot(value.source, *p);
            if (accepted.mine_known && accepted.mine_mask == flight.command.desired_mask) {
                poll_controller_observe(&value.controller, accepted, true);
                value.selection = accepted.mine_mask;
                value.results = accepted.mine_mask != 0;
            }
        }
    }
    value.cancel_requested = false;
    if (result.page_ready && value.voters_active) {
        value.bank = candidate_bank;
        value.user_count = users.count;
        value.page = value.requested_page;
        const auto* current = poll(value.source);
        const auto* answer =
            current ? message_poll_record_answer(*current, flight.command.answer_id) : nullptr;
        const u64 loaded = u64{value.loaded_before[value.page - value.cursor_first]} + users.count;
        value.voters_more =
            users.count &&
            (answer && answer->count_known ? loaded < answer->count : users.count == flight.command.limit);
    }
    value.status = error_text(value.controller.last_error);
    if (value.release_pending) {
        free_owner();
        return true;
    }
    changed();
    application_poll_synchronize();
    return true;
}
u64 application_poll_next_wake() noexcept {
    if (!actor().instance) {
        visible_expiry = 0;
        return 0;
    }
    u64 wake = owner && application_poll_active() && connected()
                   ? poll_controller_next_wake(owner->controller)
                   : 0;
    const u64 unix_now = clock_unix_milliseconds();
    u64 earliest{};
    const auto scan = [&](const MessageScene* scene) noexcept {
        if (!scene || !scene->active)
            return;
        for (u32 i = 0; i < scene->count; ++i) {
            const auto& row = scene->rows[i];
            if (row.redacted || row.forwarded_snapshot || !row.poll.generation)
                continue;
            const auto* p = message_poll_store_find(g_app.account_ui->message_poll_store, row.message_id);
            const u64 observed_now = scene->polls ? scene->polls->now_ms : 0;
            if (p && !p->expiry_null && !p->is_finalized && p->expiry_ms > observed_now &&
                (!earliest || p->expiry_ms < earliest))
                earliest = p->expiry_ms;
        }
    };
    scan(&g_app.message_scene);
    auto* search = application_search_scene();
    if (application_search_active() && search)
        scan(ui_search_scene_message_scene(search));
    if (application_poll_active()) {
        const auto* p = poll(owner->source);
        if (p && !p->expiry_null && !p->is_finalized && p->expiry_ms > unix_now &&
            (!earliest || p->expiry_ms < earliest))
            earliest = p->expiry_ms;
    }
    visible_expiry = earliest;
    if (earliest) {
        const auto expires = earliest <= unix_now
                                 ? u64{1}
                                 : deadline_after(clock_monotonic_milliseconds(), earliest - unix_now);
        wake = wake ? minimum(wake, expires) : expires;
    }
    return wake;
}
void application_poll_observe(const GatewayMessagePollVoteEvent& event, bool applied) noexcept {
    if (owner && applied && event.message_id == owner->source.source.message_id &&
        event.channel_id == owner->source.source.channel_id)
        application_poll_synchronize();
}
void application_poll_message_observe(const GatewayMessageEvent& event, bool applied) noexcept {
    if (owner && applied && event.id == owner->source.source.message_id &&
        (!event.channel_id || event.channel_id == owner->source.source.channel_id))
        application_poll_synchronize();
}
void application_poll_message_deleted(u64 channel_id, u64 message_id) noexcept {
    if (owner && channel_id == owner->source.source.channel_id &&
        message_id == owner->source.source.message_id)
        application_poll_close();
}
void application_poll_gateway_reset() noexcept {
    application_poll_close();
}
void application_poll_release() noexcept {
    visible_expiry = 0;
    if (!owner)
        return;
    application_poll_close();
    owner->release_pending = true;
    if (!owner->controller.flight.active || !g_app.account.instance)
        free_owner();
}
ApplicationPollMetrics application_poll_metrics() noexcept {
    if (!owner)
        return {};
    u32 pages{};
    for (auto* page : owner->pages)
        if (page)
            pages += (sizeof(PollPage) + 4095u) & ~4095u;
    return {sizeof(PollOwner),
            (sizeof(PollOwner) + 4095u) & ~4095u,
            pages,
            owner->revision,
            owner->publication,
            owner->controller.source_active,
            owner->controller.flight.active};
}
bool application_poll_voters_action(const UiPollVotersAction& action) noexcept {
    if (!application_poll_voters_active() ||
        !ui_widget_manager_input_enabled(owner->widgets, owner->root.handle) ||
        action.owner != owner->source.source.message_id || action.generation != owner->publication ||
        action.revision != owner->revision ||
        !ui_poll_voters_target_enabled(owner->scene, {action.control, action.id}))
        return false;
    switch (action.control) {
    case UiPollVotersControl::close:
        application_poll_voters_close();
        return true;
    case UiPollVotersControl::answer:
        return action.id <= ~u32{0} && application_poll_voters(static_cast<u32>(action.id));
    case UiPollVotersControl::previous:
        return application_poll_voters_previous();
    case UiPollVotersControl::next:
        return application_poll_voters(owner->controller.voter_answer_id, true);
    case UiPollVotersControl::retry:
        return queue_page(owner->controller.voter_answer_id, owner->requested_page,
                          owner->cursors[owner->requested_page - owner->cursor_first]);
    case UiPollVotersControl::user: {
        const auto anchor = ui_poll_voters_target_rectangle(owner->scene, {action.control, action.id});
        return application_profile_open(action.id, anchor);
    }
    default:
        return false;
    }
}
bool application_poll_consume_key_tail(UiInputKind kind, u32 code, bool repeated) noexcept {
    return owner && ui_input_consume_key_tail(&owner->held_key, {kind, code, repeated});
}
bool application_poll_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if (application_poll_consume_key_tail(kind, code, repeated))
        return true;
    if (!front())
        return false;
    const bool voters = application_poll_voters_active();
    const bool focused =
        owner && owner->card.active && owner->card.owner.manager &&
        g_app.context_focus == owner->card.owner.manager &&
        owner->card.owner.manager->focused.slot == owner->card.owner.handle.slot &&
        owner->card.owner.manager->focused.generation == owner->card.owner.handle.generation;
    if (!voters && !focused &&
        ((kind != UiInputKind::key_down && kind != UiInputKind::system_key_down) || !choose_card({}, true)))
        return false;
    if (!voters)
        owner->card.keyboard_focus = true;
    if (!voters && !application_poll_active())
        return false;
    if (kind == UiInputKind::character || kind == UiInputKind::system_character ||
        kind == UiInputKind::key_up)
        return true;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return false;
    if (code == VK_ESCAPE) {
        owner->held_key = code;
        if (voters)
            application_poll_voters_close();
        else
            ui_poll_scene_blur(&owner->card);
        application_request_frame();
        return true;
    }
    UiPollKey key{};
    if (code == VK_RETURN || code == VK_SPACE)
        key = UiPollKey::activate;
    else if (code == VK_TAB || code == VK_DOWN || code == VK_RIGHT)
        key = code == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000) ? UiPollKey::previous : UiPollKey::next;
    else if (code == VK_UP || code == VK_LEFT)
        key = UiPollKey::previous;
    else if (code == VK_HOME)
        key = UiPollKey::first;
    else if (code == VK_END)
        key = UiPollKey::last;
    else
        return true;
    if (key == UiPollKey::activate && repeated)
        return true;
    if (voters) {
        const auto result = ui_poll_voters_scene_key(&owner->scene, key);
        if (result.action_requested) {
            owner->held_key = code;
            application_poll_voters_action(result.action);
        }
    } else {
        const auto result = ui_poll_scene_key(&owner->card, key);
        if (result.action_requested) {
            owner->held_key = code;
            application_poll_action(owner->source, result.action);
        }
    }
    application_request_frame();
    return true;
}
bool application_poll_cursor(UiPoint point, UiCursor* cursor) noexcept {
    if (!cursor || !front())
        return false;
    // The selected timeline card carries live pending/vote state.
    if (owner && owner->card.active && owner->card.surface == UiPollSourceSurface::timeline &&
        ui_rect_contains(ui_rect_intersection(owner->card.projection.geometry.bounds, owner->card.clip), point.x,
                         point.y)) {
        *cursor = ui_poll_scene_cursor(owner->card, point);
        return true;
    }
    const auto& scene = g_app.message_scene;
    if (!scene.active || !ui_rect_contains(scene.viewport, point.x, point.y))
        return false;
    for (u32 i = 0; i < scene.count; ++i) {
        const auto& row = scene.rows[i];
        if (row.redacted || row.forwarded_snapshot)
            continue;
        const auto root = message_scene_rich_item_widget(row, MessageSceneRichItemKind::poll, 0);
        const auto* node = root.generation && ui_widget_manager_input_enabled(scene.widgets, root)
                               ? ui_widget_manager_layout_node(scene.widgets, root)
                               : nullptr;
        if (!node || !ui_rect_contains(ui_rect_intersection(node->rectangle, node->clip_rectangle), point.x, point.y))
            continue;
        UiPollAnswer answers[kUiPollMaximumAnswers]{};
        UiPollProjection projection{};
        *cursor = message_scene_poll_projection(scene, row, application_theme(), {answers, kUiPollMaximumAnswers},
                                                &projection)
                      ? ui_poll_controls_cursor(projection, point)
                      : UiCursor::arrow;
        return true;
    }
    return false;
}
bool application_poll_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS) {
        clear_capture();
        return false;
    }
    if (!front())
        return false;
    const bool voters = application_poll_voters_active();
    if (message == WM_SETCURSOR) {
        // The voters modal owns the client; received cards resolve with the message pane.
        UiPoint pointer{};
        if (!voters || !application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &pointer))
            return false;
        application_set_cursor(ui_poll_voters_scene_cursor(owner->scene, pointer));
        return true;
    }
    const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
    const UiPoint point{static_cast<short>(LOWORD(lparam)) * scale,
                        static_cast<short>(HIWORD(lparam)) * scale};
    if (!voters && message == WM_LBUTTONDOWN)
        choose_card(point, false);
    if (!voters && (!application_poll_active() || !owner->card.active))
        return false;
    if (!voters) {
        UiPollPointerEvent event{};
        if (message == WM_LBUTTONDOWN)
            event = UiPollPointerEvent::down;
        else if (message == WM_LBUTTONUP)
            event = UiPollPointerEvent::up;
        else if (message == WM_MOUSEMOVE)
            event = UiPollPointerEvent::move;
        else if (message == WM_MOUSELEAVE)
            event = UiPollPointerEvent::leave;
        else
            return false;
        const auto result = ui_poll_scene_pointer(&owner->card, event, point);
        if (event == UiPollPointerEvent::down && result.handled) {
            auto* manager = const_cast<UiWidgetManager*>(owner->card.owner.manager);
            if (manager) {
                ui_widget_manager_focus(manager, owner->card.owner.handle);
                g_app.context_focus = manager;
            }
        }
        if (result.request_capture) {
            owner->captured = true;
            SetCapture(g_app.window);
        }
        if (result.action_requested)
            application_poll_action(owner->source, result.action);
        if (result.release_capture)
            clear_capture();
        if (result.handled)
            application_request_frame();
        return result.handled;
    }
    if (message == WM_DROPFILES) {
        DragFinish(reinterpret_cast<HDROP>(wparam));
        return true;
    }
    if (message == WM_MOUSEWHEEL) {
        ui_poll_voters_scene_scroll(&owner->scene, -static_cast<short>(HIWORD(wparam)) / 120.f * 44.f);
        application_request_frame();
        return true;
    }
    UiPollPointerEvent event{};
    if (message == WM_LBUTTONDOWN)
        event = UiPollPointerEvent::down;
    else if (message == WM_LBUTTONUP)
        event = UiPollPointerEvent::up;
    else if (message == WM_MOUSEMOVE)
        event = UiPollPointerEvent::move;
    else if (message == WM_MOUSELEAVE)
        event = UiPollPointerEvent::leave;
    else
        return message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
               message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_MOUSEHWHEEL;
    if (event == UiPollPointerEvent::down) {
        owner->pressed = ui_poll_voters_scene_hit(owner->scene, point);
        owner->pressed_publication = owner->publication;
        owner->pressed_revision = owner->revision;
    }
    const auto result = ui_poll_voters_scene_pointer(&owner->scene, event, point);
    if (result.request_capture) {
        owner->captured = true;
        SetCapture(g_app.window);
    }
    if (result.action_requested && owner->pressed_publication == owner->publication &&
        owner->pressed_revision == owner->revision &&
        ui_poll_voters_target_equal(owner->pressed, {result.action.control, result.action.id}))
        application_poll_voters_action(result.action);
    if (result.release_capture)
        clear_capture();
    application_request_frame();
    return result.handled;
}
} // namespace maicord::app_detail
