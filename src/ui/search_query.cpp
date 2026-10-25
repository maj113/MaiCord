#include "maicord/ui/search_query.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
bool equal(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
// Filter keys are camelCase in the query syntax but match case-insensitively.
bool key_equal(Utf8View key, Utf8View name) noexcept {
    if (key.bytes != name.bytes)
        return false;
    for (u32 i = 0; i < key.bytes; ++i) {
        char a = key.data[i], b = name.data[i];
        a = a >= 'A' && a <= 'Z' ? static_cast<char>(a + 32) : a;
        b = b >= 'A' && b <= 'Z' ? static_cast<char>(b + 32) : b;
        if (a != b)
            return false;
    }
    return true;
}
u8 author_type(Utf8View value) noexcept {
    const Utf8View labels[] = {utf8_literal("user"), utf8_literal("bot"), utf8_literal("webhook")};
    for (u32 i = 0; i < 3; ++i)
        if (equal(value, labels[i]))
            return static_cast<u8>(1u << i);
    return 0;
}
// A negated value ("-link") moves the flag to the excluded set.
bool negatable(Utf8View value, u32 (*parse)(Utf8View) noexcept, u32* included, u32* excluded) noexcept {
    const bool negated = value.bytes > 1 && value.data[0] == '-';
    const u32 flag = parse(negated ? Utf8View{value.data + 1, value.bytes - 1} : value);
    if (!flag || ((negated ? *included : *excluded) & flag))
        return false;
    (negated ? *excluded : *included) |= flag;
    return true;
}
bool term(Utf8View value, char* output, u8* bytes, u32 capacity, bool hostname, bool extension) noexcept {
    if (extension && value.bytes && value.data[0] == '.')
        value = {value.data + 1, value.bytes - 1};
    if (!value.bytes || value.bytes > capacity || *bytes)
        return false;
    for (u32 i = 0; i < value.bytes; ++i) {
        char c = value.data[i];
        const bool alnum = ascii_is_alnum(c);
        if ((hostname && !alnum && c != '.' && c != '-') || (extension && !alnum))
            return false;
        if (hostname || extension)
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        output[i] = c;
    }
    *bytes = static_cast<u8>(value.bytes);
    return true;
}
bool quoted_value(Utf8View value, Slice<char> storage, Utf8View* out) noexcept {
    if (!value.bytes || value.data[0] != '"') {
        *out = value;
        return true;
    }
    if (value.bytes < 2 || value.data[value.bytes - 1] != '"') return false;
    u32 bytes = 0;
    for (u32 at = 1; at + 1 < value.bytes; ++at) {
        char c = value.data[at];
        if (c == '"') return false;
        if (c == '\\' && at + 2 < value.bytes &&
            (value.data[at + 1] == '"' || value.data[at + 1] == '\\'))
            c = value.data[++at];
        if (bytes == storage.count) return false;
        storage[bytes++] = c;
    }
    *out = {storage.data, bytes};
    return true;
}
bool number(Utf8View t, u64* out) noexcept {
    if (!t.bytes || t.bytes > 20)
        return false;
    u64 value = 0;
    for (u32 i = 0; i < t.bytes; ++i) {
        const char c = t.data[i];
        if (c < '0' || c > '9' || value > (~0ull - static_cast<u64>(c - '0')) / 10)
            return false;
        value = value * 10 + c - '0';
    }
    *out = value;
    return value != 0;
}
bool append_id(Utf8View value, u64* ids, u8* count, const UiSearchQueryContext* context,
               bool channel) noexcept {
    if (!value.bytes) return false;
    // Native mentions carry a type. A channel mention must not silently become
    // a user filter (or vice versa) just because both contain a snowflake.
    if (value.bytes >= 2 && value.data[0] == '<' &&
        (value.data[1] == '@' || value.data[1] == '#')) {
        if (value.bytes < 4 || value.data[value.bytes - 1] != '>' ||
            value.data[1] != (channel ? '#' : '@')) return false;
        const u32 prefix = !channel && value.data[2] == '!' ? 3 : 2;
        value = {value.data + prefix, value.bytes - prefix - 1};
        u64 mention_id = 0;
        if (!number(value, &mention_id)) return false;
    }
    u64 id = 0;
    if (!number(value, &id)) {
        if (!context)
            return false;
        const auto resolve = channel ? context->channel : context->user;
        if (!resolve || !resolve(context->context, value, &id) || !id)
            return false;
    }
    for (u32 i = 0; i < *count; ++i)
        if (ids[i] == id)
            return true;
    if (*count == kMessageSearchFilterIds)
        return false;
    ids[(*count)++] = id;
    return true;
}
bool date(Utf8View value, u64* day) noexcept {
    if (value.bytes != 10 || value.data[4] != '-' || value.data[7] != '-')
        return false;
    u32 year = 0, month = 0, d = 0;
    for (u32 i = 0; i < 10; ++i) {
        if (i == 4 || i == 7)
            continue;
        const auto c = value.data[i];
        if (c < '0' || c > '9')
            return false;
        if (i < 4)
            year = year * 10 + c - '0';
        else if (i < 7)
            month = month * 10 + c - '0';
        else
            d = d * 10 + c - '0';
    }
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const u8 days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 2015 || year > 2100 || !month || month > 12 || !d ||
        d > static_cast<u32>(days[month - 1]) + (month == 2 && leap ? 1u : 0u))
        return false;
    u64 count = 0;
    for (u32 y = 2015; y < year; ++y)
        count += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (u32 m = 1; m < month; ++m)
        count += days[m - 1] + (m == 2 && leap ? 1 : 0);
    count += d - 1;
    *day = count * 86400000ull;
    return true;
}
u16 has(Utf8View value) noexcept {
    // The search UI calls forwarded-message snapshots "Forward"; the
    // REST search parameter retains its snapshot spelling.
    if (equal(value, utf8_literal("forward"))) return search_has_snapshot;
    const Utf8View labels[] = {utf8_literal("link"),    utf8_literal("embed"), utf8_literal("file"),
                               utf8_literal("video"),   utf8_literal("image"), utf8_literal("sound"),
                               utf8_literal("sticker"), utf8_literal("poll"),  utf8_literal("snapshot")};
    for (u32 i = 0; i < 9; ++i)
        if (equal(value, labels[i]))
            return static_cast<u16>(1u << i);
    return 0;
}
} // namespace
} // namespace maicord
namespace maicord {
UiSearchQueryResult ui_search_query_parse(Utf8View text, MessageSearchScope scope, u64 scope_id,
                                          MessageSearchSort sort,
                                          const UiSearchQueryContext* context) noexcept {
    UiSearchQueryResult result{};
    auto& q = result.query;
    q.scope = scope;
    q.scope_id = scope_id;
    q.sort = sort;
    q.limit = 25;
    if (!scope_id || !utf8_validate(text).valid || text.bytes > kMessageSearchContentBytes) {
        result.error = UiSearchQueryError::invalid_text;
        return result;
    }
    for (u32 at = 0; at < text.bytes;) {
        while (at < text.bytes && text.data[at] == ' ')
            ++at;
        if (at == text.bytes)
            break;
        const u32 first = at;
        bool quoted = false, escaped = false;
        u32 quote_offset = first;
        while (at < text.bytes) {
            const char c = text.data[at];
            if (static_cast<u8>(c) < 32 || c == 0x7f) {
                result.error = UiSearchQueryError::invalid_text;
                result.error_offset = at;
                return result;
            }
            if (c == '"' && !escaped) {
                quoted = !quoted;
                if (quoted) quote_offset = at;
            }
            if (c == ' ' && !quoted)
                break;
            escaped = c == '\\' ? !escaped : false;
            ++at;
        }
        if (quoted) {
            result.error = UiSearchQueryError::invalid_text;
            result.error_offset = quote_offset;
            return result;
        }
        const Utf8View token{text.data + first, at - first};
        u32 colon = 0;
        while (colon < token.bytes && token.data[colon] != ':')
            ++colon;
        const Utf8View key{token.data, colon};
        Utf8View value{token.data + minimum(colon + 1, token.bytes),
                       token.bytes - minimum(colon + 1, token.bytes)};
        bool filter = colon < token.bytes, ok = true;
        const Utf8View keys[] = {
            utf8_literal("from"),      utf8_literal("mentions"), utf8_literal("in"),
            utf8_literal("has"),       utf8_literal("pinned"),   utf8_literal("before"),
            utf8_literal("after"),     utf8_literal("on"),       utf8_literal("during"),
            utf8_literal("linkFrom"),  utf8_literal("fileType"), utf8_literal("fileName"),
            utf8_literal("authorType")};
        u32 kind = 0;
        while (filter && kind < 13 && !key_equal(key, keys[kind]))
            ++kind;
        const bool recognized = filter && kind < 13;
        const bool dated = kind >= 5 && kind <= 8;
        char decoded[kMessageSearchContentBytes];
        if (recognized && !quoted_value(value, {decoded, sizeof(decoded)}, &value)) {
            result.error = UiSearchQueryError::invalid_filter;
            result.error_offset = first;
            return result;
        }
        if (!recognized)
            filter = false;
        else if (kind == 0)
            ok = append_id(value, q.author_ids, &q.author_count, context, false);
        else if (kind == 1)
            ok = append_id(value, q.mention_ids, &q.mention_count, context, false);
        else if (kind == 2)
            ok = append_id(value, q.channel_ids, &q.channel_count, context, true);
        else if (kind == 3) {
            u32 included = q.has, excluded = q.has_excluded;
            ok = negatable(value, +[](Utf8View v) noexcept -> u32 { return has(v); }, &included, &excluded);
            q.has = static_cast<u16>(included);
            q.has_excluded = static_cast<u16>(excluded);
        } else if (kind == 4) {
            ok = equal(value, utf8_literal("true")) || equal(value, utf8_literal("false"));
            q.pinned = equal(value, utf8_literal("true")) ? MessageSearchPinned::pinned
                                                          : MessageSearchPinned::unpinned;
        } else if (kind == 9)
            ok = term(value, q.link_hostname, &q.hostname_bytes, kMessageSearchHostnameBytes, true,
                      false);
        else if (kind == 10)
            ok = term(value, q.attachment_extension, &q.extension_bytes,
                      kMessageSearchExtensionBytes, false, true);
        else if (kind == 11)
            ok = term(value, q.attachment_filename, &q.filename_bytes, kMessageSearchFilenameBytes,
                      false, false);
        else if (kind == 12) {
            u32 included = q.author_types, excluded = q.author_types_excluded;
            ok = negatable(
                value, +[](Utf8View v) noexcept -> u32 { return author_type(v); }, &included, &excluded);
            q.author_types = static_cast<u8>(included);
            q.author_types_excluded = static_cast<u8>(excluded);
        } else if (dated) {
            u64 day = 0;
            if (!date(value, &day)) {
                result.error = UiSearchQueryError::invalid_date;
                result.error_offset = first;
                return result;
            }
            u64 day_first = day, day_last = day + 86400000ull;
            if (context && context->date &&
                !context->date(context->context, value, &day_first, &day_last)) {
                result.error = UiSearchQueryError::invalid_date;
                result.error_offset = first;
                return result;
            }
            // Shifting an unchecked callback boundary can wrap and turn a
            // narrow calendar filter into an unrelated or unbounded search.
            if (day_first >= day_last || day_last > (~u64{0} >> 22) || (kind == 5 && !day_first)) {
                result.error = UiSearchQueryError::invalid_date;
                result.error_offset = first;
                return result;
            }
            const u64 start = day_first << 22, end = day_last << 22;
            if (kind == 5)
                q.max_id = q.max_id ? minimum(q.max_id, start) : start;
            else if (kind == 6)
                q.min_id = maximum(q.min_id, end);
            else {
                q.min_id = maximum(q.min_id, start);
                q.max_id = q.max_id ? minimum(q.max_id, end) : end;
            }
        }
        if (!ok) {
            result.error = UiSearchQueryError::invalid_filter;
            result.error_offset = first;
            return result;
        }
        if (!filter) {
            const u32 extra = token.bytes + (q.content_bytes ? 1 : 0);
            if (extra > sizeof(q.content) - q.content_bytes) {
                result.error = UiSearchQueryError::capacity;
                result.error_offset = first;
                return result;
            }
            if (q.content_bytes)
                q.content[q.content_bytes++] = ' ';
            CopyMemory(q.content + q.content_bytes, token.data, token.bytes);
            q.content_bytes += static_cast<u16>(token.bytes);
        }
    }
    if (q.min_id && q.max_id && q.min_id >= q.max_id) {
        result.error = UiSearchQueryError::invalid_date;
        return result;
    }
    return result;
}
} // namespace maicord
