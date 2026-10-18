#include "maicord/model/user_profile.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {
namespace {
constexpr u32 maximum_records = 4096;
constexpr u32 kUnitBytes = 8;
constexpr u32 kRecordHeaderBytes = sizeof(UserAppearanceRecord);
constexpr u32 kRecordTailMaximum =
    sizeof(UserAppearance{}.decoration.data) + sizeof(UserAppearance{}.nameplate.data) +
    sizeof(UserAppearance{}.avatar.data) + sizeof(UserAppearance{}.primary_badge.data) +
    sizeof(UserAppearance{}.primary_tag.data) + sizeof(UserAppearance{}.activity.data) +
    sizeof(UserAppearance{}.status_emoji.data);
// Allocations are rounded to 16 bytes, which absorbs most string edits in place.
constexpr u32 allocation_units(u32 bytes) noexcept {
    return (bytes + 15u) / 16u * 2u;
}
constexpr u32 kRecordUnitsMaximum = allocation_units(kRecordHeaderBytes + kRecordTailMaximum);
// Every record at its largest fits; garbage beyond that is compacted away.
constexpr u32 kArenaUnits = maximum_records * kRecordUnitsMaximum;
// Compact once dead allocations exceed both this and half of the arena in use.
constexpr u32 kGarbageCompactUnits = 32 * 1024 / kUnitBytes;
static_assert(kRecordHeaderBytes == 136 && kRecordUnitsMaximum <= 0xFFFF &&
              alignof(UserAppearanceRecord) == 8);

UserAppearanceRecord* record_at(const UserAppearanceStore& s, u32 offset) noexcept {
    return reinterpret_cast<UserAppearanceRecord*>(s.records.arena.base +
                                                   static_cast<usize>(offset) * kUnitBytes);
}
bool before(const UserAppearanceRecord& a, u64 user, u64 guild) noexcept {
    return a.user_id < user || (a.user_id == user && a.guild_id < guild);
}
u32 locate(const UserAppearanceStore& s, u64 user, u64 guild) noexcept {
    const auto* order = stable_storage_data<u32>(s.order);
    u32 first = 0, last = s.count;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (before(*record_at(s, order[middle]), user, guild))
            first = middle + 1;
        else
            last = middle;
    }
    return first;
}
// The canonical record image: zero padding, no palette entries past the
// count, strings packed in field order. Returns the allocation units.
u32 encode(const UserAppearance& v, u8* image) noexcept {
    clear_bytes(image, kRecordHeaderBytes);
    auto& r = *reinterpret_cast<UserAppearanceRecord*>(image);
    r.user_id = v.user_id;
    r.guild_id = v.guild_id;
    r.public_flags = v.public_flags;
    r.primary_guild_id = v.primary_guild_id;
    r.status_emoji_id = v.status_emoji_id;
    r.nameplate_sku = v.nameplate_sku;
    r.fields = v.fields;
    r.name.font_id = v.name.font_id;
    r.name.effect_id = v.name.effect_id;
    r.name.color_count = minimum<u8>(v.name.color_count, 8);
    for (u32 i = 0; i < r.name.color_count; ++i)
        r.name.colors[i] = v.name.colors[i];
    r.bot = v.bot;
    r.primary_enabled = v.primary_enabled;
    r.emoji_animated = v.emoji_animated;
    u32 used = kRecordHeaderBytes;
    const auto put = [&](AppearanceText& field, const auto& text) noexcept {
        field.bytes = minimum<u16>(text.bytes, static_cast<u16>(sizeof(text.data)));
        field.offset = static_cast<u16>(used - static_cast<u32>(reinterpret_cast<u8*>(&field) - image));
        copy_bytes(image + used, text.data, field.bytes);
        used += field.bytes;
    };
    put(r.decoration, v.decoration);
    put(r.nameplate, v.nameplate);
    put(r.avatar, v.avatar);
    put(r.primary_badge, v.primary_badge);
    put(r.primary_tag, v.primary_tag);
    put(r.activity, v.activity);
    put(r.status_emoji, v.status_emoji);
    const u32 units = allocation_units(used);
    clear_bytes(image + used, units * kUnitBytes - used);
    r.bytes = static_cast<u16>(used);
    r.units = static_cast<u16>(units);
    return units;
}
// Slides live records down over dead ones, keeping their order entries.
void compact(UserAppearanceStore* s) noexcept {
    auto* order = stable_storage_data<u32>(s->order);
    u32 to = 0;
    for (u32 at = 0; at < s->used_units;) {
        auto* record = record_at(*s, at);
        const u32 units = record->units;
        if (record->user_id) {
            if (at != to) {
                order[locate(*s, record->user_id, record->guild_id)] = to;
                copy_bytes(record_at(*s, to), record, units * kUnitBytes);
            }
            to += units;
        }
        at += units;
    }
    s->used_units = to;
    s->garbage_units = 0;
}
// Room for one more allocation; may compact (moving every record).
bool reserve_units(UserAppearanceStore* s, u32 units) noexcept {
    if (s->used_units + units > s->records.reserved_capacity ||
        (s->garbage_units > kGarbageCompactUnits && s->garbage_units > s->used_units / 2))
        compact(s);
    return s->used_units + units <= s->records.reserved_capacity &&
           stable_storage_reserve(&s->records, s->used_units + units);
}
u32 append(UserAppearanceStore* s, const u8* image, u32 units) noexcept {
    const u32 offset = s->used_units;
    copy_bytes(record_at(*s, offset), image, units * kUnitBytes);
    s->used_units += units;
    return offset;
}
void retire(UserAppearanceStore* s, UserAppearanceRecord* record) noexcept {
    s->garbage_units += record->units;
    record->user_id = 0;
}
} // namespace
const UserAppearanceRecord* user_appearance_find(const UserAppearanceStore& s, u64 user,
                                                 u64 guild) noexcept {
    if (!s.count)
        return nullptr;
    const auto i = locate(s, user, guild);
    if (i >= s.count)
        return nullptr;
    const auto* record = record_at(s, stable_storage_data<u32>(s.order)[i]);
    return record->user_id == user && record->guild_id == guild ? record : nullptr;
}
const UserAppearanceRecord* user_appearance_find_activity(const UserAppearanceStore& s, u64 user) noexcept {
    if (!s.count || !user)
        return nullptr;
    const auto* order = stable_storage_data<u32>(s.order);
    for (u32 i = locate(s, user, 0); i < s.count; ++i) {
        const auto& row = *record_at(s, order[i]);
        if (row.user_id != user)
            break;
        if ((row.fields & appearance_activity) && (row.activity.bytes || row.status_emoji.bytes || row.status_emoji_id))
            return &row;
    }
    return nullptr;
}
void user_appearance_expand(const UserAppearanceRecord& r, UserAppearance* output) noexcept {
    if (!output)
        return;
    UserAppearance& a = *output;
    a = {};
    a.user_id = r.user_id;
    a.guild_id = r.guild_id;
    a.public_flags = r.public_flags;
    a.primary_guild_id = r.primary_guild_id;
    a.status_emoji_id = r.status_emoji_id;
    a.nameplate_sku = r.nameplate_sku;
    a.fields = r.fields;
    a.last_seen = r.last_seen;
    a.name = r.name;
    a.bot = r.bot;
    a.primary_enabled = r.primary_enabled;
    a.emoji_animated = r.emoji_animated;
    const auto get = [](auto& text, const AppearanceText& field) noexcept {
        text.bytes = minimum<u16>(field.bytes, static_cast<u16>(sizeof(text.data)));
        copy_bytes(text.data, field.view().data, text.bytes);
    };
    get(a.decoration, r.decoration);
    get(a.nameplate, r.nameplate);
    get(a.avatar, r.avatar);
    get(a.primary_badge, r.primary_badge);
    get(a.primary_tag, r.primary_tag);
    get(a.activity, r.activity);
    get(a.status_emoji, r.status_emoji);
}
bool user_appearance_upsert(UserAppearanceStore* s, const UserAppearance& v) noexcept {
    if (!s || !v.user_id || (v.fields & ~511u))
        return false;
    if (!v.fields)
        return true;
    const auto* found = user_appearance_find(*s, v.user_id, v.guild_id);
    // Empty optional fields carry clearing semantics without creating empty cold records.
    if (!found && !v.bot && !v.public_flags && !v.name.font_id && !v.name.effect_id &&
        !v.decoration.bytes && !v.nameplate_sku && !v.nameplate.bytes && !v.avatar.bytes &&
        !v.primary_enabled && !v.activity.bytes && !v.status_emoji.bytes && !v.status_emoji_id)
        return true;
    if (!s->records.arena.base &&
        (!stable_storage_initialize(&s->records, kArenaUnits, 0, kUnitBytes, kUnitBytes) ||
         !stable_storage_initialize(&s->order, maximum_records, 0, sizeof(u32), alignof(u32)))) {
        user_appearance_release(s);
        return false;
    }
    UserAppearance next{};
    if (found) {
        user_appearance_expand(*found, &next);
    } else {
        next.user_id = v.user_id;
        next.guild_id = v.guild_id;
    }
    if (v.fields & appearance_bot)
        next.bot = v.bot;
    if (v.fields & appearance_flags)
        next.public_flags = v.public_flags;
    if (v.fields & appearance_name)
        next.name = v.name;
    if (v.fields & appearance_decoration)
        next.decoration = v.decoration;
    if (v.fields & appearance_nameplate) {
        next.nameplate = v.nameplate;
        next.nameplate_sku = v.nameplate_sku;
    }
    if (v.fields & appearance_avatar)
        next.avatar = v.avatar;
    if (v.fields & appearance_guild_tag) {
        next.primary_guild_id = v.primary_guild_id;
        next.primary_badge = v.primary_badge;
        next.primary_tag = v.primary_tag;
        next.primary_enabled = v.primary_enabled;
    }
    if (v.fields & appearance_activity) {
        next.activity = v.activity;
        next.status_emoji = v.status_emoji;
        next.status_emoji_id = v.status_emoji_id;
        next.emoji_animated = v.emoji_animated;
        next.fields = (next.fields & ~u32{appearance_custom_status}) | (v.fields & appearance_custom_status);
    }
    next.fields |= v.fields;
    alignas(8) u8 image[kRecordUnitsMaximum * kUnitBytes];
    const u32 units = encode(next, image);
    auto& fresh = *reinterpret_cast<UserAppearanceRecord*>(image);
    auto* order = stable_storage_data<u32>(s->order);
    if (found) {
        auto* existing = const_cast<UserAppearanceRecord*>(found);
        fresh.last_seen = existing->last_seen;
        fresh.units = existing->units;
        if (existing->bytes == fresh.bytes && bytes_equal(existing, image, fresh.bytes)) {
            existing->last_seen = ++s->clock;
            return true;
        }
        if (units <= existing->units) {
            copy_bytes(existing, image, fresh.bytes);
        } else {
            fresh.units = static_cast<u16>(units);
            // Compaction may move the record; find it again afterwards.
            if (!reserve_units(s, units))
                return false;
            const u32 index = locate(*s, v.user_id, v.guild_id);
            existing = record_at(*s, order[index]);
            order[index] = append(s, image, units);
            retire(s, existing);
            existing = record_at(*s, order[index]);
        }
        existing->last_seen = ++s->clock;
        ++s->revision;
        return true;
    }
    if (s->count == maximum_records) {
        // Evict the least recently seen record.
        u32 oldest = 0;
        for (u32 i = 1; i < s->count; ++i)
            if (record_at(*s, order[i])->last_seen < record_at(*s, order[oldest])->last_seen)
                oldest = i;
        retire(s, record_at(*s, order[oldest]));
        for (u32 i = oldest; i + 1 < s->count; ++i)
            order[i] = order[i + 1];
        --s->count;
    } else if (!stable_storage_reserve(&s->order, s->count + 1)) {
        return false;
    }
    if (!reserve_units(s, units))
        return false;
    const u32 at = locate(*s, v.user_id, v.guild_id);
    for (u32 i = s->count; i > at; --i)
        order[i] = order[i - 1];
    order[at] = append(s, image, units);
    ++s->count;
    record_at(*s, order[at])->last_seen = ++s->clock;
    ++s->revision;
    return true;
}
void user_appearance_release(UserAppearanceStore* s) noexcept {
    if (!s)
        return;
    stable_storage_release(&s->records);
    stable_storage_release(&s->order);
    *s = {};
}

namespace {
struct AppearanceWriter {
    Slice<u8> output;
    u32 used = 0;
    bool valid = true;
    void put(const void* data, u32 count) noexcept {
        if (output.data) {
            if (used > output.count || count > output.count - used) {
                valid = false;
                return;
            }
            copy_bytes(output.data + used, data, count);
        }
        used += count;
    }
    template <typename T> void scalar(const T& v) noexcept {
        put(&v, sizeof(v));
    }
    template <u32 N> void text(const ProfileString<N>& s) noexcept {
        if (s.bytes > N) {
            valid = false;
            return;
        }
        scalar(s.bytes);
        put(s.data, s.bytes);
    }
};
struct AppearanceReader {
    ConstSlice<u8> input;
    u32 used = 0;
    bool valid = true;
    void get(void* data, u32 count) noexcept {
        if (used > input.count || count > input.count - used) {
            valid = false;
            return;
        }
        copy_bytes(data, input.data + used, count);
        used += count;
    }
    template <typename T> void scalar(T& v) noexcept {
        get(&v, sizeof(v));
    }
    template <u32 N> void text(ProfileString<N>& s) noexcept {
        scalar(s.bytes);
        if (s.bytes > N) {
            valid = false;
            return;
        }
        get(s.data, s.bytes);
        valid = valid && utf8_validate(s.view()).valid;
    }
};
} // namespace
u32 user_appearance_write(const UserAppearance& a, Slice<u8> output) noexcept {
    if (!a.fields || (a.fields & ~511u) || a.name.color_count > 8 || !output.data)
        return 0;
    AppearanceWriter w{output};
    w.scalar(a.fields);
    if (a.fields & appearance_bot)
        w.scalar(a.bot);
    if (a.fields & appearance_flags)
        w.scalar(a.public_flags);
    if (a.fields & appearance_name) {
        w.scalar(a.name.font_id);
        w.scalar(a.name.effect_id);
        w.scalar(a.name.color_count);
        w.put(a.name.colors, a.name.color_count * 4);
    }
    if (a.fields & appearance_decoration)
        w.text(a.decoration);
    if (a.fields & appearance_nameplate) {
        w.scalar(a.nameplate_sku);
        w.text(a.nameplate);
    }
    if (a.fields & appearance_avatar)
        w.text(a.avatar);
    if (a.fields & appearance_guild_tag) {
        w.scalar(a.primary_guild_id);
        w.scalar(a.primary_enabled);
        w.text(a.primary_badge);
        w.text(a.primary_tag);
    }
    if (a.fields & appearance_activity) {
        w.scalar(a.status_emoji_id);
        w.scalar(a.emoji_animated);
        w.text(a.status_emoji);
        w.text(a.activity);
    }
    return w.valid ? w.used : 0;
}
namespace {
bool read_appearance(ConstSlice<u8> input, u64 user, u64 guild, UserAppearance* output, u32* consumed) noexcept {
    if (!output || !user || (!input.data && input.count))
        return false;
    UserAppearance a{};
    a.user_id = user;
    a.guild_id = guild;
    if (!input.count) {
        *output = a;
        return true;
    }
    AppearanceReader r{input};
    r.scalar(a.fields);
    if (a.fields & ~511u)
        return false;
    if (a.fields & appearance_bot) {
        u8 v = 0;
        r.scalar(v);
        if (v > 1)
            return false;
        a.bot = v != 0;
    }
    if (a.fields & appearance_flags)
        r.scalar(a.public_flags);
    if (a.fields & appearance_name) {
        r.scalar(a.name.font_id);
        r.scalar(a.name.effect_id);
        r.scalar(a.name.color_count);
        if (a.name.color_count > 8)
            return false;
        r.get(a.name.colors, a.name.color_count * 4);
        for (u32 i = 0; i < a.name.color_count; ++i)
            if (a.name.colors[i] > 0xFFFFFF)
                return false;
    }
    if (a.fields & appearance_decoration)
        r.text(a.decoration);
    if (a.fields & appearance_nameplate) {
        r.scalar(a.nameplate_sku);
        r.text(a.nameplate);
    }
    if (a.fields & appearance_avatar)
        r.text(a.avatar);
    if (a.fields & appearance_guild_tag) {
        r.scalar(a.primary_guild_id);
        u8 v = 0;
        r.scalar(v);
        if (v > 1)
            return false;
        a.primary_enabled = v != 0;
        r.text(a.primary_badge);
        r.text(a.primary_tag);
    }
    if (a.fields & appearance_activity) {
        r.scalar(a.status_emoji_id);
        u8 v = 0;
        r.scalar(v);
        if (v > 1)
            return false;
        a.emoji_animated = v != 0;
        r.text(a.status_emoji);
        r.text(a.activity);
    }
    if (!r.valid || (!consumed && r.used != input.count))
        return false;
    if (consumed)
        *consumed = r.used;
    *output = a;
    return true;
}
} // namespace
bool user_appearance_read(ConstSlice<u8> input, u64 user, u64 guild, UserAppearance* output) noexcept {
    return read_appearance(input, user, guild, output, nullptr);
}
bool user_appearance_read_prefix(ConstSlice<u8> input, u64 user, u64 guild, UserAppearance* output,
                                 u32* consumed) noexcept {
    if (!consumed)
        return false;
    *consumed = 0;
    return read_appearance(input, user, guild, output, consumed);
}
} // namespace maicord
