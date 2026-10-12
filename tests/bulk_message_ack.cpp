#include "maicord/protocol/bulk_message_ack.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace bulk_ack_test {
u8 output[kBulkMessageAckMaximumBodyBytes];
BulkMessageAckEntry entries[kBulkMessageAckMaximumEntries + 1];
void fill() noexcept {
    for (u32 i = 0; i < sizeof(output); ++i)
        output[i] = 0xa5;
}
bool sentinel() noexcept {
    for (const auto byte : output)
        if (byte != 0xa5)
            return false;
    return true;
}
bool equal(u32 bytes, Utf8View text) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(output), bytes}, text);
}
} // namespace bulk_ack_test
void test_bulk_message_ack(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace bulk_ack_test;
    entries[0] = {1, 2, 0};
    entries[1] = {30, 40, 0};
    auto written = write_bulk_message_ack_body({entries, 2}, {output, sizeof(output)});
    check(
        written.ok() &&
            equal(written.bytes,
                  utf8_literal(
                      "{\"read_states\":[{\"channel_id\":\"1\",\"message_id\":\"2\",\"read_state_type\":0},"
                      "{\"channel_id\":\"30\",\"message_id\":\"40\",\"read_state_type\":0}]}")),
        "bulk ack: ordered exact JSON contains decimal strings, explicit state0 and no token");
    const u32 exact = written.bytes;
    check(write_bulk_message_ack_body({entries, 2}, {output, exact}).bytes == exact,
          "bulk ack: exact output boundary");
    for (u32 capacity = 0; capacity < exact; ++capacity) {
        fill();
        const auto failed = write_bulk_message_ack_body({entries, 2}, {output, capacity});
        check(!failed.bytes && failed.error == BulkMessageAckWriteError::capacity && sentinel(),
              "bulk ack: every short output is atomic");
    }
    for (u32 i = 0; i < kBulkMessageAckMaximumEntries; ++i)
        entries[i] = {~u64{0} - i, ~u64{0}, 0};
    written = write_bulk_message_ack_body({entries, 100}, {output, sizeof(output)});
    const Utf8View body{reinterpret_cast<const char*>(output), written.bytes};
    check(written.ok() && written.bytes == kBulkMessageAckMaximumBodyBytes && utf8_validate(body).valid &&
              output[written.bytes - 2] == ']' && output[written.bytes - 1] == '}',
          "bulk ack:100 full-width snowflakes exactly fill bounded UTF8 body");
    u32 channels{}, states{};
    constexpr auto channel = utf8_literal("\"channel_id\":");
    constexpr auto state = utf8_literal("\"read_state_type\":0");
    for (u32 i = 0; i < body.bytes; ++i) {
        if (i + channel.bytes <= body.bytes && bytes_equal(body.data + i, channel.data, channel.bytes))
            ++channels;
        if (i + state.bytes <= body.bytes && bytes_equal(body.data + i, state.data, state.bytes))
            ++states;
    }
    check(channels == 100 && states == 100, "bulk ack:100 entries are not truncated");
    fill();
    check(!write_bulk_message_ack_body({entries, 101}, {output, sizeof(output)}).ok() && sentinel(),
          "bulk ack:101 entries reject atomically");
    entries[99].channel_id = entries[0].channel_id;
    check(!write_bulk_message_ack_body({entries, 100}, {output, sizeof(output)}).ok() && sentinel(),
          "bulk ack: late duplicate channel validates before any output");
    entries[0] = {1, 2, 0};
    const BulkMessageAckEntry invalid[]{{0, 2, 0}, {1, 0, 0}, {1, 2, 1}, {1, 2, ~u32{0}}};
    for (const auto& entry : invalid)
        check(!write_bulk_message_ack_body({&entry, 1}, {output, sizeof(output)}).ok() && sentinel(),
              "bulk ack: zero IDs and alternate state types reject atomically");
    check(!write_bulk_message_ack_body({nullptr, 1}, {output, sizeof(output)}).ok() && sentinel() &&
              !write_bulk_message_ack_body({entries, 0}, {output, sizeof(output)}).ok() && sentinel() &&
              !write_bulk_message_ack_body({entries, 1}, {nullptr, 100}).ok(),
          "bulk ack: null and empty slices reject");
    const ConstSlice<BulkMessageAckEntry> overflow{
        reinterpret_cast<const BulkMessageAckEntry*>(~usize{0} - 8), 1};
    check(!write_bulk_message_ack_body(overflow, {output, sizeof(output)}).ok() && sentinel(),
          "bulk ack: wrapped source range rejects without dereference");
    u8 saved[sizeof(BulkMessageAckEntry)];
    copy_bytes(saved, &entries[0], sizeof(saved));
    check(write_bulk_message_ack_body({entries, 1},
                                      {reinterpret_cast<u8*>(entries) + 1, sizeof(BulkMessageAckEntry) - 1})
                      .error == BulkMessageAckWriteError::invalid_input &&
              bytes_equal(saved, &entries[0], sizeof(saved)),
          "bulk ack: partial source overlap is atomic");
    struct AliasedSource {
        ConstSlice<BulkMessageAckEntry> descriptor;
        u8 extra[32];
    } aliased_source{};
    aliased_source.descriptor = {reinterpret_cast<const BulkMessageAckEntry*>(&aliased_source.descriptor),
                                 1};
    check(!write_bulk_message_ack_body(aliased_source.descriptor, {output, sizeof(output)}).ok() &&
              sentinel(),
          "bulk ack: source entries cannot overlap their descriptor");
    struct AliasedDestination {
        Slice<u8> descriptor;
        u8 extra[32];
    } aliased_destination{};
    aliased_destination.descriptor = {output, sizeof(output)};
    check(!write_bulk_message_ack_body(
               {reinterpret_cast<const BulkMessageAckEntry*>(&aliased_destination.descriptor), 1},
               aliased_destination.descriptor)
                  .ok() &&
              sentinel(),
          "bulk ack: source entries cannot overlap output descriptor");
    ConstSlice<BulkMessageAckEntry> source{entries, 1};
    const auto source_saved = source;
    check(write_bulk_message_ack_body(source, {reinterpret_cast<u8*>(&source), sizeof(source)}).error ==
                  BulkMessageAckWriteError::invalid_input &&
              source.data == source_saved.data && source.count == source_saved.count,
          "bulk ack: input descriptor overlap is atomic");
    Slice<u8> destination{};
    destination = {reinterpret_cast<u8*>(&destination), sizeof(destination)};
    const auto destination_saved = destination;
    check(write_bulk_message_ack_body(source, destination).error ==
                  BulkMessageAckWriteError::invalid_input &&
              destination.data == destination_saved.data && destination.count == destination_saved.count,
          "bulk ack: output descriptor overlap is atomic");
}
} // namespace maicord
