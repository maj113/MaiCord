#include "game_lookup.hpp"
#include "game_profile.hpp"
#include "composer/composer.hpp"
#include "../account/account_access.hpp"
#include "../account/account_presentation.hpp"
#include "../shell/state.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kBusyRetryMs = 50;

enum class Request : u8 { none, batch, search, profile };

struct Lookup {
    GameStore store;
    GameRecord records[kGameStoreCapacity];
    GameSearchEntry searches[kGameSearchCacheCapacity];
    GuildAdminGame decoded[kGameSearchResultMaximum];
    u64 asked[kGameLookupBatchMaximum];
    u32 asked_count;
    Request request;
    u64 request_id, retry_at;
    // The search: the last noted query, the one the debounce sends next (due)
    // and the one in flight or about to be (sent).
    char noted[kGameSearchQueryBytes];
    char due[kGameSearchQueryBytes];
    char sent[kGameSearchQueryBytes];
    u16 noted_bytes, due_bytes, sent_bytes;
    u64 noted_at, invoked_at, due_at;
    bool search_due;
    // The game profile's game object, to be asked for (profile_due) or in flight.
    u64 profile_id;
    bool profile_due;
};
Lookup* g_lookup;

bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.running && g_app.account.status.gateway_ready;
}

Lookup* lookup() noexcept {
    if (g_lookup || !g_app.account_ui)
        return g_lookup;
    g_lookup = static_cast<Lookup*>(commit_pages(sizeof(Lookup)));
    if (!g_lookup)
        return nullptr;
    game_store_initialize(&g_lookup->store, {g_lookup->records, kGameStoreCapacity},
                          {g_lookup->searches, kGameSearchCacheCapacity});
    message_text_presentation_bind_games(&g_app.account_ui->message_text, &g_lookup->store);
    g_app.renderer.game_store = &g_lookup->store;
    return g_lookup;
}

Utf8View view(const char* text, u16 bytes) noexcept {
    return {text, bytes};
}

// New games repaint the chat's text, the composer's game results and the
// game profile.
void changed() noexcept {
    refresh_account_text(true);
    refresh_active_autocomplete();
    application_game_profile_games_changed();
    application_request_frame();
}

// The search's invocation: a query neither cached nor known to find nothing
// becomes due to send; a suppressed one settles at once with no results (as
// the official client stores it).
void invoke(Lookup& l, u64 now) noexcept {
    const Utf8View query = view(l.due, l.due_bytes);
    l.search_due = false;
    l.invoked_at = now;
    if (!query.bytes || game_store_search_find(l.store, query, now))
        return;
    if (game_store_search_suppressed(l.store, query, now)) {
        if (game_store_search_begin(&l.store, query, now))
            game_store_search_finish(&l.store, query, {}, false, now);
        return;
    }
    copy_bytes(l.sent, l.due, l.due_bytes);
    l.sent_bytes = l.due_bytes;
}

} // namespace

const GameStore* application_games() noexcept {
    return g_lookup ? &g_lookup->store : nullptr;
}

const GameRecord* game_lookup_find(u64 id) noexcept {
    return g_lookup ? game_store_find(g_lookup->store, id) : nullptr;
}

void game_lookup_want(u64 id) noexcept {
    Lookup* l = id ? lookup() : nullptr;
    if (l && game_store_want(&l->store, id, clock_monotonic_milliseconds()))
        application_request_frame();
}

u32 game_lookup_revision() noexcept {
    return g_lookup ? g_lookup->store.revision : 0;
}

void game_lookup_search(Utf8View typed) noexcept {
    char query[kGameSearchQueryBytes];
    const u32 bytes = guild_admin_game_query(typed, {query, sizeof(query)});
    Lookup* l = bytes ? lookup() : g_lookup;
    if (!l || (bytes == l->noted_bytes && bytes_equal(query, l->noted, bytes)))
        return;
    const u64 now = clock_monotonic_milliseconds();
    copy_bytes(l->noted, query, bytes);
    l->noted_bytes = static_cast<u16>(bytes);
    if (!bytes)
        return;
    copy_bytes(l->due, query, bytes);
    l->due_bytes = static_cast<u16>(bytes);
    // Leading edge: an edit after a quiet spell is invoked at once; else 200 ms
    // after the last edit, and never later than 500 ms after the last invocation.
    const bool quiet = now - l->noted_at >= kGameSearchDebounceMs && now - l->invoked_at >= kGameSearchDebounceMs;
    l->noted_at = now;
    if (quiet) {
        invoke(*l, now);
    } else {
        l->search_due = true;
        l->due_at = minimum(now + kGameSearchDebounceMs, l->invoked_at + kGameSearchMaximumWaitMs);
    }
    application_request_frame();
}

GameSearchResults game_lookup_results(Utf8View typed) noexcept {
    char query[kGameSearchQueryBytes];
    const u32 bytes = guild_admin_game_query(typed, {query, sizeof(query)});
    if (!g_lookup || !bytes)
        return {};
    const u64 now = clock_monotonic_milliseconds();
    const GameStore& store = g_lookup->store;
    const GameSearchEntry* exact = game_store_search_find(store, {query, bytes}, now);
    if (exact && !exact->pending)
        return {exact->ids, exact->count, false};
    const GameSearchEntry* closest = game_store_search_closest(store, {query, bytes}, now);
    return closest ? GameSearchResults{closest->ids, closest->count, true} : GameSearchResults{nullptr, 0, true};
}

void game_lookup_profile(u64 id) noexcept {
    Lookup* l = id ? lookup() : nullptr;
    if (!l)
        return;
    l->profile_id = id;
    l->profile_due = true;
    application_request_frame();
}

void game_lookup_profile_cancel() noexcept {
    if (g_lookup) {
        g_lookup->profile_id = 0;
        g_lookup->profile_due = false;
    }
}

bool application_game_lookup_pending(u64 now) noexcept {
    Lookup* l = g_lookup;
    if (!l || l->request != Request::none || now < l->retry_at || !connected())
        return false;
    if (l->search_due && now >= l->due_at)
        invoke(*l, now);
    return l->sent_bytes || l->profile_due || game_store_batch_due(l->store);
}

// Only called after application_game_lookup_pending reported work.
bool application_game_lookup_submit(u64 request_id, u64 now) noexcept {
    Lookup& l = *g_lookup;
    GuildAdminRequest request{};
    Request kind = Request::batch;
    // A typed search outranks the profile, which outranks mention batches.
    if (l.sent_bytes) {
        kind = Request::search;
        request.operation = GuildAdminOperation::game_search;
        request.game_query = view(l.sent, l.sent_bytes);
    } else if (l.profile_due) {
        kind = Request::profile;
        request.operation = GuildAdminOperation::games_list;
        request.game_ids = {&l.profile_id, 1};
    } else {
        l.asked_count = game_store_take_batch(&l.store, {l.asked, kGameLookupBatchMaximum});
        if (!l.asked_count)
            return false;
        request.operation = GuildAdminOperation::games_list;
        request.game_ids = {l.asked, l.asked_count};
    }
    const auto submitted = account_module_guild_admin(g_app.account.instance, &request, request_id);
    if (!submitted.ok()) {
        const bool busy = submitted.error == RestClientSubmitError::busy;
        if (busy)
            l.retry_at = now + kBusyRetryMs;
        // A refused batch is wanted again (busy) or fails for 15 s; a refused
        // search shows no results; a refused profile shows its failure.
        if (kind == Request::batch && busy) {
            game_store_return(&l.store, {l.asked, l.asked_count});
        } else if (kind == Request::batch) {
            game_store_settle(&l.store, {l.asked, l.asked_count}, false, now);
        } else if (kind == Request::search && !busy) {
            if (game_store_search_begin(&l.store, view(l.sent, l.sent_bytes), now))
                game_store_search_finish(&l.store, view(l.sent, l.sent_bytes), {}, true, now);
            l.sent_bytes = 0;
        } else if (kind == Request::profile && !busy) {
            l.profile_due = false;
            application_game_profile_response(l.profile_id, false, {});
        }
        return false;
    }
    if (kind == Request::search)
        game_store_search_begin(&l.store, view(l.sent, l.sent_bytes), now);
    else if (kind == Request::profile)
        l.profile_due = false;
    l.request = kind;
    l.request_id = request_id;
    return true;
}

bool application_game_lookup_response(const RestClientResponse& response, Utf8View body) noexcept {
    Lookup* l = g_lookup;
    if (!l || !l->request_id || response.request_id != l->request_id)
        return true;
    const Request kind = l->request;
    l->request = Request::none;
    l->request_id = 0;
    const u64 now = clock_monotonic_milliseconds();
    const bool ok = response.error == RestClientError::none && response.status_code == 200;
    if (response.status_code == 429)
        l->retry_at = response.retry_at_ms ? response.retry_at_ms : now + 1'000;
    const Slice<u32> scratch{g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount};
    const u32 room = kind == Request::profile ? 1u : kGameSearchResultMaximum;
    const auto decoded = ok ? guild_admin_decode_games(body, scratch, {l->decoded, room})
                            : GuildAdminResult{0, 0, GuildAdminError::input};
    const bool good = ok && decoded.error == GuildAdminError::none;
    u64 ids[kGameSearchResultMaximum];
    for (u32 i = 0; good && i < decoded.count; ++i) {
        game_store_put(&l->store, l->decoded[i], kind != Request::search);
        ids[i] = l->decoded[i].id;
    }
    if (kind == Request::batch) {
        game_store_settle(&l->store, {l->asked, l->asked_count}, good, now);
        l->asked_count = 0;
    } else if (kind == Request::search) {
        game_store_search_finish(&l->store, view(l->sent, l->sent_bytes), {ids, good ? decoded.count : 0}, !good,
                                 now);
        l->sent_bytes = 0;
    } else if (kind == Request::profile) {
        // A game the server does not know is unknown to the cache too.
        const u64 id = l->profile_id;
        if (good && !decoded.count && game_store_want(&l->store, id, now)) {
            u64 asked[1]{};
            game_store_take_batch(&l->store, {asked, 1});
            game_store_settle(&l->store, {asked, 1}, true, now);
        }
        application_game_profile_response(id, good, body);
    }
    changed();
    return true;
}

u64 application_game_lookup_next_wake() noexcept {
    const Lookup* l = g_lookup;
    if (!l || l->request != Request::none)
        return 0;
    u64 wake = 0;
    if (l->search_due)
        wake = maximum<u64>(1, l->due_at);
    if (l->sent_bytes || l->profile_due || game_store_batch_due(l->store))
        wake = 1;
    return wake && l->retry_at > wake ? l->retry_at : wake;
}

void application_game_lookup_release() noexcept {
    if (!g_lookup)
        return;
    if (g_app.account_ui)
        message_text_presentation_bind_games(&g_app.account_ui->message_text, nullptr);
    g_app.renderer.game_store = nullptr;
    pages_release(g_lookup);
    g_lookup = nullptr;
}

} // namespace maicord::app_detail
