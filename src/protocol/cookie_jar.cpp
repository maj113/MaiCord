#include "maicord/protocol/cookie_jar.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/http1.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

struct JarLock {
    CookieJar* jar;
    explicit JarLock(CookieJar* locked) noexcept : jar(locked) {
        AcquireSRWLockExclusive(jar->lock.native());
    }
    ~JarLock() noexcept {
        ReleaseSRWLockExclusive(jar->lock.native());
    }
};

constexpr u32 kRecordHeaderBytes = 24;
constexpr u8 kKnownFlags = cookie_secure | cookie_http_only | cookie_host_only;

struct ParsedCookie {
    Utf8View name, value, domain, path;
    u64 expires_ms;
    u8 flags;
    CookieSameSite same_site;
    bool expired;
};

bool cookie_space(char c) noexcept {
    return c == ' ' || c == '\t';
}

Utf8View trim(Utf8View text) noexcept {
    while (text.bytes && cookie_space(text.data[0])) {
        ++text.data;
        --text.bytes;
    }
    while (text.bytes && cookie_space(text.data[text.bytes - 1]))
        --text.bytes;
    return text;
}

// Chromium rejects control characters other than tab in names and values.
bool cookie_octets(Utf8View text) noexcept {
    for (u32 i = 0; i < text.bytes; ++i)
        if (text.data[i] == ';')
            return false;
    return http_field_value_valid({reinterpret_cast<const u8*>(text.data), text.bytes});
}

bool equal_ignore_case(Utf8View text, const char* literal) noexcept {
    u32 i = 0;
    for (; literal[i]; ++i)
        if (i >= text.bytes || ascii_lower(text.data[i]) != literal[i])
            return false;
    return i == text.bytes;
}

bool begins_with(Utf8View text, const char* literal) noexcept {
    for (u32 i = 0; literal[i]; ++i)
        if (i >= text.bytes || text.data[i] != literal[i])
            return false;
    return true;
}

bool host_equal(Utf8View host, Utf8View domain) noexcept {
    return host.bytes == domain.bytes && ascii_equal_ignore_case(host, domain);
}

// host is domain or one of its subdomains.
bool domain_match(Utf8View host, Utf8View domain) noexcept {
    if (host_equal(host, domain))
        return true;
    return host.bytes > domain.bytes && host.data[host.bytes - domain.bytes - 1] == '.' &&
           ascii_equal_ignore_case({host.data + host.bytes - domain.bytes, domain.bytes}, domain);
}

Utf8View request_path(Utf8View target) noexcept {
    u32 end = 0;
    while (end < target.bytes && target.data[end] != '?' && target.data[end] != '#')
        ++end;
    return end && target.data[0] == '/' ? Utf8View{target.data, end} : utf8_literal("/");
}

// RFC 6265 5.1.4: the request path up to its last '/', or "/".
Utf8View default_path(Utf8View target) noexcept {
    const Utf8View path = request_path(target);
    u32 last = 0;
    for (u32 i = 0; i < path.bytes; ++i)
        if (path.data[i] == '/')
            last = i;
    return last ? Utf8View{path.data, last} : utf8_literal("/");
}

bool path_match(Utf8View request, Utf8View cookie) noexcept {
    if (cookie.bytes > request.bytes || !bytes_equal(request.data, cookie.data, cookie.bytes))
        return false;
    return cookie.bytes == request.bytes || cookie.data[cookie.bytes - 1] == '/' ||
           request.data[cookie.bytes] == '/';
}

// RFC 6265 5.1.1 cookie-date, as Chromium's ParseCookieExpirationTime reads
// it. Returns false for an unparseable or impossible date.
bool date_delimiter(u8 c) noexcept {
    return c == 0x09 || (c >= 0x20 && c <= 0x2F) || (c >= 0x3B && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
           (c >= 0x7B && c <= 0x7E);
}

u32 leading_digits(Utf8View token, u32 at, u32* value) noexcept {
    u32 count = 0;
    *value = 0;
    while (at + count < token.bytes && ascii_is_digit(token.data[at + count]) && count < 5)
        *value = *value * 10 + static_cast<u32>(token.data[at + count++] - '0');
    return count;
}

bool parse_date(Utf8View text, i64* unix_ms) noexcept {
    static constexpr char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    bool have_time = false, have_day = false, have_month = false, have_year = false;
    u32 hour = 0, minute = 0, second = 0, day = 0, month = 0, year = 0;
    u32 at = 0;
    while (at < text.bytes) {
        while (at < text.bytes && date_delimiter(static_cast<u8>(text.data[at])))
            ++at;
        u32 end = at;
        while (end < text.bytes && !date_delimiter(static_cast<u8>(text.data[end])))
            ++end;
        const Utf8View token{text.data + at, end - at};
        at = end;
        if (!token.bytes)
            continue;
        u32 value = 0;
        const u32 digits = leading_digits(token, 0, &value);
        if (!have_time && digits >= 1 && digits <= 2 && digits < token.bytes && token.data[digits] == ':') {
            u32 m = 0, s = 0;
            const u32 md = leading_digits(token, digits + 1, &m);
            const u32 after = digits + 1 + md;
            const u32 sd = md >= 1 && md <= 2 && after < token.bytes && token.data[after] == ':'
                               ? leading_digits(token, after + 1, &s)
                               : 0;
            if (sd >= 1 && sd <= 2) {
                have_time = true;
                hour = value;
                minute = m;
                second = s;
                continue;
            }
        }
        if (!have_day && digits >= 1 && digits <= 2) {
            have_day = true;
            day = value;
            continue;
        }
        if (!have_month && token.bytes >= 3) {
            for (u32 index = 0; index < 12 && !have_month; ++index)
                if (ascii_lower(token.data[0]) == months[index * 3] &&
                    ascii_lower(token.data[1]) == months[index * 3 + 1] &&
                    ascii_lower(token.data[2]) == months[index * 3 + 2]) {
                    have_month = true;
                    month = index + 1;
                }
            if (have_month)
                continue;
        }
        if (!have_year && digits >= 2 && digits <= 4) {
            have_year = true;
            year = value;
        }
    }
    if (have_year && year <= 69)
        year += 2000;
    else if (have_year && year >= 70 && year <= 99)
        year += 1900;
    static constexpr u8 month_days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (!have_time || !have_day || !have_month || !have_year || year < 1601 || year > 9999 || day < 1 ||
        day > month_days[month - 1] || (month == 2 && day == 29 && !leap) || hour > 23 || minute > 59 ||
        second > 59)
        return false;
    // Days from civil (proleptic Gregorian) to 1970-01-01.
    const i64 y = static_cast<i64>(year) - (month <= 2 ? 1 : 0);
    const i64 era = y / 400;
    const i64 yoe = y - era * 400;
    const i64 mp = (static_cast<i64>(month) + 9) % 12;
    const i64 doy = (153 * mp + 2) / 5 + day - 1;
    const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const i64 days = era * 146097 + doe - 719468;
    *unix_ms = ((days * 24 + hour) * 60 + minute) * 60'000 + static_cast<i64>(second) * 1000;
    return true;
}

bool parse_max_age(Utf8View text, i64* seconds) noexcept {
    bool negative = false;
    u32 at = 0;
    if (text.bytes && text.data[0] == '-') {
        negative = true;
        at = 1;
    }
    if (at >= text.bytes)
        return false;
    i64 value = 0;
    for (; at < text.bytes; ++at) {
        if (!ascii_is_digit(text.data[at]))
            return false;
        if (value < 400ll * 24 * 60 * 60)
            value = value * 10 + (text.data[at] - '0');
    }
    *seconds = negative ? -value : value;
    return true;
}

bool parse_set_cookie(Utf8View line, const CookieRequest& request, u64 now_ms, ParsedCookie* out) noexcept {
    u32 end = 0;
    while (end < line.bytes && line.data[end] != ';')
        ++end;
    const Utf8View pair{line.data, end};
    u32 equals = 0;
    while (equals < pair.bytes && pair.data[equals] != '=')
        ++equals;
    if (equals < pair.bytes) {
        out->name = trim({pair.data, equals});
        out->value = trim({pair.data + equals + 1, pair.bytes - equals - 1});
    } else {
        out->name = {};
        out->value = trim(pair);
    }
    if ((!out->name.bytes && !out->value.bytes) || !cookie_octets(out->name) || !cookie_octets(out->value) ||
        out->name.bytes + out->value.bytes > kCookieMaximumNameValueBytes)
        return false;
    Utf8View domain{}, path{}, expires{}, max_age{};
    bool have_domain = false;
    out->flags = 0;
    out->same_site = CookieSameSite::unspecified;
    u32 at = end;
    while (at < line.bytes) {
        ++at;
        u32 stop = at;
        while (stop < line.bytes && line.data[stop] != ';')
            ++stop;
        const Utf8View attribute{line.data + at, stop - at};
        at = stop;
        u32 split = 0;
        while (split < attribute.bytes && attribute.data[split] != '=')
            ++split;
        const Utf8View key = trim({attribute.data, split});
        const Utf8View value =
            split < attribute.bytes ? trim({attribute.data + split + 1, attribute.bytes - split - 1}) : Utf8View{};
        if (value.bytes > kCookieMaximumAttributeBytes)
            continue;
        if (equal_ignore_case(key, "domain")) {
            domain = value;
            have_domain = true;
        } else if (equal_ignore_case(key, "path")) {
            path = value;
        } else if (equal_ignore_case(key, "expires")) {
            expires = value;
        } else if (equal_ignore_case(key, "max-age")) {
            max_age = value;
        } else if (equal_ignore_case(key, "secure")) {
            out->flags |= cookie_secure;
        } else if (equal_ignore_case(key, "httponly")) {
            out->flags |= cookie_http_only;
        } else if (equal_ignore_case(key, "samesite")) {
            out->same_site = equal_ignore_case(value, "none")     ? CookieSameSite::none
                             : equal_ignore_case(value, "lax")    ? CookieSameSite::lax
                             : equal_ignore_case(value, "strict") ? CookieSameSite::strict
                                                                  : CookieSameSite::unspecified;
        }
    }
    // Domain: an empty value is ignored; a leading dot is dropped. A domain
    // without a dot is a public suffix unless it is the host itself.
    if (have_domain && domain.bytes && domain.data[0] == '.') {
        ++domain.data;
        --domain.bytes;
    }
    if (have_domain && domain.bytes) {
        bool dotted = false;
        for (u32 i = 0; i < domain.bytes; ++i)
            dotted |= domain.data[i] == '.';
        if (domain.bytes > kCookieMaximumDomainBytes || !domain_match(request.host, domain) ||
            (!dotted && !host_equal(request.host, domain)))
            return false;
        if (!dotted)
            out->flags |= cookie_host_only;
        out->domain = domain;
    } else {
        if (request.host.bytes > kCookieMaximumDomainBytes)
            return false;
        out->flags |= cookie_host_only;
        out->domain = request.host;
    }
    out->path = path.bytes && path.data[0] == '/' ? path : default_path(request.target);
    if (out->path.bytes > kCookieMaximumPathBytes)
        return false;
    // Max-Age wins over Expires; either may delete the cookie.
    out->expires_ms = 0;
    out->expired = false;
    i64 seconds = 0;
    i64 date = 0;
    if (max_age.bytes && parse_max_age(max_age, &seconds)) {
        if (seconds <= 0)
            out->expired = true;
        else
            out->expires_ms = now_ms + static_cast<u64>(seconds) * 1000;
    } else if (expires.bytes && parse_date(expires, &date)) {
        if (date <= static_cast<i64>(now_ms))
            out->expired = true;
        else
            out->expires_ms = static_cast<u64>(date);
    }
    if (out->expires_ms > now_ms + kCookieMaximumLifetimeMs)
        out->expires_ms = now_ms + kCookieMaximumLifetimeMs;
    // SameSite=None requires Secure; prefixed names their documented shape.
    const bool secure = (out->flags & cookie_secure) != 0;
    if ((out->same_site == CookieSameSite::none && !secure) ||
        (begins_with(out->name, "__Secure-") && !secure) ||
        (begins_with(out->name, "__Host-") &&
         (!secure || !(out->flags & cookie_host_only) || out->path.bytes != 1 || have_domain)))
        return false;
    // A cross-site response sets only cookies that may be sent cross-site.
    return request.same_site || out->same_site == CookieSameSite::none;
}

Utf8View entry_text(const CookieJar& jar, const CookieEntry& entry, u32 part) noexcept {
    u32 offset = entry.offset;
    const u32 sizes[4] = {entry.name_bytes, entry.value_bytes, entry.domain_bytes, entry.path_bytes};
    for (u32 i = 0; i < part; ++i)
        offset += sizes[i];
    return {jar.text + offset, sizes[part]};
}

u32 entry_bytes(const CookieEntry& entry) noexcept {
    return static_cast<u32>(entry.name_bytes) + entry.value_bytes + entry.domain_bytes + entry.path_bytes;
}

void remove_entry(CookieJar* jar, u32 index) noexcept {
    const CookieEntry removed = jar->entries[index];
    const u32 bytes = entry_bytes(removed);
    if (removed.expires_ms)
        ++jar->persistent_revision;
    secure_clear_bytes(jar->text + removed.offset, bytes);
    copy_bytes(jar->text + removed.offset, jar->text + removed.offset + bytes,
               jar->text_bytes - removed.offset - bytes);
    jar->text_bytes -= bytes;
    secure_clear_bytes(jar->text + jar->text_bytes, bytes);
    for (u32 i = index; i + 1 < jar->count; ++i)
        jar->entries[i] = jar->entries[i + 1];
    --jar->count;
    clear_bytes(&jar->entries[jar->count], sizeof(CookieEntry));
    for (u32 i = 0; i < jar->count; ++i)
        if (jar->entries[i].offset > removed.offset)
            jar->entries[i].offset = static_cast<u16>(jar->entries[i].offset - bytes);
}

void drop_expired(CookieJar* jar, u64 now_ms) noexcept {
    for (u32 i = jar->count; i-- > 0;)
        if (jar->entries[i].expires_ms && jar->entries[i].expires_ms <= now_ms)
            remove_entry(jar, i);
}

bool same_identity(const CookieJar& jar, const CookieEntry& entry, const ParsedCookie& cookie) noexcept {
    const Utf8View name = entry_text(jar, entry, 0);
    const Utf8View domain = entry_text(jar, entry, 2);
    const Utf8View path = entry_text(jar, entry, 3);
    return ((entry.flags ^ cookie.flags) & cookie_host_only) == 0 && name.bytes == cookie.name.bytes &&
           bytes_equal(name.data, cookie.name.data, name.bytes) && host_equal(domain, cookie.domain) &&
           path.bytes == cookie.path.bytes && bytes_equal(path.data, cookie.path.data, path.bytes);
}

// Stores a validated cookie, replacing one with its identity (keeping that
// one's creation time) and evicting the least recently used to make room.
bool store(CookieJar* jar, const ParsedCookie& cookie, u64 created_ms, u64 now_ms) noexcept {
    const u32 bytes = cookie.name.bytes + cookie.value.bytes + cookie.domain.bytes + cookie.path.bytes;
    if (bytes > kCookieJarTextBytes)
        return false;
    for (u32 i = 0; i < jar->count; ++i)
        if (same_identity(*jar, jar->entries[i], cookie)) {
            created_ms = jar->entries[i].created_ms;
            remove_entry(jar, i);
            break;
        }
    if (cookie.expired)
        return false;
    drop_expired(jar, now_ms);
    while (jar->count == kCookieJarCapacity || kCookieJarTextBytes - jar->text_bytes < bytes) {
        u32 oldest = 0;
        for (u32 i = 1; i < jar->count; ++i)
            if (jar->entries[i].accessed_ms < jar->entries[oldest].accessed_ms)
                oldest = i;
        remove_entry(jar, oldest);
    }
    CookieEntry& entry = jar->entries[jar->count++];
    entry.expires_ms = cookie.expires_ms;
    entry.created_ms = created_ms;
    entry.accessed_ms = now_ms;
    entry.offset = static_cast<u16>(jar->text_bytes);
    entry.name_bytes = static_cast<u16>(cookie.name.bytes);
    entry.value_bytes = static_cast<u16>(cookie.value.bytes);
    entry.domain_bytes = static_cast<u8>(cookie.domain.bytes);
    entry.path_bytes = static_cast<u8>(cookie.path.bytes);
    entry.flags = cookie.flags;
    entry.same_site = cookie.same_site;
    char* cursor = jar->text + jar->text_bytes;
    copy_bytes(cursor, cookie.name.data, cookie.name.bytes);
    cursor += cookie.name.bytes;
    copy_bytes(cursor, cookie.value.data, cookie.value.bytes);
    cursor += cookie.value.bytes;
    for (u32 i = 0; i < cookie.domain.bytes; ++i)
        *cursor++ = ascii_lower(cookie.domain.data[i]);
    copy_bytes(cursor, cookie.path.data, cookie.path.bytes);
    jar->text_bytes += bytes;
    if (entry.expires_ms)
        ++jar->persistent_revision;
    return true;
}

bool set_locked(CookieJar* jar, const CookieRequest& request, Utf8View line, u64 now_ms) noexcept {
    ParsedCookie cookie{};
    if (!request.host.data || !request.host.bytes || !line.data ||
        !parse_set_cookie(line, request, now_ms, &cookie))
        return false;
    return store(jar, cookie, now_ms, now_ms);
}

struct StoreContext {
    CookieJar* jar;
    const CookieRequest* request;
    u64 now_ms;
    u32 stored;
};

bool visit_set_cookie(void* opaque, HttpField field) noexcept {
    auto* context = static_cast<StoreContext*>(opaque);
    if (http_ascii_equal_case_insensitive(field.name, "set-cookie") &&
        set_locked(context->jar, *context->request,
                   {reinterpret_cast<const char*>(field.value.data), field.value.count}, context->now_ms))
        ++context->stored;
    return true;
}

} // namespace

bool cookie_jar_set(CookieJar* jar, const CookieRequest& request, Utf8View set_cookie, u64 now_ms) noexcept {
    if (!jar)
        return false;
    JarLock guard(jar);
    return set_locked(jar, request, set_cookie, now_ms);
}

u32 cookie_jar_store_response(CookieJar* jar, const CookieRequest& request, ConstSlice<u8> head,
                              u64 now_ms) noexcept {
    if (!jar || !head.data || !head.count)
        return 0;
    JarLock guard(jar);
    StoreContext context{jar, &request, now_ms, 0};
    http_parse_response_head(head, head.count, 512, visit_set_cookie, &context);
    return context.stored;
}

u32 cookie_jar_header(CookieJar* jar, const CookieRequest& request, u64 now_ms, Slice<u8> output) noexcept {
    if (!jar || !output.data || output.count < kCookieHeaderMaximumBytes || !request.host.bytes)
        return 0;
    JarLock guard(jar);
    drop_expired(jar, now_ms);
    const Utf8View path = request_path(request.target);
    u8 order[kCookieJarCapacity];
    u32 matched = 0;
    for (u32 i = 0; i < jar->count; ++i) {
        const CookieEntry& entry = jar->entries[i];
        const Utf8View domain = entry_text(*jar, entry, 2);
        if (((entry.flags & cookie_host_only) ? host_equal(request.host, domain)
                                               : domain_match(request.host, domain)) &&
            path_match(path, entry_text(*jar, entry, 3)) &&
            (request.same_site || entry.same_site == CookieSameSite::none))
            order[matched++] = static_cast<u8>(i);
    }
    // Chromium's CookieSorter: longer paths first, then earlier creation.
    for (u32 i = 1; i < matched; ++i) {
        const u8 index = order[i];
        u32 j = i;
        for (; j > 0; --j) {
            const CookieEntry& previous = jar->entries[order[j - 1]];
            const CookieEntry& current = jar->entries[index];
            if (previous.path_bytes > current.path_bytes ||
                (previous.path_bytes == current.path_bytes && previous.created_ms <= current.created_ms))
                break;
            order[j] = order[j - 1];
        }
        order[j] = index;
    }
    u32 used = 0;
    for (u32 i = 0; i < matched; ++i) {
        CookieEntry& entry = jar->entries[order[i]];
        entry.accessed_ms = now_ms;
        if (i) {
            output[used++] = ';';
            output[used++] = ' ';
        }
        const Utf8View name = entry_text(*jar, entry, 0);
        const Utf8View value = entry_text(*jar, entry, 1);
        copy_bytes(output.data + used, name.data, name.bytes);
        used += name.bytes;
        if (name.bytes)
            output[used++] = '=';
        copy_bytes(output.data + used, value.data, value.bytes);
        used += value.bytes;
    }
    return used;
}

void cookie_jar_clear(CookieJar* jar) noexcept {
    if (!jar)
        return;
    JarLock guard(jar);
    const bool persistent = [&] {
        for (u32 i = 0; i < jar->count; ++i)
            if (jar->entries[i].expires_ms)
                return true;
        return false;
    }();
    secure_clear_bytes(jar->entries, sizeof(jar->entries));
    secure_clear_bytes(jar->text, sizeof(jar->text));
    jar->count = 0;
    jar->text_bytes = 0;
    if (persistent)
        ++jar->persistent_revision;
}

u32 cookie_jar_encode_persistent(CookieJar* jar, u64 now_ms, Slice<u8> output) noexcept {
    if (!jar || !output.data || output.count < kCookieJarPersistentMaximumBytes)
        return 0;
    JarLock guard(jar);
    drop_expired(jar, now_ms);
    u32 used = 4;
    u32 count = 0;
    for (u32 i = 0; i < jar->count; ++i) {
        const CookieEntry& entry = jar->entries[i];
        if (!entry.expires_ms)
            continue;
        u8* record = output.data + used;
        byte_store_le64(record, entry.expires_ms);
        byte_store_le64(record + 8, entry.created_ms);
        record[16] = entry.flags;
        record[17] = static_cast<u8>(entry.same_site);
        byte_store_le16(record + 18, entry.name_bytes);
        byte_store_le16(record + 20, entry.value_bytes);
        record[22] = entry.domain_bytes;
        record[23] = entry.path_bytes;
        copy_bytes(record + kRecordHeaderBytes, jar->text + entry.offset, entry_bytes(entry));
        used += kRecordHeaderBytes + entry_bytes(entry);
        ++count;
    }
    byte_store_le32(output.data, count);
    return used;
}

bool cookie_jar_decode_persistent(CookieJar* jar, ConstSlice<u8> input, u64 now_ms) noexcept {
    if (!jar)
        return false;
    JarLock guard(jar);
    const u32 revision = jar->persistent_revision;
    secure_clear_bytes(jar->entries, sizeof(jar->entries));
    secure_clear_bytes(jar->text, sizeof(jar->text));
    jar->count = 0;
    jar->text_bytes = 0;
    if (!input.data || input.count < 4 || input.count > kCookieJarPersistentMaximumBytes)
        return false;
    const u32 count = byte_load_le32(input.data);
    u32 at = 4;
    bool valid = count <= kCookieJarCapacity;
    for (u32 i = 0; valid && i < count; ++i) {
        valid = input.count - at >= kRecordHeaderBytes;
        if (!valid)
            break;
        const u8* record = input.data + at;
        ParsedCookie cookie{};
        cookie.expires_ms = byte_load_le64(record);
        const u64 created_ms = byte_load_le64(record + 8);
        cookie.flags = record[16];
        const u8 same_site = record[17];
        const u32 name_bytes = byte_load_le16(record + 18);
        const u32 value_bytes = byte_load_le16(record + 20);
        const u32 domain_bytes = record[22];
        const u32 path_bytes = record[23];
        const u32 bytes = name_bytes + value_bytes + domain_bytes + path_bytes;
        at += kRecordHeaderBytes;
        valid = input.count - at >= bytes && cookie.expires_ms && (cookie.flags & ~kKnownFlags) == 0 &&
                same_site <= static_cast<u8>(CookieSameSite::strict) && name_bytes + value_bytes != 0 &&
                name_bytes + value_bytes <= kCookieMaximumNameValueBytes && domain_bytes &&
                domain_bytes <= kCookieMaximumDomainBytes && path_bytes;
        if (!valid)
            break;
        const char* text = reinterpret_cast<const char*>(input.data + at);
        cookie.name = {text, name_bytes};
        cookie.value = {text + name_bytes, value_bytes};
        cookie.domain = {text + name_bytes + value_bytes, domain_bytes};
        cookie.path = {text + name_bytes + value_bytes + domain_bytes, path_bytes};
        cookie.same_site = static_cast<CookieSameSite>(same_site);
        at += bytes;
        valid = cookie_octets(cookie.name) && cookie_octets(cookie.value) && cookie.path.data[0] == '/' &&
                ascii_graphic_string(cookie.domain, kCookieMaximumDomainBytes, true) &&
                ascii_graphic_string(cookie.path, kCookieMaximumPathBytes, true);
        if (valid && cookie.expires_ms > now_ms)
            store(jar, cookie, created_ms, now_ms);
    }
    valid = valid && at == input.count;
    if (!valid) {
        secure_clear_bytes(jar->entries, sizeof(jar->entries));
        secure_clear_bytes(jar->text, sizeof(jar->text));
        jar->count = 0;
        jar->text_bytes = 0;
    }
    jar->persistent_revision = revision + 1;
    return valid;
}

bool cookie_jar_has_persistent(CookieJar* jar, u32* revision) noexcept {
    if (!jar)
        return false;
    JarLock guard(jar);
    if (revision)
        *revision = jar->persistent_revision;
    for (u32 i = 0; i < jar->count; ++i)
        if (jar->entries[i].expires_ms)
            return true;
    return false;
}

} // namespace maicord
