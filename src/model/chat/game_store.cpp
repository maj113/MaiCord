#include "maicord/model/chat/game_store.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

bool view_equal(Utf8View a, const char* b, u32 bytes) noexcept {
    return a.bytes == bytes && bytes_equal(a.data, b, bytes);
}

GameRecord* find(GameStore* store, u64 id) noexcept {
    for (u32 i = 0; i < store->count; ++i)
        if (store->records[i].game.id == id)
            return &store->records[i];
    return nullptr;
}

// A free record, else the least recently used one no batch waits for.
GameRecord* slot(GameStore* store) noexcept {
    if (store->count < store->records.count)
        return &store->records[store->count++];
    GameRecord* oldest = nullptr;
    for (u32 i = 0; i < store->count; ++i) {
        GameRecord& record = store->records[i];
        if (record.state != GameState::pending && (!oldest || record.used < oldest->used))
            oldest = &record;
    }
    return oldest;
}

GameSearchEntry* search_find(GameStore* store, Utf8View query) noexcept {
    for (u32 i = 0; i < store->search_count; ++i)
        if (view_equal(query, store->searches[i].query, store->searches[i].query_bytes))
            return &store->searches[i];
    return nullptr;
}

bool search_fresh(const GameSearchEntry& entry, u64 now_ms) noexcept {
    return entry.pending ||
           now_ms - entry.stored_at < (entry.failed ? kGameSearchFailureStaleMs : kGameSearchStaleMs);
}

const GameSearchEntry* settled(const GameStore& store, Utf8View query, u64 now_ms) noexcept {
    const GameSearchEntry* entry = game_store_search_find(store, query, now_ms);
    return entry && !entry->pending && !entry->failed ? entry : nullptr;
}

constexpr u64 kExcluded[] = {
    484425151631589378ull, 1124351896166076496ull, 1124351975702679583ull, 1124353289857810612ull,
    1124353460880556183ull, 1124353599590379552ull, 1124354036020297778ull, 1124354174310699169ull,
    1124354219374297238ull, 1124354724301393940ull, 1124355292591829002ull, 1124355334593593364ull,
    1124355447323906128ull, 1124355979383943238ull, 1124356108509786252ull, 1124356199043846164ull,
    1124357045823488140ull, 1124357133526384761ull, 1124357256008450188ull, 1124357277042888804ull,
    1124357563014725652ull, 1124357621219086427ull, 1124357688621551638ull, 1124358061314818138ull,
    1124358311232426086ull, 1124358369315139704ull, 1124358585397293186ull, 1124358634739093554ull,
    1124358645010935888ull, 1124358853446873148ull, 1124359047932555324ull, 1124359276488573050ull,
    1124359293873963008ull, 1124359312932884551ull, 1124359434689335407ull, 1124359758556708915ull,
    1124360177634791564ull, 1124360363962552450ull, 1124360393519804557ull, 1124360545580093551ull,
    1124360662131425361ull, 1124360678858309652ull, 1129504691517136927ull, 1129504937424986152ull,
    1129505486010589265ull, 1130001171172561006ull, 1131042996108927056ull, 1132690430694264872ull,
    1138683327470108773ull, 1185733153193725952ull, 1201723489271431208ull, 1202901407431266324ull,
    1216022702519746742ull, 1222062501890625598ull, 1251038428578189345ull, 1262015762298830888ull,
    1264748775877120030ull, 1294962893057429605ull, 1297635514555306004ull, 1328800843922804786ull,
    1340125468736618546ull, 1345440503465906216ull, 1366277825207861270ull, 1370943538572824626ull,
    1373812493151113266ull, 1381135699486445642ull, 1384291534962430072ull, 1428188279433597048ull,
    1429242833394729142ull, 1441988763529117706ull, 1445221962757701702ull, 1445976787174686730ull,
    1447467809293733930ull, 1447664345839571158ull, 1455772568979116103ull, 1458671686131122269ull,
    1459562537145077832ull,
};

} // namespace

bool game_store_initialize(GameStore* store, Slice<GameRecord> records, Slice<GameSearchEntry> searches) noexcept {
    if (!store || !records.data || !records.count || !searches.data || !searches.count)
        return false;
    reset_value(store);
    store->records = records;
    store->searches = searches;
    return true;
}

const GameRecord* game_store_find(const GameStore& store, u64 id) noexcept {
    return id ? find(const_cast<GameStore*>(&store), id) : nullptr;
}

Utf8View game_store_name(const GameStore& store, u64 id) noexcept {
    const GameRecord* record = game_store_find(store, id);
    return record && record->state == GameState::known ? Utf8View{record->game.name, record->game.name_bytes}
                                                       : Utf8View{};
}

bool game_store_want(GameStore* store, u64 id, u64 now_ms) noexcept {
    if (!store || !id)
        return false;
    GameRecord* record = find(store, id);
    if (record) {
        record->used = ++store->clock;
        // A search result asks once for its game object (the cover).
        if (record->state == GameState::known && !record->full && !record->upgrade) {
            record->upgrade = 1;
            return true;
        }
        if (record->state != GameState::failed || now_ms < record->retry_at)
            return false;
    } else {
        record = slot(store);
        if (!record)
            return false;
        reset_value(record);
        record->game.id = id;
        record->used = ++store->clock;
    }
    record->state = GameState::wanted;
    return true;
}

bool game_store_batch_due(const GameStore& store, u64) noexcept {
    for (u32 i = 0; i < store.count; ++i)
        if (store.records[i].state == GameState::wanted || store.records[i].upgrade == 1)
            return true;
    return false;
}

u32 game_store_take_batch(GameStore* store, Slice<u64> ids, u64) noexcept {
    u32 count = 0;
    const u32 limit = minimum(ids.count, kGameLookupBatchMaximum);
    for (u32 i = 0; store && ids.data && i < store->count && count < limit; ++i) {
        GameRecord& record = store->records[i];
        if (record.state == GameState::wanted)
            record.state = GameState::pending;
        else if (record.upgrade == 1)
            record.upgrade = 2;
        else
            continue;
        ids[count++] = record.game.id;
    }
    return count;
}

void game_store_put(GameStore* store, const GuildAdminGame& game, bool full, u64) noexcept {
    if (!store || !game.id)
        return;
    GameRecord* record = find(store, game.id);
    if (record && record->full && !full)
        return;
    if (!record) {
        record = slot(store);
        if (!record)
            return;
        reset_value(record);
    }
    record->game = game;
    record->state = GameState::known;
    record->full = full;
    record->used = ++store->clock;
    ++store->revision;
}

void game_store_settle(GameStore* store, ConstSlice<u64> asked, bool ok, u64 now_ms) noexcept {
    for (u32 i = 0; store && i < asked.count; ++i) {
        GameRecord* record = find(store, asked[i]);
        if (record && record->upgrade == 2)
            record->upgrade = 3;
        if (!record || record->state != GameState::pending)
            continue;
        record->state = ok ? GameState::unknown : GameState::failed;
        record->retry_at = now_ms + kGameLookupFailureRetryMs;
        ++store->revision;
    }
}

void game_store_return(GameStore* store, ConstSlice<u64> asked) noexcept {
    for (u32 i = 0; store && i < asked.count; ++i) {
        GameRecord* record = find(store, asked[i]);
        if (record && record->upgrade == 2)
            record->upgrade = 1;
        if (record && record->state == GameState::pending)
            record->state = GameState::wanted;
    }
}

const GameSearchEntry* game_store_search_find(const GameStore& store, Utf8View query, u64 now_ms) noexcept {
    const GameSearchEntry* entry = search_find(const_cast<GameStore*>(&store), query);
    return entry && search_fresh(*entry, now_ms) ? entry : nullptr;
}

const GameSearchEntry* game_store_search_closest(const GameStore& store, Utf8View query, u64 now_ms) noexcept {
    for (u32 bytes = query.bytes; bytes; --bytes)
        if (const GameSearchEntry* entry = settled(store, {query.data, bytes}, now_ms))
            return entry;
    return nullptr;
}

bool game_store_search_suppressed(const GameStore& store, Utf8View query, u64 now_ms) noexcept {
    for (u32 bytes = query.bytes ? query.bytes - 1 : 0; bytes; --bytes)
        if (const GameSearchEntry* entry = settled(store, {query.data, bytes}, now_ms))
            return !entry->count && bytes >= 7;
    return false;
}

bool game_store_search_begin(GameStore* store, Utf8View query, u64 now_ms) noexcept {
    if (!store || !query.bytes || query.bytes > kGameSearchQueryBytes ||
        game_store_search_find(*store, query, now_ms))
        return false;
    GameSearchEntry* entry = search_find(store, query);
    if (!entry && store->search_count < store->searches.count)
        entry = &store->searches[store->search_count++];
    for (u32 i = 0; !entry && i < store->search_count; ++i)
        if (!store->searches[i].pending && (!entry || store->searches[i].used < entry->used))
            entry = &store->searches[i];
    if (!entry)
        return false;
    reset_value(entry);
    copy_bytes(entry->query, query.data, query.bytes);
    entry->query_bytes = static_cast<u8>(query.bytes);
    entry->pending = true;
    entry->used = ++store->clock;
    return true;
}

void game_store_search_finish(GameStore* store, Utf8View query, ConstSlice<u64> ids, bool failed,
                              u64 now_ms) noexcept {
    GameSearchEntry* entry = store ? search_find(store, query) : nullptr;
    if (!entry)
        return;
    entry->pending = false;
    entry->failed = failed;
    entry->stored_at = now_ms;
    entry->used = ++store->clock;
    entry->count = 0;
    for (u32 i = 0; !failed && i < ids.count && entry->count < kGameSearchResultMaximum; ++i)
        if (ids[i] && !game_store_search_excluded(ids[i]))
            entry->ids[entry->count++] = ids[i];
    ++store->revision;
}

bool game_store_search_excluded(u64 id) noexcept {
    u32 low = 0, high = sizeof(kExcluded) / sizeof(kExcluded[0]);
    while (low < high) {
        const u32 middle = (low + high) / 2;
        if (kExcluded[middle] < id)
            low = middle + 1;
        else
            high = middle;
    }
    return low < sizeof(kExcluded) / sizeof(kExcluded[0]) && kExcluded[low] == id;
}

u32 game_mention_write(u64 id, Slice<char> output) noexcept {
    char digits[20];
    const u32 count = id ? decimal_write(digits, id) : 0;
    if (!count || !output.data || output.count < count + 4)
        return 0;
    copy_bytes(output.data, "<@$", 3);
    copy_bytes(output.data + 3, digits, count);
    output[count + 3] = '>';
    return count + 4;
}

namespace {
bool starts(Utf8View text, u32 at, const char* literal, u32 bytes) noexcept {
    return at <= text.bytes && bytes <= text.bytes - at && bytes_equal(text.data + at, literal, bytes);
}
} // namespace

bool game_token_at(Utf8View text, u32 offset, GameToken* token) noexcept {
    if (!text.data || !starts(text, offset, kGameTokenHead, kGameTokenHeadBytes))
        return false;
    const u32 query = offset + kGameTokenHeadBytes;
    for (u32 at = query; at < text.bytes && text.data[at] != '\n'; ++at) {
        if (starts(text, at, kGameTokenHead, 3))
            return false;
        if (starts(text, at, kGameTokenEnd, kGameTokenEndBytes)) {
            if (token)
                *token = {offset, query, at, at + kGameTokenEndBytes};
            return true;
        }
    }
    return false;
}

bool game_token_around(Utf8View text, u32 caret, GameToken* token) noexcept {
    GameToken found{};
    for (u32 at = 0; at < text.bytes; ++at)
        if (game_token_at(text, at, &found)) {
            if (caret >= found.query_first && caret <= found.query_last) {
                *token = found;
                return true;
            }
            at = found.last - 1;
        }
    return false;
}

bool game_tokens_rewrite(Utf8View text, bool flatten, Slice<char> output, u32* bytes, u32* offset) noexcept {
    u32 used = 0, mapped = ~0u;
    bool changed = false;
    const auto put = [&](const char* data, u32 count) noexcept {
        if (count > output.count - used)
            return false;
        copy_bytes(output.data + used, data, count);
        used += count;
        return true;
    };
    for (u32 at = 0; at < text.bytes;) {
        if (offset && mapped == ~0u && *offset <= at)
            mapped = used;
        GameToken token{};
        if (game_token_at(text, at, &token)) {
            if (offset && mapped == ~0u && *offset < token.last)
                mapped = used + (flatten ? 0 : *offset - at);
            if (!(flatten ? put("@", 1) && put(text.data + token.query_first, token.query_last - token.query_first)
                          : put(text.data + at, token.last - at)))
                return false;
            changed = changed || flatten;
            at = token.last;
        } else if (starts(text, at, kGameTokenHead, 3) || starts(text, at, kGameTokenEnd, kGameTokenEndBytes)) {
            // A stray mark (its pair was deleted) goes, with a head's "@game:".
            changed = true;
            at += starts(text, at, kGameTokenHead, kGameTokenHeadBytes) ? kGameTokenHeadBytes : 3;
        } else {
            if (!put(text.data + at, 1))
                return false;
            ++at;
        }
    }
    if (!changed)
        return false;
    *bytes = used;
    if (offset)
        *offset = mapped == ~0u ? used : mapped;
    return true;
}

} // namespace maicord
