#include "application_search_navigation.hpp"
#include "maicord/core/memory.hpp"
#include "rest_rate_retry.hpp"
#include "application_state.hpp"
#include "application_content_input.hpp"
#include "application_message_mutation_notice.hpp"
#include "application_composer.hpp"
#include "application_thread_actions.hpp"
#include "application_channel_authority.hpp"
#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/app/account_message_search.hpp"
#include "maicord/protocol/channel_overview.hpp"

namespace maicord::app_detail {
namespace {
enum class MessageNavigationPhase : u8 { idle, channel, parent, membership, history, history_retry };
struct MessageNavigation {
    ApplicationSearchNavigationSource source;
    ApplicationMessageNavigationSource timeline_source;
    const AccountUiRuntime* account_ui{};
    void* account_instance{};
    u64 account_scope{}, profile{}, self{}, request_id{}, requested_channel{}, parent{}, retry_at{};
    u64 target_channel{}, target_guild{}, target_message{};
    u64 source_epoch{}, target_epoch{}, deleted_channels[8]{};
    u32 resident_revision{};
    u16 resident_generation{};
    u16 admitted_generation{};
    MessageNavigationPhase phase{};
    ApplicationThreadActionTicket membership{};
    ApplicationMessageNavigationStage notice_stage{};
    u16 notice_code{};
    bool notice_valid{}, notice_admitted{};
    bool cancelled{}, parent_fetched{}, history_request{}, admitted{}, selecting{}, timeline{}, deleted_all{};
};
MessageNavigation navigation;
static_assert(sizeof(MessageNavigation) <= 512);
bool timeline_current(const ApplicationMessageNavigationSource& source) noexcept {
    if (!g_app.account_ui || source.account_ui != g_app.account_ui || source.account_instance != g_app.account.instance ||
        !g_app.account.instance || !g_app.account.status.running || g_app.account.stop_failed || g_app.login.visible ||
        source.account_scope != g_app.plugin_account_scope_serial || source.profile_id != g_app.account.status.profile_id ||
        source.self_user_id != g_app.account_ui->chat_state.self_user_id ||
        source.channel_epoch != g_app.load_trace.channel_epoch ||
        source.channel_id != g_app.account_ui->active_channel_id ||
        source.channel_id != g_app.account_ui->chat_state.selected_channel_id ||
        !application_channel_readable(g_app, source.channel_id)) return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, source.channel_id);
    const auto* message = message_store_resolve(g_app.message_view.store, source.handle);
    return channel && channel->generation == source.channel_generation && message && message->occupied &&
        message->id == source.message_id && message->channel_id == source.channel_id && message->revision == source.revision &&
        message->delivery == MessageDeliveryState::authoritative;
}
bool deleted(u64 id) noexcept {
    if (navigation.deleted_all) return true;
    for (u64 value : navigation.deleted_channels) if (id && value == id) return true;
    return false;
}
bool readable(const ChannelRecord* channel) noexcept {
    constexpr u32 required = channel_action_view | channel_action_read_history;
    const auto* guild = navigation.target_guild ?
        chat_state_store_find_guild(g_app.account_ui->chat_state, navigation.target_guild) : nullptr;
    return channel && channel->occupied && channel->id == navigation.target_channel &&
        channel->guild_id == navigation.target_guild && channel_is_message_capable(channel->type) &&
        !(channel->flags & kChannelObfuscatedFlag) &&
        (channel_is_private(channel->type) || (channel->permissions_resolved && guild && !guild->unavailable)) &&
        (channel->actions & required) == required && application_channel_readable(g_app, channel->id);
}

bool current() noexcept {
    ApplicationSearchNavigationSource source{};
    const bool actor = navigation.phase != MessageNavigationPhase::idle && g_app.account_ui &&
        g_app.account.instance && g_app.account.instance == navigation.account_instance && navigation.account_ui == g_app.account_ui &&
        g_app.account.status.running && !g_app.account.stop_failed && !g_app.login.visible &&
        navigation.account_scope == g_app.plugin_account_scope_serial &&
        navigation.profile == g_app.account.status.profile_id &&
        navigation.self == g_app.account_ui->chat_state.self_user_id;
    if (!actor || deleted(navigation.target_channel) || deleted(navigation.parent)) return false;
    if (navigation.target_guild) {
        const auto* guild = chat_state_store_find_guild(g_app.account_ui->chat_state, navigation.target_guild);
        if (!guild || !guild->occupied || guild->unavailable) return false;
    }
    if (navigation.admitted) {
        const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, navigation.target_channel);
        return (navigation.selecting || (g_app.account_ui->active_channel_id == navigation.target_channel &&
            g_app.account_ui->chat_state.selected_channel_id == navigation.target_channel &&
            g_app.load_trace.channel_epoch == navigation.target_epoch)) && readable(channel) &&
            channel->generation == navigation.admitted_generation;
    }
    if (navigation.source_epoch != g_app.load_trace.channel_epoch) return false;
    if (navigation.timeline) return timeline_current(navigation.timeline_source);
    return
        application_search_navigation_source(navigation.source.ticket, &source) &&
        source.channel_id == navigation.source.channel_id && source.guild_id == navigation.source.guild_id;
}
void notice(Utf8View text, ApplicationMessageNavigationStage stage = ApplicationMessageNavigationStage::opening_history,
    u16 code = 0) noexcept {
    if (navigation.notice_valid && navigation.notice_stage == stage && navigation.notice_code == code &&
        navigation.notice_admitted == navigation.admitted) return;
    navigation.notice_valid = true; navigation.notice_stage = stage; navigation.notice_code = code;
    navigation.notice_admitted = navigation.admitted;
    if (!navigation.timeline) application_search_source_notice(navigation.source.ticket, text);
    if (navigation.timeline || navigation.admitted) {
        const auto channel = navigation.admitted ? navigation.target_channel : navigation.timeline_source.channel_id;
        const auto message = navigation.admitted ? navigation.target_message : navigation.timeline_source.message_id;
        application_message_mutation_notice_navigation_pending(channel, message, stage, code);
        refresh_message_composer_ui(); application_request_frame();
    }
}
void finish(Utf8View text, ApplicationMessageNavigationStage stage = ApplicationMessageNavigationStage::error,
    u16 code = 0) noexcept {
    if (!navigation.timeline) application_search_source_notice(navigation.source.ticket, text);
    if (navigation.admitted || navigation.timeline) {
        const auto channel = navigation.admitted ? navigation.target_channel : navigation.timeline_source.channel_id;
        const auto message = navigation.admitted ? navigation.target_message : navigation.timeline_source.message_id;
        if (text.bytes) {
            RestClientResponse failed{};
            failed.error = RestClientError::response;
            failed.status_code = code;
            application_message_mutation_notice_navigation_failed(channel, message, stage, failed);
        } else application_message_mutation_notice_succeeded(ApplicationMessageMutationKind::navigate_message,
            channel, message);
        refresh_message_composer_ui();
        application_request_frame();
    }
    navigation.phase = MessageNavigationPhase::idle;
    navigation.retry_at = 0;
}
bool navigate() noexcept {
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                           navigation.target_channel);
    if (channel && channel_is_thread(channel->type))
        navigation.parent = channel->parent_id;
    if (!current()) {
        finish(utf8_literal("The source message changed. Try Jump again."), ApplicationMessageNavigationStage::source_changed);
        return true;
    }
    if (channel && channel->type == 12 && !channel->thread_self_joined_known &&
        !chat_state_store_thread_accessible(g_app.account_ui->chat_state, *channel)) {
        navigation.phase = MessageNavigationPhase::membership;
        const auto admission = application_thread_actions_fetch(channel->id, &navigation.membership);
        if (admission == ApplicationThreadActionEnqueueResult::accepted ||
            admission == ApplicationThreadActionEnqueueResult::coalesced) {
            navigation.retry_at = 0;
            notice(utf8_literal("Checking your access to this private thread…"), ApplicationMessageNavigationStage::waiting_membership);
        } else if (admission == ApplicationThreadActionEnqueueResult::busy) {
            reset_value(&navigation.membership);
            navigation.retry_at = deadline_after(clock_monotonic_milliseconds(), 50);
            notice(utf8_literal("Checking access…"), ApplicationMessageNavigationStage::waiting_membership);
        } else finish(application_thread_actions_admission_text(admission), ApplicationMessageNavigationStage::failure_membership);
        return true;
    }
    if (!readable(channel)) {
        finish(utf8_literal("You cannot view this channel or its message history."), ApplicationMessageNavigationStage::permission_denied, 403);
        return true;
    }
    navigation.phase = MessageNavigationPhase::history;
    navigation.request_id = 0;
    navigation.admitted_generation = channel->generation;
    navigation.admitted = navigation.selecting = true;
    navigation.target_epoch = g_app.load_trace.channel_epoch;
    if (navigation.timeline) application_message_mutation_notice_clear(ApplicationMessageMutationKind::navigate_message,
        navigation.timeline_source.channel_id, navigation.timeline_source.message_id);
    application_message_mutation_notice_begin(ApplicationMessageMutationKind::navigate_message,
        navigation.target_channel, navigation.target_message);
    notice(utf8_literal("Opening the message…"));
    if (!application_navigate_known_message(channel->id, navigation.target_message)) {
        navigation.selecting = false;
        finish(utf8_literal("The message could not be opened. Try Jump again."));
    } else {
        navigation.target_epoch = g_app.load_trace.channel_epoch;
        navigation.selecting = false;
        if (navigation.phase == MessageNavigationPhase::history && !g_app.pending_message_jump_message_id && !navigation.request_id)
            finish({}); // The real navigation helper already scrolled a resident message.
        else if (navigation.phase != MessageNavigationPhase::idle) {
            application_message_mutation_notice_pending(ApplicationMessageMutationKind::navigate_message,
                navigation.target_channel, navigation.target_message);
            refresh_message_composer_ui();
            application_request_frame();
        }
    }
    return true;
}
void retire() noexcept {
    if (navigation.request_id && !navigation.cancelled && g_app.account.instance == navigation.account_instance) {
        if (navigation.history_request && g_app.account.api.message_search)
            g_app.account.api.message_search(g_app.account.instance, AccountMessageSearchOperation::cancel,
                                             nullptr, navigation.request_id);
        else if (g_app.account.api.pinned_messages)
            g_app.account.api.pinned_messages(g_app.account.instance, nullptr, navigation.request_id);
        navigation.cancelled = true;
    }
    if (g_app.pending_message_jump_channel_id == navigation.target_channel &&
        g_app.pending_message_jump_message_id == navigation.target_message) {
        g_app.pending_message_jump_channel_id = 0;
        g_app.pending_message_jump_message_id = 0;
    }
    navigation.phase = MessageNavigationPhase::idle;
    navigation.retry_at = 0;
    if (g_app.account.instance != navigation.account_instance) navigation.request_id = 0;
    // Keep the ID until the scheduler drains its exact owned completion.
}
}

bool application_search_navigation_begin(const ApplicationSearchSourceTicket& ticket) noexcept {
    ApplicationSearchNavigationSource source{};
    if (!application_search_navigation_source(ticket, &source)) return true;
    return application_search_navigation_link(ticket, {source.guild_id, source.channel_id, ticket.message_id});
}
namespace {
bool begin_navigation(const ApplicationSearchNavigationSource& source, const MessageLink& target,
    const ApplicationMessageNavigationSource* timeline) noexcept {
    application_search_navigation_synchronize();
    if (!g_app.account_ui) return true;
    if (timeline && !timeline_current(*timeline)) return true;
    if (navigation.request_id || navigation.phase != MessageNavigationPhase::idle) {
        if (timeline && navigation.phase == MessageNavigationPhase::idle) {
            // The retired flight owns only its exact drain ID. Its former UI
            // scope may be gone, so this attempt owns a transient busy notice.
            const auto& shown = g_app.message_mutation_notice;
            if (shown.kind != ApplicationMessageMutationKind::navigate_message ||
                shown.phase != ApplicationMessageMutationNoticePhase::pending ||
                shown.navigation_stage != ApplicationMessageNavigationStage::busy ||
                shown.shown.account_serial != timeline->account_scope || shown.shown.profile_id != timeline->profile_id ||
                shown.shown.self_user_id != timeline->self_user_id || shown.shown.channel_id != timeline->channel_id ||
                shown.shown.channel_generation != timeline->channel_generation || shown.shown.message_id != timeline->message_id) {
                application_message_mutation_notice_begin(ApplicationMessageMutationKind::navigate_message,
                    timeline->channel_id, timeline->message_id);
                application_message_mutation_notice_navigation_pending(timeline->channel_id, timeline->message_id,
                    ApplicationMessageNavigationStage::busy);
                refresh_message_composer_ui(); application_request_frame();
            }
        } else if (timeline) notice(utf8_literal("Another message is opening. Try again in a moment."), ApplicationMessageNavigationStage::busy);
        else application_search_source_notice(source.ticket, utf8_literal("Another message is opening. Try again in a moment."));
        return true;
    }
    reset_value(&navigation);
    navigation.source = source;
    navigation.timeline = timeline != nullptr;
    if (timeline) {
        navigation.timeline_source = *timeline;
        navigation.source.channel_id = timeline->channel_id;
        navigation.source.guild_id = chat_state_store_find_channel(g_app.account_ui->chat_state, timeline->channel_id)->guild_id;
    }
    navigation.account_ui = g_app.account_ui;
    navigation.source_epoch = g_app.load_trace.channel_epoch;
    navigation.account_instance = g_app.account.instance;
    navigation.target_channel = target.channel_id;
    navigation.target_guild = target.guild_id;
    navigation.target_message = target.message_id;
    navigation.account_scope = g_app.plugin_account_scope_serial;
    navigation.profile = g_app.account.status.profile_id;
    navigation.self = g_app.account_ui->chat_state.self_user_id;
    navigation.phase = MessageNavigationPhase::channel;
    if (timeline) application_message_mutation_notice_begin(ApplicationMessageMutationKind::navigate_message,
        timeline->channel_id, timeline->message_id);
    if (!target.channel_id || !target.message_id) { finish(utf8_literal("This message link is invalid."), ApplicationMessageNavigationStage::error); return true; }
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, target.channel_id);
    const auto* target_guild = target.guild_id ? chat_state_store_find_guild(g_app.account_ui->chat_state, target.guild_id) : nullptr;
    if ((target.guild_id && (!target_guild || !target_guild->occupied || target_guild->unavailable)) ||
        (channel && channel->guild_id != target.guild_id)) {
        finish(utf8_literal("This channel isn't available on this account."), ApplicationMessageNavigationStage::message_missing, 404);
        return true;
    }
    if (channel) {
        if (!channel_is_thread(channel->type) || channel->permissions_resolved) return navigate();
        notice(utf8_literal("Loading thread…"), ApplicationMessageNavigationStage::waiting_channel);
        return true;
    }
    const auto* guild = chat_state_store_find_guild(g_app.account_ui->chat_state, target.guild_id);
    if (!target.guild_id || !guild || guild->unavailable || !g_app.account.api.channel_overview) {
        finish(utf8_literal("This channel isn't available on this account."), ApplicationMessageNavigationStage::message_missing, 404);
        return true;
    }
    notice(utf8_literal("Loading channel…"), ApplicationMessageNavigationStage::waiting_channel);
    return true;
}
}
bool application_search_navigation_link(const ApplicationSearchSourceTicket& ticket, const MessageLink& target) noexcept {
    ApplicationSearchNavigationSource source{};
    if (!application_search_navigation_source(ticket, &source)) return true;
    return begin_navigation(source, target, nullptr);
}
bool application_message_navigation_source(u64 channel, u64 message, MessageHandle handle, u32 revision,
    ApplicationMessageNavigationSource* output) noexcept {
    if (!output || !g_app.account_ui || !g_app.account.instance) return false;
    const auto* row = chat_state_store_find_channel(g_app.account_ui->chat_state, channel);
    if (!row) return false;
    ApplicationMessageNavigationSource source{channel, message, handle, revision,
        g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
        g_app.account_ui->chat_state.self_user_id, g_app.account.instance, g_app.account_ui,
        g_app.load_trace.channel_epoch, row->generation};
    if (!source.account_scope || !source.profile_id || !source.self_user_id || !timeline_current(source)) return false;
    *output = source; return true;
}
bool application_message_navigation_link(const ApplicationMessageNavigationSource& source,
    const MessageLink& target) noexcept {
    if (!timeline_current(source)) return true;
    return begin_navigation({}, target, &source);
}
void application_search_navigation_synchronize() noexcept {
    if (navigation.phase == MessageNavigationPhase::idle) return;
    if (!current()) {
        if (navigation.timeline && !navigation.admitted) {
            if (timeline_current(navigation.timeline_source))
                finish(utf8_literal("This channel isn't available on this account."), ApplicationMessageNavigationStage::message_missing, 404);
            else finish(utf8_literal("The source message changed. Try the link again."), ApplicationMessageNavigationStage::source_changed);
        }
        if (navigation.phase == MessageNavigationPhase::membership) {
            finish(utf8_literal("You don't have access to this private thread."));
            return;
        }
        if (navigation.admitted && g_app.account_ui && g_app.account.instance == navigation.account_instance &&
            g_app.plugin_account_scope_serial == navigation.account_scope &&
            g_app.account.status.profile_id == navigation.profile &&
            g_app.account_ui->chat_state.self_user_id == navigation.self &&
            g_app.account_ui->active_channel_id == navigation.target_channel) {
            RestClientResponse denied{}; denied.status_code = 403;
            application_message_mutation_notice_failed(ApplicationMessageMutationKind::navigate_message,
                navigation.target_channel, navigation.target_message, denied);
            refresh_message_composer_ui(); application_request_frame();
        }
        retire(); return;
    }
    if (navigation.phase == MessageNavigationPhase::membership) {
        if (!navigation.membership.serial) {
            if (navigation.retry_at <= clock_monotonic_milliseconds()) navigate();
            return;
        }
        switch (application_thread_actions_membership(navigation.membership)) {
        case ApplicationThreadMembershipStatus::joined: navigate(); return;
        case ApplicationThreadMembershipStatus::pending: return;
        case ApplicationThreadMembershipStatus::not_joined:
            finish(utf8_literal("You are not a member of this private thread."), ApplicationMessageNavigationStage::permission_denied, 403); return;
        case ApplicationThreadMembershipStatus::failed:
            finish(utf8_literal("Couldn't check access. Try again."), ApplicationMessageNavigationStage::failure_membership); return;
        case ApplicationThreadMembershipStatus::unknown:
        case ApplicationThreadMembershipStatus::stale:
            finish(utf8_literal("Private thread access changed. Try Jump again."), ApplicationMessageNavigationStage::failure_membership); return;
        }
    }
    if (navigation.phase == MessageNavigationPhase::history_retry &&
        navigation.retry_at <= clock_monotonic_milliseconds()) {
        navigation.phase = MessageNavigationPhase::history;
        navigation.retry_at = 0;
        g_app.pending_message_jump_channel_id = navigation.target_channel;
        g_app.pending_message_jump_message_id = navigation.target_message;
        notice(utf8_literal("Opening the message…"));
    }
}
void application_search_navigation_release() noexcept {
    // Intentional selection can retire the Search source synchronously. Once
    // destination admission owns the action, source UI retirement is harmless.
    if (navigation.admitted && current()) return;
    retire();
}
void application_message_navigation_release() noexcept { retire(); }
bool application_message_navigation_active() noexcept { return navigation.phase != MessageNavigationPhase::idle; }
bool application_message_navigation_observation_needed() noexcept { return application_message_navigation_active() || navigation.request_id; }
u32 application_message_navigation_owner_bytes() noexcept { return sizeof(MessageNavigation); }
bool application_message_navigation_cancel() noexcept {
    if (!application_message_navigation_active()) return false;
    notice(utf8_literal("Opening the message was cancelled."), ApplicationMessageNavigationStage::cancelled);
    retire(); return true;
}
void application_message_navigation_gateway_reset() noexcept { retire(); }
void application_message_navigation_gateway_deleted(u64 guild, u64 channel) noexcept {
    if (!channel || !application_message_navigation_active() || !g_app.account_ui ||
        g_app.account.instance != navigation.account_instance || g_app.account_ui != navigation.account_ui ||
        g_app.plugin_account_scope_serial != navigation.account_scope ||
        g_app.account.status.profile_id != navigation.profile || g_app.account_ui->chat_state.self_user_id != navigation.self) return;
    const bool source = !navigation.admitted && channel == navigation.source.channel_id && guild == navigation.source.guild_id;
    if (guild != navigation.target_guild && !source) return;
    if (!deleted(channel)) {
        bool recorded{};
        for (auto& slot : navigation.deleted_channels) if (!slot) { slot = channel; recorded = true; break; }
        if (!recorded) navigation.deleted_all = true;
    }
    if (source || deleted(navigation.target_channel) || (navigation.parent && deleted(navigation.parent))) {
        finish(utf8_literal("This channel is no longer available."), ApplicationMessageNavigationStage::message_missing, 404);
        retire();
    }
}
void application_message_navigation_message_deleted(u64 channel, u64 message) noexcept {
    if (!message || !channel || !application_message_navigation_active() || !g_app.account_ui ||
        g_app.account_ui != navigation.account_ui || g_app.account.instance != navigation.account_instance ||
        g_app.plugin_account_scope_serial != navigation.account_scope ||
        g_app.account.status.profile_id != navigation.profile ||
        g_app.account_ui->chat_state.self_user_id != navigation.self) return;
    const bool source = !navigation.admitted && channel == navigation.source.channel_id &&
        message == (navigation.timeline ? navigation.timeline_source.message_id : navigation.source.ticket.message_id);
    if (!source && (channel != navigation.target_channel || message != navigation.target_message)) return;
    finish(utf8_literal("This message is no longer available."), ApplicationMessageNavigationStage::message_missing, 404);
    retire();
}
bool application_message_navigation_history_current(u64 id, u64 channel, u64 message) noexcept {
    if (!navigation.request_id || id != navigation.request_id || !navigation.history_request) return true;
    return navigation.phase == MessageNavigationPhase::history && !navigation.cancelled &&
        channel == navigation.target_channel && message == navigation.target_message && current();
}
bool application_search_navigation_pending(u64 now) noexcept {
    application_search_navigation_synchronize();
    return (navigation.phase == MessageNavigationPhase::channel || navigation.phase == MessageNavigationPhase::parent) &&
        !navigation.request_id && (!navigation.retry_at || navigation.retry_at <= now);
}
u64 application_search_navigation_next_wake() noexcept {
    application_search_navigation_synchronize();
    if (navigation.request_id) return 0;
    switch (navigation.phase) {
    case MessageNavigationPhase::channel:
    case MessageNavigationPhase::parent:
        // A busy DETAILS lane wakes the loop when its request completes.
        return g_app.details_request_owner == MessageHistoryRequestOwner::none ? maximum(navigation.retry_at, u64{1}) : 0;
    case MessageNavigationPhase::membership: return navigation.membership.serial ? 0 : navigation.retry_at;
    case MessageNavigationPhase::history_retry: return navigation.retry_at;
    default: return 0;
    }
}
bool application_search_navigation_submit(u64 id, u64 now) noexcept {
    if (!id || !application_search_navigation_pending(now)) return false;
    if (!g_app.account.api.channel_overview) { finish(utf8_literal("Couldn't load the channel.")); return false; }
    const u64 target = navigation.phase == MessageNavigationPhase::parent ? navigation.parent : navigation.target_channel;
    const auto* resident = chat_state_store_find_channel(g_app.account_ui->chat_state, target);
    const u16 generation = resident ? resident->generation : u16{};
    const u32 revision = resident ? resident->revision : u32{};
    const auto submitted = g_app.account.api.channel_overview(g_app.account.instance,
        ChannelOverviewOperation::fetch, target, nullptr, id);
    if (!submitted.ok()) {
        if (submitted.error == RestClientSubmitError::busy) {
            navigation.retry_at = deadline_after(now, 50);
            notice(utf8_literal("Loading channel…"), ApplicationMessageNavigationStage::waiting_channel);
        } else finish(utf8_literal("Couldn't load the channel. Try again."));
        return false;
    }
    navigation.request_id = id;
    navigation.history_request = false;
    navigation.requested_channel = target;
    navigation.cancelled = false;
    navigation.retry_at = 0;
    navigation.resident_generation = generation;
    navigation.resident_revision = revision;
    if (!current()) { retire(); return true; }
    notice(utf8_literal("Loading channel…"), navigation.phase == MessageNavigationPhase::parent ?
        ApplicationMessageNavigationStage::loading_parent : ApplicationMessageNavigationStage::loading_channel);
    return true;
}
bool application_search_navigation_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!response.request_id || response.request_id != navigation.request_id || navigation.history_request) return false;
    const bool valid = current();
    navigation.request_id = 0;
    if (!valid || navigation.cancelled) {
        if (!navigation.cancelled && navigation.timeline && !navigation.admitted)
            finish(utf8_literal("The source message changed. Try the link again."), ApplicationMessageNavigationStage::source_changed);
        navigation.phase = MessageNavigationPhase::idle; return true;
    }
    if (const u64 retry_at = rest_rate_retry_deadline(response)) {
        navigation.retry_at = retry_at;
        notice(utf8_literal("Too many requests. Retrying…"), ApplicationMessageNavigationStage::rate_limited, 429);
        return true;
    }
    if (response.status_code == 403) { finish(utf8_literal("You no longer have permission to view this channel."), ApplicationMessageNavigationStage::permission_denied, 403); return true; }
    if (response.status_code == 404) { finish(utf8_literal("This channel is no longer available."), ApplicationMessageNavigationStage::message_missing, 404); return true; }
    if (!response.transport_ok() || response.status_code != 200 || body.bytes != response.body_bytes || !body.data || !body.bytes) {
        finish(utf8_literal("Couldn't load the channel. Try again.")); return true;
    }
    auto& ui = *g_app.account_ui;
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    ConstSlice<u8> payload{};
    const auto routed = gateway_client_route_channel_create({body, 0, JsonValueKind::object},
        {ui.message_history_page_structurals, kMessageHistoryPageStructuralCount}, 32,
        {reinterpret_cast<char*>(&ui.projection_scratch), sizeof(ui.projection_scratch)}, navigation.self,
        &event, &payload);
    if (!routed.ok() || event.state.id != navigation.requested_channel ||
        event.state.guild_id != navigation.target_guild || !event.state.guild_id ||
        (event.state.flags & kChannelObfuscatedFlag) ||
        (navigation.phase == MessageNavigationPhase::parent && channel_is_thread(event.state.type))) {
        finish(utf8_literal("Couldn't load the channel. Try again.")); return true;
    }
    const auto* resident = chat_state_store_find_channel(ui.chat_state, event.state.id);
    // A Gateway publication received while GET was outstanding is newer authority.
    // Its record must not be overwritten by an older REST snapshot.
    const bool newer = resident && (!navigation.resident_generation ||
        resident->generation != navigation.resident_generation || resident->revision != navigation.resident_revision);
    const u8 type = newer ? resident->type : event.state.type;
    const u64 parent = newer ? resident->parent_id : event.state.parent_id;
    if (deleted(event.state.id) || (channel_is_thread(type) && deleted(parent))) {
        finish(utf8_literal("This thread or its parent is no longer available."), ApplicationMessageNavigationStage::failure_parent, 404); return true;
    }
    if (resident && resident->guild_id != navigation.target_guild) {
        finish(utf8_literal("This channel is no longer in this server.")); return true;
    }
    if (navigation.phase == MessageNavigationPhase::channel && channel_is_thread(type))
        navigation.parent = parent;
    if (channel_is_thread(type) && !chat_state_store_find_channel(ui.chat_state, parent)) {
        if (!parent || navigation.parent_fetched || parent == navigation.target_channel) {
            finish(utf8_literal("The parent channel for this thread is unavailable."), ApplicationMessageNavigationStage::failure_parent); return true;
        }
        navigation.parent = parent;
        navigation.phase = MessageNavigationPhase::parent;
        notice(utf8_literal("Loading channel…"), ApplicationMessageNavigationStage::loading_parent);
        return true;
    }
    event.dispatch_payload_bytes = payload.count;
    if (!newer && !chat_state_apply_gateway_event(&ui.chat_state, event, payload,
            {ui.guild_id_scratch, kChatStateGuildCapacity}, &ui.user_store).ok()) {
        finish(utf8_literal("Couldn't load the channel. Try again.")); return true;
    }
    if (navigation.phase == MessageNavigationPhase::parent) {
        const auto* accepted = chat_state_store_find_channel(ui.chat_state, navigation.parent);
        constexpr u32 required = channel_action_view | channel_action_read_history;
        if (!accepted || accepted->guild_id != navigation.target_guild || !accepted->permissions_resolved ||
            !channel_is_thread_parent(accepted->type) ||
            (accepted->actions & required) != required ||
            !chat_state_store_channel_visible(ui.chat_state, *accepted)) {
            finish(utf8_literal("You cannot view this thread’s parent channel."), ApplicationMessageNavigationStage::permission_denied, 403); return true;
        }
        navigation.parent_fetched = true;
        navigation.phase = MessageNavigationPhase::channel;
        notice(utf8_literal("Loading thread…"), ApplicationMessageNavigationStage::loading_channel);
        return true;
    }
    return navigate();
}
void application_search_navigation_history_submitted(u64 channel, u64 message, u64 id) noexcept {
    if (id && navigation.admitted && !navigation.request_id && channel == navigation.target_channel &&
        message == navigation.target_message) {
        navigation.request_id = id;
        navigation.history_request = true;
        // Cancellation/source retirement can occur synchronously inside typed
        // PRIMARY admission. Retain and cancel the accepted exact ID anyway.
        if (navigation.phase != MessageNavigationPhase::history || !current()) retire();
    }
}
bool application_search_navigation_history_admission_failed(u64 channel, u64 message, bool busy) noexcept {
    if (navigation.phase != MessageNavigationPhase::history || channel != navigation.target_channel ||
        message != navigation.target_message) return false;
    if (busy) {
        navigation.phase = MessageNavigationPhase::history_retry;
        navigation.retry_at = deadline_after(clock_monotonic_milliseconds(), 50);
        notice(utf8_literal("Loading messages…"));
    } else finish(utf8_literal("Couldn't load messages. Try again."));
    g_app.pending_message_jump_channel_id = g_app.pending_message_jump_message_id = 0;
    return true;
}
void application_search_navigation_history_response(const RestClientResponse& response,
                                                      const MessageHistoryRestResult& accepted) noexcept {
    if (!response.request_id || navigation.request_id != response.request_id || !navigation.history_request) return;
    const bool valid = current();
    navigation.request_id = 0;
    if (!valid) { retire(); return; }
    if (const u64 retry_at = rest_rate_retry_deadline(response)) {
        navigation.phase = MessageNavigationPhase::history_retry;
        navigation.retry_at = retry_at;
        notice(utf8_literal("Too many requests. Retrying…"), ApplicationMessageNavigationStage::rate_limited, 429);
        application_message_mutation_notice_pending(ApplicationMessageMutationKind::navigate_message,
            navigation.target_channel, navigation.target_message, 429);
    } else if (!response.transport_ok() || response.status_code != 200 || !accepted.accepted() ||
               accepted.disposition != MessageHistoryRestDisposition::applied) {
        finish(response.status_code == 403 ? utf8_literal("You cannot read this channel’s message history.") :
            response.status_code == 404 ? utf8_literal("This message is no longer available.") :
            utf8_literal("Couldn't find the message. Try again."),
            response.status_code == 403 ? ApplicationMessageNavigationStage::permission_denied :
            response.status_code == 404 ? ApplicationMessageNavigationStage::message_missing :
            ApplicationMessageNavigationStage::error, response.status_code);
    } else finish({});
    refresh_message_composer_ui();
    application_request_frame();
}
} // namespace maicord::app_detail
