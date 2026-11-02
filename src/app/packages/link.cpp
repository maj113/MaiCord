#include "maicord/app/packages/link.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

// The longest text any form can take: the scheme and path around a fully percent-encoded URL.
constexpr u32 kTextMaximum = kPackageUrlMaximumBytes + 64;

bool prefix(const char* text, u32 bytes, const char* lower) noexcept {
    u32 n = c_string_length(lower);
    if (bytes < n)
        return false;
    for (u32 i = 0; i < n; ++i)
        if (ascii_lower(text[i]) != lower[i])
            return false;
    return true;
}

// https://host[/target]: the host and the target ('/'-led, query allowed, no fragment).
struct Split {
    const char *host, *target;
    u32 host_bytes, target_bytes;
};
bool split_https(const char* url, u32 bytes, Split* out) noexcept {
    if (bytes > kPackageUrlMaximumBytes || !prefix(url, bytes, "https://"))
        return false;
    u32 i = 8;
    while (i < bytes && url[i] != '/' && url[i] != '?' && url[i] != '#')
        ++i;
    out->host = url + 8;
    out->host_bytes = i - 8;
    out->target = url + i;
    out->target_bytes = bytes - i;
    return out->host_bytes != 0;
}

bool location_of(const char* url, u32 bytes, UpdateLocation* location) noexcept {
    Split s{};
    if (!split_https(url, bytes, &s))
        return false;
    // A bare host or a leading query becomes "/" plus it.
    char target[kPackageUrlMaximumBytes + 2];
    u32 n = 0;
    if (!s.target_bytes || s.target[0] == '?')
        target[n++] = '/';
    copy_bytes(target + n, s.target, s.target_bytes);
    n += s.target_bytes;
    return update_provider_location({s.host, s.host_bytes}, {target, n}, location);
}

int hex(char c) noexcept {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The URL of a link. `param` is a maicord://install?url= value: its raw '&' would start another parameter, so
// it must be written %26. A '#' never belongs to a package URL.
bool set_url(const char* text, u32 bytes, bool param, PackageLink* link) noexcept {
    u32 n = 0;
    const bool plain = prefix(text, bytes, "https://");
    for (u32 i = 0; i < bytes; ++i) {
        char c = text[i];
        if (c == '&' && param)
            return false;
        if (c == '%' && !plain) {
            const int hi = i + 2 < bytes ? hex(text[i + 1]) : -1, lo = i + 2 < bytes ? hex(text[i + 2]) : -1;
            if (hi < 0 || lo < 0)
                return false;
            c = static_cast<char>(hi << 4 | lo);
            i += 2;
        }
        if (n == kPackageUrlMaximumBytes || static_cast<u8>(c) <= 0x20 || static_cast<u8>(c) >= 0x7f || c == '#')
            return false;
        link->url[n++] = c;
    }
    link->url[n] = 0;
    UpdateLocation checked;
    if (!location_of(link->url, n, &checked))
        return false;
    link->url_bytes = static_cast<u16>(n);
    link->type = PackageLinkType::url;
    return true;
}

} // namespace

bool package_slug_valid(Utf8View slug) noexcept {
    if (!slug.bytes || slug.bytes > kPackageSlugMaximumBytes || !slug.data)
        return false;
    for (u32 i = 0; i < slug.bytes; ++i) {
        const char c = slug.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && i)))
            return false;
    }
    return true;
}

bool package_link_parse(Utf8View text, PackageLink* link) noexcept {
    if (!link)
        return false;
    clear_bytes(link, sizeof(*link));
    if (!text.data || !text.bytes || text.bytes > kTextMaximum)
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (static_cast<u8>(text.data[i]) <= 0x20 || static_cast<u8>(text.data[i]) >= 0x7f || text.data[i] == '\\' ||
            text.data[i] == '"')
            return false;
    const char* t = text.data;
    u32 n = text.bytes;
    bool ok = false;
    if (prefix(t, n, "https://")) {
        ok = set_url(t, n, false, link);
    } else if (prefix(t, n, "maicord://install")) {
        t += 17;
        n -= 17;
        if (n > 5 && t[0] == '?' && prefix(t + 1, n - 1, "url=")) {
            ok = set_url(t + 5, n - 5, true, link);
        } else if (n && t[0] == '/') {
            // /<kind>/<slug>[/]
            u32 kind_end = 1;
            while (kind_end < n && t[kind_end] != '/')
                ++kind_end;
            u32 slug_end = n;
            if (slug_end > kind_end + 1 && t[slug_end - 1] == '/')
                --slug_end;
            const u32 kind_bytes = kind_end - 1;
            const char* slug = t + kind_end + 1;
            const u32 slug_bytes = kind_end < n ? slug_end - kind_end - 1 : 0;
            PackageLinkKind kind{};
            const bool known = kind_bytes == 6 && prefix(t + 1, 6, "plugin") ? (kind = PackageLinkKind::plugin, true)
                               : kind_bytes == 5 && prefix(t + 1, 5, "theme") ? (kind = PackageLinkKind::theme, true)
                               : kind_bytes == 6 && prefix(t + 1, 6, "effect") ? (kind = PackageLinkKind::effect, true)
                                                                              : false;
            if (known && package_slug_valid({slug, slug_bytes})) {
                link->type = PackageLinkType::catalog;
                link->kind = kind;
                link->slug_bytes = static_cast<u8>(slug_bytes);
                for (u32 i = 0; i < slug_bytes; ++i)
                    link->slug[i] = slug[i];
                ok = true;
            }
        }
    }
    if (!ok)
        clear_bytes(link, sizeof(*link));
    return ok;
}

bool package_link_parse_wide(ConstSlice<wchar_t> text, PackageLink* link) noexcept {
    char ascii[kTextMaximum];
    if (!link || !text.data || !text.count || text.count > kTextMaximum) {
        if (link)
            clear_bytes(link, sizeof(*link));
        return false;
    }
    for (u32 i = 0; i < text.count; ++i) {
        if (text[i] > 0x7e) {
            clear_bytes(link, sizeof(*link));
            return false;
        }
        ascii[i] = static_cast<char>(text[i]);
    }
    return package_link_parse({ascii, text.count}, link);
}

bool package_link_location(const PackageLink& link, UpdateLocation* location) noexcept {
    return link.type == PackageLinkType::url && location && location_of(link.url, link.url_bytes, location);
}

} // namespace maicord
