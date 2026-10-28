#include "application_stage.hpp"
#include "application_call.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_voice_channels.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/stage_policy.hpp"
#include "maicord/protocol/stage_channel.hpp"

#include "maicord/core/memory.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {
constexpr u32 kQueueCapacity = 8;
constexpr u64 kNoticeMs = 6'000;
constexpr u64 kBusyRetryMs = 250;

static_assert(kStageVoiceStatusMaximumBytes >= kStageTopicMaximumBytes);
struct QueuedRequest {
    StageRequest request;
    char topic[kStageVoiceStatusMaximumBytes]; // A stage topic or a voice channel status.
    u64 account_scope;
};
struct TopicForm {
    u64 serial, channel_id, guild_id, account_scope;
    u64 revision;
    bool start;
    Utf8View status;
};
struct StageState {
    StageInstanceStore instances;
    QueuedRequest queue[kQueueCapacity];
    u32 head, count;
    u64 in_flight_request, retry_at_ms;
    StageOperation in_flight;
    u64 in_flight_channel;
    // Call view projection storage.
    UiCallStagePerson audience[kUiCallStageAudienceShown], requests[kUiCallStageRequestsShown];
    char notice[kUiCallStageNoticeBytes];
    u32 notice_bytes;
    u64 notice_channel, notice_until_ms;
    TopicForm form;
    u64 form_serial;
    u64 invited_channel; // The stage whose invite was last shown.
};
StageState stage{};

u64 now_ms() noexcept {
    return clock_monotonic_milliseconds();
}
const ChannelRecord* channel_of(u64 channel_id) noexcept {
    if (!g_app.account_ui || !channel_id)
        return nullptr;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    return channel && channel->type == kStageChannelType && channel->guild_id ? channel : nullptr;
}
PermissionSet self_permissions(const ChannelRecord& channel) noexcept {
    return channel.permissions_resolved ? channel.effective_permissions : 0;
}
const VoiceState* self_state(const ChannelRecord& channel) noexcept {
    const auto& ui = *g_app.account_ui;
    const auto* state = voice_state_store_find(ui.voice_states, channel.guild_id, ui.chat_state.self_user_id);
    return state && state->channel_id == channel.id ? state : nullptr;
}
// The local user's role from the roster, else the voice session's own flags.
StageRole self_role(const ChannelRecord& channel, bool* hand = nullptr) noexcept {
    const auto* state = self_state(channel);
    const auto& s = g_app.account_ui->voice_session;
    const bool session = s.channel_id == channel.id && s.phase != VoiceSetupPhase::idle &&
                         (s.present_flags & gateway_voice_suppress);
    const auto role = state || session ? stage_role_flags(state ? state->flags : s.flags) : StageRole::none;
    if (hand)
        *hand = role == StageRole::requested;
    return role;
}
void set_notice(u64 channel_id, Utf8View text) noexcept {
    stage.notice_bytes = minimum(text.bytes, kUiCallStageNoticeBytes);
    copy_bytes(stage.notice, text.data, stage.notice_bytes);
    stage.notice_channel = channel_id;
    stage.notice_until_ms = now_ms() + kNoticeMs;
    application_repaint();
}
Utf8View failure_text(StageOperation operation) noexcept {
    switch (operation) {
    case StageOperation::request_to_speak:
        return utf8_literal("Couldn't request to speak");
    case StageOperation::lower_hand:
        return utf8_literal("Couldn't lower your hand");
    case StageOperation::become_speaker:
        return utf8_literal("Couldn't join the speakers");
    case StageOperation::decline_invite:
    case StageOperation::move_self_to_audience:
    case StageOperation::move_to_audience:
        return utf8_literal("Couldn't move to the audience");
    case StageOperation::invite_to_speak:
        return utf8_literal("Couldn't invite them to speak");
    case StageOperation::start:
        return utf8_literal("Couldn't start the stage");
    case StageOperation::update_topic:
        return utf8_literal("Couldn't change the topic");
    case StageOperation::end:
        return utf8_literal("Couldn't end the stage");
    default:
        return utf8_literal("Something went wrong");
    }
}
bool enqueue(StageRequest request) noexcept {
    if (stage.count == kQueueCapacity || !stage_request_valid(request))
        return false;
    auto& slot = stage.queue[(stage.head + stage.count) % kQueueCapacity];
    reset_value(&slot);
    if (request.topic.bytes) {
        copy_bytes(slot.topic, request.topic.data, request.topic.bytes);
        request.topic = {slot.topic, request.topic.bytes};
    }
    slot.request = request;
    slot.account_scope = g_app.plugin_account_scope_serial;
    ++stage.count;
    stage.retry_at_ms = 0;
    application_request_frame();
    return true;
}
bool queue_simple(const ChannelRecord& channel, StageOperation operation, u64 user_id = 0) noexcept {
    StageRequest request{};
    request.operation = operation;
    request.guild_id = channel.guild_id;
    request.channel_id = channel.id;
    request.user_id = user_id;
    request.unix_ms = clock_unix_milliseconds();
    return enqueue(request);
}
void drop_head() noexcept {
    secure_clear_bytes(&stage.queue[stage.head], sizeof(stage.queue[stage.head]));
    stage.head = (stage.head + 1) % kQueueCapacity;
    --stage.count;
}

// Start / edit topic form ---------------------------------------------------
bool form_valid(TextFormOwnerKey key) noexcept {
    const auto& f = stage.form;
    const auto* channel = channel_of(f.channel_id);
    return f.serial && key.scope == f.serial && key.collection == f.channel_id && channel &&
           f.account_scope == g_app.plugin_account_scope_serial && !g_app.login.visible &&
           stage_moderator(self_permissions(*channel));
}
bool form_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_valid(key))
        return false;
    auto& f = stage.form;
    if (!stage_topic_valid(text)) {
        f.status = utf8_literal("Topics need 1-120 characters on one line.");
        ++f.revision;
        return false;
    }
    StageRequest request{};
    request.operation = f.start ? StageOperation::start : StageOperation::update_topic;
    request.guild_id = f.guild_id;
    request.channel_id = f.channel_id;
    const auto* instance = stage_instance_store_find(stage.instances, f.channel_id);
    request.privacy_level = instance && instance->privacy_level ? instance->privacy_level : kStagePrivacyGuildOnly;
    request.topic = text;
    if (!enqueue(request)) {
        f.status = utf8_literal("Too many stage changes at once. Try again.");
        ++f.revision;
        return false;
    }
    return true;
}
ApplicationTextFormProgress form_progress(TextFormOwnerKey key) noexcept {
    ApplicationTextFormProgress view{};
    view.revision = stage.form.revision;
    view.complete = !form_valid(key);
    view.status = stage.form.status;
    return view;
}
void form_closed(TextFormOwnerKey key) noexcept {
    if (key.scope == stage.form.serial)
        reset_value(&stage.form);
}
// Binds a fresh form owner to the channel.
void begin_form(const ChannelRecord& channel) noexcept {
    reset_value(&stage.form);
    stage.form.serial = ++stage.form_serial ? stage.form_serial : ++stage.form_serial;
    stage.form.channel_id = channel.id;
    stage.form.guild_id = channel.guild_id;
    stage.form.account_scope = g_app.plugin_account_scope_serial;
}
bool open_topic_form(const ChannelRecord& channel, bool start) noexcept {
    if (g_app.text_form)
        return false;
    begin_form(channel);
    stage.form.start = start;
    const auto* instance = stage_instance_store_find(stage.instances, channel.id);
    ApplicationTextFormRequest request{};
    request.title = start ? utf8_literal("Start stage") : utf8_literal("Edit topic");
    request.context = start ? utf8_literal("Tell listeners what this stage is about.") : Utf8View{};
    request.label = utf8_literal("Topic");
    request.accept = start ? utf8_literal("Start stage") : utf8_literal("Save");
    request.initial = !start && instance ? stage_instance_topic(*instance) : Utf8View{};
    request.owner = {stage.form.serial, channel.id, start ? 1u : 2u};
    request.valid = form_valid;
    request.save = form_save;
    request.progress = form_progress;
    request.closed = form_closed;
    request.maximum_codepoints = kStageTopicMaximumCodepoints;
    request.maximum_bytes = kStageTopicMaximumBytes;
    request.single_line = true;
    if (application_text_form_open(request))
        return true;
    reset_value(&stage.form);
    return false;
}
// End stage confirmation.
bool end_valid(TextFormOwnerKey key) noexcept {
    return form_valid(key) && stage_instance_store_find(stage.instances, stage.form.channel_id);
}
bool end_save(TextFormOwnerKey key, Utf8View) noexcept {
    const auto* channel = end_valid(key) ? channel_of(stage.form.channel_id) : nullptr;
    return channel && queue_simple(*channel, StageOperation::end);
}
bool open_end_confirmation(const ChannelRecord& channel) noexcept {
    if (g_app.text_form)
        return false;
    begin_form(channel);
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("End stage?");
    request.context = utf8_literal("The stage ends for everyone. Speakers move back to the audience.");
    request.accept = utf8_literal("End stage");
    request.owner = {stage.form.serial, channel.id, 3};
    request.valid = end_valid;
    request.save = end_save;
    request.closed = form_closed;
    request.confirmation_only = true;
    request.danger = true;
    if (application_text_form_open(request))
        return true;
    reset_value(&stage.form);
    return false;
}
} // namespace

void application_stage_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (event.kind == GatewayClientEventKind::ready) {
        stage_instance_store_reset(&stage.instances);
        return;
    }
    if (event.kind != GatewayClientEventKind::dispatch)
        return;
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_delete) {
        if (stage_instance_store_clear_guild(&stage.instances, event.state.id))
            application_repaint();
        return;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::channel_delete) {
        if (stage_instance_store_remove(&stage.instances, event.state.id))
            application_repaint();
        return;
    }
    if (event.dispatch_kind != GatewayClientDispatchKind::stage_instance_snapshot)
        return;
    const auto& header = event.voice_states;
    constexpr u32 record_bytes = sizeof(GatewayClientStageInstanceRecord);
    if (!payload.data || payload.count < header.record_count * record_bytes)
        return;
    const u32 revision = stage.instances.revision;
    if (header.replace)
        stage_instance_store_clear_guild(&stage.instances, header.guild_id);
    for (u32 i = 0; i < header.record_count; ++i) {
        GatewayClientStageInstanceRecord record;
        copy_bytes(&record, payload.data + i * record_bytes, record_bytes);
        stage_instance_store_apply(&stage.instances, record.id, record.guild_id, record.channel_id,
                                   record.privacy_level,
                                   {record.topic, minimum<u32>(record.topic_bytes, kGatewayClientStageTopicBytes)});
    }
    if (revision != stage.instances.revision)
        application_repaint();
}

const StageInstanceStore& application_stage_instances() noexcept {
    return stage.instances;
}

bool application_stage_queue_voice_status(u64 guild_id, u64 channel_id, Utf8View status) noexcept {
    StageRequest request{};
    request.operation = StageOperation::voice_status;
    request.guild_id = guild_id;
    request.channel_id = channel_id;
    request.topic = status;
    return enqueue(request);
}

bool application_stage_channel(u64 channel_id) noexcept {
    return channel_of(channel_id) != nullptr;
}

bool application_stage_speaker(u32 voice_flags) noexcept {
    return stage_role_flags(voice_flags) == StageRole::speaker;
}

UiCallStage application_stage_content(u64 channel_id) noexcept {
    UiCallStage content{};
    const auto* channel = channel_of(channel_id);
    if (!channel)
        return content;
    auto& ui = *g_app.account_ui;
    const auto* instance = stage_instance_store_find(stage.instances, channel_id);
    content.active = true;
    content.live = instance != nullptr;
    content.topic = instance ? stage_instance_topic(*instance) : Utf8View{};
    const auto role = self_role(*channel, &content.hand_raised);
    content.actions = stage_self_actions(role, self_permissions(*channel), content.live);
    const VoiceState* audience[kUiCallStageAudienceShown]{};
    const VoiceState* requests[kUiCallStageRequestsShown]{};
    const auto roster = stage_roster(ui.voice_states, channel_id, {}, {audience, kUiCallStageAudienceShown},
                                     {requests, kUiCallStageRequestsShown});
    const auto person = [&](const VoiceState* state) noexcept {
        const auto* user = user_store_find(ui.user_store, state->user_id);
        return UiCallStagePerson{state->user_id, user ? user_store_name(ui.user_store, *user) : Utf8View{}};
    };
    const u32 audience_shown = minimum(roster.audience, kUiCallStageAudienceShown);
    for (u32 i = 0; i < audience_shown; ++i)
        stage.audience[i] = person(audience[i]);
    const u32 request_shown = minimum(roster.requests, kUiCallStageRequestsShown);
    for (u32 i = 0; i < request_shown; ++i)
        stage.requests[i] = person(requests[i]);
    content.audience_count = roster.audience;
    content.audience = {stage.audience, audience_shown};
    // Only moderators act on hand raises.
    if (content.actions & stage_action_invite_others) {
        content.request_count = roster.requests;
        content.requests = {stage.requests, request_shown};
    }
    if (stage.notice_bytes && stage.notice_channel == channel_id && now_ms() < stage.notice_until_ms)
        content.notice = {stage.notice, stage.notice_bytes};
    return content;
}

bool application_stage_activate(u64 channel_id, UiCallControl control, u64 user_id) noexcept {
    const auto* channel = channel_of(channel_id);
    if (!channel)
        return false;
    bool hand = false;
    const auto role = self_role(*channel, &hand);
    const bool live = stage_instance_store_find(stage.instances, channel_id) != nullptr;
    const u32 actions = stage_self_actions(role, self_permissions(*channel), live);
    const auto allowed = [&](u32 action) noexcept { return (actions & action) != 0; };
    switch (control) {
    case UiCallControl::stage_hand:
        if (hand && allowed(stage_action_lower_hand))
            return queue_simple(*channel, StageOperation::lower_hand);
        return allowed(stage_action_raise_hand) && queue_simple(*channel, StageOperation::request_to_speak);
    case UiCallControl::stage_become:
    case UiCallControl::stage_accept:
        return allowed(control == UiCallControl::stage_accept ? stage_action_accept_invite
                                                              : stage_action_become_speaker) &&
               queue_simple(*channel, StageOperation::become_speaker);
    case UiCallControl::stage_decline:
        return allowed(stage_action_decline_invite) && queue_simple(*channel, StageOperation::decline_invite);
    case UiCallControl::stage_audience:
        return allowed(stage_action_move_to_audience) && queue_simple(*channel, StageOperation::move_self_to_audience);
    case UiCallControl::stage_invite: {
        const auto* target = voice_state_store_find(g_app.account_ui->voice_states, channel->guild_id, user_id);
        return allowed(stage_action_invite_others) && target && target->channel_id == channel_id &&
               stage_role(target) == StageRole::requested &&
               queue_simple(*channel, StageOperation::invite_to_speak, user_id);
    }
    case UiCallControl::stage_move: {
        const auto* target = voice_state_store_find(g_app.account_ui->voice_states, channel->guild_id, user_id);
        return allowed(stage_action_move_others) && target && target->channel_id == channel_id &&
               stage_role(target) != StageRole::audience &&
               queue_simple(*channel, StageOperation::move_to_audience, user_id);
    }
    case UiCallControl::stage_start:
        return allowed(stage_action_start) && open_topic_form(*channel, true);
    case UiCallControl::stage_topic:
        return allowed(stage_action_edit_topic) && open_topic_form(*channel, false);
    case UiCallControl::stage_end:
        return allowed(stage_action_end) && open_end_confirmation(*channel);
    default:
        return false;
    }
}

void application_stage_pump() noexcept {
    if (!g_app.account_ui)
        return;
    const auto& s = g_app.account_ui->voice_session;
    const auto* channel = s.phase != VoiceSetupPhase::idle ? channel_of(s.channel_id) : nullptr;
    const bool invited = channel && self_role(*channel) == StageRole::invited;
    if (!invited) {
        stage.invited_channel = 0;
        return;
    }
    // Once per invitation, like the official client's prompt.
    if (stage.invited_channel == channel->id)
        return;
    stage.invited_channel = channel->id;
    if (!application_call_visible())
        application_call_open(channel->id, 0);
    application_repaint();
}

bool application_stage_pending(u64 now) noexcept {
    // Requests from a previous account scope are dropped unsent.
    while (stage.count && stage.queue[stage.head].account_scope != g_app.plugin_account_scope_serial)
        drop_head();
    return stage.count && !stage.in_flight_request && (!stage.retry_at_ms || now >= stage.retry_at_ms) &&
           g_app.account.instance && g_app.account.api.stage;
}

bool application_stage_submit(u64 request_id, u64 now) noexcept {
    if (!application_stage_pending(now))
        return false;
    auto& host = g_app.account;
    const auto& queued = stage.queue[stage.head];
    const auto submitted = host.api.stage(host.instance, &queued.request, request_id);
    if (submitted.error == RestClientSubmitError::busy) {
        stage.retry_at_ms = now + kBusyRetryMs;
        return false;
    }
    if (!submitted.ok()) {
        log_u64(LogLevel::warning, "stage", "request_refused", static_cast<u64>(submitted.error));
        if (queued.request.operation == StageOperation::voice_status)
            application_voice_channels_status_result(queued.request.channel_id, false, 0);
        else
            set_notice(queued.request.channel_id, failure_text(queued.request.operation));
        drop_head();
        return false;
    }
    stage.in_flight_request = request_id;
    stage.in_flight = queued.request.operation;
    stage.in_flight_channel = queued.request.channel_id;
    drop_head();
    return true;
}

bool application_stage_response(const RestClientResponse& response, Utf8View) noexcept {
    if (!stage.in_flight_request || response.request_id != stage.in_flight_request)
        return true;
    const auto operation = stage.in_flight;
    const u64 channel_id = stage.in_flight_channel;
    stage.in_flight_request = 0;
    const bool ok = response.error == RestClientError::none && response.status_code >= 200 &&
                    response.status_code < 300;
    if (operation == StageOperation::voice_status) {
        if (!ok)
            log_u64(LogLevel::warning, "stage", "voice_status_failed", response.status_code);
        application_voice_channels_status_result(channel_id, ok, response.status_code);
        return true;
    }
    if (!ok) {
        log_u64(LogLevel::warning, "stage", "request_failed", response.status_code);
        set_notice(channel_id, failure_text(operation));
        return true;
    }
    // Starting a stage puts its moderator on stage quietly, as the official client does.
    if (operation == StageOperation::start)
        if (const auto* channel = channel_of(channel_id); channel && self_role(*channel) != StageRole::speaker &&
                                                          self_role(*channel) != StageRole::none) {
            StageRequest request{};
            request.operation = StageOperation::become_speaker;
            request.guild_id = channel->guild_id;
            request.channel_id = channel->id;
            request.silent = true;
            enqueue(request);
        }
    application_request_frame();
    return true;
}

u64 application_stage_next_wake(bool lane_idle) noexcept {
    // An expired notice clears with one repaint; a live one wakes at expiry.
    const u64 now = now_ms();
    if (stage.notice_bytes && now >= stage.notice_until_ms) {
        stage.notice_bytes = 0;
        application_repaint();
    }
    u64 wake = stage.notice_bytes ? stage.notice_until_ms : 0;
    if (lane_idle && stage.count && !stage.in_flight_request) {
        const u64 retry = stage.retry_at_ms > now ? stage.retry_at_ms : now + 1;
        wake = wake && wake < retry ? wake : retry;
    }
    return wake;
}

void application_stage_release() noexcept {
    // In place: the state is too large for a temporary.
    const u64 serial = stage.form_serial;
    secure_clear_bytes(&stage, sizeof(stage));
    stage.form_serial = serial;
}
} // namespace maicord::app_detail
