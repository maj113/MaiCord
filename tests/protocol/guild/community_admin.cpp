// Server Settings' Access, Enable Community / Community Overview and Server
// Template: the request targets and bodies, the replies they read, the guild
// PATCH fields they add and the drafts behind the pages.
#include "maicord/core/memory.hpp"
#include "maicord/model/guild/community_settings.hpp"
#include "maicord/model/guild/detail_store.hpp"
#include "maicord/protocol/guild/admin.hpp"
#include "maicord/protocol/guild/community_admin.hpp"
#include "../../support/test_output.hpp"

namespace maicord::community_admin_test {

u32 failures;
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}
u32 scratch[32 * 1024];
u8 output[16 * 1024];

bool text_is(u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(output), bytes}, expected);
}

Utf8View target(GuildCommunityOperation op, Utf8View code, RestMethod* method, const char* body = nullptr) noexcept {
    static u8 storage[kGuildCommunityMaximumTargetBytes];
    GuildCommunityRequest request{};
    request.operation = op;
    request.guild_id = 10;
    request.code = code;
    if (body) {
        request.body = reinterpret_cast<const u8*>(body);
        request.body_bytes = c_string_length(body);
    }
    const auto written = guild_community_write_target(request, {storage, sizeof(storage)});
    if (!written.ok())
        return {};
    *method = written.method;
    return {reinterpret_cast<const char*>(storage), written.target_bytes};
}

void test_targets() noexcept {
    RestMethod method{};
    using Op = GuildCommunityOperation;
    check(utf8_equal(target(Op::verification_update, {}, &method, "{}"),
                     utf8_literal("/api/v9/guilds/10/member-verification")) &&
              method == RestMethod::patch,
          "access: the membership form is saved with PATCH member-verification");
    check(utf8_equal(target(Op::templates_list, {}, &method), utf8_literal("/api/v9/guilds/10/templates")) &&
              method == RestMethod::get &&
              utf8_equal(target(Op::template_create, {}, &method, "{\"name\":\"ab\"}"),
                         utf8_literal("/api/v9/guilds/10/templates")) &&
              method == RestMethod::post,
          "template: listed with GET and created with POST on /guilds/{g}/templates");
    const auto code = utf8_literal("hgM48av5Q69A");
    check(utf8_equal(target(Op::template_sync, code, &method), utf8_literal("/api/v9/guilds/10/templates/hgM48av5Q69A")) &&
              method == RestMethod::put && target(Op::template_update, code, &method, "{}").bytes &&
              method == RestMethod::patch && target(Op::template_delete, code, &method).bytes &&
              method == RestMethod::delete_method,
          "template: synced with PUT, modified with PATCH and deleted with DELETE on its code");
    check(!target(Op::template_sync, {}, &method).bytes && !target(Op::template_delete, utf8_literal("ab/c"), &method).bytes &&
              !target(Op::templates_list, code, &method).bytes,
          "template: a code is required, letters and digits only, and only on the coded routes");
    check(utf8_equal(target(Op::discovery_requirements, {}, &method),
                     utf8_literal("/api/v9/guilds/10/discovery-requirements")) &&
              utf8_equal(target(Op::discovery_metadata_get, {}, &method),
                         utf8_literal("/api/v9/guilds/10/discovery-metadata")) &&
              method == RestMethod::get && target(Op::discovery_metadata_update, {}, &method, "{}").bytes &&
              method == RestMethod::patch &&
              utf8_equal(target(Op::discovery_categories, {}, &method),
                         utf8_literal("/api/v9/discovery/categories?locale=en-US&primary_only=false")),
          "discovery: requirements, metadata and the category list routes");
    static u32 units[512];
    check(utf8_equal(target(Op::join_requests_submitted, {}, &method),
                     utf8_literal("/api/v9/guilds/10/requests?status=SUBMITTED&limit=25")) &&
              method == RestMethod::get &&
              guild_join_requests_decode_total(
                  utf8_literal(R"({"guild_join_requests":[{"id":"1","application_status":"SUBMITTED"}],"total":3,)"
                               R"("limit":25})"),
                  {units, 512}) == 3 &&
              guild_join_requests_decode_total(utf8_literal(R"({"total":"x"})"), {units, 512}) == ~0u &&
              guild_join_requests_decode_total(utf8_literal("[]"), {units, 512}) == ~0u,
          "access: the submitted applications are read for their total before leaving Apply to Join");
    check(guild_community_operation_large_reply(Op::templates_list) &&
              guild_community_operation_large_reply(Op::template_create) &&
              !guild_community_operation_large_reply(Op::template_sync) &&
              !guild_community_operation_large_reply(Op::template_update) &&
              !guild_community_operation_large_reply(Op::template_delete) &&
              !guild_community_operation_large_reply(Op::verification_get),
          "template: the list and a new template's reply take the large response path (GET and POST)");
    GuildCommunityRequest sub{};
    sub.operation = Op::discovery_subcategory_add;
    sub.guild_id = 10;
    sub.event_id = 48;
    static u8 storage[kGuildCommunityMaximumTargetBytes];
    auto written = guild_community_write_target(sub, {storage, sizeof(storage)});
    const bool added = written.ok() && written.method == RestMethod::put &&
                       utf8_equal({reinterpret_cast<const char*>(storage), written.target_bytes},
                                  utf8_literal("/api/v9/guilds/10/discovery-categories/48"));
    sub.operation = Op::discovery_subcategory_remove;
    written = guild_community_write_target(sub, {storage, sizeof(storage)});
    check(added && written.ok() && written.method == RestMethod::delete_method,
          "discovery: subcategories are added with PUT and removed with DELETE on their id");
    const u32 visibility = guild_visibility_write(3, {output, sizeof(output)});
    check(text_is(visibility, utf8_literal(R"({"visibility":3})")) && !guild_visibility_write(2, {output, 64}) &&
              utf8_equal(target(Op::guild_profile_visibility, {}, &method, R"({"visibility":3})"),
                         utf8_literal("/api/v9/guilds/10/profile")) &&
              method == RestMethod::patch,
          "access: Apply from Profile PATCHes the profile's visibility alone");
}

void test_verification() noexcept {
    const Utf8View rules[]{utf8_literal("Be kind"), utf8_literal("No spam")};
    GuildVerificationFieldWrite terms{};
    terms.type = GuildVerificationFieldType::terms;
    terms.label = utf8_literal("Read and agree to the server rules");
    terms.values = {rules, 2};
    terms.required = true;
    u32 bytes = guild_verification_write_update({&terms, 1}, true, GuildVerificationBulkAction::none,
                                                {output, sizeof(output)});
    check(text_is(bytes, utf8_literal(R"({"form_fields":[{"field_type":"TERMS","label":"Read and agree to the server )"
                                      R"(rules","values":["Be kind","No spam"],"required":true}],"enabled":true})")),
          "access: Server Rules sends one TERMS field and turns the gate on");
    bytes = guild_verification_write_update({}, false, GuildVerificationBulkAction::rejected, {output, sizeof(output)});
    check(text_is(bytes, utf8_literal(R"({"form_fields":[],"enabled":false,"bulk_action":"REJECTED"})")),
          "access: no rules turns the gate off, rejecting waiting applications when asked");
    const Utf8View choices[]{utf8_literal("Yes"), utf8_literal("No")};
    GuildVerificationFieldWrite fields[2]{};
    fields[0].type = GuildVerificationFieldType::text_input;
    fields[0].label = utf8_literal("Why?");
    fields[0].required = true;
    fields[0].placeholder = utf8_literal("Tell us");
    fields[1].type = GuildVerificationFieldType::multiple_choice;
    fields[1].label = utf8_literal("Gamer?");
    fields[1].values = {choices, 2};
    bytes = guild_verification_write_update({fields, 2}, true, GuildVerificationBulkAction::none,
                                            {output, sizeof(output)});
    check(text_is(bytes, utf8_literal(R"({"form_fields":[{"field_type":"TEXT_INPUT","label":"Why?","required":true,)"
                                      R"("placeholder":"Tell us"},{"field_type":"MULTIPLE_CHOICE","label":"Gamer?",)"
                                      R"("choices":["Yes","No"],"required":false}],"enabled":true})")),
          "access: Apply to Join sends its questions, answers as choices");
    GuildVerificationFieldWrite bad = fields[1];
    bad.values = {};
    GuildVerificationFieldWrite twice[2]{terms, terms};
    GuildVerificationFieldWrite six[6]{fields[0], fields[0], fields[0], fields[0], fields[0], fields[0]};
    GuildVerificationFieldWrite blank = fields[0];
    blank.label = utf8_literal("   ");
    check(!guild_verification_fields_valid({&bad, 1}) && !guild_verification_fields_valid({twice, 2}) &&
              !guild_verification_fields_valid({six, 6}) && !guild_verification_fields_valid({&blank, 1}) &&
              !guild_verification_write_update({six, 6}, true, GuildVerificationBulkAction::none,
                                               {output, sizeof(output)}),
          "access: forms over five fields, with two rule fields, blank labels or choiceless choices are refused");
}

void test_templates() noexcept {
    check(guild_template_title_valid(utf8_literal("QA")) && !guild_template_title_valid(utf8_literal(" a ")) &&
              !guild_template_title_valid({}) &&
              guild_template_description_valid({}) &&
              !guild_template_description_valid(utf8_literal(
                  "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
                  "012345678901234567890")),
          "template: titles need two characters once trimmed and descriptions at most 120");
    u32 bytes = guild_template_write(utf8_literal("QA "), {}, {output, sizeof(output)});
    check(text_is(bytes, utf8_literal(R"({"name":"QA ","description":""})")),
          "template: create and modify send the texts as typed");
    constexpr auto list = utf8_literal(
        R"([{"code":"hgM48av5Q69A","name":"QA","description":null,"usage_count":3,"creator_id":"1",)"
        R"("created_at":"2026-11-01T10:00:00+00:00","updated_at":"2026-11-02T10:00:00+00:00",)"
        R"("source_guild_id":"10","serialized_source_guild":{"name":"QA","roles":[{"id":0,"name":"@everyone"},)"
        R"({"id":1,"name":"Mods"}],"channels":[{"id":1,"type":4,"name":"Text","parent_id":null},)"
        R"({"id":2,"type":0,"name":"general","parent_id":1},{"id":3,"type":2,"name":"Lounge","parent_id":null}]},)"
        R"("is_dirty":true}])");
    static GuildTemplate t;
    const u32 count = guild_template_decode_list(list, {scratch, sizeof(scratch) / 4}, &t);
    check(count == 1 && utf8_equal({t.code, t.code_bytes}, utf8_literal("hgM48av5Q69A")) &&
              utf8_equal({t.name, t.name_bytes}, utf8_literal("QA")) && !t.description_bytes && t.usage_count == 3 &&
              t.dirty && t.updated_at_ms == 1793613600000ull && t.created_at_ms == 1793527200000ull,
          "template: the list's template reads with its code, texts, use count, times and unsynced flag");
    check(utf8_equal({t.preview, t.preview_bytes},
                     utf8_literal("Lounge (voice)\nText\n    # general\n@everyone\n@Mods\n")) &&
              t.channel_count == 3 && t.role_count == 2,
          "template: the preview lists loose channels, each category's channels, then the roles");
    GuildTemplate empty{};
    check(guild_template_decode_list(utf8_literal("[]"), {scratch, sizeof(scratch) / 4}, &empty) == 0 &&
              guild_template_decode_list(utf8_literal("{}"), {scratch, sizeof(scratch) / 4}, &empty) == ~0u,
          "template: an empty list means none; an object is not a list");
    GuildTemplate one{};
    check(guild_template_decode(utf8_literal(R"({"code":"abc","name":"N","description":"D","is_dirty":null})"),
                                {scratch, sizeof(scratch) / 4}, &one) &&
              utf8_equal({one.description, one.description_bytes}, utf8_literal("D")) && !one.dirty &&
              !guild_template_decode(utf8_literal(R"({"name":"N"})"), {scratch, sizeof(scratch) / 4}, &one),
          "template: a reply object needs its code and name");
    char link[64];
    bytes = guild_template_write_link(utf8_literal("abc"), {link, sizeof(link)});
    check(utf8_equal({link, bytes}, utf8_literal("https://discord.new/abc")), "template: the link is discord.new");
}

void test_discovery() noexcept {
    GuildDiscoveryRequirements r{};
    check(guild_discovery_requirements_decode(
              utf8_literal(R"({"guild_id":"10","safe_environment":true,"healthy":false,"health_score_pending":true,)"
                           R"("size":false,"nsfw_properties":{},"protected":true,"sufficient":false,)"
                           R"("sufficient_without_grace_period":false,"valid_rules_channel":true,)"
                           R"("retention_healthy":false,"engagement_healthy":false,"age":true,"minimum_age":56,)"
                           R"("health_score":null,"minimum_size":1000})"),
              {scratch, sizeof(scratch) / 4}, &r) &&
              r.checklist && r.safe_environment && !r.size && r.age && r.health_score_pending &&
              r.protected_moderation && !r.bad_content && r.minimum_size == 1000 && r.minimum_age_days == 56,
          "discovery: the requirements checklist reads every item");
    GuildDiscoveryRequirements exempt{};
    check(guild_discovery_requirements_decode(
              utf8_literal(R"({"sufficient":true,"sufficient_without_grace_period":true,"nsfw_properties":{"name":["x"]}})"),
              {scratch, sizeof(scratch) / 4}, &exempt) &&
              exempt.sufficient && !exempt.checklist && exempt.bad_content,
          "discovery: exempt servers carry only sufficiency; found words mark bad content");
    constexpr auto read = utf8_literal(
        R"({"guild_id":"10","primary_category_id":49,"keywords":["test"],"emoji_discoverability_enabled":true,)"
        R"("partner_actioned_timestamp":null,"partner_application_timestamp":null,"is_published":false,)"
        R"("reasons_to_join":[{"reason":"Alien","emoji_id":null,"emoji_name":"x"}],)"
        R"("social_links":["https://example.com/alien"],"about":"Alien","category_ids":[48]})");
    GuildDiscoveryMetadata m{};
    check(guild_discovery_metadata_decode(read, {scratch, sizeof(scratch) / 4}, &m) && m.primary_category_id == 49 &&
              m.keyword_count == 1 && utf8_equal({m.keywords[0], m.keyword_bytes[0]}, utf8_literal("test")) &&
              m.emoji_discoverability_enabled && m.subcategory_count == 1 && m.subcategories[0] == 48,
          "discovery: the metadata reads its category, tags and emoji switch");
    const Utf8View tags[]{utf8_literal("games"), utf8_literal("art")};
    const u32 bytes = guild_discovery_metadata_write(read, {scratch, sizeof(scratch) / 4}, 1, {tags, 2}, false,
                                                     {output, sizeof(output)});
    check(text_is(bytes, utf8_literal(R"({"primary_category_id":1,"emoji_discoverability_enabled":false,)"
                                      R"("partner_actioned_timestamp":null,"partner_application_timestamp":null,)"
                                      R"("keywords":["games","art"],"is_published":false,)"
                                      R"("reasons_to_join":[{"reason":"Alien","emoji_id":null,"emoji_name":"x"}],)"
                                      R"("social_links":["https://example.com/alien"],"about":"Alien"})")),
          "discovery: the PATCH repeats the read around the edited category, tags and switch, in the official order");
    GuildDiscoveryCategory categories[4]{};
    const u32 count = guild_discovery_decode_categories(
        utf8_literal(R"([{"id":0,"name":"General","is_primary":true},{"id":48,"name":"Sub","is_primary":false},)"
                     R"({"id":1,"name":"Gaming","is_primary":true}])"),
        {scratch, sizeof(scratch) / 4}, {categories, 4});
    check(count == 3 && categories[0].primary && !categories[1].primary && categories[2].id == 1 &&
              utf8_equal({categories[2].name, categories[2].name_bytes}, utf8_literal("Gaming")),
          "discovery: categories read with their primary flag");
}

u32 guild_body(const GuildSettingsPatch& patch) noexcept {
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::guild_update;
    request.guild_id = 10;
    request.guild = &patch;
    if (!guild_admin_request_valid(request))
        return 0;
    const auto written = guild_admin_write_body(request, {output, sizeof(output)});
    return written.ok() ? written.bytes : 0;
}

// One length byte and the name, per feature.
u32 pack(u8* out, const Utf8View* list, u32 count) noexcept {
    u32 used = 0;
    for (u32 i = 0; i < count; ++i) {
        out[used] = static_cast<u8>(list[i].bytes);
        copy_bytes(out + used + 1, list[i].data, list[i].bytes);
        used += 1 + list[i].bytes;
    }
    return used;
}

void test_guild_patch() noexcept {
    const Utf8View stored[]{utf8_literal("NEWS"), utf8_literal("MEMBER_VERIFICATION_MANUAL_APPROVAL")};
    u8 packed[128];
    const u32 packed_bytes = pack(packed, stored, 2);
    u8 names_out[256];
    GuildCommunitySetup setup{};
    setup.rules_channel_id = 0;
    setup.public_updates_channel_id = kGuildCommunityNewChannel;
    setup.default_notifications = 0;
    GuildSettingsPatch patch{};
    check(guild_community_enable_patch(setup, {packed, packed_bytes}, 2, {names_out, sizeof(names_out)}, &patch) &&
              text_is(guild_body(patch),
                      utf8_literal(R"({"features":["NEWS","MEMBER_VERIFICATION_MANUAL_APPROVAL","COMMUNITY"],)"
                                   R"("rules_channel_id":null,"public_updates_channel_id":"1","verification_level":1,)"
                                   R"("default_message_notifications":1,"explicit_content_filter":2})")),
          "community: Enable Community adds COMMUNITY, its channels and raises what falls short");
    setup.verification_level = 3;
    setup.explicit_content_filter = 2;
    setup.default_notifications = 1;
    setup.rules_channel_id = 777;
    check(guild_community_enable_patch(setup, {packed, packed_bytes}, 2, {names_out, sizeof(names_out)}, &patch) &&
              text_is(guild_body(patch), utf8_literal(R"({"features":["NEWS","MEMBER_VERIFICATION_MANUAL_APPROVAL",)"
                                                      R"("COMMUNITY"],"rules_channel_id":"777",)"
                                                      R"("public_updates_channel_id":"1"})")),
          "community: settings already met are left alone");
    GuildCommunityOverview overview{};
    overview.community = true;
    overview.rules_channel_id = 5;
    overview.public_updates_channel_id = 6;
    overview.safety_alerts_channel_id = 7;
    copy_bytes(overview.locale, "es-419", 6);
    overview.locale_bytes = 6;
    guild_community_overview_disable(&overview);
    const Utf8View community_names[]{utf8_literal("COMMUNITY"), utf8_literal("NEWS"), utf8_literal("DISCOVERABLE"),
                                     utf8_literal("PREVIEW_ENABLED")};
    const u32 community_bytes = pack(packed, community_names, 4);
    check(guild_community_overview_patch(overview, {packed, community_bytes}, 4, {names_out, sizeof(names_out)},
                                         &patch) &&
              text_is(guild_body(patch),
                      utf8_literal(R"({"description":null,"features":["NEWS"],"rules_channel_id":null,)"
                                   R"("public_updates_channel_id":null,"safety_alerts_channel_id":"7",)"
                                   R"("preferred_locale":"es-419"})")),
          "community: Disable Community drops COMMUNITY, DISCOVERABLE and PREVIEW_ENABLED and clears its channels");
    GuildSettingsPatch level{};
    level.fields = guild_settings_patch_content_level;
    level.owner_configured_content_level = 3;
    check(text_is(guild_body(level), utf8_literal(R"({"owner_configured_content_level":3})")),
          "access: the Age-Restricted switch sets the owner's content level");
    GuildSettingsPatch splash{};
    splash.fields = guild_settings_patch_discovery_splash;
    const bool cleared = text_is(guild_body(splash), utf8_literal(R"({"discovery_splash":null})"));
    splash.discovery_splash = utf8_literal("data:image/png;base64,iVBORw0KGgo=");
    check(cleared && text_is(guild_body(splash), utf8_literal(R"({"discovery_splash":"data:image/png;base64,iVBORw0KGgo="})")),
          "access: the Discovery banner is sent as a data URI and removed as null");
    level.owner_configured_content_level = 4;
    GuildSettingsPatch locale{};
    locale.fields = guild_settings_patch_preferred_locale;
    locale.preferred_locale = utf8_literal("en US");
    check(!guild_body(level) && !guild_body(locale), "guild PATCH: unknown content levels and locales are refused");
    check(!guild_features_edit({packed, community_bytes}, 3, 0, {}, {names_out, sizeof(names_out)}, &patch),
          "features: a stored list whose count disagrees is not edited");
}

void test_access_model() noexcept {
    check(guild_join_type(0) == GuildJoinType::invite &&
              guild_join_type(guild_feature_verification_gate) == GuildJoinType::invite &&
              guild_join_type(guild_feature_verification_gate | guild_feature_manual_approval) ==
                  GuildJoinType::apply &&
              guild_join_type(guild_feature_discoverable | guild_feature_manual_approval) ==
                  GuildJoinType::discoverable,
          "access: the join type follows DISCOVERABLE, then the gate with manual approval");
    GuildDiscoveryRequirements r{};
    r.sufficient_without_grace_period = true;
    r.grace_period_end_ms = 2000;
    check(guild_discovery_standing(guild_feature_discoverable, r, 1000) == GuildDiscoveryStanding::eligible_enabled &&
              guild_discovery_standing(guild_feature_discoverable, r, 3000) == GuildDiscoveryStanding::disqualified &&
              guild_discovery_standing(0, r, 0) == GuildDiscoveryStanding::ineligible &&
              guild_discovery_standing(guild_feature_verified, r, 0) == GuildDiscoveryStanding::eligible_disabled,
          "discovery: grace periods keep a listed server, verified servers skip the requirements");

    static GuildVerificationForm form;
    check(guild_verification_decode(
              utf8_literal(R"({"version":"2026-01-01T00:00:00+00:00","description":null,"form_fields":[)"
                           R"({"field_type":"TERMS","label":"Rules","values":["Be kind"],"required":true},)"
                           R"({"field_type":"PARAGRAPH","label":"About you","required":true,"description":null}]})"),
              {scratch, sizeof(scratch) / 4}, &form),
          "access: the form reads");
    static GuildAccessForm base, draft;
    guild_access_form_read(&form, &base);
    check(base.rule_count == 1 && base.question_count == 1 && base.terms_position == 0 && !base.unsupported &&
              base.questions[0].type == GuildVerificationFieldType::paragraph,
          "access: rules and questions split with the rules' place kept");
    const u64 apply = guild_feature_verification_gate | guild_feature_manual_approval;
    copy_value(&draft, base);
    check(!guild_access_verification_dirty(apply, base, draft, GuildJoinType::apply, true) &&
              guild_access_verification_dirty(apply, base, draft, GuildJoinType::invite, true),
          "access: an unchanged application is clean; switching it to invite only is not");
    GuildVerificationFieldWrite fields[5];
    Utf8View values[64];
    const u32 invite = guild_access_fields(draft, GuildJoinType::invite, {fields, 5}, {values, 64});
    check(invite == 1 && fields[0].type == GuildVerificationFieldType::terms &&
              utf8_equal(fields[0].label, utf8_literal("Read and agree to the server rules")) &&
              fields[0].values.count == 1,
          "access: invite only sends a fresh rules field");
    const Utf8View answers[]{utf8_literal("A"), utf8_literal("B")};
    check(guild_access_set_question(&draft, draft.question_count, GuildVerificationFieldType::multiple_choice,
                                    utf8_literal("Pick"), {answers, 2}) &&
              guild_access_set_rule(&draft, 1, utf8_literal("No spam")) &&
              !guild_access_set_rule(&draft, 3, utf8_literal("Gap")) &&
              !guild_access_set_rule(&draft, 2, utf8_literal(" ")),
          "access: questions and rules add in place; blank or out-of-order rules are refused");
    const u32 applied = guild_access_fields(draft, GuildJoinType::apply, {fields, 5}, {values, 64});
    check(applied == 3 && fields[0].type == GuildVerificationFieldType::terms &&
              utf8_equal(fields[0].label, utf8_literal("Rules")) && fields[0].values.count == 2 &&
              fields[2].type == GuildVerificationFieldType::multiple_choice && fields[2].values.count == 2 &&
              guild_access_verification_dirty(apply, base, draft, GuildJoinType::apply, true),
          "access: Apply to Join sends every field in order, the rules where they were read");
    check(guild_access_remove_question(&draft, 0) && draft.terms_position == 0 && draft.question_count == 1 &&
              guild_access_remove_rule(&draft, 0) && draft.rule_count == 1,
          "access: removing keeps the rules' place");
    static GuildAccessForm none;
    guild_access_form_read(nullptr, &none);
    check(guild_access_check(none, GuildJoinType::apply, false) == GuildAccessProblem::question_missing &&
              guild_access_check(none, GuildJoinType::invite, true) == GuildAccessProblem::rules_missing &&
              guild_access_check(none, GuildJoinType::invite, false) == GuildAccessProblem::none,
          "access: Apply to Join needs a question and Server Rules needs a rule");
    guild_access_apply_defaults(&none);
    check(none.question_count == 1 &&
              utf8_equal(guild_access_text(none.questions[0].label), utf8_literal("Why do you want to join our server?")),
          "access: Apply to Join starts with the official question");
    check(guild_locale_index(utf8_literal("es-419")) == 16 &&
              guild_locale_index(utf8_literal("en-GB")) == kGuildLocaleCount &&
              utf8_equal(guild_locale(0).code, utf8_literal("en-US")) &&
              utf8_equal(guild_locale(0).name, utf8_literal("English, US")) &&
              utf8_equal(guild_locale(16).name, utf8_literal("Español, LATAM")) &&
              utf8_equal(guild_locale(kGuildLocaleCount - 1).code, utf8_literal("hi")) &&
              utf8_equal(guild_locale(kGuildLocaleCount - 1).name, utf8_literal("हिंदी")),
          "community: the language list is the official one without en-GB");
    check(guild_feature_bit(utf8_literal("MEMBER_VERIFICATION_GATE_ENABLED")) == guild_feature_verification_gate &&
              guild_feature_bit(utf8_literal("DISCOVERABLE")) == guild_feature_discoverable &&
              utf8_equal(guild_feature_name(guild_feature_preview_enabled), utf8_literal("PREVIEW_ENABLED")),
          "features: the access features read and write by name");
}

} // namespace maicord::community_admin_test

MAICORD_TEST_ENTRY(maicord_community_admin_entry) {
    using namespace maicord::community_admin_test;
    test_targets();
    test_verification();
    test_templates();
    test_discovery();
    test_guild_patch();
    test_access_model();
    maicord::test_exit(failures, "community admin");
}
