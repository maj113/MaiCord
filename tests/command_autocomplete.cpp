#include "maicord/app/command_autocomplete.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {
void test_command_autocomplete(void (*check)(bool, const char*) noexcept) noexcept {
    u32 scratch[2048];
    CommandIndex index{};
    constexpr auto catalog = utf8_literal(
        R"({"applications":[{"id":"10","name":"Search"}],"application_commands":[{"id":"100","application_id":"10","version":"7","name":"find","type":1,"options":[{"name":"required","type":6,"required":true},{"name":"scope","type":3},{"name":"query","type":3,"autocomplete":true},{"name":"number","type":10,"autocomplete":true}]}]})");
    check(command_index_decode(catalog, {scratch, 2048}, &index), "autocomplete command index fixture");
    if (index.command_count != 1)
        return;
    u8 output[2048];
    ProtocolWriter writer{{output, sizeof(output)}, 0};
    Utf8View error{};
    check(
        command_arguments(index, index.commands[0], utf8_literal("scope:all"), &writer, &error,
                          utf8_literal("query"), utf8_literal("in progress")) &&
            utf8_equal(
                {reinterpret_cast<const char*>(output), writer.used},
                utf8_literal(
                    R"([{"name":"scope","type":3,"value":"all"},{"name":"query","type":3,"value":"in progress","focused":true}])")),
        "autocomplete includes completed context and skips unfilled required options");
    writer.used = 0;
    check(command_arguments(index, index.commands[0], {}, &writer, &error, utf8_literal("query"), {}) &&
              utf8_equal({reinterpret_cast<const char*>(output), writer.used},
                         utf8_literal(R"([{"name":"query","type":3,"value":"","focused":true}])")),
          "autocomplete requests suggestions for an empty newly focused option");
    writer.used = 0;
    check(command_arguments(index, index.commands[0], {}, &writer, &error, utf8_literal("number"),
                            utf8_literal("-1.")) &&
              utf8_equal({reinterpret_cast<const char*>(output), writer.used},
                         utf8_literal(R"([{"name":"number","type":10,"value":"-1.","focused":true}])")),
          "partial numeric autocomplete preserves the user query as a string");
    writer.used = 0;
    check(!command_arguments(index, index.commands[0], {}, &writer, &error, utf8_literal("scope"),
                             utf8_literal("x")),
          "non-autocomplete option cannot request focused interactions");

    writer.used = 0;
    check(command_arguments(index, index.commands[0], utf8_literal("required:123 number:1.5e-10"), &writer,
                            &error),
          "selected scientific numeric suggestions remain usable in command invocation");
    const Utf8View bad_numbers[] = {
        utf8_literal("required:123 number:01"), utf8_literal("required:123 number:1e"),
        utf8_literal("required:123 number:1e1000"), utf8_literal("required:123 number:9007199254740994"),
        utf8_literal("required:123 number:.5")};
    for (auto input : bad_numbers) {
        writer.used = 0;
        check(!command_arguments(index, index.commands[0], input, &writer, &error),
              "malformed or out-of-range numeric suggestions cannot reach interactions");
    }
    command_index_release(&index);

    CommandAutocomplete state{};
    check(command_autocomplete_observe(&state, {7, 100, 10}, 1000) &&
              command_autocomplete_next_wake(state) == 1150 &&
              !command_autocomplete_begin(&state, 1, 1149) && command_autocomplete_begin(&state, 1, 1150) &&
              command_autocomplete_next_wake(state) == 6150,
          "autocomplete debounce and response deadlines are explicit");
    check(command_autocomplete_observe(&state, {7, 100, 11}, 1200) && state.nonce == 0 &&
              !command_autocomplete_accept(
                  &state, utf8_literal(R"({"nonce":"1","choices":[{"name":"stale","value":"old"}]})"),
                  {scratch, 2048}) &&
              command_autocomplete_begin(&state, 2, 1350),
          "editing invalidates an in-flight autocomplete nonce before replacement begins");
    check(
        !command_autocomplete_accept(&state, utf8_literal(R"({"nonce":"1","choices":[]})"),
                                     {scratch, 2048}) &&
            command_autocomplete_accept(
                &state,
                utf8_literal(
                    R"({"nonce":"2","choices":[{"name":"New \u00e9","value":"quote\"value"},{"name":"Number","value":1.5}]})"),
                {scratch, 2048}),
        "late older response is ignored and matching gateway suggestions decode");
    check(state.phase == CommandAutocompletePhase::ready && state.suggestions.count == 2 &&
              utf8_equal(state.suggestions.choices[0].name, utf8_literal("New \xc3\xa9")) &&
              utf8_equal(state.suggestions.choices[0].value, utf8_literal("quote\"value")) &&
              state.suggestions.choices[1].kind == JsonValueKind::number &&
              !command_autocomplete_next_wake(state),
          "suggestions retain decoded text and typed values with no idle wake");
    check(!command_autocomplete_transport(&state, 2, false) &&
              state.phase == CommandAutocompletePhase::ready,
          "gateway result arriving before HTTP completion cannot be overwritten by it");
    check(!command_autocomplete_observe(&state, {7, 100, 11}, 1400),
          "unchanged query performs no new request or allocation");
    command_autocomplete_observe(&state, {8, 100, 11}, 1400);
    check(state.suggestions.count == 0 && !state.suggestions.storage.base &&
              command_autocomplete_begin(&state, 3, 1550),
          "changing channel releases previous suggestions");
    check(!command_autocomplete_accept(
              &state, utf8_literal(R"({"nonce":"3","choices":[{"name":"bad","value":{}}]})"),
              {scratch, 2048}) &&
              !command_autocomplete_tick(&state, 6549) && command_autocomplete_tick(&state, 6550) &&
              state.phase == CommandAutocompletePhase::failed && !command_autocomplete_next_wake(state),
          "invalid responses time out without a polling loop");
    command_autocomplete_observe(&state, {8, 100, 12}, 7000);
    command_autocomplete_begin(&state, 4, 7150);
    check(
        command_autocomplete_accept(&state, utf8_literal(R"({"nonce":4,"choices":[]})"), {scratch, 2048}) &&
            state.phase == CommandAutocompletePhase::ready && !state.suggestions.count,
        "empty suggestions are a successful result");

    const auto previous_storage = state.suggestions.storage.base;
    writer.used = 0;
    protocol_writer_append_literal(&writer, "{\"nonce\":4,\"choices\":[");
    for (u32 n = 0; n < 26; ++n) {
        if (n)
            protocol_writer_append_byte(&writer, ',');
        protocol_writer_append_literal(&writer, "{\"name\":\"x\",\"value\":\"y\"}");
    }
    protocol_writer_append_literal(&writer, "]}");
    check(!command_suggestions_decode({reinterpret_cast<const char*>(output), writer.used}, {scratch, 2048},
                                      4, &state.suggestions) &&
              state.suggestions.storage.base == previous_storage && !state.suggestions.count,
          "over-limit suggestions are rejected transactionally");
    command_autocomplete_cancel(&state);
    check(!command_autocomplete_next_wake(state) && !state.suggestions.storage.base &&
              !command_autocomplete_accept(&state, utf8_literal(R"({"nonce":"4","choices":[]})"),
                                           {scratch, 2048}),
          "closing picker cancels delayed replies and releases result storage");

    GatewayEnvelope envelope{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    char retained[512];
    u64 ids[1];
    constexpr auto message = utf8_literal(
        R"({"op":0,"t":"APPLICATION_COMMAND_AUTOCOMPLETE_RESPONSE","s":77,"d":{"nonce":"8","choices":[{"name":"result","value":"id"}]}})");
    check(gateway_decode_envelope(message, {scratch, 2048}, 32, &envelope).ok() &&
              gateway_client_route_dispatch(envelope, {scratch, 2048}, 32, {retained, sizeof(retained)},
                                            {ids, 1}, &event, &payload)
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::command_autocomplete &&
              event.sequence == 77 && payload.count == envelope.data.source.bytes &&
              bytes_equal(payload.data, envelope.data.source.data, payload.count),
          "gateway autocomplete response crosses worker boundary with owned payload");
    check(gateway_client_route_dispatch(envelope, {scratch, 2048}, 32, {retained, 1}, {ids, 1}, &event,
                                        &payload)
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::unknown && !payload.count,
          "oversized ancillary autocomplete response does not disconnect gateway");
}
} // namespace maicord
