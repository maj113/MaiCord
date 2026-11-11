#include "maicord/protocol/guild/community.hpp"
#include "maicord/protocol/guild/community_admin.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/protocol/http/rest_route.hpp"
#include "maicord/protocol/guild/admin.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/core/json_scan.hpp"
#include "../encoding/json_helpers.hpp"
#include "../encoding/json_field_lists.hpp"
#include "field_table.hpp"

namespace maicord {
namespace {
using protocol_detail::FieldDef;
using protocol_detail::kFieldNotify;
using protocol_detail::member_index;
using protocol_detail::ObjectReader;
using protocol_detail::read_member;
using protocol_detail::read_object;
constexpr u32 kDepth = 32;
using protocol_detail::kLenientCodes;

// Decodes a JSON string into output, truncated at a code point boundary.
u32 decode_text(const JsonValue& value, char* output, u32 capacity) noexcept {
    u32 copied;
    protocol_detail::clip_text(value, output, capacity, 4096, &copied);
    return copied;
}
// A bounded text bank: decoded strings are appended and addressed by offset.
struct Bank {
    char* data;
    u32 capacity;
    u32* used;
    bool* truncated;
    GuildVerificationText add(JsonValue value, u32 limit = 4096) noexcept {
        if (value.kind == JsonValueKind::null_value)
            return {};
        JsonString source{};
        if (!json_value_as_string(value, &source)) {
            *truncated = true;
            return {};
        }
        const u32 room = capacity - *used;
        const auto decoded =
            json_string_decode_utf8(source, {data + *used, minimum(room, minimum<u32>(limit, 0xFFFF))});
        if (!decoded.ok()) {
            *truncated = true;
            return {};
        }
        const GuildVerificationText text{static_cast<u16>(*used), static_cast<u16>(decoded.bytes)};
        *used += decoded.bytes;
        return text;
    }
};

struct Scan {
    JsonIndexedDocument document;
    bool ok;
};
Scan scan(const Utf8View& body, const Slice<u32>& scratch) noexcept {
    if (!body.data || !body.bytes || body.bytes > kGuildCommunityMaximumResponseBytes || !scratch.data ||
        !scratch.count)
        return {{}, false};
    const auto scanned = json_scan_structurals_avx1(body, scratch);
    if (!scanned.ok())
        return {{}, false};
    return {{body, {scratch.data, scanned.count}}, true};
}

bool snowflake_string_array(const JsonIndexedDocument& document, JsonValue array, u64* output, u32 capacity,
                            u32* count) noexcept {
    struct Pass {
        u64* output;
        u32 capacity, count;
    } pass{output, capacity, 0};
    if (array.kind != JsonValueKind::array)
        return false;
    const bool ok = json_visit_indexed_array(document, array, kDepth,
                                             +[](void* opaque, u32, JsonValue value) noexcept {
                                                 auto& p = *static_cast<Pass*>(opaque);
                                                 u64 id = 0;
                                                 if (!json_value_to_snowflake(value, &id))
                                                     return true;
                                                 if (p.output && p.count < p.capacity)
                                                     p.output[p.count] = id;
                                                 ++p.count;
                                                 return true;
                                             },
                                             &pass)
                        .ok();
    *count = pass.count;
    return ok;
}

// Verification

struct FormPass {
    JsonIndexedDocument document;
    GuildVerificationForm* form;
    Bank bank;
    GuildVerificationField* field;
    JsonValue fields;
    bool fields_seen;
};
GuildVerificationFieldType field_type(JsonValue value) noexcept {
    JsonString name{};
    if (!json_value_as_string(value, &name))
        return GuildVerificationFieldType::unknown;
    switch (member_index<"TERMS", "TEXT_INPUT", "PARAGRAPH", "MULTIPLE_CHOICE", "VERIFICATION">(name)) {
    case 0: return GuildVerificationFieldType::terms;
    case 1: return GuildVerificationFieldType::text_input;
    case 2: return GuildVerificationFieldType::paragraph;
    case 3: return GuildVerificationFieldType::multiple_choice;
    case 4: return GuildVerificationFieldType::verification;
    default: return GuildVerificationFieldType::unknown;
    }
}
bool form_value(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<FormPass*>(opaque);
    if (p.field->value_count >= kGuildVerificationValueMaximum) {
        p.form->truncated = true;
        return true;
    }
    if (value.kind == JsonValueKind::string)
        p.field->values[p.field->value_count++] = p.bank.add(value, 2048);
    else
        p.form->truncated = true;
    return true;
}
bool form_field_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<FormPass*>(opaque);
    auto& f = *p.field;
    switch (member_index<"field_type", "label", "description", "placeholder", "required", "values", "choices">(name)) {
    case 0: f.type = field_type(value); break;
    case 1: f.label = p.bank.add(value, 512); break;
    case 2: f.description = p.bank.add(value, 1024); break;
    case 3: f.placeholder = p.bank.add(value, 256); break;
    case 4: json_value_to_bool(value, &f.required); break;
    case 5:
    case 6:
        if (value.kind == JsonValueKind::array)
            return json_visit_indexed_array(p.document, value, kDepth, form_value, &p).ok();
        if (value.kind != JsonValueKind::null_value)
            p.form->truncated = true;
        break;
    default: break;
    }
    return true;
}
bool form_field(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<FormPass*>(opaque);
    if (value.kind != JsonValueKind::object) {
        p.form->truncated = true;
        return true;
    }
    if (p.form->field_count >= kGuildVerificationFieldMaximum) {
        p.form->truncated = true;
        return true;
    }
    p.field = &p.form->fields[p.form->field_count++];
    if (!json_visit_indexed_object(p.document, value, kDepth, form_field_member, &p).ok())
        return false;
    if (p.field->type == GuildVerificationFieldType::terms && !p.field->value_count)
        p.form->truncated = true;
    return true;
}
bool form_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<FormPass*>(opaque);
    switch (member_index<"description", "form_fields">(name)) {
    case 0: p.form->description = p.bank.add(value, 2048); break;
    case 1:
        if (value.kind != JsonValueKind::array)
            return false;
        p.fields = value;
        p.fields_seen = true;
        break;
    default: break;
    }
    return true;
}
bool decode_form(const JsonIndexedDocument& document, GuildVerificationForm* output) noexcept {
    auto& form = *output;
    reset_value(&form);
    FormPass pass{
        document, &form, {form.text, kGuildVerificationTextBytes, &form.text_used, &form.truncated},
        nullptr,  {},    false};
    if (!json_visit_root_object(document.input, document.structurals, kDepth, form_member, &pass).ok() ||
        !pass.fields_seen)
        return false;
    return json_visit_indexed_array(document, pass.fields, kDepth, form_field, &pass).ok();
}

// Onboarding

struct OnboardingPass {
    JsonIndexedDocument document;
    GuildOnboarding* onboarding;
    Bank bank;
    GuildOnboardingPrompt* prompt;
    GuildOnboardingOption* option;
    JsonValue prompts, responses;
    bool prompts_seen;
    bool responses_seen;
};
bool onboarding_response(void* opaque, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<OnboardingPass*>(opaque);
    auto& o = *p.onboarding;
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id) || index >= 750) {
        o.truncated = true;
        return true;
    }
    bool matched = false;
    for (u32 i = 0; i < o.option_count; ++i) {
        if (o.options[i].id != id)
            continue;
        o.options[i].selected = true;
        matched = true;
    }
    if (!matched)
        o.truncated = true;
    return true;
}
bool emoji_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<OnboardingPass*>(opaque);
    switch (member_index<"name", "id">(name)) {
    case 0: p.option->emoji = p.bank.add(value, 64); break;
    case 1:
        if (value.kind != JsonValueKind::null_value)
            json_value_to_snowflake(value, &p.option->emoji_id);
        break;
    default: break;
    }
    return true;
}
bool option_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<OnboardingPass*>(opaque);
    auto& o = *p.option;
    switch (member_index<"id", "title", "description", "emoji">(name)) {
    case 0: return json_value_to_snowflake(value, &o.id);
    case 1: o.title = p.bank.add(value, 512); break;
    case 2: o.description = p.bank.add(value, 512); break;
    case 3:
        if (value.kind == JsonValueKind::object)
            return json_visit_indexed_object(p.document, value, kDepth, emoji_member, &p).ok();
        break;
    default: break;
    }
    return true;
}
bool prompt_option(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<OnboardingPass*>(opaque);
    auto& o = *p.onboarding;
    if (value.kind != JsonValueKind::object) {
        o.truncated = true;
        return true;
    }
    if (o.option_count >= kGuildOnboardingOptionMaximum) {
        o.truncated = true;
        return true;
    }
    GuildOnboardingOption decoded{};
    p.option = &decoded;
    if (!json_visit_indexed_object(p.document, value, kDepth, option_member, &p).ok() || !decoded.id) {
        o.truncated = true;
        return true;
    }
    o.options[o.option_count++] = decoded;
    ++p.prompt->option_count;
    return true;
}
bool prompt_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<OnboardingPass*>(opaque);
    auto& prompt = *p.prompt;
    switch (member_index<"id", "title", "single_select", "required", "in_onboarding", "options">(name)) {
    case 0: return json_value_to_snowflake(value, &prompt.id);
    case 1: prompt.title = p.bank.add(value, 512); break;
    case 2: json_value_to_bool(value, &prompt.single_select); break;
    case 3: json_value_to_bool(value, &prompt.required); break;
    case 4: json_value_to_bool(value, &prompt.in_onboarding); break;
    case 5:
        if (value.kind != JsonValueKind::array) {
            p.onboarding->truncated = true;
            return true;
        }
        prompt.first_option = static_cast<u16>(p.onboarding->option_count);
        prompt.option_count = 0;
        return json_visit_indexed_array(p.document, value, kDepth, prompt_option, &p).ok();
    default: break;
    }
    return true;
}
bool onboarding_prompt(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<OnboardingPass*>(opaque);
    auto& o = *p.onboarding;
    if (value.kind != JsonValueKind::object) {
        o.truncated = true;
        return true;
    }
    if (o.prompt_count >= kGuildOnboardingPromptMaximum) {
        o.truncated = true;
        return true;
    }
    GuildOnboardingPrompt decoded{};
    decoded.first_option = static_cast<u16>(o.option_count);
    p.prompt = &decoded;
    const u32 options_before = o.option_count;
    if (!json_visit_indexed_object(p.document, value, kDepth, prompt_member, &p).ok() || !decoded.id) {
        o.option_count = options_before; // drop the options of a rejected prompt
        o.truncated = true;
        return true;
    }
    o.prompts[o.prompt_count++] = decoded;
    return true;
}
bool onboarding_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<OnboardingPass*>(opaque);
    auto& o = *p.onboarding;
    switch (member_index<"prompts", "default_channel_ids", "responses", "enabled">(name)) {
    case 0:
        if (value.kind != JsonValueKind::array) {
            o.truncated = true;
            return true;
        }
        p.prompts = value;
        p.prompts_seen = true;
        break;
    case 1: {
        u32 count = 0;
        snowflake_string_array(p.document, value, o.default_channels, kGuildOnboardingDefaultChannelMaximum, &count);
        o.default_channel_count = minimum(count, kGuildOnboardingDefaultChannelMaximum);
        break;
    }
    case 2:
        if (p.responses_seen || value.kind != JsonValueKind::array)
            o.truncated = true;
        p.responses_seen = true;
        p.responses = value;
        break;
    case 3: json_value_to_bool(value, &o.enabled); break;
    default: break;
    }
    return true;
}

// Scheduled events

// An event's entity_metadata may say where it takes place.
constexpr FieldDef kMetadataDefs[]{{"location", MAICORD_FIELD_LABEL(ScheduledEvent, location, location_bytes)}};
MAICORD_FIELD_TABLE(kMetadataSpec, kMetadataDefs, nullptr, kLenientCodes, kDepth, 0, 0, 0, 0, 16);
// The hook reads entity_metadata (3) and, once its count is stored, notes that user_count (11) was read.
bool event_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    if (index == 11) {
        reinterpret_cast<ScheduledEvent*>(r.base)->user_count_known = true;
        return true;
    }
    if (value.kind != JsonValueKind::object)
        return true;
    ObjectReader metadata{{r.document, 0}, &kMetadataSpec, r.base, 0};
    return read_object(metadata, value) == 0;
}
// An id, the guild, the start and the metadata that does not read fail the event; status and entity are cut to 255
// and then to what they name.
constexpr FieldDef kEventDefs[]{
    {"id", MAICORD_FIELD_SNOWFLAKE(ScheduledEvent, id)},
    {"guild_id", MAICORD_FIELD_SNOWFLAKE(ScheduledEvent, guild_id)},
    {"scheduled_start_time", MAICORD_FIELD_TIMESTAMP_OR_NULL(ScheduledEvent, start_ms)},
    {"entity_metadata", MAICORD_FIELD_HOOK},
    {"channel_id", MAICORD_FIELD_SNOWFLAKE(ScheduledEvent, channel_id)},
    {"name", MAICORD_FIELD_LABEL(ScheduledEvent, name, name_bytes)},
    {"description", MAICORD_FIELD_LABEL(ScheduledEvent, description, description_bytes)},
    {"image", MAICORD_FIELD_LABEL(ScheduledEvent, image, image_bytes)},
    {"scheduled_end_time", MAICORD_FIELD_TIMESTAMP_OR_NULL(ScheduledEvent, end_ms)},
    {"status", MAICORD_FIELD_SATURATE(ScheduledEvent, status)},
    {"entity_type", MAICORD_FIELD_SATURATE(ScheduledEvent, entity)},
    {"user_count", MAICORD_ROW(field_saturate, kFieldNotify, ScheduledEvent, user_count)},
};
MAICORD_FIELD_TABLE(kEventSpec, kEventDefs, event_hook, kLenientCodes, kDepth, 1 | 2 | 4 | 8, 0, 0, 0, 16);
bool event_valid(ScheduledEvent& e) noexcept {
    if (static_cast<u8>(e.status) > static_cast<u8>(ScheduledEventStatus::canceled))
        e.status = ScheduledEventStatus::unknown;
    if (static_cast<u8>(e.entity) > static_cast<u8>(ScheduledEventEntity::external))
        e.entity = ScheduledEventEntity::unknown;
    return e.id && e.guild_id && e.start_ms;
}
bool decode_event(const JsonIndexedDocument& document, JsonValue value, ScheduledEvent* output) noexcept {
    if (value.kind != JsonValueKind::object)
        return false;
    ScheduledEvent decoded{};
    ObjectReader r{{&document, 0}, &kEventSpec, reinterpret_cast<u8*>(&decoded), 0};
    if (read_object(r, value) != 0 || !event_valid(decoded))
        return false;
    *output = decoded;
    return true;
}
struct UserPass {
    ScheduledEventUser* user;
};
bool user_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& u = *static_cast<UserPass*>(opaque)->user;
    switch (member_index<"guild_scheduled_event_id", "guild_id", "user_id">(name)) {
    case 0: json_value_to_snowflake(value, &u.event_id); break;
    case 1: json_value_to_snowflake(value, &u.guild_id); break;
    case 2: json_value_to_snowflake(value, &u.user_id); break;
    default: break;
    }
    return true;
}
bool decode_user(const JsonIndexedDocument& document, JsonValue value, ScheduledEventUser* output) noexcept {
    if (value.kind != JsonValueKind::object)
        return false;
    ScheduledEventUser decoded{};
    UserPass pass{&decoded};
    if (!json_visit_indexed_object(document, value, kDepth, user_member, &pass).ok() || !decoded.event_id)
        return false;
    *output = decoded;
    return true;
}

bool timestamp_string(ProtocolWriter& w, u64 unix_seconds) noexcept {
    char stamp[32];
    const u32 bytes = guild_admin_format_timestamp(unix_seconds, {stamp, sizeof(stamp)});
    return bytes && protocol_write(&w, '"', ConstSlice<u8>{reinterpret_cast<const u8*>(stamp), bytes}, '"');
}
u32 codepoints(const Utf8View& text, bool* valid) noexcept {
    const auto checked = utf8_validate(text);
    *valid = checked.valid;
    return checked.codepoints;
}
bool blank(Utf8View text) noexcept {
    for (u32 i = 0; i < text.bytes; ++i)
        if (!ascii_is_space(text.data[i]))
            return false;
    return true;
}
} // namespace

// Requests

bool guild_community_operation_has_body(GuildCommunityOperation operation) noexcept {
    using Op = GuildCommunityOperation;
    return operation == Op::verification_submit || operation == Op::onboarding_submit ||
           operation == Op::onboarding_update || operation == Op::event_create ||
           operation == Op::verification_update || operation == Op::template_create ||
           operation == Op::template_update || operation == Op::discovery_metadata_update ||
           operation == Op::guild_profile_visibility;
}

bool guild_community_operation_large_reply(GuildCommunityOperation operation) noexcept {
    using Op = GuildCommunityOperation;
    // The REST client's large response path takes GET and JSON POST only; a sync or an update (PUT, PATCH) reads
    // its reply within the ordinary 256 KiB.
    return operation == Op::templates_list || operation == Op::template_create;
}

bool guild_community_request_valid(const GuildCommunityRequest& request) noexcept {
    for (const u8 reserved : request.reserved)
        if (reserved)
            return false;
    if (request.operation >= GuildCommunityOperation::count || !request.guild_id)
        return false;
    const auto op = request.operation;
    const bool event = op == GuildCommunityOperation::event_get || op == GuildCommunityOperation::event_interested ||
                       op == GuildCommunityOperation::event_uninterested;
    const bool subcategory = op == GuildCommunityOperation::discovery_subcategory_add ||
                             op == GuildCommunityOperation::discovery_subcategory_remove;
    if (!subcategory && event != (request.event_id != 0))
        return false;
    if (subcategory && request.event_id > 0xFFFFFFFFull)
        return false;
    const bool coded = op == GuildCommunityOperation::template_sync || op == GuildCommunityOperation::template_update ||
                       op == GuildCommunityOperation::template_delete;
    if (coded ? !guild_template_code_valid(request.code) : (request.code.data || request.code.bytes))
        return false;
    if (!guild_community_operation_has_body(op))
        return !request.body && !request.body_bytes;
    if (!request.body || request.body_bytes < 2 || request.body_bytes > kGuildCommunityMaximumBodyBytes)
        return false;
    // A JSON object: the module never forwards anything else.
    u32 first = 0, last = request.body_bytes;
    while (first < last && ascii_is_space(static_cast<char>(request.body[first])))
        ++first;
    while (last > first && ascii_is_space(static_cast<char>(request.body[last - 1])))
        --last;
    return last - first >= 2 && request.body[first] == '{' && request.body[last - 1] == '}';
}

namespace {
// What precedes the suffix: /api/v9/guilds/{g}, that plus /scheduled-events/{e}, or nothing.
enum class CommunityPath : u8 { guild, event, root };
// What the target ends with after the suffix.
enum class CommunityTail : u8 { none, code, event_id, guild_id, session };
struct CommunityRoute {
    u64 route_id;
    RestMethod method;
    CommunityPath path;
    CommunityTail tail;
};
constexpr CommunityRoute community_route(u64 route_id, RestMethod method, CommunityPath path = CommunityPath::guild,
                                         CommunityTail tail = CommunityTail::none) noexcept {
    return {route_id, method, path, tail};
}
// One row per GuildCommunityOperation, in its order, and its suffix in kCommunitySuffixes.
constexpr CommunityRoute kCommunityRoutes[]{
    community_route(kGuildVerificationGetRouteId, RestMethod::get),
    community_route(kGuildVerificationSubmitRouteId, RestMethod::put),
    community_route(kGuildOnboardingGetRouteId, RestMethod::get),
    community_route(kGuildOnboardingSubmitRouteId, RestMethod::post),
    community_route(kGuildEventsListRouteId, RestMethod::get),
    community_route(kGuildEventGetRouteId, RestMethod::get, CommunityPath::event),
    community_route(kGuildEventCreateRouteId, RestMethod::post),
    community_route(kGuildEventInterestRouteId, RestMethod::put, CommunityPath::event),
    community_route(kGuildEventInterestRouteId, RestMethod::delete_method, CommunityPath::event),
    community_route(kGuildEventRsvpsRouteId, RestMethod::get, CommunityPath::root, CommunityTail::guild_id),
    community_route(kGuildOnboardingSubmitRouteId, RestMethod::put),
    community_route(kGuildProfileGetRouteId, RestMethod::get),
    community_route(kGuildJoinRouteId, RestMethod::put, CommunityPath::guild, CommunityTail::session),
    community_route(kGuildVerificationUpdateRouteId, RestMethod::patch),
    community_route(kGuildTemplatesRouteId, RestMethod::get),
    community_route(kGuildTemplatesRouteId, RestMethod::post),
    community_route(kGuildTemplateRouteId, RestMethod::put, CommunityPath::guild, CommunityTail::code),
    community_route(kGuildTemplateRouteId, RestMethod::patch, CommunityPath::guild, CommunityTail::code),
    community_route(kGuildTemplateRouteId, RestMethod::delete_method, CommunityPath::guild, CommunityTail::code),
    community_route(kGuildDiscoveryRequirementsRouteId, RestMethod::get),
    community_route(kGuildDiscoveryMetadataRouteId, RestMethod::get),
    community_route(kGuildDiscoveryMetadataRouteId, RestMethod::patch),
    community_route(kDiscoveryCategoriesRouteId, RestMethod::get, CommunityPath::root),
    community_route(kGuildDiscoverySubcategoryRouteId, RestMethod::put, CommunityPath::guild, CommunityTail::event_id),
    community_route(kGuildDiscoverySubcategoryRouteId, RestMethod::delete_method, CommunityPath::guild,
                    CommunityTail::event_id),
    community_route(kGuildProfileVisibilityRouteId, RestMethod::patch),
    community_route(kGuildJoinRequestsRouteId, RestMethod::get),
};
// Their suffixes. Joining a discoverable server from its tag is a lurker join (lurker=true); session_id names the
// Gateway session that receives the GUILD_CREATE. The official client asks in its own locale for every category.
constexpr char kCommunitySuffixes[] =
    "/member-verification?with_guild=false\0/requests/@me\0/onboarding\0/onboarding-responses\0"
    "/scheduled-events?with_user_count=true\0?with_user_count=true\0/scheduled-events\0/users/@me\0/users/@me\0"
    "/api/v9/users/@me/scheduled-events?guild_ids=\0/onboarding-responses\0/profile\0/members/@me?lurker=true\0"
    "/member-verification\0/templates\0/templates\0/templates/\0/templates/\0/templates/\0/discovery-requirements\0"
    "/discovery-metadata\0/discovery-metadata\0/api/v9/discovery/categories?locale=en-US&primary_only=false\0"
    "/discovery-categories/\0/discovery-categories/\0/profile\0/requests?status=SUBMITTED&limit=25\0";
static_assert(sizeof(kCommunityRoutes) / sizeof(kCommunityRoutes[0]) ==
                  static_cast<usize>(GuildCommunityOperation::count) &&
              name_list_count(kCommunitySuffixes) == static_cast<usize>(GuildCommunityOperation::count));
} // namespace

GuildCommunityWriteResult guild_community_write_target(const GuildCommunityRequest& request,
                                                       Slice<u8> target, Utf8View session_id) noexcept {
    GuildCommunityWriteResult result{};
    if (!guild_community_request_valid(request) || !target.data || (session_id.bytes && !session_id.data) ||
        session_id.bytes > 64) {
        result.error = GuildCommunityWriteError::invalid_input;
        return result;
    }
    for (u32 i = 0; i < session_id.bytes; ++i) {
        const char c = session_id.data[i];
        if (!ascii_is_alnum(c)) {
            result.error = GuildCommunityWriteError::invalid_input;
            return result;
        }
    }
    const auto index = static_cast<u32>(request.operation);
    const auto& r = kCommunityRoutes[index];
    const Utf8View suffix = name_list_at(kCommunitySuffixes, index);
    RouteWriter route{};
    bool ok = route_writer_initialize(&route, target) &&
              (r.path == CommunityPath::root || route_write(&route, "/api/v9/guilds/", request.guild_id)) &&
              (r.path != CommunityPath::event || route_write(&route, "/scheduled-events/", request.event_id)) &&
              route_append_bytes(&route, suffix.data, suffix.bytes);
    switch (r.tail) {
    case CommunityTail::code: ok = ok && route_write(&route, request.code); break;
    case CommunityTail::event_id: ok = ok && route_write(&route, request.event_id); break;
    case CommunityTail::guild_id: ok = ok && route_write(&route, request.guild_id); break;
    case CommunityTail::session:
        ok = ok && (!session_id.bytes || route_write(&route, "&session_id=", session_id));
        break;
    case CommunityTail::none: break;
    }
    result.method = r.method;
    result.route_id = r.route_id;
    if (!ok) {
        result.error = GuildCommunityWriteError::output_capacity;
        return result;
    }
    result.target_bytes = route.bytes;
    return result;
}

u32 guild_community_write_interest_body(Slice<u8> body) noexcept {
    ProtocolWriter w{body, 0};
    return protocol_write(&w, "{\"response\":1}") ? w.used : 0;
}

u32 guild_community_write_join_body(Slice<u8> body) noexcept {
    ProtocolWriter w{body, 0};
    return protocol_write(&w, "{}") ? w.used : 0;
}

u32 guild_community_write_join_context(Slice<u8> output) noexcept {
    // base64("{}"): an empty context object.
    static constexpr u8 kEmptyContext[]{'e', '3', '0', '='};
    const u32 written = rest_append_extra_header(output, 0, utf8_literal("X-Context-Properties"),
                                                 {kEmptyContext, sizeof(kEmptyContext)});
    return written == ~0u ? 0 : written;
}

GuildCommunityOutcome guild_community_classify(u16 status) noexcept {
    if (status >= 200 && status < 300)
        return GuildCommunityOutcome::ok;
    if (status == 404)
        return GuildCommunityOutcome::not_found;
    if (status == 403 || status == 401)
        return GuildCommunityOutcome::forbidden;
    if (status == 429)
        return GuildCommunityOutcome::rate_limited;
    if (status == 400)
        return GuildCommunityOutcome::invalid;
    return GuildCommunityOutcome::unavailable;
}

// Verification decoding

bool guild_verification_decode(Utf8View body, Slice<u32> scratch, GuildVerificationForm* output) noexcept {
    if (!output)
        return false;
    const auto s = scan(body, scratch);
    if (!s.ok)
        return false;
    return decode_form(s.document, output);
}

bool guild_verification_supported(const GuildVerificationForm& form) noexcept {
    if (form.truncated)
        return false;
    for (u32 i = 0; i < form.field_count; ++i)
        if (form.fields[i].required && form.fields[i].type == GuildVerificationFieldType::unknown)
            return false;
    return true;
}

bool guild_verification_answers_complete(const GuildVerificationForm& form,
                                         ConstSlice<GuildVerificationAnswer> answers) noexcept {
    if (answers.count < form.field_count)
        return false;
    for (u32 i = 0; i < form.field_count; ++i) {
        const auto& field = form.fields[i];
        const auto& answer = answers[i];
        if (!field.required)
            continue;
        switch (field.type) {
        case GuildVerificationFieldType::terms:
        case GuildVerificationFieldType::verification:
            if (!answer.agreed)
                return false;
            break;
        case GuildVerificationFieldType::text_input:
        case GuildVerificationFieldType::paragraph:
            if (!answer.answered || blank(answer.text))
                return false;
            break;
        case GuildVerificationFieldType::multiple_choice:
            if (!answer.answered || answer.choice >= field.value_count)
                return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

namespace {
struct SubmitPass {
    JsonValue version, fields;
    bool version_seen, fields_seen;
};
bool submit_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<SubmitPass*>(opaque);
    switch (member_index<"version", "form_fields">(name)) {
    case 0:
        p.version = value;
        p.version_seen = true;
        break;
    case 1:
        if (value.kind == JsonValueKind::array) {
            p.fields = value;
            p.fields_seen = true;
        }
        break;
    default: break;
    }
    return true;
}
struct ResponseScan {
    bool has_response;
};
bool response_key(void* opaque, JsonString name, JsonValue) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("response")))
        static_cast<ResponseScan*>(opaque)->has_response = true;
    return true;
}
} // namespace

u32 guild_verification_write_submit(Utf8View form_body, ConstSlice<GuildVerificationAnswer> answers,
                                    Slice<u32> scratch, Slice<u8> output) noexcept {
    const auto s = scan(form_body, scratch);
    if (!s.ok || !output.data)
        return 0;
    GuildVerificationForm form;
    if (!decode_form(s.document, &form) || !guild_verification_supported(form) ||
        !guild_verification_answers_complete(form, answers))
        return 0;
    ProtocolWriter w{output, 0};
    SubmitPass pass{};
    if (!json_visit_root_object(form_body, s.document.structurals, kDepth, submit_member, &pass).ok() ||
        !pass.fields_seen)
        return 0;
    bool ok = protocol_write(&w, "{\"version\":");
    if (pass.version_seen && (pass.version.kind == JsonValueKind::string || pass.version.kind == JsonValueKind::number))
        ok = ok && protocol_write(&w, pass.version.source);
    else
        ok = ok && protocol_write(&w, "null");
    ok = ok && protocol_write(&w, ",\"form_fields\":[");
    struct FieldPass {
        const JsonIndexedDocument* document;
        ProtocolWriter* writer;
        ConstSlice<GuildVerificationAnswer> answers;
        u32 index;
        bool ok;
    } fields{&s.document, &w, answers, 0, ok};
    ok = ok &&
         json_visit_indexed_array(
             s.document, pass.fields, kDepth,
             +[](void* opaque, u32, JsonValue value) noexcept {
                 auto& f = *static_cast<FieldPass*>(opaque);
                 if (value.kind != JsonValueKind::object || value.source.bytes < 2) {
                     f.ok = false;
                     return false;
                 }
                 ResponseScan existing{};
                 json_visit_indexed_object(*f.document, value, kDepth, response_key, &existing);
                 // The original object minus its closing brace, then the response.
                 u32 end = value.source.bytes - 1;
                 while (end > 1 && ascii_is_space(value.source.data[end - 1]))
                     --end;
                 const bool empty = end == 1;
                 bool ok = (f.index == 0 || protocol_write(f.writer, ',')) &&
                           protocol_write(f.writer, Utf8View{value.source.data, end});
                 const GuildVerificationAnswer answer = f.index < f.answers.count ? f.answers[f.index]
                                                                                   : GuildVerificationAnswer{};
                 // Find this field's type again from the source for the response shape.
                 struct TypePass {
                     GuildVerificationFieldType type;
                 } type{};
                 json_visit_indexed_object(*f.document, value, kDepth,
                                           +[](void* o, JsonString n, JsonValue v) noexcept {
                                               if (json_string_equals_ascii(n, utf8_literal("field_type")))
                                                   static_cast<TypePass*>(o)->type = field_type(v);
                                               return true;
                                           },
                                           &type);
                 if (!existing.has_response) {
                     ok = ok && (empty || protocol_write(f.writer, ',')) && protocol_write(f.writer, "\"response\":");
                     switch (type.type) {
                     case GuildVerificationFieldType::terms:
                     case GuildVerificationFieldType::verification:
                         ok = ok && protocol_write(f.writer, answer.agreed ? "true" : "false");
                         break;
                     case GuildVerificationFieldType::text_input:
                     case GuildVerificationFieldType::paragraph:
                         ok = ok && protocol_write(f.writer, or_null(answer.answered, json_text(answer.text)));
                         break;
                     case GuildVerificationFieldType::multiple_choice:
                         ok = ok && protocol_write(f.writer, or_null(answer.answered, answer.choice));
                         break;
                     default:
                         ok = ok && protocol_write(f.writer, "null");
                         break;
                     }
                 }
                 ok = ok && protocol_write(f.writer, '}');
                 ++f.index;
                 f.ok = f.ok && ok;
                 return ok;
             },
             &fields)
             .ok() &&
         fields.ok;
    ok = ok && protocol_write(&w, "]}");
    return ok ? w.used : 0;
}

GuildApplicationStatus guild_verification_decode_status(Utf8View body, Slice<u32> scratch) noexcept {
    const auto s = scan(body, scratch);
    if (!s.ok)
        return GuildApplicationStatus::unknown;
    GuildApplicationStatus status = GuildApplicationStatus::unknown;
    json_visit_root_object(body, s.document.structurals, kDepth,
                           +[](void* opaque, JsonString name, JsonValue value) noexcept {
                               JsonString text{};
                               if (!json_string_equals_ascii(name, utf8_literal("application_status")) ||
                                   !json_value_as_string(value, &text))
                                   return true;
                               auto& out = *static_cast<GuildApplicationStatus*>(opaque);
                               static constexpr GuildApplicationStatus kStatuses[]{
                                   GuildApplicationStatus::approved, GuildApplicationStatus::submitted,
                                   GuildApplicationStatus::rejected, GuildApplicationStatus::started,
                                   GuildApplicationStatus::unknown};
                               out = kStatuses[member_index<"APPROVED", "SUBMITTED", "REJECTED", "STARTED">(text)];
                               return true;
                           },
                           &status);
    return status;
}

// Onboarding decoding

bool guild_onboarding_decode(Utf8View body, Slice<u32> scratch, GuildOnboarding* output) noexcept {
    if (!output)
        return false;
    const auto s = scan(body, scratch);
    if (!s.ok)
        return false;
    auto& o = *output;
    reset_value(&o);
    OnboardingPass pass{};
    pass.document = s.document;
    pass.onboarding = &o;
    pass.bank = {o.text, kGuildOnboardingTextBytes, &o.text_used, &o.truncated};
    if (!json_visit_root_object(body, s.document.structurals, kDepth, onboarding_member, &pass).ok())
        return false;
    if (pass.prompts_seen && !json_visit_indexed_array(s.document, pass.prompts, kDepth, onboarding_prompt, &pass).ok())
        return false;
    if (pass.responses_seen && pass.responses.kind == JsonValueKind::array &&
        !json_visit_indexed_array(s.document, pass.responses, kDepth, onboarding_response, &pass).ok())
        return false;
    return true;
}

u32 guild_onboarding_flow_prompts(const GuildOnboarding& onboarding, Slice<u16> output) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < onboarding.prompt_count; ++i) {
        const auto& prompt = onboarding.prompts[i];
        if (!prompt.in_onboarding || !prompt.option_count)
            continue;
        if (output.data && count < output.count)
            output[count] = static_cast<u16>(i);
        ++count;
    }
    return count;
}

bool guild_onboarding_toggle(GuildOnboarding* onboarding, u32 prompt_index, u32 option_index) noexcept {
    if (!onboarding || prompt_index >= onboarding->prompt_count)
        return false;
    const auto& prompt = onboarding->prompts[prompt_index];
    if (option_index >= prompt.option_count)
        return false;
    auto* options = onboarding->options + prompt.first_option;
    const bool selected = !options[option_index].selected;
    if (prompt.single_select && selected)
        for (u32 i = 0; i < prompt.option_count; ++i)
            options[i].selected = false;
    options[option_index].selected = selected;
    return true;
}

bool guild_onboarding_prompt_answered(const GuildOnboarding& onboarding, u32 prompt_index) noexcept {
    if (prompt_index >= onboarding.prompt_count)
        return false;
    const auto& prompt = onboarding.prompts[prompt_index];
    for (u32 i = 0; i < prompt.option_count; ++i)
        if (onboarding.options[prompt.first_option + i].selected)
            return true;
    return false;
}

u32 guild_onboarding_write_responses(const GuildOnboarding& onboarding, u64 seen_ms, Slice<u8> output,
                                     bool editing) noexcept {
    if (!output.data || onboarding.truncated)
        return 0;
    ProtocolWriter w{output, 0};
    bool ok = protocol_write(&w, "{\"onboarding_responses\":[");
    bool first = true;
    for (u32 i = 0; ok && i < onboarding.prompt_count; ++i) {
        const auto& prompt = onboarding.prompts[i];
        if (!editing && !prompt.in_onboarding)
            continue;
        for (u32 o = 0; ok && o < prompt.option_count; ++o) {
            const auto& option = onboarding.options[prompt.first_option + o];
            if (!option.selected)
                continue;
            ok = (first || protocol_write(&w, ',')) && protocol_write(&w, snowflake_text(option.id));
            first = false;
        }
    }
    ok = ok && protocol_write(&w, "],\"onboarding_prompts_seen\":{");
    first = true;
    for (u32 i = 0; ok && i < onboarding.prompt_count; ++i) {
        const auto& prompt = onboarding.prompts[i];
        if (!editing && !prompt.in_onboarding)
            continue;
        ok = (first || protocol_write(&w, ',')) && protocol_write(&w, snowflake_text(prompt.id), ':', seen_ms);
        first = false;
    }
    ok = ok && protocol_write(&w, "},\"onboarding_responses_seen\":{");
    first = true;
    for (u32 i = 0; ok && i < onboarding.prompt_count; ++i) {
        const auto& prompt = onboarding.prompts[i];
        if (!editing && !prompt.in_onboarding)
            continue;
        for (u32 o = 0; ok && o < prompt.option_count; ++o) {
            ok = (first || protocol_write(&w, ',')) &&
                 protocol_write(&w, snowflake_text(onboarding.options[prompt.first_option + o].id), ':', seen_ms);
            first = false;
        }
    }
    ok = ok && protocol_write(&w, "}}");
    return ok ? w.used : 0;
}

// Scheduled event decoding

bool scheduled_event_decode(const Utf8View& object, const Slice<u32>& scratch,
                            ScheduledEvent* output) noexcept {
    const auto s = scan(object, scratch);
    if (!s.ok || !output)
        return false;
    ScheduledEvent decoded{};
    ObjectReader r{{&s.document, 0}, &kEventSpec, reinterpret_cast<u8*>(&decoded), 0};
    if (!json_visit_root_object(object, s.document.structurals, kDepth, read_member, &r).ok() ||
        !event_valid(decoded))
        return false;
    *output = decoded;
    return true;
}

u32 scheduled_event_decode_list(Utf8View array, Slice<u32> scratch, ScheduledEventVisitor visitor,
                                void* context) noexcept {
    const auto s = scan(array, scratch);
    if (!s.ok || !visitor)
        return ~0u;
    struct Pass {
        const JsonIndexedDocument* document;
        ScheduledEventVisitor visitor;
        void* context;
        u32 count;
    } pass{&s.document, visitor, context, 0};
    const auto read = json_visit_root_array(
        array, s.document.structurals, kDepth,
        +[](void* opaque, u32, JsonValue value) noexcept {
            auto& p = *static_cast<Pass*>(opaque);
            ScheduledEvent event{};
            if (decode_event(*p.document, value, &event)) {
                ++p.count;
                return p.visitor(p.context, event);
            }
            return true;
        },
        &pass);
    return read.ok() ? pass.count : ~0u;
}

bool scheduled_event_decode_user(Utf8View object, Slice<u32> scratch, ScheduledEventUser* output) noexcept {
    const auto s = scan(object, scratch);
    if (!s.ok || !output)
        return false;
    ScheduledEventUser decoded{};
    UserPass pass{&decoded};
    if (!json_visit_root_object(object, s.document.structurals, kDepth, user_member, &pass).ok() ||
        !decoded.event_id)
        return false;
    *output = decoded;
    return true;
}

u32 scheduled_event_decode_users(Utf8View array, Slice<u32> scratch, ScheduledEventUserVisitor visitor,
                                 void* context) noexcept {
    const auto s = scan(array, scratch);
    if (!s.ok || !visitor)
        return ~0u;
    struct Pass {
        const JsonIndexedDocument* document;
        ScheduledEventUserVisitor visitor;
        void* context;
        u32 count;
    } pass{&s.document, visitor, context, 0};
    const auto read = json_visit_root_array(
        array, s.document.structurals, kDepth,
        +[](void* opaque, u32, JsonValue value) noexcept {
            auto& p = *static_cast<Pass*>(opaque);
            ScheduledEventUser user{};
            if (decode_user(*p.document, value, &user)) {
                ++p.count;
                return p.visitor(p.context, user);
            }
            return true;
        },
        &pass);
    return read.ok() ? pass.count : ~0u;
}

ScheduledEventCreateError scheduled_event_create_validate(const ScheduledEventCreate& create,
                                                          u64 now_unix_seconds) noexcept {
    bool valid = false;
    const u32 name = codepoints(create.name, &valid);
    if (!valid || !name || name > kScheduledEventNameMaximum || blank(create.name))
        return ScheduledEventCreateError::name;
    const u32 description = codepoints(create.description, &valid);
    if (!valid || description > kScheduledEventDescriptionMaximum)
        return ScheduledEventCreateError::description;
    if (create.entity == ScheduledEventEntity::external) {
        const u32 location = codepoints(create.location, &valid);
        if (!valid || !location || location > kScheduledEventLocationMaximum || blank(create.location))
            return ScheduledEventCreateError::location;
    } else if (create.entity == ScheduledEventEntity::stage || create.entity == ScheduledEventEntity::voice) {
        if (!create.channel_id)
            return ScheduledEventCreateError::channel;
    } else {
        return ScheduledEventCreateError::channel;
    }
    if (create.start_unix_seconds <= now_unix_seconds)
        return ScheduledEventCreateError::start;
    if ((create.entity == ScheduledEventEntity::external && !create.end_unix_seconds) ||
        (create.end_unix_seconds && create.end_unix_seconds <= create.start_unix_seconds))
        return ScheduledEventCreateError::end;
    return ScheduledEventCreateError::none;
}

u32 scheduled_event_write_create(const ScheduledEventCreate& create, Slice<u8> output) noexcept {
    if (!output.data || scheduled_event_create_validate(create, 0) != ScheduledEventCreateError::none)
        return 0;
    ProtocolWriter w{output, 0};
    bool ok = protocol_write(&w, "{\"name\":", json_text(create.name), ",\"description\":",
                             or_null(create.description.bytes && !blank(create.description),
                                     json_text(create.description)),
                             ",\"privacy_level\":2,\"scheduled_start_time\":") &&
              timestamp_string(w, create.start_unix_seconds) && protocol_write(&w, ",\"scheduled_end_time\":") &&
              (create.end_unix_seconds ? timestamp_string(w, create.end_unix_seconds) : protocol_write(&w, "null")) &&
              protocol_write(&w, ",\"entity_type\":", static_cast<u64>(create.entity));
    if (create.entity == ScheduledEventEntity::external)
        ok = ok && protocol_write(&w, ",\"channel_id\":null,\"entity_metadata\":{\"location\":",
                                  json_text(create.location), '}');
    else
        ok = ok && protocol_write(&w, ",\"channel_id\":", snowflake_text(create.channel_id),
                                  ",\"entity_metadata\":null");
    ok = ok && protocol_write(&w, '}');
    return ok ? w.used : 0;
}

u32 scheduled_event_write_link(u64 guild_id, u64 event_id, Slice<char> output) noexcept {
    if (!guild_id || !event_id || !output.data)
        return 0;
    ProtocolWriter w{{reinterpret_cast<u8*>(output.data), output.count}, 0};
    const bool ok = protocol_write(&w, "https://discord.com/events/", guild_id, '/', event_id);
    return ok ? w.used : 0;
}

// Server profile

namespace {
struct ProfileActivity;
// The profile read, and the game_activity members kept until the games are known.
struct ProfileReader : ObjectReader {
    ProfileActivity* activity;
};
// "#rrggbb" (the brand colour form) or a number; ~0u otherwise.
u32 profile_color(JsonValue value) noexcept {
    u64 number = 0;
    if (json_value_to_u64(value, &number))
        return number <= 0xFFFFFF ? static_cast<u32>(number) : ~0u;
    char text[16];
    const u32 bytes = decode_text(value, text, sizeof(text));
    u32 color = 0;
    return bytes == 7 && text[0] == '#' && color_hex_parse_rgb24({text, 7}, &color) ? color : ~0u;
}
bool visit_profile_feature(void* context, u32, JsonValue feature) noexcept {
    static constexpr u32 bits[]{guild_profile_discoverable, guild_profile_tags, guild_profile_community,
                                guild_profile_verified, guild_profile_partnered, guild_profile_verification_gate,
                                guild_profile_manual_approval};
    JsonString text{};
    if (!json_value_as_string(feature, &text))
        return true;
    const u32 known = member_index<"DISCOVERABLE", "GUILD_TAGS", "COMMUNITY", "VERIFIED", "PARTNERED",
                                   "MEMBER_VERIFICATION_GATE_ENABLED", "MEMBER_VERIFICATION_MANUAL_APPROVAL">(text);
    if (known < 7)
        *static_cast<u32*>(context) |= bits[known];
    return true;
}
struct ProfileTraitItem {
    GuildProfileTrait trait;
    u64 position;
    bool positioned;
};
bool visit_profile_trait_member(void* context, JsonString key, JsonValue value) noexcept {
    auto& e = *static_cast<ProfileTraitItem*>(context);
    switch (member_index<"label", "position", "emoji_id", "emoji_name", "emoji_animated">(key)) {
    case 0: e.trait.label_bytes = static_cast<u8>(decode_text(value, e.trait.label, kGuildProfileTraitBytes)); break;
    case 1: e.positioned = json_value_to_u64(value, &e.position); break;
    case 2: json_value_to_nullable_snowflake(value, &e.trait.emoji_id); break;
    case 3:
        e.trait.emoji_name_bytes = static_cast<u8>(decode_text(value, e.trait.emoji_name, kGuildProfileEmojiNameBytes));
        break;
    case 4: json_value_to_bool(value, &e.trait.emoji_animated); break;
    default: break;
    }
    return true;
}
struct ProfileTraits {
    const JsonIndexedDocument* document;
    GuildProfileTrait slots[kGuildProfileTraits];
};
bool visit_profile_trait(void* context, u32, JsonValue item) noexcept {
    auto& t = *static_cast<ProfileTraits*>(context);
    if (item.kind != JsonValueKind::object)
        return true;
    ProfileTraitItem entry{};
    json_visit_indexed_object(*t.document, item, kDepth, visit_profile_trait_member, &entry);
    if (entry.positioned && entry.position < kGuildProfileTraits && entry.trait.label_bytes)
        t.slots[entry.position] = entry.trait;
    return true;
}
bool visit_profile_game_id(void* context, u32, JsonValue item) noexcept {
    auto& o = *static_cast<GuildProfile*>(context);
    u64 id = 0;
    if (o.game_count < kGuildProfileGames && json_value_to_snowflake(item, &id) && id)
        o.games[o.game_count++] = {id, 0, 0};
    return true;
}
// game_activity: {"<application id>": {activity_level, activity_score}}, kept until the ids are known.
struct ProfileActivity {
    const JsonIndexedDocument* document;
    GuildProfileGame entries[kGuildProfileGames];
    u32 count;
};
bool visit_profile_activity_member(void* context, JsonString key, JsonValue value) noexcept {
    auto& game = *static_cast<GuildProfileGame*>(context);
    u64 number = 0;
    switch (member_index<"activity_level", "activity_score">(key)) {
    case 0:
        if (json_value_to_u64(value, &number))
            game.level = static_cast<u8>(minimum<u64>(number, 255));
        break;
    case 1:
        if (json_value_to_u64(value, &number))
            game.score = static_cast<u32>(minimum<u64>(number, 0xFFFFFFFFull));
        break;
    default: break;
    }
    return true;
}
bool visit_profile_activity(void* context, JsonString key, JsonValue value) noexcept {
    auto& a = *static_cast<ProfileActivity*>(context);
    GuildProfileGame game{};
    if (a.count >= kGuildProfileGames || value.kind != JsonValueKind::object || !decimal_parse(key.encoded, &game.id))
        return true;
    json_visit_indexed_object(*a.document, value, kDepth, visit_profile_activity_member, &game);
    a.entries[a.count++] = game;
    return true;
}
// The members that need more than a copy: the brand colour, the features, the traits, the games and their activity.
bool profile_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    auto& o = *reinterpret_cast<GuildProfile*>(r.base);
    const bool array = value.kind == JsonValueKind::array;
    switch (index) {
    case 9: {
        const u32 color = profile_color(value);
        o.has_brand_color = color != ~0u;
        o.brand_color = o.has_brand_color ? color : 0;
        break;
    }
    case 10:
        if (array)
            json_visit_indexed_array(*r.document, value, kDepth, visit_profile_feature, &o.features);
        break;
    case 11:
        if (array) {
            // Up to five {label, position}, laid out by position.
            ProfileTraits traits{r.document, {}};
            json_visit_indexed_array(*r.document, value, kDepth, visit_profile_trait, &traits);
            o.trait_count = 0;
            for (const auto& slot : traits.slots)
                if (slot.label_bytes)
                    o.traits[o.trait_count++] = slot;
        }
        break;
    case 14:
        o.game_count = 0;
        if (array)
            json_visit_indexed_array(*r.document, value, kDepth, visit_profile_game_id, &o);
        break;
    default: {
        auto* activity = static_cast<ProfileReader&>(r).activity;
        if (value.kind == JsonValueKind::object) {
            activity->count = 0;
            json_visit_indexed_object(*r.document, value, kDepth, visit_profile_activity, activity);
        }
        break;
    }
    }
    return true;
}
// A member that does not fit is passed over; text reads up to 4096 bytes.
constexpr FieldDef kProfileDefs[]{
    {"id", MAICORD_FIELD_SNOWFLAKE(GuildProfile, id)},
    {"name", MAICORD_FIELD_LABEL(GuildProfile, name, name_bytes)},
    {"description", MAICORD_FIELD_LABEL(GuildProfile, description, description_bytes)},
    {"icon_hash", MAICORD_FIELD_LABEL(GuildProfile, icon, icon_bytes)},
    {"custom_banner_hash", MAICORD_FIELD_LABEL(GuildProfile, banner, banner_bytes)},
    {"badge_hash", MAICORD_FIELD_LABEL(GuildProfile, badge, badge_bytes)},
    {"tag", MAICORD_FIELD_LABEL(GuildProfile, tag, tag_bytes)},
    {"online_count", MAICORD_FIELD_SATURATE(GuildProfile, online_count)},
    {"member_count", MAICORD_FIELD_SATURATE(GuildProfile, member_count)},
    {"brand_color_primary", MAICORD_FIELD_HOOK},
    {"features", MAICORD_FIELD_HOOK},
    {"traits", MAICORD_FIELD_HOOK},
    {"premium_subscription_count", MAICORD_FIELD_SATURATE(GuildProfile, premium_subscription_count)},
    {"premium_tier", MAICORD_FIELD_SATURATE(GuildProfile, premium_tier)},
    {"game_application_ids", MAICORD_FIELD_HOOK},
    {"game_activity", MAICORD_FIELD_HOOK},
};
MAICORD_FIELD_TABLE(kProfileSpec, kProfileDefs, profile_hook, kLenientCodes, kDepth, 0, 0, 0, 0, 16);
} // namespace

bool guild_profile_decode(Utf8View body, Slice<u32> scratch, GuildProfile* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    const auto scanned = scan(body, scratch);
    if (!scanned.ok)
        return false;
    ProfileActivity activity{&scanned.document, {}, 0};
    ProfileReader reader{{{&scanned.document, 0}, &kProfileSpec, reinterpret_cast<u8*>(output), 0}, &activity};
    const bool read = json_visit_root_object(body, scanned.document.structurals, kDepth, read_member, &reader).ok();
    // A boost level is 0-3.
    output->premium_tier = minimum<u8>(output->premium_tier, 3);
    if (!read)
        return false;
    // The games by activity score, highest first; a stable insertion sort keeps equal scores in their order.
    auto& games = output->games;
    for (u32 i = 0; i < output->game_count; ++i)
        for (u32 k = 0; k < activity.count; ++k)
            if (activity.entries[k].id == games[i].id) {
                games[i].score = activity.entries[k].score;
                games[i].level = activity.entries[k].level;
            }
    for (u32 i = 1; i < output->game_count; ++i) {
        const GuildProfileGame game = games[i];
        u32 at = i;
        for (; at > 0 && games[at - 1].score < game.score; --at)
            games[at] = games[at - 1];
        games[at] = game;
    }
    return output->id && output->name_bytes;
}

} // namespace maicord
