#include "maicord/model/guild/server_tag.hpp"
#include "maicord/model/guild/detail_store.hpp"
#include "maicord/core/memory.hpp"
#include "../../support/test_output.hpp"

// Server Tag (Server Settings): the tag fields of GET/PATCH /guilds/{g}/profile, the page's draft and badge
// catalogue, and PUT /users/@me/clan (Userdoccers: Discovery > Guild Profile Structure, Modify Guild Profile and
// Guild Badge Type; Users > Set Guild Identity).
namespace maicord {
namespace server_tag_test {

u32 failures;
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}

constexpr u64 kGuild = 100;

bool same(Utf8View a, const char* b) noexcept {
    u32 length = 0;
    while (b[length])
        ++length;
    return a.bytes == length && bytes_equal(a.data, b, length);
}

// The request's target and body as the account layer writes them.
bool request_equals(const GuildAdminRequest& request, RestMethod method, const char* target,
                    const char* body) noexcept {
    if (!guild_admin_request_valid(request) || guild_admin_method(request.operation) != method)
        return false;
    u8 path[kGuildAdminMaximumTargetBytes], json[512];
    const auto t = guild_admin_write_target(request, {path, sizeof(path)});
    const auto b = guild_admin_write_body(request, {json, sizeof(json)});
    return t.ok() && b.ok() && same({reinterpret_cast<const char*>(path), t.bytes}, target) &&
           same({reinterpret_cast<const char*>(json), b.bytes}, body);
}

bool decode(const char* body, GuildAdminProfile* profile) noexcept {
    static u32 scratch[1024];
    u32 length = 0;
    while (body[length])
        ++length;
    return guild_admin_decode_profile({body, length}, {scratch, 1024}, profile).ok();
}

void test_decode() noexcept {
    GuildAdminProfile profile{};
    check(decode(R"({"id":"100","name":"Bugs","tag":"BUG","badge":6,"badge_color_primary":"#32a070",)"
                 R"("badge_color_secondary":"#57b59e","badge_hash":"6082c2553b03b47ccaea5203567df3cf",)"
                 R"("visibility":1})",
                 &profile) &&
              same({profile.tag, profile.tag_bytes}, "BUG") && profile.badge_known && profile.badge == 6 &&
              same({profile.badge_color_primary, profile.badge_color_primary_bytes}, "#32a070") &&
              same({profile.badge_color_secondary, profile.badge_color_secondary_bytes}, "#57b59e") &&
              same({profile.badge_hash, profile.badge_hash_bytes}, "6082c2553b03b47ccaea5203567df3cf"),
          "decode: the profile's tag, badge, badge colours and badge hash are read");
    check(decode(R"({"name":"Bugs","tag":null,"badge":null,"badge_color_primary":null,"badge_color_secondary":null})",
                 &profile) &&
              !profile.tag_bytes && !profile.badge_known && !profile.badge_color_primary_bytes &&
              !profile.badge_color_secondary_bytes,
          "decode: null tag fields read as no tag and the badge's own colours");
    check(!decode(R"({"tag":"A","tag":"B"})", &profile), "decode: a duplicate tag key is refused");
}

void test_draft() noexcept {
    GuildAdminProfile profile{};
    check(decode(R"({"tag":"BUG","badge":6,"badge_color_primary":"#32a070","badge_color_secondary":"#57b59e"})",
                 &profile),
          "draft: fixture decodes");
    auto base = guild_tag_draft(profile);
    check(base.enabled && base.badge == 6 && base.primary_set && base.primary == 0x32a070 && base.secondary_set &&
              base.secondary == 0x57b59e && guild_tag_palette_choice(base) == 3,
          "draft: a stored tag is on, and its colour pair selects the matching preset (teal and mint green)");
    auto draft = base;
    draft.enabled = false;
    check(!guild_tag_draft_equal(base, draft) && guild_tag_renamed(draft, profile),
          "draft: turning the tag off is a change and drops the stored tag");
    draft.enabled = true;
    check(guild_tag_draft_equal(base, draft), "draft: turning it back on restores the kept text");
    guild_tag_set_text(&draft, utf8_literal("BUGS"));
    check(guild_tag_renamed(draft, profile) && !guild_tag_draft_equal(base, draft),
          "draft: a new tag text is a rename (members adopt it again)");
    guild_tag_set_palette(&draft, kGuildTagPaletteDefault);
    check(!draft.primary_set && !draft.secondary_set && guild_tag_palette_choice(draft) == kGuildTagPaletteDefault &&
              guild_tag_primary(draft) == guild_tag_badge(6)->primary,
          "draft: Reset to default colors clears both, and the badge shows its own colour");
    draft.primary = 0x123456;
    draft.primary_set = true;
    check(guild_tag_palette_choice(draft) == kGuildTagPaletteCustom,
          "draft: a colour outside the presets selects the custom tile");
    // The server's own image of its badge stands in for exactly the tile that shows the saved badge in its saved colours.
    auto sword = base;
    sword.badge = 0;
    auto plain = base;
    guild_tag_set_palette(&plain, kGuildTagPaletteDefault);
    const auto* leaf = guild_tag_badge(6);
    check(guild_tag_badge_saved(base, 6, 0x32a070, 0x57b59e) && guild_tag_badge_saved(base, 6, 0x32a070, 0) &&
              !guild_tag_badge_saved(base, 6, 0x32a071, 0x57b59e) && !guild_tag_badge_saved(base, 0, 0x32a070, 0x57b59e) &&
              !guild_tag_badge_saved(base, 15, 0x32a070, 0x57b59e),
          "saved badge: the same badge in the same primary colour, and no other badge or colour (a one-colour badge has no "
          "secondary to match)");
    check(guild_tag_badge_saved(sword, 0, 0x32a070, 0x57b59e) && !guild_tag_badge_saved(sword, 0, 0x32a070, 0x57b59f) &&
              !guild_tag_badge_saved(sword, 0, 0x32a071, 0x57b59e),
          "saved badge: a two-colour badge matches only with both colours");
    check(guild_tag_badge_saved(plain, 6, leaf->primary, leaf->primary) && !guild_tag_badge_saved(plain, 6, 0x32a070, 0x57b59e) &&
              !guild_tag_badge_saved(plain, 7, guild_tag_badge(7)->primary, 0),
          "saved badge: a badge saved in its own colours matches its default tile, not a palette's");

    GuildAdminProfile none{};
    check(decode(R"({"name":"Plain","badge":15})", &none), "draft: deprecated badge fixture decodes");
    const auto off = guild_tag_draft(none);
    check(!off.enabled && off.badge == kGuildTagDefaultBadge && !off.primary_set,
          "draft: no tag is off; a badge that can no longer be set reads as Leaf");
}

void test_catalogue() noexcept {
    u32 base = 0, packs = 0;
    for (const auto& badge : kGuildTagBadges) {
        base += !badge.pack;
        packs += badge.pack != 0;
    }
    check(base == kGuildTagBaseBadgeCount && packs == 20 && kGuildTagBadges[0].type == kGuildTagDefaultBadge &&
              guild_tag_badge(3) && guild_tag_badge(3)->tints == 2 && !guild_tag_badge(10) && !guild_tag_badge(20),
          "badges: ten base badges (Leaf first), twenty pack badges, none of the retired 10-20");
    const auto& bunny = *guild_tag_badge(21);
    check(!guild_tag_badge_unlocked(bunny, 0) && guild_tag_badge_unlocked(bunny, guild_feature_badge_pack_pets) &&
              guild_tag_badge_unlocked(*guild_tag_badge(6), 0),
          "badges: a pack badge needs its GUILD_TAGS_BADGE_PACK_* feature");
    check(guild_feature_bit(utf8_literal("GUILD_TAGS")) == guild_feature_guild_tags &&
              guild_feature_bit(utf8_literal("GUILD_TAGS_BADGE_PACK_FLEX")) == guild_feature_badge_pack_flex,
          "badges: the GUILD_TAGS features are read from the guild");
    check(kGuildTagPalettes[0].primary == 0xff1c90 && kGuildTagPalettes[0].secondary == 0xff7fc0 &&
              kGuildTagPalettes[kGuildTagPaletteCount - 1].primary == 0x222222,
          "palette: thirteen preset pairs, hot pink first");
}

void test_validation() noexcept {
    check(guild_tag_text_valid(utf8_literal("BUG")) && guild_tag_text_valid(utf8_literal("A1")) &&
              guild_tag_text_valid(utf8_literal("WUMP")) && guild_tag_text_valid(utf8_literal("\xC3\x89T\xC3\x89")),
          "tag: 2-4 letters or digits pass, other scripts are left to the server");
    check(!guild_tag_text_valid(utf8_literal("A")) && !guild_tag_text_valid(utf8_literal("WUMPS")) &&
              !guild_tag_text_valid(utf8_literal("A B")) && !guild_tag_text_valid(utf8_literal("A-B")) &&
              !guild_tag_text_valid(utf8_literal("\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80")),
          "tag: one character, five, spaces, punctuation and more than four UTF-16 units are refused");
    check(guild_tag_badge_settable(0) && guild_tag_badge_settable(40) && !guild_tag_badge_settable(12) &&
              !guild_tag_badge_settable(41),
          "badge: only 0-9 and 21-40 may be set");
}

void test_requests() noexcept {
    GuildAdminProfile profile{};
    check(decode(R"({"tag":"BUG","badge":6,"badge_color_primary":"#32A070","badge_color_secondary":"#57b59e"})",
                 &profile),
          "requests: fixture decodes");
    auto draft = guild_tag_draft(profile);
    GuildTagWrite write{};
    GuildTagColorText colors{};
    GuildSettingsPatch patch{};
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::tag_update;
    request.guild_id = kGuild;
    request.guild = &patch;
    check(guild_tag_write(draft, &colors, &write) && ((patch.tag = &write), true) &&
              request_equals(request, RestMethod::patch, "/api/v9/guilds/100/profile",
                             R"({"tag":"BUG","badge":6,"badge_color_primary":"#32a070",)"
                             R"("badge_color_secondary":"#57b59e"})"),
          "requests: the save PATCHes /guilds/{g}/profile with tag, badge and both colours (lower case)");
    draft.enabled = false;
    guild_tag_set_palette(&draft, kGuildTagPaletteDefault);
    check(guild_tag_write(draft, &colors, &write) &&
              request_equals(request, RestMethod::patch, "/api/v9/guilds/100/profile",
                             R"({"tag":null,"badge":6,"badge_color_primary":null,"badge_color_secondary":null})"),
          "requests: a tag turned off sends null, and default colours null");
    draft.enabled = true;
    guild_tag_set_text(&draft, utf8_literal("B G"));
    check(!guild_tag_draft_valid(draft) && !guild_tag_write(draft, &colors, &write),
          "requests: an invalid tag is never written");
    write = {utf8_literal("BUG"), {}, {}, 12};
    check(!guild_admin_request_valid(request), "requests: a retired badge is refused");
    write.badge = 6;
    patch.fields = guild_settings_patch_description;
    check(!guild_admin_request_valid(request), "requests: the tag PATCH carries no other guild field");
    patch.fields = 0;
    request.operation = GuildAdminOperation::profile_update;
    check(!guild_admin_request_valid(request), "requests: the tag write belongs to tag_update alone");

    GuildAdminRequest adopt{};
    adopt.operation = GuildAdminOperation::identity_adopt;
    adopt.guild_id = kGuild;
    check(request_equals(adopt, RestMethod::put, "/api/v9/users/@me/clan",
                         R"({"identity_guild_id":"100","identity_enabled":true})"),
          "requests: adopting the tag PUTs /users/@me/clan with the guild and identity_enabled");
    adopt.guild_id = 0;
    check(!guild_admin_request_valid(adopt), "requests: adopting needs the guild");
}

} // namespace server_tag_test
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_server_tag_entry) {
    using namespace maicord::server_tag_test;
    test_decode();
    test_draft();
    test_catalogue();
    test_validation();
    test_requests();
    maicord::test_exit(failures, "server tag");
}
