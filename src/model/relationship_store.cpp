#include "maicord/model/relationship_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/model/sorted_search.hpp"

namespace maicord {
namespace {
bool valid(const RelationshipStore& store) noexcept {
    return store.records.data && store.records.count && store.records.count <= kRelationshipMaximumRecords &&
        store.count <= store.records.count && byte_range_valid({reinterpret_cast<const u8*>(store.records.data),
            store.records.count * static_cast<u32>(sizeof(RelationshipRecord))});
}
bool valid(const RelationshipRecord& record) noexcept {
    return record.user_id && record.type <= RelationshipType::suggestion &&
        record.presence <= RelationshipPresence::unknown && record.nickname_bytes <= kRelationshipNicknameMaximumBytes &&
        utf8_validate({record.nickname, record.nickname_bytes}).valid;
}
bool equal(const RelationshipRecord& a, const RelationshipRecord& b) noexcept {
    return a.user_id == b.user_id && a.type == b.type && a.presence == b.presence && a.ignored == b.ignored &&
        a.spam == b.spam && a.stranger == b.stranger && a.stranger_known == b.stranger_known && a.nickname_bytes == b.nickname_bytes &&
        bytes_equal(a.nickname, b.nickname, a.nickname_bytes);
}
u32 lower(const RelationshipStore& store, u64 id) noexcept {
    return sorted_lower_bound(store.count, [&](u32 at) noexcept { return store.records[at].user_id < id; });
}
RelationshipStoreResult result(RelationshipStoreError error, bool changed = false) noexcept { return {error, changed}; }
} // namespace
bool relationship_store_initialize(RelationshipStore* store, Slice<RelationshipRecord> records) noexcept {
    if (!store || store->count || !records.data || !records.count || records.count > kRelationshipMaximumRecords ||
        !byte_range_valid({reinterpret_cast<const u8*>(records.data), records.count * static_cast<u32>(sizeof(*records.data))}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(store), sizeof(*store)},
            {reinterpret_cast<const u8*>(records.data), records.count * static_cast<u32>(sizeof(*records.data))})) return false;
    const u32 revision = store->revision;
    *store = {records, 0, revision};
    for (u32 i = 0; i < records.count; ++i) reset_value(&records[i]);
    return true;
}
const RelationshipRecord* relationship_store_find(const RelationshipStore& store, u64 id) noexcept {
    if (!valid(store) || !id) return nullptr;
    const u32 index = lower(store, id);
    return index < store.count && store.records[index].user_id == id ? &store.records[index] : nullptr;
}
RelationshipStoreResult relationship_store_upsert(RelationshipStore* store, const RelationshipRecord& record) noexcept {
    if (!store || !valid(*store) || !valid(record)) return result(RelationshipStoreError::invalid_input);
    const RelationshipRecord owned = record;
    const u32 index = lower(*store, owned.user_id);
    if (index < store->count && store->records[index].user_id == owned.user_id) {
        if (equal(store->records[index], owned)) return result(RelationshipStoreError::none);
        store->records[index] = owned;
    } else {
        if (store->count == store->records.count) return result(RelationshipStoreError::capacity);
        for (u32 i = store->count; i > index; --i) store->records[i] = store->records[i - 1];
        store->records[index] = owned;
        ++store->count;
    }
    saturating_increment(store->revision);
    return result(RelationshipStoreError::none, true);
}
RelationshipStoreResult relationship_store_remove(RelationshipStore* store, u64 id) noexcept {
    if (!store || !valid(*store) || !id) return result(RelationshipStoreError::invalid_input);
    const u32 index = lower(*store, id);
    if (index == store->count || store->records[index].user_id != id) return result(RelationshipStoreError::none);
    for (u32 i = index + 1; i < store->count; ++i) store->records[i - 1] = store->records[i];
    reset_value(&store->records[--store->count]);
    saturating_increment(store->revision);
    return result(RelationshipStoreError::none, true);
}
RelationshipStoreResult relationship_store_replace(RelationshipStore* store, ConstSlice<RelationshipRecord> sorted) noexcept {
    if (!store || !valid(*store) || sorted.count > kRelationshipMaximumRecords ||
        !byte_range_valid({reinterpret_cast<const u8*>(sorted.data), sorted.count * static_cast<u32>(sizeof(*sorted.data))}))
        return result(RelationshipStoreError::invalid_input);
    if (sorted.count > store->records.count) return result(RelationshipStoreError::capacity);
    const ConstSlice<u8> source{reinterpret_cast<const u8*>(sorted.data), sorted.count * static_cast<u32>(sizeof(*sorted.data))};
    if (byte_ranges_overlap(source, {reinterpret_cast<const u8*>(store), sizeof(*store)}) ||
        (sorted.data != store->records.data && byte_ranges_overlap(source, {reinterpret_cast<const u8*>(store->records.data),
            store->records.count * static_cast<u32>(sizeof(*store->records.data))}))) return result(RelationshipStoreError::invalid_input);
    bool unchanged = sorted.count == store->count;
    for (u32 i = 0; i < sorted.count; ++i) {
        if (!valid(sorted[i])) return result(RelationshipStoreError::invalid_input);
        if (i && sorted[i - 1].user_id >= sorted[i].user_id) return result(RelationshipStoreError::duplicate);
        if (unchanged && !equal(sorted[i], store->records[i])) unchanged = false;
    }
    if (unchanged) return result(RelationshipStoreError::none);
    const u32 previous = store->count;
    for (u32 i = 0; i < sorted.count; ++i) store->records[i] = sorted[i];
    for (u32 i = sorted.count; i < previous; ++i) reset_value(&store->records[i]);
    store->count = sorted.count;
    saturating_increment(store->revision);
    return result(RelationshipStoreError::none, true);
}
RelationshipStoreResult relationship_store_presence(RelationshipStore* store, u64 id, RelationshipPresence presence) noexcept {
    if (!store || !valid(*store) || !id || presence > RelationshipPresence::unknown) return result(RelationshipStoreError::invalid_input);
    const u32 index = lower(*store, id);
    if (index == store->count || store->records[index].user_id != id || store->records[index].presence == presence)
        return result(RelationshipStoreError::none);
    store->records[index].presence = presence;
    saturating_increment(store->revision);
    return result(RelationshipStoreError::none, true);
}
bool relationship_matches(const RelationshipRecord& record, RelationshipFilter filter) noexcept {
    switch (filter) {
    case RelationshipFilter::all: return record.type == RelationshipType::friend_;
    case RelationshipFilter::online: return record.type == RelationshipType::friend_ &&
        (record.presence == RelationshipPresence::online || record.presence == RelationshipPresence::idle || record.presence == RelationshipPresence::dnd);
    case RelationshipFilter::pending: return record.type == RelationshipType::outgoing ||
        (record.type == RelationshipType::incoming && !record.spam && !record.ignored);
    case RelationshipFilter::blocked: return record.type == RelationshipType::blocked;
    }
    return false;
}
Utf8View relationship_display_name(const RelationshipRecord& record, const UserStore& users) noexcept {
    if (record.nickname_bytes && record.nickname_bytes <= kRelationshipNicknameMaximumBytes) return {record.nickname, record.nickname_bytes};
    const auto* user = user_store_find(users, record.user_id);
    return user ? user_store_name(users, *user) : Utf8View{};
}
} // namespace maicord
