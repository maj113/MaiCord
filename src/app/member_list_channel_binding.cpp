#include "maicord/app/member_list_channel_binding.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/model/chat_state.hpp"

namespace maicord {
namespace {

struct VisibilityOverwrite {
    char text[26]; // "allow:" plus a 20-digit ID
    u8 bytes;
};

struct ListHash {
    u32 hash = 0;
    u32 word = 0;
    u32 bytes = 0;
    u32 tail = 0;

    static u32 rotate(u32 value, u32 count) noexcept {
        return (value << count) | (value >> (32u - count));
    }
    static u32 mix(u32 value) noexcept {
        return rotate(value * 0xcc9e2d51u, 15) * 0x1b873593u;
    }
    void append(u8 byte) noexcept {
        word |= static_cast<u32>(byte) << (tail * 8);
        ++bytes;
        if (++tail == 4) {
            hash = rotate(hash ^ mix(word), 13) * 5u + 0xe6546b64u;
            word = 0;
            tail = 0;
        }
    }
    u32 finish() const noexcept {
        u32 value = hash ^ (tail ? mix(word) : 0) ^ bytes;
        value ^= value >> 16;
        value *= 0x85ebca6bu;
        value ^= value >> 13;
        value *= 0xc2b2ae35u;
        return value ^ (value >> 16);
    }
};

// Doubling retry delay after `failures` (at least one) consecutive failures.
u32 retry_delay(u32 failures) noexcept {
    return minimum(kMemberListSubscriptionRetryMaximumMs,
                   kMemberListSubscriptionRetryMinimumMs << minimum(failures - 1u, 4u));
}

} // namespace

u64 member_list_channel_id(const ChatStateStore& state, u64 channel_id) noexcept {
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    if (!channel) return 0;
    // Ordinary threads have an independent membership snapshot. Their parent
    // permission identity must never admit unrelated guild members here.
    if (channel->type == 11 || channel->type == 12) return 0;
    if (channel->member_list_id) return channel->member_list_id;
    // Announcement threads subscribe to their parent's member list.
    if (channel->type == 10) {
        channel = chat_state_store_find_channel(state, channel->parent_id);
        if (!channel) return 0;
        if (channel->member_list_id) return channel->member_list_id;
    }
    if (channel->overwrite_offset > state.overwrite_count ||
        channel->overwrite_count > state.overwrite_count - channel->overwrite_offset ||
        (channel->overwrite_count && !state.overwrites)) return 0;
    // Everyone can view the channel: the @everyone role's own VIEW_CHANNEL bit
    // (an ADMINISTRATOR bit is not consulted) and no overwrite denying it.
    const auto* everyone = chat_state_store_find_role(state, channel->guild_id, channel->guild_id);
    bool public_list = everyone && (everyone->permissions & kPermissionViewChannel) != 0;
    for (u32 i = 0; i < channel->overwrite_count; ++i) {
        if (state.overwrites[channel->overwrite_offset + i].deny & kPermissionViewChannel)
            public_list = false;
    }
    if (public_list) return kMemberListEveryone;
    // The member list id sorts the textual allow:/deny: entries lexicographically before
    // MurmurHash3 x86-32 (seed zero). Unrelated permission bits are excluded.
    VisibilityOverwrite entries[512];
    u32 count = 0;
    for (u32 i = 0; i < channel->overwrite_count; ++i) {
        const auto& overwrite = state.overwrites[channel->overwrite_offset + i];
        const bool allow = (overwrite.allow & kPermissionViewChannel) != 0;
        if (!allow && !(overwrite.deny & kPermissionViewChannel)) continue;
        if (count == 512) return 0;
        auto& entry = entries[count++];
        const char* prefix = allow ? "allow:" : "deny:";
        entry.bytes = allow ? 6 : 5;
        for (u32 j = 0; j < entry.bytes; ++j) entry.text[j] = prefix[j];
        entry.bytes = static_cast<u8>(entry.bytes + decimal_write(entry.text + entry.bytes, overwrite.id));
    }
    heap_sort(entries, count, [](const VisibilityOverwrite& a, const VisibilityOverwrite& b) noexcept {
        for (u32 i = 0; i < minimum(a.bytes, b.bytes); ++i)
            if (a.text[i] != b.text[i]) return a.text[i] < b.text[i];
        return a.bytes < b.bytes;
    });
    ListHash hash;
    for (u32 i = 0; i < count; ++i) {
        if (i) hash.append(',');
        for (u32 j = 0; j < entries[i].bytes; ++j) hash.append(static_cast<u8>(entries[i].text[j]));
    }
    const u32 token = hash.finish();
    return token ? token : kMemberListZeroHash;
}

u64 member_list_channel_cached_id(MemberListIdentityCache* cache,
                                 const ChatStateStore& state, u64 channel_id) noexcept {
    if (!cache) return 0;
    if (cache->channel_id != channel_id || cache->channel_mutations != state.channel_mutations ||
        cache->permission_mutations != state.permission_mutations) {
        cache->channel_id = channel_id;
        cache->channel_mutations = state.channel_mutations;
        cache->permission_mutations = state.permission_mutations;
        cache->list_id = member_list_channel_id(state, channel_id);
    }
    return cache->list_id;
}

bool member_list_subscription_select(MemberListSubscriptionRetry* retry, u64 guild_id, u64 channel_id,
                                     u64 now_ms) noexcept {
    if (!retry || now_ms == 0 || ((guild_id == 0) != (channel_id == 0))) {
        return false;
    }
    reset_value<MemberListSubscriptionRetry>(retry);
    if (channel_id != 0) {
        retry->guild_id = guild_id;
        retry->channel_id = channel_id;
        retry->retry_at_ms = now_ms;
        retry->range_count = 1;
        retry->ranges[0] = {0, kMemberListSubscriptionPageRows - 1};
    }
    return true;
}

bool member_list_subscription_due(const MemberListSubscriptionRetry& retry, u64 now_ms) noexcept {
    return retry.guild_id != 0 && retry.channel_id != 0 && retry.retry_at_ms != 0 &&
           now_ms >= retry.retry_at_ms;
}

bool member_list_subscription_fail(MemberListSubscriptionRetry* retry, u64 now_ms) noexcept {
    if (!retry || retry->guild_id == 0 || retry->channel_id == 0 || now_ms == 0) {
        return false;
    }
    saturating_increment(retry->failures);
    retry->retry_at_ms = deadline_after(now_ms, retry_delay(retry->failures));
    return true;
}

void member_list_subscription_complete(MemberListSubscriptionRetry* retry) noexcept {
    if (retry) {
        reset_value<MemberListSubscriptionRetry>(retry);
    }
}

namespace {
bool same_ranges(const GatewayMemberSubscriptionRange* a, u32 a_count,
                 const GatewayMemberSubscriptionRange* b, u32 b_count) noexcept {
    if (a_count != b_count) return false;
    for (u32 i = 0; i < a_count; ++i)
        if (a[i].first != b[i].first || a[i].last != b[i].last) return false;
    return true;
}
bool retain_page_window(const GatewayMemberSubscriptionRange* requested, u32 requested_count,
                        const GatewayMemberSubscriptionRange* current, u32 current_count) noexcept {
    if (!requested_count || !current_count) return false;
    const u32 requested_low = requested_count > 1 ? requested[1].first : 0;
    const u32 current_low = current_count > 1 ? current[1].first : 0;
    const u32 requested_high = requested[requested_count - 1].last;
    const u32 current_high = current[current_count - 1].last;
    if (maximum(requested_low, current_low) - minimum(requested_low, current_low) > kMemberListSubscriptionPageRows ||
        maximum(requested_high, current_high) - minimum(requested_high, current_high) > kMemberListSubscriptionPageRows)
        return false;
    for (u32 i = 0; i < requested_count; ++i) {
        bool found = false;
        for (u32 j = 0; j < current_count; ++j)
            if (requested[i].first >= current[j].first && requested[i].last <= current[j].last) found = true;
        if (!found) return false;
    }
    return true;
}
}

bool member_list_subscription_viewport(MemberListSubscriptionRetry* retry,
    u64 guild_id, u64 channel_id, u32 logical_count, u32 first_visible,
    u32 visible_rows, bool visible, u64 now_ms) noexcept {
    if (!retry || !now_ms) return false;
    if (!visible || !guild_id || !channel_id || !visible_rows) {
        member_list_subscription_complete(retry);
        return true;
    }
    const bool selected = retry->guild_id != guild_id || retry->channel_id != channel_id;
    if (selected && !member_list_subscription_select(retry, guild_id, channel_id, now_ms)) return false;
    if (logical_count) first_visible = minimum(first_visible, logical_count - 1);
    const u32 overscan = visible_rows / 2 + visible_rows % 2;
    const u32 first = first_visible > overscan ? first_visible - overscan : 0;
    u64 last = static_cast<u64>(first_visible) + visible_rows + overscan - 1;
    if (logical_count) last = minimum(last, static_cast<u64>(logical_count - 1));
    last = minimum(last, static_cast<u64>(~0u));
    u32 first_page = first / kMemberListSubscriptionPageRows;
    const u32 last_page = static_cast<u32>(last) / kMemberListSubscriptionPageRows;
    GatewayMemberSubscriptionRange ranges[kGatewayMemberSubscriptionMaximumRanges]{};
    u32 count = 1;
    ranges[0] = {0, kMemberListSubscriptionPageRows - 1};
    first_page = maximum(first_page, 1u);
    const u32 pages = last_page >= first_page ? last_page - first_page + 1 : 0;
    retry->coverage_limited = pages >= kGatewayMemberSubscriptionMaximumRanges;
    if (retry->coverage_limited) {
        retry->retry_at_ms = 0;
        return false;
    }
    for (u32 page = first_page; page <= last_page && count < kGatewayMemberSubscriptionMaximumRanges; ++page) {
        const u32 low = page * kMemberListSubscriptionPageRows;
        ranges[count++] = {low, static_cast<u32>(minimum(static_cast<u64>(low) + kMemberListSubscriptionPageRows - 1, static_cast<u64>(~0u)))};
    }
    // Retain at most one adjacent extra page until overscan leaves the current
    // envelope. Small reversals at a page edge therefore do not churn requests.
    if (same_ranges(ranges, count, retry->ranges, retry->range_count) ||
        (!selected && retain_page_window(ranges, count, retry->ranges, retry->range_count))) {
        if (!retry->sent_range_count && !retry->retry_at_ms) retry->retry_at_ms = now_ms;
        return true;
    }
    for (u32 i = 0; i < count; ++i) retry->ranges[i] = ranges[i];
    retry->range_count = count;
    retry->failures = 0;
    retry->retry_at_ms = same_ranges(ranges, count, retry->sent_ranges, retry->sent_range_count) ? 0 :
        (selected || !retry->sent_range_count ? now_ms : deadline_after(now_ms, kMemberListSubscriptionDebounceMs));
    return true;
}

void member_list_subscription_sent(MemberListSubscriptionRetry* retry) noexcept {
    if (!retry) return;
    retry->sent_range_count = retry->range_count;
    for (u32 i = 0; i < retry->range_count; ++i) retry->sent_ranges[i] = retry->ranges[i];
    retry->retry_at_ms = 0;
    retry->failures = 0;
}

void member_list_subscription_refresh(MemberListSubscriptionRetry* retry, u64 now_ms) noexcept {
    if (!retry || !now_ms || !retry->guild_id || !retry->channel_id) return;
    retry->sent_range_count = 0;
    retry->retry_at_ms = now_ms;
    retry->failures = 0;
}

bool member_list_channel_memory_subscribe(MemberListChannelMemory* memory, u64 guild_id, u64 channel_id,
                                          const GatewayMemberSubscriptionRange* ranges, u32 range_count,
                                          bool list_lost, GatewayGuildSubscription* frame) noexcept {
    if (!memory || !frame || !guild_id || !channel_id || !ranges || !range_count ||
        range_count > kGatewayMemberSubscriptionMaximumRanges)
        return false;
    MemberListRememberedGuild* guild = nullptr;
    MemberListRememberedGuild* victim = &memory->guilds[0];
    for (auto& candidate : memory->guilds) {
        if (candidate.guild_id == guild_id) {
            guild = &candidate;
            break;
        }
        if (!candidate.guild_id ? victim->guild_id != 0 : victim->guild_id && candidate.used < victim->used)
            victim = &candidate;
    }
    const bool new_guild = !guild;
    if (new_guild) {
        guild = victim;
        reset_value(guild);
        guild->guild_id = guild_id;
    }
    saturating_increment(memory->clock);
    guild->used = memory->clock;
    u32 at = guild->count;
    for (u32 i = 0; i < guild->count; ++i)
        if (guild->channels[i].channel_id == channel_id)
            at = i;
    const bool remembered = at < guild->count;
    const bool equal = remembered && same_ranges(guild->channels[at].ranges,
                                                 guild->channels[at].range_count, ranges, range_count);
    if (remembered && !list_lost)
        guild->lost[at] = false;
    const bool resend_lost = remembered && list_lost && !guild->lost[at];
    if (equal && !resend_lost && !memory->flush)
        return false;
    // lru.set: the channel becomes the most recent; a sixth drops the oldest.
    GatewayChannelSubscription entry{channel_id, {}, range_count};
    for (u32 i = 0; i < range_count; ++i)
        entry.ranges[i] = ranges[i];
    const bool was_lost = remembered ? guild->lost[at] : false;
    const u32 shift = remembered ? at : minimum(guild->count, kGatewayGuildSubscriptionMaximumChannels - 1);
    for (u32 i = shift; i > 0; --i) {
        guild->channels[i] = guild->channels[i - 1];
        guild->lost[i] = guild->lost[i - 1];
    }
    guild->channels[0] = entry;
    guild->lost[0] = list_lost && (remembered ? resend_lost || was_lost : true);
    if (!remembered)
        guild->count = minimum(guild->count + 1, kGatewayGuildSubscriptionMaximumChannels);
    reset_value(frame);
    frame->guild_id = guild_id;
    frame->channel_id = channel_id;
    frame->range_count = range_count;
    for (u32 i = 0; i < range_count; ++i)
        frame->ranges[i] = ranges[i];
    frame->typing = frame->activities = frame->threads = true;
    frame->omit_features = !new_guild && !memory->flush;
    frame->channel_count = guild->count - 1;
    for (u32 i = 1; i < guild->count; ++i)
        frame->channels[i - 1] = guild->channels[i];
    memory->flush = false;
    return true;
}

void member_list_channel_memory_connection(MemberListChannelMemory* memory, bool fresh,
                                           u64 shown_guild_id) noexcept {
    if (!memory)
        return;
    if (fresh)
        for (auto& guild : memory->guilds)
            if (guild.guild_id != shown_guild_id || !shown_guild_id)
                reset_value(&guild);
    memory->flush = true;
}

bool thread_member_list_subscription_select(ThreadMemberListSubscriptionRetry* retry,
    u64 guild_id, u64 thread_id, bool visible, u64 now_ms) noexcept {
    if (!retry || !now_ms || (visible && ((guild_id == 0) != (thread_id == 0)))) return false;
    if (!visible) guild_id = thread_id = 0;
    if (retry->guild_id == guild_id && retry->thread_id == thread_id) return true;
    retry->guild_id = guild_id;
    retry->thread_id = thread_id;
    retry->failures = 0;
    retry->retry_at_ms = retry->sent_guild_id != guild_id || retry->sent_thread_id != thread_id ? now_ms : 0;
    return true;
}

bool thread_member_list_subscription_command(const ThreadMemberListSubscriptionRetry& retry,
    u64 now_ms, ThreadMemberListSubscriptionCommand* command) noexcept {
    if (!command || !retry.retry_at_ms || now_ms < retry.retry_at_ms) return false;
    if (retry.sent_guild_id && retry.sent_guild_id != retry.guild_id)
        *command = {retry.sent_guild_id, 0};
    else if (retry.guild_id && retry.thread_id)
        *command = {retry.guild_id, retry.thread_id};
    else return false;
    return true;
}

bool thread_member_list_subscription_fail(ThreadMemberListSubscriptionRetry* retry, u64 now_ms) noexcept {
    ThreadMemberListSubscriptionCommand command{};
    if (!retry || !now_ms || !thread_member_list_subscription_command(*retry, now_ms, &command)) return false;
    saturating_increment(retry->failures);
    retry->retry_at_ms = deadline_after(now_ms, retry_delay(retry->failures));
    return true;
}

void thread_member_list_subscription_sent(ThreadMemberListSubscriptionRetry* retry,
    const ThreadMemberListSubscriptionCommand& command, u64 now_ms) noexcept {
    if (!retry || !command.guild_id || !now_ms) return;
    // Admission is synchronous today, but the command identity remains explicit
    // so an older accepted request cannot erase a newer desired selection.
    if (!command.thread_id) {
        if (retry->sent_guild_id == command.guild_id) {
            retry->sent_guild_id = 0;
            retry->sent_thread_id = 0;
        }
    } else {
        retry->sent_guild_id = command.guild_id;
        retry->sent_thread_id = command.thread_id;
    }
    retry->failures = 0;
    retry->retry_at_ms = retry->sent_guild_id != retry->guild_id ||
        retry->sent_thread_id != retry->thread_id ? now_ms : 0;
}

void thread_member_list_subscription_refresh(ThreadMemberListSubscriptionRetry* retry, u64 now_ms) noexcept {
    if (retry && now_ms && (retry->guild_id || retry->sent_guild_id)) {
        retry->retry_at_ms = now_ms;
        retry->failures = 0;
    }
}
void thread_member_list_subscription_complete(ThreadMemberListSubscriptionRetry* retry) noexcept {
    if (retry) reset_value(retry);
}

} // namespace maicord
