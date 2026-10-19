#include "maicord/model/read_state.hpp"

namespace maicord {
namespace read_model_probe {
struct Backing {
    u32 calls, backed;
    bool fail;
};
bool reserve(void* context, u32 required) noexcept {
    auto& backing = *static_cast<Backing*>(context);
    ++backing.calls;
    if (backing.fail) return false;
    backing.backed = maximum(backing.backed, required);
    return true;
}
struct Fixture {
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[8];
    u16 guild_order[1], channel_order[8];
    u8 names[1024];
    ReadStateStore reads;
    ReadStateEntry pending[8];
    RoleRecord roles[1];
    PermissionOverwriteRecord overwrites[1];
    Backing backing;
};
Fixture fixture{};
bool initialize() noexcept {
    auto& f = fixture;
    f.backing = {};
    return chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1},
        {f.channels, 8}, {f.channel_order, 8}, {f.names, sizeof(f.names)}) &&
        chat_state_store_upsert_guild(&f.state, {7, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        read_state_initialize(&f.reads, {f.pending, 8}) &&
        read_state_set_reserve(&f.reads, reserve, &f.backing) &&
        read_state_bind(&f.state, &f.reads);
}
bool channel(u64 id, u64 last, u8 type = 0) noexcept {
    ChannelInput input{};
    input.id = id; input.guild_id = channel_is_private(type) ? 0 : 7;
    input.type = type; input.last_message_id = last; input.last_message_known = true;
    input.name = utf8_literal("Channel"); input.name_known = true;
    return chat_state_store_upsert_channel(&fixture.state, input).ok();
}
// A thread of channel 80; join_seconds is the member's join time.
bool thread(u64 id, u64 last, bool joined, bool archived = false, u32 join_seconds = 0) noexcept {
    ChannelInput input{};
    input.id = id; input.guild_id = 7; input.parent_id = 80; input.type = 11;
    input.last_message_id = last; input.last_message_known = true;
    input.name = utf8_literal("Thread"); input.name_known = true;
    input.thread_self_joined_known = input.thread_archived_known = input.thread_active_known = true;
    input.thread_self_joined = joined; input.thread_archived = archived; input.thread_listed_active = !archived;
    input.thread_join_seconds = joined ? join_seconds : 0;
    return chat_state_store_upsert_channel(&fixture.state, input).ok();
}
// A message ID at a Unix time in milliseconds.
constexpr u64 snowflake(u64 unix_ms) noexcept { return (unix_ms - 1'420'070'400'000ull) << 22; }
ReadStateEntry entry(u64 id, u64 cursor, u32 mentions) noexcept {
    return {id, cursor, mentions, 70'000, 0x80000005u,
        read_state_cursor | read_state_mentions | read_state_last_viewed | read_state_flags, {}};
}
const ChannelRecord* find(u64 id) noexcept {
    return chat_state_store_find_channel(fixture.state, id);
}
}

void test_read_state_model(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace read_model_probe;
    auto& f = fixture;
    const bool initialized = initialize();
    check(initialized && !f.backing.calls && !f.reads.backed_count && !f.reads.count,
        "read model: initialization reserves no unmatched entry backing");
    if (!initialized) return;
    check(channel(80, 100) && find(80)->last_read_message_id == 80 && !find(80)->read_state_known &&
        f.guild.unread_channels == 1, "read model: no read snapshot uses channel baseline instead of assuming latest read");
    check(read_state_upsert(&f.state, entry(80, 100, 0), 10) && !f.backing.calls &&
        !f.reads.count && !f.guild.unread_channels && find(80)->read_state_known &&
        find(80)->read_last_viewed == 70'000 && find(80)->read_flags == 0x80000005u,
        "read model: resident read metadata lives only in channel and preserves full u32 values");
    check(read_state_upsert(&f.state, entry(81, 110, 2), 11) && f.reads.count == 1 && f.backing.calls == 1,
        "read model: read entry arriving before channel lazily backs one unmatched slot");
    check(channel(81, 120) && !f.reads.count && find(81)->read_state_known && find(81)->last_read_message_id == 110 &&
        find(81)->mention_count == 2 && f.guild.unread_channels == 1 && f.guild.mention_count == 2,
        "read model: channel insertion consumes authoritative cursor before aggregate accounting");
    check(read_state_apply_ack(&f.state, 81, 130, false, 4, true, 12) &&
        !chat_state_store_channel_unread(f.state, *find(81)) && f.guild.unread_channels == 0 &&
        f.guild.mention_count == 4 && find(81)->mention_count == 4 && find(81)->read_last_viewed == 70'000 &&
        find(81)->read_flags == 0x80000005u,
        "read model: mention badge is independent of white unread state and ACK preserves omitted metadata");
    check(chat_state_store_select_channel(&f.state, 81).ok() &&
        read_state_apply_ack(&f.state, 81, 0, true, 3, true, 13) && find(81)->last_read_message_id == 0 &&
        find(81)->read_auto_ack_suppressed && f.guild.unread_channels == 1 && f.guild.mention_count == 3,
        "read model: manual backward zero cursor restores unread and suppresses selected auto ACK");
    check(read_state_apply_ack(&f.state, 81, 90, true, 2, false, 14) && find(81)->mention_count == 2 &&
        find(81)->read_mentions_incomplete, "read model: partial cached manual mention count stays explicitly incomplete");
    check(chat_state_store_select_channel(&f.state, 80).ok() && !find(81)->read_auto_ack_suppressed,
        "read model: leaving selection releases manual auto-ACK suppression");
    check(read_state_mark_local(&f.state, 81, 120, 90'000, 1) && !find(81)->mention_count &&
        !find(81)->read_mentions_incomplete && !find(81)->read_auto_ack_suppressed && !f.guild.unread_channels,
        "read model: explicit local ACK clears counters and updates complete metadata");
    check(read_state_apply_ack(&f.state, 81, 90, false, 1, true, 1) &&
        f.reads.version == 1 && find(81)->last_read_message_id == 90 && f.guild.unread_channels == 1 && f.guild.mention_count == 1,
        "read model: observed automatic older cursor and version are accepted without invented monotonic rejection");
    f.state.self_user_id = 1;
    check(chat_state_store_note_message(&f.state, 81, 140, 1, false, false) && find(81)->last_read_message_id == 140 &&
        !find(81)->mention_count && !f.guild.unread_channels,
        "read model: own-author message advances read cursor even while view is inactive");
    check(chat_state_store_note_message(&f.state, 81, 150, 2, true, false) && find(81)->mention_count == 1 &&
        f.guild.unread_channels == 1 && f.guild.mention_count == 1,
        "read model: unviewed other-author mention updates independent unread and badge totals");

    auto invalid = entry(81, 160, 0); invalid.reserved[0] = 1;
    const u64 previous_cursor = find(81)->last_read_message_id;
    const u32 previous_revision = f.state.channel_mutations;
    check(!read_state_upsert(&f.state, invalid, 99) && find(81)->last_read_message_id == previous_cursor &&
        f.state.channel_mutations == previous_revision, "read model: malformed normalized record cannot partially alter resident reads");
    check(read_state_upsert(&f.state, entry(82, 0, 2), 15) &&
        read_state_apply_ack(&f.state, 82, 75, false, 0, true, 16) &&
        read_state_pending(f.reads, 82)->message_id == 75 &&
        read_state_pending(f.reads, 82)->last_viewed == 70'000 &&
        read_state_pending(f.reads, 82)->flags == 0x80000005u,
        "read model: unmatched ACK replaces cursor and counts while preserving omitted metadata");
    const u32 before_partial = f.reads.count;
    f.backing.fail = true;
    check(!read_state_prepare_ready(&f.state, true, 17) && f.reads.count == before_partial &&
        !f.reads.preparing_ready && find(81)->last_read_message_id == previous_cursor,
        "read model: failed partial READY carry reserve preserves all resident read entries");
    f.backing.fail = false;
    check(read_state_prepare_ready(&f.state, true, 18), "read model: partial READY carry preflights backing");
    chat_state_store_clear_guilds(&f.state);
    read_state_finish_ready(&f.state);
    check(!f.state.channel_count && f.reads.count == 3 && read_state_pending(f.reads, 81) &&
        read_state_pending(f.reads, 81)->mention_count == 1,
        "read model: partial READY preserves omitted resident reads only after their channels become unmatched");
    check(chat_state_store_upsert_guild(&f.state, {7, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
        channel(81, 150) && f.reads.count == 2 && find(81)->last_read_message_id == 140 && find(81)->mention_count == 1 &&
        f.guild.unread_channels == 1 && f.guild.mention_count == 1,
        "read model: rehydration consumes partial carry without duplicating a resident read cache");
    check(read_state_prepare_ready(&f.state, false, 19) && !f.reads.count,
        "read model: full fresh READY replaces unmatched read snapshot");
    chat_state_store_clear_guilds(&f.state); read_state_finish_ready(&f.state);
    check(!f.reads.count && f.reads.version == 19 && !f.reads.preparing_ready,
        "read model: full READY withdraws prior resident reads instead of retaining a second cache");

    check(initialize() && channel(80, 100) &&
        read_state_upsert(&f.state, entry(80, 100, 3), 1),
        "read model: caught-up history fixture starts with independent mentions and read metadata");
    const u32 history_activity = f.state.activity_mutations;
    check(chat_state_store_note_history_latest(&f.state, 80, 120) &&
        find(80)->last_message_id == 120 && find(80)->last_read_message_id == 100 &&
        find(80)->mention_count == 3 && f.guild.mention_count == 3 && f.guild.unread_channels == 1 &&
        chat_state_store_channel_unread(f.state, *find(80)) && f.state.activity_mutations == history_activity,
        "read model: recovered history advances latest and white unread without replaying mentions or CREATE activity");
    const u32 history_revision = f.state.channel_mutations;
    check(chat_state_store_note_history_latest(&f.state, 80, 110) &&
        chat_state_store_note_history_latest(&f.state, 80, 120) &&
        find(80)->last_message_id == 120 && f.state.channel_mutations == history_revision &&
        find(80)->last_read_message_id == 100 && find(80)->mention_count == 3,
        "read model: older or identical history pages cannot decrease latest or create redundant mutations");
    check(chat_state_store_select_channel(&f.state, 80).ok() &&
        read_state_apply_ack(&f.state, 80, 100, true, 3, true, 2) &&
        chat_state_store_note_history_latest(&f.state, 80, 140) &&
        find(80)->read_auto_ack_suppressed && find(80)->last_read_message_id == 100 &&
        find(80)->mention_count == 3 && f.guild.unread_channels == 1 && f.guild.mention_count == 3,
        "read model: validated newer history preserves selected manual unread suppression and counts");
    check(!chat_state_store_note_history_latest(&f.state, 999, 160) &&
        !chat_state_store_note_history_latest(&f.state, 80, 0) && find(80)->last_message_id == 140,
        "read model: missing scope or invalid history cursor cannot alter latest metadata");

    check(initialize() && channel(80, 150) && channel(81, 200) &&
        read_state_upsert(&f.state, entry(80, 150, 0), 42) &&
        read_state_upsert(&f.state, entry(81, 200, 2), 42) &&
        chat_state_store_select_channel(&f.state, 80).ok(),
        "read model: local manual boundary fixture retains complete channel metadata");
    check(read_state_mark_unread_local(&f.state, 80, 125, 3, false) &&
        find(80)->last_read_message_id == 125 && find(80)->read_auto_ack_suppressed &&
        find(80)->read_mentions_incomplete && find(80)->mention_count == 3 &&
        find(80)->read_flags == 0x80000005u && find(80)->read_last_viewed == 70'000 &&
        f.guild.unread_channels == 1 && f.guild.mention_count == 5 && f.reads.version == 42 && !f.reads.count,
        "read model: selected local manual cursor adjusts guild aggregates and retains incomplete estimate without metadata or version loss");
    const u32 manual_revision = f.state.channel_mutations;
    check(read_state_mark_unread_local(&f.state, 80, 125, 3, false) &&
        f.state.channel_mutations == manual_revision && f.guild.unread_channels == 1 && f.guild.mention_count == 5,
        "read model: repeating identical local manual state does not duplicate aggregates or mutations");
    check(read_state_mark_unread_local(&f.state, 81, 0, 1, true) &&
        !find(81)->read_auto_ack_suppressed && !find(81)->read_mentions_incomplete &&
        find(81)->last_read_message_id == 0 && f.guild.unread_channels == 2 && f.guild.mention_count == 4,
        "read model: unselected local manual zero boundary restores unread while retaining a complete known mention count");
    check(!read_state_mark_unread_local(&f.state, 999, 0, 0, false) && !f.reads.count &&
        f.guild.unread_channels == 2 && f.guild.mention_count == 4,
        "read model: local manual action cannot create unmatched state for a missing channel");
    check(read_state_mark_local(&f.state, 80, 150, 90'000, 1) &&
        !find(80)->read_auto_ack_suppressed && !find(80)->read_mentions_incomplete &&
        !find(80)->mention_count && f.guild.unread_channels == 1 && f.guild.mention_count == 1,
        "read model: explicit ordinary mark read releases manual suppression and its estimated mentions");

    check(initialize() && channel(80, 150) && channel(81, 200) &&
        read_state_upsert(&f.state, entry(80, 100, 0), 50) &&
        read_state_upsert(&f.state, entry(81, 120, 2), 50) &&
        f.guild.unread_channels == 2 && f.guild.mention_count == 2, "read model: mute fixture has two unread channels");
    check(chat_state_store_set_channel_mute(&f.state, 81, true, true) && chat_state_store_channel_unread(f.state, *find(81)) &&
        f.guild.unread_channels == 2 && f.guild.mention_count == 2 && find(81)->notification_dimmed,
        "read model: a muted channel with a mention stays unread and keeps its badge");
    check(!chat_state_store_set_channel_mute(&f.state, 81, true, true) && f.guild.unread_channels == 2,
        "read model: repeating a mute projection is not a mutation");
    check(chat_state_store_set_channel_mute(&f.state, 80, true, false) && f.guild.unread_channels == 1 &&
        !chat_state_store_channel_unread(f.state, *find(80)) &&
        chat_state_store_set_guild_mute(&f.state, 7, true) && f.guild.notification_muted,
        "read model: a muted channel without mentions leaves the unread aggregate");
    check(chat_state_store_set_channel_mute(&f.state, 80, false, false) && f.guild.unread_channels == 2 &&
        read_state_mark_local(&f.state, 80, 150, 90'000, 1) && f.guild.unread_channels == 1,
        "read model: unmute restores unread and a read channel leaves the aggregate");

    // The guild unread pill follows the official client's guild read state;
    // every channel the list hides must also stay out of it.
    check(initialize() && channel(80, 100) && read_state_upsert(&f.state, entry(80, 100, 0), 50) &&
        thread(90, 500, false) && !chat_state_store_channel_unread(f.state, *find(90)) && !f.guild.unread_channels,
        "read model: a thread the member has not joined is never unread");
    check(thread(91, 500, true, true) && !chat_state_store_channel_unread(f.state, *find(91)) && !f.guild.unread_channels,
        "read model: an archived joined thread is never unread");
    constexpr u32 joined_at = 1'700'000'000;
    check(thread(92, snowflake(u64{joined_at} * 1000 - 10'000), true, false, joined_at) &&
        !chat_state_store_channel_unread(f.state, *find(92)) && !f.guild.unread_channels,
        "read model: a joined thread without an ACK reads from its join time");
    check(chat_state_store_note_message(&f.state, 92, snowflake(u64{joined_at} * 1000 + 1'000), 3, false, false, false, false) &&
        chat_state_store_channel_unread(f.state, *find(92)) && f.guild.unread_channels == 1,
        "read model: a message after the join makes the joined thread unread");
    f.state.self_user_id = 1;
    check(chat_state_store_set_thread_self_membership(&f.state, 92, 1, false) &&
        !chat_state_store_channel_unread(f.state, *find(92)) && !f.guild.unread_channels,
        "read model: leaving a thread drops it from the guild unread aggregate");
    check(channel(81, 0, 2) && chat_state_store_note_message(&f.state, 81, 300, 3, false, false, false, false) &&
        !chat_state_store_channel_unread(f.state, *find(81)) && !f.guild.unread_channels,
        "read model: voice text chat is not unread without a mention");
    check(chat_state_store_note_message(&f.state, 81, 301, 3, true, false, false, false) &&
        chat_state_store_channel_unread(f.state, *find(81)) && f.guild.unread_channels == 1 && f.guild.mention_count == 1,
        "read model: a mention makes voice text chat unread");
    check(read_state_mark_local(&f.state, 81, 301, 0, 0) && !f.guild.unread_channels && !f.guild.mention_count,
        "read model: reading voice text chat clears it");
    {
        ChannelInput resource{};
        resource.id = 82; resource.guild_id = 7; resource.type = 0; resource.flags = 1u << 7;
        resource.last_message_id = 400; resource.last_message_known = true;
        resource.name = utf8_literal("Resource"); resource.name_known = true;
        check(chat_state_store_upsert_channel(&f.state, resource).ok() &&
            !chat_state_store_channel_unread(f.state, *find(82)) && !f.guild.unread_channels,
            "read model: a guild resource channel is not unread");
    }
    check(channel(83, 400) && f.guild.unread_channels == 1 &&
        chat_state_store_set_guild_channel_opt_in(&f.state, 7, true) && !f.guild.unread_channels &&
        !chat_state_store_channel_unread(f.state, *find(83)),
        "read model: channel opt-in recounts and drops channels the member has not opted into");
    check(chat_state_store_set_channel_opted_in(&f.state, 83, true) && f.guild.unread_channels == 1 &&
        chat_state_store_set_channel_opted_in(&f.state, 83, false) && !f.guild.unread_channels &&
        chat_state_store_set_guild_channel_opt_in(&f.state, 7, false) && f.guild.unread_channels == 1,
        "read model: opting in or out of a channel moves it in the guild unread aggregate");
    check(thread(93, 0, true, false, joined_at) && !chat_state_store_channel_unread(f.state, *find(93)) &&
        f.guild.unread_channels == 1, "read model: a joined thread without messages is not unread");

    check(initialize() && channel(80, 100), "read model: hidden channel fixture initializes");
    check(chat_state_store_bind_permissions(&f.state, {f.roles, 1}, {f.overwrites, 1}) &&
        chat_state_store_upsert_role(&f.state, {7, 7, 0, utf8_literal("everyone")}),
        "read model: hidden channel permissions initialize");
    f.guild.member_roles_known = true; f.state.self_user_id = 2;
    chat_state_store_recompute_guild_permissions(&f.state, 7);
    check(!chat_state_store_channel_visible(f.state, *find(80)) && read_state_mark_local(&f.state, 80, 100, 0, 0) &&
        find(80)->last_read_message_id == 100 && !f.guild.unread_channels && !f.guild.mention_count,
        "read model: explicit mark read updates hidden cursor independently of visible aggregate gating");
}
} // namespace maicord
