#include "maicord/model/guild_member_search.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
u32 chunk_of(u32 offset, u32 chunk) noexcept {
    return (offset ? offset - 1 : 0) / chunk;
}
bool size_valid(u32 size) noexcept {
    for (const u32 allowed : kGuildMemberSearchPageSizes)
        if (allowed == size)
            return true;
    return false;
}
} // namespace

GuildMemberSearchMode guild_member_search_mode(u32 page, u32 previous_page, u32 page_size,
                                               bool cursors_known) noexcept {
    const u32 chunk = guild_member_search_limit(page_size);
    const u32 previous = chunk_of(page_size * (page ? page - 1 : 0), chunk);
    const u32 current = chunk_of(page_size * page, chunk);
    const u32 next = chunk_of(page_size * (page + 1), chunk);
    if (!cursors_known || (current == next && current == 0))
        return GuildMemberSearchMode::first;
    if (current == next && current == previous)
        return GuildMemberSearchMode::repeat;
    if (previous_page < page && current < next)
        return GuildMemberSearchMode::after;
    if (previous_page > page && current >= previous)
        return current > 0 ? GuildMemberSearchMode::before : GuildMemberSearchMode::first;
    return GuildMemberSearchMode::repeat;
}

bool guild_member_search_bind(GuildMemberSearchStore* store, Slice<GuildSearchMember> members) noexcept {
    if (!store || !members.data || !members.count)
        return false;
    reset_value(store);
    store->members = members.data;
    store->capacity = members.count;
    store->page = 1;
    store->page_size = kGuildMemberSearchPageSizes[0];
    return true;
}

void guild_member_search_reset(GuildMemberSearchStore* store, bool forget) noexcept {
    if (!store)
        return;
    store->count = store->total = 0;
    store->total_known = false;
    store->page = 1;
    store->retries = 0;
    store->retry_at_ms = 0;
    store->phase = GuildMemberSearchPhase::idle;
    if (forget) {
        store->before = store->after = store->last_cursor = {};
        store->cursors_known = false;
        store->last_body_hash = 0;
        store->last_body_bytes = 0;
        store->last_page = 0;
    }
}

void guild_member_search_plan(const GuildMemberSearchStore& store, GuildMemberSearchQuery* query) noexcept {
    if (!query)
        return;
    query->limit = guild_member_search_limit(store.page_size);
    query->after = query->before = {};
    switch (guild_member_search_mode(store.page, store.last_page, store.page_size, store.cursors_known)) {
    case GuildMemberSearchMode::repeat: query->after = store.last_cursor; break;
    case GuildMemberSearchMode::after: query->after = store.after; break;
    case GuildMemberSearchMode::before: query->before = store.before; break;
    default: break;
    }
}

bool guild_member_search_begin(GuildMemberSearchStore* store, const GuildMemberSearchQuery& planned, u64 body_hash,
                               u32 body_bytes) noexcept {
    if (!store || (body_hash == store->last_body_hash && body_bytes == store->last_body_bytes))
        return false;
    store->last_body_hash = body_hash;
    store->last_body_bytes = body_bytes;
    store->last_page = store->page;
    store->last_cursor = planned.after.guild_joined_at_ms ? planned.after : planned.before;
    store->retries = 0;
    store->retry_at_ms = 0;
    store->phase = GuildMemberSearchPhase::loading;
    return true;
}

void guild_member_search_apply(GuildMemberSearchStore* store, const GuildMemberSearchPage& page) noexcept {
    if (!store)
        return;
    for (u32 i = 0; i < page.member_count; ++i) {
        const auto& member = page.members[i];
        u32 at = 0;
        while (at < store->count && store->members[at].user_id != member.user_id)
            ++at;
        if (at == store->count) {
            if (store->count == store->capacity)
                continue;
            ++store->count;
        }
        copy_value(&store->members[at], member);
    }
    store->total = page.total_result_count;
    store->total_known = page.total_known;
    store->cursors_known = true;
    store->before = store->after = {};
    if (page.member_count) {
        const auto& first = page.members[0];
        const auto& last = page.members[page.member_count - 1];
        if (first.joined_at_ms)
            store->before = {first.joined_at_ms, first.user_id};
        if (last.joined_at_ms)
            store->after = {last.joined_at_ms, last.user_id};
    }
    store->retries = 0;
    store->phase = GuildMemberSearchPhase::loaded;
}

bool guild_member_search_indexing(GuildMemberSearchStore* store, u64 retry_after_ms, u64 now_ms) noexcept {
    if (!store)
        return false;
    if (!retry_after_ms || store->retries >= kGuildMemberSearchRetryMaximum) {
        store->phase = GuildMemberSearchPhase::failed;
        return false;
    }
    ++store->retries;
    store->retry_at_ms = now_ms + retry_after_ms;
    store->phase = GuildMemberSearchPhase::indexing;
    return true;
}

void guild_member_search_failed(GuildMemberSearchStore* store) noexcept {
    if (store)
        store->phase = GuildMemberSearchPhase::failed;
}

u32 guild_member_search_page_rows(const GuildMemberSearchStore& store, u8 sort, Slice<u32> scratch,
                                  Slice<const GuildSearchMember*> output) noexcept {
    if (!scratch.data || scratch.count < store.count || !output.data)
        return 0;
    for (u32 i = 0; i < store.count; ++i)
        scratch[i] = i;
    const auto* m = store.members;
    heap_sort(scratch.data, store.count, [m, sort](u32 a, u32 b) noexcept {
        const auto& x = m[a];
        const auto& y = m[b];
        switch (sort) {
        case 2: return x.joined_at_ms < y.joined_at_ms || (x.joined_at_ms == y.joined_at_ms && x.user_id < y.user_id);
        case 3: return x.user_id > y.user_id;
        case 4: return x.user_id < y.user_id;
        default: return x.joined_at_ms > y.joined_at_ms || (x.joined_at_ms == y.joined_at_ms && x.user_id > y.user_id);
        }
    });
    const u32 start = (store.page ? store.page - 1 : 0) * store.page_size;
    u32 written = 0;
    for (u32 i = start; i < store.count && written < store.page_size && written < output.count; ++i)
        output[written++] = &m[scratch[i]];
    return written;
}

u32 guild_member_search_count(const GuildMemberSearchStore& store) noexcept {
    return !store.total_known || store.total < guild_member_search_limit(store.page_size) ? store.count
                                                                                          : store.total;
}

u32 guild_member_search_pages(const GuildMemberSearchStore& store) noexcept {
    const u32 count = guild_member_search_count(store);
    return count ? (count + store.page_size - 1) / store.page_size : 1;
}

void guild_member_search_set_page_size(GuildMemberSearchStore* store, u32 page_size) noexcept {
    if (!store || !size_valid(page_size) || page_size == store->page_size)
        return;
    // As the official store computes it: the old page scaled to the new size
    // when new size x old size members are held, else page 1.
    const u32 old_size = store->page_size;
    store->page = page_size * old_size <= store->count
                      ? maximum((old_size * store->page + page_size - 1) / page_size, 1u)
                      : 1;
    store->page_size = page_size;
}

} // namespace maicord
