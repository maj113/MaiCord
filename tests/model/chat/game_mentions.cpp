#include "maicord/model/chat/game_store.hpp"
#include "maicord/protocol/guild/admin.hpp"
#include "maicord/protocol/guild/game_profile.hpp"
#include "maicord/ui/chat/messages/markdown.hpp"
#include "maicord/core/memory.hpp"
#include "../../support/test_output.hpp"

// Game mentions (<@$id>): the mention syntax, the game cache and search, the composer's @game token, the markdown
// chip and plain text, the requests' targets and the game profile's details.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool contains(Utf8View text, Utf8View part) noexcept {
    for (u32 at = 0; part.bytes <= text.bytes && at <= text.bytes - part.bytes; ++at)
        if (bytes_equal(text.data + at, part.data, part.bytes))
            return true;
    return false;
}

void test_syntax(Check check) noexcept {
    char out[32];
    check(game_mention_write(77, {out, sizeof(out)}) == 6 && bytes_equal(out, "<@$77>", 6) &&
              !game_mention_write(77, {out, 5}) && !game_mention_write(0, {out, sizeof(out)}),
          "game mentions: <@$id> writes when it fits");
}

struct Store {
    GameRecord records[4]{};
    GameSearchEntry searches[2]{};
    GameStore store{};
    bool initialize() noexcept {
        return game_store_initialize(&store, {records, 4}, {searches, 2});
    }
};

GuildAdminGame game(u64 id, const char* name) noexcept {
    GuildAdminGame g{};
    g.id = id;
    g.name_bytes = static_cast<u8>(c_string_length(name));
    copy_bytes(g.name, name, g.name_bytes);
    return g;
}

void test_store(Check check) noexcept {
    static Store s;
    s = Store{};
    check(s.initialize(), "game store: initializes over caller storage");
    auto& store = s.store;
    check(game_store_want(&store, 10, 0) && game_store_want(&store, 11, 0) && !game_store_want(&store, 10, 0) &&
              game_store_batch_due(store, 0),
          "game store: a new id is wanted once");
    u64 ids[kGameLookupBatchMaximum]{};
    check(game_store_take_batch(&store, {ids, kGameLookupBatchMaximum}, 0) == 2 && ids[0] == 10 && ids[1] == 11 &&
              !game_store_batch_due(store, 0) && game_store_find(store, 10)->state == GameState::pending,
          "game store: a batch takes the wanted ids");
    game_store_put(&store, game(10, "Starfall Tactics"), true, 0);
    game_store_settle(&store, {ids, 2}, true, 0);
    check(utf8_equal(game_store_name(store, 10), utf8_literal("Starfall Tactics")) &&
              game_store_find(store, 11)->state == GameState::unknown && !game_store_name(store, 11).bytes,
          "game store: a batch's games are known and the ids it leaves out unknown");
    check(game_store_want(&store, 12, 0) && game_store_take_batch(&store, {ids, 1}, 0) == 1, "game store: one more");
    game_store_settle(&store, {ids, 1}, false, 1'000);
    check(game_store_find(store, 12)->state == GameState::failed && !game_store_want(&store, 12, 2'000) &&
              game_store_want(&store, 12, 1'000 + kGameLookupFailureRetryMs),
          "game store: a failed batch is asked again after 15 s");
    game_store_take_batch(&store, {ids, 1}, 0);
    game_store_return(&store, {ids, 1});
    check(game_store_find(store, 12)->state == GameState::wanted, "game store: an unsent batch is wanted again");
    // A search result is not the game object: it is fetched once for the cover, and never replaces one.
    game_store_put(&store, game(13, "Moss Garden"), false, 0);
    check(game_store_want(&store, 13, 0) && !game_store_want(&store, 13, 0) &&
              game_store_find(store, 13)->upgrade == 1,
          "game store: a search result asks once for its game object");
    game_store_put(&store, game(10, "Other"), false, 0);
    check(utf8_equal(game_store_name(store, 10), utf8_literal("Starfall Tactics")),
          "game store: a search result never replaces a game object");
}

void test_search(Check check) noexcept {
    static Store s;
    s = Store{};
    s.initialize();
    auto& store = s.store;
    const u64 found[]{1, 484425151631589378ull, 2};
    check(game_store_search_excluded(484425151631589378ull) && !game_store_search_excluded(2),
          "game search: the official client's excluded ids are known");
    check(game_store_search_begin(&store, utf8_literal("star"), 0) &&
              game_store_search_find(store, utf8_literal("star"), 0)->pending &&
              !game_store_search_find(store, utf8_literal("star"), 0)->count &&
              !game_store_search_closest(store, utf8_literal("star"), 0) &&
              !game_store_search_begin(&store, utf8_literal("star"), 0),
          "game search: a pending query has no results yet and is not sent twice");
    game_store_search_finish(&store, utf8_literal("star"), {found, 3}, false, 0);
    const auto* entry = game_store_search_find(store, utf8_literal("star"), 0);
    check(entry && entry->count == 2 && entry->ids[0] == 1 && entry->ids[1] == 2,
          "game search: results drop the excluded ids");
    const auto* closest = game_store_search_closest(store, utf8_literal("starf"), 0);
    check(closest == entry && !game_store_search_begin(&store, utf8_literal("star"), 1'000),
          "game search: a longer query shows the cached prefix; a fresh query is not sent again");
    check(!game_store_search_find(store, utf8_literal("star"), kGameSearchStaleMs + 1),
          "game search: results go stale after an hour");
    check(game_store_search_begin(&store, utf8_literal("nothing"), 0), "game search: an empty search begins");
    game_store_search_finish(&store, utf8_literal("nothing"), {}, false, 0);
    check(game_store_search_suppressed(store, utf8_literal("nothingx"), 0) &&
              !game_store_search_suppressed(store, utf8_literal("starx"), 0),
          "game search: a query is not sent when a 7+ character prefix found nothing");
}

void test_token(Check check) noexcept {
    char draft[64];
    u32 used = 0;
    const auto put = [&](const char* text, u32 bytes) noexcept {
        copy_bytes(draft + used, text, bytes);
        used += bytes;
    };
    put("hi ", 3);
    put(kGameTokenHead, kGameTokenHeadBytes);
    put("star", 4);
    put(kGameTokenEnd, kGameTokenEndBytes);
    put(" ok", 3);
    const Utf8View text{draft, used};
    GameToken token{};
    check(game_token_at(text, 3, &token) && token.query_first == 3 + kGameTokenHeadBytes &&
              token.query_last == token.query_first + 4 && token.last == token.query_last + kGameTokenEndBytes,
          "game token: the head, query and end are found");
    GameToken around{};
    check(game_token_around(text, token.query_first + 2, &around) && around.first == 3 &&
              !game_token_around(text, 1, &around),
          "game token: the caret inside the query finds its token");
    char out[64];
    u32 bytes = 0, offset = token.query_last;
    check(game_tokens_rewrite(text, true, {out, sizeof(out)}, &bytes, &offset) &&
              utf8_equal({out, bytes}, utf8_literal("hi @star ok")) && offset == 3,
          "game token: an unpicked token is sent as @query");
    check(!game_tokens_rewrite(text, false, {out, sizeof(out)}, &bytes), "game token: a whole token needs no repair");
    // Backspace over the end mark leaves the head alone: the repair removes it with its "@game:".
    const Utf8View broken{draft, token.query_last};
    check(game_tokens_rewrite(broken, false, {out, sizeof(out)}, &bytes) &&
              utf8_equal({out, bytes}, utf8_literal("hi star")),
          "game token: a stray head goes with its @game:");
}

Utf8View resolve(void*, MessageMarkdownEntityKind kind, u64 id) noexcept {
    if (kind == MessageMarkdownEntityKind::game && id == 10)
        return utf8_literal("Starfall Tactics");
    if (kind == MessageMarkdownEntityKind::user && id == 5)
        return utf8_literal("Ada");
    if (kind == MessageMarkdownEntityKind::role && id == 6)
        return utf8_literal("Mods");
    return {};
}
bool resolve_channel(void*, u64, u64 channel, MessageMarkdownChannelTarget* target) noexcept {
    if (channel != 7)
        return false;
    *target = {};
    target->name = utf8_literal("general");
    target->flags = message_markdown_channel_known | message_markdown_channel_viewable |
                    message_markdown_channel_mentionable | message_markdown_channel_guild_known |
                    message_markdown_channel_same_guild;
    return true;
}

void test_markdown(Check check) noexcept {
    char output[512];
    u32 styles[512];
    TextStyleSpan spans[64];
    MessageMarkdownSemantic semantics[16];
    MessageMarkdownOptions options{};
    options.resolve_entity = resolve;
    const auto parse = [&](Utf8View source) noexcept {
        return message_markdown_parse(source, {output, sizeof(output)}, {styles, 512}, {spans, 64}, options,
                                      {semantics, 16});
    };
    auto result = parse(utf8_literal("<@$10> and <@$11>"));
    check(result.ok() && utf8_equal(result.text, utf8_literal("@Starfall Tactics and @unknown-game")) &&
              result.semantic_count == 2 && semantics[0].kind == MessageMarkdownSemanticKind::game_mention &&
              semantics[0].target == 10 && semantics[0].source_bytes == 6 && semantics[1].target == 11 &&
              (styles[0] & text_style_mention),
          "game markdown: plain surfaces read @name, an unknown game @unknown-game");
    options.resolve_channel = resolve_channel;
    result = parse(utf8_literal("<@5> <@$10> <@&6> <#7>"));
    u32 games = 0, icons = 0, users = 0, roles = 0, channels = 0;
    for (u32 i = 0; i < result.semantic_count; ++i) {
        games += semantics[i].kind == MessageMarkdownSemanticKind::game_mention;
        icons += semantics[i].kind == MessageMarkdownSemanticKind::game_icon && semantics[i].target == 10;
        users += semantics[i].kind == MessageMarkdownSemanticKind::user_mention;
        roles += semantics[i].kind == MessageMarkdownSemanticKind::role_mention;
        channels += semantics[i].kind == MessageMarkdownSemanticKind::channel_mention;
    }
    check(result.ok() && games == 1 && icons == 1 && contains(result.text, utf8_literal("Starfall Tactics")) &&
              !contains(result.text, utf8_literal("@Starfall")),
          "game markdown: the chip is its icon cell and name");
    check(users == 1 && roles == 1 && contains(result.text, utf8_literal("@Ada")),
          "game markdown: user and role mentions beside the chip");
    check(channels == 1, "game markdown: a channel mention beside the chip");
    result = parse(utf8_literal("<@$> <@$ab> <@$0> \\<@$10>"));
    games = 0;
    for (u32 i = 0; i < result.semantic_count; ++i)
        games += semantics[i].kind == MessageMarkdownSemanticKind::game_mention;
    check(result.ok() && !games && contains(result.text, utf8_literal("<@$ab>")),
          "game markdown: malformed and escaped game mentions stay text");
    options.preserve_entities = true;
    result = parse(utf8_literal("a <@$10> b"));
    check(result.ok() && utf8_equal(result.text, utf8_literal("a <@$10> b")),
          "game markdown: editors keep the mention's source");
}

void test_requests(Check check) noexcept {
    u8 target[256];
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::game_search;
    request.game_query = utf8_literal("  Star_Fall ");
    auto written = guild_admin_write_target(request, {target, sizeof(target)});
    const Utf8View search{reinterpret_cast<const char*>(target), written.bytes};
    check(written.ok() && contains(search, utf8_literal("games/autocomplete?q=star%20fall&filter_group=default")),
          "game requests: the search is GET /games/autocomplete with the normalized query");
    const u64 ids[]{10, 11};
    request = {};
    request.operation = GuildAdminOperation::games_list;
    request.game_ids = {ids, 2};
    written = guild_admin_write_target(request, {target, sizeof(target)});
    const Utf8View list{reinterpret_cast<const char*>(target), written.bytes};
    check(written.ok() && contains(list, utf8_literal("games?game_ids=10&game_ids=11")),
          "game requests: the lookup is GET /games?game_ids= per id");
}

void test_profile(Check check) noexcept {
    static u32 scratch[4096];
    static GameProfile profile;
    const Utf8View body = utf8_literal(
        R"([{"id":"9","name":"Other"},{"id":"10","name":"Starfall Tactics","description":"Top summary.",)"
        R"("genres":[48,53],"platforms":[0,6,1],"companies":[{"name":"QA Publishing","roles":[1]},)"
        R"({"name":"Fake Studio","roles":[2]},{"name":"Both Co","roles":[1,2]}],)"
        R"("first_release_date":"2024-03-14T00:00:00+00:00","websites":[{"url":"https://example.com","category":1},)"
        R"({"url":"https://example.com/x","category":5}],)"
        R"("supplemental_game_data":{"summary":"Supplemental.","publisher_names":["Other"],"genres":[1]}}])");
    check(game_profile_decode(body, {scratch, 4096}, 10, &profile) && profile.id == 10 &&
              utf8_equal({profile.summary, profile.summary_bytes}, utf8_literal("Top summary.")) &&
              profile.genre_count == 2 && profile.genres[0] == 48 && profile.genres[1] == 53 &&
              profile.platforms == ((1u << 0) | (1u << 6) | (1u << 1)) && profile.publisher_count == 2 &&
              utf8_equal({profile.publishers, profile.publishers_bytes}, utf8_literal("QA Publishing, Both Co")) &&
              utf8_equal({profile.developers, profile.developers_bytes}, utf8_literal("Fake Studio, Both Co")) &&
              profile.release_ms == 1710374400000ull && profile.link_count == 2 && profile.links[1].category == 5,
          "game profile: the top-level details win over the supplemental data");
    const Utf8View supplemental = utf8_literal(
        R"([{"id":"10","name":"S","supplemental_game_data":{"summary":"Only here.","summary_localized":"Localized.",)"
        R"("publisher_names":["P1","P2"],"developer_names":["D"],"genres":[1]}}])");
    check(game_profile_decode(supplemental, {scratch, 4096}, 10, &profile) &&
              utf8_equal({profile.summary, profile.summary_bytes}, utf8_literal("Localized.")) &&
              utf8_equal({profile.publishers, profile.publishers_bytes}, utf8_literal("P1, P2")) &&
              profile.developer_count == 1 && profile.genre_count == 1,
          "game profile: the supplemental data fills what the top level leaves out");
    check(!game_profile_decode(body, {scratch, 4096}, 12, &profile) &&
              !game_profile_decode(utf8_literal("{}"), {scratch, 4096}, 10, &profile),
          "game profile: a missing game or a body that is no list fails");
}

} // namespace

void test_game_mentions(Check check) noexcept {
    test_syntax(check);
    test_store(check);
    test_search(check);
    test_token(check);
    test_markdown(check);
    test_requests(check);
    test_profile(check);
}

namespace game_mentions_entry {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
} // namespace game_mentions_entry
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_game_mentions_tests_entry) {
    maicord::test_game_mentions(maicord::game_mentions_entry::check);
    ::maicord::test_process_exit(maicord::game_mentions_entry::failures ? 1 : 0);
}
