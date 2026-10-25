#include "maicord/protocol/guild_community.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/rest_route.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/json_scan.hpp"
#include "json_helpers.hpp"

namespace maicord {
namespace {
constexpr u32 kDepth = 32;

// Decodes a JSON string into output, truncated at a code point boundary.
u32 decode_text(JsonValue value, char* output, u32 capacity) noexcept {
    return protocol_detail::copy_text_truncated<4096>(value, output, capacity);
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
Scan scan(Utf8View body, Slice<u32> scratch) noexcept {
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
    if (json_string_equals_ascii(name, utf8_literal("TERMS")))
        return GuildVerificationFieldType::terms;
    if (json_string_equals_ascii(name, utf8_literal("TEXT_INPUT")))
        return GuildVerificationFieldType::text_input;
    if (json_string_equals_ascii(name, utf8_literal("PARAGRAPH")))
        return GuildVerificationFieldType::paragraph;
    if (json_string_equals_ascii(name, utf8_literal("MULTIPLE_CHOICE")))
        return GuildVerificationFieldType::multiple_choice;
    if (json_string_equals_ascii(name, utf8_literal("VERIFICATION")))
        return GuildVerificationFieldType::verification;
    return GuildVerificationFieldType::unknown;
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
    if (json_string_equals_ascii(name, utf8_literal("field_type")))
        f.type = field_type(value);
    else if (json_string_equals_ascii(name, utf8_literal("label")))
        f.label = p.bank.add(value, 512);
    else if (json_string_equals_ascii(name, utf8_literal("description")))
        f.description = p.bank.add(value, 1024);
    else if (json_string_equals_ascii(name, utf8_literal("placeholder")))
        f.placeholder = p.bank.add(value, 256);
    else if (json_string_equals_ascii(name, utf8_literal("required")))
        json_value_to_bool(value, &f.required);
    else if (json_string_equals_ascii(name, utf8_literal("values")) ||
             json_string_equals_ascii(name, utf8_literal("choices"))) {
        if (value.kind == JsonValueKind::array)
            return json_visit_indexed_array(p.document, value, kDepth, form_value, &p).ok();
        if (value.kind != JsonValueKind::null_value)
            p.form->truncated = true;
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
    if (json_string_equals_ascii(name, utf8_literal("version"))) {
        if (value.kind == JsonValueKind::string)
            p.form->version_bytes =
                static_cast<u8>(decode_text(value, p.form->version, kGuildVerificationVersionBytes));
    } else if (json_string_equals_ascii(name, utf8_literal("description"))) {
        p.form->description = p.bank.add(value, 2048);
    } else if (json_string_equals_ascii(name, utf8_literal("form_fields"))) {
        if (value.kind != JsonValueKind::array)
            return false;
        p.fields = value;
        p.fields_seen = true;
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
    if (json_string_equals_ascii(name, utf8_literal("name")))
        p.option->emoji = p.bank.add(value, 64);
    else if (json_string_equals_ascii(name, utf8_literal("id")) && value.kind != JsonValueKind::null_value)
        json_value_to_snowflake(value, &p.option->emoji_id);
    return true;
}
bool option_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<OnboardingPass*>(opaque);
    auto& o = *p.option;
    u32 count = 0;
    if (json_string_equals_ascii(name, utf8_literal("id")))
        return json_value_to_snowflake(value, &o.id);
    if (json_string_equals_ascii(name, utf8_literal("title")))
        o.title = p.bank.add(value, 512);
    else if (json_string_equals_ascii(name, utf8_literal("description")))
        o.description = p.bank.add(value, 512);
    else if (json_string_equals_ascii(name, utf8_literal("emoji")) && value.kind == JsonValueKind::object)
        return json_visit_indexed_object(p.document, value, kDepth, emoji_member, &p).ok();
    else if (json_string_equals_ascii(name, utf8_literal("role_ids")) && value.kind == JsonValueKind::array) {
        snowflake_string_array(p.document, value, nullptr, 0, &count);
        o.role_count = static_cast<u16>(minimum<u32>(count, 0xFFFF));
    } else if (json_string_equals_ascii(name, utf8_literal("channel_ids")) && value.kind == JsonValueKind::array) {
        snowflake_string_array(p.document, value, nullptr, 0, &count);
        o.channel_count = static_cast<u16>(minimum<u32>(count, 0xFFFF));
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
    u64 number = 0;
    if (json_string_equals_ascii(name, utf8_literal("id")))
        return json_value_to_snowflake(value, &prompt.id);
    if (json_string_equals_ascii(name, utf8_literal("title")))
        prompt.title = p.bank.add(value, 512);
    else if (json_string_equals_ascii(name, utf8_literal("type")) && json_value_to_u64(value, &number))
        prompt.type = static_cast<u8>(minimum<u64>(number, 255));
    else if (json_string_equals_ascii(name, utf8_literal("single_select")))
        json_value_to_bool(value, &prompt.single_select);
    else if (json_string_equals_ascii(name, utf8_literal("required")))
        json_value_to_bool(value, &prompt.required);
    else if (json_string_equals_ascii(name, utf8_literal("in_onboarding")))
        json_value_to_bool(value, &prompt.in_onboarding);
    else if (json_string_equals_ascii(name, utf8_literal("options"))) {
        if (value.kind != JsonValueKind::array) {
            p.onboarding->truncated = true;
            return true;
        }
        prompt.first_option = static_cast<u16>(p.onboarding->option_count);
        prompt.option_count = 0;
        return json_visit_indexed_array(p.document, value, kDepth, prompt_option, &p).ok();
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
    u64 number = 0;
    if (json_string_equals_ascii(name, utf8_literal("prompts"))) {
        if (value.kind != JsonValueKind::array) {
            o.truncated = true;
            return true;
        }
        p.prompts = value;
        p.prompts_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("default_channel_ids"))) {
        u32 count = 0;
        snowflake_string_array(p.document, value, o.default_channels, kGuildOnboardingDefaultChannelMaximum, &count);
        o.default_channel_count = minimum(count, kGuildOnboardingDefaultChannelMaximum);
    } else if (json_string_equals_ascii(name, utf8_literal("responses"))) {
        if (p.responses_seen || value.kind != JsonValueKind::array)
            o.truncated = true;
        p.responses_seen = true;
        p.responses = value;
    } else if (json_string_equals_ascii(name, utf8_literal("enabled"))) {
        json_value_to_bool(value, &o.enabled);
    } else if (json_string_equals_ascii(name, utf8_literal("mode")) && json_value_to_u64(value, &number)) {
        o.mode = static_cast<u8>(minimum<u64>(number, 255));
    }
    return true;
}

// Scheduled events

struct EventPass {
    JsonIndexedDocument document;
    ScheduledEvent* event;
    bool bad;
};
bool timestamp(JsonValue value, u64* output) noexcept {
    JsonString text{};
    if (value.kind == JsonValueKind::null_value) {
        *output = 0;
        return true;
    }
    return json_value_as_string(value, &text) && parse_iso_timestamp(text, output);
}
bool metadata_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<EventPass*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("location")))
        p.event->location_bytes = static_cast<u8>(decode_text(value, p.event->location, kScheduledEventLocationBytes));
    return true;
}
bool event_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<EventPass*>(opaque);
    auto& e = *p.event;
    u64 number = 0;
    if (json_string_equals_ascii(name, utf8_literal("id")))
        p.bad = p.bad || !json_value_to_snowflake(value, &e.id);
    else if (json_string_equals_ascii(name, utf8_literal("guild_id")))
        p.bad = p.bad || !json_value_to_snowflake(value, &e.guild_id);
    else if (json_string_equals_ascii(name, utf8_literal("channel_id"))) {
        if (value.kind != JsonValueKind::null_value)
            json_value_to_snowflake(value, &e.channel_id);
    } else if (json_string_equals_ascii(name, utf8_literal("name")))
        e.name_bytes = static_cast<u16>(decode_text(value, e.name, kScheduledEventNameBytes));
    else if (json_string_equals_ascii(name, utf8_literal("description")))
        e.description_bytes = static_cast<u16>(decode_text(value, e.description, kScheduledEventDescriptionBytes));
    else if (json_string_equals_ascii(name, utf8_literal("image")))
        e.image_bytes = static_cast<u8>(decode_text(value, e.image, kScheduledEventImageBytes));
    else if (json_string_equals_ascii(name, utf8_literal("scheduled_start_time")))
        p.bad = p.bad || !timestamp(value, &e.start_ms);
    else if (json_string_equals_ascii(name, utf8_literal("scheduled_end_time")))
        timestamp(value, &e.end_ms);
    else if (json_string_equals_ascii(name, utf8_literal("status")) && json_value_to_u64(value, &number))
        e.status = number >= 1 && number <= 4 ? static_cast<ScheduledEventStatus>(number) : ScheduledEventStatus::unknown;
    else if (json_string_equals_ascii(name, utf8_literal("entity_type")) && json_value_to_u64(value, &number))
        e.entity = number >= 1 && number <= 3 ? static_cast<ScheduledEventEntity>(number) : ScheduledEventEntity::unknown;
    else if (json_string_equals_ascii(name, utf8_literal("user_count")) && json_value_to_u64(value, &number)) {
        e.user_count = static_cast<u32>(minimum<u64>(number, ~u32{0}));
        e.user_count_known = true;
    } else if (json_string_equals_ascii(name, utf8_literal("entity_metadata")) &&
               value.kind == JsonValueKind::object)
        return json_visit_indexed_object(p.document, value, kDepth, metadata_member, &p).ok();
    return true;
}
bool event_valid(const EventPass& pass) noexcept {
    return !pass.bad && pass.event->id && pass.event->guild_id && pass.event->start_ms;
}
bool decode_event(const JsonIndexedDocument& document, JsonValue value, ScheduledEvent* output) noexcept {
    if (value.kind != JsonValueKind::object)
        return false;
    ScheduledEvent decoded{};
    EventPass pass{document, &decoded, false};
    if (!json_visit_indexed_object(document, value, kDepth, event_member, &pass).ok() || !event_valid(pass))
        return false;
    *output = decoded;
    return true;
}
struct UserPass {
    ScheduledEventUser* user;
};
bool user_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& u = *static_cast<UserPass*>(opaque)->user;
    if (json_string_equals_ascii(name, utf8_literal("guild_scheduled_event_id")))
        json_value_to_snowflake(value, &u.event_id);
    else if (json_string_equals_ascii(name, utf8_literal("guild_id")))
        json_value_to_snowflake(value, &u.guild_id);
    else if (json_string_equals_ascii(name, utf8_literal("user_id")))
        json_value_to_snowflake(value, &u.user_id);
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
    return bytes && protocol_writer_append_byte(&w, '"') &&
           protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(stamp), bytes) &&
           protocol_writer_append_byte(&w, '"');
}
u32 codepoints(Utf8View text, bool* valid) noexcept {
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
    return operation == GuildCommunityOperation::verification_submit ||
           operation == GuildCommunityOperation::onboarding_submit ||
           operation == GuildCommunityOperation::onboarding_update ||
           operation == GuildCommunityOperation::event_create;
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
    if (event != (request.event_id != 0))
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
    RouteWriter route{};
    bool ok = route_writer_initialize(&route, target);
    const auto guild_path = [&]() noexcept {
        return route_append(&route, utf8_literal("/api/v10/guilds/")) &&
               route_append_u64(&route, request.guild_id);
    };
    const auto event_path = [&]() noexcept {
        return guild_path() && route_append(&route, utf8_literal("/scheduled-events/")) &&
               route_append_u64(&route, request.event_id);
    };
    switch (request.operation) {
    case GuildCommunityOperation::verification_get:
        ok = ok && guild_path() && route_append(&route, utf8_literal("/member-verification?with_guild=false"));
        result.method = RestMethod::get;
        result.route_id = kGuildVerificationGetRouteId;
        break;
    case GuildCommunityOperation::verification_submit:
        ok = ok && guild_path() && route_append(&route, utf8_literal("/requests/@me"));
        result.method = RestMethod::put;
        result.route_id = kGuildVerificationSubmitRouteId;
        break;
    case GuildCommunityOperation::onboarding_get:
        ok = ok && guild_path() && route_append(&route, utf8_literal("/onboarding"));
        result.method = RestMethod::get;
        result.route_id = kGuildOnboardingGetRouteId;
        break;
    case GuildCommunityOperation::onboarding_submit:
    case GuildCommunityOperation::onboarding_update:
        ok = ok && guild_path() && route_append(&route, utf8_literal("/onboarding-responses"));
        result.method = request.operation == GuildCommunityOperation::onboarding_submit ? RestMethod::post
                                                                                        : RestMethod::put;
        result.route_id = kGuildOnboardingSubmitRouteId;
        break;
    case GuildCommunityOperation::events_list:
        ok = ok && guild_path() && route_append(&route, utf8_literal("/scheduled-events?with_user_count=true"));
        result.method = RestMethod::get;
        result.route_id = kGuildEventsListRouteId;
        break;
    case GuildCommunityOperation::event_get:
        ok = ok && event_path() && route_append(&route, utf8_literal("?with_user_count=true"));
        result.method = RestMethod::get;
        result.route_id = kGuildEventGetRouteId;
        break;
    case GuildCommunityOperation::event_create:
        ok = ok && guild_path() && route_append(&route, utf8_literal("/scheduled-events"));
        result.method = RestMethod::post;
        result.route_id = kGuildEventCreateRouteId;
        break;
    case GuildCommunityOperation::event_interested:
    case GuildCommunityOperation::event_uninterested:
        ok = ok && event_path() && route_append(&route, utf8_literal("/users/@me"));
        result.method = request.operation == GuildCommunityOperation::event_interested
                            ? RestMethod::put
                            : RestMethod::delete_method;
        result.route_id = kGuildEventInterestRouteId;
        break;
    case GuildCommunityOperation::event_rsvps:
        ok = ok && route_append(&route, utf8_literal("/api/v10/users/@me/scheduled-events?guild_ids=")) &&
             route_append_u64(&route, request.guild_id);
        result.method = RestMethod::get;
        result.route_id = kGuildEventRsvpsRouteId;
        break;
    case GuildCommunityOperation::guild_profile_get:
        ok = ok && guild_path() && route_append(&route, utf8_literal("/profile"));
        result.method = RestMethod::get;
        result.route_id = kGuildProfileGetRouteId;
        break;
    case GuildCommunityOperation::guild_join:
        // Joining a discoverable server from its tag is a lurker join
        // (lurker=true); session_id names the Gateway session that receives the
        // GUILD_CREATE.
        ok = ok && guild_path() && route_append(&route, utf8_literal("/members/@me?lurker=true")) &&
             (!session_id.bytes || (route_append(&route, utf8_literal("&session_id=")) &&
                                    route_append(&route, session_id)));
        result.method = RestMethod::put;
        result.route_id = kGuildJoinRouteId;
        break;
    default:
        ok = false;
        break;
    }
    if (!ok) {
        result.error = GuildCommunityWriteError::output_capacity;
        return result;
    }
    result.target_bytes = route.bytes;
    return result;
}

u32 guild_community_write_interest_body(Slice<u8> body) noexcept {
    ProtocolWriter w{body, 0};
    return protocol_writer_append_literal(&w, "{\"response\":1}") ? w.used : 0;
}

u32 guild_community_write_join_body(Slice<u8> body) noexcept {
    ProtocolWriter w{body, 0};
    return protocol_writer_append_literal(&w, "{}") ? w.used : 0;
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
    ProtocolWriter* writer;
    ConstSlice<GuildVerificationAnswer> answers;
    JsonValue version, fields;
    u32 index;
    bool ok, version_seen, fields_seen;
};
bool submit_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<SubmitPass*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("version"))) {
        p.version = value;
        p.version_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("form_fields")) && value.kind == JsonValueKind::array) {
        p.fields = value;
        p.fields_seen = true;
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
    GuildVerificationForm form{};
    if (!decode_form(s.document, &form) || !guild_verification_supported(form) ||
        !guild_verification_answers_complete(form, answers))
        return 0;
    ProtocolWriter w{output, 0};
    SubmitPass pass{&w, answers, {}, {}, 0, true, false, false};
    if (!json_visit_root_object(form_body, s.document.structurals, kDepth, submit_member, &pass).ok() ||
        !pass.fields_seen)
        return 0;
    bool ok = protocol_writer_append_literal(&w, "{\"version\":");
    if (pass.version_seen && (pass.version.kind == JsonValueKind::string || pass.version.kind == JsonValueKind::number))
        ok = ok && protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(pass.version.source.data),
                                                pass.version.source.bytes);
    else
        ok = ok && protocol_writer_append_literal(&w, "null");
    ok = ok && protocol_writer_append_literal(&w, ",\"form_fields\":[");
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
                 bool ok = (f.index == 0 || protocol_writer_append_byte(f.writer, ',')) &&
                           protocol_writer_append_bytes(f.writer, reinterpret_cast<const u8*>(value.source.data), end);
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
                     ok = ok && (empty || protocol_writer_append_byte(f.writer, ',')) &&
                          protocol_writer_append_literal(f.writer, "\"response\":");
                     switch (type.type) {
                     case GuildVerificationFieldType::terms:
                     case GuildVerificationFieldType::verification:
                         ok = ok && (answer.agreed ? protocol_writer_append_literal(f.writer, "true")
                                                   : protocol_writer_append_literal(f.writer, "false"));
                         break;
                     case GuildVerificationFieldType::text_input:
                     case GuildVerificationFieldType::paragraph:
                         ok = ok && (answer.answered ? protocol_writer_append_json_string(f.writer, answer.text)
                                                     : protocol_writer_append_literal(f.writer, "null"));
                         break;
                     case GuildVerificationFieldType::multiple_choice:
                         ok = ok && (answer.answered ? protocol_writer_append_u64(f.writer, answer.choice)
                                                     : protocol_writer_append_literal(f.writer, "null"));
                         break;
                     default:
                         ok = ok && protocol_writer_append_literal(f.writer, "null");
                         break;
                     }
                 }
                 ok = ok && protocol_writer_append_byte(f.writer, '}');
                 ++f.index;
                 f.ok = f.ok && ok;
                 return ok;
             },
             &fields)
             .ok() &&
         fields.ok;
    ok = ok && protocol_writer_append_literal(&w, "]}");
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
                               out = json_string_equals_ascii(text, utf8_literal("APPROVED"))    ? GuildApplicationStatus::approved
                                     : json_string_equals_ascii(text, utf8_literal("SUBMITTED")) ? GuildApplicationStatus::submitted
                                     : json_string_equals_ascii(text, utf8_literal("REJECTED"))  ? GuildApplicationStatus::rejected
                                     : json_string_equals_ascii(text, utf8_literal("STARTED"))   ? GuildApplicationStatus::started
                                                                                                 : GuildApplicationStatus::unknown;
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
    OnboardingPass local{};
    OnboardingPass* pass = &local;
    pass->document = s.document;
    pass->onboarding = &o;
    pass->bank = {o.text, kGuildOnboardingTextBytes, &o.text_used, &o.truncated};
    if (!json_visit_root_object(body, s.document.structurals, kDepth, onboarding_member, pass).ok())
        return false;
    if (pass->prompts_seen && !json_visit_indexed_array(s.document, pass->prompts, kDepth, onboarding_prompt, pass).ok())
        return false;
    if (pass->responses_seen && pass->responses.kind == JsonValueKind::array &&
        !json_visit_indexed_array(s.document, pass->responses, kDepth, onboarding_response, pass).ok())
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
    bool ok = protocol_writer_append_literal(&w, "{\"onboarding_responses\":[");
    bool first = true;
    for (u32 i = 0; ok && i < onboarding.prompt_count; ++i) {
        const auto& prompt = onboarding.prompts[i];
        if (!editing && !prompt.in_onboarding)
            continue;
        for (u32 o = 0; ok && o < prompt.option_count; ++o) {
            const auto& option = onboarding.options[prompt.first_option + o];
            if (!option.selected)
                continue;
            ok = (first || protocol_writer_append_byte(&w, ',')) &&
                 protocol_writer_append_snowflake(&w, option.id);
            first = false;
        }
    }
    ok = ok && protocol_writer_append_literal(&w, "],\"onboarding_prompts_seen\":{");
    first = true;
    for (u32 i = 0; ok && i < onboarding.prompt_count; ++i) {
        const auto& prompt = onboarding.prompts[i];
        if (!editing && !prompt.in_onboarding)
            continue;
        ok = (first || protocol_writer_append_byte(&w, ',')) &&
             protocol_writer_append_snowflake(&w, prompt.id) && protocol_writer_append_byte(&w, ':') &&
             protocol_writer_append_u64(&w, seen_ms);
        first = false;
    }
    ok = ok && protocol_writer_append_literal(&w, "},\"onboarding_responses_seen\":{");
    first = true;
    for (u32 i = 0; ok && i < onboarding.prompt_count; ++i) {
        const auto& prompt = onboarding.prompts[i];
        if (!editing && !prompt.in_onboarding)
            continue;
        for (u32 o = 0; ok && o < prompt.option_count; ++o) {
            ok = (first || protocol_writer_append_byte(&w, ',')) &&
                 protocol_writer_append_snowflake(&w, onboarding.options[prompt.first_option + o].id) &&
                 protocol_writer_append_byte(&w, ':') && protocol_writer_append_u64(&w, seen_ms);
            first = false;
        }
    }
    ok = ok && protocol_writer_append_literal(&w, "}}");
    return ok ? w.used : 0;
}

// Scheduled event decoding

bool scheduled_event_decode(Utf8View object, Slice<u32> scratch, ScheduledEvent* output) noexcept {
    const auto s = scan(object, scratch);
    if (!s.ok || !output)
        return false;
    ScheduledEvent decoded{};
    EventPass pass{s.document, &decoded, false};
    if (!json_visit_root_object(object, s.document.structurals, kDepth, event_member, &pass).ok() ||
        !event_valid(pass))
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
    bool ok = protocol_writer_append_literal(&w, "{\"name\":") && protocol_writer_append_json_string(&w, create.name) &&
              protocol_writer_append_literal(&w, ",\"description\":") &&
              (create.description.bytes && !blank(create.description)
                   ? protocol_writer_append_json_string(&w, create.description)
                   : protocol_writer_append_literal(&w, "null")) &&
              protocol_writer_append_literal(&w, ",\"privacy_level\":2,\"scheduled_start_time\":") &&
              timestamp_string(w, create.start_unix_seconds) &&
              protocol_writer_append_literal(&w, ",\"scheduled_end_time\":") &&
              (create.end_unix_seconds ? timestamp_string(w, create.end_unix_seconds)
                                       : protocol_writer_append_literal(&w, "null")) &&
              protocol_writer_append_literal(&w, ",\"entity_type\":") &&
              protocol_writer_append_u64(&w, static_cast<u64>(create.entity));
    if (create.entity == ScheduledEventEntity::external)
        ok = ok && protocol_writer_append_literal(&w, ",\"channel_id\":null,\"entity_metadata\":{\"location\":") &&
             protocol_writer_append_json_string(&w, create.location) && protocol_writer_append_byte(&w, '}');
    else
        ok = ok && protocol_writer_append_literal(&w, ",\"channel_id\":") &&
             protocol_writer_append_snowflake(&w, create.channel_id) &&
             protocol_writer_append_literal(&w, ",\"entity_metadata\":null");
    ok = ok && protocol_writer_append_byte(&w, '}');
    return ok ? w.used : 0;
}

u32 scheduled_event_write_link(u64 guild_id, u64 event_id, Slice<char> output) noexcept {
    if (!guild_id || !event_id || !output.data)
        return 0;
    ProtocolWriter w{{reinterpret_cast<u8*>(output.data), output.count}, 0};
    const bool ok = protocol_writer_append_literal(&w, "https://discord.com/events/") &&
                    protocol_writer_append_u64(&w, guild_id) && protocol_writer_append_byte(&w, '/') &&
                    protocol_writer_append_u64(&w, event_id);
    return ok ? w.used : 0;
}

// Server profile

namespace {
struct ProfileParse {
    const JsonIndexedDocument* document;
    GuildProfile* output;
};
// "#rrggbb" (the brand colour form) or a number; ~0u otherwise.
u32 profile_color(JsonValue value) noexcept {
    u64 number = 0;
    if (json_value_to_u64(value, &number))
        return number <= 0xFFFFFF ? static_cast<u32>(number) : ~0u;
    char text[16];
    const u32 bytes = decode_text(value, text, sizeof(text));
    if (bytes != 7 || text[0] != '#')
        return ~0u;
    u32 color = 0;
    for (u32 i = 1; i < 7; ++i) {
        const char c = text[i];
        const u32 digit = c >= '0' && c <= '9'   ? static_cast<u32>(c - '0')
                          : c >= 'a' && c <= 'f' ? static_cast<u32>(c - 'a' + 10)
                          : c >= 'A' && c <= 'F' ? static_cast<u32>(c - 'A' + 10)
                                                 : 16u;
        if (digit > 15)
            return ~0u;
        color = color << 4 | digit;
    }
    return color;
}
bool visit_profile_feature(void* context, u32, JsonValue feature) noexcept {
    struct Known {
        Utf8View name;
        u32 bit;
    };
    static constexpr Known known[]{
        {utf8_literal("DISCOVERABLE"), guild_profile_discoverable},
        {utf8_literal("GUILD_TAGS"), guild_profile_tags},
    };
    JsonString text{};
    if (!json_value_as_string(feature, &text))
        return true;
    for (const auto& k : known)
        if (json_string_equals_ascii(text, k.name))
            *static_cast<u32*>(context) |= k.bit;
    return true;
}
struct ProfileTraitItem {
    GuildProfileTrait trait;
    u64 position;
    bool positioned;
};
bool visit_profile_trait_member(void* context, JsonString key, JsonValue value) noexcept {
    auto& e = *static_cast<ProfileTraitItem*>(context);
    if (json_string_equals_ascii(key, utf8_literal("label")))
        e.trait.label_bytes = static_cast<u8>(decode_text(value, e.trait.label, kGuildProfileTraitBytes));
    else if (json_string_equals_ascii(key, utf8_literal("position")))
        e.positioned = json_value_to_u64(value, &e.position);
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
bool visit_profile(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileParse*>(opaque);
    auto& o = *p.output;
    u64 number = 0;
    if (json_string_equals_ascii(name, utf8_literal("id")))
        json_value_to_snowflake(value, &o.id);
    else if (json_string_equals_ascii(name, utf8_literal("name")))
        o.name_bytes = static_cast<u16>(decode_text(value, o.name, kGuildProfileNameBytes));
    else if (json_string_equals_ascii(name, utf8_literal("description")))
        o.description_bytes = static_cast<u16>(decode_text(value, o.description, kGuildProfileDescriptionBytes));
    else if (json_string_equals_ascii(name, utf8_literal("icon_hash")))
        o.icon_bytes = static_cast<u8>(decode_text(value, o.icon, kGuildProfileHashBytes));
    else if (json_string_equals_ascii(name, utf8_literal("custom_banner_hash")))
        o.banner_bytes = static_cast<u8>(decode_text(value, o.banner, kGuildProfileHashBytes));
    else if (json_string_equals_ascii(name, utf8_literal("badge_hash")))
        o.badge_bytes = static_cast<u8>(decode_text(value, o.badge, kGuildProfileHashBytes));
    else if (json_string_equals_ascii(name, utf8_literal("tag")))
        o.tag_bytes = static_cast<u8>(decode_text(value, o.tag, kGuildProfileTagBytes));
    else if (json_string_equals_ascii(name, utf8_literal("online_count")) && json_value_to_u64(value, &number))
        o.online_count = static_cast<u32>(minimum<u64>(number, 0xFFFFFFFFull));
    else if (json_string_equals_ascii(name, utf8_literal("member_count")) && json_value_to_u64(value, &number))
        o.member_count = static_cast<u32>(minimum<u64>(number, 0xFFFFFFFFull));
    else if (json_string_equals_ascii(name, utf8_literal("visibility")) && json_value_to_u64(value, &number))
        o.visibility = number <= 3 ? static_cast<GuildProfileVisibility>(number) : GuildProfileVisibility::not_specified;
    else if (json_string_equals_ascii(name, utf8_literal("brand_color_primary"))) {
        const u32 color = profile_color(value);
        o.has_brand_color = color != ~0u;
        o.brand_color = o.has_brand_color ? color : 0;
    } else if (json_string_equals_ascii(name, utf8_literal("features")) && value.kind == JsonValueKind::array)
        json_visit_indexed_array(*p.document, value, kDepth, visit_profile_feature, &o.features);
    else if (json_string_equals_ascii(name, utf8_literal("traits")) && value.kind == JsonValueKind::array) {
        // Up to five {label, position}, laid out by position.
        ProfileTraits traits{p.document, {}};
        json_visit_indexed_array(*p.document, value, kDepth, visit_profile_trait, &traits);
        o.trait_count = 0;
        for (const auto& slot : traits.slots)
            if (slot.label_bytes)
                o.traits[o.trait_count++] = slot;
    }
    return true;
}
} // namespace

bool guild_profile_decode(Utf8View body, Slice<u32> scratch, GuildProfile* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    const auto scanned = scan(body, scratch);
    if (!scanned.ok)
        return false;
    ProfileParse parse{&scanned.document, output};
    if (!json_visit_root_object(body, scanned.document.structurals, kDepth, visit_profile, &parse).ok())
        return false;
    return output->id && output->name_bytes;
}

} // namespace maicord
