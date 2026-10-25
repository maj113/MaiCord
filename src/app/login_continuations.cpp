#include "maicord/app/login_continuations.hpp"
#include "maicord/protocol/login_continuations.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
inline constexpr Utf8View kPrefix = utf8_literal("https://discord.com/verify");
inline constexpr Utf8View kResetPrefix = utf8_literal("https://discord.com/reset");
bool link_candidate(Utf8View link, Utf8View prefix) noexcept {
    if (!link.data || link.bytes < prefix.bytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(link.data), link.bytes})) return false;
    for (u32 i = 0; i < prefix.bytes; ++i)
        if (link.data[i] != prefix.data[i]) return false;
    return link.bytes == prefix.bytes || link.data[prefix.bytes] == '?' || link.data[prefix.bytes] == '#';
}
bool next_byte(Utf8View value, u32* cursor, u8* decoded) noexcept {
    const char c = value.data[(*cursor)++];
    if (c != '%') {
        *decoded = c == '+' ? ' ' : static_cast<u8>(c);
        return true;
    }
    if (value.bytes - *cursor < 2) return false;
    const int a = ascii_hex_value(value.data[*cursor]);
    const int b = ascii_hex_value(value.data[*cursor + 1]);
    if (a < 0 || b < 0) return false;
    *cursor += 2;
    *decoded = static_cast<u8>((a << 4) | b);
    return true;
}
bool token_name(Utf8View name) noexcept {
    static constexpr char expected[] = "token";
    u32 cursor = 0;
    u32 count = 0;
    while (cursor < name.bytes) {
        u8 decoded = 0;
        if (!next_byte(name, &cursor, &decoded) || count == 5 || decoded != expected[count++])
            return false;
    }
    return count == 5;
}
LoginVerificationLinkError find_token(Utf8View parameters, Utf8View* selected, bool* found) noexcept {
    *found = false;
    for (u32 begin = 0; begin < parameters.bytes;) {
        u32 end = begin;
        while (end < parameters.bytes && parameters.data[end] != '&') ++end;
        u32 equals = begin;
        while (equals < end && parameters.data[equals] != '=') ++equals;
        if (token_name({parameters.data + begin, equals - begin})) {
            if (*found) return LoginVerificationLinkError::duplicate_token;
            *found = true;
            const u32 start = equals == end ? end : equals + 1;
            *selected = {parameters.data + start, end - start};
        }
        begin = end == parameters.bytes ? end : end + 1;
    }
    return LoginVerificationLinkError::none;
}
LoginVerificationLinkResult fail_link(SecureBuffer* output, LoginVerificationLinkError error) noexcept {
    if (output && output->locked && output->data &&
        byte_range_valid({output->data, output->capacity})) secure_buffer_clear(output);
    return {0, error};
}
// Validate form-percent UTF-8 without copying either selected or ignored data.
bool form_valid(Utf8View value, u32* measured = nullptr) noexcept {
    u32 cursor = 0, bytes = 0, remaining = 0;
    u8 low = 0x80, high = 0xBF;
    while (cursor < value.bytes) {
        u8 c = 0;
        if (!next_byte(value, &cursor, &c) || c < 0x20 || c == 0x7F) return false;
        ++bytes;
        if (remaining) {
            if (c < low || c > high) return false;
            --remaining; low = 0x80; high = 0xBF;
        } else if (c < 0x80) {
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            remaining = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            remaining = 2; low = c == 0xE0 ? 0xA0 : 0x80; high = c == 0xED ? 0x9F : 0xBF;
        } else if (c >= 0xF0 && c <= 0xF4) {
            remaining = 3; low = c == 0xF0 ? 0x90 : 0x80; high = c == 0xF4 ? 0x8F : 0xBF;
        } else return false;
    }
    if (remaining) return false;
    if (measured) *measured = bytes;
    return true;
}
LoginResetLinkError find_reset_token(Utf8View parameters, Utf8View* selected, bool* found) noexcept {
    *found = false;
    for (u32 begin = 0; begin < parameters.bytes;) {
        u32 end = begin;
        while (end < parameters.bytes && parameters.data[end] != '&') ++end;
        u32 equals = begin;
        while (equals < end && parameters.data[equals] != '=') ++equals;
        const Utf8View name{parameters.data + begin, equals - begin};
        const u32 start = equals == end ? end : equals + 1;
        const Utf8View value{parameters.data + start, end - start};
        if (!form_valid(name) || !form_valid(value)) return LoginResetLinkError::malformed_encoding;
        static constexpr char token[] = "token";
        u32 cursor = 0, count = 0;
        bool prefix = true, array = false;
        while (cursor < name.bytes) {
            u8 c = 0;
            next_byte(name, &cursor, &c);
            if (count < 5 && c != token[count]) prefix = false;
            if (count == 5 && prefix && c == '[') array = true;
            ++count;
        }
        if (array) return LoginResetLinkError::duplicate_token;
        if (prefix && count == 5) {
            if (*found) return LoginResetLinkError::duplicate_token;
            *found = true;
            *selected = value;
        }
        begin = end == parameters.bytes ? end : end + 1;
    }
    return LoginResetLinkError::none;
}
} // namespace

bool login_verification_link_candidate(Utf8View link) noexcept {
    return link_candidate(link, kPrefix);
}

LoginVerificationLinkResult login_verification_consume_link(Utf8View link, SecureBuffer* output) noexcept {
    if (!output || !output->locked || !output->data || !output->capacity ||
        !byte_range_valid({output->data, output->capacity}) || !link.data || !link.bytes ||
        link.bytes > kLoginVerificationMaximumLinkBytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(link.data), link.bytes}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(link.data), link.bytes},
                            {output->data, output->capacity})) {
        return fail_link(output, LoginVerificationLinkError::invalid_input);
    }
    constexpr Utf8View prefix = kPrefix;
    if (!login_verification_link_candidate(link))
        return fail_link(output, LoginVerificationLinkError::unsupported_link);
    u32 fragment = prefix.bytes;
    while (fragment < link.bytes && link.data[fragment] != '#') ++fragment;
    Utf8View selected{};
    bool found = false;
    LoginVerificationLinkError error = LoginVerificationLinkError::none;
    if (fragment < link.bytes)
        error = find_token({link.data + fragment + 1, link.bytes - fragment - 1}, &selected, &found);
    if (error != LoginVerificationLinkError::none) return fail_link(output, error);
    if (!found && prefix.bytes < fragment && link.data[prefix.bytes] == '?')
        error = find_token({link.data + prefix.bytes + 1, fragment - prefix.bytes - 1}, &selected, &found);
    if (error != LoginVerificationLinkError::none) return fail_link(output, error);
    if (!found || !selected.bytes) return fail_link(output, LoginVerificationLinkError::missing_token);
    // Validate/decode once without storage to bound the output before touching it.
    u32 cursor = 0;
    u32 bytes = 0;
    while (cursor < selected.bytes) {
        u8 decoded = 0;
        if (!next_byte(selected, &cursor, &decoded) || decoded < 0x20 || decoded == 0x7F)
            return fail_link(output, LoginVerificationLinkError::malformed_encoding);
        if (++bytes > kLoginVerificationMaximumTokenBytes)
            return fail_link(output, LoginVerificationLinkError::token_too_large);
    }
    if (bytes > output->capacity) return fail_link(output, LoginVerificationLinkError::output_too_small);
    secure_buffer_clear(output);
    cursor = 0;
    for (u32 i = 0; i < bytes; ++i) next_byte(selected, &cursor, output->data + i);
    output->bytes = bytes;
    if (!utf8_validate({reinterpret_cast<const char*>(output->data), bytes}).valid)
        return fail_link(output, LoginVerificationLinkError::malformed_encoding);
    return {bytes, LoginVerificationLinkError::none};
}
bool login_reset_link_candidate(Utf8View link) noexcept { return link_candidate(link, kResetPrefix); }
LoginResetLinkResult login_reset_consume_link(Utf8View link, SecureBuffer* output) noexcept {
    if (!output || !output->locked || !output->data || !output->capacity ||
        !byte_range_valid({output->data, output->capacity}) || !link.data || !link.bytes ||
        link.bytes > kLoginResetMaximumLinkBytes ||
        !byte_range_valid({reinterpret_cast<const u8*>(link.data), link.bytes}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(link.data), link.bytes}, {output->data, output->capacity}))
        return fail_link(output, LoginResetLinkError::invalid_input);
    if (!login_reset_link_candidate(link)) return fail_link(output, LoginResetLinkError::unsupported_link);
    u32 fragment = kResetPrefix.bytes;
    while (fragment < link.bytes && link.data[fragment] != '#') ++fragment;
    Utf8View query_token{}, fragment_token{};
    bool query_found = false, fragment_found = false;
    auto error = LoginResetLinkError::none;
    if (kResetPrefix.bytes < fragment && link.data[kResetPrefix.bytes] == '?')
        error = find_reset_token({link.data + kResetPrefix.bytes + 1, fragment - kResetPrefix.bytes - 1},
            &query_token, &query_found);
    if (error != LoginResetLinkError::none) return fail_link(output, error);
    if (fragment < link.bytes)
        error = find_reset_token({link.data + fragment + 1, link.bytes - fragment - 1}, &fragment_token, &fragment_found);
    if (error != LoginResetLinkError::none) return fail_link(output, error);
    const Utf8View selected = fragment_found ? fragment_token : query_token;
    if ((!fragment_found && !query_found) || !selected.bytes) return fail_link(output, LoginResetLinkError::missing_token);
    u32 bytes = 0;
    if (!form_valid(selected, &bytes)) return fail_link(output, LoginResetLinkError::malformed_encoding);
    if (bytes > kLoginResetMaximumTokenBytes) return fail_link(output, LoginResetLinkError::token_too_large);
    if (bytes > output->capacity) return fail_link(output, LoginResetLinkError::output_too_small);
    secure_buffer_clear(output);
    u32 cursor = 0;
    for (u32 i = 0; i < bytes; ++i) next_byte(selected, &cursor, output->data + i);
    output->bytes = bytes;
    return {bytes, LoginResetLinkError::none};
}
} // namespace maicord
