#include "maicord/protocol/user/emoji_preferences.hpp"
#include "maicord/protocol/encoding/base64.hpp"
#include "../encoding/json_fields.hpp"
#include "../encoding/proto_wire.hpp"
#include "frecency_wire.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/utf.hpp"


namespace maicord {
namespace {
using namespace protocol_detail;
using E = EmojiPreferencesError;
// sticker_frecency (4) is read only; patches never write it.
constexpr u32 kStickerFrecency = 4, kFavorites = 5, kFrecency = 6, kReactionFrecency = 13;

// A cursor over [at, end) of a raw buffer, reading through the shared wire helpers.
struct Reader {
    const u8* data;
    u32 at, end;
    ProtoSource source() const noexcept {
        return proto_source({data, end});
    }
    bool varint(u64& n) noexcept {
        return proto_varint(source(), at, end, n);
    }
    bool tag(u32& field, u32& type) noexcept {
        return proto_tag(source(), at, end, field, type);
    }
    bool length(u32& stop) noexcept {
        return proto_length(source(), at, end, stop);
    }
    bool skip(u32 type, u32 field) noexcept {
        return proto_skip(source(), at, end, type, field);
    }
};

Utf8View view(const u8* data, u32 begin, u32 end) noexcept {
    return {reinterpret_cast<const char*>(data + begin), end - begin};
}

bool key_valid(const Utf8View& key) noexcept {
    return key.bytes && key.bytes <= kEmojiPreferencesMaximumKeyBytes && utf8_validate(key).valid;
}

// One map entry: key (1) and value (2); missing value is a default item.
E map_entry(Reader r, Utf8View& key, FrecencyItem& item) noexcept {
    const auto error = frecency_entry_decode(r.source(), r.at, r.end, &key, &item);
    return error == FrecencyWire::ok ? E::none : error == FrecencyWire::utf8 ? E::utf8 : E::protobuf;
}

// Validates a favorites (map false: repeated strings) or frecency (map true) group.
E check_group(const ConstSlice<u8>& group, bool map) noexcept {
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
            FrecencyItem item;
            const E error = map ? map_entry({r.data, r.at, stop}, key, item)
                                : utf8_validate(view(r.data, r.at, stop)).valid ? E::none : E::utf8;
            if (error != E::none)
                return error;
            r.at = stop;
        } else if (!r.skip(t, f))
            return E::protobuf;
    }
    return E::none;
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
        return patch->identity ? patch->identity(key) : 0;
    }
    u64 observed(const u64* cache, u32 ordinal, const Utf8View& key) const noexcept {
        return ordinal < kCachedIdentities ? cache[ordinal] : of(key);
    }
};

bool same_emoji(u64 a, const Utf8View& a_key, u64 b, const Utf8View& b_key) noexcept {
    return (a && a == b) || utf8_equal(a_key, b_key);
}

using EntryVisit = bool (*)(void* context, Utf8View key, const FrecencyItem& item) noexcept;

// Calls visit for each entry of a validated group in wire order until it returns false: a favorite's string (the
// item stays default) or a frecency map entry's key and item.
void each_entry(ConstSlice<u8> group, bool map, EntryVisit visit, void* context) noexcept {
    FrecencyItem item{};
    Reader r{group.data, 0, group.count};
    while (r.at < r.end) {
        u32 f = 0, t = 0;
        r.tag(f, t);
        if (f == 1) {
            u32 stop = 0;
            r.length(stop);
            Utf8View key = view(r.data, r.at, stop);
            if (map)
                map_entry({r.data, r.at, stop}, key, item);
            if (!visit(context, key, item))
                return;
            r.at = stop;
        } else
            r.skip(t, f);
    }
}

// each_entry for a callable of the key alone.
template <class Visit> void each_key(ConstSlice<u8> group, bool map, Visit visit) noexcept {
    each_entry(group, map, [](void* context, Utf8View key, const FrecencyItem&) noexcept {
        (*static_cast<Visit*>(context))(key);
        return true;
    }, &visit);
}

struct Visitors {
    EmojiFavoriteVisitor favorite;
    EmojiFrecencyVisitor frecency;
    void* context;
};
// The caller's visitor over a validated favorites (map false) or frecency group.
EmojiPreferencesError visit_group(ConstSlice<u8> group, bool map, Visitors visitors) noexcept {
    if (!byte_range_valid(group))
        return E::input;
    const E checked = check_group(group, map);
    if (checked != E::none)
        return checked;
    each_entry(group, map, [](void* context, Utf8View key, const FrecencyItem& item) noexcept {
        const auto& called = *static_cast<const Visitors*>(context);
        return called.favorite ? called.favorite(called.context, key) : called.frecency(called.context, key, item);
    }, &visitors);
    return E::none;
}

void cache_observed(const Identities& ids, const ConstSlice<u8>& group, bool map, u64* cache) noexcept {
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
u32 write_favorites(ProtoWriter& out, ConstSlice<u8> group, ConstSlice<EmojiFavoriteEdit> edits,
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

void write_frecency(ProtoWriter& out, ConstSlice<u8> group, ConstSlice<EmojiFrecencyEntry> entries,
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
            FrecencyItem item;
            map_entry({r.data, r.at, stop}, key, item);
            r.at = stop;
            const u32 current = ordinal++;
            const u64 id = ids.observed(cache, current, key);
            const u32 match = matching_entry(entries, entry_ids, id, key);
            if (match != kNoEntry && last[match] == current) {
                ++written;
                if (entries[match].updated) {
                    frecency_entry_write(out, key, entries[match].item);
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
            frecency_entry_write(out, entries[i].key, entries[i].item);
            ++written;
        }
}

bool entries_valid(ConstSlice<EmojiFrecencyEntry> entries) noexcept {
    if (entries.count > kEmojiPreferencesMaximumFrecencyEntries || (entries.count && !entries.data))
        return false;
    for (u32 i = 0; i < entries.count; ++i)
        if (!key_valid(entries[i].key) || entries[i].item.recent_count > kFrecencyRecentUses)
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
    reset_value(snapshot);
    u32 sizes[4]{};
    bool sticker_ok = true;
    // Pass 0 validates and measures; pass 1 copies once everything fits.
    for (u32 pass = 0; pass < 2; ++pass) {
        u32 offsets[4]{0, sizes[0], sizes[0] + sizes[1], sizes[0] + sizes[1] + sizes[2]}, written[4]{};
        Reader r{proto.data, 0, proto.count};
        while (r.at < r.end) {
            u32 f = 0, t = 0;
            if (!r.tag(f, t)) {
                result.error = E::protobuf;
                return result;
            }
            const u32 slot = f == kFavorites           ? 0
                             : f == kFrecency          ? 1
                             : f == kReactionFrecency  ? 2
                             : f == kStickerFrecency   ? 3
                                                       : 4;
            if ((slot < 4 || f == 1) && t == 2) {
                u32 stop = 0;
                if (!r.length(stop)) {
                    result.error = E::protobuf;
                    return result;
                }
                if (f == 1) {
                    if (!pass)
                        result.error = settings_data_version(r.source(), r.at, stop, &snapshot->data_version)
                                           ? E::none
                                           : E::protobuf;
                } else if (!pass) {
                    const ConstSlice<u8> part{r.data + r.at, stop - r.at};
                    const E checked = check_group(part, slot != 0);
                    const bool fits = part.count <= kEmojiPreferencesMaximumGroupBytes - sizes[slot];
                    // Sticker frecency only weighs results: a bad group is dropped, never fatal.
                    if (slot == 3 && (checked != E::none || !fits))
                        sticker_ok = false;
                    else
                        result.error = !fits ? E::capacity : checked;
                    sizes[slot] += part.count;
                } else if (slot != 3 || sticker_ok) {
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
        if (!sticker_ok)
            sizes[3] = 0;
        result.group_bytes = sizes[0] + sizes[1] + sizes[2] + sizes[3];
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
            snapshot->sticker_frecency = {output.data + offsets[3], sizes[3]};
        }
    }
    return result;
}

EmojiPreferencesError emoji_preferences_visit_favorites(ConstSlice<u8> group, EmojiFavoriteVisitor visit,
                                                        void* context) noexcept {
    return visit_group(group, false, {visit, nullptr, context});
}

EmojiPreferencesError emoji_preferences_visit_frecency(ConstSlice<u8> group, EmojiFrecencyVisitor visit,
                                                       void* context) noexcept {
    return visit_group(group, true, {nullptr, visit, context});
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
    if (check_group(patch.observed_favorites, false) != E::none ||
        check_group(patch.observed_frecency, true) != E::none ||
        check_group(patch.observed_reaction_frecency, true) != E::none)
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
    const auto body = [&](ProtoWriter& out) noexcept {
        if (patch.favorite_edits.count)
            frecency_group_write(out, kFavorites, [&](ProtoWriter& inner) noexcept {
                favorite_count = write_favorites(inner, patch.observed_favorites, patch.favorite_edits, ids);
                full |= favorite_count == ~0u;
            });
        if (patch.write_frecency) {
            frecency_group_write(out, kFrecency, [&](ProtoWriter& inner) noexcept {
                write_frecency(inner, patch.observed_frecency, patch.frecency, ids.entries[0], ids.frecency, ids);
            });
            frecency_group_write(out, kReactionFrecency, [&](ProtoWriter& inner) noexcept {
                write_frecency(inner, patch.observed_reaction_frecency, patch.reaction_frecency, ids.entries[1],
                               ids.reaction, ids);
            });
        }
    };
    ProtoWriter counted{};
    body(counted);
    if (full)
        return fail(E::capacity);
    if (counted.at > scratch.count || counted.at > 3 * (kEmojiPreferencesMaximumGroupBytes + 8))
        return fail(E::capacity);
    const u32 json_bytes = proto_settings_patch_bytes(counted.at, patch.required_data_version, true);
    if (json_bytes > json.count || json_bytes > kEmojiPreferencesMaximumPatchBytes)
        return fail(E::capacity);
    ProtoWriter proto{scratch, 0, true};
    body(proto);
    if (!proto.ok || proto.at != counted.at)
        return fail(E::capacity);
    proto_write_settings_patch({scratch.data, proto.at}, patch.required_data_version, json, true);
    return {json_bytes, proto.at, patch.favorite_edits.count ? favorite_count : 0, E::none};
}

EmojiPreferencesError emoji_preferences_validate_patch_json(Utf8View input) noexcept {
    if (input.bytes > kEmojiPreferencesMaximumPatchBytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(input.data), input.bytes}))
        return E::input;
    Utf8View encoded{};
    if (settings_patch_members(input, &encoded) != 3 || !encoded.bytes)
        return E::envelope;
    const auto measured = base64_decode(encoded, {});
    if (measured.error != Base64Error::output_too_small && !measured.ok())
        return E::envelope;
    if (!measured.bytes || measured.bytes > 3 * (kEmojiPreferencesMaximumGroupBytes + 8))
        return E::capacity;
    u8* raw = static_cast<u8*>(commit_pages(measured.bytes));
    if (!raw)
        return E::capacity;
    E error = base64_decode(encoded, {raw, measured.bytes}).ok() ? E::none : E::envelope;
    Reader r{raw, 0, measured.bytes};
    while (error == E::none && r.at < r.end) {
        u32 f = 0, t = 0, stop = 0;
        if (!r.tag(f, t) || t != 2 || (f != kFavorites && f != kFrecency && f != kReactionFrecency) ||
            !r.length(stop)) {
            error = E::envelope;
            break;
        }
        const ConstSlice<u8> part{raw + r.at, stop - r.at};
        error = part.count > kEmojiPreferencesMaximumGroupBytes ? E::capacity : check_group(part, f != kFavorites);
        r.at = stop;
    }
    pages_release(raw, measured.bytes);
    return error;
}
} // namespace maicord
