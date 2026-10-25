#include "maicord/model/user_activity.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u8 kWireMarker = 0xA8;
constexpr u32 kMaximumUsers = 2048;
constexpr u32 kRecordMaximumBytes = kUserActivityRecordMaximumBytes;
// Bank budget: 2048 users at 512 bytes, about twice a typical rich presence.
constexpr u32 kBankBytes = 1024 * 1024;
constexpr u16 kDeadEntry = 0xFFFF;
struct Row {
    u64 user_id;
    u64 scope;  // guild of the presence that wrote it; 0 for friends
    u32 offset; // of the bank entry
    u32 last_seen;
};
// Bank entry header, followed by the record. A replaced or removed record
// keeps its header with slot kDeadEntry until compaction reclaims it.
struct Entry {
    u16 bytes;
    u16 slot;
};
static_assert(kRecordMaximumBytes < kDeadEntry && kMaximumUsers < kDeadEntry);
struct Out {
    Slice<u8> output;
    u32 used = 0;
    bool ok = true;
    void put(const void* data, u32 bytes) noexcept {
        if (!ok || bytes > output.count - used) {
            ok = false;
            return;
        }
        copy_bytes(output.data + used, data, bytes);
        used += bytes;
    }
    template <typename T> void scalar(const T& value) noexcept {
        put(&value, sizeof(value));
    }
};
struct In {
    ConstSlice<u8> input;
    u32 used = 0;
    bool ok = true;
    void get(void* data, u32 bytes) noexcept {
        if (!ok || bytes > input.count - used) {
            ok = false;
            return;
        }
        copy_bytes(data, input.data + used, bytes);
        used += bytes;
    }
    template <typename T> void scalar(T& value) noexcept {
        get(&value, sizeof(value));
    }
};
u8* bank(const UserActivityStore& s) noexcept {
    return const_cast<u8*>(stable_storage_data<u8>(s.bank));
}
Row* rows(const UserActivityStore& s) noexcept {
    return const_cast<Row*>(stable_storage_data<Row>(s.records));
}
u16* order(const UserActivityStore& s) noexcept {
    return const_cast<u16*>(stable_storage_data<u16>(s.order));
}
Entry entry(const UserActivityStore& s, u32 offset) noexcept {
    Entry e;
    copy_bytes(&e, bank(s) + offset, sizeof(e));
    return e;
}
void set_entry(const UserActivityStore& s, u32 offset, Entry e) noexcept {
    copy_bytes(bank(s) + offset, &e, sizeof(e));
}
u32 locate(const UserActivityStore& s, u64 user) noexcept {
    const Row* r = rows(s);
    const u16* o = order(s);
    u32 first = 0, last = s.count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (r[o[middle]].user_id < user)
            first = middle + 1;
        else
            last = middle;
    }
    return first;
}
const Row* find_row(const UserActivityStore& s, u64 user) noexcept {
    const u32 at = user ? locate(s, user) : s.count;
    if (at >= s.count)
        return nullptr;
    const Row& row = rows(s)[order(s)[at]];
    return row.user_id == user ? &row : nullptr;
}
void release_entry(UserActivityStore* s, u32 offset) noexcept {
    Entry e = entry(*s, offset);
    s->bank_dead += sizeof(Entry) + e.bytes;
    e.slot = kDeadEntry;
    set_entry(*s, offset, e);
}
// Removes the user at order position `at`; the last row moves into its slot.
void remove(UserActivityStore* s, u32 at) noexcept {
    Row* r = rows(*s);
    u16* o = order(*s);
    const u32 slot = o[at];
    release_entry(s, r[slot].offset);
    for (u32 i = at; i + 1 < s->count; ++i)
        o[i] = o[i + 1];
    --s->count;
    if (slot == s->count)
        return;
    r[slot] = r[s->count];
    Entry moved = entry(*s, r[slot].offset);
    moved.slot = static_cast<u16>(slot);
    set_entry(*s, r[slot].offset, moved);
    for (u32 i = 0; i < s->count; ++i)
        if (o[i] == s->count) {
            o[i] = static_cast<u16>(slot);
            break;
        }
}
// Evicts the least recently updated user other than `keep`; guild-scoped
// records go first, so busy servers cannot push friends out.
bool evict_oldest(UserActivityStore* s, u64 keep) noexcept {
    const Row* r = rows(*s);
    u32 slot = s->count;
    const auto older = [&](u32 i) noexcept {
        return (r[i].scope != 0) != (r[slot].scope != 0) ? r[i].scope != 0 : r[i].last_seen < r[slot].last_seen;
    };
    for (u32 i = 0; i < s->count; ++i)
        if (r[i].user_id != keep && (slot == s->count || older(i)))
            slot = i;
    if (slot == s->count)
        return false;
    remove(s, locate(*s, r[slot].user_id));
    return true;
}
void compact(UserActivityStore* s) noexcept {
    u8* b = bank(*s);
    Row* r = rows(*s);
    u32 read = 0, write = 0;
    while (read < s->bank_used) {
        const Entry e = entry(*s, read);
        const u32 bytes = sizeof(Entry) + e.bytes;
        if (e.slot != kDeadEntry) {
            copy_bytes(b + write, b + read, bytes);
            r[e.slot].offset = write;
            write += bytes;
        }
        read += bytes;
    }
    s->bank_used = write;
    s->bank_dead = 0;
}
// Room for `bytes` at the bank's end: reclaim holes, then commit pages, and at
// the budget evict the least recently updated users (never `keep`).
bool make_room(UserActivityStore* s, u32 bytes, u64 keep) noexcept {
    while (s->bank_used + bytes > s->bank.capacity) {
        if (s->bank_dead)
            compact(s);
        else if (!stable_storage_reserve(&s->bank, s->bank_used + bytes) && !evict_oldest(s, keep))
            return false;
    }
    return true;
}
void append(UserActivityStore* s, u32 slot, ConstSlice<u8> encoded) noexcept {
    rows(*s)[slot].offset = s->bank_used;
    set_entry(*s, s->bank_used, {static_cast<u16>(encoded.count), static_cast<u16>(slot)});
    copy_bytes(bank(*s) + s->bank_used + sizeof(Entry), encoded.data, encoded.count);
    s->bank_used += sizeof(Entry) + encoded.count;
}
} // namespace

u32 user_activities_write(const UserActivitySet& set, Slice<u8> output) noexcept {
    if (!output.data || set.count > kUserActivityMaximum)
        return 0;
    Out w{output};
    w.scalar(kWireMarker);
    w.scalar(set.count);
    for (u32 i = 0; i < set.count; ++i) {
        const auto& a = set.items[i];
        u32 bytes = 0;
        for (u32 k = 0; k < kUserActivityFieldCount; ++k)
            bytes += a.lengths[k];
        if (bytes > kUserActivityTextBytes)
            return 0;
        const u8 type = static_cast<u8>(a.type), platform = static_cast<u8>(a.platform);
        w.scalar(type);
        w.scalar(a.button_count);
        w.scalar(a.detail);
        w.scalar(platform);
        w.scalar(a.application_id);
        w.scalar(a.start_ms);
        w.scalar(a.end_ms);
        w.scalar(a.created_ms);
        w.scalar(a.party_size);
        w.scalar(a.party_max);
        w.scalar(a.flags);
        w.put(a.lengths, sizeof(a.lengths));
        w.put(a.text, bytes);
    }
    return w.ok ? w.used : 0;
}
bool user_activities_read(ConstSlice<u8> input, u64 user, UserActivitySet* output) noexcept {
    if (!output || !user || !input.data || input.count < 2)
        return false;
    In r{input};
    u8 marker = 0, count = 0;
    r.scalar(marker);
    r.scalar(count);
    if (marker != kWireMarker || count > kUserActivityMaximum)
        return false;
    UserActivitySet set{};
    set.user_id = user;
    set.count = count;
    for (u32 i = 0; i < count && r.ok; ++i) {
        auto& a = set.items[i];
        u8 type = 0, platform = 0;
        r.scalar(type);
        r.scalar(a.button_count);
        r.scalar(a.detail);
        r.scalar(platform);
        r.scalar(a.application_id);
        r.scalar(a.start_ms);
        r.scalar(a.end_ms);
        r.scalar(a.created_ms);
        r.scalar(a.party_size);
        r.scalar(a.party_max);
        r.scalar(a.flags);
        r.get(a.lengths, sizeof(a.lengths));
        if (!r.ok || type > 5 || type == 4 || a.button_count > 2 || a.detail > 15 ||
            platform > static_cast<u8>(UserActivityPlatform::meta_quest))
            return false;
        a.type = static_cast<UserActivityType>(type);
        a.platform = static_cast<UserActivityPlatform>(platform);
        u32 bytes = 0;
        for (u32 k = 0; k < kUserActivityFieldCount; ++k) {
            if (a.lengths[k] > kUserActivityFieldBudget[k])
                return false;
            bytes += a.lengths[k];
        }
        if (bytes > kUserActivityTextBytes || !a.lengths[0])
            return false;
        r.get(a.text, bytes);
        for (u32 k = 0; r.ok && k < kUserActivityFieldCount; ++k)
            if (!utf8_validate(user_activity_text(a, static_cast<UserActivityField>(k))).valid)
                return false;
    }
    if (!r.ok || r.used != input.count)
        return false;
    *output = set;
    return true;
}


bool user_activity_find(const UserActivityStore& s, u64 user, UserActivitySet* output) noexcept {
    const Row* row = output ? find_row(s, user) : nullptr;
    return row && user_activities_read({bank(s) + row->offset + sizeof(Entry), entry(s, row->offset).bytes}, user,
                                       output);
}
u32 user_activity_count(const UserActivityStore& s, u64 user) noexcept {
    const Row* row = find_row(s, user);
    return row ? bank(s)[row->offset + sizeof(Entry) + 1] : 0;
}
bool user_activity_first_type(const UserActivityStore& s, u64 user, UserActivityType* output) noexcept {
    const Row* row = output ? find_row(s, user) : nullptr;
    const u8* record = row ? bank(s) + row->offset + sizeof(Entry) : nullptr;
    if (!record || !record[1])
        return false;
    *output = static_cast<UserActivityType>(record[2]);
    return true;
}
bool user_activity_upsert(UserActivityStore* s, const UserActivitySet& value) noexcept {
    return user_activity_apply(s, value, 0, false);
}
bool user_activity_apply(UserActivityStore* s, const UserActivitySet& value, u64 scope, bool offline) noexcept {
    if (!s || !value.user_id || value.count > kUserActivityMaximum)
        return false;
    const u32 at = locate(*s, value.user_id);
    const bool exists = at < s->count && rows(*s)[order(*s)[at]].user_id == value.user_id;
    if (!value.count || offline) {
        const u64 owner = exists ? rows(*s)[order(*s)[at]].scope : 0;
        if (exists && (offline || !scope || owner == scope)) {
            remove(s, at);
            ++s->revision;
        }
        return true;
    }
    u8 encoded[kRecordMaximumBytes];
    const u32 bytes = user_activities_write(value, {encoded, sizeof(encoded)});
    if (!bytes)
        return false;
    if (exists) {
        Row& row = rows(*s)[order(*s)[at]];
        row.last_seen = ++s->clock;
        row.scope = scope;
        const Entry old = entry(*s, row.offset);
        u8* stored = bank(*s) + row.offset + sizeof(Entry);
        if (old.bytes == bytes && bytes_equal(stored, encoded, bytes))
            return true;
        if (old.bytes == bytes) {
            copy_bytes(stored, encoded, bytes);
        } else {
            // Evictions may move this user's row; find it again afterwards.
            if (!make_room(s, sizeof(Entry) + bytes, value.user_id))
                return false;
            const u32 slot = order(*s)[locate(*s, value.user_id)];
            rows(*s)[slot].scope = scope;
            release_entry(s, rows(*s)[slot].offset);
            append(s, slot, {encoded, bytes});
        }
        ++s->revision;
        return true;
    }
    if (!s->records.arena.base &&
        (!stable_storage_initialize(&s->records, kMaximumUsers, 0, sizeof(Row), alignof(Row)) ||
         !stable_storage_initialize(&s->order, kMaximumUsers, 0, sizeof(u16), alignof(u16)) ||
         !stable_storage_initialize(&s->bank, kBankBytes, 0, 1, 1))) {
        user_activity_release(s);
        return false;
    }
    if (s->count == kMaximumUsers)
        evict_oldest(s, 0);
    if (!make_room(s, sizeof(Entry) + bytes, 0) || !stable_storage_reserve(&s->records, s->count + 1) ||
        !stable_storage_reserve(&s->order, s->count + 1))
        return false;
    const u32 slot = s->count;
    rows(*s)[slot] = {value.user_id, scope, 0, ++s->clock};
    append(s, slot, {encoded, bytes});
    const u32 insert = locate(*s, value.user_id);
    u16* o = order(*s);
    for (u32 i = s->count; i > insert; --i)
        o[i] = o[i - 1];
    o[insert] = static_cast<u16>(slot);
    ++s->count;
    ++s->revision;
    return true;
}
void user_activity_release(UserActivityStore* s) noexcept {
    if (!s)
        return;
    stable_storage_release(&s->records);
    stable_storage_release(&s->order);
    stable_storage_release(&s->bank);
    reset_value(s);
}
} // namespace maicord
