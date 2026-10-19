// Discord-sized limits of the member, voice and message stores: complete role
// lists past the inline 16, every guild's voice states, and member roles in
// messages. Counts and committed bytes, never times.
#include "maicord/core/memory.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/permission_compute.hpp"
#include "maicord/model/voice_state_store.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/gateway_payload.hpp"
#include "test_output.hpp"

namespace maicord::store_limits_test {
namespace {
u32 failures = 0;
void check(bool condition, const char* name) noexcept {
    test_write_text(condition ? "PASS " : "FAIL ");
    test_write_line(name);
    failures += condition ? 0u : 1u;
}
void metric(const char* name, u64 value) noexcept {
    test_write_text(name);
    test_write_u64(value);
    test_write_text("\n");
}

constexpr u64 kGuild = 7;
constexpr u32 kRoles = 250;
constexpr u64 kAdminRole = 1000 + 200;
constexpr u32 kAdminColor = 0x123456;
u64 role_id(u32 index) noexcept {
    return 1000 + index;
}

struct Fixture {
    ChatStateStore state{};
    GuildRecord guilds[2]{};
    u16 guild_order[2]{};
    ChannelRecord channels[1]{};
    u16 channel_order[1]{};
    u8 names[16 * 1024]{};
    RoleRecord roles[256]{};
    PermissionOverwriteRecord overwrites[1]{};
    GuildMemberRecord records[64]{};
    u32 order[64]{};
    GuildMemberStore members{};

    bool initialize() noexcept {
        bool ok = chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 1},
                                              {channel_order, 1}, {names, sizeof(names)}) &&
                  chat_state_store_bind_permissions(&state, {roles, 256}, {overwrites, 1}) &&
                  chat_state_store_upsert_guild(&state, {kGuild, utf8_literal("G"), GuildMutationKind::create, 1})
                      .ok();
        // Ranked by index; only role 200 is coloured, hoisted and administrator.
        for (u32 r = 0; ok && r < kRoles; ++r) {
            const bool admin = role_id(r) == kAdminRole;
            ok = chat_state_store_upsert_role(&state, {role_id(r), kGuild, admin ? kPermissionAdministrator : 0,
                                                       {}, r + 1, admin ? kAdminColor : 0u, admin});
        }
        return ok && guild_member_store_initialize(&members, {records, 64}, {order, 64});
    }
};
Fixture fixture;

bool upsert(u64 user, ConstSlice<u64> roles) noexcept {
    return guild_member_store_upsert(&fixture.members, fixture.state, kGuild, user, {}, roles, true,
                                     GuildMemberStatus::online)
        .ok();
}

void member_roles() noexcept {
    auto& f = fixture;
    check(f.initialize() && guild_member_store_enable_role_overflow(&f.members, 64) &&
              !guild_member_store_enable_role_overflow(&f.members, 65),
          "member roles: fixture and overflow bound");
    metric("MEASURE member_record_bytes=", sizeof(GuildMemberRecord));
    metric("MEASURE member_store_bytes=", sizeof(GuildMemberStore));
    u64 ids[kGuildMemberRoleMaximum + 1];
    for (u32 i = 0; i <= kGuildMemberRoleMaximum; ++i)
        ids[i] = role_id(i % kRoles);
    bool typical = true;
    for (u32 m = 0; m < 32; ++m)
        typical = upsert(100 + m, {ids + m % 4, 10}) && typical;
    check(typical && !f.members.role_overflow.storage.arena.base && !f.members.role_overflow.used,
          "member roles: up to 16 roles stay inline and reserve no overflow");

    // The administrator role (index 200) sits far past the inline 16.
    check(upsert(200, {ids, 220}), "member roles: 220 roles admitted");
    const auto* heavy = guild_member_store_find(f.members, kGuild, 200);
    const auto roles = heavy ? guild_member_store_role_ids(f.members, *heavy) : ConstSlice<u64>{};
    const PermissionMember subject{200, roles, 0};
    check(heavy && guild_member_store_role_ids_known(*heavy) && roles.count == 220 && roles[219] == role_id(219) &&
              (permission_compute_base(chat_state_store_permission_guild(f.state, kGuild), subject) &
               kPermissionAdministrator) &&
              heavy->role_color == kAdminColor && heavy->color_role_id == kAdminRole &&
              heavy->group_role_id == kAdminRole,
          "member roles: permissions, colour and hoist read every role past 16");
    const u32 used = f.members.role_overflow.used;
    ids[5] = role_id(249);
    check(upsert(200, {ids, 220}) && f.members.role_overflow.used == used &&
              guild_member_store_role_ids(f.members, *heavy)[5] == role_id(249),
          "member roles: a same-length change rewrites its span in place");
    ids[5] = role_id(5);
    check(upsert(201, {ids, kRoles}) && guild_member_store_role_ids_known(*guild_member_store_find(f.members, kGuild, 201)) &&
              upsert(202, {ids, kGuildMemberRoleMaximum + 1}) &&
              !guild_member_store_role_ids_known(*guild_member_store_find(f.members, kGuild, 202)) &&
              !guild_member_store_role_ids(f.members, *guild_member_store_find(f.members, kGuild, 202)).count,
          "member roles: 250 roles are kept; past 256 they are unknown, never truncated");
    const u32 before = f.members.role_overflow.used;
    check(upsert(201, {ids, 3}) && f.members.role_overflow.used == before - kRoles,
          "member roles: shrinking to inline reclaims the tail span");

    // Heavy churn: spans released mid-arena are compacted, not grown into.
    bool churn = true;
    for (u32 round = 0; round < 40 && churn; ++round)
        for (u32 m = 0; m < 8 && churn; ++m)
            churn = upsert(300 + m, {ids + round % 8, 120 + (round + m) % 2 * 10});
    u32 live = 0;
    for (u32 slot = 0; slot < f.members.capacity; ++slot)
        if (f.records[slot].occupied && f.records[slot].role_overflow)
            live += f.records[slot].role_count;
    const auto& overflow = f.members.role_overflow;
    metric("MEASURE member_overflow_heavy_live_ids=", live);
    metric("MEASURE member_overflow_heavy_committed_bytes=", overflow.storage.arena.committed);
    check(churn && overflow.compactions > 0 && overflow.used - overflow.wasted == live &&
              overflow.storage.arena.committed <= (u64{live} * 2u + 512u) * 8u,
          "member roles: churn compacts released spans and keeps the commit near the live lists");
    bool removed = true;
    for (u32 slot = 0; slot < f.members.capacity; ++slot)
        if (f.records[slot].occupied && f.records[slot].role_overflow)
            removed = guild_member_store_remove(&f.members, kGuild, f.records[slot].user_id) && removed;
    check(removed && !overflow.used && !overflow.storage.arena.committed,
          "member roles: removing every heavy member returns the overflow pages");
    check(upsert(200, {ids, 120}), "member roles: heavy member re-admitted");
    guild_member_store_release(&f.members);
    check(!f.members.role_overflow.storage.arena.base &&
              !guild_member_store_role_ids_known(*guild_member_store_find(f.members, kGuild, 200)),
          "member roles: release drops the overflow and leaves heavy lists unknown");

    // Without overflow (the search store) heavy lists stay unknown and stable.
    GuildMemberRecord records[4]{};
    u32 order[4]{};
    GuildMemberStore plain{};
    const bool plain_ok = guild_member_store_initialize(&plain, {records, 4}, {order, 4}) &&
                          guild_member_store_upsert(&plain, f.state, kGuild, 9, {}, {ids, 40}, true,
                                                    GuildMemberStatus::online)
                              .ok();
    const u32 revision = plain.revision;
    check(plain_ok && !guild_member_store_role_ids_known(records[0]) && records[0].role_count == 40 &&
              guild_member_store_upsert(&plain, f.state, kGuild, 9, {}, {ids, 40}, true, GuildMemberStatus::online)
                  .ok() &&
              plain.revision == revision,
          "member roles: without overflow an unchanged heavy list publishes no revision");
}

void message_roles() noexcept {
    char json[8192];
    u32 n = 0;
    const auto put = [&](const char* text) noexcept {
        while (*text)
            json[n++] = *text++;
    };
    const auto build = [&](u32 roles) noexcept {
        n = 0;
        put(R"({"op":0,"s":1,"t":"MESSAGE_CREATE","d":{"id":"900","channel_id":"7","author":{"id":"11","username":"W"},)"
            R"("member":{"roles":[)");
        for (u32 r = 0; r < roles; ++r) {
            char digits[8];
            u32 value = 10000 + r, d = 0;
            do {
                digits[d++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value);
            put(r ? ",\"" : "\"");
            while (d)
                json[n++] = digits[--d];
            put("\"");
        }
        put(R"(]},"content":"x","timestamp":"2024-08-30T12:34:56Z"}})");
        return Utf8View{json, n};
    };
    static u32 structurals[8192];
    char text[1024];
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds];
    u64 member_roles[kGatewayMessageMemberRoleMaximumIds];
    GatewayMessageEvent message{};
    const auto decode = [&](u32 roles, u32 scratch) noexcept {
        GatewayEnvelope envelope{};
        message = {};
        return gateway_decode_envelope(build(roles), {structurals, 8192}, 32, &envelope).ok() &&
               gateway_decode_message_create(envelope.data, {structurals, 8192}, 32, {text, sizeof(text)},
                                             {mention_roles, kGatewayMessageMentionRoleMaximumIds},
                                             {member_roles, scratch}, {}, GatewayMessageRichScratch{}, 11, &message)
                   .ok();
    };
    check(decode(250, kGatewayMessageMemberRoleMaximumIds) && message.member_roles.count == 250 &&
              message.member_roles[249] == 10249 && !message.member_roles_partial,
          "message roles: a member's 250 roles decode whole");
    check(decode(150, 100) && !message.member_roles.count && message.member_roles_partial &&
              (message.fields & gateway_message_field_member),
          "message roles: a list past the scratch is reported partial, never truncated");
    check(!decode(kGatewayMessageMemberRoleMaximumIds + 1, kGatewayMessageMemberRoleMaximumIds),
          "message roles: more than 256 roles are malformed");
}

void voice_states() noexcept {
    VoiceStateStore store{};
    check(voice_state_store_initialize_owned(&store, kVoiceStateCapacity) && !store.owned.arena.committed &&
              !store.records.count,
          "voice: owned store reserves without committing");
    // 64 guilds x 512 states = 32,768: sixteen times the former bound.
    static u8 packed[512 * sizeof(VoiceState)];
    bool applied = true;
    for (u32 g = 0; g < 64 && applied; ++g) {
        for (u32 i = 0; i < 512; ++i) {
            const VoiceState row{100 + g, 1000 + g * 16 + i % 16, 50000 + i, i % 3 ? 0u : u32{voice_self_mute}, 0};
            copy_bytes(packed + i * sizeof(VoiceState), &row, sizeof(row));
        }
        applied = voice_state_store_clear_guild(&store, 100 + g) != VoiceStateResult::invalid_input &&
                  voice_state_store_apply_batch(&store, {packed, sizeof(packed)}) == VoiceStateResult::changed;
    }
    metric("MEASURE voice_32768_committed_bytes=", store.owned.arena.committed);
    bool sorted = true;
    for (u32 i = 1; i < store.count; ++i)
        sorted = sorted && store.records[i - 1].channel_id <= store.records[i].channel_id;
    // Join order within a channel is arrival order.
    const VoiceState* second = voice_state_store_channel_at(store, 1000 + 5 * 16 + 3, 1);
    check(applied && store.count == 64 * 512 && !store.dropped && sorted &&
              voice_state_store_channel_count(store, 1000 + 5 * 16 + 3) == 32 && second &&
              second->user_id == 50000 + 16 + 3 && voice_state_store_find(store, 100 + 63, 50000 + 511) &&
              store.owned.arena.committed <= (64u * 512u + 4u * 128u) * sizeof(VoiceState),
          "voice: 32,768 states commit only their pages and stay channel-sorted in join order");

    // Single updates keep the order: a move joins the end of the new channel.
    const u64 channel = 1000 + 5 * 16 + 3;
    check(voice_state_store_apply(&store, 105, channel, 50000 + 3, voice_self_mute) == VoiceStateResult::unchanged &&
              voice_state_store_apply(&store, 105, channel + 1, 50000 + 3, 0) == VoiceStateResult::changed &&
              voice_state_store_channel_count(store, channel) == 31 &&
              voice_state_store_channel_at(store, channel + 1, 32)->user_id == 50000 + 3 &&
              voice_state_store_apply(&store, 105, 0, 50000 + 3, 0) == VoiceStateResult::changed &&
              voice_state_store_channel_count(store, channel + 1) == 32,
          "voice: move and leave keep channel runs and join order");
    // A batch with moves, flag changes, leaves and repeats equals row-by-row apply.
    VoiceState reference_rows[64];
    VoiceStateStore reference{};
    voice_state_store_initialize_sparse(&reference, {reference_rows, 64});
    VoiceStateStore batched{};
    voice_state_store_initialize_owned(&batched, 256);
    u8 rows[40 * sizeof(VoiceState)];
    for (u32 i = 0; i < 40; ++i) {
        const VoiceState row{1, i < 34 ? 10 + (i * 7) % 5 : 0, i < 34 ? 900 + (i * 3) % 23 : 900 + (i - 34) * 2,
                             i % 4 ? 0u : u32{voice_self_deaf}, 0};
        copy_bytes(rows + i * sizeof(VoiceState), &row, sizeof(row));
    }
    for (u32 i = 0; i < 40; ++i) {
        VoiceState row;
        copy_bytes(&row, rows + i * sizeof(VoiceState), sizeof(row));
        voice_state_store_apply(&reference, row.guild_id, row.channel_id, row.user_id, row.flags);
    }
    voice_state_store_apply_batch(&batched, {rows, sizeof(rows)});
    bool same = batched.count == reference.count;
    for (u32 i = 0; same && i < reference.count; ++i)
        same = bytes_equal(&batched.records[i], &reference.records[i], sizeof(VoiceState));
    check(same && reference.count > 12, "voice: a batch applies exactly as its rows one by one");
    voice_state_store_release(&batched);

    // At the bound the largest other guild goes; the selected one never.
    store.retained_guild = 100;
    const u32 evictions = store.evictions;
    VoiceStateStore small{};
    voice_state_store_initialize_owned(&small, 256);
    small.retained_guild = 1;
    bool filled = true;
    for (u32 i = 0; i < 256 && filled; ++i)
        filled = voice_state_store_apply(&small, i < 100 ? 1 : i < 220 ? 2 : 3, 10 + i % 3, 7000 + i, 0) ==
                 VoiceStateResult::changed;
    check(filled && voice_state_store_apply(&small, 4, 99, 1, 0) == VoiceStateResult::changed &&
              small.evictions == 1 && small.dropped == 120 && !voice_state_store_find(small, 2, 7100) &&
              voice_state_store_find(small, 1, 7000) && voice_state_store_find(small, 4, 1),
          "voice: at the bound the largest unselected guild is evicted");
    voice_state_store_release(&small);
    (void)evictions;

    check(voice_state_store_reset(&store) && store.owned.arena.committed <= 4096,
          "voice: reset returns the committed pages");
    voice_state_store_release(&store);
}
} // namespace

void run() noexcept {
    member_roles();
    message_roles();
    voice_states();
}
} // namespace maicord::store_limits_test

extern "C" __declspec(noreturn) void WINAPI maicord_store_limits_entry() noexcept {
    maicord::store_limits_test::run();
    ExitProcess(maicord::store_limits_test::failures ? 1 : 0);
}
