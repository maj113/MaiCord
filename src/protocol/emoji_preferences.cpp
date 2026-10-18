#include "maicord/protocol/emoji_preferences.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/writer.hpp"
#include "json_fields.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using namespace protocol_detail;
using E = EmojiPreferencesError;
constexpr u32 kFavorites = 5, kFrecency = 6, kReactionFrecency = 13;

struct Reader {
    const u8* data;
    u32 at, end;
    bool varint(u64& n) noexcept {
        n = 0;
        for (u32 i = 0; i < 10; ++i) {
            if (at == end)
                return false;
            const u8 b = data[at++];
            if (i == 9 && b > 1)
                return false;
            n |= static_cast<u64>(b & 127) << (i * 7);
            if (!(b & 128))
                return true;
        }
        return false;
    }
    bool tag(u32& field, u32& type) noexcept {
        u64 n = 0;
        if (!varint(n) || n > ~u32{0} || !(n >> 3))
            return false;
        field = static_cast<u32>(n >> 3);
        type = static_cast<u32>(n & 7);
        return true;
    }
    bool length(u32& stop) noexcept {
        u64 n = 0;
        if (!varint(n) || n > end - at)
            return false;
        stop = at + static_cast<u32>(n);
        return true;
    }
    bool skip(u32 type, u32 field, u32 depth = 0) noexcept {
        u64 n = 0;
        if (type == 0)
            return varint(n);
        if (type == 1 || type == 5) {
            const u32 bytes = type == 1 ? 8 : 4;
            if (bytes > end - at)
                return false;
            at += bytes;
            return true;
        }
        if (type == 2) {
            u32 stop = 0;
            if (!length(stop))
                return false;
            at = stop;
            return true;
        }
        if (type != 3 || depth >= 16)
            return false;
        while (at < end) {
            u32 f = 0, t = 0;
            if (!tag(f, t))
                return false;
            if (t == 4)
                return f == field;
            if (!skip(t, f, depth + 1))
                return false;
        }
        return false;
    }
};

Utf8View view(const u8* data, u32 begin, u32 end) noexcept {
    return {reinterpret_cast<const char*>(data + begin), end - begin};
}

bool key_valid(Utf8View key) noexcept {
    return key.bytes && key.bytes <= kEmojiPreferencesMaximumKeyBytes && utf8_validate(key).valid;
}

void push_recent(EmojiFrecencyItem& item, u64 value) noexcept {
    if (!value)
        return;
    // Keep the newest samples in ascending order.
    if (item.recent_count == kEmojiPreferencesRecentUses) {
        if (value <= item.recent_uses[0])
            return;
        for (u32 i = 1; i < kEmojiPreferencesRecentUses; ++i)
            item.recent_uses[i - 1] = item.recent_uses[i];
        --item.recent_count;
    }
    u32 at = item.recent_count++;
    while (at && item.recent_uses[at - 1] > value) {
        item.recent_uses[at] = item.recent_uses[at - 1];
        --at;
    }
    item.recent_uses[at] = value;
}

E frecency_item(Reader r, EmojiFrecencyItem& item) noexcept {
    item = {};
    while (r.at < r.end) {
        u32 f = 0, t = 0;
        u64 n = 0;
        if (!r.tag(f, t))
            return E::protobuf;
        if (f == 1 || f == 3 || f == 4) {
            if (t != 0 || !r.varint(n))
                return E::protobuf;
            if (f == 1)
                item.total_uses = static_cast<u32>(n);
            else if (f == 3)
                item.frecency = static_cast<i32>(n);
            else
                item.score = static_cast<i32>(n);
        } else if (f == 2 && t == 2) {
            u32 stop = 0;
            if (!r.length(stop))
                return E::protobuf;
            Reader packed{r.data, r.at, stop};
            while (packed.at < packed.end) {
                if (!packed.varint(n))
                    return E::protobuf;
                push_recent(item, n);
            }
            r.at = stop;
        } else if (f == 2 && t == 0) {
            if (!r.varint(n))
                return E::protobuf;
            push_recent(item, n);
        } else if (!r.skip(t, f))
            return E::protobuf;
    }
    return E::none;
}

// One map entry: key (1) and value (2); missing value is a default item.
E map_entry(Reader r, Utf8View& key, EmojiFrecencyItem& item) noexcept {
    key = {};
    item = {};
    while (r.at < r.end) {
        u32 f = 0, t = 0;
        if (!r.tag(f, t))
            return E::protobuf;
        if ((f == 1 || f == 2) && t == 2) {
            u32 stop = 0;
            if (!r.length(stop))
                return E::protobuf;
            if (f == 1) {
                key = view(r.data, r.at, stop);
                if (!utf8_validate(key).valid)
                    return E::utf8;
            } else {
                const E error = frecency_item({r.data, r.at, stop}, item);
                if (error != E::none)
                    return error;
            }
            r.at = stop;
        } else if (!r.skip(t, f))
            return E::protobuf;
    }
    return E::none;
}

E check_favorites(ConstSlice<u8> group) noexcept {
    Reader r{group.data, 0, group.count};
    while (r.at < r.end) {
        u32 f = 0, t = 0;
        if (!r.tag(f, t))
            return E::protobuf;
        if (f == 1) {
            u32 stop = 0;
            if (t != 2 || !r.length(stop))
                return E::protobuf;
            if (!utf8_validate(view(r.data, r.at, stop)).valid)
                return E::utf8;
            r.at = stop;
        } else if (!r.skip(t, f))
            return E::protobuf;
    }
    return E::none;
}

E check_frecency(ConstSlice<u8> group) noexcept {
    Reader r{group.data, 0, group.count};
    while (r.at < r.end) {
        u32 f = 0, t = 0;
        if (!r.tag(f, t))
            return E::protobuf;
        if (f == 1) {
            u32 stop = 0;
            if (t != 2 || !r.length(stop))
                return E::protobuf;
            Utf8View key{};
            EmojiFrecencyItem item{};
            const E error = map_entry({r.data, r.at, stop}, key, item);
            if (error != E::none)
                return error;
            r.at = stop;
        } else if (!r.skip(t, f))
            return E::protobuf;
    }
    return E::none;
}

E check_versions(Reader r, u32& data_version) noexcept {
    while (r.at < r.end) {
        u32 f = 0, t = 0;
        u64 n = 0;
        if (!r.tag(f, t))
            return E::protobuf;
        if (f == 3 && t == 0) {
            if (!r.varint(n) || n > ~u32{0})
                return E::protobuf;
            data_version = static_cast<u32>(n);
        } else if (!r.skip(t, f))
            return E::protobuf;
    }
    return E::none;
}

struct Writer {
    u8* data;
    u32 used, limit;
    bool overflow;
    void bytes(const u8* source, u32 count) noexcept {
        if (count > limit - minimum(used, limit))
            overflow = true;
        else if (data && count)
            copy_bytes(data + used, source, count);
        used += count;
    }
    void byte(u8 value) noexcept {
        bytes(&value, 1);
    }
    void number(u64 n) noexcept {
        do {
            const u8 b = static_cast<u8>((n & 127) | (n > 127 ? 128 : 0));
            byte(b);
            n >>= 7;
        } while (n);
    }
    void scalar(u32 field, u64 n) noexcept {
        number(static_cast<u64>(field) << 3);
        number(n);
    }
    void string(u32 field, Utf8View text) noexcept {
        number((static_cast<u64>(field) << 3) | 2);
        number(text.bytes);
        bytes(reinterpret_cast<const u8*>(text.data), text.bytes);
    }
};

u32 varint_bytes(u64 n) noexcept {
    u32 bytes = 1;
    while (n > 127) {
        n >>= 7;
        ++bytes;
    }
    return bytes;
}

void write_item(Writer& out, const EmojiFrecencyItem& item) noexcept {
    if (item.total_uses)
        out.scalar(1, item.total_uses);
    u32 packed = 0;
    for (u32 i = 0; i < item.recent_count; ++i)
        packed += varint_bytes(item.recent_uses[i]);
    if (packed) {
        out.number((2 << 3) | 2);
        out.number(packed);
        for (u32 i = 0; i < item.recent_count; ++i)
            out.number(item.recent_uses[i]);
    }
    // int32 negatives are sign-extended to ten bytes on the wire.
    if (item.frecency)
        out.scalar(3, static_cast<u64>(static_cast<i64>(item.frecency)));
    if (item.score)
        out.scalar(4, static_cast<u64>(static_cast<i64>(item.score)));
}

u32 item_bytes(const EmojiFrecencyItem& item) noexcept {
    Writer counter{nullptr, 0, ~0u, false};
    write_item(counter, item);
    return counter.used;
}

void write_entry(Writer& out, Utf8View key, const EmojiFrecencyItem& item) noexcept {
    const u32 value = item_bytes(item);
    const u32 entry = 1 + varint_bytes(key.bytes) + key.bytes + 1 + varint_bytes(value) + value;
    out.number((1 << 3) | 2);
    out.number(entry);
    out.string(1, key);
    out.number((2 << 3) | 2);
    out.number(value);
    write_item(out, item);
}

// Identities of observed keys, edits and entries, resolved once per write
// (the patch body is laid out twice). Keys past the cache resolve on demand.
constexpr u32 kCachedIdentities = 256;
struct Identities {
    const EmojiPreferencesPatch* patch;
    u64 favorites[kCachedIdentities], frecency[kCachedIdentities], reaction[kCachedIdentities];
    u64 edits[64];
    u64 entries[2][kEmojiPreferencesMaximumFrecencyEntries];
    u64 of(Utf8View key) const noexcept {
        return patch->identity ? patch->identity(patch->identity_context, key) : 0;
    }
    u64 observed(const u64* cache, u32 ordinal, Utf8View key) const noexcept {
        return ordinal < kCachedIdentities ? cache[ordinal] : of(key);
    }
};

bool same_emoji(u64 a, Utf8View a_key, u64 b, Utf8View b_key) noexcept {
    return (a && a == b) || utf8_equal(a_key, b_key);
}

// Calls visit(key) for each favorite or frecency map entry of a validated group.
template <class Visit> void each_key(ConstSlice<u8> group, bool map, Visit visit) noexcept {
    Reader r{group.data, 0, group.count};
    while (r.at < r.end) {
        u32 f = 0, t = 0;
        r.tag(f, t);
        if (f == 1 && t == 2) {
            u32 stop = 0;
            r.length(stop);
            Utf8View key = view(r.data, r.at, stop);
            if (map) {
                EmojiFrecencyItem item{};
                map_entry({r.data, r.at, stop}, key, item);
            }
            visit(key);
            r.at = stop;
        } else
            r.skip(t, f);
    }
}

void cache_observed(const Identities& ids, ConstSlice<u8> group, bool map, u64* cache) noexcept {
    u32 ordinal = 0;
    each_key(group, map, [&](Utf8View key) noexcept {
        if (ordinal < kCachedIdentities)
            cache[ordinal] = ids.of(key);
        ++ordinal;
    });
}

// The last edit naming the emoji decides whether it stays a favorite.
bool final_favorite(ConstSlice<EmojiFavoriteEdit> edits, const Identities& ids, u64 id, Utf8View key,
                    bool* favorite) noexcept {
    bool found = false;
    for (u32 i = 0; i < edits.count; ++i)
        if (same_emoji(ids.edits[i], edits[i].key, id, key)) {
            *favorite = edits[i].favorite;
            found = true;
        }
    return found;
}

bool observed_has(ConstSlice<u8> group, const Identities& ids, u64 id, Utf8View key) noexcept {
    u32 ordinal = 0;
    bool found = false;
    each_key(group, false, [&](Utf8View observed) noexcept {
        found = found || same_emoji(ids.observed(ids.favorites, ordinal, observed), observed, id, key);
        ++ordinal;
    });
    return found;
}

// Returns the resulting favorite count; ~0u when an add exceeds the limit.
u32 write_favorites(Writer& out, ConstSlice<u8> group, ConstSlice<EmojiFavoriteEdit> edits,
                    const Identities& ids) noexcept {
    u32 count = 0, ordinal = 0;
    Reader r{group.data, 0, group.count};
    while (r.at < r.end) {
        const u32 begin = r.at;
        u32 f = 0, t = 0;
        r.tag(f, t);
        if (f == 1 && t == 2) {
            u32 stop = 0;
            r.length(stop);
            const Utf8View key = view(r.data, r.at, stop);
            const u64 id = ids.observed(ids.favorites, ordinal++, key);
            r.at = stop;
            bool favorite = true;
            if (final_favorite(edits, ids, id, key, &favorite) && !favorite)
                continue;
            ++count;
        } else
            r.skip(t, f);
        out.bytes(group.data + begin, r.at - begin);
    }
    for (u32 i = 0; i < edits.count; ++i) {
        bool favorite = false;
        final_favorite(edits, ids, ids.edits[i], edits[i].key, &favorite);
        bool earlier = false;
        for (u32 j = 0; j < i && !earlier; ++j)
            earlier = same_emoji(ids.edits[j], edits[j].key, ids.edits[i], edits[i].key);
        if (!favorite || earlier || observed_has(group, ids, ids.edits[i], edits[i].key))
            continue;
        if (count == kEmojiPreferencesMaximumFavorites)
            return ~0u;
        out.string(1, edits[i].key);
        ++count;
    }
    return count;
}

constexpr u32 kNoEntry = ~0u;
u32 matching_entry(ConstSlice<EmojiFrecencyEntry> entries, const u64* entry_ids, u64 id, Utf8View key) noexcept {
    for (u32 i = 0; i < entries.count; ++i)
        if (same_emoji(entry_ids[i], entries[i].key, id, key))
            return i;
    return kNoEntry;
}

void write_frecency(Writer& out, ConstSlice<u8> group, ConstSlice<EmojiFrecencyEntry> entries,
                    const u64* entry_ids, const u64* cache, const Identities& ids) noexcept {
    constexpr u32 limit = kEmojiPreferencesMaximumFrecencyEntries;
    // The last observed occurrence of an entry's emoji is authoritative.
    u32 last[limit];
    for (u32 i = 0; i < entries.count; ++i)
        last[i] = kNoEntry;
    u32 ordinal = 0;
    each_key(group, true, [&](Utf8View key) noexcept {
        const u32 match = matching_entry(entries, entry_ids, ids.observed(cache, ordinal, key), key);
        if (match != kNoEntry)
            last[match] = ordinal;
        ++ordinal;
    });
    u32 claimed = 0, fresh = 0;
    for (u32 i = 0; i < entries.count; ++i) {
        claimed += last[i] != kNoEntry;
        fresh += last[i] == kNoEntry && entries[i].updated;
    }
    // Entries this client holds come first; the rest share what remains.
    const u32 spare = limit - minimum(limit, claimed + fresh);
    u32 written = 0, kept = 0;
    ordinal = 0;
    Reader r{group.data, 0, group.count};
    while (r.at < r.end) {
        const u32 begin = r.at;
        u32 f = 0, t = 0;
        r.tag(f, t);
        if (f == 1 && t == 2) {
            u32 stop = 0;
            r.length(stop);
            Utf8View key{};
            EmojiFrecencyItem item{};
            map_entry({r.data, r.at, stop}, key, item);
            r.at = stop;
            const u32 current = ordinal++;
            const u64 id = ids.observed(cache, current, key);
            const u32 match = matching_entry(entries, entry_ids, id, key);
            if (match != kNoEntry && last[match] == current) {
                ++written;
                if (entries[match].updated) {
                    write_entry(out, key, entries[match].item);
                    continue;
                }
            } else if ((match != kNoEntry || !id) && kept < spare) {
                ++kept;
                ++written;
            } else {
                continue;
            }
        } else
            r.skip(t, f);
        out.bytes(group.data + begin, r.at - begin);
    }
    for (u32 i = 0; i < entries.count && written < limit; ++i)
        if (last[i] == kNoEntry && entries[i].updated) {
            write_entry(out, entries[i].key, entries[i].item);
            ++written;
        }
}

template <class Body> void group(Writer& out, u32 field, Body body) noexcept {
    Writer counter{nullptr, 0, ~0u, false};
    body(counter);
    out.number((static_cast<u64>(field) << 3) | 2);
    out.number(counter.used);
    body(out);
}

bool entries_valid(ConstSlice<EmojiFrecencyEntry> entries) noexcept {
    if (entries.count > kEmojiPreferencesMaximumFrecencyEntries || (entries.count && !entries.data))
        return false;
    for (u32 i = 0; i < entries.count; ++i)
        if (!key_valid(entries[i].key) || entries[i].item.recent_count > kEmojiPreferencesRecentUses)
            return false;
    return true;
}
} // namespace

EmojiPreferencesResult emoji_preferences_decode(ConstSlice<u8> proto, Slice<u8> output,
                                                EmojiPreferencesSnapshot* snapshot) noexcept {
    EmojiPreferencesResult result{};
    if (!byte_range_valid(proto) || (output.data && !byte_range_valid({output.data, output.count})) ||
        (output.data && byte_ranges_overlap(proto, {output.data, output.count}))) {
        result.error = E::input;
        return result;
    }
    *snapshot = {};
    u32 sizes[3]{};
    // Pass 0 validates and measures; pass 1 copies once everything fits.
    for (u32 pass = 0; pass < 2; ++pass) {
        u32 offsets[3]{0, sizes[0], sizes[0] + sizes[1]}, written[3]{};
        Reader r{proto.data, 0, proto.count};
        while (r.at < r.end) {
            u32 f = 0, t = 0;
            if (!r.tag(f, t)) {
                result.error = E::protobuf;
                return result;
            }
            const u32 slot = f == kFavorites ? 0 : f == kFrecency ? 1 : f == kReactionFrecency ? 2 : 3;
            if ((slot < 3 || f == 1) && t == 2) {
                u32 stop = 0;
                if (!r.length(stop)) {
                    result.error = E::protobuf;
                    return result;
                }
                if (f == 1) {
                    if (!pass) {
                        snapshot->versions_present = true;
                        result.error = check_versions({r.data, r.at, stop}, snapshot->data_version);
                    }
                } else if (!pass) {
                    const ConstSlice<u8> part{r.data + r.at, stop - r.at};
                    result.error = slot == 0 ? check_favorites(part) : check_frecency(part);
                    if (part.count > kEmojiPreferencesMaximumGroupBytes - sizes[slot])
                        result.error = E::capacity;
                    sizes[slot] += part.count;
                } else {
                    copy_bytes(output.data + offsets[slot] + written[slot], r.data + r.at, stop - r.at);
                    written[slot] += stop - r.at;
                }
                if (!result.ok())
                    return result;
                r.at = stop;
            } else if (!r.skip(t, f)) {
                result.error = E::protobuf;
                return result;
            }
        }
        result.group_bytes = sizes[0] + sizes[1] + sizes[2];
        if (!pass) {
            if (!output.data)
                return result;
            if (output.count < result.group_bytes) {
                result.error = E::capacity;
                return result;
            }
        } else {
            snapshot->favorites = {output.data, sizes[0]};
            snapshot->frecency = {output.data + offsets[1], sizes[1]};
            snapshot->reaction_frecency = {output.data + offsets[2], sizes[2]};
        }
    }
    return result;
}

EmojiPreferencesError emoji_preferences_visit_favorites(ConstSlice<u8> group, EmojiFavoriteVisitor visit,
                                                        void* context) noexcept {
    if (!byte_range_valid(group))
        return E::input;
    const E checked = check_favorites(group);
    if (checked != E::none)
        return checked;
    Reader r{group.data, 0, group.count};
    while (r.at < r.end) {
        u32 f = 0, t = 0;
        r.tag(f, t);
        if (f == 1) {
            u32 stop = 0;
            r.length(stop);
            if (!visit(context, view(r.data, r.at, stop)))
                return E::none;
            r.at = stop;
        } else
            r.skip(t, f);
    }
    return E::none;
}

EmojiPreferencesError emoji_preferences_visit_frecency(ConstSlice<u8> group, EmojiFrecencyVisitor visit,
                                                       void* context) noexcept {
    if (!byte_range_valid(group))
        return E::input;
    const E checked = check_frecency(group);
    if (checked != E::none)
        return checked;
    Reader r{group.data, 0, group.count};
    while (r.at < r.end) {
        u32 f = 0, t = 0;
        r.tag(f, t);
        if (f == 1) {
            u32 stop = 0;
            r.length(stop);
            Utf8View key{};
            EmojiFrecencyItem item{};
            map_entry({r.data, r.at, stop}, key, item);
            if (!visit(context, key, item))
                return E::none;
            r.at = stop;
        } else
            r.skip(t, f);
    }
    return E::none;
}

EmojiPreferencesWriteResult emoji_preferences_write_patch(const EmojiPreferencesPatch& patch, Slice<u8> scratch,
                                                          Slice<u8> json) noexcept {
    const auto fail = [](E error) noexcept { return EmojiPreferencesWriteResult{0, 0, 0, error}; };
    const ConstSlice<u8> groups[]{patch.observed_favorites, patch.observed_frecency,
                                  patch.observed_reaction_frecency};
    for (const auto observed : groups)
        if (!byte_range_valid(observed) || observed.count > kEmojiPreferencesMaximumGroupBytes)
            return fail(E::input);
    if (!byte_range_valid({scratch.data, scratch.count}) || !byte_range_valid({json.data, json.count}) ||
        byte_ranges_overlap({scratch.data, scratch.count}, {json.data, json.count}) ||
        (patch.favorite_edits.count && !patch.favorite_edits.data) ||
        patch.favorite_edits.count > 64 || (!patch.favorite_edits.count && !patch.write_frecency) ||
        !entries_valid(patch.frecency) || !entries_valid(patch.reaction_frecency))
        return fail(E::input);
    for (u32 i = 0; i < patch.favorite_edits.count; ++i)
        if (!key_valid(patch.favorite_edits[i].key))
            return fail(E::input);
    if (check_favorites(patch.observed_favorites) != E::none ||
        check_frecency(patch.observed_frecency) != E::none ||
        check_frecency(patch.observed_reaction_frecency) != E::none)
        return fail(E::protobuf);
    Identities ids;
    ids.patch = &patch;
    for (u32 i = 0; i < patch.favorite_edits.count; ++i)
        ids.edits[i] = ids.of(patch.favorite_edits[i].key);
    if (patch.favorite_edits.count)
        cache_observed(ids, patch.observed_favorites, false, ids.favorites);
    if (patch.write_frecency) {
        cache_observed(ids, patch.observed_frecency, true, ids.frecency);
        cache_observed(ids, patch.observed_reaction_frecency, true, ids.reaction);
        for (u32 i = 0; i < patch.frecency.count; ++i)
            ids.entries[0][i] = ids.of(patch.frecency[i].key);
        for (u32 i = 0; i < patch.reaction_frecency.count; ++i)
            ids.entries[1][i] = ids.of(patch.reaction_frecency[i].key);
    }
    u32 favorite_count = 0;
    bool full = false;
    const auto body = [&](Writer& out) noexcept {
        if (patch.favorite_edits.count)
            group(out, kFavorites, [&](Writer& inner) noexcept {
                favorite_count = write_favorites(inner, patch.observed_favorites, patch.favorite_edits, ids);
                full |= favorite_count == ~0u;
            });
        if (patch.write_frecency) {
            group(out, kFrecency, [&](Writer& inner) noexcept {
                write_frecency(inner, patch.observed_frecency, patch.frecency, ids.entries[0], ids.frecency, ids);
            });
            group(out, kReactionFrecency, [&](Writer& inner) noexcept {
                write_frecency(inner, patch.observed_reaction_frecency, patch.reaction_frecency, ids.entries[1],
                               ids.reaction, ids);
            });
        }
    };
    Writer counted{nullptr, 0, ~0u, false};
    body(counted);
    if (full)
        return fail(E::capacity);
    if (counted.used > scratch.count || counted.used > 3 * (kEmojiPreferencesMaximumGroupBytes + 8))
        return fail(E::capacity);
    constexpr char start[] = "{\"settings\":\"";
    constexpr char middle[] = "\",\"required_data_version\":";
    constexpr u32 start_bytes = static_cast<u32>(sizeof(start) - 1);
    constexpr u32 middle_bytes = static_cast<u32>(sizeof(middle) - 1);
    const u32 encoded_bytes = base64_encoded_size(counted.used);
    const u32 json_bytes =
        start_bytes + encoded_bytes + middle_bytes + decimal_digits(patch.required_data_version) + 1;
    if (json_bytes > json.count || json_bytes > kEmojiPreferencesMaximumPatchBytes)
        return fail(E::capacity);
    Writer proto{scratch.data, 0, scratch.count, false};
    body(proto);
    if (proto.overflow || proto.used != counted.used)
        return fail(E::capacity);
    copy_bytes(json.data, start, start_bytes);
    base64_encode({scratch.data, proto.used}, {reinterpret_cast<char*>(json.data + start_bytes), encoded_bytes});
    ProtocolWriter text{json, start_bytes + encoded_bytes};
    protocol_writer_append_literal(&text, middle);
    protocol_writer_append_u64(&text, patch.required_data_version);
    protocol_writer_append_byte(&text, '}');
    return {text.used, proto.used, patch.favorite_edits.count ? favorite_count : 0, E::none};
}

EmojiPreferencesError emoji_preferences_validate_patch_json(Utf8View input) noexcept {
    if (input.bytes > kEmojiPreferencesMaximumPatchBytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(input.data), input.bytes}))
        return E::input;
    u32 structurals[16]{};
    const auto scan = json_scan_structurals_avx1(input, {structurals, 16});
    SettingsPatchEnvelope parsed{};
    if (!scan.ok() || !json_visit_root_object(input, {structurals, scan.count}, 2, settings_patch_member, &parsed).ok() ||
        parsed.seen != 3 || !parsed.encoded.bytes)
        return E::envelope;
    const auto measured = base64_decode(parsed.encoded, {});
    if (measured.error != Base64Error::output_too_small && !measured.ok())
        return E::envelope;
    if (!measured.bytes || measured.bytes > 3 * (kEmojiPreferencesMaximumGroupBytes + 8))
        return E::capacity;
    u8* raw = static_cast<u8*>(VirtualAlloc(nullptr, measured.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!raw)
        return E::capacity;
    E error = base64_decode(parsed.encoded, {raw, measured.bytes}).ok() ? E::none : E::envelope;
    Reader r{raw, 0, measured.bytes};
    while (error == E::none && r.at < r.end) {
        u32 f = 0, t = 0, stop = 0;
        if (!r.tag(f, t) || t != 2 || (f != kFavorites && f != kFrecency && f != kReactionFrecency) ||
            !r.length(stop)) {
            error = E::envelope;
            break;
        }
        const ConstSlice<u8> part{raw + r.at, stop - r.at};
        error = part.count > kEmojiPreferencesMaximumGroupBytes ? E::capacity
                : f == kFavorites                               ? check_favorites(part)
                                                                : check_frecency(part);
        r.at = stop;
    }
    VirtualFree(raw, 0, MEM_RELEASE);
    return error;
}
} // namespace maicord
