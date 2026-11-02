#pragma once
#include "maicord/app/update/fetch.hpp"
#include "maicord/core/slice.hpp"

// Package links (plugin design 16.2). Every byte is untrusted: the grammar is closed and anything else is dropped.
//   maicord://install/<kind>/<slug>     kind = plugin | theme | effect; slug = [a-z0-9][a-z0-9-]{0,39}
//   maicord://install?url=<https URL>   the one parameter; raw or percent-encoded; no fragment
//   https://host/path                   a direct package URL (Install from link, drag of a link)
// A URL is https only, at most 2,048 bytes of printable ASCII, a DNS host name without userinfo or port.
namespace maicord {

inline constexpr u32 kPackageUrlMaximumBytes = 2048;
inline constexpr u32 kPackageSlugMaximumBytes = 40;

// The same order as the catalog index's kinds.
enum class PackageLinkKind : u8 { plugin, theme, effect };
enum class PackageLinkType : u8 { none, catalog, url };

struct PackageLink {
    PackageLinkType type;
    PackageLinkKind kind;
    u8 slug_bytes;
    u16 url_bytes;
    char slug[kPackageSlugMaximumBytes + 1];
    char url[kPackageUrlMaximumBytes + 1];
};

// False (and *link zeroed) for anything outside the grammar. The wide form accepts ASCII only.
bool package_link_parse(Utf8View text, PackageLink* link) noexcept;
bool package_link_parse_wide(ConstSlice<wchar_t> text, PackageLink* link) noexcept;
// The fetch location of a `url` link: a provider origin, so a redirect never leaves the host.
bool package_link_location(const PackageLink& link, UpdateLocation* location) noexcept;
bool package_slug_valid(Utf8View slug) noexcept;

} // namespace maicord
