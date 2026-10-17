#include "maicord/app/gateway_member_request_owner.hpp"
#include "maicord/app/mention_member_query.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/protocol/message_create.hpp"
#include "maicord/ui/composer_autocomplete.hpp"
#include "maicord/ui/message_markdown.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool contains(Utf8View text, Utf8View expected) noexcept {
    for (u32 i = 0; i + expected.bytes <= text.bytes; ++i)
        if (bytes_equal(text.data + i, expected.data, expected.bytes))
            return true;
    return false;
}

Utf8View insertion(const ComposerAutocompleteSuggestion& suggestion) noexcept {
    return {suggestion.insertion, suggestion.insertion_bytes};
}

struct MentionFixture {
    ChannelRecord channels[2]{};
    u16 channel_order[2]{};
    RoleRecord roles[4]{};
    u8 names[64]{};
    ChatStateStore state{};
    UserRecord user_records[1]{};
    u32 user_order[1]{};
    u8 user_names[32]{};
    UserStore users{};
    MemberListView members{};
    NavigationView navigation{};
    char text[64]{};
    MessageComposer composer{};
    ComposerAutocomplete autocomplete{};

    void role(u32 index, u64 id, Utf8View name, u32 position, bool mentionable, u32 color, u32* used) noexcept {
        RoleRecord& record = roles[index];
        record.id = id;
        record.guild_id = 99;
        record.position = position;
        record.mentionable = mentionable ? 1 : 0;
        record.color = color;
        record.name_offset = *used;
        record.name_bytes = static_cast<u16>(name.bytes);
        copy_bytes(names + *used, name.data, name.bytes);
        *used += name.bytes;
    }

    bool initialize() noexcept {
        // Live roles stay sorted by (guild_id, id); the @everyone role id is the guild id.
        u32 used = 0;
        role(0, 99, utf8_literal("everyone"), 0, true, 0, &used);
        role(1, 501, utf8_literal("Mods"), 5, true, 0xE91E63, &used);
        role(2, 502, utf8_literal("Admins"), 9, false, 0, &used);
        role(3, 503, utf8_literal("Members"), 1, true, 0, &used);
        channels[0].id = 7;
        channels[0].guild_id = 99;
        channels[0].type = 0;
        channels[1].id = 8;
        channels[1].type = 1;
        channels[1].actions = channel_action_mention_everyone;
        channel_order[0] = 0;
        channel_order[1] = 1;
        state.channels = channels;
        state.channel_order = channel_order;
        state.channel_count = 2;
        state.roles = roles;
        state.role_count = 4;
        state.names = names;
        state.name_bank_bytes = sizeof(names);
        return message_composer_initialize(&composer, Slice<char>{text, sizeof(text)}) &&
               user_store_initialize(&users, Slice<UserRecord>{user_records, 1}, Slice<u32>{user_order, 1},
                                     Slice<u8>{user_names, sizeof(user_names)});
    }

    bool build(u64 channel_id, Utf8View draft) noexcept {
        return message_composer_restore(&composer, channel_id, draft, draft.bytes, draft.bytes) &&
               composer_autocomplete_build(&autocomplete, composer, members, users, navigation, state);
    }
};

void test_autocomplete_roles(Check check) noexcept {
    MentionFixture f{};
    check(f.initialize(), "composer mentions: fixture initializes");
    const auto& s = f.autocomplete.suggestions;
    check(f.build(7, utf8_literal("@")) && f.autocomplete.kind == ComposerAutocompleteKind::member &&
              f.autocomplete.count == 2 && s[1].item_kind == ComposerAutocompleteKind::role && s[1].role_id == 501 &&
              utf8_equal(s[1].label, utf8_literal("@Mods")) && s[1].role_color == 0xE91E63 &&
              utf8_equal(insertion(s[1]), utf8_literal("<@&501> ")) &&
              utf8_equal(s[1].description,
                      utf8_literal("Notify users with this role who have permission to view this channel.")) &&
              s[0].role_id == 503 && !s[0].role_color,
          "composer mentions: without MENTION_EVERYONE only mentionable roles list, by name like match-sorter, "
          "no globals");
    f.channels[0].actions = channel_action_mention_everyone;
    check(f.build(7, utf8_literal("@")) && f.autocomplete.count == 5 &&
              s[0].item_kind == ComposerAutocompleteKind::global_mention &&
              utf8_equal(insertion(s[0]), utf8_literal("@everyone ")) &&
              utf8_equal(s[0].description, utf8_literal("Notify everyone who has permission to view this channel.")) &&
              utf8_equal(insertion(s[1]), utf8_literal("@here ")) &&
              utf8_equal(s[1].description,
                      utf8_literal("Notify everyone online who has permission to view this channel.")) &&
              s[2].role_id == 502 && s[3].role_id == 503 && s[4].role_id == 501,
          "composer mentions: MENTION_EVERYONE adds the globals and every role except @everyone's own");
    check(f.build(7, utf8_literal("@her")) && f.autocomplete.count == 1 &&
              utf8_equal(s[0].label, utf8_literal("@here")),
          "composer mentions: globals match their names like roles");
    check(f.build(7, utf8_literal("@mem")) && f.autocomplete.count == 1 && s[0].role_id == 503 &&
              f.build(7, utf8_literal("@ds")) && f.autocomplete.count == 2 && s[0].role_id == 501 &&
              s[1].role_id == 502,
          "composer mentions: role queries use subsequence matching and rank substring above scattered hits");
    check(f.build(8, utf8_literal("@e")) && f.autocomplete.count == 0,
          "composer mentions: private channels never offer roles or globals");
    composer_autocomplete_release(&f.autocomplete);
}

// Users without the member list: cached guild members (who can view the
// channel), recent authors for an empty query, threads, private channels.
struct MemberFixture {
    GuildRecord guilds[1]{};
    u16 guild_order[1]{};
    ChannelRecord channels[3]{};
    u16 channel_order[3]{};
    RoleRecord roles[2]{};
    u8 names[32]{};
    ChatStateStore state{};
    UserRecord user_records[24]{};
    u32 user_order[24]{};
    u8 user_names[1024]{};
    UserStore users{};
    GuildMemberRecord member_records[24]{};
    u32 member_order[24]{};
    GuildMemberStore members{};
    MessageRecord message_records[8]{};
    u32 message_order[8]{};
    u8 message_text[64]{};
    MessageStore messages{};
    PrivateChannelRecord private_records[1]{};
    u64 private_recipients[4]{};
    PrivateNicknameRecord private_nicknames[2]{};
    PrivateChannelStore privates{};
    MemberListView list{};
    NavigationView navigation{};
    char text[64]{};
    MessageComposer composer{};
    ComposerAutocomplete autocomplete{};

    bool user(u64 id, Utf8View display, Utf8View username) noexcept {
        return user_store_upsert(&users, id, display, username).ok();
    }
    bool member(u64 id, Utf8View nickname, bool crew, bool roles_known = true, u32 list_index = ~0u) noexcept {
        const u64 role = 601;
        return guild_member_store_upsert(&members, state, 99, id, nickname, {crew ? &role : nullptr, crew ? 1u : 0u},
                                         roles_known, GuildMemberStatus::online, list_index, list_index == ~0u ? 0 : 7)
            .ok();
    }
    bool message(u64 id, u64 channel_id, u64 author_id) noexcept {
        MessageInput input{};
        input.id = id;
        input.channel_id = channel_id;
        input.author_id = author_id;
        input.timestamp_ms = kSnowflakeEpochMilliseconds + id;
        input.content = utf8_literal("x");
        return message_store_upsert(&messages, input).ok();
    }

    bool initialize() noexcept {
        guilds[0].id = 99;
        guilds[0].owner_id = 1000;
        state.guilds = guilds;
        state.guild_order = guild_order;
        state.guild_count = 1;
        // 7: guild text channel, 8: DM, 9: thread under 7.
        channels[0].id = 7;
        channels[0].guild_id = 99;
        channels[1].id = 8;
        channels[1].type = 1;
        channels[2].id = 9;
        channels[2].guild_id = 99;
        channels[2].type = 11;
        channels[2].parent_id = 7;
        for (u16 i = 0; i < 3; ++i)
            channel_order[i] = i;
        state.channels = channels;
        state.channel_order = channel_order;
        state.channel_count = 3;
        // @everyone cannot view; Crew can. Crew is mentionable.
        const auto crew = utf8_literal("Crew");
        roles[0] = RoleRecord{};
        roles[0].id = 99;
        roles[0].guild_id = 99;
        roles[1] = RoleRecord{};
        roles[1].id = 601;
        roles[1].guild_id = 99;
        roles[1].permissions = kPermissionViewChannel | kPermissionSendMessages;
        roles[1].position = 1;
        roles[1].mentionable = 1;
        roles[1].name_bytes = static_cast<u16>(crew.bytes);
        copy_bytes(names, crew.data, crew.bytes);
        state.roles = roles;
        state.role_count = 2;
        state.names = names;
        state.name_bank_bytes = sizeof(names);
        state.self_user_id = 1;
        state.private_channels = &privates;
        if (!message_composer_initialize(&composer, Slice<char>{text, sizeof(text)}) ||
            !user_store_initialize(&users, {user_records, 24}, {user_order, 24}, {user_names, sizeof(user_names)}) ||
            !guild_member_store_initialize(&members, {member_records, 24}, {member_order, 24}) ||
            !message_store_initialize(&messages, {message_records, 8}, {message_order, 8},
                                      {message_text, sizeof(message_text)}) ||
            !private_channel_store_initialize(&privates, {private_records, 1}, {private_recipients, 4},
                                              {private_nicknames, 2}))
            return false;
        bool ok = user(1, utf8_literal("Alice"), utf8_literal("alice")) &&
                  user(2, utf8_literal("Al"), utf8_literal("alfred99")) &&
                  user(3, utf8_literal("Bob"), utf8_literal("bobby")) &&
                  user(4, utf8_literal("Carol"), utf8_literal("carol")) &&
                  user(5, utf8_literal("Dave"), utf8_literal("dave")) &&
                  user(6, utf8_literal("Alma"), utf8_literal("alma")) &&
                  user(7, utf8_literal("Malcolm"), utf8_literal("malcolm")) &&
                  user(77, utf8_literal("Hook"), utf8_literal("hook")) &&
                  member(1, {}, true, true, 4) && member(2, {}, true, true, 2) && member(3, {}, true) &&
                  member(4, utf8_literal("Alcove"), true, true, 9) && member(5, {}, true) &&
                  // Alma's known roles cannot view #7; Malcolm's roles are not known yet.
                  member(6, {}, false) && member(7, {}, false, false, 1);
        static constexpr const char* allies[12]{"ally01", "ally02", "ally03", "ally04", "ally05", "ally06",
                                                "ally07", "ally08", "ally09", "ally10", "ally11", "ally12"};
        for (u32 i = 0; i < 12 && ok; ++i)
            ok = user(100 + i, {allies[i], 6}, {allies[i], 6}) && member(100 + i, {}, true, true, 20 + i);
        // #7: authors 2, 3, 2, a non-member 77, then 1 newest. The thread: 3.
        ok = ok && message(11, 7, 2) && message(12, 7, 3) && message(13, 7, 2) && message(14, 7, 77) &&
             message(15, 7, 1) && message(16, 9, 3);
        const u64 recipients[2]{3, 5};
        PrivateChannelInput dm{};
        dm.id = 8;
        dm.recipients = {recipients, 2};
        dm.recipients_known = true;
        const PrivateNicknameInput nick{5, utf8_literal("Davey")};
        return ok && private_channel_store_replace(&privates, dm) &&
               private_channel_store_replace_nicknames(&privates, 8, {&nick, 1});
    }

    bool build(u64 channel_id, Utf8View draft, bool with_messages = true) noexcept {
        return message_composer_restore(&composer, channel_id, draft, draft.bytes, draft.bytes) &&
               composer_autocomplete_build(&autocomplete, composer, list, users, navigation, state, &members,
                                           with_messages ? &messages : nullptr);
    }
    u64 user_at(u32 index) const noexcept {
        return index < autocomplete.count ? autocomplete.suggestions[index].user_id : 0;
    }
};

void test_autocomplete_members(Check check) noexcept {
    static MemberFixture f;
    f = MemberFixture{};
    check(f.initialize(), "composer mentions: member fixture initializes");
    const auto& s = f.autocomplete.suggestions;
    check(f.build(7, utf8_literal("@ali")) && f.autocomplete.count == 1 && f.user_at(0) == 1 &&
              utf8_equal(s[0].label, utf8_literal("Alice")) && utf8_equal(s[0].username, utf8_literal("alice")) &&
              utf8_equal(insertion(s[0]), utf8_literal("<@1> ")) && f.autocomplete.member_query_guild_id == 99,
          "composer mentions: cached guild members match with the member list closed and ask the gateway");
    check(f.build(7, utf8_literal("@alc")) && f.autocomplete.count == 3 && f.user_at(0) == 4 &&
              utf8_equal(s[0].label, utf8_literal("Alcove")) && f.user_at(1) == 7 && f.user_at(2) == 1,
          "composer mentions: a nickname prefix outranks in-order letter matches (list order breaks ties)");
    check(f.build(7, utf8_literal("@alm")) && f.autocomplete.count == 1 && f.user_at(0) == 7 &&
              f.autocomplete.member_query_guild_id == 99,
          "composer mentions: members whose known roles cannot view the channel are left out");
    check(f.build(7, utf8_literal("@ally")) && f.autocomplete.count == kMentionAutocompleteLimit &&
              f.user_at(0) == 100 && f.user_at(9) == 109 && s[9].item_kind == ComposerAutocompleteKind::member,
          "composer mentions: ten users fill the list and leave no row for roles");
    check(f.build(7, utf8_literal("@zzz")) && f.autocomplete.count == 0 &&
              f.autocomplete.kind == ComposerAutocompleteKind::none && f.autocomplete.member_query_guild_id == 99,
          "composer mentions: an unmatched query still asks the gateway for members");
    check(f.build(7, utf8_literal("@")) && f.autocomplete.count == 4 && f.user_at(0) == 1 && f.user_at(1) == 2 &&
              f.user_at(2) == 3 && s[3].item_kind == ComposerAutocompleteKind::role && s[3].role_id == 601 &&
              !f.autocomplete.member_query_guild_id,
          "composer mentions: an empty query lists recent member authors newest first, then roles, with no "
          "gateway request");
    check(f.build(9, utf8_literal("@")) && f.autocomplete.count == 2 && f.user_at(0) == 3 &&
              s[1].role_id == 601,
          "composer mentions: a thread's recent authors are its own");
    check(f.build(9, utf8_literal("@alc")) && f.autocomplete.count == 3 && f.user_at(0) == 4 &&
              f.autocomplete.member_query_guild_id == 99,
          "composer mentions: threads search guild members through the parent's permissions");
    check(f.build(7, utf8_literal("@"), false) && f.autocomplete.count == kMentionAutocompleteLimit &&
              f.user_at(0) == 7 && f.user_at(1) == 2 && f.autocomplete.member_query_guild_id == 99,
          "composer mentions: without recent authors an empty query lists cached members and asks the gateway");
    check(f.build(8, utf8_literal("@")) && f.autocomplete.count == 3 && f.user_at(0) == 3 && f.user_at(1) == 5 &&
              utf8_equal(s[1].label, utf8_literal("Davey")) && f.user_at(2) == 1 &&
              !f.autocomplete.member_query_guild_id,
          "composer mentions: private channels list their recipients (with channel nicknames), then you");
    check(f.build(8, utf8_literal("@dav")) && f.autocomplete.count == 1 && f.user_at(0) == 5 &&
              f.build(8, utf8_literal("@cr")) && f.autocomplete.count == 0,
          "composer mentions: private channels match recipients only and never offer roles");
    composer_autocomplete_release(&f.autocomplete);
}

// Opcode 8 member search: the official 200 ms shared debounce, limit 10, one
// request per guild/query a minute; its own queue slot beside author batches.
void test_member_query(Check check) noexcept {
    MentionMemberQueryOwner owner{};
    GatewayGuildMemberRequest request{};
    check(mention_member_query_note(&owner, 99, 7, utf8_literal("Al"), 1000) &&
              mention_member_query_next_wake(owner) == 1200 &&
              !mention_member_query_due(owner, 1199, &request) &&
              mention_member_query_note(&owner, 99, 7, utf8_literal("al"), 1100) &&
              mention_member_query_next_wake(owner) == 1200 &&
              mention_member_query_note(&owner, 99, 7, utf8_literal("Ali"), 1150) &&
              mention_member_query_next_wake(owner) == 1350,
          "member query: one trailing 200 ms timer restarts for a new query, not for the same one");
    const bool due = mention_member_query_due(owner, 1350, &request);
    check(due && request.guild_id == 99 && request.channel_id == 7 && request.user_count == 0 &&
              request.limit == 10 && utf8_equal({request.query, request.query_bytes}, utf8_literal("ali")) &&
              (request.nonce & kMentionMemberQueryNonceTag) == kMentionMemberQueryNonceTag &&
              gateway_guild_member_request_is_query(request) && gateway_guild_member_request_valid(request),
          "member query: the due request carries the lower-cased query and limit 10");
    mention_member_query_complete(&owner, request, 1350, true);
    check(!mention_member_query_next_wake(owner) && owner.requests == 1 &&
              !mention_member_query_note(&owner, 99, 9, utf8_literal("ALI"), 30'000) &&
              !mention_member_query_next_wake(owner) &&
              mention_member_query_note(&owner, 98, 9, utf8_literal("ali"), 30'000) &&
              mention_member_query_note(&owner, 99, 7, utf8_literal("ali"), 61'351),
          "member query: a guild/query pair is asked at most once a minute");
    mention_member_query_due(owner, 61'551, &request);
    mention_member_query_complete(&owner, request, 61'551, false);
    check(mention_member_query_note(&owner, 99, 7, utf8_literal("ali"), 61'600) &&
              !mention_member_query_note(&owner, 99, 7, {"\xC3", 1}, 61'700) &&
              mention_member_query_next_wake(owner) == 61'800,
          "member query: an unsent query is not remembered; undecodable queries are not asked");

    request = {};
    request.guild_id = 7;
    request.channel_id = 80;
    request.nonce = 12;
    request.limit = 10;
    const auto quoted = utf8_literal("a\"b");
    copy_bytes(request.query, quoted.data, quoted.bytes);
    request.query_bytes = quoted.bytes;
    u8 wire[512]{};
    const auto written = gateway_write_guild_member_request(request, {wire, sizeof(wire)});
    const auto expected =
        utf8_literal(R"({"op":8,"d":{"guild_id":"7","query":"a\"b","limit":10,"presences":false,"nonce":"12"}})");
    const auto fitted = gateway_write_guild_member_request(request, {wire, written.bytes});
    const auto short_output = gateway_write_guild_member_request(request, {wire, written.bytes - 1});
    request.query_bytes = 0;
    const auto empty = gateway_write_guild_member_request(request, {wire, sizeof(wire)});
    const auto empty_expected =
        utf8_literal(R"({"op":8,"d":{"guild_id":"7","query":"","limit":10,"presences":false,"nonce":"12"}})");
    check(written.ok() && fitted.ok() && !short_output.ok() && empty.ok() &&
              utf8_equal({reinterpret_cast<const char*>(wire), empty.bytes}, empty_expected) &&
              expected.bytes == written.bytes,
          "member query: opcode 8 sends guild_id, query, limit, presences false and the nonce");
    request.limit = 101;
    const bool too_many = !gateway_guild_member_request_valid(request);
    request.limit = 10;
    request.query_bytes = kGatewayGuildMemberQueryMaximumBytes + 1;
    check(too_many && !gateway_guild_member_request_valid(request), "member query: limits are validated");
    request.query_bytes = 1;

    GatewayGuildMemberRequest batch{};
    batch.guild_id = 7;
    batch.channel_id = 80;
    batch.nonce = 3;
    batch.user_count = 1;
    batch.user_ids[0] = 20;
    GatewayMemberRequestOwner queue{};
    check(gateway_member_request_admit(&queue, batch) && gateway_member_request_admit(&queue, request) &&
              gateway_member_request_next(queue) == &queue.query,
          "member query: a search queues beside a visible-author batch and goes first");
    gateway_member_request_cancel_batch(&queue);
    check(gateway_member_request_next(queue) == &queue.query && gateway_member_request_admit(&queue, batch),
          "member query: cancelling author batches keeps the search");
    gateway_member_request_sent(&queue);
    check(gateway_member_request_next(queue) == &queue.pending, "member query: the batch follows the search");
    gateway_member_request_sent(&queue);
    check(!gateway_member_request_next(queue), "member query: both slots drain");

    gateway_member_request_rate_limited(&queue, {7, 500, request.nonce, 8}, 1000);
    check(queue.query_pending && queue.query_retries == 1 &&
              !gateway_member_request_next(queue, 1499),
          "member query: server rejection requeues the admitted name query behind its guild deadline");
    gateway_member_request_cancel_query(&queue);
    gateway_member_request_rate_limited(&queue, {7, 100, request.nonce, 8}, 1100);
    check(!queue.query_pending && !queue.query_current && queue.batch_current,
          "member query: typed query cancellation retires rejected replay authority and preserves author interest");

    ++batch.nonce;
    check(gateway_member_request_admit(&queue, batch, 1200) && queue.occupied &&
              !gateway_member_request_next(queue, 1499) &&
              gateway_member_request_next(queue, 1500) == &queue.pending,
          "member query: cancellation preserves the guild floor for a separately queued author batch");
    gateway_member_request_sent(&queue, gateway_member_request_next(queue, 1500));
    ++request.nonce;
    check(gateway_member_request_admit(&queue, request, 1500),
          "member query: a new query supersedes cancelled query interest");
    gateway_member_request_sent(&queue, gateway_member_request_next(queue, 1500));
    const u64 old_nonce = request.nonce;
    ++request.nonce;
    request.query[0] = 'b';
    check(gateway_member_request_admit(&queue, request, 1500),
          "member query: a replacement query is admitted independently");
    gateway_member_request_rate_limited(&queue, {7, 100, old_nonce, 8}, 1500);
    check(queue.query_pending && queue.query.nonce == request.nonce && !queue.query_retries &&
              gateway_member_request_next(queue, 1600) == &queue.query,
          "member query: old rejection holds the shared guild floor without replacing or replaying stale "
          "query text");
    gateway_member_request_sent(&queue, gateway_member_request_next(queue, 1600));
    gateway_member_request_complete(&queue, {7, request.nonce, 0, 1});
    gateway_member_request_rate_limited(&queue, {7, 100, request.nonce, 8}, 1600);
    check(!queue.query_pending && !queue.query_current,
          "member query: completed name lookup cannot be resurrected by a late rejection");
}

void test_mention_warning(Check check) noexcept {
    check(message_mention_warning_round(1234) == 1000 && message_mention_warning_round(31) == 30 &&
              message_mention_warning_round(999) == 900 && message_mention_warning_round(7) == 7 &&
              message_mention_warning_round(12345) == 10000,
          "composer mentions: warning count rounds down to its leading digit");
    MessageMentionWarningInput input{utf8_literal("hi @everyone"),        1234, 40,  0,
                                     MessageMentionWarningPlace::channel, true, true};
    auto warning = message_mention_warning_evaluate(input);
    char body[kMessageMentionWarningBodyBytes]{};
    u32 bytes = message_mention_warning_body(warning, input.place, {body, sizeof(body)});
    check(warning.required && !warning.here && warning.count == 1000 &&
              utf8_equal({body, bytes},
                      utf8_literal(
                          "By using @everyone, you are about to mention 1,000+ people. Are you sure?")),
          "composer mentions: large @everyone in a guild channel asks with the official copy");
    input.content = utf8_literal("@here first then @everyone");
    warning = message_mention_warning_evaluate(input);
    check(warning.required && warning.here && warning.count == 40,
          "composer mentions: @here counts online members");
    input.online_count = 30;
    check(!message_mention_warning_evaluate(input).required,
          "composer mentions: 30 people or fewer send directly");
    input.content = utf8_literal("`@everyone` and ```\n@here\n```");
    check(!message_mention_warning_evaluate(input).required,
          "composer mentions: mentions inside inline and fenced code are ignored");
    input.content = utf8_literal("@everyone");
    input.can_mention_everyone = false;
    check(!message_mention_warning_evaluate(input).required,
          "composer mentions: no confirmation without MENTION_EVERYONE");
    input.can_mention_everyone = true;
    input.guild_channel = false;
    check(!message_mention_warning_evaluate(input).required, "composer mentions: no confirmation outside guilds");
    input.guild_channel = true;
    input.place = MessageMentionWarningPlace::thread;
    input.thread_member_count = 52;
    warning = message_mention_warning_evaluate(input);
    bytes = message_mention_warning_body(warning, input.place, {body, sizeof(body)});
    check(warning.required && warning.count == 50 &&
              utf8_equal({body, bytes}, utf8_literal(
                  "By using @everyone, you are about to mention 50+ people in this thread. Are you sure?")),
          "composer mentions: threads count thread members");
}

void test_silent_send(Check check) noexcept {
    Utf8View rest{};
    check(message_silent_prefix(utf8_literal("@silent  hello \n"), &rest) &&
              utf8_equal(rest, utf8_literal("hello")) && message_silent_prefix(utf8_literal("@silent"), &rest) &&
              !rest.bytes && !message_silent_prefix(utf8_literal("@silently"), &rest) &&
              !message_silent_prefix(utf8_literal(" @silent hi"), &rest),
          "composer mentions: @silent prefix needs a following space or the end and trims the rest");
    u8 body[512]{};
    const auto written = write_message_create_body(utf8_literal("hello"), 5, 0, 0, {}, {}, {body, sizeof(body)},
                                                   true, kMessageFlagSuppressNotifications);
    const Utf8View json{reinterpret_cast<const char*>(body), written.bytes};
    const auto plain = write_message_create_body(utf8_literal("x"), 6, {body + 256, 256});
    check(written.ok() && contains(json, utf8_literal(R"("flags":4096)")) &&
              contains(json, utf8_literal(R"({"content":"hello")")) && plain.ok() &&
              contains({reinterpret_cast<const char*>(body + 256), plain.bytes}, utf8_literal(R"("flags":0,)")) &&
              !write_message_create_body(utf8_literal("x"), 7, 0, 0, {}, {}, {body, sizeof(body)}, true, 1)
                   .ok(),
          "composer mentions: message body writes SUPPRESS_NOTIFICATIONS and rejects other flags");

    MessageRecord records[8]{};
    u32 order[8]{}, heights[8]{};
    u64 fenwick[8]{};
    u8 text[512]{}, draft_text[128]{}, request_text[kMessageComposerMaximumBytes]{};
    char composer_text[128]{};
    MessageDraftRecord draft_records[4]{};
    MessageStore store{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageComposer composer{};
    MessageDraftStore drafts{};
    MessageComposerController controller{};
    const bool initialized =
        message_store_initialize(&store, {records, 8}, {order, 8}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&list, {heights, 8}, {fenwick, 8}, 0, 100) &&
        message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
        message_composer_initialize(&composer, {composer_text, sizeof(composer_text)}) &&
        message_draft_store_initialize(&drafts, {draft_records, 4}, {draft_text, sizeof(draft_text)}) &&
        message_composer_controller_initialize(&controller, &composer, &drafts, &timeline,
                                               {request_text, sizeof(request_text)}) &&
        message_composer_controller_select_channel(&controller, 7).ok();
    check(initialized, "composer mentions: controller fixture initializes");
    if (!initialized)
        return;
    message_composer_insert_utf8(&composer, utf8_literal("@silent"));
    const auto empty = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 10);
    check(!empty.ok() && composer.bytes == 7 && !controller.request_count,
          "composer mentions: a bare @silent keeps the draft and sends nothing");
    message_composer_insert_utf8(&composer, utf8_literal("   quiet please "));
    const auto queued = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 11);
    MessageComposerSubmit submit{};
    const auto found = message_store_find(store, queued.nonce);
    check(queued.ok() && message_composer_controller_prepare_submit(&controller, 12, &submit).ok() &&
              submit.suppress_notifications && utf8_equal(submit.content, utf8_literal("quiet please")) && found.ok() &&
              utf8_equal(message_store_content(store, found.handle), utf8_literal("quiet please")),
          "composer mentions: @silent strips the prefix for the request and the local echo and marks the flag");
}

void test_command_mentions(Check check) noexcept {
    char output[256]{};
    u32 styles[256]{};
    TextStyleSpan spans[32]{};
    MessageMarkdownSemantic semantics[8]{};
    MessageMarkdownOptions options{};
    constexpr Utf8View source = utf8_literal("try </ban user temp:42> or </ping:7>");
    auto result = message_markdown_parse(source, {output, sizeof(output)}, {styles, sizeof(styles) / 4},
                                         {spans, 32}, options, {semantics, 8});
    check(result.ok() && utf8_equal(result.text, utf8_literal("try /ban user temp or /ping")) &&
              result.semantic_count == 2 && semantics[0].kind == MessageMarkdownSemanticKind::command_mention &&
              semantics[0].target == 42 && semantics[0].byte_offset == 4 && semantics[0].byte_count == 14 &&
              semantics[0].source_offset == 4 && semantics[0].source_bytes == 19 && semantics[1].target == 7 &&
              (styles[4] & text_style_mention) && (styles[17] & text_style_mention) && !(styles[4] & text_style_link),
          "composer mentions: command mentions render as /name sub pills with id and source semantics");
    options.has_hover = true;
    options.hovered_kind = MessageMarkdownSemanticKind::command_mention;
    options.hovered_source_offset = 4;
    result = message_markdown_parse(source, {output, sizeof(output)}, {styles, sizeof(styles) / 4}, {spans, 32},
                                    options, {semantics, 8});
    check(result.ok() && (styles[4] & text_style_interactive_hover) && !(styles[22] & text_style_interactive_hover),
          "composer mentions: hovered command mention gains the hover style");
    options = {};
    options.preserve_entities = true;
    result = message_markdown_parse(source, {output, sizeof(output)}, {styles, sizeof(styles) / 4}, {spans, 32},
                                    options, {semantics, 8});
    check(result.ok() && utf8_equal(result.text, source) && result.semantic_count == 0 &&
              (styles[4] & text_style_mention) && (styles[22] & text_style_mention),
          "composer mentions: editors keep command mention source intact");
    options = {};
    result = message_markdown_parse(utf8_literal("</a b c d:1> </bad!:2> </x:0>"), {output, sizeof(output)},
                                    {styles, sizeof(styles) / 4}, {spans, 32}, options, {semantics, 8});
    check(result.ok() && result.semantic_count == 0, "composer mentions: malformed command mentions stay text");
    options.profile_bio = true;
    result = message_markdown_parse(utf8_literal("</ping:7>"), {output, sizeof(output)}, {styles, sizeof(styles) / 4},
                                    {spans, 32}, options, {semantics, 8});
    check(result.ok() && utf8_equal(result.text, utf8_literal("/ping")) && result.semantic_count == 0 &&
              !(styles[0] & text_style_mention),
          "composer mentions: profile bios show plain command names");
}
} // namespace

void test_composer_mentions(void (*check)(bool, const char*) noexcept) noexcept {
    test_autocomplete_roles(check);
    test_autocomplete_members(check);
    test_member_query(check);
    test_mention_warning(check);
    test_silent_send(check);
    test_command_mentions(check);
}

} // namespace maicord
