#include "maicord/ui/composer_autocomplete.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/permission_compute.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/navigation_view.hpp"

namespace maicord {
namespace {

bool ensure_suggestion_storage(ComposerAutocomplete* autocomplete) noexcept {
    if (autocomplete->suggestions) {
        return true;
    }
    constexpr usize bytes = sizeof(ComposerAutocompleteSuggestion) * kComposerAutocompleteCapacity;
    if (!arena_initialize(&autocomplete->storage, bytes, 0, 4 * 1024)) {
        return false;
    }
    // This owner has just reserved new, zero-filled pages.
    autocomplete->suggestions = arena_allocate<ComposerAutocompleteSuggestion>(
        &autocomplete->storage, kComposerAutocompleteCapacity);
    if (!autocomplete->suggestions) {
        arena_release(&autocomplete->storage);
        return false;
    }
    return true;
}

bool contains_folded_ascii(Utf8View candidate, Utf8View query) noexcept {
    if (query.bytes == 0) {
        return true;
    }
    if (!candidate.data || query.bytes > candidate.bytes) {
        return false;
    }
    for (u32 start = 0; start <= candidate.bytes - query.bytes; ++start) {
        bool match = true;
        for (u32 index = 0; index < query.bytes; ++index) {
            if (ascii_lower(static_cast<u8>(candidate.data[start + index])) !=
                ascii_lower(static_cast<u8>(query.data[index]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool active_token(const MessageComposer& composer, Utf8View* token, u32* begin) noexcept {
    if (!composer.text || composer.anchor != composer.caret ||
        composer.caret > composer.bytes) {
        return false;
    }
    u32 first = composer.caret;
    while (first != 0) {
        const char value = composer.text[first - 1];
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
            break;
        }
        --first;
    }
    if (first == composer.caret) {
        return false;
    }
    *token = Utf8View{composer.text + first, composer.caret - first};
    *begin = first;
    return true;
}

u32 decimal_u64(u64 value, Slice<char> output) noexcept {
    const u32 digits = decimal_digits(value);
    return digits > output.count ? 0 : decimal_write(output.data, value);
}

bool set_snowflake_insertion(ComposerAutocompleteSuggestion* suggestion, char marker, u64 id) noexcept {
    if (id == 0) {
        return false;
    }
    char digits[20]{};
    const u32 digit_count = decimal_u64(id, Slice<char>{digits, 20});
    if (digit_count == 0 || digit_count + 4u > sizeof(suggestion->insertion)) {
        return false;
    }
    u32 cursor = 0;
    suggestion->insertion[cursor++] = '<';
    suggestion->insertion[cursor++] = marker;
    for (u32 digit = 0; digit < digit_count; ++digit) {
        suggestion->insertion[cursor++] = digits[digit];
    }
    suggestion->insertion[cursor++] = '>';
    suggestion->insertion[cursor++] = ' ';
    suggestion->insertion_bytes = static_cast<u16>(cursor);
    return true;
}

bool set_emoji_insertion(ComposerAutocompleteSuggestion* suggestion, Utf8View glyph) noexcept {
    if (!glyph.data || glyph.bytes == 0 ||
        glyph.bytes + 1u > sizeof(suggestion->insertion)) {
        return false;
    }
    u32 cursor = 0;
    copy_bytes(suggestion->insertion + cursor, glyph.data, glyph.bytes);
    cursor += glyph.bytes;
    suggestion->insertion[cursor++] = ' ';
    suggestion->insertion_bytes = static_cast<u16>(cursor);
    return true;
}

bool set_emoji_label(ComposerAutocompleteSuggestion* suggestion, Utf8View name, Utf8View glyph) noexcept {
    if (!name.data || !glyph.data || name.bytes == 0 || glyph.bytes == 0 ||
        glyph.bytes + name.bytes + 4u > sizeof(suggestion->label_storage)) {
        return false;
    }
    u32 cursor = 0;
    if (!regional_flag_sequence(glyph)) {
        copy_bytes(suggestion->label_storage + cursor, glyph.data, glyph.bytes);
        cursor += glyph.bytes;
        suggestion->label_storage[cursor++] = ' ';
        suggestion->label_storage[cursor++] = ' ';
    }
    suggestion->label_storage[cursor++] = ':';
    copy_bytes(suggestion->label_storage + cursor, name.data, name.bytes);
    cursor += name.bytes;
    suggestion->label_storage[cursor++] = ':';
    suggestion->label = Utf8View{suggestion->label_storage, cursor};
    suggestion->label_bytes = static_cast<u16>(cursor);
    return true;
}

// Mention candidates are filtered with an ordered subsequence match.
bool subsequence_folded_ascii(Utf8View candidate, Utf8View query) noexcept {
    u32 matched = 0;
    for (u32 index = 0; index < candidate.bytes && matched < query.bytes; ++index)
        if (ascii_lower(static_cast<u8>(candidate.data[index])) ==
            ascii_lower(static_cast<u8>(query.data[matched])))
            ++matched;
    return matched == query.bytes;
}

bool starts_with_folded_ascii(Utf8View candidate, Utf8View query) noexcept {
    if (query.bytes > candidate.bytes)
        return false;
    for (u32 index = 0; index < query.bytes; ++index)
        if (ascii_lower(static_cast<u8>(candidate.data[index])) != ascii_lower(static_cast<u8>(query.data[index])))
            return false;
    return true;
}

// Ranking in the style of the match-sorter library, for role results:
// case-sensitive equal 7, equal 6, starts with 5, word starts with 4,
// contains 3, acronym 2, then 1 plus closeness for in-order letters; zero is
// no match. Fixed point, kMatchRankUnit per ranking.
constexpr u32 kMatchRankUnit = 1u << 20;
u32 match_sorter_rank(Utf8View name, Utf8View query) noexcept {
    if (query.bytes > name.bytes)
        return 0;
    if (query.bytes == name.bytes && (!query.bytes || bytes_equal(name.data, query.data, query.bytes)))
        return 7 * kMatchRankUnit;
    if (query.bytes == name.bytes && starts_with_folded_ascii(name, query))
        return 6 * kMatchRankUnit;
    if (starts_with_folded_ascii(name, query))
        return 5 * kMatchRankUnit;
    for (u32 start = 1; start + query.bytes <= name.bytes; ++start)
        if (name.data[start - 1] == ' ' && starts_with_folded_ascii({name.data + start, name.bytes - start}, query))
            return 4 * kMatchRankUnit;
    if (contains_folded_ascii(name, query))
        return 3 * kMatchRankUnit;
    if (query.bytes == 1)
        return 0;
    // First letters of words split at spaces, then at hyphens.
    char acronym[kComposerAutocompleteLabelBytes];
    u32 letters = 0;
    for (u32 index = 0; index < name.bytes && letters < sizeof(acronym); ++index)
        if (name.data[index] != ' ' && name.data[index] != '-' &&
            (index == 0 || name.data[index - 1] == ' ' || name.data[index - 1] == '-'))
            acronym[letters++] = name.data[index];
    if (contains_folded_ascii({acronym, letters}, query))
        return 2 * kMatchRankUnit;
    // Closeness: 1 + in-order share / spread from the first match.
    u32 first = 0, at = 0, matched = 0;
    for (u32 index = 0; index < query.bytes; ++index) {
        while (at < name.bytes &&
               ascii_lower(static_cast<u8>(name.data[at])) != ascii_lower(static_cast<u8>(query.data[index])))
            ++at;
        if (at == name.bytes)
            return 0;
        ++at;
        ++matched;
        if (index == 0)
            first = at;
    }
    const u32 spread = at > first ? at - first : 1;
    return kMatchRankUnit + kMatchRankUnit * matched / query.bytes / spread;
}

// localeCompare for role names: letters case-insensitively, then bytes.
i32 compare_names(Utf8View left, Utf8View right) noexcept {
    const u32 shared = minimum(left.bytes, right.bytes);
    for (u32 index = 0; index < shared; ++index) {
        const u8 a = ascii_lower(static_cast<u8>(left.data[index])), b = ascii_lower(static_cast<u8>(right.data[index]));
        if (a != b)
            return a < b ? -1 : 1;
    }
    if (left.bytes != right.bytes)
        return left.bytes < right.bytes ? -1 : 1;
    for (u32 index = 0; index < shared; ++index)
        if (left.data[index] != right.data[index])
            return static_cast<u8>(left.data[index]) < static_cast<u8>(right.data[index]) ? -1 : 1;
    return 0;
}

struct RoleCandidate {
    const RoleRecord* role;
    Utf8View name;
    u32 rank;
};

bool role_before(const RoleCandidate& left, const RoleCandidate& right) noexcept {
    if (left.rank != right.rank)
        return left.rank > right.rank;
    if (const i32 order = compare_names(left.name, right.name))
        return order < 0;
    return left.role->id < right.role->id;
}

ComposerAutocompleteSuggestion* append(ComposerAutocomplete* autocomplete, Utf8View label) noexcept {
    if (autocomplete->count >= kComposerAutocompleteCapacity || !label.data ||
        label.bytes == 0) {
        return nullptr;
    }
    ComposerAutocompleteSuggestion* suggestion = &autocomplete->suggestions[autocomplete->count++];
    reset_value(suggestion);
    if (label.bytes > sizeof(suggestion->label_storage)) { --autocomplete->count; return nullptr; }
    copy_bytes(suggestion->label_storage, label.data, label.bytes);
    suggestion->label = {suggestion->label_storage, label.bytes};
    suggestion->insertion_bytes = 0;
    suggestion->label_bytes = 0;
    suggestion->item_kind = autocomplete->kind;
    return suggestion;
}

bool append_global(ComposerAutocomplete* autocomplete, Utf8View text, Utf8View description) noexcept {
    ComposerAutocompleteSuggestion* suggestion = append(autocomplete, text);
    if (!suggestion)
        return false;
    if (text.bytes + 1u > sizeof(suggestion->insertion)) {
        --autocomplete->count;
        return false;
    }
    suggestion->item_kind = ComposerAutocompleteKind::global_mention;
    suggestion->description = description;
    copy_bytes(suggestion->insertion, text.data, text.bytes);
    suggestion->insertion[text.bytes] = ' ';
    suggestion->insertion_bytes = static_cast<u16>(text.bytes + 1);
    return true;
}

bool append_role(ComposerAutocomplete* autocomplete, const RoleCandidate& candidate) noexcept {
    char label[kComposerAutocompleteLabelBytes];
    u32 bytes = minimum(candidate.name.bytes, static_cast<u32>(sizeof(label) - 1));
    while (bytes < candidate.name.bytes && bytes &&
           (static_cast<u8>(candidate.name.data[bytes]) & 0xC0u) == 0x80u)
        --bytes;
    label[0] = '@';
    copy_bytes(label + 1, candidate.name.data, bytes);
    ComposerAutocompleteSuggestion* suggestion = append(autocomplete, {label, bytes + 1});
    if (!suggestion)
        return false;
    suggestion->item_kind = ComposerAutocompleteKind::role;
    suggestion->role_id = candidate.role->id;
    suggestion->role_color = candidate.role->color & 0xFFFFFFu;
    suggestion->description =
        utf8_literal("Notify users with this role who have permission to view this channel.");
    char digits[20]{};
    const u32 digit_count = decimal_u64(candidate.role->id, Slice<char>{digits, 20});
    if (!digit_count || digit_count + 5u > sizeof(suggestion->insertion)) {
        --autocomplete->count;
        return false;
    }
    u32 cursor = 0;
    suggestion->insertion[cursor++] = '<';
    suggestion->insertion[cursor++] = '@';
    suggestion->insertion[cursor++] = '&';
    copy_bytes(suggestion->insertion + cursor, digits, digit_count);
    cursor += digit_count;
    suggestion->insertion[cursor++] = '>';
    suggestion->insertion[cursor++] = ' ';
    suggestion->insertion_bytes = static_cast<u16>(cursor);
    return true;
}

// Users come first; roles fill the rows left of the ten and the globals take
// only rows still free, but list between users and roles.
void append_mention_extras(ComposerAutocomplete* autocomplete, const MessageComposer& composer,
                           const ChatStateStore& state, Utf8View query) noexcept {
    const ChannelRecord* channel =
        composer.channel_id ? chat_state_store_find_channel(state, composer.channel_id) : nullptr;
    const u64 guild_id = channel ? channel->guild_id : 0;
    if (!guild_id || autocomplete->count >= kMentionAutocompleteLimit)
        return;
    const bool mention_everyone = (channel->actions & channel_action_mention_everyone) != 0;
    RoleCandidate roles[kMentionAutocompleteLimit]{};
    const u32 role_room = kMentionAutocompleteLimit - autocomplete->count;
    u32 role_count = 0;
    for (u32 index = 0; state.roles && index < state.role_count; ++index) {
        const RoleRecord& role = state.roles[index];
        if (role.guild_id != guild_id || role.id == guild_id || !(role.mentionable || mention_everyone))
            continue;
        const Utf8View name = chat_state_store_role_name(state, role);
        if (!name.data || !name.bytes || !subsequence_folded_ascii(name, query))
            continue;
        const RoleCandidate candidate{&role, name, match_sorter_rank(name, query)};
        if (!candidate.rank && query.bytes)
            continue;
        u32 slot = role_count;
        while (slot && role_before(candidate, roles[slot - 1]))
            --slot;
        if (slot >= role_room)
            continue;
        for (u32 move = minimum(role_count, role_room - 1); move > slot; --move)
            roles[move] = roles[move - 1];
        roles[slot] = candidate;
        role_count = minimum(role_count + 1, role_room);
    }
    u32 total = autocomplete->count + role_count;
    const bool everyone = mention_everyone && total < kMentionAutocompleteLimit &&
                          subsequence_folded_ascii(utf8_literal("everyone"), query);
    total += everyone ? 1u : 0u;
    const bool here = mention_everyone && total < kMentionAutocompleteLimit &&
                      subsequence_folded_ascii(utf8_literal("here"), query);
    if (everyone)
        append_global(autocomplete, utf8_literal("@everyone"),
                      utf8_literal("Notify everyone who has permission to view this channel."));
    if (here)
        append_global(autocomplete, utf8_literal("@here"),
                      utf8_literal("Notify everyone online who has permission to view this channel."));
    for (u32 index = 0; index < role_count; ++index)
        append_role(autocomplete, roles[index]);
}

// User ranking: a name that starts with the query outranks an in-order letter
// match; at most 50 of the latter are kept to fill rows prefixes leave. Ties
// keep the member cache's order, which `order` approximates (member list
// position, then discovery).
constexpr u32 kMentionFuzzyMaximum = 50;
// Recent authors come from at most this many newest loaded messages.
constexpr u32 kMentionAuthorScanMaximum = 2'048;

struct UserCandidate {
    u64 user_id;
    u32 order;
    GuildMemberStatus status;
    Utf8View nickname;
};

struct UserRanking {
    UserCandidate prefix[kMentionAutocompleteLimit];
    UserCandidate fuzzy[kMentionFuzzyMaximum];
    u32 prefix_count;
    u32 fuzzy_count;

    bool seen(u64 user_id) const noexcept {
        for (u32 i = 0; i < prefix_count; ++i)
            if (prefix[i].user_id == user_id)
                return true;
        for (u32 i = 0; i < fuzzy_count; ++i)
            if (fuzzy[i].user_id == user_id)
                return true;
        return false;
    }
    // Keeps the `capacity` lowest orders, sorted.
    static void insert(UserCandidate* bucket, u32* count, u32 capacity, const UserCandidate& candidate) noexcept {
        u32 slot = *count;
        while (slot && candidate.order < bucket[slot - 1].order)
            --slot;
        if (slot >= capacity)
            return;
        for (u32 move = minimum(*count, capacity - 1); move > slot; --move)
            bucket[move] = bucket[move - 1];
        bucket[slot] = candidate;
        *count = minimum(*count + 1, capacity);
    }
    // 2: a name starts with the query, 1: in-order letters, 0: no match.
    u32 tier(const UserStore& users, const UserCandidate& candidate, Utf8View query) const noexcept {
        const UserRecord* user = user_store_find(users, candidate.user_id);
        if (!user || seen(candidate.user_id))
            return 0;
        const Utf8View names[3]{candidate.nickname, user_store_name(users, *user), user_store_username(users, *user)};
        u32 best = 0;
        for (const Utf8View name : names)
            if (name.bytes)
                best = maximum(best, starts_with_folded_ascii(name, query) ? 2u
                                     : subsequence_folded_ascii(name, query) ? 1u
                                                                             : 0u);
        return best;
    }
    void add(u32 rank, const UserCandidate& candidate) noexcept {
        if (rank == 2)
            insert(prefix, &prefix_count, kMentionAutocompleteLimit, candidate);
        else if (rank == 1)
            insert(fuzzy, &fuzzy_count, kMentionFuzzyMaximum, candidate);
    }
    void offer(const UserStore& users, const UserCandidate& candidate, Utf8View query) noexcept {
        add(tier(users, candidate, query), candidate);
    }
};

GuildMemberStatus listed_status(const MemberListView& members, u64 user_id, GuildMemberStatus fallback) noexcept {
    for (u32 index = 0; members.rows.data && index < members.count; ++index)
        if (members.rows[index].kind == MemberListRowKind::member && members.rows[index].user_id == user_id)
            return members.rows[index].status;
    return fallback;
}

bool append_user(ComposerAutocomplete* autocomplete, const UserStore& users, const MemberListView& members,
                 const UserCandidate& candidate) noexcept {
    const UserRecord* user = user_store_find(users, candidate.user_id);
    if (!user || autocomplete->count >= kMentionAutocompleteLimit)
        return false;
    const Utf8View name = user_store_name(users, *user), username = user_store_username(users, *user);
    const Utf8View label = candidate.nickname.bytes ? candidate.nickname : name.bytes ? name : username;
    ComposerAutocompleteSuggestion* suggestion = append(autocomplete, label);
    if (!suggestion)
        return false;
    const u32 username_bytes = minimum(username.bytes, static_cast<u32>(sizeof(suggestion->username_storage)));
    copy_bytes(suggestion->username_storage, username.data, username_bytes);
    suggestion->username = {suggestion->username_storage, username_bytes};
    suggestion->user_id = candidate.user_id;
    suggestion->status = listed_status(members, candidate.user_id, candidate.status);
    const auto hash = user_store_avatar_hash(*user);
    suggestion->avatar_variant = user_avatar_variant(hash);
    suggestion->animated_avatar = hash.bytes == 34;
    if (!set_snowflake_insertion(suggestion, '@', candidate.user_id)) {
        --autocomplete->count;
        return false;
    }
    return true;
}

// First index in the (guild_id, user_id) order at or after guild_id.
u32 guild_members_begin(const GuildMemberStore& store, u64 guild_id) noexcept {
    u32 first = 0, count = store.count;
    while (count) {
        const u32 step = count / 2;
        if (store.records[store.order[first + step]].guild_id < guild_id) {
            first += step + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first;
}

// Recent authors (empty query): the channel's loaded messages,
// newest first, one row per author who is a member, at most ten.
u32 recent_authors(const MessageStore& messages, const GuildMemberStore& members, u64 guild_id, u64 channel_id,
                   UserCandidate* output) noexcept {
    u32 count = 0;
    for (u32 index = messages.count, scanned = 0;
         index-- && scanned < kMentionAuthorScanMaximum && count < kMentionAutocompleteLimit; ++scanned) {
        const MessageRecord* record = message_store_resolve(messages, message_store_ordered_handle(messages, index));
        if (!record || record->channel_id != channel_id || !record->author_id)
            continue;
        bool seen = false;
        for (u32 i = 0; i < count && !seen; ++i)
            seen = output[i].user_id == record->author_id;
        const GuildMemberRecord* member = seen ? nullptr : guild_member_store_find(members, guild_id, record->author_id);
        if (member) {
            output[count] = {record->author_id, count, member->status, guild_member_store_nickname(*member)};
            ++count;
        }
    }
    return count;
}

void append_mention_users(ComposerAutocomplete* autocomplete, const MessageComposer& composer,
                          const MemberListView& members, const UserStore& users, const ChatStateStore& state,
                          const GuildMemberStore* member_store, const MessageStore* messages, Utf8View query) noexcept {
    const ChannelRecord* channel =
        composer.channel_id ? chat_state_store_find_channel(state, composer.channel_id) : nullptr;
    const u64 guild_id = channel ? channel->guild_id : 0;
    UserRanking ranking{};
    if (guild_id && member_store) {
        if (!query.bytes && messages) {
            UserCandidate authors[kMentionAutocompleteLimit]{};
            const u32 count = recent_authors(*messages, *member_store, guild_id, composer.channel_id, authors);
            for (u32 index = 0; index < count; ++index)
                append_user(autocomplete, users, members, authors[index]);
            if (autocomplete->count)
                return;
        }
        // Cached members who can view the channel (a thread's parent decides);
        // the gateway is asked for more as the query changes.
        autocomplete->member_query_guild_id = guild_id;
        const ChannelRecord* scope =
            channel_is_thread(channel->type) ? chat_state_store_find_channel(state, channel->parent_id) : channel;
        const bool permissions =
            chat_state_store_find_guild(state, guild_id) && scope && scope->guild_id == guild_id;
        for (u32 index = guild_members_begin(*member_store, guild_id), visit = 0;
             index < member_store->count && member_store->records[member_store->order[index]].guild_id == guild_id;
             ++index, ++visit) {
            const GuildMemberRecord& member = member_store->records[member_store->order[index]];
            if (!member.occupied || !member.user_id)
                continue;
            const UserCandidate candidate{member.user_id,
                                          member.list_index != ~0u ? member.list_index : 0x8000'0000u | visit,
                                          member.status, guild_member_store_nickname(member)};
            const u32 rank = ranking.tier(users, candidate, query);
            if (!rank)
                continue;
            if (permissions && guild_member_store_role_ids_known(member)) {
                const PermissionMember subject{member.user_id, guild_member_store_role_ids(*member_store, member),
                                               member.communication_disabled_until};
                if (!(chat_state_store_member_permissions(state, guild_id, composer.channel_id, subject, {}) &
                      kPermissionViewChannel))
                    continue;
            }
            ranking.add(rank, candidate);
        }
    } else if (channel && !guild_id && state.private_channels) {
        // Private channels: the recipients (their channel nicknames), then you.
        const PrivateChannelStore& store = *state.private_channels;
        const PrivateChannelRecord* record = private_channel_store_find(store, channel->id);
        const ConstSlice<u64> recipients =
            record ? private_channel_store_recipients(store, *record) : ConstSlice<u64>{};
        for (u32 index = 0; index < recipients.count; ++index)
            ranking.offer(users,
                          {recipients[index], index, GuildMemberStatus::unknown,
                           private_channel_store_nickname(store, channel->id, recipients[index])},
                          query);
        if (state.self_user_id)
            ranking.offer(users, {state.self_user_id, ~0u, GuildMemberStatus::unknown, {}}, query);
    }
    if (!(guild_id && member_store) && !ranking.prefix_count && !ranking.fuzzy_count) {
        // Without a known channel scope, the member list view's rows.
        for (u32 index = 0; members.rows.data && index < members.count; ++index) {
            const MemberListRow& row = members.rows[index];
            if (row.kind != MemberListRowKind::member || !row.user_id)
                continue;
            const Utf8View label =
                member_store ? member_list_row_label(row, *member_store, users, state) : Utf8View{};
            ranking.offer(users, {row.user_id, index, row.status, label}, query);
        }
    }
    for (u32 index = 0; index < ranking.prefix_count; ++index)
        append_user(autocomplete, users, members, ranking.prefix[index]);
    for (u32 index = 0; index < ranking.fuzzy_count && autocomplete->count < kMentionAutocompleteLimit; ++index)
        append_user(autocomplete, users, members, ranking.fuzzy[index]);
}

bool append_emoji(ComposerAutocomplete* autocomplete, Utf8View name, Utf8View glyph) noexcept {
    for (u32 i = 0; i < autocomplete->count; ++i) {
        const auto& existing = autocomplete->suggestions[i];
        if (!existing.emoji_id && existing.insertion_bytes == glyph.bytes + 1 &&
            emoji_catalog_name_starts_with({existing.insertion, existing.insertion_bytes}, glyph)) return true;
    }
    ComposerAutocompleteSuggestion* suggestion = append(autocomplete, name);
    if (!suggestion) {
        return false;
    }
    if (!set_emoji_label(suggestion, name, glyph) || !set_emoji_insertion(suggestion, glyph)) {
        --autocomplete->count;
        return false;
    }
    return true;
}

} // namespace

bool composer_autocomplete_build(ComposerAutocomplete* autocomplete, const MessageComposer& composer,
                                 const MemberListView& members, const UserStore& users,
                                 const NavigationView& navigation,
                                 const ChatStateStore& state, const GuildMemberStore* member_store,
                                 const MessageStore* messages) noexcept {
    if (!autocomplete) {
        return false;
    }
    autocomplete->count = 0;
    autocomplete->token_begin = 0;
    autocomplete->token_end = 0;
    autocomplete->kind = ComposerAutocompleteKind::none;
    autocomplete->member_query_guild_id = 0;

    Utf8View token{};
    u32 token_begin = 0;
    if (!active_token(composer, &token, &token_begin) || !token.data || token.bytes == 0) {
        return true;
    }

    const char marker = token.data[0];
    const Utf8View query{token.data + 1, token.bytes - 1};
    autocomplete->token_begin = token_begin;
    autocomplete->token_end = composer.caret;

    if (marker == '@') {
        if (!ensure_suggestion_storage(autocomplete)) {
            return false;
        }
        autocomplete->kind = ComposerAutocompleteKind::member;
        append_mention_users(autocomplete, composer, members, users, state, member_store, messages, query);
        append_mention_extras(autocomplete, composer, state, query);
    } else if (marker == '#') {
        if (!ensure_suggestion_storage(autocomplete)) {
            return false;
        }
        autocomplete->kind = ComposerAutocompleteKind::channel;
        for (u32 row_index = 0;
             row_index < navigation.channel_count && autocomplete->count < kComposerAutocompleteCapacity;
             ++row_index) {
            const ChannelViewRow& row = navigation.channel_rows[row_index];
            if (!row.selectable || row.id == 0) {
                continue;
            }
            const ChannelRecord* channel = chat_state_store_find_channel(state, row.id);
            const Utf8View name = channel ? chat_state_store_channel_name(state, *channel) : Utf8View{};
            if (!contains_folded_ascii(name, query)) {
                continue;
            }
            ComposerAutocompleteSuggestion* suggestion = append(autocomplete, name);
            if (!suggestion) {
                continue;
            }
            if (!set_snowflake_insertion(suggestion, '#', row.id)) {
                --autocomplete->count;
            }
        }
    } else if (marker == ':' && query.bytes >= 2) {
        if (!ensure_suggestion_storage(autocomplete)) {
            return false;
        }
        autocomplete->kind = ComposerAutocompleteKind::emoji;
        const ConstSlice<EmojiCatalogEntry> entries = unicode_emoji_catalog();
        const auto aliases = emoji_shortcodes();
        // Pass 0 takes prefix matches; pass 1 adds substring matches after them.
        for (u32 pass = 0; pass < 2 && autocomplete->count < kComposerAutocompleteCapacity; ++pass) {
            for (u32 i = 0; i < state.emoji_count && autocomplete->count < kComposerAutocompleteCapacity; ++i) {
                const auto& emoji = state.emojis[i];
                if (emoji.guild_id != state.selected_guild_id || !chat_state_store_emoji_usable(state, emoji)) continue;
                const auto name = chat_state_store_emoji_name(emoji);
                const bool prefix = emoji_catalog_name_starts_with(name, query);
                if (!(pass == 0 ? prefix : !prefix && emoji_catalog_name_contains(name, query))) continue;
                auto* suggestion = append(autocomplete, name);
                if (!suggestion) continue;
                suggestion->emoji_id = emoji.id;
                suggestion->animated_emoji = (emoji.flags & custom_emoji_animated) != 0;
                u32 used = 0;
                suggestion->insertion[used++] = '<';
                if (suggestion->animated_emoji) suggestion->insertion[used++] = 'a';
                suggestion->insertion[used++] = ':';
                copy_bytes(suggestion->insertion + used, name.data, name.bytes); used += name.bytes;
                suggestion->insertion[used++] = ':';
                used += decimal_u64(emoji.id, {suggestion->insertion + used, kComposerAutocompleteInsertionBytes - used - 2});
                suggestion->insertion[used++] = '>'; suggestion->insertion[used++] = ' ';
                suggestion->insertion_bytes = static_cast<u16>(used);
            }
            for (u32 alias_index = 0; alias_index < aliases.count; ++alias_index) {
                const auto& alias = aliases[alias_index];
                if (autocomplete->count >= kComposerAutocompleteCapacity) break;
                const auto name = alias.name();
                const bool prefix = emoji_catalog_name_starts_with(name, query);
                if (pass == 0 ? prefix : !prefix && emoji_catalog_name_contains(name, query))
                    append_emoji(autocomplete, name, alias.glyph());
            }
            for (u32 entry = 0;
                 entry < entries.count && autocomplete->count < kComposerAutocompleteCapacity; ++entry) {
                const bool prefix = emoji_catalog_name_starts_with(entries[entry].name(), query);
                const bool match =
                    pass == 0 ? prefix
                              : !prefix && emoji_catalog_name_contains(entries[entry].name(), query);
                if (match) {
                    append_emoji(autocomplete, entries[entry].name(), entries[entry].glyph());
                }
            }
        }
    }

    if (autocomplete->count == 0) {
        autocomplete->kind = ComposerAutocompleteKind::none;
    }
    return true;
}

void composer_autocomplete_release(ComposerAutocomplete* autocomplete) noexcept {
    if (!autocomplete) {
        return;
    }
    arena_release(&autocomplete->storage);
    reset_value<ComposerAutocomplete>(autocomplete);
}

} // namespace maicord
