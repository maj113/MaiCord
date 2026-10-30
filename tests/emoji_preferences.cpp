#include "maicord/protocol/emoji_preferences.hpp"
#include "maicord/model/reaction_preferences.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/ui/reaction_picker.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr u64 kDay = 24ull * 60 * 60 * 1000;
constexpr u64 kNow = 1'800'000'000'000ull;
constexpr u64 kBlob = 123456789012345678ull;

struct Wire {
    u8 bytes[4096];
    u32 used = 0;
    void number(u64 n) noexcept {
        do {
            const u8 b = static_cast<u8>(n & 127);
            n >>= 7;
            bytes[used++] = b | (n ? 128 : 0);
        } while (n);
    }
    void scalar(u32 field, u64 n) noexcept {
        number(field * 8);
        number(n);
    }
    void message(u32 field, ConstSlice<u8> input) noexcept {
        number(field * 8 + 2);
        number(input.count);
        copy_bytes(bytes + used, input.data, input.count);
        used += input.count;
    }
    void string(u32 field, Utf8View text) noexcept {
        message(field, {reinterpret_cast<const u8*>(text.data), text.bytes});
    }
    ConstSlice<u8> view() const noexcept {
        return {bytes, used};
    }
};

u16 catalog_index(Utf8View name) noexcept {
    const auto catalog = unicode_emoji_catalog();
    for (u32 i = 0; i < catalog.count; ++i)
        if (utf8_equal(catalog[i].name(), name))
            return static_cast<u16>(i);
    return kReactionPreferenceInvalidCatalogIndex;
}

void frecency_entry(Wire& group, Utf8View key, u32 total, ConstSlice<u64> recent) noexcept {
    Wire packed{}, item{}, entry{};
    for (u32 i = 0; i < recent.count; ++i)
        packed.number(recent[i]);
    item.scalar(1, total);
    item.message(2, packed.view());
    item.scalar(3, 77);
    entry.string(1, key);
    entry.message(2, item.view());
    group.message(1, entry.view());
}

struct Keys {
    char text[8][96];
    u32 bytes[8];
    u32 count;
};
bool collect(void* context, Utf8View key) noexcept {
    auto& keys = *static_cast<Keys*>(context);
    if (keys.count < 8) {
        copy_bytes(keys.text[keys.count], key.data, key.bytes);
        keys.bytes[keys.count++] = key.bytes;
    }
    return true;
}
bool has(const Keys& keys, Utf8View key) noexcept {
    for (u32 i = 0; i < keys.count; ++i)
        if (utf8_equal({keys.text[i], keys.bytes[i]}, key))
            return true;
    return false;
}
struct Items {
    Keys keys;
    EmojiFrecencyItem items[8];
};
bool collect_item(void* context, Utf8View key, const EmojiFrecencyItem& item) noexcept {
    auto& items = *static_cast<Items*>(context);
    if (items.keys.count < 8)
        items.items[items.keys.count] = item;
    return collect(&items.keys, key);
}
u8 scratch[3 * (kEmojiPreferencesMaximumGroupBytes + 8) + 32];
u8 json[kEmojiPreferencesMaximumPatchBytes];

// The application's identity: custom IDs, or a tagged catalog index.
u64 identity(void*, Utf8View key) noexcept {
    const auto emoji = reaction_emoji_from_settings_key(key);
    return !emoji.valid() ? 0 : emoji.custom() ? emoji.custom_id : (1ull << 63) | emoji.catalog_index;
}

void test_model(Check check) noexcept {
    ReactionPreferenceStore store{};
    check(reaction_preferences_initialize(&store), "emoji preferences: store initializes");
    const u16 thumbs = catalog_index(utf8_literal("thumbs_up"));
    const auto custom = reaction_emoji_custom(kBlob);
    // Reaction frecency: trunc(1000 * (total/max * 0.2 + score/1000 * 0.8)).
    reaction_preferences_track_emoji(&store, ReactionPreferenceKind::reaction, reaction_emoji_unicode(thumbs),
                                     kNow - 20 * kDay);
    reaction_preferences_track_emoji(&store, ReactionPreferenceKind::reaction, reaction_emoji_unicode(thumbs),
                                     kNow - 20 * kDay);
    reaction_preferences_track_emoji(&store, ReactionPreferenceKind::reaction, custom, kNow - kDay);
    auto frequent = reaction_preferences_frequently(store, ReactionPreferenceKind::reaction);
    const auto& table = store.tables[0];
    u32 custom_frecency = 0, thumbs_frecency = 0;
    for (const auto& usage : table.usage) {
        if (usage.occupied && usage.emoji == custom)
            custom_frecency = usage.frecency;
        if (usage.occupied && usage.emoji == reaction_emoji_unicode(thumbs))
            thumbs_frecency = usage.frecency;
    }
    // custom: 1000*(1/2*0.2 + 100/1000*0.8) = 180; thumbs: 1000*(0.2 + 100/1000*0.8) = 280.
    check(frequent.count == 2 && custom_frecency == 180 && thumbs_frecency == 280 &&
              frequent[0] == reaction_emoji_unicode(thumbs) && frequent[1] == custom,
          "emoji preferences: reaction frecency decays by age and keeps custom identities");
    // Composer frecency: ceil(total * score / samples).
    reaction_preferences_track_emoji(&store, ReactionPreferenceKind::message, custom, kNow - 50 * kDay);
    reaction_preferences_track_emoji(&store, ReactionPreferenceKind::message, custom, kNow);
    reaction_preferences_track_emoji(&store, ReactionPreferenceKind::message, reaction_emoji_unicode(thumbs), kNow);
    frequent = reaction_preferences_frequently(store, ReactionPreferenceKind::message);
    check(frequent.count == 2 && frequent[0] == custom && store.tables[1].usage[0].frecency == 110 &&
              store.pending_use_count == 6,
          "emoji preferences: composer frecency uses total times mean sample weight");
    // Seeds stay local.
    reaction_preferences_track_emoji(&store, ReactionPreferenceKind::message, reaction_emoji_unicode(1), kNow, 1,
                                     false);
    check(store.pending_use_count == 6, "emoji preferences: seeded uses are not queued for sync");

    // Favorite edits survive an authoritative snapshot until committed.
    check(reaction_preferences_toggle_favorite_emoji(&store, custom).favorite &&
              reaction_preferences_is_favorite_emoji(store, custom) && store.favorite_edit_count == 1,
          "emoji preferences: custom emoji favorite toggles locally");
    ReactionEmojiRef remote_favorites[] = {reaction_emoji_unicode(thumbs)};
    ReactionPreferenceRemoteUsage remote{};
    remote.emoji = reaction_emoji_unicode(5);
    remote.total_uses = 9;
    remote.recent_count = 1;
    remote.recent_uses[0] = kNow - kDay;
    check(reaction_preferences_apply_remote(&store, {remote_favorites, 1}, {&remote, 1}, {}, kNow) &&
              store.favorite_count == 2 && store.favorites[0] == reaction_emoji_unicode(thumbs) &&
              store.favorites[1] == custom,
          "emoji preferences: remote favorites replace local ones and replay pending edits");
    bool replayed = false;
    for (const auto& usage : store.tables[0].usage)
        replayed |= usage.occupied && usage.emoji == custom && usage.total_uses == 1;
    check(store.tables[0].usage_count == 3 && replayed && store.tables[1].usage_count == 2,
          "emoji preferences: remote frecency replaces tables and replays unsent uses");
    reaction_preferences_commit(&store, 1, store.pending_use_count);
    ReactionEmojiRef none[] = {reaction_emoji_unicode(thumbs)};
    reaction_preferences_apply_remote(&store, {none, 1}, {}, {}, kNow);
    check(store.favorite_count == 1 && !store.tables[0].usage_count && !store.tables[0].seeded &&
              !store.pending_use_count,
          "emoji preferences: committed edits stop replaying and empty tables reseed");

    // Eviction keeps the most recently used entries, as the official client serializes them.
    ReactionPreferenceStore bounded{};
    reaction_preferences_initialize(&bounded);
    for (u32 i = 0; i < kReactionPreferenceUsageCapacity; ++i)
        for (u32 use = 0; use < (i == 0 ? 5u : 1u); ++use)
            reaction_preferences_track_emoji(&bounded, ReactionPreferenceKind::reaction,
                                             reaction_emoji_unicode(static_cast<u16>(i)), kNow + i, 1, false);
    reaction_preferences_track_emoji(&bounded, ReactionPreferenceKind::reaction, custom, kNow + 500, 1, false);
    bool oldest = false, newest = false;
    for (const auto& usage : bounded.tables[0].usage) {
        oldest |= usage.occupied && usage.emoji == reaction_emoji_unicode(0);
        newest |= usage.occupied && usage.emoji == custom;
    }
    check(bounded.tables[0].usage_count == kReactionPreferenceUsageCapacity && !oldest && newest,
          "emoji preferences: full history evicts the least recently used emoji");
    reaction_preferences_release(&bounded);
    reaction_preferences_release(&store);
}

void test_keys(Check check) noexcept {
    const u16 thumbs = catalog_index(utf8_literal("thumbs_up"));
    const u16 hundred = catalog_index(utf8_literal("hundred_points"));
    char key[96]{};
    const u32 bytes = reaction_emoji_settings_key(reaction_emoji_unicode(thumbs), {key, 96});
    check(utf8_equal({key, bytes}, utf8_literal("thumbsup")) &&
              reaction_emoji_from_settings_key(utf8_literal("thumbsup")) == reaction_emoji_unicode(thumbs) &&
              reaction_emoji_from_settings_key(utf8_literal("+1")) == reaction_emoji_unicode(thumbs),
          "emoji preferences: new Unicode keys use the joypixels name and parsing accepts every alias");
    check(reaction_emoji_from_settings_key(utf8_literal("100")) == reaction_emoji_unicode(hundred),
          "emoji preferences: numeric Unicode names are not custom snowflakes");
    // Uniform tones key as name_toneN, listed mixed tones by their joypixels name.
    const u16 thumbs_tone2 = catalog_index(utf8_literal("thumbs_up_medium_light_skin_tone"));
    const u16 mixed = catalog_index(utf8_literal("handshake_light_skin_tone_dark_skin_tone"));
    const u16 crowned = catalog_index(utf8_literal("person_with_crown"));
    const auto key_is = [&](u16 index, Utf8View expected) noexcept {
        const u32 count = reaction_emoji_settings_key(reaction_emoji_unicode(index), {key, 96});
        return utf8_equal({key, count}, expected) &&
               reaction_emoji_from_settings_key({key, count}) == reaction_emoji_unicode(index);
    };
    check(key_is(thumbs_tone2, utf8_literal("thumbsup_tone2")) &&
              key_is(mixed, utf8_literal("handshake_tone1_tone5")) &&
              key_is(crowned, utf8_literal("person_with_crown")) &&
              reaction_emoji_from_settings_key(utf8_literal("+1::skin-tone-2")) == reaction_emoji_unicode(thumbs_tone2) &&
              reaction_emoji_from_settings_key(utf8_literal("thumbs_up_tone2")) == reaction_emoji_unicode(thumbs_tone2) &&
              !reaction_emoji_from_settings_key(utf8_literal("thumbsup_tone6")).valid() &&
              !reaction_emoji_from_settings_key(utf8_literal("100_tone2")).valid(),
          "emoji preferences: skin-tone keys round-trip through Unicode tone variants");
    // Names other clients store resolve, whichever alias set they come from.
    const auto resolves = [](const char* key, const char* catalog_name) noexcept {
        const Utf8View name{catalog_name, c_string_length(catalog_name)};
        const Utf8View text{key, c_string_length(key)};
        const u16 index = catalog_index(name);
        return index != kReactionPreferenceInvalidCatalogIndex &&
               reaction_emoji_from_settings_key(text) == reaction_emoji_unicode(index);
    };
    check(resolves("slight_smile", "slightly_smiling_face") && resolves("upside_down", "upside_down_face") &&
              resolves("face_palm", "person_facepalming") && resolves("skull_crossbones", "skull_and_crossbones") &&
              resolves("poo", "pile_of_poo") && resolves("thumbup", "thumbs_up") &&
              resolves("drool", "drooling_face") && resolves("first_place", "1st_place_medal") &&
              resolves("pouting_face", "enraged_face") && resolves("beach_umbrella", "umbrella_on_ground") &&
              resolves("city_sunset", "sunset") && resolves("cat", "cat_face") &&
              resolves("\xF0\x9F\x98\x82", "face_with_tears_of_joy"),
          "emoji preferences: familiar, gemoji, catalog and raw-sequence keys resolve");
    const u32 custom_bytes = reaction_emoji_settings_key(reaction_emoji_custom(kBlob), {key, 96});
    check(utf8_equal({key, custom_bytes}, utf8_literal("123456789012345678")) &&
              reaction_emoji_from_settings_key({key, custom_bytes}) == reaction_emoji_custom(kBlob) &&
              !reaction_emoji_from_settings_key(utf8_literal("not_an_emoji_name")).valid(),
          "emoji preferences: custom keys are decimal IDs and unknown names stay unresolved");
    ReactionEmojiRef found[8]{};
    const u32 count = reaction_emoji_scan_message(
        utf8_literal("hi <:blob:123456789012345678> \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD and \xF0\x9F\x91\x8D "
                     "\xF0\x9F\x91\x8D 1\xEF\xB8\x8F\xE2\x83\xA3 12 <a:x:987654321098765432>"),
        {found, 8});
    check(count == 5 && found[0] == reaction_emoji_custom(kBlob) && !found[1].custom() &&
              found[2] == reaction_emoji_unicode(thumbs) && !found[3].custom() &&
              found[4] == reaction_emoji_custom(987654321098765432ull) && found[1] != found[2],
          "emoji preferences: sent content yields unique custom, toned, plain and keycap emoji");
}

void test_protocol(Check check) noexcept {
    Wire favorites{}, frecency{}, reaction{}, versions{}, root{};
    favorites.string(1, utf8_literal("thumbsup"));
    favorites.scalar(7, 1); // unknown field preserved
    favorites.string(1, utf8_literal("heart"));
    const u64 recent[] = {kNow - 2 * kDay, kNow - kDay};
    frecency_entry(frecency, utf8_literal("thumbsup"), 4, {recent, 2});
    frecency_entry(frecency, utf8_literal("future_name_unknown"), 2, {recent, 1});
    frecency.scalar(9, 3);
    frecency_entry(reaction, utf8_literal("123456789012345678"), 3, {recent, 2});
    Wire stickers{};
    frecency_entry(stickers, utf8_literal("749054660769218631"), 2, {recent, 2});
    versions.scalar(1, 5);
    versions.scalar(3, 41);
    root.message(1, versions.view());
    root.string(2, utf8_literal("gifs-are-opaque"));
    root.message(4, stickers.view());
    root.message(5, favorites.view());
    root.message(6, frecency.view());
    root.message(13, reaction.view());
    root.scalar(99, 1);
    EmojiPreferencesSnapshot snapshot{};
    const auto measured = emoji_preferences_decode(root.view(), {}, &snapshot);
    u8 bank[1024]{};
    const auto decoded = emoji_preferences_decode(root.view(), {bank, measured.group_bytes}, &snapshot);
    check(measured.ok() && decoded.ok() && snapshot.data_version == 41 && snapshot.versions_present &&
              snapshot.favorites.count == favorites.used && snapshot.frecency.count == frecency.used &&
              snapshot.reaction_frecency.count == reaction.used && snapshot.sticker_frecency.count == stickers.used,
          "emoji preferences: decode isolates groups 4, 5, 6 and 13 with the data version");
    Items sticker_items{};
    check(emoji_preferences_visit_frecency(snapshot.sticker_frecency, collect_item, &sticker_items) ==
                  EmojiPreferencesError::none &&
              sticker_items.keys.count == 1 && sticker_items.items[0].recent_count == 2,
          "emoji preferences: sticker frecency entries decode by sticker ID");
    Wire bad_stickers{}, tolerant{};
    bad_stickers.scalar(1, 5);
    tolerant.message(4, bad_stickers.view());
    tolerant.message(6, frecency.view());
    EmojiPreferencesSnapshot partial{};
    u8 partial_bank[1024]{};
    const auto partial_size = emoji_preferences_decode(tolerant.view(), {}, &partial);
    check(partial_size.ok() && partial_size.group_bytes == frecency.used &&
              emoji_preferences_decode(tolerant.view(), {partial_bank, partial_size.group_bytes}, &partial).ok() &&
              !partial.sticker_frecency.count && partial.frecency.count == frecency.used,
          "emoji preferences: a malformed sticker group is dropped without failing the emoji groups");
    Keys keys{};
    Items items{};
    check(emoji_preferences_visit_favorites(snapshot.favorites, collect, &keys) == EmojiPreferencesError::none &&
              keys.count == 2 &&
              emoji_preferences_visit_frecency(snapshot.frecency, collect_item, &items) ==
                  EmojiPreferencesError::none &&
              items.keys.count == 2 && items.items[0].total_uses == 4 && items.items[0].recent_count == 2 &&
              items.items[0].recent_uses[1] == kNow - kDay && items.items[0].frecency == 77,
          "emoji preferences: favorites and packed frecency samples decode");
    Wire truncated{};
    truncated.message(5, favorites.view());
    truncated.used -= 2;
    check(emoji_preferences_decode(truncated.view(), {}, &snapshot).error == EmojiPreferencesError::protobuf,
          "emoji preferences: malformed protos are rejected");

    emoji_preferences_decode(root.view(), {bank, measured.group_bytes}, &snapshot);
    const EmojiFavoriteEdit edits[] = {{utf8_literal("123456789012345678"), true},
                                       {utf8_literal("heart"), false}};
    EmojiFrecencyEntry ours{};
    ours.key = utf8_literal("thumbsup");
    ours.item.total_uses = 5;
    ours.item.recent_count = 1;
    ours.item.recent_uses[0] = kNow;
    ours.item.frecency = 360;
    ours.item.score = 100;
    ours.updated = true;
    EmojiPreferencesPatch patch{};
    patch.observed_favorites = snapshot.favorites;
    patch.favorite_edits = {edits, 2};
    patch.observed_frecency = snapshot.frecency;
    patch.observed_reaction_frecency = snapshot.reaction_frecency;
    patch.frecency = {&ours, 1};
    patch.identity = identity;
    patch.required_data_version = 41;
    patch.write_frecency = true;
    const auto written = emoji_preferences_write_patch(patch, {scratch, sizeof(scratch)}, {json, sizeof(json)});
    const Utf8View text{reinterpret_cast<const char*>(json), written.bytes};
    check(written.ok() && written.favorite_count == 2 &&
              emoji_preferences_validate_patch_json(text) == EmojiPreferencesError::none &&
              text.bytes > 29 && bytes_equal(text.data + text.bytes - 29, "\",\"required_data_version\":41}", 29),
          "emoji preferences: patch is a validated conditional envelope");
    // Re-decode the written proto to prove what is (and is not) sent.
    EmojiPreferencesSnapshot sent{};
    u8 sent_bank[2048]{};
    const auto sent_result = emoji_preferences_decode({scratch, written.proto_bytes}, {sent_bank, 2048}, &sent);
    Keys sent_favorites{};
    Items sent_frecency{}, sent_reaction{};
    emoji_preferences_visit_favorites(sent.favorites, collect, &sent_favorites);
    emoji_preferences_visit_frecency(sent.frecency, collect_item, &sent_frecency);
    emoji_preferences_visit_frecency(sent.reaction_frecency, collect_item, &sent_reaction);
    bool unknown_favorite_field = false, unknown_group_field = false, gif_sent = false;
    for (u32 i = 0; i + 1 < sent.favorites.count; ++i)
        unknown_favorite_field |= sent.favorites[i] == 7 * 8 && sent.favorites[i + 1] == 1;
    for (u32 i = 0; i + 1 < sent.frecency.count; ++i)
        unknown_group_field |= sent.frecency[i] == 9 * 8 && sent.frecency[i + 1] == 3;
    for (u32 i = 0; i < written.proto_bytes; ++i)
        gif_sent |= scratch[i] == 'g' && i + 3 < written.proto_bytes && scratch[i + 1] == 'i' && scratch[i + 2] == 'f';
    check(sent_result.ok() && !sent.versions_present && !gif_sent && sent_favorites.count == 2 &&
              has(sent_favorites, utf8_literal("thumbsup")) && has(sent_favorites, utf8_literal("123456789012345678")) &&
              !has(sent_favorites, utf8_literal("heart")) && unknown_favorite_field,
          "emoji preferences: favorite edits rebase on observed entries and keep unknown fields");
    check(sent_frecency.keys.count == 2 && has(sent_frecency.keys, utf8_literal("future_name_unknown")) &&
              sent_frecency.items[0].total_uses == 5 && sent_frecency.items[0].frecency == 360 &&
              unknown_group_field && sent_reaction.keys.count == 0,
          "emoji preferences: frecency updates represented entries in place and keeps unresolved ones");
    // The favorite limit is enforced before a write.
    static Wire full{};
    full.used = 0;
    char name[16]{};
    for (u32 i = 0; i < kEmojiPreferencesMaximumFavorites; ++i) {
        u32 n = i, bytes = 0;
        name[bytes++] = 'e';
        do {
            name[bytes++] = static_cast<char>('0' + n % 10);
            n /= 10;
        } while (n);
        full.string(1, {name, bytes});
    }
    EmojiPreferencesPatch overflow{};
    const EmojiFavoriteEdit add[] = {{utf8_literal("thumbsup"), true}};
    overflow.observed_favorites = full.view();
    overflow.favorite_edits = {add, 1};
    check(emoji_preferences_write_patch(overflow, {scratch, sizeof(scratch)}, {json, sizeof(json)}).error ==
              EmojiPreferencesError::capacity,
          "emoji preferences: adding past 250 favorites is refused");
    check(emoji_preferences_validate_patch_json(utf8_literal("{\"settings\":\"EgA=\",\"required_data_version\":1}")) ==
              EmojiPreferencesError::envelope,
          "emoji preferences: patches carrying other settings groups are rejected");
}

bool same_bytes(ConstSlice<u8> sent, const Wire& expected) noexcept {
    return sent.count == expected.used && bytes_equal(sent.data, expected.bytes, expected.used);
}

// Keys another client wrote ("thumbup", a raw sequence, gemoji names) are sent
// back exactly as received, even though this client names the emoji otherwise.
void test_interop(Check check) noexcept {
    const u64 recent[] = {kNow - 2 * kDay, kNow - kDay};
    Wire favorites{}, frecency{}, reaction{}, root{};
    favorites.string(1, utf8_literal("slightly_smiling_face"));
    favorites.string(1, utf8_literal("thumbup"));
    favorites.scalar(7, 1);
    favorites.string(1, utf8_literal("future_name_unknown"));
    favorites.string(1, utf8_literal("123456789012345678"));
    frecency_entry(frecency, utf8_literal("thumbup"), 4, {recent, 2});
    frecency_entry(frecency, utf8_literal("\xF0\x9F\x92\xA9"), 3, {recent, 1});
    frecency_entry(frecency, utf8_literal("future_name_unknown"), 2, {recent, 1});
    frecency.scalar(9, 3);
    frecency_entry(frecency, utf8_literal("skull_and_crossbones"), 1, {recent, 1});
    frecency_entry(reaction, utf8_literal("slightly_smiling_face"), 6, {recent, 2});
    root.message(5, favorites.view());
    root.message(6, frecency.view());
    root.message(13, reaction.view());
    EmojiPreferencesSnapshot snapshot{};
    u8 bank[1024]{};
    const bool decoded = emoji_preferences_decode(root.view(), {bank, sizeof(bank)}, &snapshot).ok();

    // The local tables hold the same emoji under this client's own keys.
    char keys[6][96]{};
    const auto entry = [&](u32 slot, const char* name, bool updated) noexcept {
        const Utf8View text{name, c_string_length(name)};
        EmojiFrecencyEntry result{};
        result.key = {keys[slot], reaction_emoji_settings_key(reaction_emoji_unicode(catalog_index(text)),
                                                              {keys[slot], 96})};
        result.item.total_uses = 10;
        result.item.recent_count = 1;
        result.item.recent_uses[0] = kNow;
        result.item.frecency = 500;
        result.updated = updated;
        return result;
    };
    EmojiFrecencyEntry message[] = {entry(0, "thumbs_up", false), entry(1, "pile_of_poo", false),
                                    entry(2, "skull_and_crossbones", false)};
    EmojiFrecencyEntry reactions[] = {entry(3, "slightly_smiling_face", false)};
    const EmojiFrecencyEntry grin = entry(4, "grinning_face", true);
    const bool own_keys = utf8_equal(message[0].key, utf8_literal("thumbsup")) &&
                          utf8_equal(message[1].key, utf8_literal("poop")) &&
                          utf8_equal(message[2].key, utf8_literal("skull_crossbones")) &&
                          utf8_equal(reactions[0].key, utf8_literal("slight_smile")) &&
                          utf8_equal(grin.key, utf8_literal("grinning"));
    const EmojiFavoriteEdit adds[] = {{grin.key, true}, {message[0].key, true}};
    EmojiPreferencesPatch patch{};
    patch.observed_favorites = snapshot.favorites;
    patch.observed_frecency = snapshot.frecency;
    patch.observed_reaction_frecency = snapshot.reaction_frecency;
    patch.favorite_edits = {adds, 2};
    patch.frecency = {message, 3};
    patch.reaction_frecency = {reactions, 1};
    patch.identity = identity;
    patch.write_frecency = true;
    EmojiPreferencesSnapshot sent{};
    u8 sent_bank[2048]{};
    auto written = emoji_preferences_write_patch(patch, {scratch, sizeof(scratch)}, {json, sizeof(json)});
    bool sent_ok = written.ok() &&
                   emoji_preferences_decode({scratch, written.proto_bytes}, {sent_bank, sizeof(sent_bank)}, &sent).ok();
    Wire appended = favorites;
    appended.string(1, utf8_literal("grinning"));
    check(decoded && own_keys && sent_ok && written.favorite_count == 5 && same_bytes(sent.favorites, appended) &&
              same_bytes(sent.frecency, frecency) && same_bytes(sent.reaction_frecency, reaction),
          "emoji preferences: untouched foreign keys re-encode byte for byte; an add only appends");

    // A use updates the value under the observed key, an emoji the client no
    // longer holds drops out, a new emoji is appended under this client's key,
    // and a removal finds the favorite by emoji rather than by spelling.
    message[0].updated = true;
    message[2] = grin;
    const EmojiFavoriteEdit removal[] = {{reactions[0].key, false}};
    patch.favorite_edits = {removal, 1};
    written = emoji_preferences_write_patch(patch, {scratch, sizeof(scratch)}, {json, sizeof(json)});
    sent_ok = written.ok() &&
              emoji_preferences_decode({scratch, written.proto_bytes}, {sent_bank, sizeof(sent_bank)}, &sent).ok();
    Wire kept{}, updated{}, item{}, packed{}, single{};
    kept.string(1, utf8_literal("thumbup"));
    kept.scalar(7, 1);
    kept.string(1, utf8_literal("future_name_unknown"));
    kept.string(1, utf8_literal("123456789012345678"));
    packed.number(kNow);
    item.scalar(1, 10);
    item.message(2, packed.view());
    item.scalar(3, 500);
    single.string(1, utf8_literal("thumbup"));
    single.message(2, item.view());
    updated.message(1, single.view());
    frecency_entry(updated, utf8_literal("\xF0\x9F\x92\xA9"), 3, {recent, 1});
    frecency_entry(updated, utf8_literal("future_name_unknown"), 2, {recent, 1});
    updated.scalar(9, 3);
    single.used = 0;
    single.string(1, utf8_literal("grinning"));
    single.message(2, item.view());
    updated.message(1, single.view());
    check(sent_ok && written.favorite_count == 3 && same_bytes(sent.favorites, kept) &&
              same_bytes(sent.frecency, updated) && same_bytes(sent.reaction_frecency, reaction),
          "emoji preferences: updates keep observed keys, evictions drop, removals match by emoji");
}
} // namespace

void test_emoji_preferences(Check check) noexcept {
    test_model(check);
    test_keys(check);
    test_protocol(check);
    test_interop(check);
}
} // namespace maicord
