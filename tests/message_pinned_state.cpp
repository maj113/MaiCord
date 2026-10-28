#include "maicord/app/gateway_client.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/message_history_sync.hpp"
#include "maicord/model/message_reconcile.hpp"
#include <stddef.h>
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/guild_member_store.hpp"

namespace maicord::message_pinned_state_test {
static_assert(sizeof(MessageRecord) == 64);
static_assert(sizeof(MessageInput) == 72);
static_assert(offsetof(GatewayMessageEvent, snapshot) == 456);
static_assert(sizeof(GatewayMessageEvent) ==
              456 + sizeof(GatewayMessageSnapshot) + sizeof(GatewayMessagePoll) + sizeof(GatewayMessageThread) +
                  sizeof(GatewayMessageCall));
static_assert(sizeof(GatewayClientMessageMutation) == 88);
u32 failures{};
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
struct Fixture {
    MessageRecord records[8];
    u32 order[8];
    u8 text[4096];
    MessageStore store;
    u32 heights[8];
    u64 sums[8];
    UiVariableList list;
    MessageTimeline timeline;
    u8 pump_payload[kMessageEventPumpPayloadCapacity];
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds];
    MessageEventPump pump;
    ChatStateStore state;
    u64 guild_ids[1];
    u32 structurals[2048], message_structurals[2048];
    char decoded_text[4096];
    GatewayMessageEvent messages[8];
    char packed[16384];
} fixture{};
constexpr u32 pinned_mention = message_presentation_pinned | message_presentation_mentions_current_user;
constexpr u32 live_badge = message_presentation_read_badge_known | message_presentation_read_badge;
const MessageRecord* resident(u64 id) noexcept {
    const auto found = message_store_find(fixture.store, id);
    return found.ok() ? message_store_resolve(fixture.store, found.handle) : nullptr;
}
bool flags(u64 id, u32 expected) noexcept {
    const auto* record = resident(id);
    return record && record->presentation_flags == expected;
}
bool route(Utf8View json, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    GatewayEnvelope envelope{};
    return gateway_decode_envelope(json, {fixture.structurals, 2048}, 32, &envelope).ok() &&
           gateway_client_route_dispatch(
               envelope, {fixture.structurals, 2048}, 32, {fixture.packed, sizeof(fixture.packed)},
               {fixture.bulk_ids, kGatewayBulkDeleteMaximumIds}, 42, event, payload)
               .ok();
}
bool apply(Utf8View json) noexcept {
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    if (!route(json, &event, &payload))
        return false;
    const auto result = message_event_pump_consume(&fixture.pump, event, payload);
    return result.ok() && result.kind == MessageEventPumpConsumeKind::applied && result.repaint;
}
void run() noexcept {
    check(
        message_store_initialize(&fixture.store, {fixture.records, 8}, {fixture.order, 8},
                                 {fixture.text, sizeof(fixture.text)}) &&
            ui_variable_list_initialize(&fixture.list, {fixture.heights, 8}, {fixture.sums, 8}, 0, 100) &&
            message_timeline_initialize(&fixture.timeline, &fixture.store, &fixture.list, 100, 100, true) &&
            message_event_pump_initialize(
                &fixture.pump, &fixture.timeline, {fixture.pump_payload, sizeof(fixture.pump_payload)},
                {fixture.bulk_ids, kGatewayBulkDeleteMaximumIds}, MessageContentAvailability::available),
        "pinned state: native store/timeline/pump initialize");
    fixture.state.self_user_id = 42;
    fixture.state.selected_channel_id = 7;
    check(message_event_pump_set_state_store(&fixture.pump, &fixture.state, {fixture.guild_ids, 1}),
          "pinned state: self mention projection connects");
    constexpr auto create = utf8_literal(
        R"({"op":0,"t":"MESSAGE_CREATE","s":1,"d":{"id":"100","channel_id":"7","author":{"id":"41"},"content":"hello","timestamp":"2026-10-04T12:00:00Z","pinned":true,"mentions":[{"id":"42","username":"self"}]}})");
    GatewayClientEvent packed{};
    ConstSlice<u8> payload{};
    GatewayClientEvent body_event{};
    ConstSlice<u8> body{};
    GatewayMessageEvent projected{};
    check(
        route(create, &packed, &payload) && (packed.message.fields & gateway_message_field_pinned) &&
            (packed.message.mention_flags & gateway_client_message_pinned) &&
            gateway_client_notification_trim(packed, payload, &body_event, &body) &&
            gateway_client_event_as_message(body_event, body, &projected) && projected.pinned &&
            (projected.fields & gateway_message_field_pinned) && projected.mentions_current_user,
        "pinned state: actual Gateway parser, worker packing and UI unpacking preserve pin presence/value");
    check(message_event_pump_consume(&fixture.pump, packed, payload).ok() && flags(100, pinned_mention | live_badge),
          "pinned state: create keeps both pin and current-user mention");
    check(apply(utf8_literal(
              R"({"op":0,"t":"MESSAGE_UPDATE","s":2,"d":{"id":"100","channel_id":"7","pinned":false}})")) &&
              flags(100, message_presentation_mentions_current_user | live_badge),
          "pinned state: explicit false update clears only pin and retains omitted content/mention");
    check(apply(utf8_literal(
              R"({"op":0,"t":"MESSAGE_UPDATE","s":3,"d":{"id":"100","channel_id":"7","pinned":true}})")) &&
              flags(100, pinned_mention | live_badge),
          "pinned state: partial true update restores pin without replacing mention");
    check(
        apply(utf8_literal(
            R"({"op":0,"t":"MESSAGE_UPDATE","s":4,"d":{"id":"100","channel_id":"7","content":"edited"}})")) &&
            flags(100, pinned_mention | live_badge),
        "pinned state: omitted pin and mention survive ordinary content edit");
    check(
        apply(utf8_literal(
            R"({"op":0,"t":"MESSAGE_UPDATE","s":5,"d":{"id":"100","channel_id":"7","mention_everyone":false,"mentions":[],"mention_roles":[]}})")) &&
            flags(100, message_presentation_pinned | live_badge),
        "pinned state: explicit empty mentions clear highlight without clearing pin");
    check(
        apply(utf8_literal(
            R"({"op":0,"t":"MESSAGE_UPDATE","s":6,"d":{"id":"100","channel_id":"7","mention_everyone":true}})")) &&
            flags(100, pinned_mention | live_badge),
        "pinned state: mention-only update adds highlight while preserving pin");

    constexpr Utf8View invalid[]{
        utf8_literal(R"({"id":"100","channel_id":"7","pinned":true,"pinned":false})"),
        utf8_literal(R"({"id":"100","channel_id":"7","pinned":null})"),
        utf8_literal(R"({"id":"100","channel_id":"7","pinned":1})"),
        utf8_literal(R"({"id":"100","channel_id":"7","pinned":"true"})")};
    for (Utf8View input : invalid) {
        GatewayMessageEvent failed{};
        failed.id = 999;
        failed.pinned = true;
        const auto result = gateway_decode_message_update(
            {input, 0, JsonValueKind::object}, {fixture.structurals, 2048}, 32,
            {fixture.decoded_text, sizeof(fixture.decoded_text)}, &failed);
        check(!result.ok() && !failed.id && !failed.fields && !failed.pinned && flags(100, pinned_mention | live_badge),
              "pinned state: duplicate/nonboolean pin rejects atomically before resident mutation");
    }
    const auto* prior = resident(100);
    const auto revision = prior ? prior->revision : 0;
    check(message_store_set_presentation_flags(&fixture.store, 100, pinned_mention | live_badge) &&
              resident(100)->revision == revision &&
              !message_store_set_presentation_flags(&fixture.store, 100, 1u << 31) &&
              flags(100, pinned_mention | live_badge),
          "pinned state: known-bit setter accepts pin and unchanged state without revision churn");
    MessageInput invalid_flags{200, 7, 41, 200, 0, utf8_literal("x"), 0};
    invalid_flags.presentation_flags = 1u << 8;
    check(!message_store_upsert(&fixture.store, invalid_flags).ok() && !resident(200),
          "pinned state: insertion rejects unknown presentation bits before narrowing to u8");

    GatewayMessageEvent full{};
    full.id = 100;
    full.channel_id = 7;
    full.author_id = 41;
    full.timestamp_ms = 100;
    full.content = utf8_literal("refresh");
    full.fields = gateway_message_field_id | gateway_message_field_channel_id |
                  gateway_message_field_author_id | gateway_message_field_content |
                  gateway_message_field_timestamp;
    check(message_reconcile_validate_create(&fixture.store, full, MessageContentAvailability::available)
                  .ok() &&
              message_reconcile_create(&fixture.store, full, MessageContentAvailability::available).ok() &&
              flags(100, pinned_mention | live_badge),
          "pinned state: full duplicate create/history refresh retains omitted pin and mention bits");
    full.fields |= gateway_message_field_pinned;
    check(message_reconcile_create(&fixture.store, full, MessageContentAvailability::available).ok() &&
              flags(100, message_presentation_mentions_current_user | live_badge),
          "pinned state: explicit false in full refresh clears cached pin while retaining mention bits");
    GatewayMessageEvent partial{};
    partial.id = 100;
    partial.channel_id = 7;
    partial.fields = gateway_message_field_id | gateway_message_field_channel_id;
    partial.pinned = true;
    check(message_reconcile_update(&fixture.store, partial, MessageContentAvailability::available).ok() &&
              flags(100, message_presentation_mentions_current_user | live_badge),
          "pinned state: value without presence cannot manufacture pinned state");

    check(message_timeline_reset(&fixture.timeline), "pinned state: empty history fixture resets");
    constexpr auto history = utf8_literal(
        R"([{"id":"300","channel_id":"7","author":{"id":"41"},"content":"<@42>","timestamp":"2026-10-04T12:00:00Z","pinned":true},{"id":"200","channel_id":"7","author":{"id":"41"},"content":"plain","timestamp":"2026-10-04T11:00:00Z","pinned":false,"type":6}])");
    const auto decoded = decode_message_history_page(
        history, {fixture.structurals, 2048}, {fixture.message_structurals, 2048}, 32,
        {fixture.decoded_text, sizeof(fixture.decoded_text)}, {fixture.messages, 8});
    check(decoded.ok() && decoded.page.count == 2 && fixture.messages[0].pinned &&
              !fixture.messages[1].pinned && (fixture.messages[0].fields & gateway_message_field_pinned) &&
              (fixture.messages[1].fields & gateway_message_field_pinned) &&
              message_timeline_apply_history_page(&fixture.timeline, {fixture.messages, decoded.page.count},
                                                  MessageHistoryDirection::newer,
                                                  MessageContentAvailability::available)
                  .ok() &&
              flags(300, message_presentation_pinned) && flags(200, message_presentation_none) &&
              resident(200)->type == 6,
          "pinned state: actual full history decoder and timeline project independent true/false pins");
    check(message_history_apply_mention_flags(&fixture.timeline, &fixture.state, {fixture.messages, 2}) &&
              flags(300, pinned_mention),
          "pinned state: history current-user mention recomputation preserves pin");
    fixture.messages[0].content = utf8_literal("plain");
    check(message_history_apply_mention_flags(&fixture.timeline, &fixture.state, {fixture.messages, 2}) &&
              flags(300, message_presentation_pinned),
          "pinned state: history mention removal clears only highlight while retaining pin");

    constexpr Utf8View notices[]{
        utf8_literal(R"({"op":0,"t":"CHANNEL_PINS_UPDATE","s":7,"d":{"channel_id":"9"}})"),
        utf8_literal(
            R"({"op":0,"t":"CHANNEL_PINS_UPDATE","s":8,"d":{"channel_id":"9","guild_id":"5","last_pin_timestamp":"2026-10-05T01:02:03.123456+00:00"}})"),
        utf8_literal(
            R"({"op":0,"t":"CHANNEL_PINS_UPDATE","s":9,"d":{"channel_id":"9","guild_id":null,"last_pin_timestamp":null,"future":{"hint":true}}})")};
    for (Utf8View input : notices) {
        GatewayClientEvent notice{};
        ConstSlice<u8> notice_payload{};
        const auto count = fixture.store.count;
        check(route(input, &notice, &notice_payload) &&
                  notice.dispatch_kind == GatewayClientDispatchKind::channel_pins_update &&
                  notice.message.channel_id == 9 &&
                  notice.message.fields == gateway_message_field_channel_id && !notice.message.id &&
                  !notice.dispatch_payload_bytes && !notice_payload.count,
              "pinned state: actual channel pin notice routes as fixed-size channel identity without raw "
              "payload");
        const auto consumed = message_event_pump_consume(&fixture.pump, notice, notice_payload);
        check(consumed.ok() && consumed.kind == MessageEventPumpConsumeKind::ignored_dispatch &&
                  fixture.store.count == count && !fixture.pump.resync_required &&
                  flags(300, message_presentation_pinned),
              "pinned state: off-timeline channel pin notice is harmless to ordinary message projection");
    }
    constexpr Utf8View invalid_notices[]{
        utf8_literal(R"({"op":0,"t":"CHANNEL_PINS_UPDATE","d":{}})"),
        utf8_literal(R"({"op":0,"t":"CHANNEL_PINS_UPDATE","d":{"channel_id":null}})"),
        utf8_literal(R"({"op":0,"t":"CHANNEL_PINS_UPDATE","d":{"channel_id":"0"}})"),
        utf8_literal(R"({"op":0,"t":"CHANNEL_PINS_UPDATE","d":{"channel_id":9}})"),
        utf8_literal(R"({"op":0,"t":"CHANNEL_PINS_UPDATE","d":{"channel_id":"9","channel_id":"10"}})"),
        utf8_literal(R"({"op":0,"t":"CHANNEL_PINS_UPDATE","d":{"channel_id":"9","guild_id":false}})"),
        utf8_literal(
            R"({"op":0,"t":"CHANNEL_PINS_UPDATE","d":{"channel_id":"9","guild_id":"5","guild_id":"6"}})"),
        utf8_literal(
            R"({"op":0,"t":"CHANNEL_PINS_UPDATE","d":{"channel_id":"9","last_pin_timestamp":"bad"}})"),
        utf8_literal(
            R"({"op":0,"t":"CHANNEL_PINS_UPDATE","d":{"channel_id":"9","last_pin_timestamp":null,"last_pin_timestamp":null}})")};
    for (Utf8View input : invalid_notices) {
        GatewayClientEvent failed{};
        ConstSlice<u8> failed_payload{};
        check(!route(input, &failed, &failed_payload) &&
                  failed.dispatch_kind != GatewayClientDispatchKind::channel_pins_update &&
                  !failed.message.channel_id && !failed_payload.count,
              "pinned state: malformed or duplicate channel notice cannot publish a partial pin "
              "invalidation identity");
    }
    check(
        apply(utf8_literal(
            R"({"op":0,"t":"MESSAGE_CREATE","s":10,"d":{"id":"500","channel_id":"7","author":{"id":"41"},"content":"system notice","timestamp":"2026-10-05T02:00:00Z","type":6,"flags":0}})")) &&
            resident(500) && resident(500)->type == 6 && !resident(500)->flags,
        "pinned state: actual Gateway system type survives parser/worker/pump independently of message "
        "flags");
    check(
        apply(utf8_literal(
            R"({"op":0,"t":"MESSAGE_UPDATE","s":11,"d":{"id":"500","channel_id":"7","content":"updated notice"}})")) &&
            resident(500)->type == 6,
        "pinned state: omitted message type survives partial content update for system pin guard");
    check(
        apply(utf8_literal(
            R"({"op":0,"t":"MESSAGE_UPDATE","s":12,"d":{"id":"500","channel_id":"7","pinned":false}})")) &&
            resident(500)->type == 6,
        "pinned state: pin-only mutation cannot erase system message type");
    GatewayMessageEvent system_refresh{};
    system_refresh.id = 500;
    system_refresh.channel_id = 7;
    system_refresh.author_id = 41;
    system_refresh.timestamp_ms = 500;
    system_refresh.content = utf8_literal("refreshed notice");
    system_refresh.fields = gateway_message_field_id | gateway_message_field_channel_id |
                            gateway_message_field_author_id | gateway_message_field_content |
                            gateway_message_field_timestamp;
    check(
        message_reconcile_validate_create(&fixture.store, system_refresh,
                                          MessageContentAvailability::available)
                .ok() &&
            message_reconcile_create(&fixture.store, system_refresh, MessageContentAvailability::available)
                .ok() &&
            resident(500)->type == 6,
        "pinned state: full refresh with omitted type retains existing system type");
    check(apply(utf8_literal(
              R"({"op":0,"t":"MESSAGE_UPDATE","s":13,"d":{"id":"500","channel_id":"7","type":0}})")) &&
              resident(500)->type == 0,
          "pinned state: explicit zero message type clears retained type rather than treating zero as "
          "omitted");
}
} // namespace maicord::message_pinned_state_test
extern "C" __declspec(noreturn) void WINAPI maicord_message_pinned_state_entry() noexcept {
    maicord::message_pinned_state_test::run();
    ExitProcess(maicord::message_pinned_state_test::failures ? 1 : 0);
}
