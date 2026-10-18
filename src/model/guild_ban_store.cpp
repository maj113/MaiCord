#include "maicord/model/guild_ban_store.hpp"
#include "maicord/core/memory.hpp"


namespace maicord {
namespace {

u32 lower_bound(const GuildBanStore& store, u64 user_id) noexcept {
    u32 first = 0, count = store.count;
    while (count) {
        const u32 step = count / 2;
        if (store.records[first + step].user_id < user_id) {
            first += step + 1;
            count -= step + 1;
        } else
            count = step;
    }
    return first;
}

void assign(GuildBanRecord* record, u64 user_id, Utf8View name, Utf8View reason) noexcept {
    *record = {};
    record->user_id = user_id;
    record->name_bytes = static_cast<u16>(minimum<u32>(name.bytes, kGuildAdminUsernameBytes));
    record->reason_bytes = static_cast<u16>(minimum<u32>(reason.bytes, kGuildAdminReasonBytes));
    copy_bytes(record->name, name.data, record->name_bytes);
    copy_bytes(record->reason, reason.data, record->reason_bytes);
}

bool insert(GuildBanStore* store, u32 at, u64 user_id, Utf8View name, Utf8View reason) noexcept {
    if (store->count >= store->capacity)
        return false;
    for (u32 i = store->count; i > at; --i)
        store->records[i] = store->records[i - 1];
    assign(&store->records[at], user_id, name, reason);
    ++store->count;
    return true;
}

} // namespace

bool guild_ban_store_bind(GuildBanStore* store, Slice<GuildBanRecord> records) noexcept {
    if (!store || !records.data || !records.count)
        return false;
    *store = {records.data, records.count, 0, 0, 0, false};
    return true;
}

void guild_ban_store_reset(GuildBanStore* store, u64 guild_id) noexcept {
    if (!store)
        return;
    if (store->records && store->count)
        secure_clear_bytes(store->records, store->count * sizeof(GuildBanRecord));
    store->count = 0;
    store->guild_id = guild_id;
    store->complete = false;
    saturating_increment(store->revision);
}

u64 guild_ban_store_next_after(const GuildBanStore& store) noexcept {
    return store.count ? store.records[store.count - 1].user_id : 0;
}

bool guild_ban_store_append_page(GuildBanStore* store, u64 guild_id, ConstSlice<GuildAdminBan> page,
                                 bool last_page) noexcept {
    if (!store || !store->records || !guild_id || guild_id != store->guild_id || store->complete ||
        (!page.data && page.count) || page.count > store->capacity - store->count)
        return false;
    u64 previous = guild_ban_store_next_after(*store);
    for (u32 i = 0; i < page.count; ++i) {
        if (!page[i].user.id || page[i].user.id <= previous)
            return false;
        previous = page[i].user.id;
    }
    for (u32 i = 0; i < page.count; ++i) {
        const auto& ban = page[i];
        const Utf8View name = ban.user.display_bytes ? Utf8View{ban.user.display, ban.user.display_bytes}
                                                     : Utf8View{ban.user.username, ban.user.username_bytes};
        assign(&store->records[store->count++], ban.user.id, name, {ban.reason, ban.reason_bytes});
    }
    store->complete = last_page;
    saturating_increment(store->revision);
    return true;
}

bool guild_ban_store_apply_add(GuildBanStore* store, u64 guild_id, u64 user_id, Utf8View name) noexcept {
    if (!store || !store->records || !guild_id || guild_id != store->guild_id || !user_id)
        return false;
    const u32 at = lower_bound(*store, user_id);
    if (at < store->count && store->records[at].user_id == user_id)
        return true;
    // Beyond the loaded prefix the next page will deliver it.
    if (!store->complete && at == store->count)
        return true;
    if (!insert(store, at, user_id, name, {}))
        return false;
    saturating_increment(store->revision);
    return true;
}

bool guild_ban_store_apply_remove(GuildBanStore* store, u64 guild_id, u64 user_id) noexcept {
    if (!store || !store->records || !guild_id || guild_id != store->guild_id || !user_id)
        return false;
    const u32 at = lower_bound(*store, user_id);
    if (at >= store->count || store->records[at].user_id != user_id)
        return true;
    for (u32 i = at + 1; i < store->count; ++i)
        store->records[i - 1] = store->records[i];
    --store->count;
    secure_clear_bytes(&store->records[store->count], sizeof(GuildBanRecord));
    saturating_increment(store->revision);
    return true;
}

const GuildBanRecord* guild_ban_store_find(const GuildBanStore& store, u64 user_id) noexcept {
    if (!store.records || !user_id)
        return nullptr;
    const u32 at = lower_bound(store, user_id);
    return at < store.count && store.records[at].user_id == user_id ? &store.records[at] : nullptr;
}

} // namespace maicord
