#include "maicord/app/account_profile_edit.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/profile_editor.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace profile_editor_test {

u32 failures;
// The last request the transport saw; large bodies only record their size.
struct Capture {
    RestRouteKey route;
    u32 submissions, large_submissions, target_bytes, body_bytes, timeout_ms;
    u8 target[kRestClientMaximumTargetBytes], body[4096];
    bool json, large;
} capture;

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool same(const void* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({static_cast<const char*>(data), bytes}, expected);
}
// Exact target and body of a valid request.
bool writes(const ProfileEditRequest& request, Utf8View target, Utf8View body) noexcept {
    u8 t[kProfileEditMaximumTargetBytes], b[2048];
    const auto written_target = profile_edit_write_target(request, {t, sizeof(t)});
    const auto written_body = profile_edit_write_body(request, {b, sizeof(b)});
    const bool get = profile_edit_method(request.operation) == RestMethod::get;
    return profile_edit_request_valid(request) && written_target.ok && same(t, written_target.bytes, target) &&
           (get ? !written_body.ok && !body.bytes : written_body.ok && same(b, written_body.bytes, body)) &&
           (get || profile_edit_body_capacity(request) >= written_body.bytes);
}
ProfileEditRequest user_request(const UserPatch* patch) noexcept {
    ProfileEditRequest r{};
    r.operation = ProfileEditOperation::user_patch;
    r.user = patch;
    return r;
}
ProfileEditRequest profile_request(const ProfilePatch* patch, u64 guild = 0) noexcept {
    ProfileEditRequest r{};
    r.operation = guild ? ProfileEditOperation::member_profile_patch : ProfileEditOperation::user_profile_patch;
    r.guild_id = guild;
    r.profile = patch;
    return r;
}
ProfileEditRequest member_request(const MemberEditPatch* patch) noexcept {
    ProfileEditRequest r{};
    r.operation = ProfileEditOperation::member_patch;
    r.guild_id = 10;
    r.member = patch;
    return r;
}

constexpr Utf8View kAvatarUri = utf8_literal("data:image/png;base64,iVBORw0KGgo=");
constexpr Utf8View kBannerUri = utf8_literal("data:image/gif;base64,R0lGODlhAQABAAAAACw=");

// The official client's PATCH bodies: only changed fields, in its key order.
void test_protocol() noexcept {
    const auto me = utf8_literal("/api/v10/users/@me");
    const auto me_profile = utf8_literal("/api/v10/users/@me/profile");
    UserPatch name{};
    name.fields = user_patch_global_name;
    name.global_name = utf8_literal("Ada \"Q\" L\xC3\xB6we");
    check(writes(user_request(&name), me, utf8_literal("{\"global_name\":\"Ada \\\"Q\\\" L\xC3\xB6we\"}")),
          "profile edit: a display name alone PATCHes /users/@me, JSON-escaped");
    name.global_name = {};
    check(writes(user_request(&name), me, utf8_literal(R"({"global_name":""})")),
          "profile edit: clearing the display name sends an empty string");
    UserPatch avatar{};
    avatar.fields = user_patch_avatar;
    avatar.avatar = kAvatarUri;
    avatar.avatar_description = utf8_literal("cat photo");
    check(writes(user_request(&avatar), me,
                 utf8_literal(R"({"avatar":"data:image/png;base64,iVBORw0KGgo=","avatar_description":"cat photo"})")),
          "profile edit: a new avatar is a data URI with its description");
    avatar.avatar = {};
    avatar.avatar_description = {};
    check(writes(user_request(&avatar), me, utf8_literal(R"({"avatar":null})")),
          "profile edit: removing the avatar sends null");
    UserPatch all{};
    all.fields = user_patch_avatar | user_patch_global_name | user_patch_avatar_decoration;
    all.avatar = kAvatarUri;
    all.global_name = utf8_literal("Ada");
    all.avatar_decoration_sku = 1144058522808614923ull;
    check(writes(user_request(&all), me,
                 utf8_literal(R"({"avatar":"data:image/png;base64,iVBORw0KGgo=","global_name":"Ada",)"
                              R"("avatar_decoration_sku_id":"1144058522808614923"})")),
          "profile edit: avatar, name and decoration keep the client's key order");
    UserPatch decoration{};
    decoration.fields = user_patch_avatar_decoration;
    check(writes(user_request(&decoration), me, utf8_literal(R"({"avatar_decoration_sku_id":null})")),
          "profile edit: removing the decoration sends a null sku");

    ProfilePatch text{};
    text.fields = profile_patch_bio | profile_patch_pronouns;
    text.bio = utf8_literal("Line one\nTab\there \"quoted\"");
    text.pronouns = utf8_literal("she/her");
    check(writes(profile_request(&text), me_profile,
                 utf8_literal(R"({"bio":"Line one\nTab\there \"quoted\"","pronouns":"she/her"})")),
          "profile edit: bio and pronouns PATCH /users/@me/profile");
    ProfilePatch everything{};
    everything.fields = profile_patch_banner | profile_patch_bio | profile_patch_pronouns | profile_patch_accent |
                        profile_patch_theme | profile_patch_collectibles;
    everything.banner = kBannerUri;
    everything.accent_color = 0x5865F2;
    everything.theme_colors[0] = 0x112233;
    everything.theme_colors[1] = 0xABCDEF;
    everything.collectibles[0] = 11;
    everything.collectibles[1] = 22;
    everything.collectible_count = 2;
    check(writes(profile_request(&everything), me_profile,
                 utf8_literal(R"({"banner":"data:image/gif;base64,R0lGODlhAQABAAAAACw=","bio":"","pronouns":"",)"
                              R"("accent_color":5793266,"theme_colors":[1122867,11259375],)"
                              R"("collectibles_sku_ids":["11","22"]})")),
          "profile edit: banner, bio, pronouns, accent, theme and collectibles in order");
    ProfilePatch removal{};
    removal.fields = profile_patch_banner | profile_patch_theme | profile_patch_accent | profile_patch_collectibles;
    removal.theme = ProfileThemePatch::none;
    removal.accent_none = true;
    check(writes(profile_request(&removal), me_profile,
                 utf8_literal(R"({"banner":null,"accent_color":null,"theme_colors":null,"collectibles_sku_ids":[]})")),
          "profile edit: removals send null and an empty collectibles list");

    const auto member_target = utf8_literal("/api/v10/guilds/10/members/@me");
    const auto member_profile = utf8_literal("/api/v10/guilds/10/profile/@me");
    MemberEditPatch nick{};
    nick.fields = member_edit_nick | member_edit_avatar | member_edit_avatar_decoration;
    nick.avatar = kAvatarUri;
    nick.avatar_decoration_sku = 7;
    check(writes(member_request(&nick), member_target,
                 utf8_literal(R"({"nick":"","avatar":"data:image/png;base64,iVBORw0KGgo=","avatar_decoration_sku_id":"7"})")),
          "profile edit: server nickname reset, avatar and decoration share the member PATCH");
    ProfilePatch reset{};
    reset.fields = profile_patch_theme | profile_patch_banner;
    reset.theme = ProfileThemePatch::reset;
    check(writes(profile_request(&reset, 10), member_profile, utf8_literal(R"({"banner":null,"theme_colors":[null,null]})")),
          "profile edit: Reset Theme sends [null,null] to the member profile");
    const auto route = profile_edit_route(profile_request(&reset, 10));
    check(route.major == RestMajorParameter::guild && route.major_key == 10 &&
              route.method == RestMethod::patch,
          "profile edit: member saves are guild-major PATCHes");

    ProfileEditRequest fetch{};
    fetch.operation = ProfileEditOperation::profile_fetch;
    fetch.id = 42;
    check(writes(fetch,
                 utf8_literal("/api/v10/users/42/profile?with_mutual_guilds=false&with_mutual_friends=false"
                              "&with_mutual_friends_count=false"),
                 {}),
          "profile edit: the editor loads the profile without mutuals");
    fetch.guild_id = 10;
    check(writes(fetch,
                 utf8_literal("/api/v10/users/42/profile?with_mutual_guilds=false&with_mutual_friends=false"
                              "&with_mutual_friends_count=false&guild_id=10"),
                 {}),
          "profile edit: the per-server tab adds its guild");
    ProfileEditRequest owned{};
    owned.operation = ProfileEditOperation::collectibles;
    check(writes(owned, utf8_literal("/api/v10/users/@me/collectibles-purchases?variants_return_style=2"), {}),
          "profile edit: owned collectibles come from collectibles-purchases");
    ProfileEditRequest product{};
    product.operation = ProfileEditOperation::collectible_product;
    product.id = 99;
    check(writes(product, utf8_literal("/api/v10/collectibles-products/99"), {}),
          "profile edit: an effect's layers come from its product");

    // Rejections: each differs from a valid request by one property.
    UserPatch url{};
    url.fields = user_patch_avatar;
    url.avatar = utf8_literal("https://cdn.example/a.png");
    bool rejected = !profile_edit_request_valid(user_request(&url));
    UserPatch stray{};
    stray.fields = user_patch_global_name;
    stray.avatar = kAvatarUri;
    rejected = rejected && !profile_edit_request_valid(user_request(&stray));
    UserPatch long_name{};
    long_name.fields = user_patch_global_name;
    long_name.global_name = utf8_literal("123456789012345678901234567890123");
    rejected = rejected && !profile_edit_request_valid(user_request(&long_name));
    ProfilePatch member_accent{};
    member_accent.fields = profile_patch_accent;
    rejected = rejected && !profile_edit_request_valid(profile_request(&member_accent, 10)) &&
               profile_edit_request_valid(profile_request(&member_accent));
    ProfilePatch main_reset{};
    main_reset.fields = profile_patch_theme;
    main_reset.theme = ProfileThemePatch::reset;
    rejected = rejected && !profile_edit_request_valid(profile_request(&main_reset));
    ProfilePatch zero_sku{};
    zero_sku.fields = profile_patch_collectibles;
    zero_sku.collectible_count = 1;
    rejected = rejected && !profile_edit_request_valid(profile_request(&zero_sku));
    ProfileEditRequest no_guild = member_request(&nick);
    no_guild.guild_id = 0;
    rejected = rejected && !profile_edit_request_valid(no_guild);
    check(rejected, "profile edit: malformed requests are rejected");
}

void test_transport() noexcept {
    RestClient details{};
    capture = {};
    UserPatch name{};
    name.fields = user_patch_global_name;
    name.global_name = utf8_literal("Ada");
    auto request = user_request(&name);
    const auto submitted = account_profile_edit_submit(&details, &request, 5);
    check(submitted.ok() && capture.submissions == 1 && !capture.large && capture.json &&
              same(capture.target, capture.target_bytes, utf8_literal("/api/v10/users/@me")) &&
              same(capture.body, capture.body_bytes, utf8_literal(R"({"global_name":"Ada"})")) &&
              capture.route.method == RestMethod::patch,
          "profile edit transport: a small save goes out as one JSON PATCH");
    // An avatar data URI over the ordinary ceiling takes the large path.
    const u32 bytes = 200 * 1024;
    auto* uri = static_cast<char*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    bool large = false;
    if (uri) {
        copy_bytes(uri, "data:image/png;base64,", 22);
        for (u32 i = 22; i < bytes; ++i)
            uri[i] = 'A';
        UserPatch avatar{};
        avatar.fields = user_patch_avatar;
        avatar.avatar = {uri, bytes};
        request = user_request(&avatar);
        capture = {};
        large = account_profile_edit_submit(&details, &request, 6).ok() && capture.large_submissions == 1 &&
                capture.large && capture.body_bytes == bytes + 13 && capture.timeout_ms >= 60'000;
        VirtualFree(uri, 0, MEM_RELEASE);
    }
    check(large, "profile edit transport: image saves use the large body path with a long timeout");
    ProfileEditRequest fetch{};
    fetch.operation = ProfileEditOperation::profile_fetch;
    fetch.id = 42;
    capture = {};
    check(account_profile_edit_submit(&details, &fetch, 7).ok() && capture.submissions == 1 && !capture.body_bytes &&
              capture.route.method == RestMethod::get,
          "profile edit transport: loads are bodyless GETs");
    check(!account_profile_edit_submit(&details, &request, 0).ok() &&
              account_profile_edit_cancel(&details, 7).ok(),
          "profile edit transport: a request id is required and cancels are forwarded");
}

ProfileEditorDraft draft_from(const ProfileEditorBase& base) noexcept {
    ProfileEditorDraft d{};
    d.name = base.name;
    d.pronouns = base.pronouns;
    d.bio = base.bio;
    d.decoration_sku = base.decoration_sku;
    d.effect_sku = base.effect_sku;
    d.theme[0] = base.theme[0];
    d.theme[1] = base.theme[1];
    d.accent = base.accent;
    return d;
}

void test_gate() noexcept {
    const auto main_free = profile_editor_gate(ProfileEditorTab::main, 0, false);
    const auto main_nitro = profile_editor_gate(ProfileEditorTab::main, kProfileEditorNitroPremiumType, false);
    check(!(main_free.enabled & (profile_editor_banner | profile_editor_theme)) &&
              (main_free.enabled & (profile_editor_accent | profile_editor_avatar | profile_editor_bio |
                                    profile_editor_decoration | profile_editor_effect)) ==
                  (profile_editor_accent | profile_editor_avatar | profile_editor_bio | profile_editor_decoration |
                   profile_editor_effect) &&
              main_nitro.enabled == profile_editor_all,
          "profile gate: the main profile's banner and theme need Nitro; Banner Color is free");
    const auto guild_free = profile_editor_gate(ProfileEditorTab::guild, 1, true);
    const auto guild_locked = profile_editor_gate(ProfileEditorTab::guild, kProfileEditorNitroPremiumType, false);
    check(guild_free.enabled == (profile_editor_name | profile_editor_pronouns) &&
              !(guild_locked.enabled & profile_editor_name) && (guild_locked.enabled & profile_editor_bio) &&
              !(guild_locked.enabled & profile_editor_accent),
          "profile gate: per-server nickname needs CHANGE_NICKNAME, everything but pronouns needs Nitro");
    check(utf8_equal(profile_editor_gate_reason(ProfileEditorTab::main, main_free, profile_editor_banner),
                     utf8_literal("Only with Nitro.")) &&
              utf8_equal(profile_editor_gate_reason(ProfileEditorTab::guild, guild_locked, profile_editor_name),
                         utf8_literal("You don't have permission to change your nickname in this server.")) &&
              !profile_editor_gate_reason(ProfileEditorTab::main, main_nitro, profile_editor_banner).bytes,
          "profile gate: gated controls explain themselves without an upsell");
}

void test_patches() noexcept {
    const ProfileCollectibleRef saved[] = {{900, ProfileCollectibleType::profile_frame},
                                           {800, ProfileCollectibleType::profile_effect}};
    ProfileEditorBase base{};
    base.name = utf8_literal("Ada");
    base.pronouns = utf8_literal("she/her");
    base.bio = utf8_literal("Hello");
    base.avatar = true;
    base.decoration_sku = 5;
    base.effect_sku = 800;
    base.collectibles = {saved, 2};
    auto draft = draft_from(base);
    const auto nitro = profile_editor_gate(ProfileEditorTab::main, kProfileEditorNitroPremiumType, false);
    const auto free = profile_editor_gate(ProfileEditorTab::main, 0, false);
    check(!profile_editor_changes(ProfileEditorTab::main, base, draft), "profile draft: an untouched draft is clean");
    // Unsaved-changes flow: an edit dirties the page, its revert cleans it.
    draft.pronouns = utf8_literal("they/them");
    const bool dirty = profile_editor_changes(ProfileEditorTab::main, base, draft) == profile_editor_pronouns;
    draft.pronouns = base.pronouns;
    check(dirty && !profile_editor_changes(ProfileEditorTab::main, base, draft),
          "profile draft: editing and reverting a field toggles unsaved changes");
    ProfileEditorBase unnamed{};
    auto empty = draft_from(unnamed);
    empty.name = {};
    check(!profile_editor_changes(ProfileEditorTab::main, unnamed, empty),
          "profile draft: an empty display name over none is no change");

    // A save across both requests.
    draft.name = utf8_literal("Ada L");
    draft.avatar = ProfileImageEdit::replace;
    draft.avatar_uri = kAvatarUri;
    draft.avatar_description = utf8_literal("me");
    draft.decoration_set = true;
    draft.decoration_sku = 0;
    draft.effect_set = true;
    draft.effect_sku = 801;
    draft.bio = utf8_literal("New bio");
    draft.theme_set = true;
    draft.theme[0] = 1;
    draft.theme[1] = 2;
    UserPatch user{};
    MemberEditPatch member{};
    ProfilePatch profile{};
    const bool built =
        profile_editor_patches(ProfileEditorTab::main, base, draft, nitro, &user, &member, &profile);
    check(built &&
              user.fields == (user_patch_avatar | user_patch_global_name | user_patch_avatar_decoration) &&
              utf8_equal(user.avatar, kAvatarUri) && utf8_equal(user.avatar_description, utf8_literal("me")) &&
              !user.avatar_decoration_sku && !member.fields &&
              profile.fields == (profile_patch_bio | profile_patch_theme | profile_patch_collectibles) &&
              profile.theme == ProfileThemePatch::colors && profile.collectible_count == 2 &&
              profile.collectibles[0] == 900 && profile.collectibles[1] == 801,
          "profile save: account fields PATCH the user, the rest the profile (frames kept beside the new effect)");
    check(!profile_editor_patches(ProfileEditorTab::main, base, draft, free, &user, &member, &profile),
          "profile save: a theme change without Nitro is refused");
    auto unprepared = draft;
    unprepared.avatar_uri = {};
    check(!profile_editor_patches(ProfileEditorTab::main, base, unprepared, nitro, &user, &member, &profile),
          "profile save: a replaced avatar needs its data URI first");
    char long_bio[200];
    for (auto& c : long_bio)
        c = 'b';
    auto too_long = draft_from(base);
    too_long.bio = {long_bio, 191};
    check(!profile_editor_changes_allowed(ProfileEditorTab::main, base, too_long, nitro) &&
              profile_editor_bio_remaining(too_long.bio) == -1 && profile_editor_bio_valid({long_bio, 190}),
          "profile save: a bio over 190 characters disables Save and the counter goes negative");

    // Per-server: resets are allowed without Nitro; changes are not.
    ProfileEditorBase guild{};
    guild.avatar = true;
    guild.has_theme = true;
    guild.theme[0] = 3;
    guild.theme[1] = 4;
    const auto guild_free = profile_editor_gate(ProfileEditorTab::guild, 0, true);
    auto reset = draft_from(guild);
    reset.avatar = ProfileImageEdit::remove;
    reset.theme_set = true;
    reset.theme_reset = true;
    reset.name = utf8_literal("Server Ada");
    const bool reset_built =
        profile_editor_patches(ProfileEditorTab::guild, guild, reset, guild_free, &user, &member, &profile);
    check(reset_built && member.fields == (member_edit_nick | member_edit_avatar) && !member.avatar.bytes &&
              utf8_equal(member.nick, utf8_literal("Server Ada")) && profile.fields == profile_patch_theme &&
              profile.theme == ProfileThemePatch::reset && !user.fields,
          "profile save: per-server resets go out without Nitro");
    auto gated = draft_from(guild);
    gated.bio = utf8_literal("Server bio");
    check(!profile_editor_changes_allowed(ProfileEditorTab::guild, guild, gated, guild_free),
          "profile save: a per-server bio needs Nitro");

    char hex[8];
    u32 color = 0;
    check(profile_editor_format_color(0x5865F2, {hex, sizeof(hex)}) == 7 &&
              utf8_equal({hex, 7}, utf8_literal("#5865F2")) && profile_editor_parse_color(utf8_literal("5865f2"), &color) &&
              color == 0x5865F2 && !profile_editor_parse_color(utf8_literal("#12345G"), &color) &&
              !profile_editor_parse_color(utf8_literal("#1234"), &color),
          "profile colours: hex input converts to the API's integer RGB");
}

void test_decode() noexcept {
    static u32 structurals[4096];
    const auto body = utf8_literal(
        R"({"user":{"id":"42","username":"ada","global_name":"Ada","avatar":"a_0123456789abcdef0123456789abcdef",)"
        R"("avatar_decoration_data":{"asset":"a_dec","sku_id":"1144"},"banner":null},)"
        R"("user_profile":{"bio":"Hi","pronouns":"she/her","banner":"b1","accent_color":5793266,)"
        R"("theme_colors":[1,2],"collectibles":[{"sku_id":"800","type":1},{"sku_id":"900","type":3}]},)"
        R"("guild_member":{"nick":"Server Ada","avatar":null,"avatar_decoration_data":null},)"
        R"("guild_member_profile":{"bio":"","pronouns":"they","banner":null,"theme_colors":[null,null]},)"
        R"("premium_type":2})");
    ProfileEditSaved saved{};
    const bool decoded = profile_edit_decode_profile(body, {structurals, 4096}, 42, 10, &saved);
    check(decoded && utf8_equal(saved.global_name.view(), utf8_literal("Ada")) && saved.decoration_sku == 1144 &&
              utf8_equal(saved.decoration_asset.view(), utf8_literal("a_dec")) && saved.has_theme &&
              saved.theme_colors[1] == 2 && saved.has_accent && saved.accent_color == 5793266 &&
              saved.collectible_count == 2 &&
              profile_edit_collectible({saved.collectibles, saved.collectible_count},
                                       ProfileCollectibleType::profile_effect) == 800 &&
              utf8_equal(saved.nick.view(), utf8_literal("Server Ada")) &&
              !saved.has_member_theme && utf8_equal(saved.member_pronouns.view(), utf8_literal("they")) &&
              saved.premium_type == 2,
          "profile decode: the editor reads user, profile, member and member profile values");
    check(!profile_edit_decode_profile(body, {structurals, 4096}, 43, 0, &saved),
          "profile decode: another user's profile is rejected");

    ProfileEditErrors errors{};
    const auto invalid = utf8_literal(
        R"({"code":50035,"errors":{"bio":{"_errors":[{"code":"BASE_TYPE_MAX_LENGTH","message":"Must be 190 or fewer in length."}]},)"
        R"("avatar_decoration_sku_id":{"_errors":[{"code":"X","message":"Not owned."}]}},"message":"Invalid Form Body"})");
    check(profile_edit_decode_errors(invalid, {structurals, 4096}, &errors) && errors.code == 50035 &&
              utf8_equal(errors.fields[static_cast<u32>(ProfileEditErrorField::bio)].view(),
                         utf8_literal("Must be 190 or fewer in length.")) &&
              utf8_equal(errors.fields[static_cast<u32>(ProfileEditErrorField::misc)].view(), utf8_literal("Not owned.")) &&
              utf8_equal(errors.message.view(), utf8_literal("Invalid Form Body")),
          "profile errors: 50035 field errors flatten to their first message");
    const auto username = utf8_literal(R"({"code":50035,"errors":{"username":{"_errors":[{"message":"Bad name"}]}}})");
    check(profile_edit_decode_errors(username, {structurals, 4096}, &errors) && errors.username &&
              utf8_equal(errors.fields[static_cast<u32>(ProfileEditErrorField::nick)].view(), utf8_literal("Bad name")),
          "profile errors: a username error marks the account and shows as the nickname's");
    const auto limited = utf8_literal(R"({"message":"You are being rate limited.","retry_after":1.5,"global":false})");
    check(profile_edit_decode_errors(limited, {structurals, 4096}, &errors) && !errors.code &&
              utf8_equal(errors.message.view(), utf8_literal("You are being rate limited.")),
          "profile errors: other failures keep their message");

    ProfileCollectible owned[8]{};
    const auto purchases = utf8_literal(
        R"([{"sku_id":"1","name":"Cat Ears","items":[{"type":0,"sku_id":"1","asset":"a_cat","label":""}]},)"
        R"({"sku_id":"2","name":"Snow","items":[{"type":1,"sku_id":"2","title":"Snowfall","thumbnailPreviewSrc":"https://cdn.discordapp.com/x.png"}]},)"
        R"({"sku_id":"3","name":"Bundle","items":[{"type":2,"sku_id":"3"}],"variants":[{"sku_id":"4","name":"Red",)"
        R"("items":[{"type":0,"sku_id":"4","asset":"red","label":"Red Ears"}]}]},)"
        R"({"sku_id":"1","name":"Cat Ears","items":[{"type":0,"sku_id":"1","asset":"a_cat"}]}])");
    const u32 count = profile_edit_decode_collectibles(purchases, {structurals, 4096}, {owned, 8});
    check(count == 3 && owned[0].sku == 1 && utf8_equal(owned[0].label.view(), utf8_literal("Cat Ears")) &&
              utf8_equal(owned[0].asset.view(), utf8_literal("a_cat")) &&
              owned[1].type == ProfileCollectibleType::profile_effect &&
              utf8_equal(owned[1].label.view(), utf8_literal("Snowfall")) && owned[2].sku == 4 &&
              utf8_equal(owned[2].label.view(), utf8_literal("Red Ears")),
          "profile collectibles: owned decorations and effects, variants flattened, duplicates once");
    check(profile_edit_decode_collectibles(utf8_literal("{\"oops\":1}"), {structurals, 4096}, {owned, 8}) == ~0u,
          "profile collectibles: a malformed body is reported");
}

} // namespace profile_editor_test

RestClientSubmitResult rest_client_submit(RestClient*,
                                          const RestClientRequest& request) noexcept {
    auto& c = profile_editor_test::capture;
    ++c.submissions;
    c.large = false;
    c.route = request.route;
    c.timeout_ms = request.timeout_ms;
    c.json = request.json_body;
    if (request.target.count > sizeof(c.target) || request.body.count > sizeof(c.body))
        return {RestClientSubmitError::body_capacity};
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    copy_bytes(c.target, request.target.data, request.target.count);
    copy_bytes(c.body, request.body.data, request.body.count);
    return {RestClientSubmitError::none};
}
RestClientSubmitResult rest_client_submit_large(RestClient*,
                                                const RestClientRequest& request) noexcept {
    auto& c = profile_editor_test::capture;
    ++c.large_submissions;
    c.large = true;
    c.route = request.route;
    c.timeout_ms = request.timeout_ms;
    c.json = request.json_body;
    c.target_bytes = request.target.count <= sizeof(c.target) ? request.target.count : 0;
    copy_bytes(c.target, request.target.data, c.target_bytes);
    c.body_bytes = request.body.count;
    return {RestClientSubmitError::none};
}
bool rest_client_cancel(RestClient*, u64) noexcept {
    return true;
}

} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_profile_editor_entry() noexcept {
    using namespace maicord::profile_editor_test;
    test_protocol();
    test_transport();
    test_gate();
    test_patches();
    test_decode();
    constexpr char pass[] = "PASS profile editor\n";
    constexpr char fail[] = "FAIL profile editor\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
