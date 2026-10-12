#include "maicord/core/time.hpp"
#include "../src/app/application_notifications.hpp"
#include "../src/app/application_state.hpp"
#include "../src/app/gateway_ready_projection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"

#if !defined(MAICORD_NOTIFICATION_FIXTURE)
#error This integration fixture requires the nonemitting notification sink.
#endif

namespace maicord {
namespace notification_application_probe {
using namespace app_detail;
u32 taskbar_flashes;
struct Captured {
    DesktopNotificationActivation target;
    char title[256], body[1024];
    u32 title_bytes, body_bytes;
    bool silent, suppress_popup;
};
struct Fixture {
    AccountUiRuntime ui;
    GuildRecord guilds[4];
    ChannelRecord channels[32];
    RoleRecord roles[16];
    PermissionOverwriteRecord overwrites[32];
    u16 guild_order[4], channel_order[32];
    u8 names[8192];
    UserRecord users[16];
    u32 user_order[16];
    u8 user_names[4096];
    MessageStore messages;
    MessageRecord message_rows[8];
    u32 message_order[8], heights[8];
    u64 fenwick[8], bulk_ids[kGatewayBulkDeleteMaximumIds], guild_ids[4];
    u8 message_text[4096], pump_scratch[kMessageEventPumpPayloadCapacity];
    UiVariableList list;
    MessageTimeline timeline;
    u32 arrays[4096], records[4096];
    alignas(8) char payload[32 * 1024], header[4096], source[8192];
    Captured last;
    NotificationAudioRequest sounds[64];
    NotificationAudioStatus sound_status;
    NotificationAudioResult sound_result;
    u64 sound_now;
    u32 sound_count;
    GatewayClientEvent completion;
    MessageEventPumpConsumeResult consumed;
    u32 delivered, relationship_flags, navigation_calls, trace_lines;
    DesktopNotificationActivation removals[64];
    DesktopNotificationResult removal_result;
    u32 removal_count, show_action;
    bool relationship_known, hold_completion, sink_valid, trace_ready, auto_drain;
};
Fixture fixture{};

// Bounded semantic diagnostics only: no names, content, credentials, OS
// enrollment, or additional artifacts. Root can inspect the first READY's
// producer flags and public owner metrics without altering production state.
void trace(Utf8View phase, const GatewayClientEvent* event = nullptr) noexcept {
    auto& f = fixture;
    if (f.trace_lines == 32) return;
    ++f.trace_lines;
    char line[320];
    ProtocolWriter writer{{reinterpret_cast<u8*>(line), sizeof(line)}, 0};
    protocol_writer_append_literal(&writer, "notification_fixture ");
    protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(phase.data), phase.bytes);
    const auto number = [&](Utf8View label, u64 value) noexcept {
        protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(label.data), label.bytes);
        protocol_writer_append_u64(&writer, value);
    };
    if (event) {
        number(utf8_literal(" kind="), static_cast<u32>(event->kind));
        number(utf8_literal(" dispatch="), static_cast<u32>(event->dispatch_kind));
        number(utf8_literal(" flags="), event->notification.flags);
        number(utf8_literal(" input_overrides="), event->notification.override_count);
        number(utf8_literal(" bytes="), event->dispatch_payload_bytes);
        number(utf8_literal(" side_bytes="), event->notification_payload_bytes);
    }
    const auto metrics = application_notifications_fixture_metrics(&g_app);
    number(utf8_literal(" guilds="), metrics.guilds);
    number(utf8_literal(" overrides="), metrics.overrides);
    number(utf8_literal(" threads="), metrics.threads);
    number(utf8_literal(" ready="), metrics.ready);
    number(utf8_literal(" live="), metrics.live);
    number(utf8_literal(" seen="), metrics.seen_count);
    number(utf8_literal(" delivered="), f.delivered);
    number(utf8_literal(" pump_error="), static_cast<u32>(f.consumed.error));
    if (!event) {
        number(utf8_literal(" model_guilds="), f.ui.chat_state.guild_count);
        number(utf8_literal(" model_channels="), f.ui.chat_state.channel_count);
        number(utf8_literal(" self="), f.ui.chat_state.self_user_id);
        number(utf8_literal(" selected="), f.ui.chat_state.selected_channel_id);
    }
    protocol_writer_append_literal(&writer, "\r\n");
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, writer.used, &written, nullptr);
}
void memory_trace(Utf8View phase,const ApplicationNotificationFixtureMetrics& m) noexcept {
    char line[768]; ProtocolWriter w{{reinterpret_cast<u8*>(line),sizeof(line)},0};
    protocol_writer_append_literal(&w,"notification_memory ");
    protocol_writer_append_bytes(&w,reinterpret_cast<const u8*>(phase.data),phase.bytes);
    const auto number=[&](Utf8View label,u64 value) noexcept {
        protocol_writer_append_bytes(&w,reinterpret_cast<const u8*>(label.data),label.bytes);
        protocol_writer_append_u64(&w,value);
    };
    number(utf8_literal(" fixture_owner="),m.owner_sizeof_bytes);
    number(utf8_literal(" fixture_extension="),m.owner_fixture_extension_bytes);
    number(utf8_literal(" owner_commit="),m.owner_vm_committed_bytes);
    number(utf8_literal(" storage_commit="),m.storage_committed_bytes);
    number(utf8_literal(" storage_reserve="),m.storage_reserved_bytes);
    number(utf8_literal(" thread_commit="),m.thread_storage.committed_bytes);
    number(utf8_literal(" display_sizeof="),m.display_owner_sizeof_bytes);
    number(utf8_literal(" display_commit="),m.display_vm_committed_bytes);
    number(utf8_literal(" display_outstanding="),m.display_outstanding);
    number(utf8_literal(" display_bytes="),m.display_bytes);
    number(utf8_literal(" retained_channels="),m.retention_channels);
    number(utf8_literal(" retained_tickets="),m.retention_tickets);
    number(utf8_literal(" retention_logical="),m.retention_logical_bytes);
    number(utf8_literal(" retention_rows_commit="),m.retention_rows_committed_bytes);
    number(utf8_literal(" retention_index_commit="),m.retention_index_committed_bytes);
    protocol_writer_append_literal(&w,"\r\n"); DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,w.used,&written,nullptr);
}

void capture(void* context, const DesktopNotificationPayload& payload) noexcept {
    auto& f = *static_cast<Fixture*>(context);
    ++f.delivered;
    f.last = {};
    f.last.target = payload.target;
    f.last.silent = payload.silent;
    f.last.suppress_popup = payload.suppress_popup;
    if (payload.title.bytes > sizeof(f.last.title) || payload.body.bytes > sizeof(f.last.body) ||
        (payload.title.bytes && !payload.title.data) || (payload.body.bytes && !payload.body.data)) {
        f.sink_valid = false; return;
    }
    f.last.title_bytes = payload.title.bytes;
    f.last.body_bytes = payload.body.bytes;
    copy_bytes(f.last.title, payload.title.data, payload.title.bytes);
    copy_bytes(f.last.body, payload.body.data, payload.body.bytes);
    const u32 action=f.show_action; f.show_action=0;
    if(action==1) application_notifications_ack(&g_app,payload.target.channel_id);
    else if(action==2) application_notifications_account_stop(&g_app);
    else if(action==3 || action==5) {
        if(action==5) g_app.account.status.profile_id=88;
        GatewayClientEvent ready_event{}; ready_event.kind=GatewayClientEventKind::ready;
        application_notifications_gateway(&g_app,ready_event,{});
    }
    else if(action==4) g_app.account.status.profile_id=88;
}
DesktopNotificationResult capture_removal(void* context,DesktopNotificationActivation target) noexcept {
    auto& f=*static_cast<Fixture*>(context);
    if(f.removal_count<64) f.removals[f.removal_count]=target;
    ++f.removal_count; return f.removal_result;
}
NotificationAudioResult capture_sound(void* context,const NotificationAudioRequest& request) noexcept {
    auto& f=*static_cast<Fixture*>(context);
    if(!f.sound_result.ok()) return f.sound_result;
    if(f.sound_count>=64) return {NotificationAudioError::full_queue,0};
    f.sounds[f.sound_count++]=request; return {};
}
NotificationAudioStatus sound_status(void* context) noexcept {return static_cast<Fixture*>(context)->sound_status;}
u64 sound_clock(void* context) noexcept {return static_cast<Fixture*>(context)->sound_now;}
bool enable_sound() noexcept {
    application_notifications_fixture_focus(&g_app,false);
    return application_notifications_fixture_audio(&g_app,capture_sound,sound_status,sound_clock,&fixture);
}

bool publish(void*, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    auto& f = fixture;
    if (f.hold_completion && event.dispatch_kind == GatewayClientDispatchKind::ready_state_complete) {
        f.completion = event;
        if (f.trace_ready) trace(utf8_literal("held_completion"), &event);
        return true;
    }
    application_notifications_gateway(&g_app, event, payload);
    if (f.trace_ready) trace(utf8_literal("event"), &event);
    f.consumed = message_event_pump_consume(&g_app.message_pump, event, payload);
    if(f.auto_drain && event.kind==GatewayClientEventKind::dispatch &&
        event.dispatch_kind==GatewayClientDispatchKind::message_create) application_notifications_drain(&g_app);
    return f.consumed.ok();
}
bool initialize() noexcept {
    application_notifications_release(&g_app);
    clear_bytes(&g_app, sizeof(g_app));
    auto& f = fixture;
    f.delivered = f.relationship_flags = f.navigation_calls = f.trace_lines = 0;
    f.relationship_known = f.sink_valid = true; f.hold_completion = false;
    f.trace_ready = false;
    f.auto_drain = true;
    f.sound_count=0; f.sound_now=0; f.sound_status={};
    f.sound_status.enabled=true; f.sound_result={};
    f.last = {}; f.completion = {}; f.consumed = {};
    f.removal_count=f.show_action=0; f.removal_result={};
    clear_bytes(f.removals,sizeof(f.removals));
    g_app.preferences = client_preferences_defaults();
    g_app.account.status.profile_id = 77;
    g_app.account_ui = &f.ui;
    if (!chat_state_store_initialize(&f.ui.chat_state, {f.guilds, 4}, {f.guild_order, 4},
            {f.channels, 32}, {f.channel_order, 32}, {f.names, sizeof(f.names)}) ||
        !chat_state_store_bind_permissions(&f.ui.chat_state, {f.roles, 16}, {f.overwrites, 32}) ||
        !user_store_initialize(&f.ui.user_store, {f.users, 16}, {f.user_order, 16},
            {f.user_names, sizeof(f.user_names)}) ||
        !message_store_initialize(&f.messages, {f.message_rows, 8}, {f.message_order, 8},
            {f.message_text, sizeof(f.message_text)}) ||
        !ui_variable_list_initialize(&f.list, {f.heights, 8}, {f.fenwick, 8}, 0, 100) ||
        !message_timeline_initialize(&f.timeline, &f.messages, &f.list, 100, 200, true) ||
        !message_event_pump_initialize(&g_app.message_pump, &f.timeline,
            {f.pump_scratch, sizeof(f.pump_scratch)}, {f.bulk_ids, kGatewayBulkDeleteMaximumIds},
            MessageContentAvailability::available) ||
        !message_event_pump_set_state_store(&g_app.message_pump, &f.ui.chat_state, {f.guild_ids, 4}) ||
        !message_event_pump_set_user_store(&g_app.message_pump, &f.ui.user_store)) return false;
    message_event_pump_set_create_observer(&g_app.message_pump, application_notifications_message, &g_app);
    return application_notifications_fixture_sink(&g_app, capture, &f) &&
        application_notifications_fixture_retention_remove(&g_app,capture_removal,&f);
}
// Synthetic public IDs only. READY and all mutations go through the production
// typed producers; no pre-decoded policy records are planted in the owner.
constexpr auto ready = utf8_literal(R"({"v":9,"user":{"id":"1","username":"self"},"session_id":"notification-fixture","resume_gateway_url":"wss://gateway.discord.gg","users":[{"id":"2","username":"Peer"}],"guilds":[{"id":"10","name":"Guild","owner_id":"99","default_message_notifications":0,"joined_at":"2026-10-01T00:00:00Z","roles":[{"id":"10","name":"@everyone","permissions":"68608"}],"members":[{"user":{"id":"1","username":"self"},"roles":[]}],"channels":[{"id":"20","type":0,"name":"selected","permission_overwrites":[]},{"id":"21","type":0,"name":"other","permission_overwrites":[]},{"id":"22","type":4,"name":"category","permission_overwrites":[]}],"threads":[]}],"user_guild_settings":{"entries":[{"guild_id":"10","message_notifications":0,"muted":false,"channel_overrides":[{"channel_id":"21","message_notifications":3}]}],"partial":false,"version":10},"notification_settings":{"flags":0},"user_settings_proto":""})");
bool hydrate(Utf8View input = ready, u64 sequence = 10, bool hold = false,
    Utf8View settings_override = {}) noexcept {
    auto& f = fixture;
    f.hold_completion = hold;
    GatewayReady decoded{};
    if (!gateway_decode_ready({input, 0, JsonValueKind::object}, {f.arrays, 4096}, 64, &decoded).ok()) return false;
    if(settings_override.data) decoded.user_guild_settings={settings_override,0,JsonValueKind::object};
    // Trace the first complete producer sequence, keeping room in the same
    // 32-line budget for the later partial/full snapshot boundaries.
    f.trace_ready = f.trace_lines == 0;
    const auto result = gateway_ready_project(decoded,
        {{f.arrays, 4096}, {f.records, 4096}, {f.payload, sizeof(f.payload)}, {f.header, sizeof(f.header)}},
        sequence, 1000, publish, nullptr, nullptr);
    f.hold_completion = false;
    f.trace_ready = false;
    const bool valid = result.ok() && chat_state_store_select_channel(&f.ui.chat_state, 20).ok();
    trace(utf8_literal("after_hydrate"));
    return valid;
}
bool snapshot(Utf8View settings_json) noexcept {
    auto& f = fixture;
    GatewayReady decoded{};
    if (!gateway_decode_ready({ready, 0, JsonValueKind::object}, {f.arrays, 4096}, 64, &decoded).ok()) return false;
    decoded.user_guild_settings = {settings_json, 0, JsonValueKind::object};
    f.trace_ready = true;
    const auto result = gateway_client_publish_notification_snapshot(decoded, {f.arrays, 4096}, {f.records, 4096},
        {f.payload, sizeof(f.payload)}, 10, 1000, publish, nullptr);
    f.trace_ready = false;
    trace(utf8_literal("after_snapshot"));
    return result.ok();
}
bool dispatch(Utf8View name, Utf8View json, u64 sequence = 20) noexcept {
    auto& f = fixture;
    GatewayEnvelope envelope{};
    envelope.opcode = 0; envelope.has_event_name = envelope.has_sequence = true;
    envelope.event_name = {name, 0, false}; envelope.data = {json, 0, JsonValueKind::object};
    envelope.sequence = sequence;
    GatewayClientEvent event{}; ConstSlice<u8> payload{};
    if (!gateway_client_route_dispatch(envelope, {f.records, 4096}, 64,
            {f.payload, sizeof(f.payload)}, {f.bulk_ids, kGatewayBulkDeleteMaximumIds}, 1, &event, &payload).ok()) return false;
    return publish(nullptr, event, payload);
}
bool lifecycle(GatewayClientEventKind kind) noexcept {
    GatewayClientEvent event{}; event.kind = kind;
    return publish(nullptr, event, {});
}
bool message(u64 id, u64 sequence, u64 channel = 21,
    Utf8View content = utf8_literal("\"hello\""), u64 author = 2,
    Utf8View extra = {}, Utf8View author_name = utf8_literal("Peer"), u32 author_flags = 0,u16 message_type=0) noexcept {
    auto& f = fixture;
    ProtocolWriter writer{{reinterpret_cast<u8*>(f.source), sizeof(f.source)}, 0};
    const auto append = [&](Utf8View text) noexcept {
        return protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(text.data), text.bytes);
    };
    if (!protocol_writer_append_literal(&writer, "{\"id\":\"") || !protocol_writer_append_u64(&writer, id) ||
        !protocol_writer_append_literal(&writer, "\",\"channel_id\":\"") || !protocol_writer_append_u64(&writer, channel) ||
        !protocol_writer_append_literal(&writer, "\",\"type\":") || !protocol_writer_append_u64(&writer,message_type) ||
        !protocol_writer_append_literal(&writer, ",\"author\":{\"id\":\"") ||
        !protocol_writer_append_u64(&writer, author) ||
        !protocol_writer_append_literal(&writer, "\",\"username\":\"") || !append(author_name) ||
        !protocol_writer_append_literal(&writer, "\",\"public_flags\":") ||
        !protocol_writer_append_u64(&writer, author_flags) ||
        !protocol_writer_append_literal(&writer, "},\"content\":") || !append(content) ||
        !protocol_writer_append_literal(&writer, ",\"timestamp\":\"2026-10-02T00:00:00Z\",\"mention_everyone\":false,\"mentions\":[],\"mention_roles\":[]") ||
        !append(extra) || !protocol_writer_append_byte(&writer, '}')) return false;
    return dispatch(utf8_literal("MESSAGE_CREATE"), {f.source, writer.used}, sequence);
}
bool body(Utf8View expected) noexcept {
    return utf8_equal({fixture.last.body, fixture.last.body_bytes}, expected);
}
bool safe_text() noexcept {
    const auto& row = fixture.last;
    for (u32 i = 0; i < row.title_bytes; ++i) if (!row.title[i]) return false;
    for (u32 i = 0; i < row.body_bytes; ++i) if (!row.body[i]) return false;
    return fixture.sink_valid;
}
bool settings(Utf8View json) noexcept { return dispatch(utf8_literal("USER_GUILD_SETTINGS_UPDATE"), json); }
bool global(Utf8View json) noexcept { return dispatch(utf8_literal("USER_SETTINGS_PROTO_UPDATE"), json); }
void sound_regressions(void(*check)(bool,const char*) noexcept) noexcept {
    auto& f=fixture;
    check(initialize() && enable_sound() && hydrate(),"notification audio: explicit fake sink and production READY initialize without endpoint output");
    f.auto_drain=false;
    check(application_notifications_fixture_metrics(&g_app).audio_output_percent==100,
        "notification audio: VM-created owner explicitly initializes worker volume to migrated100 percent");
    g_app.preferences.output_volume=200;application_notifications_settings_refresh(&g_app);
    check(application_notifications_fixture_metrics(&g_app).audio_output_percent==200,
        "notification audio: refreshed volume updates atomic bank independently of queued nominal gain");
    check(message(1000,2) && f.sound_count==1 && f.sounds[0].clip_id==1 && f.sounds[0].gain==.4f &&
        f.sounds[0].epoch==application_notifications_fixture_metrics(&g_app).audio_epoch && !f.delivered &&
        application_notifications_next_wake(&g_app)==1000,
        "notification audio: popup fallback emits leading classic1 nominal gain before deferred Show");
    application_notifications_drain(&g_app);
    check(f.delivered==1 && f.sound_count==1 && !application_notifications_fixture_metrics(&g_app).audio_worker_started,
        "notification audio: fake popup is silent and fixture never creates native audio worker");
    f.sound_now=100;
    check(message(1001,3) && f.sound_count==1 && application_notifications_next_wake(&g_app)==1000,
        "notification audio: burst retains trailing request rather than dropping all later message sounds");
    f.sound_now=999; application_notifications_drain(&g_app);
    check(f.sound_count==1,"notification audio: early host continuation cannot emit trailing sound");
    f.sound_now=1000; application_notifications_drain(&g_app);
    check(f.sound_count==2 && f.sounds[1].clip_id==1 && !application_notifications_next_wake(&g_app),
        "notification audio: exact H endpoint emits copied trailing clip once and retires deadline");
    f.sound_now=1001;
    check(message(1002,4) && f.sound_count==2 && application_notifications_next_wake(&g_app)==2001,
        "notification audio: immediate post-trailing call owns later debounce deadline");
    const u64 before_mute=application_notifications_fixture_metrics(&g_app).audio_epoch;
    g_app.preferences.message_sounds_enabled=false;
    application_notifications_settings_refresh(&g_app);
    check(application_notifications_fixture_metrics(&g_app).audio_epoch>before_mute && !application_notifications_next_wake(&g_app),
        "notification audio: device sound mute advances epoch and cancels H without purging PCM lifetime");
    f.sound_now=2001; application_notifications_drain(&g_app);
    check(f.sound_count==2 && f.delivered==3 && message(1003,5),
        "notification audio: muted pending sound cannot play while existing ordinary popup still continues");
    application_notifications_drain(&g_app);
    check(f.sound_count==2 && f.delivered==4,"notification audio: message1 disabled does not disable new ordinary popups");
    g_app.preferences.selected_message_sounds_enabled=true;
    check(message(1004,6,20) && !application_notifications_fixture_metrics(&g_app).display_outstanding && f.sound_count==2,
        "notification audio: blurred selected T ends ordinary popup flow even when shared message1 sound is muted");
    g_app.preferences.message_sounds_enabled=true;
    application_notifications_settings_refresh(&g_app);
    check(message(1005,7,20) && f.sound_count==3 && f.sounds[2].clip_id==2 &&
        !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification audio: blurred original main selection emits direct message3 and no ordinary popup");
    application_notifications_fixture_focus(&g_app,true);
    check(message(1006,8,20) && f.sound_count==4 && f.sounds[3].clip_id==2,
        "notification audio: focused selected sound is direct despite ordinary selected-channel suppression");
    check(dispatch(utf8_literal("THREAD_CREATE"),utf8_literal(R"({"id":"30","guild_id":"10","parent_id":"21","type":11,"name":"selected","thread_metadata":{"archived":false},"member":{"id":"30","user_id":"1","join_timestamp":"2026-10-01T00:00:00Z","flags":8}})")) &&
        chat_state_store_select_channel(&f.ui.chat_state,30).ok(),"notification audio: real selected thread NONE fixture prepares normalized type21 split");
    g_app.preferences.message_sounds_enabled=false; application_notifications_settings_refresh(&g_app);
    g_app.preferences.message_sounds_enabled=true; application_notifications_settings_refresh(&g_app);
    f.sound_now=5000;
    const u32 before_type21=f.sound_count;
    check(message(1007,9,30,utf8_literal("\"thread starter\""),2,{},utf8_literal("Peer"),0,21) &&
        f.sound_count==before_type21+2 && f.sounds[before_type21].clip_id==2 && f.sounds[before_type21+1].clip_id==1 &&
        application_notifications_fixture_metrics(&g_app).display_outstanding==1,
        "notification audio: type21 uses original main selection for direct3 and normalized parent for ordinary1 plus popup");
    application_notifications_drain(&g_app);
    check(f.last.target.channel_id==30 && f.last.target.message_id==1007,
        "notification audio: type21 activation retains original thread despite parent policy");
    const u32 before_thread=f.sound_count;
    check(message(1008,10,30) && f.sound_count==before_thread+1 && f.sounds[before_thread].clip_id==2 &&
        !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification audio: selected generic sound ignores joined-thread NONE while ordinary p rejects it");
    application_notifications_fixture_focus(&g_app,false);
    g_app.preferences.selected_message_sounds_enabled=false;
    check(chat_state_store_select_channel(&f.ui.chat_state,20).ok() &&
        application_notifications_fixture_sink(&g_app,nullptr,nullptr),"notification audio: popup identity unavailable fixture retains independent sound sink");
    f.sound_now=7000;
    const u32 before_missing=f.sound_count;
    check(message(1009,11) && f.sound_count==before_missing+1 && f.sounds[before_missing].clip_id==1 &&
        application_notifications_next_wake(&g_app)==8000,
        "notification audio: missing popup identity requests fallback H instead of desktop NEVER direct route");
    f.sound_now=7001;check(message(1010,12) && f.sound_count==before_missing+1,
        "notification audio: identity-suppressed popup fallback preserves trailing H semantics");
    g_app.preferences.desktop_enabled=false;
    check(message(1011,13) && message(1012,14) && f.sound_count==before_missing+3 &&
        f.sounds[before_missing+1].clip_id==1 && f.sounds[before_missing+2].clip_id==1,
        "notification audio: explicit desktop NEVER emits both direct message1 requests without H coalescing");
    f.sound_status.queued=kNotificationAudioQueueCapacity;f.sound_status.outstanding=kNotificationAudioVoiceCapacity;
    check(!application_notifications_can_pop(&g_app) && !application_notifications_gateway_pending(&g_app) &&
        !application_notifications_next_wake(&g_app),
        "notification audio: full worker capacity blocks pre-pop but supplies no spinning Gateway or timer work");
    f.sound_status.queued=31;f.sound_status.outstanding=63;
    check(!application_notifications_gateway_pending(&g_app) && !application_notifications_can_pop(&g_app),
        "notification audio: one restored slot is insufficient for worst-case two-route pre-pop reservation");
    f.sound_status.queued=30;f.sound_status.outstanding=62;
    check(application_notifications_gateway_pending(&g_app) && application_notifications_can_pop(&g_app) &&
        !application_notifications_gateway_pending(&g_app) && application_notifications_next_wake(&g_app)==8000,
        "notification audio: worker capacity wake restores two-route Gateway admission and retained H deadline");
    f.sound_now=8000; application_notifications_drain(&g_app);
    check(f.sound_count==before_missing+4,"notification audio: deferred capacity never destroys trailing H intent");
    f.sound_status.queued=32;f.sound_status.outstanding=64;
    check(!application_notifications_can_pop(&g_app),"notification audio: held Gateway prepares mute-resume regression");
    g_app.preferences.message_sounds_enabled=false;application_notifications_settings_refresh(&g_app);
    check(application_notifications_gateway_pending(&g_app) && application_notifications_can_pop(&g_app) &&
        !application_notifications_gateway_pending(&g_app),
        "notification audio: muting a full worker retains Gateway resumption until successful pre-pop without new input");
    g_app.preferences.message_sounds_enabled=true;application_notifications_settings_refresh(&g_app);
    check(!application_notifications_can_pop(&g_app),"notification audio: held Gateway prepares terminal-exit wake regression");
    f.sound_status.worker_exited=true;
    application_notifications_wake(&g_app);application_notifications_drain(&g_app);
    check(application_notifications_gateway_pending(&g_app),
        "notification audio: terminal-exit wake keeps held Gateway resumable after status observation");
    check(application_notifications_can_pop(&g_app) && application_notifications_fixture_metrics(&g_app).audio_failed &&
        !application_notifications_gateway_pending(&g_app) && !application_notifications_next_wake(&g_app),
        "notification audio: terminal worker exit disables only audio instead of permanently stalling Gateway");
    check(message(1013,15) && f.sound_count==before_missing+4,"notification audio: failed worker is not relaunched behind normal message admission");

    check(initialize() && enable_sound() && hydrate(),"notification audio: fresh fake owner prepares metadata-specific restrictions");
    f.auto_drain=false;
    check(message(1050,2,21,utf8_literal("\"hello\""),2,utf8_literal(",\"activity_instance\":null,\"interaction\":{\"id\":\"9\",\"type\":2,\"name\":\"command\",\"user\":{\"id\":\"1\",\"username\":\"self\"}}")) &&
        !f.sound_count && !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification audio: explicit null activity instance plus own legacy interaction suppresses ordinary lx");
    check(message(1051,3,21,utf8_literal("\"hello\""),2,{},utf8_literal("Peer"),0,47) && !f.sound_count &&
        message(1052,4,21,utf8_literal("\"hello\""),2,utf8_literal(",\"changelog_id\":\"9\""),utf8_literal("Peer"),0,47) && !f.sound_count &&
        !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification audio: missing changelog ID rejects and unknown latest-ID match stays incomplete");
    f.sound_result={NotificationAudioError::full_queue,0};
    check(message(1053,5) && application_notifications_fixture_metrics(&g_app).audio_failed,
        "notification audio: fake atomic submit failure disables admission without opening endpoint");
    const u64 failure_epoch=application_notifications_fixture_metrics(&g_app).audio_epoch;
    g_app.preferences.message_sounds_enabled=false;application_notifications_settings_refresh(&g_app);
    const auto failed_reset=application_notifications_fixture_metrics(&g_app);
    check(failed_reset.audio_epoch>failure_epoch && failed_reset.audio_backend_epoch==failed_reset.audio_epoch && failed_reset.audio_failed,
        "notification audio: mute still resets backend desired epoch after failed admission while keeping borrowed bank attached");
    check(initialize() && enable_sound() && hydrate(),"notification audio: fresh fake owner prepares terminal Show failure branch");
    f.auto_drain=false;
    application_notifications_fixture_show_state(&g_app,DesktopNotificationShowState::retry_required);
    check(message(1100,2) && f.sound_count==1,"notification audio: retry fixture retains initial fallback leading sound");
    application_notifications_drain(&g_app);
    check(f.delivered==1 && f.sound_count==1 && application_notifications_fixture_metrics(&g_app).display_outstanding==1,
        "notification audio: first native retry carries copied sound epoch and does not request failure fallback yet");
    f.sound_now=1000; application_notifications_drain(&g_app);
    check(f.delivered==2 && f.sound_count==2 && !application_notifications_fixture_metrics(&g_app).display_outstanding &&
        f.sounds[1].clip_id==1,"notification audio: terminal retry failure requests a distinct second H invocation at its current clock");
    f.sound_now=2000;application_notifications_drain(&g_app);
    check(f.sound_count==2,"notification audio: failure fallback single leading invocation has no synthetic trailing duplicate");
    check(message(1101,3),"notification audio: copied old-epoch sound prepares late terminal failure");
    const u32 before_epoch=f.sound_count;
    g_app.preferences.message_sounds_enabled=false;application_notifications_settings_refresh(&g_app);
    g_app.preferences.message_sounds_enabled=true;application_notifications_settings_refresh(&g_app);
    application_notifications_fixture_show_state(&g_app,DesktopNotificationShowState::failed);
    application_notifications_drain(&g_app);
    check(f.sound_count==before_epoch,"notification audio: canceled old sound epoch cannot reenter H on later popup failure");
    f.sound_now=2100; check(message(1102,4),"notification audio: leading sound prepares resume retained pending request");
    f.sound_now=2200; check(message(1103,5),"notification audio: second sound prepares H trailing request");
    const u64 resume_epoch=application_notifications_fixture_metrics(&g_app).audio_epoch;
    const u64 resume_due=application_notifications_next_wake(&g_app);
    check(lifecycle(GatewayClientEventKind::reconnecting) && !application_notifications_next_wake(&g_app) &&
        lifecycle(GatewayClientEventKind::resumed) && application_notifications_fixture_metrics(&g_app).audio_epoch==resume_epoch &&
        application_notifications_next_wake(&g_app)==resume_due,
        "notification audio: reconnect suppresses host timers while RESUMED retains epoch and H intent");
    check(hydrate(ready,1) && application_notifications_fixture_metrics(&g_app).audio_epoch>resume_epoch &&
        !application_notifications_next_wake(&g_app),"notification audio: READY advances epoch and clears prior trailing request");
    f.sound_now=3000;check(message(1104,2),"notification audio: post-READY sound prepares account stop ownership barrier");
    const u64 stop_epoch=application_notifications_fixture_metrics(&g_app).audio_epoch;
    f.sound_status.queued=32;f.sound_status.outstanding=64;
    check(!application_notifications_can_pop(&g_app),"notification audio: held full Gateway prepares account invalidation");
    f.sound_status.worker_exited=true;application_notifications_wake(&g_app);application_notifications_drain(&g_app);
    check(application_notifications_gateway_pending(&g_app),"notification audio: observed failure retains held input before account invalidation");
    application_notifications_account_stop(&g_app);
    check(application_notifications_fixture_metrics(&g_app).audio_epoch>stop_epoch && !application_notifications_next_wake(&g_app) &&
        !application_notifications_gateway_pending(&g_app) && !application_notifications_fixture_metrics(&g_app).audio_worker_started,
        "notification audio: account stop cancels sound epoch without creating or releasing fixture PCM worker");
    application_notifications_release(&g_app);
}

void pack_regressions(void(*check)(bool,const char*) noexcept) noexcept {
    auto& f=fixture;
    check(initialize() && enable_sound() && hydrate(),"notification packs: fake owner and production READY initialize Classic");
    f.auto_drain=false;
    const u64 initial_epoch=application_notifications_fixture_metrics(&g_app).audio_epoch;
    check(message(1200,2) && f.sound_count==1 && f.sounds[0].clip_id==1,
        "notification packs: default H leading submission fixes concrete Classic bank ID");
    f.sound_now=100;
    check(message(1201,3) && f.sound_count==1,"notification packs: second logical message1 waits in inherited-current H");
    g_app.preferences.soundpack=NotificationSoundpack::retro;application_notifications_settings_refresh(&g_app);
    auto metrics=application_notifications_fixture_metrics(&g_app);
    check(metrics.audio_epoch==initial_epoch && metrics.audio_pack==NotificationSoundpack::retro && metrics.sound_deadline==1000 &&
        f.sounds[0].clip_id==1,"notification packs: preference change retains epoch pending H and already-admitted immutable clip ID");
    f.sound_now=1000;application_notifications_drain(&g_app);
    check(f.sound_count==2 && f.sounds[1].clip_id==1 && f.sounds[0].clip_id==1,
        "notification sounds: legacy preference change retains original alert and prior worker request");
    check(initialize() && enable_sound() && hydrate(),"notification packs: fresh fake owner prepares queued native retry mapping");
    f.auto_drain=false;application_notifications_fixture_show_state(&g_app,DesktopNotificationShowState::retry_required);
    check(message(1210,2) && f.sound_count==1 && f.sounds[0].clip_id==1,"notification packs: popup starts Classic fallback before retry");
    application_notifications_drain(&g_app);
    const u64 retry_epoch=application_notifications_fixture_metrics(&g_app).audio_epoch;
    g_app.preferences.soundpack=NotificationSoundpack::ducky;application_notifications_settings_refresh(&g_app);
    f.sound_now=1000;application_notifications_drain(&g_app);
    check(f.delivered==2 && f.sound_count==2 && f.sounds[1].clip_id==1 &&
        application_notifications_fixture_metrics(&g_app).audio_epoch==retry_epoch &&
        !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification sounds: terminal FAILED retains ordinary alert routing across legacy preference change");
    check(initialize() && enable_sound() && hydrate(),"notification packs: fresh fake owner prepares all local direct overrides");
    f.auto_drain=false;g_app.preferences.desktop_enabled=false;
    constexpr u32 clip_ids[9]={1,1,1,1,1,1,1,1,1};
    const u64 direct_epoch=application_notifications_fixture_metrics(&g_app).audio_epoch;
    bool direct=true;
    for(u32 i=0;i<9;++i) {
        g_app.preferences.soundpack=static_cast<NotificationSoundpack>(i);application_notifications_settings_refresh(&g_app);
        direct=direct && message(1300+i,10+i) && f.sound_count==i+1 && f.sounds[i].clip_id==clip_ids[i] &&
            f.sounds[i].epoch==direct_epoch && !application_notifications_fixture_metrics(&g_app).display_outstanding;
    }
    check(direct,"notification sounds: all legacy ordinary choices map the original alert without epoch reset");
    g_app.preferences.selected_message_sounds_enabled=true;application_notifications_fixture_focus(&g_app,true);
    bool selected=true;
    for(u32 i=0;i<9;++i) {
        g_app.preferences.soundpack=static_cast<NotificationSoundpack>(i);application_notifications_settings_refresh(&g_app);
        const u64 id=1400+i;
        selected=selected && message(id,100+2*i,20) && f.sound_count==10+i && f.sounds[9+i].clip_id==2 &&
            f.sounds[9+i].epoch==direct_epoch;
        // Retire the selected timeline row through the actual delete producer,
        // avoiding an unrelated eight-row fixture capacity limit.
        ProtocolWriter writer{{reinterpret_cast<u8*>(f.header),sizeof(f.header)},0};
        selected=selected && protocol_writer_append_literal(&writer,"{\"id\":\"") && protocol_writer_append_u64(&writer,id) &&
            protocol_writer_append_literal(&writer,"\",\"channel_id\":\"20\"}") &&
            dispatch(utf8_literal("MESSAGE_DELETE"),{f.header,writer.used},101+2*i);
    }
    check(selected && !application_notifications_fixture_metrics(&g_app).audio_worker_started,
        "notification packs: selected message3 stays sealed ID2 for every pack through production message and delete routing without real audio");
    application_notifications_release(&g_app);
}

bool retention_channel(u64 channel) noexcept {
    auto& f=fixture;
    ProtocolWriter w{{reinterpret_cast<u8*>(f.header),sizeof(f.header)},0};
    return protocol_writer_append_literal(&w,"{\"id\":\"") && protocol_writer_append_u64(&w,channel) &&
        protocol_writer_append_literal(&w,"\",\"guild_id\":\"10\",\"type\":0,\"name\":\"retained\",\"nsfw\":false,\"permission_overwrites\":[]}") &&
        dispatch(utf8_literal("CHANNEL_CREATE"),{f.header,w.used});
}
void retention_regressions(void(*check)(bool,const char*) noexcept) noexcept {
    auto& f=fixture;
    check(initialize() && hydrate(ready,1),"notification retention: production owner starts with empty successful history");
    auto m=application_notifications_fixture_metrics(&g_app);
    check(!m.retention_channels && !m.retention_tickets && !m.retention_capacity_channels &&
        !m.retention_rows_committed_bytes && !m.retention_index_committed_bytes,
        "notification retention: initialization and READY allocate zero ledger VM");
    g_app.preferences.message_sounds_enabled=false; application_notifications_settings_refresh(&g_app);
    bool twenty=true;
    for(u32 i=0;i<20;++i) {
        application_notifications_fixture_suppress_popup(&g_app,i==19);
        twenty=message(2000+i,2+i) && twenty;
    }
    application_notifications_fixture_suppress_popup(&g_app,false);
    m=application_notifications_fixture_metrics(&g_app);
    SYSTEM_INFO system{};GetSystemInfo(&system);
    check(twenty && f.delivered==20 && f.last.silent && f.last.suppress_popup && m.retention_channels==1 && m.retention_tickets==20 &&
        m.retention_row_bytes==176 && m.retention_logical_bytes==180 &&
        m.retention_rows_committed_bytes%system.dwPageSize==0 && m.retention_rows_committed_bytes>0 &&
        m.retention_index_committed_bytes%system.dwPageSize==0 && m.retention_index_committed_bytes>0 &&
        application_notifications_fixture_retained(&g_app,21,0)==2000 &&
        application_notifications_fixture_retained(&g_app,21,19)==2019 && !f.removal_count,
        "notification retention: exactly20 successful silent and ActionCenter-only Shows keep actual-channel FIFO in page-rounded sparse banks");
    memory_trace(utf8_literal("retention_twenty"),m);
    f.removal_result={DesktopNotificationError::transport,5};
    check(message(2020,22) && f.removal_count==1 && f.removals[0].profile_id==77 &&
        f.removals[0].channel_id==21 && f.removals[0].message_id==2000 &&
        application_notifications_fixture_retained(&g_app,21,0)==2001 &&
        application_notifications_fixture_retained(&g_app,21,19)==2020 &&
        application_notifications_fixture_metrics(&g_app).retention_tickets==20,
        "notification retention: 21st success retires oldest metadata even when native tag close fails");
    application_notifications_fixture_show_state(&g_app,DesktopNotificationShowState::failed);
    check(message(2021,23) && application_notifications_fixture_metrics(&g_app).retention_tickets==20 && f.removal_count==1,
        "notification retention: failed Show excludes successful ledger");
    f.auto_drain=false; application_notifications_fixture_show_state(&g_app,DesktopNotificationShowState::retry_required);
    check(message(2022,24) && application_notifications_fixture_metrics(&g_app).display_outstanding==1 &&
        application_notifications_fixture_metrics(&g_app).retention_tickets==20,
        "notification retention: pending queue intent excludes successful ledger");
    application_notifications_drain(&g_app);
    check(application_notifications_fixture_metrics(&g_app).retention_tickets==20 && f.removal_count==1 &&
        application_notifications_fixture_metrics(&g_app).display_outstanding==1,
        "notification retention: first native retry excludes successful ledger");
    application_notifications_fixture_show_state(&g_app,DesktopNotificationShowState::submitted);
    application_notifications_drain(&g_app); f.auto_drain=true;
    check(f.removal_count==2 && f.removals[1].message_id==2001 &&
        application_notifications_fixture_retained(&g_app,21,19)==2022 &&
        !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification retention: successful retry appends once and evicts one FIFO ticket");
    const u32 seen=application_notifications_fixture_metrics(&g_app).seen_count;
    const u32 delivered=f.delivered;
    application_notifications_ack(&g_app,21);
    check(!application_notifications_fixture_metrics(&g_app).retention_channels &&
        application_notifications_fixture_metrics(&g_app).seen_count==seen && message(2005,25) && f.delivered==delivered &&
        !application_notifications_fixture_metrics(&g_app).retention_tickets,
        "notification retention: channel ACK deletes success owner without clearing receive replay state");
    check(message(2030,26) && chat_state_store_select_channel(&f.ui.chat_state,21).ok(),
        "notification retention: next successful message prepares selection clear");
    application_notifications_selection(&g_app);
    check(!application_notifications_fixture_metrics(&g_app).retention_channels,
        "notification retention: selecting channel clears successful history");
    check(message(2031,27),"notification retention: blurred selected channel prepares focused ACK");
    const u32 focus_seen=application_notifications_fixture_metrics(&g_app).seen_count;
    application_notifications_fixture_focus(&g_app,true); application_notifications_selection(&g_app);
    check(!application_notifications_fixture_metrics(&g_app).retention_channels &&
        application_notifications_fixture_metrics(&g_app).seen_count==focus_seen,
        "notification retention: regained focus clears selected success history independently of Seen");

    check(initialize() && hydrate(ready,1) && message(2100,2),"notification retention: fresh success prepares same-profile READY");
    const auto before_ready=application_notifications_fixture_metrics(&g_app);
    check(hydrate(ready,1) && application_notifications_fixture_metrics(&g_app).retention_tickets==1 &&
        application_notifications_fixture_metrics(&g_app).retention_rows_committed_bytes==before_ready.retention_rows_committed_bytes &&
        !application_notifications_fixture_metrics(&g_app).seen_count && message(2100,2) &&
        application_notifications_fixture_retained(&g_app,21,0)==2100 &&
        application_notifications_fixture_retained(&g_app,21,1)==2100,
        "notification retention: same-profile READY resets ingress but retains successful duplicate opaque tags");
    bool duplicates=true;
    for(u32 i=0;i<18;++i) duplicates=message(2101+i,3+i) && duplicates;
    check(duplicates && hydrate(ready,1) && message(2100,2) && f.removal_count==1 &&
        f.removals[0].message_id==f.last.target.message_id &&
        application_notifications_fixture_retained(&g_app,21,0)==2100 &&
        application_notifications_fixture_retained(&g_app,21,19)==2100,
        "notification retention: source permits duplicate tickets whose Windows stable-tag eviction also identifies newest replacement");
    g_app.account.status.profile_id=88;
    check(hydrate(ready,1) && !application_notifications_fixture_metrics(&g_app).retention_tickets &&
        !application_notifications_fixture_metrics(&g_app).retention_rows_committed_bytes &&
        !application_notifications_fixture_metrics(&g_app).retention_index_committed_bytes,
        "notification retention: native profile change frees prior private success metadata");

    check(initialize() && hydrate(ready,1) && message(2300,2),"notification retention: first success prepares geometric growth");
    bool populated=true;
    for(u32 i=0;i<15;++i) populated=retention_channel(100+i) && message(2301+i,3+i,100+i) && populated;
    m=application_notifications_fixture_metrics(&g_app);
    check(populated && m.retention_channels==16 && m.retention_capacity_channels==16 && m.retention_index_slots==32,
        "notification retention: separate successful channels fill initial geometry without global FIFO eviction");
    application_notifications_fixture_retention_oom(&g_app,true);
    check(message(2400,18) && retention_channel(115) && message(2316,19,115),
        "notification retention: existing channel and new capacity demand exercise isolated allocation failure");
    auto failed_growth=application_notifications_fixture_metrics(&g_app);
    check(f.delivered==17 && failed_growth.retention_channels==16 && failed_growth.retention_tickets==17 &&
        failed_growth.retention_allocation_failures==1 && !failed_growth.display_outstanding &&
        failed_growth.retention_rows_committed_bytes==m.retention_rows_committed_bytes &&
        application_notifications_fixture_retained(&g_app,21,0)==2300 &&
        application_notifications_fixture_retained(&g_app,21,1)==2400 && application_notifications_can_pop(&g_app),
        "notification retention: failed growth preserves both banks and tickets; existing capacity works and failed intent retires before Show");
    application_notifications_fixture_retention_oom(&g_app,2);
    check(message(2317,20,115) && f.delivered==17 &&
        application_notifications_fixture_metrics(&g_app).retention_allocation_failures==2 &&
        application_notifications_fixture_metrics(&g_app).retention_rows_committed_bytes==m.retention_rows_committed_bytes &&
        application_notifications_fixture_metrics(&g_app).retention_index_committed_bytes==m.retention_index_committed_bytes &&
        application_notifications_fixture_retained(&g_app,21,1)==2400,
        "notification retention: second-bank OOM frees replacement rows while preserving old index and successful history");
    application_notifications_fixture_retention_oom(&g_app,false);
    check(message(2318,21,115) && application_notifications_fixture_metrics(&g_app).retention_channels==17 &&
        application_notifications_fixture_metrics(&g_app).retention_capacity_channels==32 &&
        application_notifications_fixture_retained(&g_app,100,0)==2301 &&
        application_notifications_fixture_retained(&g_app,115,0)==2318,
        "notification retention: recovered geometric growth rebinds dense rows and channel index without losing history");
    memory_trace(utf8_literal("retention_grown"),application_notifications_fixture_metrics(&g_app));
    check(hydrate(ready,1) && application_notifications_fixture_metrics(&g_app).retention_channels==17,
        "notification retention: READY omitting historical channels retains their successful metadata");
    bool historical=true;
    for(u32 i=0;i<16;++i) historical=retention_channel(200+i) && message(2410+i,2+i,200+i) && historical;
    check(historical && application_notifications_fixture_metrics(&g_app).retention_channels==33 &&
        application_notifications_fixture_metrics(&g_app).retention_capacity_channels==64 &&
        application_notifications_fixture_metrics(&g_app).retention_channels>f.ui.chat_state.channel_count &&
        application_notifications_fixture_retained(&g_app,115,0)==2318,
        "notification retention: historical success owners grow beyond current channel-store capacity without global eviction");
    application_notifications_account_stop(&g_app);
    m=application_notifications_fixture_metrics(&g_app);
    check(!m.retention_channels && !m.retention_tickets && !m.retention_capacity_bytes &&
        !m.retention_rows_committed_bytes && !m.retention_index_committed_bytes,
        "notification retention: account stop frees both successful metadata banks");

    check(initialize() && enable_sound() && hydrate(ready,1),"notification retention: full queue prepares pre-Show OOM fallback");
    f.auto_drain=false; application_notifications_fixture_retention_oom(&g_app,true);
    bool full=true;
    for(u32 i=0;i<32;++i) full=message(2500+i,2+i) && full;
    check(full && application_notifications_fixture_metrics(&g_app).display_outstanding==32 && !application_notifications_can_pop(&g_app),
        "notification retention: deferred burst reaches existing full display queue");
    f.sound_now=2000;
    for(u32 i=0;i<32;++i) application_notifications_drain(&g_app);
    m=application_notifications_fixture_metrics(&g_app);
    check(!f.delivered && !m.display_outstanding && !m.retention_channels && !m.retention_rows_committed_bytes &&
        m.retention_allocation_failures==32 && application_notifications_can_pop(&g_app) && f.sound_count==3 &&
        application_notifications_next_wake(&g_app)==3000,
        "notification retention: preflight OOM retires every full-queue intent with no untracked Show and preserves trailing failure fallback");
    f.sound_now=3000; application_notifications_drain(&g_app);
    check(f.sound_count==4 && f.sounds[3].clip_id==1 && !application_notifications_next_wake(&g_app),
        "notification retention: failed preflight fallback consumes normal H deadline without polling or blocking ingress");
    application_notifications_fixture_retention_oom(&g_app,false); f.auto_drain=true;
    check(message(2600,34) && f.delivered==1 && application_notifications_fixture_metrics(&g_app).retention_tickets==1,
        "notification retention: allocation recovery admits and tracks next success");

    check(initialize() && hydrate(ready,1) && dispatch(utf8_literal("THREAD_CREATE"),
        utf8_literal(R"({"id":"30","guild_id":"10","parent_id":"21","type":11,"name":"starter","thread_metadata":{"archived":false},"member":{"id":"30","user_id":"1","join_timestamp":"2026-10-01T00:00:00Z","flags":8}})")) &&
        message(2700,2,30,utf8_literal("\"thread starter\""),2,{},utf8_literal("Peer"),0,21) &&
        application_notifications_fixture_retained(&g_app,30,0)==2700 && !application_notifications_fixture_retained(&g_app,21,0),
        "notification retention: normalized parent policy retains actual original thread channel");
    application_notifications_ack(&g_app,21);
    check(application_notifications_fixture_retained(&g_app,30,0)==2700 && dispatch(utf8_literal("MESSAGE_DELETE"),
        utf8_literal(R"({"id":"2700","channel_id":"30","guild_id":"10"})")) &&
        application_notifications_fixture_retained(&g_app,30,0)==2700,
        "notification retention: parent ACK and message deletion do not rewrite successful thread ledger");
    check(dispatch(utf8_literal("CHANNEL_DELETE"),utf8_literal(R"({"id":"30","guild_id":"10","type":11})")) &&
        !application_notifications_fixture_metrics(&g_app).retention_channels,
        "notification retention: existing stronger native channel-delete ACK clears successful metadata");
    for(u32 action=1;action<=5;++action) {
        bool prepared=initialize() && hydrate(ready,1) && message(2800,2);
        f.auto_drain=false; f.show_action=action;
        prepared=message(2801,3) && prepared; application_notifications_drain(&g_app);
        const auto after=application_notifications_fixture_metrics(&g_app);
        const u32 expected=action==4?1:0;
        check(prepared && !after.display_outstanding && after.retention_tickets==expected &&
            !application_notifications_fixture_retained(&g_app,21,expected),
            "notification retention: reentrant ACK/account-stop/READY/profile-change during Show retires canceled whole-channel history without stale append");
        if(action==5) {
            f.auto_drain=true;
            check(hydrate(ready,1) && message(2802,2) && f.last.target.profile_id==88 &&
                application_notifications_fixture_retained(&g_app,21,0)==2802,
                "notification retention: old-profile canceled completion cannot erase following new-profile successful channel history");
        }
    }
    application_notifications_account_stop(&g_app); application_notifications_release(&g_app);
}

}

// Notification controls: the account's own DND choice and local setting
// projections apply before any server echo.
void controls_regressions(void(*check)(bool,const char*) noexcept) noexcept {
    using namespace notification_application_probe;
    using namespace app_detail;
    auto& f = fixture;
    if (!initialize() || !hydrate(ready, 10)) { check(false, "notification controls: fixture hydrates"); return; }
    const u32 base = f.delivered;
    g_app.self_status.queued = true;
    g_app.self_status.desired = GatewayUserStatus::dnd;
    const u32 flashes = taskbar_flashes;
    check(message(900, 100) && f.delivered == base && taskbar_flashes == flashes,
        "notification controls: the account's own Do Not Disturb choice silences popups and taskbar flashes at once");
    g_app.self_status = {};
    check(message(901, 101) && f.delivered == base + 1 && taskbar_flashes == flashes + 1,
        "notification controls: leaving Do Not Disturb resumes notifications");
    MessageNotificationSetting muted{};
    muted.muted = true;
    muted.fields = notification_muted | notification_mute_end;
    muted.mute_end_ms = clock_unix_milliseconds() + 3'600'000;
    check(application_notifications_project(&g_app, 10, 0, muted) && message(902, 102) && f.delivered == base + 1,
        "notification controls: a locally projected server mute suppresses before the settings echo");
    muted.mute_end_ms = clock_unix_milliseconds() - 1;
    check(application_notifications_project(&g_app, 10, 0, muted) && message(903, 103) && f.delivered == base + 2,
        "notification controls: an expired timed mute no longer suppresses");
    MessageNotificationSetting channel{};
    channel.muted = true;
    channel.fields = notification_muted | notification_mute_end;
    check(application_notifications_project(&g_app, 10, 21, channel) && message(904, 104) && f.delivered == base + 2 &&
              message_notifications_override(*application_notifications_store(&g_app), 10, 21),
        "notification controls: a muted channel inside an unmuted server stays silent");
    channel.muted = false;
    check(application_notifications_project(&g_app, 10, 21, channel) && message(905, 105) && f.delivered == base + 3,
        "notification controls: unmuting the channel resumes notifications");
    MessageNotificationSetting nothing{};
    nothing.level = MessageNotificationLevel::none;
    nothing.fields = notification_level;
    check(application_notifications_project(&g_app, 10, 21, nothing) && message(906, 106) && f.delivered == base + 3,
        "notification controls: Nothing on a channel suppresses its messages");
    application_notifications_release(&g_app);
}
void test_application_notifications(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace notification_application_probe;
    using namespace app_detail;
    auto& f = fixture;
    const bool owner_initialized=initialize();
    check(owner_initialized,"notification application: nonemitting owner initializes");
    if(!owner_initialized) { application_notifications_release(&g_app); return; }
    auto initial_metrics=application_notifications_fixture_metrics(&g_app);
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    check(initial_metrics.owner_vm_query_ok && initial_metrics.owner_fixture_extension_bytes>=9*sizeof(void*) &&
        initial_metrics.owner_fixture_extension_bytes<11*sizeof(void*) &&
        initial_metrics.owner_vm_committed_bytes==((initial_metrics.owner_sizeof_bytes+info.dwPageSize-1)/info.dwPageSize)*info.dwPageSize &&
        initial_metrics.storage_committed_bytes==0 && initial_metrics.storage_reserved_bytes>0 &&
        !initial_metrics.display_owner_sizeof_bytes && !initial_metrics.display_vm_committed_bytes &&
        initial_metrics.guild_storage.stride_bytes==40 && initial_metrics.override_storage.stride_bytes==40 &&
        initial_metrics.thread_storage.stride_bytes==40 && initial_metrics.staging_storage.stride_bytes==40 &&
        initial_metrics.guild_scope_storage.stride_bytes==32 && initial_metrics.channel_scope_storage.stride_bytes==32,
        "notification application: compiled owner commits its page-rounded size and six stable banks initially reserve without commit");
    memory_trace(utf8_literal("before_ready"),initial_metrics);
    const bool initialized = hydrate(ready, 10, true);
    check(initialized, "notification application: nonemitting owner and actual READY projection hydrate");
    if (!initialized) { application_notifications_release(&g_app); return; }
    auto metrics = application_notifications_fixture_metrics(&g_app);
    check(metrics.storage_committed_bytes>0 && metrics.storage_committed_bytes<metrics.storage_reserved_bytes &&
        !metrics.thread_storage.committed_bytes,"notification application: small READY commits only populated notification prefixes");
    memory_trace(utf8_literal("small_ready"),metrics);
    check(!metrics.ready && metrics.live && metrics.guilds == 1 && metrics.overrides == 1,
        "notification application: copied READY settings do not arm before completion");
    check(message(100, 11) && f.delivered == 0,
        "notification application: validated pre-completion create cannot emit");
    check(publish(nullptr, f.completion, {}) && application_notifications_fixture_metrics(&g_app).ready,
        "notification application: producer completion arms hydrated owner");
    check(message(500, 12) && f.delivered == 1 && f.last.target.profile_id == 77 &&
        f.last.target.channel_id == 21 && f.last.target.message_id == 500 && body(utf8_literal("hello")) &&
        f.consumed.kind == MessageEventPumpConsumeKind::ignored_dispatch && f.messages.count == 0,
        "notification application: real off-selected create emits through observer without selected timeline residency");
    const auto* other = chat_state_store_find_channel(f.ui.chat_state, 21);
    check(other && other->last_message_id == 500 && message(400, 13) && f.delivered == 2 &&
        other->last_message_id == 500 && f.last.target.message_id == 400,
        "notification application: older admitted live create is observed even when latest-message counter stays unchanged");
    check(message(400, 14) && f.delivered == 2 && message(401, 13) && f.delivered == 2,
        "notification application: message identity and nonincreasing sequence both suppress duplicate delivery");
    check(lifecycle(GatewayClientEventKind::reconnecting) && message(402, 15) && f.delivered == 2 &&
        lifecycle(GatewayClientEventKind::resumed) && message(500, 16) && f.delivered == 2 &&
        message(403, 17) && f.delivered == 3,
        "notification application: reconnect suppresses; RESUMED retains identity suppression and admits fresh creates");

    f.relationship_flags = notification_gate_blocked;
    check(message(510, 18) && f.delivered == 3, "notification application: blocked relationship suppresses independently");
    f.relationship_flags = notification_gate_ignored;
    check(message(511, 19) && f.delivered == 3, "notification application: ignored relationship suppresses independently");
    f.relationship_flags = 0; f.relationship_known = false;
    check(message(512, 20) && f.delivered == 3, "notification application: unknown relationship authority fails closed");
    f.relationship_known = true;
    check(dispatch(utf8_literal("CHANNEL_UPDATE"), utf8_literal(R"({"id":"21","guild_id":"10","type":0,"permission_overwrites":[{"id":"10","type":0,"allow":"0","deny":"1024"}]})")) &&
        message(513, 21) && f.delivered == 3,
        "notification application: actual channel permission mutation suppresses inaccessible creates");
    check(dispatch(utf8_literal("CHANNEL_UPDATE"), utf8_literal(R"({"id":"21","guild_id":"10","type":0,"permission_overwrites":[]})")) &&
        message(514, 22) && f.delivered == 4, "notification application: restored view permission admits subsequent live create");
    g_app.preferences.desktop_enabled = false;
    check(message(515, 23) && f.delivered == 4, "notification application: device-local popup toggle suppresses only desktop sink");
    g_app.preferences.desktop_enabled = true;
    check(message(516, 24, 21, utf8_literal("\"hello\""), 1) && f.delivered == 4 &&
        message(517, 25, 21, utf8_literal("\"hello\""), 2, utf8_literal(",\"flags\":4096")) && f.delivered == 4,
        "notification application: self authored and suppress-notifications creates do not emit");
    check(message(518, 26, 21, utf8_literal("\"hello\""), 2, {}, utf8_literal("Peer"), 1048576) && f.delivered == 4,
        "notification application: actual author policy metadata suppresses known spammer");

    check(message(520, 27, 21, utf8_literal("\"before ||private secret|| after\"")) && f.delivered == 5 &&
        body(utf8_literal("before [Spoiler] after")), "notification application: preview redacts spoiler bytes through production markdown");
    check(message(521, 28, 21, utf8_literal("\"\"")) && f.delivered == 6 && body(utf8_literal("New message")),
        "notification application: empty content has generic preview");
    check(message(522, 29, 21, utf8_literal("\"prefix\\u0000private\"")) && f.delivered == 7 && safe_text() &&
        (f.last.body_bytes == 0 || body(utf8_literal("New message"))),
        "notification application: embedded NUL cannot leak a truncated raw content suffix");
    check(message(523, 30, 21, utf8_literal("\"hello\""), 2, {}, utf8_literal("Peer\\u0000private")) &&
        f.delivered == 8 && safe_text() && f.last.title_bytes == 0,
        "notification application: embedded author NUL cannot become a truncated private title");
    char oversized[780]; oversized[0] = '"';
    for (u32 i = 1; i < sizeof(oversized) - 1; ++i) oversized[i] = 's';
    oversized[sizeof(oversized) - 1] = '"';
    check(message(524, 31, 21, {oversized, sizeof(oversized)}) && f.delivered == 9 && body(utf8_literal("New message")),
        "notification application: oversize preview uses generic label rather than raw source prefix");

    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"WgcKBQoDZG5k"}})")) &&
        message(530, 32) && f.delivered == 9, "notification application: actual partial DND protobuf update suppresses");
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":""}})")) &&
        message(531, 33) && f.delivered == 9, "notification application: sparse empty partial proto preserves existing DND");
    check(global(utf8_literal(R"({"partial":false,"settings":{"type":1,"proto":""}})")) &&
        message(532, 34) && f.delivered == 10, "notification application: full global replacement clears old DND to connected defaults");
    check(settings(utf8_literal(R"({"guild_id":"10","version":11,"muted":true})")) &&
        message(540, 35) && f.delivered == 10 && settings(utf8_literal(R"({"guild_id":"10","version":12})")) &&
        message(541, 36) && f.delivered == 10,
        "notification application: sparse settings merge keeps explicit mute while full entry replaces overrides");
    check(settings(utf8_literal(R"({"guild_id":"10","version":13,"muted":false})")) &&
        message(542, 37) && f.delivered == 11, "notification application: explicit unmute resumes eligibility");
    metrics = application_notifications_fixture_metrics(&g_app);
    check(metrics.overrides == 0, "notification application: full guild update replaces prior channel override set");

    check(dispatch(utf8_literal("THREAD_CREATE"), utf8_literal(R"({"id":"30","guild_id":"10","parent_id":"21","type":11,"name":"joined","thread_metadata":{"archived":false},"member":{"id":"30","user_id":"1","join_timestamp":"2026-10-01T00:00:00Z","flags":2}})")) &&
        application_notifications_fixture_metrics(&g_app).threads == 1 && message(550, 38, 30) && f.delivered == 12,
        "notification application: real thread channel sidecar hydrates self membership outside selected participant cache");
    check(dispatch(utf8_literal("THREAD_MEMBER_UPDATE"), utf8_literal(R"({"id":"30","guild_id":"10","user_id":"1","join_timestamp":"2026-10-01T00:00:00Z","flags":8})")) &&
        message(551, 39, 30) && f.delivered == 12,
        "notification application: live self thread NONE flags suppress without touching selected timeline");
    check(dispatch(utf8_literal("THREAD_MEMBER_UPDATE"), utf8_literal(R"({"id":"30","guild_id":"10","user_id":"1","join_timestamp":"2026-10-01T00:00:00Z","flags":2})")) &&
        message(552, 40, 30) && f.delivered == 13,
        "notification application: full self thread update restores ALL and unmuted defaults");
    check(dispatch(utf8_literal("CHANNEL_DELETE"), utf8_literal(R"({"id":"30","guild_id":"10","type":11})")) &&
        application_notifications_fixture_metrics(&g_app).threads == 0,
        "notification application: channel deletion clears notification membership alongside ordinary state");

    check(dispatch(utf8_literal("USER_UPDATE"), utf8_literal(R"({"id":"1","restricted_schedule":{"rules":[{"enabled":true}]}})")) &&
        message(560, 41) && f.delivered == 13,
        "notification application: enabled unknown restriction invalidates earlier READY known-clear authority");
    check(dispatch(utf8_literal("USER_UPDATE"), utf8_literal(R"({"id":"1","restricted_schedule":{"rules":[]}})")) &&
        message(561, 42) && f.delivered == 14,
        "notification application: proven empty restriction rules restore explicit clear authority");

    const u32 before_reset = f.delivered;
    check(hydrate(ready, 1) && message(500, 2) && f.delivered == before_reset + 1,
        "notification application: fresh READY resets version and old sequence/message identities");
    check(settings(utf8_literal(R"({"guild_id":"10","version":11,"message_notifications":2})")) &&
        message(570, 3) && f.delivered == before_reset + 1,
        "notification application: post-READY settings NONE suppresses live create");
    // A new authoritative READY admits its lower version instead of treating
    // its defaults as a stale partial mutation from the previous session.
    check(hydrate(ready, 1) && message(570, 2) && f.delivered == before_reset + 2,
        "notification application: lower-version fresh session replaces prior NONE policy");
    const u32 before_partial = f.delivered;
    check(snapshot(utf8_literal(R"({"entries":[{"guild_id":"99","message_notifications":2}],"partial":true,"version":20})")),
        "notification application: actual partial snapshot producer admission succeeds");
    metrics = application_notifications_fixture_metrics(&g_app);
    check(metrics.guilds == 2 && metrics.overrides == 1,
        "notification application: partial snapshot preserves omitted guild and override counts");
    check(message(571, 3) && f.delivered == before_partial + 1,
        "notification application: partial snapshot preserves omitted guild live eligibility");
    check(snapshot(utf8_literal(R"({"entries":[],"partial":false,"version":1})")),
        "notification application: actual full snapshot producer admission succeeds");
    metrics = application_notifications_fixture_metrics(&g_app);
    check(metrics.guilds == 0 && metrics.overrides == 0,
        "notification application: full snapshot replaces prior guild and override counts");
    check(message(572, 4) && f.delivered == before_partial + 2,
        "notification application: full snapshot reset version retains live default eligibility");
    const u32 before_metadata = f.delivered;
    check(dispatch(utf8_literal("GUILD_UPDATE"), utf8_literal(R"({"id":"10","name":"Guild","owner_id":"99","roles":[{"id":"10","name":"@everyone","permissions":"68608"}],"lurking":true})")) &&
        message(580, 5) && f.delivered == before_metadata &&
        dispatch(utf8_literal("GUILD_UPDATE"), utf8_literal(R"({"id":"10","name":"Guild","owner_id":"99","roles":[{"id":"10","name":"@everyone","permissions":"68608"}],"lurking":false})")) &&
        message(581, 6) && f.delivered == before_metadata + 1,
        "notification application: actual guild metadata toggles lurking gate without unrelated state reset");
    const u32 before_partial_ready=f.delivered;
    check(hydrate(ready,1) && message(590,2) && f.delivered==before_partial_ready+1,
        "notification application: full READY restores the initial guild policy");
    check(hydrate(ready,1,false,utf8_literal(R"({"entries":[{"guild_id":"99","message_notifications":2}],"partial":true,"version":20})")),
        "notification application: same-account partial READY producer succeeds");
    metrics=application_notifications_fixture_metrics(&g_app);
    check(metrics.guilds==2 && metrics.overrides==1 && message(591,2) && f.delivered==before_partial_ready+2,
        "notification application: same-account partial READY retains omitted settings and resets numeric create sequence");
    const u32 before_snapshot_versions=f.delivered;
    check(snapshot(utf8_literal(R"({"entries":[{"guild_id":"99","version":100,"message_notifications":0},{"guild_id":"10","version":4,"message_notifications":2}],"partial":false,"version":100})")),
        "notification application: full snapshot admits descending per-entry versions");
    metrics=application_notifications_fixture_metrics(&g_app);
    check(metrics.guilds==2 && !metrics.overrides && message(600,3) && f.delivered==before_snapshot_versions,
        "notification application: lower-version full snapshot entry retains explicit NONE rather than implicit ALL");
    check(snapshot(utf8_literal(R"({"entries":[{"guild_id":"99","version":200,"message_notifications":2},{"guild_id":"10","version":5,"message_notifications":0}],"partial":true,"version":200})")),
        "notification application: partial snapshot admits descending per-entry versions");
    check(message(601,4) && f.delivered==before_snapshot_versions+1,
        "notification application: lower-version partial snapshot entry restores explicit ALL");
    check(settings(utf8_literal(R"({"guild_id":"10","version":150,"message_notifications":2})")) &&
        message(602,5) && f.delivered==before_snapshot_versions+2,
        "notification application: snapshot admission preserves maximum version for later stale live update guard");

    GatewayClientEvent barrier{};barrier.kind=GatewayClientEventKind::dispatch;
    barrier.dispatch_kind=GatewayClientDispatchKind::notification_snapshot_begin;
    barrier.notification.flags=gateway_notification_partial|gateway_notification_version_known;
    barrier.notification.version=201;
    const u32 before_barrier=f.delivered;
    check(publish(nullptr,barrier,{}) && message(603,6) && f.delivered==before_barrier,
        "notification application: open typed policy snapshot blocks interleaved live create admission");
    barrier.dispatch_kind=GatewayClientDispatchKind::notification_snapshot_complete;
    check(publish(nullptr,barrier,{}) && message(604,7) && f.delivered==before_barrier+1,
        "notification application: successful policy completion restores previously armed eligibility without rearming READY");
    const u32 before_incomplete=f.delivered;
    check(snapshot(utf8_literal(R"({"entries":[{"guild_id":"10","message_notifications":99}],"partial":true,"version":202})")) &&
        message(605,8) && f.delivered==before_incomplete,
        "notification application: malformed snapshot remains typed incomplete and blocks prior known eligibility");
    check(snapshot(utf8_literal(R"({"entries":[],"partial":false,"version":203})")) &&
        message(606,9) && f.delivered==before_incomplete+1,
        "notification application: successful full policy snapshot recovers incomplete barrier on already armed session");

    const u32 before_empty_status=f.delivered;
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"WgcKBQoDZG5k"}})")) &&
        message(607,10) && f.delivered==before_empty_status,
        "notification application: known DND establishes full empty-wrapper regression precondition");
    // root status(11), status StringValue(1), explicitly present empty wrapper.
    check(global(utf8_literal(R"({"partial":false,"settings":{"type":1,"proto":"WgIKAA=="}})")) &&
        message(608,11) && f.delivered==before_empty_status+1,
        "notification application: full present empty status wrapper resolves effective connected ONLINE");
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"WgcKBQoDZG5k"}})")) &&
        message(609,12) && f.delivered==before_empty_status+1,
        "notification application: known DND establishes partial empty-wrapper regression precondition");
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"WgIKAA=="}})")) &&
        message(610,13) && f.delivered==before_empty_status+2,
        "notification application: partial present empty status wrapper replaces DND with effective connected ONLINE");
    check(global(utf8_literal(R"({"partial":false,"settings":{"type":1,"proto":"AB=="}})")) &&
        message(611,14) && f.delivered==before_empty_status+2,
        "notification application: malformed entire proto stays unknown rather than receiving valid-wrapper defaults");
    check(global(utf8_literal(R"({"partial":false,"settings":{"type":1,"proto":"WgIKAA=="}})")) &&
        message(612,15) && f.delivered==before_empty_status+3,
        "notification application: valid full empty wrapper recovers unknown entire proto authority");
    const u32 before_group_replacement=f.delivered;
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"OgQqAggB"}})")) &&
        message(613,16) && f.delivered==before_group_replacement,
        "notification application: present notifications group enables indefinite quiet mode");
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"WgA="}})")) &&
        message(614,17) && f.delivered==before_group_replacement,
        "notification application: empty partial status group preserves unrelated quiet group");
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"OgA="}})")) &&
        message(615,18) && f.delivered==before_group_replacement+1,
        "notification application: empty partial notifications group clears quiet and expiry defaults");
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"WhAKBQoDZG5kIQEAAAAAAAAA"}})")) &&
        message(616,19) && f.delivered==before_group_replacement+2,
        "notification application: expired finite DND projects connected ONLINE before admission");
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"WgcKBQoDZG5k"}})")) &&
        message(617,20) && f.delivered==before_group_replacement+2,
        "notification application: replacement status group clears omitted old expiry so new DND remains indefinite");
    check(global(utf8_literal(R"({"partial":true,"settings":{"type":1,"proto":"WgA="}})")) &&
        message(618,21) && f.delivered==before_group_replacement+3,
        "notification application: empty partial status group clears DND to effective connected ONLINE");
    // Hold the actual production continuation while admitted creates reuse the
    // producer payload and stack preview. Storage must retain copied FIFO text.
    check(hydrate(ready,1),"notification application: fresh READY prepares bounded burst");
    f.auto_drain=false;
    const u32 before_burst=f.delivered;
    bool burst=true;
    for(u32 i=0;i<32;++i) burst=burst && application_notifications_can_pop(&g_app) &&
        message(700+i,2+i,21,i?utf8_literal("\"later\""):utf8_literal("\"first copied body\""));
    metrics=application_notifications_fixture_metrics(&g_app);
    check(burst && f.delivered==before_burst && metrics.display_outstanding==32 &&
        metrics.display_owner_sizeof_bytes<24*1024 && metrics.display_vm_committed_bytes==24*1024 &&
        !application_notifications_can_pop(&g_app) && application_notifications_gateway_pending(&g_app),
        "notification application: copied burst fills exactly32 rows and pre-pop admission applies backpressure");
    memory_trace(utf8_literal("full_display"),metrics);
    application_notifications_drain(&g_app);
    metrics=application_notifications_fixture_metrics(&g_app);
    check(f.delivered==before_burst+1 && f.last.target.message_id==700 && body(utf8_literal("first copied body")) &&
        metrics.display_outstanding==31 && application_notifications_can_pop(&g_app) &&
        !application_notifications_gateway_pending(&g_app),
        "notification application: one outer continuation submits one copied FIFO intent and releases producer capacity");
    application_notifications_ack(&g_app,21);
    application_notifications_drain(&g_app);
    check(!application_notifications_fixture_metrics(&g_app).display_outstanding && f.delivered==before_burst+1,
        "notification application: ACK purges all pending channel intents before submission");
    check(message(750,40) && application_notifications_fixture_metrics(&g_app).display_outstanding==1 &&
        hydrate(ready,1) && !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification application: READY discards prior-generation copied intents");
    check(message(751,2),"notification application: copied intent prepares current permission revalidation");
    auto* queued_channel=const_cast<ChannelRecord*>(chat_state_store_find_channel(f.ui.chat_state,21));
    if(queued_channel) queued_channel->actions&=~channel_action_view;
    application_notifications_drain(&g_app);
    check(queued_channel && f.delivered==before_burst+1 && !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification application: lost current view permission retires copied intent without delivery");
    check(hydrate(ready,1) && message(752,2),"notification application: copied intent prepares device toggle revalidation");
    g_app.preferences.desktop_enabled=false;
    application_notifications_drain(&g_app);
    check(f.delivered==before_burst+1 && !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification application: disabled device popup sink retires copied intent without delivery");
    g_app.preferences.desktop_enabled=true;
    check(hydrate(ready,1) && message(790,2) && message(791,3) &&
        dispatch(utf8_literal("MESSAGE_DELETE"),utf8_literal(R"({"id":"790","channel_id":"21"})")) &&
        application_notifications_fixture_metrics(&g_app).display_outstanding==1 && message(792,4) && message(793,5) &&
        dispatch(utf8_literal("MESSAGE_DELETE_BULK"),utf8_literal(R"({"ids":["792","793"],"channel_id":"21"})")) &&
        application_notifications_fixture_metrics(&g_app).display_outstanding==1,
        "notification application: single and bulk deletion purge matching copied intents using existing pump scratch");
    application_notifications_drain(&g_app);
    check(f.delivered==before_burst+2 && f.last.target.message_id==791,
        "notification application: delete cancellation preserves the other FIFO message");
    check(message(794,6),"notification application: copied intent prepares required action revalidation");
    g_app.account.status.required_action=AccountRequirement::agreements;
    application_notifications_drain(&g_app);
    check(f.delivered==before_burst+2 && !application_notifications_fixture_metrics(&g_app).display_outstanding,
        "notification application: current required action blocks a previously admitted copied intent");
    g_app.account.status.required_action=AccountRequirement::none;
    f.auto_drain=true;
    check(settings(utf8_literal(R"({"guild_id":"10","muted":true})")) &&
        dispatch(utf8_literal("THREAD_CREATE"),utf8_literal(R"({"id":"31","guild_id":"10","parent_id":"21","type":11,"name":"joined","thread_metadata":{"archived":false},"member":{"id":"31","user_id":"1","join_timestamp":"2026-10-01T00:00:00Z","flags":2}})")),
        "notification application: guild settings and self thread membership precede unavailable transition");
    check(dispatch(utf8_literal("GUILD_DELETE"),utf8_literal(R"({"id":"10","unavailable":true})")),
        "notification application: unavailable guild preserves ordinary state admission");
    metrics=application_notifications_fixture_metrics(&g_app);
    check(metrics.guilds==1 && metrics.threads==0,
        "notification application: unavailable retains notification choices while clearing self thread membership");
    check(dispatch(utf8_literal("GUILD_DELETE"),utf8_literal(R"({"id":"10","unavailable":false})")) &&
        !application_notifications_fixture_metrics(&g_app).guilds,
        "notification application: actual leave frees guild notification authority");
    application_notifications_account_stop(&g_app);
    metrics = application_notifications_fixture_metrics(&g_app);
    check(!metrics.ready && !metrics.live && !metrics.guilds && !metrics.overrides && !metrics.threads &&
        !metrics.seen_count && f.navigation_calls == 0 && !metrics.audio_worker_started && !metrics.audio_submitted,
        "notification application: account stop discards owner authority and never navigates or opens OS delivery");
    application_notifications_release(&g_app);
    check(!g_app.notifications, "notification application: release clears nonemitting owner");
    sound_regressions(check);
    controls_regressions(check);
    pack_regressions(check);
    retention_regressions(check);
}
} // namespace maicord

namespace maicord::app_detail {
void application_taskbar_attention_notify() noexcept {
    ++notification_application_probe::taskbar_flashes;
}
bool application_friends_notification_gates(u64, u32* gates) noexcept {
    if (!gates || !notification_application_probe::fixture.relationship_known) return false;
    *gates = notification_application_probe::fixture.relationship_flags;
    return true;
}
bool application_navigate_notification(u64, u64) noexcept {
    ++notification_application_probe::fixture.navigation_calls;
    return false;
}
}
