#include "maicord/model/guild_detail_store.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

constexpr struct {
    Utf8View name;
    u64 bit;
} kFeatures[] = {
    {utf8_literal("MORE_EMOJI"), guild_feature_more_emoji},
    {utf8_literal("MORE_STICKERS"), guild_feature_more_stickers},
    {utf8_literal("ROLE_ICONS"), guild_feature_role_icons},
    {utf8_literal("BANNER"), guild_feature_banner},
    {utf8_literal("ANIMATED_BANNER"), guild_feature_animated_banner},
    {utf8_literal("COMMUNITY"), guild_feature_community},
    {utf8_literal("ACTIVITY_FEED_ENABLED_BY_USER"), guild_feature_activity_feed_enabled},
    {utf8_literal("ACTIVITY_FEED_DISABLED_BY_USER"), guild_feature_activity_feed_disabled},
    {utf8_literal("VANITY_URL"), guild_feature_vanity_url},
    {utf8_literal("INVITE_SPLASH"), guild_feature_invite_splash},
    {utf8_literal("CREATOR_MONETIZABLE"), guild_feature_creator_monetizable},
    {utf8_literal("CREATOR_MONETIZABLE_PROVISIONAL"), guild_feature_creator_monetizable_provisional},
    {utf8_literal("CREATOR_MONETIZABLE_DISABLED"), guild_feature_creator_monetizable_disabled},
    {utf8_literal("SUMMARIES_ENABLED_GA"), guild_feature_summaries_ga},
    {utf8_literal("SUMMARIES_ENABLED_BY_USER"), guild_feature_summaries_enabled},
};

GuildDetailRecord* find(GuildDetailStore* store, u64 guild_id) noexcept {
    for (u32 i = 0; i < store->count; ++i)
        if (store->records[i].guild_id == guild_id)
            return &store->records[i];
    return nullptr;
}

u32 region_bytes(const GuildDetailRecord& record) noexcept {
    return static_cast<u32>(record.description_bytes) + record.vanity_bytes + record.feature_names_bytes;
}

// Regions only ever move toward the bank start, so a forward copy is safe.
void move_down(char* destination, const char* source, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i)
        destination[i] = source[i];
}

// Places regions in bank order; equal offsets (empty regions) by index.
void compact(GuildDetailStore* store) noexcept {
    u32 used = 0;
    u64 floor = 0;
    for (u32 placed = 0; placed < store->count; ++placed) {
        u32 next = ~0u;
        u64 next_key = ~0ull;
        for (u32 i = 0; i < store->count; ++i) {
            const u64 key = static_cast<u64>(store->records[i].text_offset) << 32 | i;
            if (key >= floor && key < next_key) {
                next_key = key;
                next = i;
            }
        }
        if (next == ~0u)
            break;
        floor = next_key + 1;
        auto& record = store->records[next];
        const u32 bytes = region_bytes(record);
        move_down(store->text + used, store->text + record.text_offset, bytes);
        record.text_offset = used;
        used += bytes;
    }
    store->text_used = used;
}

} // namespace

u64 guild_feature_bit(Utf8View name) noexcept {
    for (const auto& feature : kFeatures)
        if (utf8_equal(name, feature.name))
            return feature.bit;
    return 0;
}

Utf8View guild_feature_name(u64 bit) noexcept {
    for (const auto& feature : kFeatures)
        if (feature.bit == bit)
            return feature.name;
    return {};
}

bool guild_feature_name_next(ConstSlice<u8> names, u32* cursor, Utf8View* name) noexcept {
    if (!cursor || !name || *cursor >= names.count || !names.data)
        return false;
    const u32 bytes = names[*cursor];
    if (!bytes || bytes > names.count - *cursor - 1)
        return false;
    *name = {reinterpret_cast<const char*>(names.data + *cursor + 1), bytes};
    *cursor += 1 + bytes;
    return true;
}

bool guild_detail_store_bind(GuildDetailStore* store, Slice<GuildDetailRecord> records, Slice<char> text) noexcept {
    if (!store || !records.data || !records.count || !text.data || !text.count)
        return false;
    *store = {records.data, text.data, records.count, 0, text.count, 0, 0};
    return true;
}

void guild_detail_store_clear(GuildDetailStore* store) noexcept {
    if (!store || (!store->count && !store->text_used))
        return;
    store->count = 0;
    store->text_used = 0;
    ++store->revision;
}

bool guild_detail_store_apply(GuildDetailStore* store, const GuildDetailInput& input) noexcept {
    if (!store || !store->records || !input.guild_id || (input.known & ~guild_detail_all) ||
        input.description.bytes > kGuildDetailDescriptionMaximumBytes ||
        input.vanity_url_code.bytes > kGuildDetailVanityMaximumBytes ||
        input.splash.bytes > kGuildDetailHashBytes || (input.splash.bytes && !input.splash.data) ||
        input.feature_names.count > 0xFFFF ||
        (input.feature_names.count && !input.feature_names.data))
        return false;
    u64 features = 0;
    if (input.known & guild_detail_features) {
        u32 cursor = 0, count = 0;
        Utf8View name{};
        while (guild_feature_name_next(input.feature_names, &cursor, &name)) {
            features |= guild_feature_bit(name);
            ++count;
        }
        if (cursor != input.feature_names.count || count != input.feature_count)
            return false;
    }
    GuildDetailRecord* record = find(store, input.guild_id);
    if (!record) {
        if (store->count >= store->capacity)
            return false;
        record = &store->records[store->count++];
        *record = {};
        record->guild_id = input.guild_id;
        record->text_offset = store->text_used;
    }
    const u16 k = input.known;
    if (k & guild_detail_premium_tier)
        record->premium_tier = input.premium_tier;
    if (k & guild_detail_boost_count)
        record->premium_subscription_count = input.premium_subscription_count;
    if (k & guild_detail_member_count)
        record->member_count = input.member_count;
    if (k & guild_detail_system_channel_flags)
        record->system_channel_flags = input.system_channel_flags;
    if (k & guild_detail_rules_channel)
        record->rules_channel_id = input.rules_channel_id;
    if (k & guild_detail_public_updates_channel)
        record->public_updates_channel_id = input.public_updates_channel_id;
    if (k & guild_detail_safety_alerts_channel)
        record->safety_alerts_channel_id = input.safety_alerts_channel_id;
    if (k & guild_detail_progress_bar)
        record->premium_progress_bar_enabled = input.premium_progress_bar_enabled;
    if (k & guild_detail_splash) {
        copy_bytes(record->splash, input.splash.data, input.splash.bytes);
        record->splash_bytes = static_cast<u8>(input.splash.bytes);
    }
    if (k & guild_detail_incidents) {
        record->invites_disabled_until_ms = input.invites_disabled_until_ms;
        record->dms_disabled_until_ms = input.dms_disabled_until_ms;
        record->raid_detected_at_ms = input.raid_detected_at_ms;
        record->dm_spam_detected_at_ms = input.dm_spam_detected_at_ms;
        record->lockdown_duration_hours = input.lockdown_duration_hours;
    }
    if (k & guild_detail_features)
        record->features = features;
    record->known = static_cast<u16>(record->known | k);

    // Text pieces: the new value where observed, else the stored one.
    const bool description_new = k & guild_detail_description;
    const bool vanity_new = k & guild_detail_vanity;
    const bool names_new = k & guild_detail_features;
    const char* base = store->text + record->text_offset;
    Utf8View description = description_new ? input.description : Utf8View{base, record->description_bytes};
    Utf8View vanity =
        vanity_new ? input.vanity_url_code : Utf8View{base + record->description_bytes, record->vanity_bytes};
    bool keep_names = names_new ? input.keep_feature_names : record->feature_names_kept;
    ConstSlice<u8> names{};
    u16 name_count = 0;
    if (keep_names) {
        names = names_new ? input.feature_names
                          : ConstSlice<u8>{reinterpret_cast<const u8*>(base + record->description_bytes +
                                                                       record->vanity_bytes),
                                           record->feature_names_bytes};
        name_count = names_new ? input.feature_count : record->feature_count;
    }
    const auto finish = [&]() noexcept {
        record->feature_count = name_count;
        record->feature_names_kept = keep_names;
        ++store->revision;
        return true;
    };
    // In place: the text before the names is unchanged and the names fit
    // where the old ones were.
    if (!description_new && !vanity_new && names.count <= record->feature_names_bytes) {
        if (names_new && names.count)
            copy_bytes(store->text + record->text_offset + record->description_bytes + record->vanity_bytes,
                       names.data, names.count);
        record->feature_names_bytes = static_cast<u16>(names.count);
        return finish();
    }
    // Otherwise the whole region moves to the end of the bank. Pieces kept
    // from the old region sit below text_used, so the copies never overlap.
    const bool carry = (!description_new && description.bytes) || (!vanity_new && vanity.bytes) ||
                       (!names_new && names.count);
    u32 needed = description.bytes + vanity.bytes + names.count;
    if (needed > store->text_capacity - store->text_used) {
        if (!carry) {
            record->description_bytes = record->feature_names_bytes = 0;
            record->vanity_bytes = 0;
        }
        const u32 before = record->text_offset;
        compact(store);
        const u32 shift = before - record->text_offset;
        if (!description_new && description.bytes)
            description.data -= shift;
        if (!vanity_new && vanity.bytes)
            vanity.data -= shift;
        if (!names_new && names.count)
            names.data -= shift;
    }
    if (needed > store->text_capacity - store->text_used && names.count) {
        needed -= names.count;
        names = {};
        name_count = 0;
        keep_names = false;
        record->known = static_cast<u16>(record->known & ~guild_detail_features);
        if (!description_new && !vanity_new) {
            // The kept description and vanity code stay where they are.
            record->feature_names_bytes = 0;
            return finish();
        }
    }
    if (needed > store->text_capacity - store->text_used && description.bytes) {
        needed -= description.bytes;
        description = {};
        record->known = static_cast<u16>(record->known & ~guild_detail_description);
    }
    if (needed > store->text_capacity - store->text_used)
        return false;
    const u32 offset = store->text_used;
    copy_bytes(store->text + offset, description.data, description.bytes);
    copy_bytes(store->text + offset + description.bytes, vanity.data, vanity.bytes);
    copy_bytes(store->text + offset + description.bytes + vanity.bytes, names.data, names.count);
    record->text_offset = offset;
    record->description_bytes = static_cast<u16>(description.bytes);
    record->vanity_bytes = static_cast<u8>(vanity.bytes);
    record->feature_names_bytes = static_cast<u16>(names.count);
    store->text_used = offset + needed;
    return finish();
}

bool guild_detail_store_remove(GuildDetailStore* store, u64 guild_id) noexcept {
    GuildDetailRecord* record = store ? find(store, guild_id) : nullptr;
    if (!record)
        return false;
    *record = store->records[--store->count];
    ++store->revision;
    return true;
}

const GuildDetailRecord* guild_detail_store_find(const GuildDetailStore& store, u64 guild_id) noexcept {
    return guild_id ? find(const_cast<GuildDetailStore*>(&store), guild_id) : nullptr;
}

Utf8View guild_detail_store_vanity_url_code(const GuildDetailStore& store, const GuildDetailRecord& record) noexcept {
    return {store.text + record.text_offset + record.description_bytes, record.vanity_bytes};
}

ConstSlice<u8> guild_detail_store_feature_names(const GuildDetailStore& store, const GuildDetailRecord& record) noexcept {
    if (!record.feature_names_kept)
        return {};
    return {reinterpret_cast<const u8*>(store.text + record.text_offset + record.description_bytes +
                                        record.vanity_bytes),
            record.feature_names_bytes};
}

} // namespace maicord
