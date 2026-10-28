#include "maicord/app/quick_switcher_controller.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/friend_search.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/relationship_store.hpp"

namespace maicord {
namespace {
QuickSwitcherSourceStamp stamp(QuickSwitcherSources sources, QuickSwitcherMode mode = QuickSwitcherMode::navigation) noexcept {
    QuickSwitcherSourceStamp value{}; value.sources = sources;
    if (sources.state) {
        const auto& s = *sources.state;
        if (mode == QuickSwitcherMode::navigation) {
            value.selected_channel_id = s.selected_channel_id; value.selected_guild_id = s.selected_guild_id;
            value.activity = s.activity_mutations; value.selection = s.selection_revision;
        }
        value.self_user_id = s.self_user_id;
        value.guilds = s.guild_mutations; value.channels = s.channel_mutations;
        value.permissions = s.permission_mutations;
        value.private_source = s.private_channels;
        value.private_channels = s.private_channels ? s.private_channels->revision : 0;
    }
    if (sources.users) { value.users = sources.users->mutations; value.user_revision = sources.users->revision; }
    value.relationships = sources.relationships ? sources.relationships->revision : 0;
    return value;
}
bool same_stamp(const QuickSwitcherSourceStamp& a, const QuickSwitcherSourceStamp& b) noexcept {
    return a.sources.state == b.sources.state && a.sources.users == b.sources.users &&
        a.sources.relationships == b.sources.relationships && a.private_source == b.private_source && a.selected_channel_id == b.selected_channel_id &&
        a.selected_guild_id == b.selected_guild_id && a.self_user_id == b.self_user_id &&
        a.guilds == b.guilds && a.channels == b.channels && a.permissions == b.permissions &&
        a.activity == b.activity && a.selection == b.selection && a.users == b.users &&
        a.user_revision == b.user_revision && a.private_channels == b.private_channels && a.relationships == b.relationships;
}
bool valid_scope(QuickSwitcherScope scope, QuickSwitcherSources sources) noexcept {
    return scope.account_serial && scope.profile_key && scope.self_user_id && sources.state &&
        sources.state->self_user_id == scope.self_user_id;
}
// Text destinations; voice channels stay join entries even though they carry chat.
bool selectable(const ChatStateStore& state, const ChannelRecord& c) noexcept {
    if (!c.occupied || !channel_is_message_capable(c.type) || channel_is_voice_chat(c.type) ||
        (c.flags & kChannelObfuscatedFlag)) return false;
    if (!c.guild_id) return c.type == 1 || c.type == 3;
    const auto* guild = chat_state_store_find_guild(state, c.guild_id);
    return guild && guild->occupied && !guild->unavailable && c.permissions_resolved &&
        (c.actions & channel_action_view) && chat_state_store_channel_visible(state, c);
}
// Guild voice channels the user may join (Stage channels excluded).
bool joinable_voice(const ChatStateStore& state, const ChannelRecord& c) noexcept {
    if (!c.occupied || c.type != 2 || !c.guild_id || (c.flags & kChannelObfuscatedFlag)) return false;
    const auto* guild = chat_state_store_find_guild(state, c.guild_id);
    return guild && guild->occupied && !guild->unavailable && c.permissions_resolved &&
        (c.actions & channel_action_view) && (c.actions & channel_action_connect) &&
        chat_state_store_channel_visible(state, c);
}
bool allowed_user(QuickSwitcherSources sources, u64 id) noexcept {
    if (!sources.users || !sources.relationships || id == sources.state->self_user_id) return false;
    const auto* relation = relationship_store_find(*sources.relationships, id);
    return relation && relation->type == RelationshipType::friend_ && !relation->ignored && !relation->spam &&
        user_store_find(*sources.users, id);
}
u16 copy_text(char* output, u32 capacity, const Utf8View& text) noexcept {
    return static_cast<u16>(utf8_copy_truncated(output, capacity, text));
}
u32 field_score(const Utf8View& query, const Utf8View& field) noexcept {
    if (!field.bytes) return 0;
    if (ascii_equal_ignore_case(query, field)) return 1000;
    if (query.bytes <= field.bytes && ascii_equal_ignore_case(query, {field.data, query.bytes})) return 850;
    const auto match = friend_search(query, {}, {}, field);
    if (match.ok() && match.matched) return 650;
    // ASCII subsequences keep abbreviations useful; Unicode uses the shared full lowercase matcher.
    u32 position = 0, gaps = 0;
    for (u32 i = 0; i < query.bytes; ++i) {
        if (static_cast<u8>(query.data[i]) >= 128) return 0;
        while (position < field.bytes && ascii_lower(field.data[position]) != ascii_lower(query.data[i])) { ++position; ++gaps; }
        if (position == field.bytes) return 0;
        ++position;
    }
    return 300 - minimum(gaps, 100u);
}
u32 match_score(const Utf8View& query, const Utf8View& label, const Utf8View& detail, const Utf8View& alternate, u64 id) noexcept {
    if (!query.bytes) return 100;
    u64 numeric = 0; bool digits = true;
    for (u32 i = 0; i < query.bytes; ++i) {
        const u8 c = static_cast<u8>(query.data[i]);
        if (c < '0' || c > '9' || numeric > (static_cast<u64>(-1) - (c - '0')) / 10) { digits = false; break; }
        numeric = numeric * 10 + c - '0';
    }
    if (digits && numeric == id) return 1200;
    u32 score = maximum(field_score(query, label), field_score(query, alternate));
    // Space-delimited terms may span channel and server/recipient fields.
    if (!score) {
        u32 position = 0, total = 0, terms = 0;
        while (position < query.bytes) {
            while (position < query.bytes && ascii_is_space(query.data[position])) ++position;
            const u32 first = position;
            while (position < query.bytes && !ascii_is_space(query.data[position])) ++position;
            if (first == position) break;
            const Utf8View term{query.data + first, position - first};
            const u32 part = maximum(maximum(field_score(term, label), field_score(term, alternate)), field_score(term, detail));
            if (!part) return 0;
            total += part; ++terms;
        }
        if (terms > 1) score = total / terms;
    }
    return score;
}
u64 peer(QuickSwitcherSources sources, const ChannelRecord& c) noexcept {
    if (c.type != 1 || !sources.state->private_channels) return 0;
    const auto& priv = *sources.state->private_channels;
    const auto* record = private_channel_store_find(priv, c.id);
    if (!record) return 0;
    const auto recipients = private_channel_store_recipients(priv, *record);
    for (u32 i = 0; i < recipients.count; ++i) if (recipients[i] != sources.state->self_user_id) return recipients[i];
    return 0;
}
void channel_row(QuickSwitcherSources sources, const ChannelRecord& c, QuickSwitcherRow* row, Utf8View* alternate) noexcept {
    const auto& state = *sources.state;
    row->target = {c.id, c.guild_id, c.id, peer(sources, c), c.generation,
        c.type == 1 ? QuickSwitcherKind::private_channel : c.type == 3 ? QuickSwitcherKind::group_private
        : c.type == 2 ? QuickSwitcherKind::voice_channel : QuickSwitcherKind::text_channel};
    auto name = chat_state_store_channel_name(state, c);
    if (c.guild_id) {
        const auto* guild = chat_state_store_find_guild(state, c.guild_id);
        row->detail_bytes = copy_text(row->detail, sizeof(row->detail), chat_state_store_guild_name(state, *guild));
    } else {
        row->detail_bytes = copy_text(row->detail, sizeof(row->detail), c.type == 1 ? utf8_literal("Direct message") : utf8_literal("Group message"));
        const auto* user = sources.users ? user_store_find(*sources.users, row->target.user_id) : nullptr;
        if (user) {
            *alternate = user_store_username(*sources.users, *user);
            const auto avatar = user_store_avatar_hash(*user);
            row->avatar_hash_bytes = static_cast<u8>(copy_text(row->avatar_hash, sizeof(row->avatar_hash), avatar));
        }
        if (!name.bytes && c.type == 1 && user) {
            const auto nickname = state.private_channels ? private_channel_store_nickname(*state.private_channels, c.id, user->id) : Utf8View{};
            name = nickname.bytes ? nickname : user_store_name(*sources.users, *user);
        }
        if (!name.bytes && c.type == 3 && state.private_channels) {
            const auto* priv = private_channel_store_find(*state.private_channels, c.id);
            if (priv) {
                const auto recipients = private_channel_store_recipients(*state.private_channels, *priv);
                for (u32 i = 0; i < recipients.count; ++i) {
                    if (recipients[i] == state.self_user_id) continue;
                    const auto* member = sources.users ? user_store_find(*sources.users, recipients[i]) : nullptr;
                    const auto nickname = private_channel_store_nickname(*state.private_channels, c.id, recipients[i]);
                    const auto display = nickname.bytes ? nickname : member ? user_store_name(*sources.users, *member) : Utf8View{};
                    if (!display.bytes) continue;
                    if (row->label_bytes && row->label_bytes + 2 < sizeof(row->label)) {
                        row->label[row->label_bytes++] = ','; row->label[row->label_bytes++] = ' ';
                    }
                    row->label_bytes += copy_text(row->label + row->label_bytes, sizeof(row->label) - row->label_bytes, display);
                    if (row->label_bytes == sizeof(row->label)) break;
                }
                row->avatar_hash_bytes = static_cast<u8>(copy_text(row->avatar_hash, sizeof(row->avatar_hash), {priv->icon, priv->icon_bytes}));
            }
        }
    }
    if (name.bytes) row->label_bytes = copy_text(row->label, sizeof(row->label), name);
    if (!row->label_bytes) row->label_bytes = copy_text(row->label, sizeof(row->label), c.guild_id ? utf8_literal("Channel") :
        c.type == 3 ? utf8_literal("Group message") : utf8_literal("Direct message"));
}
bool better(const QuickSwitcherRow& a, const QuickSwitcherRow& b) noexcept {
    if (a.score != b.score) return a.score > b.score;
    const auto order = friend_name_compare({a.label, a.label_bytes}, {b.label, b.label_bytes});
    if (order.ok() && order.order) return order.order < 0;
    if (a.target.id != b.target.id) return a.target.id < b.target.id;
    return static_cast<u8>(a.target.kind) < static_cast<u8>(b.target.kind);
}
// Rows are caller scratch: insert and consider may change score and target.
void insert(QuickSwitcherController* c, QuickSwitcherRow& row, u32 limit) noexcept {
    for (u32 i = 0; i < c->count; ++i) {
        if (c->rows[i].target.id == row.target.id ||
            (row.target.user_id && c->rows[i].target.user_id == row.target.user_id)) {
            // A friend alias can improve the score/label of an existing DM,
            // while activation keeps the already admitted channel route.
            if (row.target.kind == QuickSwitcherKind::user &&
                c->rows[i].target.kind == QuickSwitcherKind::private_channel) {
                row.target = c->rows[i].target;
                for (u32 j = 0; j < c->recent_count; ++j) if (row.target.channel_id == c->recent_channels[j]) {
                    row.score += 40 - j; row.recent = true; break;
                }
            }
            if (!better(row, c->rows[i])) return;
            copy_bytes(&c->rows[i], &c->rows[i + 1], (c->count - i - 1) * static_cast<u32>(sizeof(c->rows[0])));
            --c->count; break;
        }
    }
    u32 position = 0;
    bool first = false;
    // Results are already ranked. Avoid folding every tied name for an
    // outside-tail candidate, and reuse the first comparison for new leaders.
    if (c->count == limit) {
        first = better(row, c->rows[0]);
        if (!first) {
            if (!better(row, c->rows[c->count - 1])) {
                c->overflow = true;
                return;
            }
            position = 1;
        }
    }
    while (!first && position < c->count && !better(row, c->rows[position])) ++position;
    if (position >= limit) { c->overflow = true; return; }
    if (c->count < limit) ++c->count; else c->overflow = true;
    copy_bytes(&c->rows[position + 1], &c->rows[position], (c->count - 1 - position) * static_cast<u32>(sizeof(row)));
    copy_value(&c->rows[position], row);
}
void consider(QuickSwitcherController* c, QuickSwitcherRow& row, Utf8View query, Utf8View alternate, u32 limit) noexcept {
    ++c->stats.visited_candidates;
    row.score = match_score(query, {row.label, row.label_bytes}, {row.detail, row.detail_bytes}, alternate, row.target.id);
    if (!row.score) return;
    ++c->stats.matched_candidates;
    for (u32 i = 0; i < c->recent_count; ++i) if (row.target.channel_id == c->recent_channels[i]) {
        row.score += 40 - i; row.recent = true; break;
    }
    insert(c, row, limit);
}
bool target_current(QuickSwitcherSources sources, const QuickSwitcherTarget& target) noexcept {
    if (target.kind == QuickSwitcherKind::user) {
        const auto* user = sources.users ? user_store_find(*sources.users, target.user_id) : nullptr;
        return user && user->generation == target.generation && allowed_user(sources, target.user_id);
    }
    if (target.kind == QuickSwitcherKind::guild) {
        const auto* guild = chat_state_store_find_guild(*sources.state, target.guild_id);
        return guild && guild->occupied && !guild->unavailable && guild->generation == target.generation;
    }
    const auto* channel = chat_state_store_find_channel(*sources.state, target.channel_id);
    return channel && channel->generation == target.generation && channel->guild_id == target.guild_id &&
        (target.kind == QuickSwitcherKind::voice_channel ? joinable_voice(*sources.state, *channel) : selectable(*sources.state, *channel));
}
void resolve_cached_friend_routes(QuickSwitcherController* c, QuickSwitcherSources sources) noexcept {
    const auto* priv = sources.state->private_channels;
    if (!priv) return;
    u32 pending[kQuickSwitcherMaximumResults], count = 0;
    for (u32 i = 0; i < c->count; ++i)
        if (c->rows[i].target.kind == QuickSwitcherKind::user) pending[count++] = i;
    bool changed = false;
    // Only retained results need this lookup: at most one hundred small IDs,
    // rather than a private-channel scan for every friend in the account.
    for (u32 i = 0; i < priv->count && count; ++i) {
        const auto& record = priv->records[i];
        const auto recipients = private_channel_store_recipients(*priv, record);
        u64 user_id = 0;
        for (u32 j = 0; j < recipients.count; ++j) {
            if (recipients[j] == c->scope.self_user_id) continue;
            if (user_id) { user_id = 0; break; }
            user_id = recipients[j];
        }
        if (!user_id) continue;
        for (u32 j = 0; j < count; ++j) {
            auto& row = c->rows[pending[j]];
            if (row.target.user_id != user_id) continue;
            const auto* channel = chat_state_store_find_channel(*sources.state, record.id);
            if (!channel || channel->type != 1 || !selectable(*sources.state, *channel)) break;
            row.target = {channel->id, 0, channel->id, user_id, channel->generation, QuickSwitcherKind::private_channel};
            for (u32 k = 0; k < c->recent_count; ++k) if (channel->id == c->recent_channels[k]) {
                row.score += 40 - k; row.recent = true; break;
            }
            pending[j] = pending[--count]; changed = true; break;
        }
    }
    // Keep score/name/identity ordering deterministic after changing route IDs.
    if (changed) for (u32 i = 1; i < c->count; ++i) {
        u32 position = i;
        while (position && better(c->rows[i], c->rows[position - 1])) --position;
        if (position == i) continue;
        QuickSwitcherRow row;
        copy_value(&row, c->rows[i]);
        copy_bytes(&c->rows[position + 1], &c->rows[position], (i - position) * static_cast<u32>(sizeof(row)));
        copy_value(&c->rows[position], row);
    }
}
} // namespace

bool quick_switcher_channel_forwardable(QuickSwitcherSources sources, const ChannelRecord& channel,
    bool source_age_restricted) noexcept {
    if (!sources.state || !selectable(*sources.state, channel) || channel.thread_archived) return false;
    if (source_age_restricted && !chat_state_store_channel_age_restricted(*sources.state, channel)) return false;
    if (channel.guild_id) return (channel.actions & channel_action_send_messages) != 0;
    if (channel.type == 1 && sources.relationships) {
        const auto id = peer(sources, channel);
        const auto* relation = relationship_store_find(*sources.relationships, id);
        if (relation && (relation->type == RelationshipType::blocked || relation->ignored || relation->spam)) return false;
    }
    return true;
}

bool quick_switcher_synchronize(QuickSwitcherController* c, QuickSwitcherScope scope,
    QuickSwitcherSources sources, Utf8View query, QuickSwitcherMode mode, bool forwarding_age_restricted) noexcept {
    if (!c) return false;
    const auto versions = stamp(sources, mode);
    const bool valid = valid_scope(scope, sources) && query.bytes <= kQuickSwitcherQueryMaximumBytes &&
        (!query.bytes || query.data) && utf8_validate(query).valid;
    if (c->initialized && c->mode == mode && c->forwarding_age_restricted == forwarding_age_restricted &&
        valid && quick_switcher_scope_equal(c->scope, scope) && same_stamp(c->stamp, versions) &&
        c->query_bytes == query.bytes && bytes_equal(c->query, query.data, query.bytes)) return false;
    if (!quick_switcher_scope_equal(c->scope, scope)) c->recent_count = 0;
    c->scope = scope; c->stamp = versions; c->initialized = true;
    c->mode = mode;
    c->forwarding_age_restricted = forwarding_age_restricted;
    c->count = 0; c->overflow = false; reset_value(&c->stats); c->phase = QuickSwitcherPhase::ready;
    c->query_bytes = valid ? static_cast<u16>(query.bytes) : 0;
    if (valid) copy_bytes(c->query, query.data, query.bytes);
    ++c->revision; if (!c->revision) ++c->revision;
    ++c->rebuild_count;
    if (!valid) { c->phase = QuickSwitcherPhase::invalid_query; return true; }
    u32 first = 0, last = query.bytes;
    while (first < last && ascii_is_space(query.data[first])) ++first;
    while (last > first && ascii_is_space(query.data[last - 1])) --last;
    char prefix = first < last ? query.data[first] : 0;

    if (prefix == '*' || prefix == '#' || prefix == '@' || prefix == '!') {
        ++first; if (prefix == '@' && first < last && query.data[first] == '@') ++first;
        while (first < last && ascii_is_space(query.data[first])) ++first;
    } else prefix = 0;
    const Utf8View match{query.data ? query.data + first : nullptr, last - first};
    const auto& state = *sources.state;
    if (mode == QuickSwitcherMode::navigation && !prefix && !match.bytes && c->recent_count) {
        for (u32 i = 0; i < c->recent_count; ++i) {
            const auto* channel = chat_state_store_find_channel(state, c->recent_channels[i]);
            if (!channel || channel->id == state.selected_channel_id || !selectable(state, *channel)) continue;
            QuickSwitcherRow row{}; Utf8View alternate{}; channel_row(sources, *channel, &row, &alternate);
            row.score = 1000 - i; row.recent = true; insert(c, row, 7);
            ++c->stats.visited_candidates; ++c->stats.matched_candidates;
        }
        if (c->count) return true;
    }
    const bool friends = mode == QuickSwitcherMode::friends;
    const u32 limit = prefix || mode != QuickSwitcherMode::navigation ? kQuickSwitcherMaximumResults : 5;
    if (prefix != '*' && !friends) {
        for (u32 i = 0; i < state.channel_count; ++i) {
            const auto& channel = state.channels[state.channel_order[i]];
            // Voice channels are joined: "!" lists only them, plain searches include them.
            const bool voice = mode == QuickSwitcherMode::navigation && (prefix == '!' || (!prefix && match.bytes)) &&
                joinable_voice(state, channel);
            if (prefix == '!' && !voice) continue;
            if (!voice && (mode == QuickSwitcherMode::forwarding ? !quick_switcher_channel_forwardable(sources, channel, forwarding_age_restricted) :
                (channel.id == state.selected_channel_id || !selectable(state, channel)))) continue;
            if (prefix == '#' && !channel.guild_id) continue;
            if (prefix == '@' && channel.guild_id) continue;
            QuickSwitcherRow row{}; Utf8View alternate{}; channel_row(sources, channel, &row, &alternate);
            consider(c, row, match, alternate, limit);
        }
    }
    if (mode == QuickSwitcherMode::navigation && (prefix == '*' || (!prefix && match.bytes))) {
        for (u32 i = 0; i < state.guild_count; ++i) {
            const auto& guild = state.guilds[state.guild_order[i]];
            if (!guild.occupied || guild.unavailable || guild.id == state.selected_guild_id) continue;
            QuickSwitcherRow row{};
            row.target = {guild.id, guild.id, 0, 0, guild.generation, QuickSwitcherKind::guild};
            row.label_bytes = copy_text(row.label, sizeof(row.label), chat_state_store_guild_name(state, guild));
            row.detail_bytes = copy_text(row.detail, sizeof(row.detail), utf8_literal("Server"));
            row.avatar_hash_bytes = static_cast<u8>(copy_text(row.avatar_hash, sizeof(row.avatar_hash), chat_state_store_guild_icon_hash(guild)));
            consider(c, row, match, {}, limit);
        }
    }
    if ((friends || (mode == QuickSwitcherMode::navigation && (prefix == '@' || (!prefix && match.bytes)))) &&
        sources.relationships && sources.users) {
        for (u32 i = 0; i < sources.relationships->count; ++i) {
            const auto& relation = sources.relationships->records[i];
            if (!allowed_user(sources, relation.user_id)) continue;
            const auto* user = user_store_find(*sources.users, relation.user_id);
            QuickSwitcherRow row{};
            row.target = {user->id, 0, 0, user->id, user->generation, QuickSwitcherKind::user};
            row.label_bytes = copy_text(row.label, sizeof(row.label), relationship_display_name(relation, *sources.users));
            row.detail_bytes = copy_text(row.detail, sizeof(row.detail),
                friends ? user_store_username(*sources.users, *user) : utf8_literal("Friend"));
            row.avatar_hash_bytes = static_cast<u8>(copy_text(row.avatar_hash, sizeof(row.avatar_hash), user_store_avatar_hash(*user)));
            consider(c, row, match, user_store_username(*sources.users, *user), limit);
        }
        if (!friends) resolve_cached_friend_routes(c, sources);
    }
    return true;
}
bool quick_switcher_visit(QuickSwitcherController* c, QuickSwitcherScope scope,
    QuickSwitcherSources sources, u64 channel_id) noexcept {
    if (!c || !valid_scope(scope, sources)) return false;
    const auto* channel = chat_state_store_find_channel(*sources.state, channel_id);
    if (!channel || !selectable(*sources.state, *channel)) return false;
    if (!quick_switcher_scope_equal(c->scope, scope)) {
        c->scope = scope; c->recent_count = 0; c->count = 0; c->initialized = false;
    }
    if (c->recent_count && c->recent_channels[0] == channel_id) return false;
    u32 found = c->recent_count;
    for (u32 i = 0; i < c->recent_count; ++i) if (c->recent_channels[i] == channel_id) { found = i; break; }
    if (found == c->recent_count && c->recent_count < kQuickSwitcherRecentMaximum) ++c->recent_count;
    for (u32 i = minimum(found, kQuickSwitcherRecentMaximum - 1); i; --i) c->recent_channels[i] = c->recent_channels[i - 1];
    c->recent_channels[0] = channel_id;
    // Force the next synchronization without publishing a partial result set.
    c->initialized = false;
    return true;
}
bool quick_switcher_current(const QuickSwitcherController& c, QuickSwitcherScope scope,
    QuickSwitcherSources sources, u32 revision) noexcept {
    return c.initialized && revision == c.revision &&
        valid_scope(scope, sources) && quick_switcher_scope_equal(c.scope, scope) && same_stamp(c.stamp, stamp(sources, c.mode));
}
bool quick_switcher_resolve(const QuickSwitcherController& c, QuickSwitcherScope scope,
    QuickSwitcherSources sources, u32 index, u32 revision, QuickSwitcherTarget* target) noexcept {
    if (target) reset_value(target);
    if (!target || index >= c.count || !quick_switcher_current(c, scope, sources, revision) || !target_current(sources, c.rows[index].target)) return false;
    *target = c.rows[index].target; return true;
}
} // namespace maicord
