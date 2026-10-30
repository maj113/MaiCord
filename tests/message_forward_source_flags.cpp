#include "maicord/app/gateway_client.hpp"
#include "maicord/model/message_reconcile.hpp"
#include "gateway_message_scratch.hpp"

namespace maicord {
namespace {
struct SourcePolicyFixture {
    u32 structurals[256]{};
    char text[1024]{}, mailbox[2048]{};
    MessageRecord records[2]{};
    u32 order[2]{};
    u8 owned_text[1024]{};
} fixture;
constexpr Utf8View create = utf8_literal(R"({"id":"900","channel_id":"7","author":{"id":"11"},"content":"source","timestamp":"2026-10-05T00:00:00Z","type":0,"poll":{},"call":{},"activity":{}})");
bool decode(Utf8View body, bool update, GatewayMessageEvent* event) noexcept {
    const JsonValue value{body, 0, JsonValueKind::object};
    return update ? gateway_decode_message_update(value, {fixture.structurals, 256}, 16,
        {fixture.text, 1024}, event).ok() : gateway_decode_message_create(value,
        {fixture.structurals, 256}, 16, {fixture.text, 1024}, event).ok();
}
const MessageRecord* record(const MessageStore& store) noexcept {
    const auto found = message_store_find(store, 900);
    return found.ok() ? message_store_resolve(store, found.handle) : nullptr;
}
}
void test_message_forward_source_flags(void (*check)(bool, const char*) noexcept) noexcept {
    GatewayMessageEvent event{};
    const bool decoded = decode(create, false, &event);
    check(decoded && event.forwarding_features == (gateway_message_forwarding_poll |
        gateway_message_forwarding_call | gateway_message_forwarding_activity) &&
        (event.fields & (gateway_message_field_poll | gateway_message_field_call | gateway_message_field_activity)) ==
        (gateway_message_field_poll | gateway_message_field_call | gateway_message_field_activity),
        "forward source: poll/call/activity object presence survives shared scalar codec");
    if (!decoded) return;
    MessageStore store{};
    check(message_store_initialize(&store, {fixture.records, 2}, {fixture.order, 2}, {fixture.owned_text, 1024}) &&
        message_reconcile_create(&store, event, MessageContentAvailability::available).ok() &&
        record(store) && record(store)->presentation_flags == kMessagePresentationForwardingObstacles,
        "forward source: retained obstacle flags use existing message record padding");
    check(decode(utf8_literal(R"({"id":"900","channel_id":"7","content":"edited"})"), true, &event) &&
        message_reconcile_update(&store, event, MessageContentAvailability::available).ok() &&
        record(store)->presentation_flags == kMessagePresentationForwardingObstacles,
        "forward source: omitted obstacle members retain independent source policy on partial update");
    check(decode(utf8_literal(R"({"id":"900","channel_id":"7","poll":null,"pinned":true})"), true, &event) &&
        message_reconcile_update(&store, event, MessageContentAvailability::available).ok() &&
        record(store)->presentation_flags == (message_presentation_has_call | message_presentation_has_activity |
            message_presentation_pinned),
        "forward source: explicit poll null clears one obstacle without losing call/activity or pinned status");
    check(message_store_set_presentation_flags(&store, 900, record(store)->presentation_flags |
        message_presentation_mentions_current_user) &&
        decode(utf8_literal(R"({"id":"900","channel_id":"7","call":null,"activity":null})"), true, &event) &&
        message_reconcile_update(&store, event, MessageContentAvailability::available).ok() &&
        record(store)->presentation_flags == (message_presentation_mentions_current_user | message_presentation_pinned),
        "forward source: independent explicit clears preserve mention and pin authority");
    check(!decode(utf8_literal(R"({"id":"900","channel_id":"7","poll":false})"), true, &event) &&
        !decode(utf8_literal(R"({"id":"900","channel_id":"7","activity":[]})"), true, &event) &&
        !decode(utf8_literal(R"({"id":"900","channel_id":"7","call":{},"call":null})"), true, &event),
        "forward source: malformed objects and duplicate members cannot silently grant eligibility");
    constexpr auto envelope_text = utf8_literal(R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"900","channel_id":"7","author":{"id":"11"},"content":"source","timestamp":"2026-10-05T00:00:00Z","poll":{"question":{"text":"Choice?"},"answers":[{"answer_id":1,"poll_media":{"text":"Here"}}],"expiry":null,"allow_multiselect":false,"layout_type":1},"call":null,"activity":{}}})");
    GatewayEnvelope envelope{}; GatewayClientEvent routed{}; ConstSlice<u8> payload{}; u64 ids[2]{};
    GatewayMessagePollAnswer answers[kGatewayMessagePollMaximumAnswers]{};
    GatewayMessagePollAnswerCount counts[kGatewayMessagePollMaximumAnswers]{};
    GatewayMessageRichScratch rich{};
    rich.poll = {{answers, kGatewayMessagePollMaximumAnswers}, {counts, kGatewayMessagePollMaximumAnswers}};
    const bool transported = gateway_decode_envelope(envelope_text, {fixture.structurals, 256}, 16, &envelope).ok() &&
        gateway_client_route_dispatch(envelope, {fixture.structurals, 256}, 16, {fixture.mailbox, 2048}, {ids, 2}, 1,
            &routed, &payload, gateway_test_message_scratch()).ok() && gateway_client_event_as_message(routed, payload, rich, &event);
    check(transported && event.forwarding_features == (gateway_message_forwarding_poll | gateway_message_forwarding_activity) &&
        (event.fields & gateway_message_field_call),
        "forward source: pointer-free mailbox preserves typed poll and object policy in unchanged mutation layout");
    if (transported) {
        auto invalid = routed; invalid.message.forwarding_features |= 8;
        check(!gateway_client_event_as_message(invalid, payload, rich, &event),
            "forward source: mailbox rejects unknown source-policy bits");
    }
}
} // namespace maicord
