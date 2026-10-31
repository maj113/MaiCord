// Membership screening, onboarding and scheduled events: request targets and
// bodies, response decoding, the events store, the Gateway projections, the
// channel-list events row and event link cards.
#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/scheduled_event_store.hpp"
#include "maicord/protocol/guild_assets.hpp"
#include "maicord/protocol/guild_community.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/ui/guild_banner.hpp"
#include "maicord/ui/guild_card_scene.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/media/asset.hpp"

namespace maicord::guild_community_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}
bool text_is(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}
u32 scratch[16384];
// A baseline greyscale JPEG of a flat picture: one quantization table, a
// one-code DC table (difference 0) and AC table (end of block), each block
// two zero bits. Returns its bytes (0 when it does not fit).
u32 flat_jpeg(u32 width, u32 height, u8* out, u32 capacity) noexcept {
    const u32 blocks = ((width + 7) / 8) * ((height + 7) / 8);
    const u32 entropy = (blocks * 2 + 7) / 8;
    u32 n = 0;
    const auto put = [&](u32 value) noexcept {
        if (n < capacity)
            out[n] = static_cast<u8>(value);
        ++n;
    };
    put(0xFF), put(0xD8);
    put(0xFF), put(0xDB), put(0), put(67), put(0);
    for (u32 i = 0; i < 64; ++i)
        put(1);
    put(0xFF), put(0xC0), put(0), put(11), put(8), put(height >> 8), put(height), put(width >> 8), put(width);
    put(1), put(1), put(0x11), put(0);
    for (u32 table = 0; table <= 0x10; table += 0x10) {
        put(0xFF), put(0xC4), put(0), put(20), put(table), put(1);
        for (u32 i = 1; i < 16; ++i)
            put(0);
        put(0);
    }
    put(0xFF), put(0xDA), put(0), put(8), put(1), put(1), put(0), put(0), put(63), put(0);
    for (u32 i = 0; i < entropy; ++i)
        put(i + 1 == entropy && (blocks * 2) % 8 ? 0xFFu >> ((blocks * 2) % 8) : 0u);
    put(0xFF), put(0xD9);
    return n <= capacity ? n : 0;
}

Utf8View target_of(GuildCommunityOperation op, u64 guild, u64 event, const char* body, RestMethod* method,
                   u8* storage, u32 capacity) noexcept {
    GuildCommunityRequest request{};
    request.operation = op;
    request.guild_id = guild;
    request.event_id = event;
    if (body) {
        request.body = reinterpret_cast<const u8*>(body);
        request.body_bytes = c_string_length(body);
    }
    const auto written = guild_community_write_target(request, {storage, capacity});
    if (!written.ok())
        return {};
    *method = written.method;
    return {reinterpret_cast<const char*>(storage), written.target_bytes};
}

void test_requests() noexcept {
    u8 target[kGuildCommunityMaximumTargetBytes];
    RestMethod method{};
    check(utf8_equal(target_of(GuildCommunityOperation::verification_get, 10, 0, nullptr, &method, target, sizeof(target)),
                     utf8_literal("/api/v10/guilds/10/member-verification?with_guild=false")) &&
              method == RestMethod::get,
          "community: the rules form is read from member-verification");
    check(utf8_equal(target_of(GuildCommunityOperation::verification_submit, 10, 0, "{\"version\":1}", &method, target,
                               sizeof(target)),
                     utf8_literal("/api/v10/guilds/10/requests/@me")) &&
              method == RestMethod::put,
          "community: answers are PUT to requests/@me");
    check(utf8_equal(target_of(GuildCommunityOperation::onboarding_get, 10, 0, nullptr, &method, target, sizeof(target)),
                     utf8_literal("/api/v10/guilds/10/onboarding")) &&
              method == RestMethod::get &&
              utf8_equal(target_of(GuildCommunityOperation::onboarding_submit, 10, 0, "{}", &method, target,
                                   sizeof(target)),
                         utf8_literal("/api/v10/guilds/10/onboarding-responses")) &&
              method == RestMethod::post &&
              target_of(GuildCommunityOperation::onboarding_update, 10, 0, "{}", &method, target, sizeof(target)).bytes &&
              method == RestMethod::put,
          "community: onboarding finishes with POST and Channels & Roles saves with PUT");
    check(utf8_equal(target_of(GuildCommunityOperation::events_list, 10, 0, nullptr, &method, target, sizeof(target)),
                     utf8_literal("/api/v10/guilds/10/scheduled-events?with_user_count=true")) &&
              utf8_equal(target_of(GuildCommunityOperation::event_get, 10, 77, nullptr, &method, target, sizeof(target)),
                         utf8_literal("/api/v10/guilds/10/scheduled-events/77?with_user_count=true")) &&
              utf8_equal(target_of(GuildCommunityOperation::event_create, 10, 0, "{\"name\":\"x\"}", &method, target,
                                   sizeof(target)),
                         utf8_literal("/api/v10/guilds/10/scheduled-events")) &&
              method == RestMethod::post,
          "community: events are listed with counts, read singly and created by POST");
    check(utf8_equal(target_of(GuildCommunityOperation::event_interested, 10, 77, nullptr, &method, target,
                               sizeof(target)),
                     utf8_literal("/api/v10/guilds/10/scheduled-events/77/users/@me")) &&
              method == RestMethod::put &&
              target_of(GuildCommunityOperation::event_uninterested, 10, 77, nullptr, &method, target, sizeof(target))
                  .bytes &&
              method == RestMethod::delete_method &&
              utf8_equal(target_of(GuildCommunityOperation::event_rsvps, 10, 0, nullptr, &method, target,
                                   sizeof(target)),
                         utf8_literal("/api/v10/users/@me/scheduled-events?guild_ids=10")),
          "community: Interested is PUT/DELETE on users/@me and own RSVPs come from the user route");
    u8 body[32];
    const u32 interest = guild_community_write_interest_body({body, sizeof(body)});
    check(text_is(body, interest, utf8_literal("{\"response\":1}")), "community: Interested sends response 1");
    GuildCommunityRequest bad{};
    bad.operation = GuildCommunityOperation::event_interested;
    bad.guild_id = 10;
    const bool missing_event = !guild_community_request_valid(bad);
    bad.event_id = 5;
    bad.operation = GuildCommunityOperation::events_list;
    const bool stray_event = !guild_community_request_valid(bad);
    bad.event_id = 0;
    bad.operation = GuildCommunityOperation::event_create;
    bad.body = reinterpret_cast<const u8*>("[1]");
    bad.body_bytes = 3;
    const bool array_body = !guild_community_request_valid(bad);
    bad.body = reinterpret_cast<const u8*>(" {} ");
    bad.body_bytes = 4;
    const bool object_body = guild_community_request_valid(bad);
    bad.operation = GuildCommunityOperation::events_list;
    const bool unexpected_body = !guild_community_request_valid(bad);
    bad.body = nullptr;
    bad.body_bytes = 0;
    bad.guild_id = 0;
    check(missing_event && stray_event && array_body && object_body && unexpected_body &&
              !guild_community_request_valid(bad),
          "community: requests need their IDs and exactly the bodies their routes take (JSON objects only)");
    check(guild_community_classify(204) == GuildCommunityOutcome::ok &&
              guild_community_classify(404) == GuildCommunityOutcome::not_found &&
              guild_community_classify(429) == GuildCommunityOutcome::rate_limited &&
              guild_community_classify(403) == GuildCommunityOutcome::forbidden,
          "community: statuses classify");
}

constexpr Utf8View kForm = utf8_literal(
    R"({"version":"2026-10-01T00:00:00.000000+00:00","description":"Welcome!","form_fields":[)"
    R"({"field_type":"TERMS","label":"Read and agree to the server rules","description":null,"automations":null,)"
    R"("required":true,"values":["Be kind","No spam"]},)"
    R"({"field_type":"PARAGRAPH","label":"Why join?","required":true,"placeholder":"Tell us"},)"
    R"({"field_type":"MULTIPLE_CHOICE","label":"Age","required":false,"choices":["13-17","18+"]}]})");

void test_verification() noexcept {
    static GuildVerificationForm form;
    const bool decoded = guild_verification_decode(kForm, {scratch, 16384}, &form);
    check(decoded && form.field_count == 3 && form.version_bytes == 32 &&
              utf8_equal(guild_verification_text(form, form.description), utf8_literal("Welcome!")) &&
              form.fields[0].type == GuildVerificationFieldType::terms && form.fields[0].required &&
              form.fields[0].value_count == 2 &&
              utf8_equal(guild_verification_text(form, form.fields[0].values[1]), utf8_literal("No spam")) &&
              form.fields[1].type == GuildVerificationFieldType::paragraph &&
              utf8_equal(guild_verification_text(form, form.fields[1].placeholder), utf8_literal("Tell us")) &&
              form.fields[2].type == GuildVerificationFieldType::multiple_choice && form.fields[2].value_count == 2 &&
              guild_verification_supported(form),
          "community: the rules form decodes rules, questions and choices");
    GuildVerificationAnswer answers[3]{};
    const bool incomplete = !guild_verification_answers_complete(form, {answers, 3});
    answers[0].agreed = true;
    answers[1].text = utf8_literal("   ");
    answers[1].answered = true;
    const bool blank = !guild_verification_answers_complete(form, {answers, 3});
    answers[1].text = utf8_literal("I like \"cats\"");
    check(incomplete && blank && guild_verification_answers_complete(form, {answers, 3}),
          "community: required rules and non-blank answers gate the submit; optional choices do not");
    answers[2].choice = 1;
    answers[2].answered = true;
    static u8 body[8192];
    const u32 bytes = guild_verification_write_submit(kForm, {answers, 3}, {scratch, 16384}, {body, sizeof(body)});
    const Utf8View text{reinterpret_cast<const char*>(body), bytes};
    check(bytes && test_contains(text, utf8_literal(R"({"version":"2026-10-01T00:00:00.000000+00:00","form_fields":[)")) &&
              test_contains(text, utf8_literal(R"("values":["Be kind","No spam"],"response":true})")) &&
              test_contains(text, utf8_literal(R"("automations":null,)")) &&
              test_contains(text, utf8_literal(R"("placeholder":"Tell us","response":"I like \"cats\""})")) &&
              test_contains(text, utf8_literal(R"("choices":["13-17","18+"],"response":1}]})")),
          "community: the submit echoes each original field with its response");
    check(guild_verification_decode_status(utf8_literal(R"({"application_status":"SUBMITTED","guild_id":"1"})"),
                                           {scratch, 16384}) == GuildApplicationStatus::submitted &&
              guild_verification_decode_status(utf8_literal(R"({"application_status":"APPROVED"})"),
                                               {scratch, 16384}) == GuildApplicationStatus::approved,
          "community: the join request status decodes");
    static GuildVerificationForm unknown;
    check(guild_verification_decode(
              utf8_literal(R"({"version":"1","form_fields":[{"field_type":"FUTURE","required":true}]})"),
              {scratch, 16384}, &unknown) &&
              !guild_verification_supported(unknown) &&
              !guild_verification_decode(utf8_literal(R"({"version":"1"})"), {scratch, 16384}, &unknown),
          "community: unknown required fields are not answerable here; bodies without fields are refused");

    // Eleven valid Unicode rules exactly fill the fixed text bank. The last
    // rule grows by one scalar in the second pass, without exceeding its schema
    // limit, so accepting it would agree to text that was not retained.
    static u8 unicode_body[kGuildCommunityMaximumBodyBytes];
    static u8 submitted[kGuildCommunityMaximumBodyBytes];
    const auto unicode_form = [&](u32 tail_characters) noexcept -> Utf8View {
        ProtocolWriter writer{{unicode_body, sizeof(unicode_body)}, 0};
        bool ok = protocol_writer_append_literal(
            &writer, R"({"version":"1","form_fields":[{"field_type":"TERMS","required":true,"values":[)");
        for (u32 value = 0; ok && value < 11; ++value) {
            ok = (value == 0 || protocol_writer_append_byte(&writer, ',')) &&
                 protocol_writer_append_byte(&writer, '"');
            const u32 characters = value < 10 ? 300 : tail_characters;
            for (u32 i = 0; ok && i < characters; ++i)
                ok = protocol_writer_append_literal(&writer, "\\ud83d\\ude00");
            ok = ok && protocol_writer_append_byte(&writer, '"');
        }
        ok = ok && protocol_writer_append_literal(&writer, "]}]}");
        return {reinterpret_cast<const char*>(unicode_body), ok ? writer.used : 0};
    };
    GuildVerificationAnswer agreed{};
    agreed.agreed = true;
    const auto exact = unicode_form(72);
    check(exact.bytes && guild_verification_decode(exact, {scratch, 16384}, &form) && !form.truncated &&
              form.text_used == kGuildVerificationTextBytes && form.fields[0].value_count == 11 &&
              form.fields[0].values[0].bytes == 1200 && form.fields[0].values[10].bytes == 288 &&
              test_contains(guild_verification_text(form, form.fields[0].values[10]),
                       utf8_literal("\xF0\x9F\x98\x80")) &&
              guild_verification_supported(form) &&
              guild_verification_write_submit(exact, {&agreed, 1}, {scratch, 16384},
                                              {submitted, sizeof(submitted)}),
          "community: escaped Unicode rules fit exactly and remain submit-ready without byte loss");
    const auto over = unicode_form(73);
    check(over.bytes && guild_verification_decode(over, {scratch, 16384}, &form) && form.field_count == 1 &&
              form.truncated && !guild_verification_supported(form) &&
              !guild_verification_write_submit(over, {&agreed, 1}, {scratch, 16384},
                                               {submitted, sizeof(submitted)}),
          "community: one lost Unicode scalar makes even a one-field form unanswerable and unwritable");
    ProtocolWriter label{{unicode_body, sizeof(unicode_body)}, 0};
    bool label_ok = protocol_writer_append_literal(
        &label, R"({"form_fields":[{"field_type":"TERMS","required":true,"label":")");
    for (u32 i = 0; label_ok && i < 300; ++i)
        label_ok = protocol_writer_append_literal(&label, "\\u00e9");
    label_ok = label_ok && protocol_writer_append_literal(&label, R"(","values":["Rule"]}]})");
    const Utf8View long_label{reinterpret_cast<const char*>(unicode_body), label.used};
    check(label_ok && guild_verification_decode(long_label, {scratch, 16384}, &form) && form.truncated &&
              !guild_verification_supported(form) &&
              !guild_verification_write_submit(long_label, {&agreed, 1}, {scratch, 16384},
                                               {submitted, sizeof(submitted)}),
          "community: a valid Unicode label exceeding its fixed byte budget cannot lose text silently");
    const Utf8View malformed_values[] = {
        utf8_literal(R"({"form_fields":[{"field_type":"TERMS","required":true,"values":["Visible",7]}]})"),
        utf8_literal(
            R"({"form_fields":[{"field_type":"TERMS","required":true,"values":["Visible",null]}]})"),
        utf8_literal(R"({"form_fields":[{"field_type":"TERMS","required":true,"values":"Hidden"}]})"),
        utf8_literal(R"({"form_fields":[{"required":true,"values":null,"field_type":"TERMS"}]})"),
        utf8_literal(R"({"form_fields":[{"field_type":"TERMS","required":true,"values":[]}]})"),
        utf8_literal(R"({"form_fields":[{"field_type":"TERMS","required":true,"values":["Rule"]},null]})")};
    for (const auto invalid : malformed_values)
        check(guild_verification_decode(invalid, {scratch, 16384}, &form) && form.truncated &&
                  !guild_verification_supported(form) &&
                  !guild_verification_write_submit(invalid, {&agreed, 1}, {scratch, 16384},
                                                   {submitted, sizeof(submitted)}),
              "community: malformed rules cannot be silently omitted from consent");
}

constexpr Utf8View kOnboarding = utf8_literal(
    R"({"guild_id":"10","enabled":true,"mode":0,"default_channel_ids":["300","301"],"responses":["502","701"],)"
    R"("prompts":[{"id":"40","type":0,"title":"What brings you here?","single_select":false,"required":true,)"
    R"("in_onboarding":true,"options":[{"id":"501","title":"Games","description":"Play together",)"
    R"("emoji":{"id":null,"name":"🎮"},"role_ids":["7"],"channel_ids":["300","301"]},)"
    R"({"id":"502","title":"Art","description":null,"emoji":null,"role_ids":[],"channel_ids":[]}]},)"
    R"({"id":"41","type":1,"title":"Region","single_select":true,"required":false,"in_onboarding":true,)"
    R"("options":[{"id":"601","title":"EU"},{"id":"602","title":"NA"}]},)"
    R"({"id":"42","type":0,"title":"Later","single_select":false,"required":false,"in_onboarding":false,)"
    R"("options":[{"id":"701","title":"Extra"}]}]})");

void test_onboarding() noexcept {
    static GuildOnboarding onboarding;
    const bool decoded = guild_onboarding_decode(kOnboarding, {scratch, 16384}, &onboarding);
    u16 flow[8];
    const u32 flow_count = guild_onboarding_flow_prompts(onboarding, {flow, 8});
    check(decoded && onboarding.enabled && onboarding.prompt_count == 3 && onboarding.option_count == 5 &&
              onboarding.default_channel_count == 2 && onboarding.default_channels[1] == 301 &&
              onboarding.prompts[0].option_count == 2 && onboarding.prompts[1].first_option == 2 &&
              onboarding.prompts[1].single_select && onboarding.options[0].role_count == 1 &&
              onboarding.options[0].channel_count == 2 &&
              utf8_equal(guild_onboarding_text(onboarding, onboarding.options[0].emoji),
                         utf8_literal("\xF0\x9F\x8E\xAE")) &&
              !onboarding.options[0].selected && onboarding.options[1].selected &&
              onboarding.options[4].selected && flow_count == 2 && flow[1] == 1,
          "community: onboarding decodes prompts, options, saved responses and the flow");
    // Single-select keeps one option.
    guild_onboarding_toggle(&onboarding, 1, 0);
    guild_onboarding_toggle(&onboarding, 1, 1);
    const bool single = !onboarding.options[2].selected && onboarding.options[3].selected;
    guild_onboarding_toggle(&onboarding, 0, 1); // clear Art: the required prompt is unanswered
    const bool required = onboarding.prompts[0].required && !guild_onboarding_prompt_answered(onboarding, 0);
    guild_onboarding_toggle(&onboarding, 0, 0);
    check(single && required && guild_onboarding_prompt_answered(onboarding, 0),
          "community: single-select prompts keep one choice and required prompts gate Finish");
    static u8 body[4096];
    const u32 bytes = guild_onboarding_write_responses(onboarding, 1700000000000ull, {body, sizeof(body)});
    check(text_is(body, bytes,
                  utf8_literal(R"({"onboarding_responses":["501","602"],)"
                               R"("onboarding_prompts_seen":{"40":1700000000000,"41":1700000000000},)"
                               R"("onboarding_responses_seen":{"501":1700000000000,"502":1700000000000,)"
                               R"("601":1700000000000,"602":1700000000000}})")),
          "community: responses carry selected options and every flow prompt and option as seen");
    const u32 edited =
        guild_onboarding_write_responses(onboarding, 1700000000000ull, {body, sizeof(body)}, true);
    check(text_is(
              body, edited,
              utf8_literal(
                  R"({"onboarding_responses":["501","602","701"],)"
                  R"("onboarding_prompts_seen":{"40":1700000000000,"41":1700000000000,"42":1700000000000},)"
                  R"("onboarding_responses_seen":{"501":1700000000000,"502":1700000000000,)"
                  R"("601":1700000000000,"602":1700000000000,"701":1700000000000}})")),
          "community: editing preserves non-flow choices and marks all prompts and options seen");

    const Utf8View incomplete[] = {
        utf8_literal(R"({"prompts":[{"id":"1","options":[{"id":"2"}]}],"responses":["3"]})"),
        utf8_literal(R"({"prompts":[{"id":"1","options":[{"id":"2"}]}],"responses":["2","bad"]})"),
        utf8_literal(R"({"prompts":[],"responses":["0"]})"),
        utf8_literal(R"({"prompts":[],"responses":[2]})"),
        utf8_literal(R"({"prompts":[],"responses":null})"),
        utf8_literal(R"({"prompts":[],"responses":{}})"),
        utf8_literal(R"({"prompts":[],"responses":[],"responses":[]})"),
        utf8_literal(R"({"prompts":null})"),
        utf8_literal(R"({"prompts":{}})"),
        utf8_literal(R"({"prompts":[{"id":"1","options":null}]})")};
    for (const auto invalid : incomplete)
        check(guild_onboarding_decode(invalid, {scratch, 16384}, &onboarding) && onboarding.truncated &&
                  !guild_onboarding_write_responses(onboarding, 1, {body, sizeof(body)}) &&
                  !guild_onboarding_write_responses(onboarding, 1, {body, sizeof(body)}, true),
              "community: malformed or unmatched saved choices cannot become a partial replacement");

    const Utf8View empty_responses[] = {
        utf8_literal(R"({"prompts":[{"id":"1","options":[{"id":"2"}]}]})"),
        utf8_literal(R"({"prompts":[{"id":"1","options":[{"id":"2"}]}],"responses":[]})")};
    for (const auto empty : empty_responses)
        check(guild_onboarding_decode(empty, {scratch, 16384}, &onboarding) && !onboarding.truncated &&
                  !onboarding.options[0].selected &&
                  guild_onboarding_write_responses(onboarding, 1, {body, sizeof(body)}, true),
              "community: a complete successful GET may omit saved choices or return an empty list");

    static u8 capacity_body[8192];
    const struct {
        u32 options, count;
        u64 final_id;
    } responses[] = {{128, 128, 0}, {128, 750, 0}, {128, 751, 0}, {128, 750, 9999}, {129, 128, 0}};
    for (const auto response : responses) {
        ProtocolWriter writer{{capacity_body, sizeof(capacity_body)}, 0};
        bool ok = protocol_writer_append_literal(
            &writer, R"({"prompts":[{"id":"1","in_onboarding":true,"options":[)");
        for (u32 i = 0; ok && i < response.options; ++i)
            ok = (i == 0 || protocol_writer_append_byte(&writer, ',')) &&
                 protocol_writer_append_literal(&writer, "{\"id\":") &&
                 protocol_writer_append_snowflake(&writer, 1000 + i) &&
                 protocol_writer_append_byte(&writer, '}');
        ok = ok && protocol_writer_append_literal(&writer, R"(]}],"responses":[)");
        for (u32 i = 0; ok && i < response.count; ++i)
            ok = (i == 0 || protocol_writer_append_byte(&writer, ',')) &&
                 protocol_writer_append_snowflake(&writer, i + 1 == response.count && response.final_id
                                                               ? response.final_id
                                                               : 1000 + i % kGuildOnboardingOptionMaximum);
        ok = ok && protocol_writer_append_literal(&writer, "]}");
        const Utf8View input{reinterpret_cast<const char*>(capacity_body), writer.used};
        const bool complete = response.options <= kGuildOnboardingOptionMaximum && response.count <= 750 &&
                              !response.final_id;
        const bool parsed = ok && guild_onboarding_decode(input, {scratch, 16384}, &onboarding);
        u32 selected = 0;
        for (u32 i = 0; i < onboarding.option_count; ++i)
            selected += onboarding.options[i].selected;
        check(parsed && onboarding.option_count == kGuildOnboardingOptionMaximum &&
                  selected == kGuildOnboardingOptionMaximum && onboarding.truncated != complete &&
                  (guild_onboarding_write_responses(onboarding, 1, {body, sizeof(body)}, true) != 0) ==
                      complete,
              "community: retained choices round-trip through the response limit; overflow refuses "
              "replacement");
    }
}

constexpr Utf8View kEvent = utf8_literal(
    R"({"id":"900","guild_id":"10","channel_id":null,"creator_id":"5","name":"Game night",)"
    R"("description":"Bring snacks","scheduled_start_time":"2030-01-02T18:00:00+00:00",)"
    R"("scheduled_end_time":"2030-01-02T20:00:00+00:00","privacy_level":2,"status":1,"entity_type":3,)"
    R"("entity_id":null,"entity_metadata":{"location":"The park"},"user_count":4,"image":"abc123"})");

bool add_to(void* context, const ScheduledEvent& event) noexcept {
    return scheduled_event_store_upsert(static_cast<ScheduledEventStore*>(context), event) || true;
}

void test_events() noexcept {
    ScheduledEvent event{};
    const bool decoded = scheduled_event_decode(kEvent, {scratch, 16384}, &event);
    check(decoded && event.id == 900 && event.guild_id == 10 && !event.channel_id &&
              event.start_ms == 1893607200000ull && event.end_ms == 1893614400000ull &&
              event.status == ScheduledEventStatus::scheduled && event.entity == ScheduledEventEntity::external &&
              event.user_count_known && event.user_count == 4 &&
              utf8_equal({event.location, event.location_bytes}, utf8_literal("The park")) &&
              utf8_equal({event.name, event.name_bytes}, utf8_literal("Game night")) && event.image_bytes == 6,
          "community: a scheduled event decodes time, place, status, cover and count");
    static ScheduledEventStore store;
    scheduled_event_store_reset(&store);
    const u32 listed = scheduled_event_decode_list(
        utf8_literal(R"([{"id":"901","guild_id":"10","name":"Later","scheduled_start_time":"2030-02-01T00:00:00Z",)"
                     R"("status":1,"entity_type":2,"channel_id":"20"},)"
                     R"({"id":"902","guild_id":"10","name":"Live","scheduled_start_time":"2030-03-01T00:00:00Z",)"
                     R"("status":2,"entity_type":1,"channel_id":"21"},{"bad":true},)"
                     R"({"id":"903","guild_id":"11","name":"Elsewhere","scheduled_start_time":"2030-01-01T00:00:00Z",)"
                     R"("status":1,"entity_type":3}])"),
        {scratch, 16384}, add_to, &store);
    scheduled_event_store_upsert(&store, event);
    const ScheduledEvent* ordered[8]{};
    const u32 count = scheduled_event_store_guild_events(store, 10, {ordered, 8});
    check(listed == 3 && count == 3 && ordered[0]->id == 902 && ordered[1]->id == 900 && ordered[2]->id == 901 &&
              scheduled_event_store_guild_count(store, 10) == 3 && scheduled_event_store_guild_count(store, 11) == 1,
          "community: the store lists a server's events live first, then by start; bad rows are skipped");
    // A Gateway update carries no count or interest; both are kept.
    scheduled_event_store_set_interest(&store, 900, true, false);
    ScheduledEvent update = event;
    update.user_count_known = false;
    update.user_count = 0;
    update.name_bytes = 4;
    scheduled_event_store_upsert(&store, update);
    const auto* kept = scheduled_event_store_find(store, 900);
    check(kept && kept->user_count_known && kept->user_count == 4 && kept->interested && kept->name_bytes == 4,
          "community: updates keep the known count and the account's interest");
    ScheduledEventUser user{};
    const bool user_decoded = scheduled_event_decode_user(
        utf8_literal(R"({"guild_scheduled_event_id":"900","user_id":"8","guild_id":"10"})"), {scratch, 16384}, &user);
    scheduled_event_store_user(&store, user.event_id, user.user_id, 1, true);
    const bool counted = scheduled_event_store_find(store, 900)->user_count == 5;
    // Our own confirmed change is not counted twice.
    const bool own = !scheduled_event_store_user(&store, 900, 1, 1, true);
    scheduled_event_store_set_interest(&store, 900, false, true);
    check(user_decoded && user.user_id == 8 && counted && own && scheduled_event_store_find(store, 900)->user_count == 4 &&
              !scheduled_event_store_find(store, 900)->interested,
          "community: USER_ADD/REMOVE move the count and the account's own row sets its interest");
    ScheduledEvent ended = event;
    ended.status = ScheduledEventStatus::completed;
    check(scheduled_event_store_upsert(&store, ended) && !scheduled_event_store_find(store, 900) &&
              scheduled_event_store_clear_guild(&store, 11) && scheduled_event_store_guild_count(store, 11) == 0 &&
              scheduled_event_store_remove(&store, 901) && scheduled_event_store_guild_count(store, 10) == 1,
          "community: finished events leave the store; guild clears and deletes remove events");
    scheduled_event_store_reset(&store);
    for (u64 i = 0; i < kScheduledEventStoreCapacity + 4; ++i) {
        ScheduledEvent filler = event;
        filler.id = 1000 + i;
        filler.start_ms = 2'000'000'000'000ull - i * 1000; // later ones start earlier
        scheduled_event_store_upsert(&store, filler);
    }
    check(store.count == kScheduledEventStoreCapacity && scheduled_event_store_find(store, 1000 + kScheduledEventStoreCapacity + 3) &&
              !scheduled_event_store_find(store, 1000),
          "community: a full store keeps the soonest events");
    u32 rsvps = 0;
    scheduled_event_decode_users(utf8_literal(R"([{"guild_scheduled_event_id":"5","user_id":"1","response":1}])"),
                                 {scratch, 16384},
                                 +[](void* context, const ScheduledEventUser& row) noexcept {
                                     *static_cast<u32*>(context) += row.event_id == 5;
                                     return true;
                                 },
                                 &rsvps);
    check(rsvps == 1, "community: own RSVP rows decode");
}

void test_create() noexcept {
    ScheduledEventCreate create{};
    create.name = utf8_literal("Movie \"night\"");
    create.entity = ScheduledEventEntity::external;
    create.location = utf8_literal("Cinema");
    create.start_unix_seconds = 1'900'000'000;
    const auto no_end = scheduled_event_create_validate(create, 1'800'000'000);
    create.end_unix_seconds = 1'900'003'600;
    const auto past = scheduled_event_create_validate(create, 1'950'000'000);
    static u8 body[2048];
    const u32 bytes = scheduled_event_write_create(create, {body, sizeof(body)});
    check(no_end == ScheduledEventCreateError::end && past == ScheduledEventCreateError::start &&
              text_is(body, bytes,
                      utf8_literal(R"({"name":"Movie \"night\"","description":null,"privacy_level":2,)"
                                   R"("scheduled_start_time":"2030-03-17T17:46:40+00:00",)"
                                   R"("scheduled_end_time":"2030-03-17T18:46:40+00:00","entity_type":3,)"
                                   R"("channel_id":null,"entity_metadata":{"location":"Cinema"}})")),
          "community: external events need a location and an end; the body sends both");
    ScheduledEventCreate voice{};
    voice.name = utf8_literal("Hangout");
    voice.description = utf8_literal("Chill");
    voice.entity = ScheduledEventEntity::voice;
    voice.start_unix_seconds = 1'900'000'000;
    const auto no_channel = scheduled_event_create_validate(voice, 0);
    voice.channel_id = 20;
    const u32 voice_bytes = scheduled_event_write_create(voice, {body, sizeof(body)});
    check(no_channel == ScheduledEventCreateError::channel &&
              text_is(body, voice_bytes,
                      utf8_literal(R"({"name":"Hangout","description":"Chill","privacy_level":2,)"
                                   R"("scheduled_start_time":"2030-03-17T17:46:40+00:00","scheduled_end_time":null,)"
                                   R"("entity_type":2,"channel_id":"20","entity_metadata":null})")),
          "community: voice events need a channel and may stay open-ended");
    ScheduledEventCreate blank = voice;
    blank.name = utf8_literal("   ");
    check(scheduled_event_create_validate(blank, 0) == ScheduledEventCreateError::name &&
              !scheduled_event_write_create(blank, {body, sizeof(body)}),
          "community: a blank topic is refused");
    char link[96];
    const u32 link_bytes = scheduled_event_write_link(10, 900, {link, sizeof(link)});
    check(utf8_equal({link, link_bytes}, utf8_literal("https://discord.com/events/10/900")),
          "community: the share link names the server and the event");
}

// Server tag popout: GET /guilds/{g}/profile and the discoverable join, the
// profile decode (features, traits by position, brand colour) and the card.
void test_guild_profile() noexcept {
    u8 target[kGuildCommunityMaximumTargetBytes];
    RestMethod method{};
    check(utf8_equal(target_of(GuildCommunityOperation::guild_profile_get, 10, 0, nullptr, &method, target,
                               sizeof(target)),
                     utf8_literal("/api/v10/guilds/10/profile")) &&
              method == RestMethod::get &&
              utf8_equal(target_of(GuildCommunityOperation::guild_join, 10, 0, nullptr, &method, target, sizeof(target)),
                         utf8_literal("/api/v10/guilds/10/members/@me?lurker=true")) &&
              method == RestMethod::put,
          "guild profile: the popout reads GET /guilds/{g}/profile and lurks with PUT members/@me?lurker=true");
    // The official client's lurking join names its Gateway session (the
    // GUILD_CREATE goes there) and sends X-Context-Properties of {source:
    // undefined}, i.e. base64("{}").
    GuildCommunityRequest lurk{};
    lurk.operation = GuildCommunityOperation::guild_join;
    lurk.guild_id = 222333444555666777ull;
    const auto with_session =
        guild_community_write_target(lurk, {target, sizeof(target)}, utf8_literal("0123abcdEF4567890123abcdef456789"));
    const auto bad_session = guild_community_write_target(lurk, {target, sizeof(target)}, utf8_literal("a&b=c"));
    u8 header[64];
    const u32 header_bytes = guild_community_write_join_context({header, sizeof(header)});
    check(with_session.ok() && !bad_session.ok() &&
              text_is(header, header_bytes, utf8_literal("X-Context-Properties: e30=\r\n")),
          "guild profile: the lurk names an alphanumeric session and carries X-Context-Properties e30=");
    const auto rewritten =
        guild_community_write_target(lurk, {target, sizeof(target)}, utf8_literal("0123abcdEF4567890123abcdef456789"));
    check(rewritten.ok() &&
              text_is(target, rewritten.target_bytes,
                      utf8_literal("/api/v10/guilds/222333444555666777/members/@me?lurker=true&session_id="
                                   "0123abcdEF4567890123abcdef456789")),
          "guild profile: the session joins the lurk query as &session_id=");
    u8 body[8];
    const u32 join = guild_community_write_join_body({body, sizeof(body)});
    GuildCommunityRequest with_body{};
    with_body.operation = GuildCommunityOperation::guild_join;
    with_body.guild_id = 10;
    with_body.body = body;
    with_body.body_bytes = join;
    check(text_is(body, join, utf8_literal("{}")) && !guild_community_request_valid(with_body),
          "guild profile: the join carries the module's fixed {} body, never the caller's");
    static GuildProfile profile{};
    const auto json = utf8_literal(
        R"({"id":"1234567890123456789","name":"Native Server","description":"A friendly place.",)"
        R"("icon_hash":"a_0123456789abcdef0123456789abcdef","custom_banner_hash":null,"online_count":1234,)"
        R"("member_count":56789,"brand_color_primary":"#5865F2","visibility":3,"tag":"GEAR","badge":7,)"
        R"("badge_hash":"fedcba9876543210fedcba9876543210","features":["GUILD_TAGS","DISCOVERABLE","COMMUNITY"],)"
        R"("traits":[{"label":"Second","position":1,"emoji_id":null},{"label":"First","position":0},)"
        R"({"label":"Bad","position":9}],"game_application_ids":[],"premium_tier":2})");
    check(guild_profile_decode(json, {scratch, 16384}, &profile) && profile.id == 1234567890123456789ull &&
              utf8_equal({profile.name, profile.name_bytes}, utf8_literal("Native Server")) &&
              utf8_equal({profile.tag, profile.tag_bytes}, utf8_literal("GEAR")) && profile.icon_bytes == 34 &&
              profile.badge_bytes == 32 && !profile.banner_bytes && profile.online_count == 1234 &&
              profile.member_count == 56789 && profile.has_brand_color && profile.brand_color == 0x5865F2 &&
              profile.visibility == GuildProfileVisibility::public_with_recruitment &&
              profile.features == (guild_profile_tags | guild_profile_discoverable) && profile.trait_count == 2 &&
              utf8_equal({profile.traits[0].label, profile.traits[0].label_bytes}, utf8_literal("First")) &&
              utf8_equal({profile.traits[1].label, profile.traits[1].label_bytes}, utf8_literal("Second")),
          "guild profile: decode keeps counts, tag, badge, brand colour, known features and traits by position");
    // A large community server's profile as the server returns it (the invite's
    // embedded profile object): DISCOVERABLE with a custom banner, which the
    // card draws from the discovery-splash path, and no brand colour.
    const auto community = utf8_literal(
        R"({"id":"222333444555666777","name":"Cogwheel Club","icon_hash":"a_00112233445566778899aabbccddeeff",)"
        R"("member_count":123456,"online_count":34567,"description":"Community for a fictional factory game.",)"
        R"("banner_hash":null,"game_application_ids":["500000000000000011"],"game_activity":{"500000000000000011":)"
        R"({"activity_level":2,"activity_score":50000}},"tag":"GEAR","badge":5,"badge_color_primary":"#eebe1a",)"
        R"("badge_color_secondary":"#fffc7f","badge_hash":"0f1e2d3c4b5a69788796a5b4c3d2e1f0","traits":[{"emoji_id":)"
        R"(null,"emoji_name":"gear","emoji_animated":false,"label":"Automation","position":0},{"emoji_id":null,)"
        R"("emoji_name":"video_game","emoji_animated":false,"label":"Video Game","position":1}],"features":)"
        R"(["MEMBER_VERIFICATION_GATE_ENABLED","PARTNERED","DISCOVERABLE","COMMUNITY"],"visibility":1,)"
        R"("custom_banner_hash":"1234abcd1234abcd1234abcd1234abcd","premium_subscription_count":49,"premium_tier":3})");
    UiGuildImageMedia banner{};
    const bool community_ok = guild_profile_decode(community, {scratch, 8192}, &profile);
    check(community_ok && (profile.features & guild_profile_discoverable) && !profile.has_brand_color &&
              utf8_equal({profile.banner, profile.banner_bytes}, utf8_literal("1234abcd1234abcd1234abcd1234abcd")) &&
              ui_guild_card_banner_media(profile.id, {profile.banner, profile.banner_bytes},
                                         (profile.features & guild_profile_discoverable) != 0, 96, &banner) &&
              utf8_equal(banner.url_view(),
                         utf8_literal("https://cdn.discordapp.com/discovery-splashes/222333444555666777/"
                                      "1234abcd1234abcd1234abcd1234abcd.jpg?size=512")) &&
              media_asset_key_valid(banner.key) && banner.key.kind == MediaAssetKind::profile_image &&
              media_asset_embed_format(banner.key) == MediaAttachmentFormat::jpeg,
          "guild profile: a discoverable profile's custom banner resolves the discovery-splash JPEG");
    // The card asks for 512 * dpr, capped at the 512 px the CDN JPEG decoder takes
    // (a 16:9 splash is 512x288 at size=512; the decoder refuses 1024x576).
    UiGuildImageMedia scaled_banner{}, private_banner{};
    bool capped = true;
    constexpr u32 kScales[]{120, 144, 192, 288};
    for (const u32 dpi : kScales)
        capped = capped &&
                 ui_guild_card_banner_media(profile.id, {profile.banner, profile.banner_bytes}, true, dpi,
                                            &scaled_banner) &&
                 utf8_equal(scaled_banner.url_view(), banner.url_view()) &&
                 media_asset_key_equal(scaled_banner.key, banner.key);
    static u8 jpeg[4096];
    static JpegWorkspace jpeg_workspace;
    JpegImage scaled{}, oversized{};
    const u32 scaled_bytes = flat_jpeg(512, 288, jpeg, sizeof(jpeg));
    const auto scaled_decode = jpeg_decode_coefficients({jpeg, scaled_bytes}, &scaled, &jpeg_workspace);
    const u32 oversized_bytes = flat_jpeg(1024, 576, jpeg, sizeof(jpeg));
    const auto oversized_decode = jpeg_decode_coefficients({jpeg, oversized_bytes}, &oversized, &jpeg_workspace);
    check(capped && scaled_bytes && scaled_decode.ok() && scaled.width == 512 && scaled.height == 288 &&
              oversized_bytes && oversized_decode.error == JpegError::dimensions &&
              !ui_guild_card_banner_media(profile.id, {profile.banner, profile.banner_bytes}, false, 96,
                                          &private_banner) &&
              !private_banner.url_bytes && !media_asset_key_valid(private_banner.key) &&
              !ui_guild_card_banner_media(profile.id, {}, true, 96, &private_banner),
          "guild profile: the card's banner asks for at most the 512 px rendition the JPEG decoder takes");
    check(!guild_profile_decode(utf8_literal(R"({"name":"No id"})"), {scratch, 16384}, &profile) &&
              !guild_profile_decode(utf8_literal(R"([1])"), {scratch, 16384}, &profile) &&
              guild_asset_error_code(utf8_literal(R"({"message":"Missing Access","code":50001})"),
                                     {scratch, 16384}) == kGuildProfilePrivateErrorCode,
          "guild profile: an identity-less body fails and 50001 names a private profile");

    // The card: a ready profile lays out top to bottom inside 300 DIPs and
    // offers its action; private and loading phases carry no button.
    static UiGuildCardScene card{};
    UiGuildCardContent content{};
    content.guild_id = 1234567890123456789ull;
    content.phase = UiGuildCardPhase::ready;
    content.name = utf8_literal("Native Server");
    content.description = utf8_literal("A friendly place.");
    content.tag = utf8_literal("GEAR");
    content.traits[0] = utf8_literal("First");
    content.traits[1] = utf8_literal("Second");
    content.trait_count = 2;
    content.online_count = 1234;
    content.member_count = 56789;
    content.action = UiGuildCardAction::join;
    const UiRect viewport{0, 0, 1280, 800}, chip{400, 300, 460, 320};
    bool valid = ui_guild_card_show(&card, content.guild_id, content, chip, viewport, 1000, true, nullptr);
    const UiRect panel = card.panel;
    const float bx = (card.button.left + card.button.right) * .5f + panel.left,
                by = (card.button.top + card.button.bottom) * .5f + panel.top;
    valid = valid && card.active && ui_rect_width(panel) == kUiGuildCardWidth && panel.left >= chip.right &&
            card.name.top >= card.icon.bottom && card.tag_chip.top >= card.name.bottom &&
            card.online.top >= card.tag_chip.bottom && card.established.top >= card.online.bottom &&
            card.description.top >= card.established.bottom && card.traits[0].top >= card.description.bottom &&
            card.traits[1].left >= card.traits[0].right && card.button.top >= card.traits[0].bottom &&
            card.button.bottom + panel.top <= panel.bottom &&
            utf8_equal({card.online_text, card.online_bytes}, utf8_literal("1,234 Online")) &&
            utf8_equal({card.members_text, card.members_bytes}, utf8_literal("56,789 Members")) &&
            utf8_equal({card.established_text, card.established_bytes}, utf8_literal("Est. Apr 2024")) &&
            ui_guild_card_hit(card, bx, by) == UiGuildCardControl::action &&
            ui_guild_card_hit(card, panel.left + 4, panel.top + 4) == UiGuildCardControl::none &&
            ui_guild_card_contains(card, panel.left + 4, panel.top + 4) && !ui_guild_card_contains(card, 10, 10);
    check(valid, "guild card: name, tag, counts, founding month, description, traits and Join stack in the card");
    const u32 revision = card.revision;
    content.action = UiGuildCardAction::none;
    valid = ui_guild_card_show(&card, content.guild_id, content, chip, viewport, 1000, true, nullptr) &&
            card.revision != revision && !ui_rect_has_area(card.button) &&
            ui_guild_card_hit(card, bx, by) == UiGuildCardControl::none;
    content.phase = UiGuildCardPhase::private_profile;
    valid = valid && ui_guild_card_show(&card, content.guild_id, content, chip, viewport, 1000, true, nullptr) &&
            ui_rect_has_area(card.status_title) && !ui_rect_has_area(card.name) && !ui_rect_has_area(card.button);
    content.phase = UiGuildCardPhase::failed;
    valid = valid && ui_guild_card_show(&card, content.guild_id, content, chip, viewport, 1000, true, nullptr) &&
            ui_guild_card_hit(card, card.panel.left + 20, card.panel.top + 20) == UiGuildCardControl::retry;
    // Against the right edge the card opens left of its chip.
    content.phase = UiGuildCardPhase::loading;
    valid = valid && ui_guild_card_show(&card, 99, content, {1200, 300, 1260, 320}, viewport, 1000, true, nullptr) &&
            card.panel.right <= 1200 && ui_guild_card_spinner_visible(card);
    ui_guild_card_close(&card);
    char digits[16];
    check(valid && !card.active && ui_guild_card_format_count(0, digits, 16) == 1 &&
              ui_guild_card_format_count(1000000, digits, 16) == 9 && bytes_equal(digits, "1,000,000", 9),
          "guild card: no action has no button; private, failed (retry) and loading phases; it flips left");
}

void test_links() noexcept {
    InviteLink link{};
    const bool event = message_markdown_invite_link(utf8_literal("https://discord.com/events/10/900"), &link);
    InviteLink recurring{};
    const bool recurrence =
        message_markdown_invite_link(utf8_literal("https://canary.discord.com/events/10/900/1234"), &recurring);
    InviteLink invite{};
    const bool plain = message_markdown_invite_link(utf8_literal("https://discord.gg/abc"), &invite);
    InviteLink broken{};
    check(event && invite_link_event(link) && link.event_guild_id == 10 && link.event_id == 900 &&
              utf8_equal({link.code, link.code_bytes}, utf8_literal("900")) && recurrence && recurring.event_id == 900 &&
              plain && !invite_link_event(invite) &&
              !message_markdown_invite_link(utf8_literal("https://discord.com/events/10"), &broken) &&
              !message_markdown_invite_link(utf8_literal("https://discord.gg/events/10/900"), &broken),
          "community: event links parse on service hosts (with a recurrence) and stay distinct from invites");
    InviteLink found[4]{};
    const u32 count = message_markdown_find_invites(
        utf8_literal("Join https://discord.gg/abc and https://discord.com/events/10/900, again "
                     "https://discord.com/events/10/900 or https://discord.com/events/11/900"),
        {found, 4});
    check(count == 3 && !invite_link_event(found[0]) && found[1].event_id == 900 && found[2].event_guild_id == 11,
          "community: message content yields invite and event cards without duplicates");
}

GatewayClientEvent event;
ConstSlice<u8> payload;
char storage[64 * 1024];
GatewayClientDispatchRouteResult route(Utf8View name, Utf8View data) noexcept {
    event = {};
    payload = {};
    GatewayEnvelope envelope{};
    envelope.opcode = 0;
    envelope.has_event_name = true;
    envelope.event_name = {name, 0, false};
    envelope.data = {data, 0, JsonValueKind::object};
    return gateway_client_route_dispatch(envelope, {scratch, 16384}, 64, {storage, sizeof(storage)}, {}, 7, &event,
                                         &payload);
}
struct Published {
    GatewayClientEvent event;
    char json[512];
    u32 bytes;
    u32 count;
};
bool publish(void* context, const GatewayClientEvent& value, ConstSlice<u8> data) noexcept {
    auto& p = *static_cast<Published*>(context);
    p.event = value;
    p.bytes = minimum<u32>(static_cast<u32>(data.count), sizeof(p.json));
    copy_bytes(p.json, data.data, p.bytes);
    ++p.count;
    return true;
}

void test_gateway() noexcept {
    const auto created = route(utf8_literal("GUILD_SCHEDULED_EVENT_CREATE"), kEvent);
    ScheduledEvent decoded{};
    check(created.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_scheduled_event_create &&
              scheduled_event_decode({reinterpret_cast<const char*>(payload.data), static_cast<u32>(payload.count)},
                                     {scratch, 16384}, &decoded) &&
              decoded.id == 900,
          "gateway: GUILD_SCHEDULED_EVENT_CREATE arrives verbatim");
    check(route(utf8_literal("GUILD_SCHEDULED_EVENT_USER_REMOVE"),
                utf8_literal(R"({"guild_scheduled_event_id":"900","user_id":"8","guild_id":"10"})"))
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::guild_scheduled_event_user_remove,
          "gateway: event user removals arrive verbatim");
    static Published published;
    published = {};
    const Utf8View guild = utf8_literal(
        R"({"id":"10","name":"G","guild_scheduled_events":[{"id":"1","guild_id":"10","name":"E",)"
        R"("scheduled_start_time":"2030-01-01T00:00:00Z","status":1,"entity_type":3}],"stage_instances":[]})");
    static char snapshot[kGatewayClientDispatchPayloadCapacity];
    gateway_client_publish_scheduled_events({guild, 0, JsonValueKind::object}, 10, {scratch, 16384},
                                            {snapshot, sizeof(snapshot)}, 3, 0, publish, &published);
    const bool none = [] {
        static Published empty;
        empty = {};
        gateway_client_publish_scheduled_events({utf8_literal(R"({"id":"10"})"), 0, JsonValueKind::object}, 10,
                                                {scratch, 16384}, {snapshot, sizeof(snapshot)}, 3, 0, publish, &empty);
        return empty.count == 0;
    }();
    check(published.count == 1 &&
              published.event.dispatch_kind == GatewayClientDispatchKind::guild_scheduled_event_snapshot &&
              published.event.voice_states.guild_id == 10 && published.json[0] == '[' &&
              scheduled_event_decode_list({published.json, published.bytes}, {scratch, 16384},
                                          +[](void*, const ScheduledEvent&) noexcept { return true; }, nullptr) == 1 &&
              none,
          "gateway: a guild's guild_scheduled_events become one snapshot; absent arrays publish nothing");
    // The current member's pending flag and onboarding flags.
    const auto pending = route(utf8_literal("GUILD_MEMBER_UPDATE"),
                               utf8_literal(R"({"guild_id":"10","user":{"id":"7","username":"me"},"roles":[],)"
                                            R"("pending":true,"flags":8})"));
    check(pending.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_member_update &&
              (event.state.guild_settings_known & guild_settings_self_member) &&
              event.state.self_member == (guild_self_member_pending | guild_self_member_onboarding_started),
          "gateway: own member updates carry pending and onboarding flags");
    const auto accepted = route(utf8_literal("GUILD_MEMBER_UPDATE"),
                                utf8_literal(R"({"guild_id":"10","user":{"id":"7","username":"me"},"roles":[],)"
                                             R"("pending":false,"flags":10})"));
    check(accepted.ok() && event.state.self_member == (guild_self_member_onboarding_started |
                                                        guild_self_member_onboarding_completed),
          "gateway: accepting the rules clears pending");
    const auto guild_create = route(
        utf8_literal("GUILD_CREATE"),
        utf8_literal(R"({"id":"10","name":"G","owner_id":"1","features":["COMMUNITY","GUILD_ONBOARDING",)"
                     R"("MEMBER_VERIFICATION_GATE_ENABLED"],"roles":[{"id":"10","name":"@everyone","permissions":"0"}],)"
                     R"("members":[{"user":{"id":"7","username":"me"},"roles":[],"pending":true,"flags":1024}],"channels":[]})"));
    check(guild_create.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_create &&
              event.state.upload_features_known &&
              (event.state.upload_features & guild_record_community) &&
              (event.state.upload_features & guild_record_onboarding) &&
              (event.state.upload_features & guild_record_verification_gate) &&
              (event.state.guild_settings_known & guild_settings_self_member) &&
              (event.state.guild_settings_known & guild_settings_self_quarantine) &&
              (event.state.self_member & guild_self_member_quarantined) &&
              (event.state.self_member & guild_self_member_pending),
          "gateway: GUILD_CREATE carries community features and the member's pending state");
    // The state model keeps it, and the gate opens and closes with it.
    static GuildRecord guilds[2];
    static ChannelRecord channels[2];
    static u16 guild_order[2], channel_order[2];
    static u8 names[4096];
    static RoleRecord roles[4];
    static PermissionOverwriteRecord overwrites[2];
    static ChatStateStore store;
    store = {};
    store.self_user_id = 7;
    u64 ids[4]{};
    const bool bound = chat_state_store_initialize(&store, {guilds, 2}, {guild_order, 2}, {channels, 2},
                                                   {channel_order, 2}, {names, sizeof(names)}) &&
                       chat_state_store_bind_permissions(&store, {roles, 4}, {overwrites, 2});
    store.self_user_id = 7;
    const auto applied = chat_state_apply_gateway_event(&store, event, payload, {ids, 4});
    const auto* g = chat_state_store_find_guild(store, 10);
    const bool gated = g && guild_rules_gate(g->upload_features, g->settings.self_member,
                                             (g->settings.known & guild_settings_self_member) != 0);
    route(utf8_literal("GUILD_MEMBER_UPDATE"),
          utf8_literal(R"({"guild_id":"10","user":{"id":"7","username":"me"},"roles":[],"pending":false})"));
    chat_state_apply_gateway_event(&store, event, payload, {ids, 4});
    g = chat_state_store_find_guild(store, 10);
    check(bound && applied.ok() && gated && g &&
              guild_self_quarantined(g->settings) &&
              !guild_rules_gate(g->upload_features, g->settings.self_member, true),
          "gateway: the stored pending state gates the composer until GUILD_MEMBER_UPDATE clears it");
    const auto released = route(
        utf8_literal("GUILD_MEMBER_UPDATE"),
        utf8_literal(R"({"guild_id":"10","user":{"id":"7","username":"me"},"roles":[],"flags":0})"));
    check(released.ok() && chat_state_apply_gateway_event(&store, event, payload, {ids, 4}).ok() &&
              !guild_self_quarantined(g->settings),
          "gateway: own member flags clear previously stored quarantine through the state consumer");

    // A server joined from another client while running: its GUILD_CREATE may
    // lack the member list, list members without roles, or (a preview) leave
    // the current member out with no join time. Each still becomes a guild.
    const auto memberless = route(
        utf8_literal("GUILD_CREATE"),
        utf8_literal(R"({"id":"11","name":"H","owner_id":"1","joined_at":"2026-10-12T00:00:00Z",)"
                     R"("roles":[{"id":"11","name":"@everyone","permissions":"1024"}],"channels":[]})"));
    check(memberless.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_create && event.state.id == 11 &&
              !(event.state.guild_settings_known & guild_settings_self_member),
          "gateway: a live GUILD_CREATE without members still creates the guild");
    const auto partial = route(
        utf8_literal("GUILD_CREATE"),
        utf8_literal(R"({"id":"12","name":"J","owner_id":"1","joined_at":"2026-10-12T00:00:00Z",)"
                     R"("roles":[{"id":"12","name":"@everyone","permissions":"1024"}],)"
                     R"("members":[{"user":{"id":"3","username":"voice"}},)"
                     R"({"user":{"id":"7","username":"me"},"pending":true}],"channels":[]})"));
    check(partial.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_create &&
              event.state.member_role_count == 0 && (event.state.self_member & guild_self_member_pending) &&
              !(event.state.self_member & guild_self_member_lurking),
          "gateway: members without roles are skipped and a pending member keeps its state");
    const auto preview = route(
        utf8_literal("GUILD_CREATE"),
        utf8_literal(R"({"id":"13","name":"K","owner_id":"1","joined_at":null,)"
                     R"("roles":[{"id":"13","name":"@everyone","permissions":"1024"}],)"
                     R"("members":[{"user":{"id":"3","username":"voice"},"roles":[]}],"channels":[]})"));
    const bool preview_routed = preview.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_create &&
                                (event.state.guild_settings_known & guild_settings_self_member) &&
                                (event.state.self_member & guild_self_member_lurking);
    const auto previewed = chat_state_apply_gateway_event(&store, event, payload, {ids, 4});
    const auto* lurked = chat_state_store_find_guild(store, 13);
    check(preview_routed && previewed.ok() && lurked && !lurked->unavailable &&
              (lurked->settings.self_member & guild_self_member_lurking),
          "gateway: a previewed GUILD_CREATE applies as a guild marked previewing");
    route(utf8_literal("GUILD_MEMBER_UPDATE"),
          utf8_literal(R"({"guild_id":"13","user":{"id":"7","username":"me"},"roles":[],"pending":true})"));
    const auto joined = chat_state_apply_gateway_event(&store, event, payload, {ids, 4});
    lurked = chat_state_store_find_guild(store, 13);
    check(joined.ok() && lurked && !(lurked->settings.self_member & guild_self_member_lurking) &&
              guild_rules_gate(0, lurked->settings.self_member, true),
          "gateway: joining the previewed server replaces preview with the pending member state");
    check(guild_rules_gate(0, guild_self_member_pending, true) &&
              !guild_rules_gate(guild_record_onboarding, guild_self_member_pending, true) &&
              guild_rules_gate(guild_record_onboarding | guild_record_verification_gate, guild_self_member_pending,
                               true) &&
              !guild_rules_gate(guild_record_verification_gate, guild_self_member_pending, false),
          "community: rules gate follows the screening and onboarding features");
}

void test_navigation() noexcept {
    static GuildRecord guilds[2];
    static ChannelRecord channels[4];
    static u16 guild_order[2], channel_order[4];
    static u8 names[4096];
    static ChatStateStore state;
    state = {};
    chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 4}, {channel_order, 4},
                                {names, sizeof(names)});
    chat_state_store_upsert_guild(&state, {10, utf8_literal("G"), GuildMutationKind::create, 1});
    ChannelInput input{20, 10, 0, utf8_literal("general"), 0, 0, 0};
    input.name_known = true;
    chat_state_store_upsert_channel(&state, input);
    chat_state_store_select_guild(&state, 10);
    static GuildViewRow guild_rows[2];
    static ChannelViewRow channel_rows[4];
    static u16 channel_scratch[4];
    static u8 visibility[4];
    static ChannelSortKey keys[4];
    static NavigationView view;
    view = {};
    navigation_view_initialize(&view, {guild_rows, 2}, {channel_rows, 4}, {channel_scratch, 4}, {visibility, 4},
                               {keys, 4});
    const auto plain = navigation_view_rebuild(&view, state);
    const u32 without = view.channel_count;
    view.events_row = true;
    view.event_count = 3;
    ++view.events_revision;
    const auto with = navigation_view_rebuild(&view, state);
    check(plain.ok() && with.ok() && with.changed && view.channel_count == without + 1 &&
              view.channel_rows[0].id == kNavigationEventsRowId &&
              view.channel_rows[0].type == kNavigationEventsRowType && !view.channel_rows[0].selectable &&
              utf8_equal({view.events_label, view.events_label_bytes}, utf8_literal("3 Events")) &&
              navigation_view_find_channel(view, 20) == &view.channel_rows[1],
          "navigation: the events row leads the server's channels with its count");
    view.event_count = 1;
    ++view.events_revision;
    navigation_view_rebuild(&view, state);
    const bool one = utf8_equal({view.events_label, view.events_label_bytes}, utf8_literal("1 Event"));
    view.event_count = 0;
    ++view.events_revision;
    navigation_view_rebuild(&view, state);
    const bool none = utf8_equal({view.events_label, view.events_label_bytes}, utf8_literal("Events"));
    view.events_row = false;
    ++view.events_revision;
    navigation_view_rebuild(&view, state);
    check(one && none && view.channel_count == without && view.channel_rows[0].id == 20,
          "navigation: the label counts events and the row leaves when the host hides it");
}

// Every row kind of a server's channel list, with and without the leading events row: the pointer lands on the
// row it is painted on (the layout rectangle of that view row) and the row's id is the channel the menu and the
// selection act on.
void test_navigation_rows() noexcept {
    static GuildRecord guilds[2];
    static ChannelRecord channels[16];
    static u16 guild_order[2], channel_order[16];
    static u8 names[4096];
    static ChatStateStore state;
    state = {};
    chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 16}, {channel_order, 16},
                                {names, sizeof(names)});
    chat_state_store_upsert_guild(&state, {10, utf8_literal("G"), GuildMutationKind::create, 1});
    const auto add = [&](u64 id, u64 parent, u8 type, u32 position, Utf8View name) noexcept {
        ChannelInput input{id, 10, parent, name, position, 0, type};
        input.name_known = true;
        if (type == 11) {
            input.thread_archived_known = true;
            input.thread_active_known = true;
            input.thread_listed_active = true;
            input.thread_self_joined_known = true;
            input.thread_self_joined = true;
        }
        chat_state_store_upsert_channel(&state, input);
    };
    add(100, 0, 4, 0, utf8_literal("JAMOSIS"));
    add(101, 100, 0, 0, utf8_literal("MAIN"));
    add(102, 100, 0, 1, utf8_literal("ARCHIVES"));
    add(103, 101, 11, 0, utf8_literal("THREAD"));
    add(110, 0, 4, 1, utf8_literal("VOICE"));
    add(111, 110, 2, 0, utf8_literal("VC"));
    add(112, 110, 2, 1, utf8_literal("RADIO"));
    add(120, 0, 4, 2, utf8_literal("MODS"));
    add(121, 120, 0, 0, utf8_literal("MODCHAT"));
    chat_state_store_select_guild(&state, 10);
    static GuildViewRow guild_rows[2];
    static ChannelViewRow channel_rows[16];
    static u16 channel_scratch[16];
    static u8 visibility[16];
    static ChannelSortKey keys[16];
    static NavigationView view;
    view = {};
    navigation_view_initialize(&view, {guild_rows, 2}, {channel_rows, 16}, {channel_scratch, 16}, {visibility, 16},
                               {keys, 16});
    static ShellNavigationRowWidget rows[16];
    static UiWidget widgets[128];
    static UiLayoutNode nodes[128];
    static ShellNavigationScene scene;
    scene = {};
    check(shell_navigation_scene_initialize(&scene, {rows, 16}, {widgets, 128}, {nodes, 128}),
          "navigation rows: the scene initializes");
    for (u32 lead = 0; lead < 2; ++lead) {
        view.events_row = lead != 0;
        view.event_count = 2;
        ++view.events_revision;
        const auto built = navigation_view_rebuild(&view, state);
        const auto laid = shell_navigation_scene_update(&scene, &view, {0, 0, 240, 800}, 0);
        bool rows_ok = built.ok() && laid.ok() && scene.count == view.channel_count &&
                       view.channel_count == 9 + lead &&
                       (lead ? view.channel_rows[0].id == kNavigationEventsRowId : view.channel_rows[0].id == 100);
        u32 threads = 0;
        for (u32 i = 0; rows_ok && i < scene.count; ++i) {
            const auto* layout = shell_navigation_scene_layout_node(scene, scene.rows[i]);
            rows_ok = layout && scene.rows[i].source_index == i && scene.rows[i].view.id == view.channel_rows[i].id;
            if (!rows_ok)
                break;
            threads += view.channel_rows[i].type == 11;
            const float x = (layout->rectangle.left + layout->rectangle.right) * .5f;
            const float y = (layout->rectangle.top + layout->rectangle.bottom) * .5f;
            const auto hit = shell_navigation_scene_hit_test(scene, x, y);
            // The row above ends where this one starts: a point just inside either edge is still this row.
            const auto top = shell_navigation_scene_hit_test(scene, x, layout->rectangle.top + 1.0f);
            const auto bottom = shell_navigation_scene_hit_test(scene, x, layout->rectangle.bottom - 1.0f);
            rows_ok = hit.id == view.channel_rows[i].id && hit.source_index == i &&
                      hit.selectable == view.channel_rows[i].selectable && top.id == hit.id &&
                      bottom.id == hit.id &&
                      navigation_view_find_channel(view, hit.id) == &view.channel_rows[i] &&
                      (hit.id == kNavigationEventsRowId || chat_state_store_find_channel(state, hit.id));
        }
        check(rows_ok && threads == 1,
              lead ? "navigation rows: with the events row every row hit resolves to its own channel, "
                     "category, voice channel, thread and the events row itself"
                   : "navigation rows: without the events row every row hit resolves to its own channel, "
                     "category, voice channel and thread");
    }
    // The same holds in a viewport shorter than the list, at every scroll position, with a server banner.
    for (u32 lead = 0; lead < 2; ++lead) {
        view.events_row = lead != 0;
        ++view.events_revision;
        navigation_view_rebuild(&view, state);
        for (u32 banner = 0; banner < 2; ++banner) {
            shell_navigation_scene_set_banner(&scene, banner ? 90.0f : 0.0f, false, banner);
            bool windows_ok = true;
            for (u32 height = 60; height <= 220; height += 80) {
                for (u32 scroll = 0; scroll <= 400; scroll += 23) {
                    const auto laid = shell_navigation_scene_update(&scene, &view, {0, 40, 240, 40.0f + height},
                                                                    static_cast<float>(scroll));
                    windows_ok = windows_ok && laid.ok() && scene.count != 0;
                    for (u32 i = 0; windows_ok && i < scene.count; ++i) {
                        const auto& row = scene.rows[i];
                        const auto* layout = shell_navigation_scene_layout_node(scene, row);
                        windows_ok = layout && row.view.id == view.channel_rows[row.source_index].id &&
                                     ui_rect_height(layout->rectangle) == shell_navigation_row_height(row.view);
                        if (!windows_ok)
                            break;
                        const float y = (layout->rectangle.top + layout->rectangle.bottom) * .5f;
                        if (y < 40.0f || y >= 40.0f + height)
                            continue;
                        const auto hit = shell_navigation_scene_hit_test(scene, 120.0f, y);
                        windows_ok = hit.id == row.view.id && hit.source_index == row.source_index;
                    }
                }
            }
            check(windows_ok, "navigation rows: hits resolve to the painted row in every scrolled window");
        }
    }
}

void run() noexcept {
    test_requests();
    test_verification();
    test_onboarding();
    test_events();
    test_create();
    test_links();
    test_guild_profile();
    test_gateway();
    test_navigation();
    test_navigation_rows();
}
} // namespace maicord::guild_community_test

extern "C" __declspec(noreturn) void WINAPI maicord_guild_community_entry() noexcept {
    maicord::guild_community_test::run();
    ExitProcess(maicord::guild_community_test::failures ? 1 : 0);
}
