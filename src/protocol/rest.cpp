#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/client_user_agent.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/gateway_commands.hpp"
#include "json_command_writer.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

using Writer = protocol_detail::JsonCommandWriter;

inline constexpr char kApiHost[] = "discord.com";

struct RateHeadContext {
    RateLimitHeaders rate;
    RestRepresentation representation;
    RestHeadError error;
    u32 error_offset;
    bool has_content_type;
    bool has_content_encoding;
};

struct RateBodyContext {
    u64 retry_after_ms;
    u32 fields;
    u32 error_offset;
    RateLimitBodyError error;
    bool global;
    bool retry_only;
};

constexpr u32 rate_body_field_retry_after = 1u << 0;
constexpr u32 rate_body_field_global = 1u << 1;
constexpr u32 rate_body_field_code = 1u << 2;

#include "rest_metadata.inc"

bool valid_user_headers(const RestUserRequestHeaders& headers) noexcept {
    return valid_base64_metadata(headers.super_properties) &&
           ascii_graphic_string(headers.locale, kRestUserLocaleMaximumBytes, true) &&
           ascii_graphic_string(headers.timezone, kRestUserTimezoneMaximumBytes) &&
           ascii_graphic_string(headers.accept_language, kRestUserAcceptLanguageMaximumBytes);
}

const char* method_literal(RestMethod method) noexcept {
    switch (method) {
    case RestMethod::get:
        return "GET";
    case RestMethod::post:
        return "POST";
    case RestMethod::put:
        return "PUT";
    case RestMethod::patch:
        return "PATCH";
    case RestMethod::delete_method:
        return "DELETE";
    case RestMethod::head:
        return "HEAD";
    default:
        return nullptr;
    }
}

u32 literal_bytes(const char* value) noexcept {
    u32 count = 0;
    while (value[count] != 0) {
        ++count;
    }
    return count;
}

bool valid_target(ConstSlice<u8> target) noexcept {
    if (!target.data || target.count == 0 || target[0] != '/' || (target.count > 1 && target[1] == '/')) {
        return false;
    }
    for (u32 index = 0; index < target.count; ++index) {
        const u8 value = target[index];
        if (value < 0x21 || value > 0x7E || value == '#') {
            return false;
        }
    }
    return true;
}

bool valid_token(ConstSlice<u8> token) noexcept {
    if (!token.data || token.count == 0) {
        return false;
    }
    for (u32 index = 0; index < token.count; ++index) {
        if (token[index] < 0x21 || token[index] > 0x7E) {
            return false;
        }
    }
    return true;
}

bool parse_u32(ConstSlice<u8> value, u32* output) noexcept {
    if (!value.data || value.count == 0) {
        return false;
    }
    u32 result = 0;
    for (u32 index = 0; index < value.count; ++index) {
        const u8 digit = value[index];
        if (digit < '0' || digit > '9' || result > (~0u - static_cast<u32>(digit - '0')) / 10u) {
            return false;
        }
        result = result * 10u + static_cast<u32>(digit - '0');
    }
    *output = result;
    return true;
}

bool parse_decimal_milliseconds(ConstSlice<u8> value, u64* output) noexcept {
    if (!value.data || value.count == 0) {
        return false;
    }
    u32 offset = 0;
    u64 seconds = 0;
    while (offset < value.count && value[offset] >= '0' && value[offset] <= '9') {
        const u64 digit = static_cast<u64>(value[offset] - '0');
        if (seconds > (~0ull - digit) / 10ull) {
            return false;
        }
        seconds = seconds * 10ull + digit;
        ++offset;
    }
    if (offset == 0) {
        return false;
    }
    u64 fractional_ms = 0;
    u32 fractional_digits = 0;
    bool round_up = false;
    if (offset < value.count && value[offset] == '.') {
        ++offset;
        const u32 fraction_begin = offset;
        while (offset < value.count && value[offset] >= '0' && value[offset] <= '9') {
            const u8 digit = static_cast<u8>(value[offset] - '0');
            if (fractional_digits < 3) {
                fractional_ms = fractional_ms * 10ull + digit;
            } else if (digit != 0) {
                round_up = true;
            }
            ++fractional_digits;
            ++offset;
        }
        if (offset == fraction_begin) {
            return false;
        }
    }
    if (offset != value.count) {
        return false;
    }
    while (fractional_digits < 3) {
        fractional_ms *= 10ull;
        ++fractional_digits;
    }
    if (seconds > (~0ull - fractional_ms - (round_up ? 1ull : 0ull)) / 1000ull) {
        return false;
    }
    *output = seconds * 1000ull + fractional_ms + (round_up ? 1ull : 0ull);
    return true;
}

bool parse_bool_ascii(ConstSlice<u8> value, bool* output) noexcept {
    if (http_ascii_equal_case_insensitive(value, "true")) {
        *output = true;
        return true;
    }
    if (http_ascii_equal_case_insensitive(value, "false")) {
        *output = false;
        return true;
    }
    return false;
}

bool begin_rate_field(RateHeadContext* context, u32 field, u32 offset) noexcept {
    if ((context->rate.fields & field) != 0) {
        context->error = RestHeadError::duplicate_rate_field;
        context->error_offset = offset;
        return false;
    }
    context->rate.fields |= field;
    return true;
}

RestContentType classify_content_type(ConstSlice<u8> value) noexcept {
    u32 media_bytes = 0;
    while (media_bytes < value.count && value[media_bytes] != ';') {
        ++media_bytes;
    }
    const ConstSlice<u8> media = http_trim_ows(ConstSlice<u8>{value.data, media_bytes});
    if (http_ascii_equal_case_insensitive(media, "application/json") ||
        http_ascii_equal_case_insensitive(media, "text/json") ||
        (media.count >= 5 &&
         http_ascii_equal_case_insensitive(ConstSlice<u8>{media.data + media.count - 5, 5}, "+json"))) {
        return RestContentType::json;
    }
    if (http_ascii_equal_case_insensitive(media, "text/html") ||
        http_ascii_equal_case_insensitive(media, "application/xhtml+xml")) {
        return RestContentType::html;
    }
    return media.count >= 5 && http_ascii_equal_case_insensitive(ConstSlice<u8>{media.data, 5}, "text/")
               ? RestContentType::text
               : RestContentType::other;
}

RestContentEncoding classify_content_encoding(ConstSlice<u8> value) noexcept {
    const ConstSlice<u8> encoding = http_trim_ows(value);
    if (http_ascii_equal_case_insensitive(encoding, "identity")) {
        return RestContentEncoding::identity;
    }
    if (http_ascii_equal_case_insensitive(encoding, "gzip")) {
        return RestContentEncoding::gzip;
    }
    if (http_ascii_equal_case_insensitive(encoding, "deflate")) {
        return RestContentEncoding::deflate;
    }
    if (http_ascii_equal_case_insensitive(encoding, "br")) {
        return RestContentEncoding::brotli;
    }
    if (http_ascii_equal_case_insensitive(encoding, "zstd")) {
        return RestContentEncoding::zstd;
    }
    return RestContentEncoding::other;
}

bool rate_head_failure(RateHeadContext* context, RestHeadError error, u32 offset) noexcept {
    context->error = error;
    context->error_offset = offset;
    return false;
}

// A rate-limit field whose value parsed (or failed to) as `parsed`.
bool rate_value(RateHeadContext* context, u32 field, u32 offset, bool parsed) noexcept {
    return begin_rate_field(context, field, offset) &&
           (parsed || rate_head_failure(context, RestHeadError::invalid_rate_value, offset));
}

bool visit_rate_header(void* opaque, HttpField field) noexcept {
    auto* context = static_cast<RateHeadContext*>(opaque);
    if (http_ascii_equal_case_insensitive(field.name, "content-type")) {
        context->representation.content_type =
            context->has_content_type ? RestContentType::other : classify_content_type(field.value);
        context->has_content_type = true;
        return true;
    }
    if (http_ascii_equal_case_insensitive(field.name, "content-encoding")) {
        context->representation.content_encoding = context->has_content_encoding
                                                       ? RestContentEncoding::other
                                                       : classify_content_encoding(field.value);
        context->has_content_encoding = true;
        return true;
    }
    if (http_ascii_equal_case_insensitive(field.name, "x-ratelimit-limit")) {
        return rate_value(context, rate_limit_field_limit, field.source_offset,
                          parse_u32(field.value, &context->rate.limit));
    }
    if (http_ascii_equal_case_insensitive(field.name, "x-ratelimit-remaining")) {
        return rate_value(context, rate_limit_field_remaining, field.source_offset,
                          parse_u32(field.value, &context->rate.remaining));
    }
    if (http_ascii_equal_case_insensitive(field.name, "x-ratelimit-reset")) {
        return rate_value(context, rate_limit_field_reset, field.source_offset,
                          parse_decimal_milliseconds(field.value, &context->rate.reset_epoch_ms));
    }
    if (http_ascii_equal_case_insensitive(field.name, "x-ratelimit-reset-after")) {
        return rate_value(context, rate_limit_field_reset_after, field.source_offset,
                          parse_decimal_milliseconds(field.value, &context->rate.reset_after_ms));
    }
    if (http_ascii_equal_case_insensitive(field.name, "retry-after")) {
        return rate_value(context, rate_limit_field_retry_after, field.source_offset,
                          parse_decimal_milliseconds(field.value, &context->rate.retry_after_ms));
    }
    if (http_ascii_equal_case_insensitive(field.name, "x-ratelimit-bucket")) {
        if (!begin_rate_field(context, rate_limit_field_bucket, field.source_offset)) {
            return false;
        }
        if (!field.value.data || field.value.count == 0 ||
            field.value.count > kRateLimitBucketMaximumBytes) {
            return rate_head_failure(context, RestHeadError::bucket_capacity, field.source_offset);
        }
        for (u32 index = 0; index < field.value.count; ++index) {
            if (field.value[index] < 0x21 || field.value[index] > 0x7E) {
                return rate_head_failure(context, RestHeadError::invalid_rate_value,
                                         field.source_offset);
            }
            context->rate.bucket[index] = field.value[index];
        }
        context->rate.bucket_bytes = static_cast<u8>(field.value.count);
        return true;
    }
    if (http_ascii_equal_case_insensitive(field.name, "x-ratelimit-global")) {
        return rate_value(context, rate_limit_field_global, field.source_offset,
                          parse_bool_ascii(field.value, &context->rate.global));
    }
    if (http_ascii_equal_case_insensitive(field.name, "x-ratelimit-scope")) {
        if (!begin_rate_field(context, rate_limit_field_scope, field.source_offset)) {
            return false;
        }
        if (http_ascii_equal_case_insensitive(field.value, "user")) {
            context->rate.scope = RateLimitScope::user;
        } else if (http_ascii_equal_case_insensitive(field.value, "global")) {
            context->rate.scope = RateLimitScope::global;
        } else if (http_ascii_equal_case_insensitive(field.value, "shared")) {
            context->rate.scope = RateLimitScope::shared;
        } else {
            return rate_head_failure(context, RestHeadError::invalid_rate_value,
                                     field.source_offset);
        }
    }
    return true;
}

bool visit_rate_body(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<RateBodyContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("retry_after"))) {
        if ((context->fields & rate_body_field_retry_after) != 0) {
            context->error = RateLimitBodyError::duplicate_field;
            context->error_offset = name.source_offset;
            return false;
        }
        context->fields |= rate_body_field_retry_after;
        if (value.kind != JsonValueKind::number ||
            !parse_decimal_milliseconds(
                ConstSlice<u8>{reinterpret_cast<const u8*>(value.source.data), value.source.bytes},
                &context->retry_after_ms)) {
            context->error = RateLimitBodyError::invalid_retry_after;
            context->error_offset = value.source_offset;
            return false;
        }
    } else if (context->retry_only && json_string_equals_ascii(name, utf8_literal("code"))) {
        if ((context->fields & rate_body_field_code) != 0) {
            context->error = RateLimitBodyError::duplicate_field;
            context->error_offset = name.source_offset;
            return false;
        }
        context->fields |= rate_body_field_code;
        u64 code = 0;
        if (!json_value_to_u64(value, &code) || code != 110000) {
            context->error = RateLimitBodyError::invalid_code;
            context->error_offset = value.source_offset;
            return false;
        }
    } else if (!context->retry_only && json_string_equals_ascii(name, utf8_literal("global"))) {
        if ((context->fields & rate_body_field_global) != 0) {
            context->error = RateLimitBodyError::duplicate_field;
            context->error_offset = name.source_offset;
            return false;
        }
        context->fields |= rate_body_field_global;
        if (!json_value_to_bool(value, &context->global)) {
            context->error = RateLimitBodyError::invalid_global;
            context->error_offset = value.source_offset;
            return false;
        }
    }
    return true;
}

constexpr u16 invalid_rate_index = 0xFFFF;

bool valid_route(const RestRouteKey& route) noexcept {
    if (route.route_id == 0 || !method_literal(route.method)) {
        return false;
    }
    switch (route.major) {
    case RestMajorParameter::none:
        return route.major_key == 0;
    case RestMajorParameter::channel:
    case RestMajorParameter::guild:
    case RestMajorParameter::webhook:
        return route.major_key != 0;
    default:
        return false;
    }
}

bool same_route(const RestRouteKey& left, const RestRouteKey& right) noexcept {
    return left.route_id == right.route_id && left.major_key == right.major_key &&
           left.method == right.method && left.major == right.major;
}

bool same_hash(const RestRateBucket& bucket, const RateLimitHeaders& rate,
               const RestRouteKey& route) noexcept {
    if (!bucket.used || !bucket.canonical || bucket.hash_bytes != rate.bucket_bytes ||
        bucket.major != route.major || bucket.major_key != route.major_key) {
        return false;
    }
    for (u32 index = 0; index < rate.bucket_bytes; ++index) {
        if (bucket.hash[index] != rate.bucket[index]) {
            return false;
        }
    }
    return true;
}

bool same_provisional(const RestRateBucket& bucket, const RestRouteKey& route) noexcept {
    return bucket.used && !bucket.canonical && bucket.route_id == route.route_id &&
           bucket.major_key == route.major_key && bucket.method == route.method &&
           bucket.major == route.major;
}

u16 find_binding(const RestRateScheduler& scheduler, const RestRouteKey& route) noexcept {
    for (u32 index = 0; index < scheduler.bindings.count; ++index) {
        if (scheduler.bindings[index].used && same_route(scheduler.bindings[index].route, route)) {
            return static_cast<u16>(index);
        }
    }
    return invalid_rate_index;
}

u16 acquire_binding_slot(RestRateScheduler* scheduler, const RestRouteKey& route,
                         u64 now_ms) noexcept {
    u16 index = find_binding(*scheduler, route);
    if (index != invalid_rate_index) {
        scheduler->bindings[index].last_used_ms = now_ms;
        return index;
    }
    u16 candidate = invalid_rate_index;
    u64 oldest = ~0ull;
    for (u32 current = 0; current < scheduler->bindings.count; ++current) {
        const RestRateBinding& binding = scheduler->bindings[current];
        if (!binding.used) {
            candidate = static_cast<u16>(current);
            break;
        }
        if (!binding.in_flight && binding.last_used_ms < oldest) {
            oldest = binding.last_used_ms;
            candidate = static_cast<u16>(current);
        }
    }
    if (candidate == invalid_rate_index) {
        return candidate;
    }
    RestRateBinding& binding = scheduler->bindings[candidate];
    const u16 generation = next_generation(binding.generation);
    reset_value<RestRateBinding>(&binding);
    binding.route = route;
    binding.last_used_ms = now_ms;
    binding.bucket_index = invalid_rate_index;
    binding.generation = generation;
    binding.used = true;
    return candidate;
}

RestRateBucket* resolve_bucket(RestRateScheduler* scheduler, u16 index,
                               u16 generation) noexcept {
    if (index == invalid_rate_index || index >= scheduler->buckets.count) {
        return nullptr;
    }
    RestRateBucket& bucket = scheduler->buckets[index];
    return bucket.used && bucket.generation == generation ? &bucket : nullptr;
}

u16 find_bucket(const RestRateScheduler& scheduler, const RestRouteKey& route,
                const RateLimitHeaders* rate) noexcept {
    for (u32 index = 0; index < scheduler.buckets.count; ++index) {
        const RestRateBucket& bucket = scheduler.buckets[index];
        if (rate ? same_hash(bucket, *rate, route) : same_provisional(bucket, route)) {
            return static_cast<u16>(index);
        }
    }
    return invalid_rate_index;
}

u16 acquire_bucket_slot(RestRateScheduler* scheduler, const RestRouteKey& route,
                        const RateLimitHeaders* rate, u64 now_ms) noexcept {
    u16 index = find_bucket(*scheduler, route, rate);
    if (index != invalid_rate_index) {
        scheduler->buckets[index].last_used_ms = now_ms;
        return index;
    }
    u16 candidate = invalid_rate_index;
    u64 oldest = ~0ull;
    for (u32 current = 0; current < scheduler->buckets.count; ++current) {
        const RestRateBucket& bucket = scheduler->buckets[current];
        if (!bucket.used) {
            candidate = static_cast<u16>(current);
            break;
        }
        if (bucket.in_flight == 0 && bucket.last_used_ms < oldest) {
            oldest = bucket.last_used_ms;
            candidate = static_cast<u16>(current);
        }
    }
    if (candidate == invalid_rate_index) {
        return candidate;
    }
    RestRateBucket& bucket = scheduler->buckets[candidate];
    const u16 generation = next_generation(bucket.generation);
    reset_value<RestRateBucket>(&bucket);
    bucket.route_id = route.route_id;
    bucket.major_key = route.major_key;
    bucket.last_used_ms = now_ms;
    bucket.generation = generation;
    bucket.method = route.method;
    bucket.major = route.major;
    bucket.used = true;
    bucket.canonical = rate != nullptr;
    if (rate) {
        bucket.hash_bytes = rate->bucket_bytes;
        for (u32 hash_index = 0; hash_index < rate->bucket_bytes; ++hash_index) {
            bucket.hash[hash_index] = rate->bucket[hash_index];
        }
    }
    return candidate;
}

void release_lease_bucket(RestRateScheduler* scheduler, const RestRateLease& lease) noexcept {
    RestRateBucket* bucket = resolve_bucket(scheduler, lease.bucket_index, lease.bucket_generation);
    if (bucket && bucket->in_flight != 0) {
        --bucket->in_flight;
    }
}

RestRateObservation observation_error(RestRateError error) noexcept {
    return RestRateObservation{0, error, false, false};
}

} // namespace

RestUserPropertiesResult write_rest_user_super_properties(
    const GatewayUserProperties& properties, Slice<u8> json_scratch, Slice<u8> output) noexcept {
    if (!json_scratch.data || !output.data || json_scratch.data == output.data) {
        return RestUserPropertiesResult{0, RestUserPropertiesError::invalid_input};
    }
    const GatewayCommandWriteResult measured = gateway_write_user_properties(properties, {});
    if (!measured.ok()) {
        return RestUserPropertiesResult{0, RestUserPropertiesError::invalid_properties};
    }
    const u32 required = measured.bytes;
    if (required > json_scratch.count) {
        return RestUserPropertiesResult{0, RestUserPropertiesError::json_capacity};
    }
    const u32 base64_bytes = base64_encoded_size(required);
    if (base64_bytes == 0 || base64_bytes > kRestUserSuperPropertiesMaximumBytes ||
        base64_bytes > output.count) {
        return RestUserPropertiesResult{0, RestUserPropertiesError::output_capacity};
    }

    const GatewayCommandWriteResult written =
        gateway_write_user_properties(properties, Slice<u8>{json_scratch.data, required});
    if (!written.ok() || written.bytes != required) {
        secure_clear_bytes(json_scratch.data, json_scratch.count);
        return RestUserPropertiesResult{0, RestUserPropertiesError::json_capacity};
    }
    const Base64Result base64_result =
        base64_encode(ConstSlice<u8>{json_scratch.data, required},
                      Slice<char>{reinterpret_cast<char*>(output.data), output.count});
    secure_clear_bytes(json_scratch.data, required);
    if (!base64_result.ok() || base64_result.bytes != base64_bytes) {
        return RestUserPropertiesResult{0, RestUserPropertiesError::output_capacity};
    }
    return RestUserPropertiesResult{base64_bytes, RestUserPropertiesError::none};
}

namespace {
// The bytes encodeURIComponent leaves as they are.
bool audit_unreserved(u8 value) noexcept {
    return ascii_is_alnum(value) || value == '-' || value == '_' || value == '.' || value == '!' ||
           value == '~' || value == '*' || value == '\'' || value == '(' || value == ')';
}
bool audit_hex(u8 value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
}
} // namespace

RestAuditReasonResult rest_encode_audit_reason(Utf8View reason, Slice<u8> output) noexcept {
    if ((!reason.data && reason.bytes) || (!output.data && output.count) ||
        reason.bytes > kRestAuditReasonMaximumBytes || !utf8_validate(reason).valid)
        return {0, false};
    u32 required = 0;
    for (u32 i = 0; i < reason.bytes; ++i) {
        const u8 value = static_cast<u8>(reason.data[i]);
        if (value < 0x20 || value == 0x7F)
            return {0, false};
        required += audit_unreserved(value) ? 1u : 3u;
    }
    if (required > output.count)
        return {required, false};
    u32 used = 0;
    for (u32 i = 0; i < reason.bytes; ++i) {
        const u8 value = static_cast<u8>(reason.data[i]);
        if (audit_unreserved(value)) {
            output[used++] = value;
        } else {
            output[used++] = '%';
            output[used++] = static_cast<u8>(ascii_hex_digit_upper(value >> 4));
            output[used++] = static_cast<u8>(ascii_hex_digit_upper(value));
        }
    }
    return {used, true};
}

bool rest_audit_reason_valid(ConstSlice<u8> encoded) noexcept {
    if ((!encoded.data && encoded.count) || encoded.count > kRestAuditReasonMaximumEncodedBytes)
        return false;
    for (u32 i = 0; i < encoded.count; ++i) {
        if (audit_unreserved(encoded[i]))
            continue;
        if (encoded[i] != '%' || encoded.count - i < 3 || !audit_hex(encoded[i + 1]) || !audit_hex(encoded[i + 2]))
            return false;
        i += 2;
    }
    return true;
}

namespace {
constexpr Utf8View kExtraHeaderNames[] = {
    utf8_literal("X-Captcha-Key"),
    utf8_literal("X-Captcha-Rqtoken"),
    utf8_literal("X-Captcha-Session-Id"),
    utf8_literal("X-Context-Properties"),
    utf8_literal("X-Discord-Original-MD5"),
};
bool extra_header_name(const u8* data, u32 bytes) noexcept {
    for (const auto name : kExtraHeaderNames)
        if (name.bytes == bytes && bytes_equal(name.data, data, bytes))
            return true;
    return false;
}
bool extra_header_value(const u8* data, u32 bytes) noexcept {
    if (!bytes)
        return false;
    for (u32 i = 0; i < bytes; ++i)
        if (data[i] < 0x20 || data[i] > 0x7E)
            return false;
    return data[0] != ' ' && data[bytes - 1] != ' ';
}
} // namespace

bool rest_extra_headers_valid(ConstSlice<u8> headers) noexcept {
    if ((!headers.data && headers.count) || headers.count > kRestExtraHeadersMaximumBytes)
        return false;
    u32 at = 0;
    while (at < headers.count) {
        u32 colon = at;
        while (colon < headers.count && headers[colon] != ':')
            ++colon;
        if (colon + 2 >= headers.count || headers[colon + 1] != ' ' || !extra_header_name(headers.data + at, colon - at))
            return false;
        u32 end = colon + 2;
        while (end < headers.count && headers[end] != '\r')
            ++end;
        if (end + 1 >= headers.count || headers[end + 1] != '\n' ||
            !extra_header_value(headers.data + colon + 2, end - colon - 2))
            return false;
        at = end + 2;
    }
    return true;
}

u32 rest_append_extra_header(Slice<u8> output, u32 used, Utf8View name, ConstSlice<u8> value) noexcept {
    const u32 bytes = name.bytes + 2 + value.count + 2;
    if (!output.data || used > output.count || bytes > output.count - used ||
        used + bytes > kRestExtraHeadersMaximumBytes ||
        !extra_header_name(reinterpret_cast<const u8*>(name.data), name.bytes) ||
        !extra_header_value(value.data, value.count))
        return ~0u;
    u8* cursor = output.data + used;
    copy_bytes(cursor, name.data, name.bytes);
    cursor += name.bytes;
    *cursor++ = ':';
    *cursor++ = ' ';
    copy_bytes(cursor, value.data, value.count);
    cursor += value.count;
    *cursor++ = '\r';
    *cursor = '\n';
    return used + bytes;
}

bool rest_multipart_boundary_valid(ConstSlice<u8> boundary) noexcept {
    if (!boundary.data || !boundary.count || boundary.count > kRestMultipartBoundaryMaximumBytes)
        return false;
    for (u32 i = 0; i < boundary.count; ++i) {
        const u8 c = boundary[i];
        if (!(ascii_is_alnum(c) || c == '-' || c == '_'))
            return false;
    }
    return true;
}

namespace {
// The request head for a request write_rest_request_head validated, in the
// order Chromium's network stack sends an XMLHttpRequest: Host, Connection and
// the body length first, then the page's fields (client hints and the
// headers Discord sets), User-Agent, Content-Type, Accept, Origin, the fetch
// metadata and Referer, then Accept-Encoding, Accept-Language and Cookie. An
// empty POST or PUT body is declared with Content-Length: 0; other bodyless
// methods send no length. Returns where the extra headers end.
u32 emit_request_head(Writer& writer, const RestRequestHead& request) noexcept {
    const char* const method = method_literal(request.method);
    const RestUserRequestHeaders* const page = request.user_headers;
    writer.raw(method, literal_bytes(method));
    writer.literal(" ");
    writer.bytes(request.target);
    writer.literal(" HTTP/1.1\r\nHost: ");
    writer.literal(kApiHost);
    if (request.close_connection)
        writer.literal("\r\nConnection: close\r\n");
    else
        writer.literal("\r\nConnection: keep-alive\r\n");
    if (request.json_body) {
        writer.literal("Content-Length: ");
        writer.number(request.content_length);
        writer.literal("\r\n");
    } else if (request.method == RestMethod::post || request.method == RestMethod::put) {
        writer.literal("Content-Length: 0\r\n");
    }
    if (page) {
        writer.literal("sec-ch-ua: ");
        writer.raw(kClientBrandList.text, kClientBrandList.bytes);
        writer.literal("\r\nsec-ch-ua-mobile: ?0\r\nsec-ch-ua-platform: \"Windows\"\r\n");
    }
    if (request.authorization != RestAuthorization::none) {
        if (request.authorization == RestAuthorization::bearer)
            writer.literal("Authorization: Bearer ");
        else
            writer.literal("Authorization: ");
        writer.bytes(request.token);
        writer.literal("\r\n");
    }
    if (page) {
        // In the order the desktop client sets them.
        writer.literal("X-Super-Properties: ");
        writer.bytes(page->super_properties);
        writer.literal("\r\nX-Discord-Locale: ");
        writer.raw(page->locale.data, page->locale.bytes);
        writer.literal("\r\n");
        if (page->timezone.bytes) {
            writer.literal("X-Discord-Timezone: ");
            writer.raw(page->timezone.data, page->timezone.bytes);
            writer.literal("\r\n");
        }
    }
    if (request.audit_reason.count || request.send_empty_audit_reason) {
        writer.literal("X-Audit-Log-Reason: ");
        writer.bytes(request.audit_reason);
        writer.literal("\r\n");
    }
    writer.bytes(request.extra_headers);
    const u32 extra_headers_end = static_cast<u32>(writer.at);
    writer.literal("User-Agent: ");
    writer.literal(kClientUserAgent);
    writer.literal("\r\n");
    if (request.json_body) {
        if (request.multipart_boundary.count) {
            writer.literal("Content-Type: multipart/form-data; boundary=");
            writer.bytes(request.multipart_boundary);
            writer.literal("\r\n");
        } else {
            writer.literal("Content-Type: application/json\r\n");
        }
    }
    // XMLHttpRequest's default; Discord's HTTP wrapper sets no Accept.
    writer.literal("Accept: */*\r\n");
    if (page) {
        // Chromium sends the page origin on same-origin requests other than
        // GET and HEAD. The page sets no referrer policy, so the default
        // strict-origin-when-cross-origin sends its whole URL.
        if (request.method != RestMethod::get && request.method != RestMethod::head)
            writer.literal("Origin: https://discord.com\r\n");
        writer.literal("Sec-Fetch-Site: same-origin\r\nSec-Fetch-Mode: cors\r\nSec-Fetch-Dest: empty\r\n");
        if (request.authorization == RestAuthorization::normal_user)
            writer.literal("Referer: https://discord.com/channels/@me\r\n");
        else
            writer.literal("Referer: https://discord.com/login\r\n");
    }
    if (request.accept_gzip)
        writer.literal("Accept-Encoding: gzip\r\n");
    if (page && page->accept_language.bytes) {
        writer.literal("Accept-Language: ");
        writer.raw(page->accept_language.data, page->accept_language.bytes);
        writer.literal("\r\n");
    }
    if (request.cookie.count) {
        writer.literal("Cookie: ");
        writer.bytes(request.cookie);
        writer.literal("\r\n");
    }
    writer.literal("\r\n");
    return extra_headers_end;
}

// A Cookie value: no control characters other than tab.
bool valid_cookie_header(ConstSlice<u8> cookie) noexcept {
    if ((!cookie.data && cookie.count) || cookie.count > kCookieHeaderMaximumBytes)
        return false;
    for (u32 i = 0; i < cookie.count; ++i)
        if ((cookie[i] < 0x20 && cookie[i] != '\t') || cookie[i] == 0x7F)
            return false;
    return true;
}
} // namespace

RestRequestWriteResult write_rest_request_head(const RestRequestHead& request,
                                               Slice<u8> output) noexcept {
    const char* const method = method_literal(request.method);
    const bool multipart = request.multipart_boundary.count != 0;
    if ((!request.multipart_boundary.data && request.multipart_boundary.count) ||
        (multipart && (!request.json_body || !rest_multipart_boundary_valid(request.multipart_boundary))))
        return RestRequestWriteResult{0, RestRequestError::invalid_input};
    const bool normal_user = request.authorization == RestAuthorization::normal_user;
    if (!method || (!output.data && output.count != 0) ||
        (request.authorization == RestAuthorization::none && request.token.count != 0) ||
        (request.authorization != RestAuthorization::none &&
         request.authorization != RestAuthorization::bearer && !normal_user) ||
        (normal_user && !request.user_headers) ||
        (request.authorization == RestAuthorization::bearer && request.user_headers) ||
        (request.user_headers && !valid_user_headers(*request.user_headers)) ||
        (!request.token.data && request.token.count != 0) ||
        (!request.json_body && request.content_length != 0) ||
        !rest_audit_reason_valid(request.audit_reason) ||
        (request.send_empty_audit_reason && request.method == RestMethod::get) ||
        !rest_extra_headers_valid(request.extra_headers) ||
        (request.cookie.count && !request.user_headers) || !valid_cookie_header(request.cookie)) {
        return RestRequestWriteResult{0, RestRequestError::invalid_input};
    }
    if (!valid_target(request.target)) {
        return RestRequestWriteResult{0, RestRequestError::invalid_target};
    }
    if (request.authorization != RestAuthorization::none && !valid_token(request.token)) {
        return RestRequestWriteResult{0, RestRequestError::invalid_token};
    }

    // The first pass measures; nothing is written unless the head fits.
    Writer writer{};
    emit_request_head(writer, request);
    if (writer.at > output.count) {
        return RestRequestWriteResult{0, RestRequestError::output_capacity};
    }
    const u32 required = static_cast<u32>(writer.at);
    writer = Writer{output.data, 0};
    const u32 extra_headers_end = emit_request_head(writer, request);
    return RestRequestWriteResult{required, RestRequestError::none, extra_headers_end};
}

RestHeadResult parse_rest_response_head(ConstSlice<u8> input, u32 maximum_header_bytes,
                                        u32 maximum_fields) noexcept {
    if ((!input.data && input.count != 0) || maximum_header_bytes == 0 || maximum_fields == 0) {
        return RestHeadResult{{},
                              {},
                              0,
                              HttpError::invalid_input,
                              RestHeadError::invalid_input,
                              HttpParseState::error};
    }
    RateHeadContext context{};
    const HttpHeadResult parsed =
        http_parse_response_head(input, maximum_header_bytes, maximum_fields, visit_rate_header, &context);
    if (parsed.state == HttpParseState::need_more) {
        return RestHeadResult{
            {}, {}, 0, HttpError::none, RestHeadError::none, HttpParseState::need_more};
    }
    if (parsed.state == HttpParseState::error) {
        if (context.error != RestHeadError::none) {
            return RestHeadResult{{},           context.rate,  context.error_offset,
                                  parsed.error, context.error, HttpParseState::error};
        }
        return RestHeadResult{
            {}, {}, parsed.error_offset, parsed.error, RestHeadError::http, HttpParseState::error};
    }
    if ((context.rate.fields & rate_limit_field_limit) != 0 &&
        (context.rate.fields & rate_limit_field_remaining) != 0 &&
        context.rate.remaining > context.rate.limit) {
        return RestHeadResult{parsed.head,
                              context.rate,
                              0,
                              HttpError::none,
                              RestHeadError::inconsistent_rate_fields,
                              HttpParseState::error};
    }
    if ((context.rate.fields & rate_limit_field_global) != 0 && context.rate.global &&
        (context.rate.fields & rate_limit_field_scope) != 0 &&
        context.rate.scope != RateLimitScope::global) {
        return RestHeadResult{parsed.head,
                              context.rate,
                              0,
                              HttpError::none,
                              RestHeadError::inconsistent_rate_fields,
                              HttpParseState::error};
    }
    RestHeadResult result{parsed.head,
                          context.rate,
                          0,
                          HttpError::none,
                          RestHeadError::none,
                          HttpParseState::complete};
    result.representation = context.representation;
    return result;
}

RestBodyKind rest_classify_body(ConstSlice<u8> body) noexcept {
    if (!body.data || body.count == 0) {
        return RestBodyKind::empty;
    }
    if (body.count >= 2 && body[0] == 0x1F && body[1] == 0x8B) {
        return RestBodyKind::gzip;
    }
    if (body.count >= 4 && body[0] == 0x28 && body[1] == 0xB5 && body[2] == 0x2F && body[3] == 0xFD) {
        return RestBodyKind::zstd;
    }
    if (body.count >= 2 && (body[0] & 0x0F) == 8 && (body[0] >> 4) <= 7 &&
        ((static_cast<u32>(body[0]) << 8) | body[1]) % 31 == 0) {
        return RestBodyKind::zlib;
    }

    u32 offset = 0;
    if (body.count >= 3 && body[0] == 0xEF && body[1] == 0xBB && body[2] == 0xBF) {
        offset = 3;
    }
    while (offset < body.count &&
           (body[offset] == ' ' || body[offset] == '\t' || body[offset] == '\r' || body[offset] == '\n')) {
        ++offset;
    }
    if (offset == body.count) {
        return RestBodyKind::utf8_text;
    }
    if (body[offset] == '{') {
        return RestBodyKind::json_object;
    }
    if (body[offset] == '[') {
        return RestBodyKind::json_array;
    }
    if (body[offset] == '<') {
        return RestBodyKind::markup;
    }
    return body[offset] >= 0x20 && body[offset] <= 0x7E ? RestBodyKind::utf8_text
                                                        : RestBodyKind::binary;
}

static RateLimitBodyResult decode_retry_body(Utf8View body, Slice<u32> structural_scratch,
                                             u32 maximum_depth, bool retry_only) noexcept {
    if (!body.data || body.bytes == 0 || !structural_scratch.data || structural_scratch.count == 0 ||
        maximum_depth == 0) {
        return RateLimitBodyResult{
            0,    0, RateLimitBodyError::invalid_input, JsonScanError::none, JsonReadError::none,
            false};
    }
    const JsonScanResult scan = json_scan_structurals_avx1(body, structural_scratch);
    if (!scan.ok()) {
        return RateLimitBodyResult{
            0,          scan.error_offset,   RateLimitBodyError::scan_failed,
            scan.error, JsonReadError::none, false};
    }
    RateBodyContext context{};
    context.retry_only = retry_only;
    const JsonReadResult read =
        json_visit_root_object(body, ConstSlice<u32>{structural_scratch.data, scan.count}, maximum_depth,
                               visit_rate_body, &context);
    if (!read.ok()) {
        return RateLimitBodyResult{
            0,
            context.error != RateLimitBodyError::none ? context.error_offset : read.error_offset,
            context.error != RateLimitBodyError::none ? context.error
                                                             : RateLimitBodyError::invalid_json,
            JsonScanError::none,
            read.error,
            false};
    }
    const u32 required = rate_body_field_retry_after | rate_body_field_global;
    if (retry_only ? !(context.fields & (rate_body_field_code | rate_body_field_retry_after))
                   : (context.fields & required) != required) {
        return RateLimitBodyResult{0,
                                   0,
                                   RateLimitBodyError::missing_required_field,
                                   JsonScanError::none,
                                   JsonReadError::none,
                                   false};
    }
    return RateLimitBodyResult{context.retry_after_ms,
                               0,
                               RateLimitBodyError::none,
                               JsonScanError::none,
                               JsonReadError::none,
                               context.global};
}

RateLimitBodyResult decode_rate_limit_body(Utf8View body, Slice<u32> structural_scratch,
                                           u32 maximum_depth) noexcept {
    return decode_retry_body(body, structural_scratch, maximum_depth, false);
}

RateLimitBodyResult decode_search_indexing_body(Utf8View body, Slice<u32> structural_scratch,
                                            u32 maximum_depth) noexcept {
    return decode_retry_body(body, structural_scratch, maximum_depth, true);
}

bool rest_rate_scheduler_initialize(RestRateScheduler* scheduler,
                                    Slice<RestRateBucket> buckets,
                                    Slice<RestRateBinding> bindings) noexcept {
    if (!buckets.data || buckets.count == 0 || buckets.count >= invalid_rate_index || !bindings.data ||
        bindings.count == 0 || bindings.count >= invalid_rate_index) {
        return false;
    }
    for (u32 index = 0; index < buckets.count; ++index) {
        reset_value<RestRateBucket>(&buckets[index]);
    }
    for (u32 index = 0; index < bindings.count; ++index) {
        reset_value<RestRateBinding>(&bindings[index]);
        bindings[index].bucket_index = invalid_rate_index;
    }
    reset_value<RestRateScheduler>(scheduler);
    scheduler->buckets = buckets;
    scheduler->bindings = bindings;
    return true;
}

RestRateAcquireResult rest_rate_acquire(RestRateScheduler* scheduler,
                                        const RestRouteKey& route,
                                        u64 now_ms) noexcept {
    if (!scheduler->buckets.data || !scheduler->bindings.data || !valid_route(route)) {
        return RestRateAcquireResult{
            {}, 0, RestRateAcquireKind::error, RestRateError::invalid_input};
    }
    if (scheduler->global_reset_at_ms != 0) {
        if (now_ms < scheduler->global_reset_at_ms) {
            ++scheduler->waited;
            return RestRateAcquireResult{{},
                                         scheduler->global_reset_at_ms,
                                         RestRateAcquireKind::waiting,
                                         RestRateError::none};
        }
        scheduler->global_reset_at_ms = 0;
    }

    const u16 binding_index = acquire_binding_slot(scheduler, route, now_ms);
    if (binding_index == invalid_rate_index) {
        return RestRateAcquireResult{
            {}, 0, RestRateAcquireKind::error, RestRateError::capacity};
    }
    RestRateBinding& binding = scheduler->bindings[binding_index];
    if (binding.in_flight) {
        ++scheduler->busy;
        return RestRateAcquireResult{
            {}, 0, RestRateAcquireKind::busy, RestRateError::none};
    }

    RestRateBucket* bucket =
        resolve_bucket(scheduler, binding.bucket_index, binding.bucket_generation);
    if (!bucket) {
        binding.bucket_index = invalid_rate_index;
        binding.bucket_generation = 0;
    } else {
        if (bucket->reset_at_ms != 0 && now_ms >= bucket->reset_at_ms) {
            bucket->reset_at_ms = 0;
            if ((bucket->fields & rate_limit_field_limit) != 0) {
                bucket->remaining = bucket->limit;
                bucket->fields |= rate_limit_field_remaining;
            } else {
                // A 429 without X-RateLimit-Limit exhausted an unknown window;
                // once its deadline passes the next response teaches the limit.
                bucket->fields &= ~rate_limit_field_remaining;
            }
        }
        if ((bucket->fields & rate_limit_field_remaining) != 0 && bucket->remaining == 0) {
            if (bucket->reset_at_ms == 0) {
                return RestRateAcquireResult{{},
                                             0,
                                             RestRateAcquireKind::error,
                                             RestRateError::incomplete_bucket_deadline};
            }
            ++scheduler->waited;
            return RestRateAcquireResult{
                {}, bucket->reset_at_ms, RestRateAcquireKind::waiting, RestRateError::none};
        }
        if ((bucket->fields & rate_limit_field_remaining) != 0) {
            --bucket->remaining;
        }
        ++bucket->in_flight;
        bucket->last_used_ms = now_ms;
    }

    binding.in_flight = true;
    binding.last_used_ms = now_ms;
    ++scheduler->acquired;
    return RestRateAcquireResult{
        RestRateLease{binding_index, binding.generation,
                      bucket ? binding.bucket_index : invalid_rate_index,
                      bucket ? binding.bucket_generation : static_cast<u16>(0), true},
        0, RestRateAcquireKind::acquired, RestRateError::none};
}

RestRateObservation rest_rate_observe(RestRateScheduler* scheduler,
                                      RestRateLease* lease,
                                      const RestHeadResult& response_head,
                                      const RateLimitBodyResult* rate_limit_body,
                                      u64 now_ms, u64 unix_time_ms) noexcept {
    if (!lease->active || !response_head.ok() || (rate_limit_body && !rate_limit_body->ok()) ||
        lease->binding_index >= scheduler->bindings.count) {
        return observation_error(RestRateError::invalid_input);
    }
    RestRateBinding& binding = scheduler->bindings[lease->binding_index];
    if (!binding.used || binding.generation != lease->binding_generation || !binding.in_flight) {
        return observation_error(RestRateError::stale_lease);
    }

    const RestRouteKey route = binding.route;
    release_lease_bucket(scheduler, *lease);
    binding.in_flight = false;
    lease->active = false;
    ++scheduler->observations;

    const RateLimitHeaders& rate = response_head.rate;
    const bool limited = response_head.head.status_code == 429;
    const bool has_bucket = (rate.fields & rate_limit_field_bucket) != 0;
    constexpr u32 bucket_fields = rate_limit_field_bucket | rate_limit_field_limit |
                                  rate_limit_field_remaining | rate_limit_field_reset |
                                  rate_limit_field_reset_after;
    const bool has_rate_data = (rate.fields & bucket_fields) != 0 || limited;
    RestRateBucket* bucket = nullptr;
    u16 bucket_index = invalid_rate_index;
    if (has_rate_data) {
        if (!has_bucket) {
            bucket = resolve_bucket(scheduler, binding.bucket_index, binding.bucket_generation);
            if (bucket) {
                bucket_index = binding.bucket_index;
            }
        }
        if (!bucket) {
            bucket_index = acquire_bucket_slot(scheduler, route, has_bucket ? &rate : nullptr, now_ms);
        }
        if (bucket_index == invalid_rate_index) {
            return observation_error(RestRateError::capacity);
        }
        bucket = &scheduler->buckets[bucket_index];
        if (binding.bucket_index != bucket_index || binding.bucket_generation != bucket->generation) {
            if (has_bucket && binding.bucket_index != invalid_rate_index) {
                ++scheduler->migrations;
            }
            binding.bucket_index = bucket_index;
            binding.bucket_generation = bucket->generation;
        }

        if ((rate.fields & rate_limit_field_limit) != 0) {
            bucket->limit = rate.limit;
            bucket->fields |= rate_limit_field_limit;
        }
        if ((rate.fields & rate_limit_field_remaining) != 0) {
            // Concurrent responses can arrive in reverse order. A late,
            // larger value must not refund reservations in the same window.
            const bool same_window = bucket->reset_at_ms > now_ms &&
                (bucket->fields & rate_limit_field_remaining) != 0;
            bucket->remaining = same_window ? minimum(bucket->remaining, rate.remaining) : rate.remaining;
            bucket->fields |= rate_limit_field_remaining;
        }
        if ((rate.fields & rate_limit_field_reset_after) != 0) {
            // Another alias can complete after an exhausted/429 response.
            // Preserve its active floor as well as its remaining reservations.
            bucket->reset_at_ms = maximum(bucket->reset_at_ms, deadline_after(now_ms, rate.reset_after_ms));
            bucket->fields |= rate_limit_field_reset_after;
        } else if ((rate.fields & rate_limit_field_reset) != 0) {
            const u64 delay = rate.reset_epoch_ms > unix_time_ms ? rate.reset_epoch_ms - unix_time_ms : 0;
            bucket->reset_at_ms = maximum(bucket->reset_at_ms, deadline_after(now_ms, delay));
            bucket->fields |= rate_limit_field_reset;
        }
        bucket->last_used_ms = now_ms;
    }

    u64 retry_after_ms = 0;
    if ((rate.fields & rate_limit_field_retry_after) != 0) {
        retry_after_ms = rate.retry_after_ms;
    }
    if (rate_limit_body && rate_limit_body->retry_after_ms > retry_after_ms) {
        retry_after_ms = rate_limit_body->retry_after_ms;
    }
    const bool global =
        limited && (((rate.fields & rate_limit_field_global) != 0 && rate.global) ||
                    ((rate.fields & rate_limit_field_scope) != 0 && rate.scope == RateLimitScope::global) ||
                    (rate_limit_body && rate_limit_body->global));
    // A non-429 Retry-After is owner-scoped metadata, not a scheduler throttle.
    u64 retry_at_ms =
        (rate.fields & rate_limit_field_retry_after) != 0 ? deadline_after(now_ms, rate.retry_after_ms) : 0;
    if (limited) {
        if ((rate.fields & rate_limit_field_retry_after) == 0 && !rate_limit_body) {
            return observation_error(RestRateError::missing_retry_deadline);
        }
        retry_at_ms = deadline_after(now_ms, retry_after_ms);
        if (global) {
            scheduler->global_reset_at_ms = maximum(scheduler->global_reset_at_ms, retry_at_ms);
            if (bucket && (bucket->fields & rate_limit_field_remaining) != 0 &&
                bucket->remaining == 0) {
                bucket->reset_at_ms = maximum(bucket->reset_at_ms, retry_at_ms);
            }
        } else {
            if (!bucket) {
                return observation_error(RestRateError::invalid_observation);
            }
            bucket->reset_at_ms = maximum(bucket->reset_at_ms, retry_at_ms);
            bucket->remaining = 0;
            bucket->fields |= rate_limit_field_remaining;
        }
    }

    const u16 status = response_head.head.status_code;
    const bool shared_429 = status == 429 && (rate.fields & rate_limit_field_scope) != 0 &&
                            rate.scope == RateLimitScope::shared;
    if (status == 401 || status == 403 || (status == 429 && !shared_429)) {
        ++scheduler->invalid_requests;
    }
    if (bucket && (bucket->fields & rate_limit_field_remaining) != 0 && bucket->remaining == 0 &&
        bucket->reset_at_ms == 0) {
        return observation_error(RestRateError::incomplete_bucket_deadline);
    }
    return RestRateObservation{retry_at_ms, RestRateError::none, limited, global};
}

bool rest_rate_cancel(RestRateScheduler* scheduler, RestRateLease* lease) noexcept {
    if (!lease->active || lease->binding_index >= scheduler->bindings.count) {
        return false;
    }
    RestRateBinding& binding = scheduler->bindings[lease->binding_index];
    if (!binding.used || binding.generation != lease->binding_generation || !binding.in_flight) {
        return false;
    }
    release_lease_bucket(scheduler, *lease);
    binding.in_flight = false;
    lease->active = false;
    ++scheduler->cancellations;
    return true;
}

namespace {

HttpHeadResult rest_response_head_parser(void* context, ConstSlice<u8> input, u32 maximum_header_bytes,
                                         u32 maximum_fields) noexcept {
    auto* response = static_cast<RestResponse*>(context);
    const RestHeadResult parsed =
        parse_rest_response_head(input, maximum_header_bytes, maximum_fields);
    response->parsed_head = parsed;
    return HttpHeadResult{parsed.head, parsed.error_offset, parsed.http_error, parsed.state};
}

} // namespace

bool rest_response_initialize(RestResponse* response, Slice<u8> input_storage,
                              Slice<u8> body_output, RestMethod request_method) noexcept {
    if (!input_storage.data || input_storage.count < kRestMaximumHeaderBytes ||
        (!body_output.data && body_output.count != 0) || !method_literal(request_method)) {
        return false;
    }
    reset_value<RestResponse>(response);
    response->request_method = request_method;
    return http_response_decoder_initialize(&response->http, input_storage, {}, body_output,
                                            kRestMaximumHeaderBytes, kRestMaximumHeaderFields,
                                            request_method == RestMethod::head,
                                            rest_response_head_parser, response);
}

RestResponseStep rest_response_feed(RestResponse* response, ConstSlice<u8> input,
                                    bool peer_closed) noexcept {
    return http_response_decoder_feed(&response->http, input, peer_closed);
}

} // namespace maicord
