#include "../src/app/gateway_subscription_thread_owner.hpp"
#include "maicord/app/member_list_channel_binding.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace thread_subscription_probe {
u8 output[4096];
GatewayGuildSubscription thread(u64 guild, u64 id) noexcept {
    GatewayGuildSubscription result{};
    result.guild_id = guild; result.thread_id = id;
    result.thread_member_lists_present = true; result.omit_channel_ranges = true;
    result.omit_features = true; return result;
}
GatewayGuildSubscription normal(u64 guild, u64 channel) noexcept {
    GatewayGuildSubscription result{};
    result.guild_id = guild; result.channel_id = channel;
    result.range_last = 99; result.typing = result.activities = result.threads = true;
    return result;
}
bool exact(const GatewayGuildSubscription& subscription, Utf8View expected) noexcept {
    const auto result = gateway_write_guild_subscriptions_bulk(subscription, {output, sizeof(output)});
    if (!result.ok() || result.bytes != expected.bytes || !bytes_equal(output, expected.data, result.bytes)) return false;
    // Capacity is preflighted before every write, including the final brace.
    for (auto& byte : output) byte = 0xA5;
    const auto short_result = gateway_write_guild_subscriptions_bulk(subscription, {output, expected.bytes - 1});
    if (short_result.error != GatewayCommandError::output_too_small) return false;
    for (const auto byte : output) if (byte != 0xA5) return false;
    const auto exact_result = gateway_write_guild_subscriptions_bulk(subscription, {output, expected.bytes});
    return exact_result.ok() && exact_result.bytes == expected.bytes && bytes_equal(output, expected.data, expected.bytes) && output[expected.bytes] == 0xA5;
}
bool invalid_unchanged(const GatewayGuildSubscription& subscription) noexcept {
    for (auto& byte : output) byte = 0xA5;
    const auto result = gateway_write_guild_subscriptions_bulk(subscription, {output, sizeof(output)});
    if (result.error != GatewayCommandError::invalid_input) return false;
    for (const auto byte : output) if (byte != 0xA5) return false;
    return true;
}
}
void test_thread_subscriptions(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace thread_subscription_probe;
    check(exact(normal(7, 70), utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"typing\":true,\"activities\":true,\"threads\":true,\"channels\":{\"70\":[[0,99]]}}}}}")),
        "normal member subscription retains exact opcode37 ranges and feature frame");
    check(exact(thread(7, 71), utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"thread_member_lists\":[\"71\"]}}}}")),
        "ordinary thread membership uses exact thread array without channel or parent range fiction");
    check(exact(thread(7, 0), utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"thread_member_lists\":[]}}}}")),
        "thread release writes explicit empty retained interest array");
    auto features = thread(7, 71); features.omit_features = false;
    check(exact(features, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"typing\":false,\"activities\":false,\"threads\":false,\"thread_member_lists\":[\"71\"]}}}}")),
        "thread subscription optional feature flags preserve false boolean widths and exact capacity");
    auto compound = thread(8, 81); compound.clear_thread_guild_id = 7;
    check(exact(compound, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"thread_member_lists\":[]},\"8\":{\"thread_member_lists\":[\"81\"]}}}}")),
        "cross guild thread selection atomically writes old empty and latest thread interest");
    auto compound_normal = normal(8, 80); compound_normal.clear_thread_guild_id = 7; compound_normal.thread_member_lists_present = true;
    check(exact(compound_normal, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"thread_member_lists\":[]},\"8\":{\"typing\":true,\"activities\":true,\"threads\":true,\"channels\":{\"80\":[[0,99]]},\"thread_member_lists\":[]}}}}")),
        "cross guild channel selection clears old thread alongside ordinary channel frame");
    check(exact(thread(~0ull, ~0ull), utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"18446744073709551615\":{\"thread_member_lists\":[\"18446744073709551615\"]}}}}")),
        "thread subscription supports maximum decimal snowflake width");
    auto maximum_ranges = normal(~0ull, ~0ull); maximum_ranges.range_count = 5;
    for (u32 i = 0; i < 5; ++i) maximum_ranges.ranges[i] = {i * 100, i * 100 + 99};
    check(exact(maximum_ranges, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"18446744073709551615\":{\"typing\":true,\"activities\":true,\"threads\":true,\"channels\":{\"18446744073709551615\":[[0,99],[100,199],[200,299],[300,399],[400,499]]}}}}}")),
        "five bounded channel ranges retain exact JSON and capacity checks");
    auto maximum_index = normal(~0ull, ~0ull); maximum_index.range_first = maximum_index.range_last = ~0u;
    check(exact(maximum_index, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"18446744073709551615\":{\"typing\":true,\"activities\":true,\"threads\":true,\"channels\":{\"18446744073709551615\":[[4294967295,4294967295]]}}}}}")),
        "maximum channel range endpoints cannot overflow preflight byte count");
    auto bad = thread(7, 71); bad.thread_member_lists_present = false;
    check(invalid_unchanged(bad), "thread ID requires explicit thread field presence");
    bad = thread(7, 71); bad.channel_id = 70;
    check(invalid_unchanged(bad), "omitted channel ranges cannot carry channel identity");
    bad = thread(7, 71); bad.range_count = 1;
    check(invalid_unchanged(bad), "ordinary thread subscription rejects accidental channel range records");
    bad = thread(7, 71); bad.clear_thread_guild_id = 7;
    check(invalid_unchanged(bad), "duplicate guild keys cannot represent thread clear and replacement");
    bad = thread(7, 0); bad.thread_member_lists_present = false;
    check(invalid_unchanged(bad), "subscription cannot omit every mutable field");
    bad = maximum_ranges; bad.range_count = 6;
    check(invalid_unchanged(bad), "six channel ranges exceed bounded writer admission");
    bad = maximum_ranges; bad.ranges[1].first = 99;
    check(invalid_unchanged(bad), "overlapping channel ranges are rejected before writing");
    bad = maximum_ranges; bad.ranges[0].last = 100;
    check(invalid_unchanged(bad), "explicit channel range cannot exceed 100 positions");
    bad = normal(7, 70); bad.range_last = 1000;
    check(invalid_unchanged(bad), "legacy scalar channel range cannot exceed 1000 positions");
    auto followed = thread(7, 0); followed.thread_member_lists_present = false;
    followed.members_present = true; followed.member_count = 2; followed.members[0] = 20; followed.members[1] = 31;
    check(exact(followed, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"members\":[\"20\",\"31\"]}}}}")),
        "followed users travel as a members-only op 37 frame of string snowflakes");
    followed.member_count = 0;
    check(exact(followed, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"members\":[]}}}}")),
        "an empty members list unsubscribes the guild's followed users");
    auto beside = normal(7, 70); beside.members_present = true; beside.member_count = 1; beside.members[0] = 20;
    check(exact(beside, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"typing\":true,\"activities\":true,\"threads\":true,\"members\":[\"20\"],\"channels\":{\"70\":[[0,99]]}}}}}")),
        "followed users sit between the feature flags and the channel ranges");
    bad = followed; bad.member_count = 1; bad.members[0] = 0;
    check(invalid_unchanged(bad), "a followed user needs an ID");
    bad = followed; bad.member_count = 2; bad.members[0] = bad.members[1] = 20;
    check(invalid_unchanged(bad), "followed users are distinct");
    bad = followed; bad.member_count = kGatewayGuildSubscriptionMaximumMembers + 1;
    check(invalid_unchanged(bad), "at most 50 users are followed");
    bad = followed; bad.members_present = false; bad.member_count = 1; bad.members[0] = 20;
    check(invalid_unchanged(bad), "followed users need the explicit members field");
    bad = followed; bad.members_present = false;
    check(invalid_unchanged(bad), "a members-only frame without members changes nothing");
    followed.member_count = kGatewayGuildSubscriptionMaximumMembers;
    for (u32 i = 0; i < followed.member_count; ++i) followed.members[i] = ~0ull - i;
    check(gateway_write_guild_subscriptions_bulk(followed, {output, sizeof(output)}).ok(),
        "fifty maximum-width followed users fit one command");
    bad = normal(7, 70); bad.range_first = 100; bad.range_last = 99;
    check(invalid_unchanged(bad), "descending channel range rejects without unsigned subtraction overflow");
    bad = thread(0, 71);
    check(invalid_unchanged(bad), "thread subscription needs a guild scope");

    GatewaySubscriptionThreadOwner owner{}; GatewayGuildSubscription frame{};
    check(gateway_subscription_thread_owner_prepare(owner, thread(7, 71), &frame) && !frame.clear_thread_guild_id && !owner.sent_guild_id,
        "untransmitted desired selection does not claim worker thread ownership");
    gateway_subscription_thread_owner_admit(&owner, frame);
    check(owner.active && !owner.sent_guild_id, "admitted inflight thread frame has separate pending owner");
    gateway_subscription_thread_owner_complete(&owner, true);
    check(owner.sent_guild_id == 7 && !owner.active, "completed transport write commits actual thread guild owner");
    check(gateway_subscription_thread_owner_prepare(owner, normal(7, 70), &frame) && !frame.thread_member_lists_present && !frame.clear_thread_guild_id,
        "same guild channel-only delta does not invent an absent thread clear field");
    gateway_subscription_thread_owner_admit(&owner, frame); gateway_subscription_thread_owner_complete(&owner, true);
    check(owner.sent_guild_id == 7, "completed delta without thread field retains existing thread ownership");
    check(gateway_subscription_thread_owner_prepare(owner, thread(8, 81), &frame) && frame.clear_thread_guild_id == 7 &&
        gateway_subscription_thread_owner_prepare(owner, thread(9, 91), &frame) && frame.guild_id == 9 && frame.clear_thread_guild_id == 7,
        "latest desired supersession preserves old completed guild cleanup");
    check(exact(frame, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"thread_member_lists\":[]},\"9\":{\"thread_member_lists\":[\"91\"]}}}}")),
        "latest wins publishes latest guild plus worker retained old clear");
    gateway_subscription_thread_owner_prepare(owner, thread(8, 81), &frame);
    gateway_subscription_thread_owner_admit(&owner, frame);
    const auto latest = thread(9, 91);
    gateway_subscription_thread_owner_complete(&owner, true);
    check(owner.sent_guild_id == 8 && gateway_subscription_thread_owner_prepare(owner, latest, &frame) && frame.clear_thread_guild_id == 8 && frame.guild_id == 9,
        "completion of inflight old selection cannot erase cleanup required by newer desired guild");
    gateway_subscription_thread_owner_admit(&owner, frame);
    gateway_subscription_thread_owner_complete(&owner, false);
    check(owner.sent_guild_id == 8 && !owner.active && gateway_subscription_thread_owner_prepare(owner, latest, &frame) && frame.clear_thread_guild_id == 8,
        "failed send commits no new thread owner and retry retains old cleanup");
    gateway_subscription_thread_owner_admit(&owner, frame);
    gateway_subscription_thread_owner_reset_connection(&owner);
    gateway_subscription_thread_owner_ready(&owner, false);
    check(owner.sent_guild_id == 8 && !owner.active && gateway_subscription_thread_owner_prepare(owner, latest, &frame) && frame.clear_thread_guild_id == 8,
        "connection reset drops active send while successful resume retains completed thread owner");
    gateway_subscription_thread_owner_ready(&owner, true);
    check(!owner.sent_guild_id && gateway_subscription_thread_owner_prepare(owner, latest, &frame) && !frame.clear_thread_guild_id,
        "fresh READY clears completed old-session thread ownership before republishing desired scope");
    gateway_subscription_thread_owner_admit(&owner, frame); gateway_subscription_thread_owner_complete(&owner, true);
    auto channel = normal(10, 100);
    check(gateway_subscription_thread_owner_prepare(owner, channel, &frame) && frame.clear_thread_guild_id == 9 && frame.thread_member_lists_present && !frame.thread_id,
        "channel-only latest selection acquires explicit old thread cleanup from worker owner");
    gateway_subscription_thread_owner_admit(&owner, frame); gateway_subscription_thread_owner_complete(&owner, true);
    check(!owner.sent_guild_id, "completed compound channel selection releases actual thread interest");
    gateway_subscription_thread_owner_prepare(owner, thread(7, 71), &frame);
    gateway_subscription_thread_owner_admit(&owner, frame); gateway_subscription_thread_owner_complete(&owner, true);
    check(gateway_subscription_thread_owner_prepare(owner, thread(7, 0), &frame) && !frame.clear_thread_guild_id,
        "same guild explicit release clears current thread field without duplicate guild key");
    gateway_subscription_thread_owner_admit(&owner, frame); gateway_subscription_thread_owner_complete(&owner, true);
    check(!owner.sent_guild_id, "same guild empty thread array releases owner only after completed transport write");
    bad = thread(7, 71); bad.clear_thread_guild_id = 8;
    frame = thread(11, 111);
    check(!gateway_subscription_thread_owner_prepare(owner, bad, &frame) && frame.guild_id == 11,
        "caller supplied cleanup cannot forge worker ownership or alter prepared output");
    // The official client writes its guild's remembered channels (up to five,
    // most recent first) in every frame, each with at least one range.
    auto remembered = normal(7, 80);
    remembered.channel_count = 2;
    remembered.channels[0] = {70, {{0, 99}, {100, 199}}, 2};
    remembered.channels[1] = {60, {{0, 99}}, 1};
    check(exact(remembered, utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"typing\":true,\"activities\":true,\"threads\":true,\"channels\":{\"80\":[[0,99]],\"70\":[[0,99],[100,199]],\"60\":[[0,99]]}}}}}")),
        "remembered channels follow the current one with their own ranges");
    bad = remembered; bad.channels[1].range_count = 0;
    check(invalid_unchanged(bad), "a remembered channel never writes an empty range array");
    bad = remembered; bad.channels[1].channel_id = 70;
    check(invalid_unchanged(bad), "a remembered channel is written once");
    bad = remembered; bad.channels[0].channel_id = 80;
    check(invalid_unchanged(bad), "the current channel is not repeated among remembered ones");
    bad = remembered; bad.channel_count = 5;
    check(invalid_unchanged(bad), "at most five channels per guild");

    MemberListChannelMemory memory{};
    GatewayGuildSubscription memory_frame{};
    const GatewayMemberSubscriptionRange top[1]{{0, 99}};
    const GatewayMemberSubscriptionRange scrolled[2]{{0, 99}, {100, 199}};
    const auto no_empty_ranges = [&]() noexcept {
        if (!memory_frame.range_count) return false;
        for (u32 i = 0; i < memory_frame.channel_count; ++i)
            if (!memory_frame.channels[i].range_count) return false;
        return gateway_write_guild_subscriptions_bulk(memory_frame, {output, sizeof(output)}).ok();
    };
    check(member_list_channel_memory_subscribe(&memory, 7, 70, top, 1, true, &memory_frame) && memory_frame.channel_id == 70 &&
              !memory_frame.channel_count && !memory_frame.omit_features && no_empty_ranges(),
        "a guild's first channel writes its ranges with the guild's features");
    check(!member_list_channel_memory_subscribe(&memory, 7, 70, top, 1, true, &memory_frame),
        "the same channel and ranges again write nothing, even before its list arrived");
    check(member_list_channel_memory_subscribe(&memory, 7, 80, top, 1, true, &memory_frame) && memory_frame.channel_id == 80 &&
              memory_frame.channel_count == 1 && memory_frame.channels[0].channel_id == 70 && memory_frame.omit_features &&
              no_empty_ranges(),
        "a new channel writes every remembered channel, the newest first, without unsubscribing any");
    check(!member_list_channel_memory_subscribe(&memory, 7, 70, top, 1, false, &memory_frame),
        "switching back to a remembered channel writes nothing; its list kept its rows");
    check(member_list_channel_memory_subscribe(&memory, 7, 70, scrolled, 2, false, &memory_frame) && memory_frame.channel_id == 70 &&
              memory_frame.range_count == 2 && memory_frame.channel_count == 1 && memory_frame.channels[0].channel_id == 80 &&
              no_empty_ranges(),
        "changed ranges write the channel again as the newest");
    bool written = true;
    for (u64 next = 81; next < 85; ++next)
        written &= member_list_channel_memory_subscribe(&memory, 7, next, top, 1, false, &memory_frame);
    check(written, "each new channel writes a frame");
    check(memory_frame.channel_id == 84 && memory_frame.channel_count == 4 && memory_frame.channels[3].channel_id == 70 && no_empty_ranges(),
        "a sixth channel leaves the oldest out of the frame, as the official five-channel memory does");
    check(member_list_channel_memory_subscribe(&memory, 7, 80, top, 1, false, &memory_frame) && memory_frame.channel_id == 80,
        "a channel the memory let go is written as new");
    check(member_list_channel_memory_subscribe(&memory, 7, 82, top, 1, true, &memory_frame) && memory_frame.channel_id == 82 &&
              !member_list_channel_memory_subscribe(&memory, 7, 82, top, 1, true, &memory_frame),
        "a remembered channel whose list MaiCord lost is written once more, then waits");
    member_list_channel_memory_connection(&memory, false, 7);
    check(member_list_channel_memory_subscribe(&memory, 7, 82, top, 1, false, &memory_frame) && !memory_frame.omit_features &&
              memory_frame.channel_count == 4 && no_empty_ranges() &&
              !member_list_channel_memory_subscribe(&memory, 7, 82, top, 1, false, &memory_frame),
        "a resumed connection rewrites the guild's full state once");
    check(member_list_channel_memory_subscribe(&memory, 9, 90, top, 1, false, &memory_frame),
        "another guild writes its own first frame");
    member_list_channel_memory_connection(&memory, true, 9);
    check(member_list_channel_memory_subscribe(&memory, 9, 90, top, 1, true, &memory_frame) && memory_frame.channel_count == 0 &&
              member_list_channel_memory_subscribe(&memory, 7, 82, top, 1, true, &memory_frame) && !memory_frame.channel_count &&
              !memory_frame.omit_features,
        "a fresh session keeps only the guild on screen");
}
} // namespace maicord
