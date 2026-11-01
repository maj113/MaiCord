#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {

// The official client's rules for leaving the client through a link. A link
// whose text is its own URL, outside a DM with someone who is not a friend,
// opens directly. Masked links, embed and button links open directly only on a
// trusted host: the service's own (discord.com, discordapp.com,
// discordmerch.com and their subdomains, discord.gg, discord.gift,
// discord.new, the media proxy) and Spotify, or a host the user trusted from
// the "Leaving Discord" warning. Other protocols (discord://) need a trusted
// protocol.
inline constexpr u32 kLinkTrustHostCapacity = 128;
inline constexpr u32 kLinkTrustHostBytes = 253;
inline constexpr u32 kLinkTrustProtocolCapacity = 16;
inline constexpr u32 kLinkTrustProtocolBytes = 32;
// Magic, version, host count, protocol count, u8 length + bytes per entry,
// then a 32-bit FNV-1a checksum.
inline constexpr u32 kLinkTrustFileBytes = 8 + kLinkTrustHostCapacity * (1 + kLinkTrustHostBytes) +
                                          kLinkTrustProtocolCapacity * (1 + kLinkTrustProtocolBytes) + 4;

// The user's trusted hosts (exact, lowercase, punycode as typed in the URL)
// and protocols ("steam:"). The official client offers no way to remove them.
struct LinkTrustStore {
    char hosts[kLinkTrustHostCapacity][kLinkTrustHostBytes];
    u8 host_bytes[kLinkTrustHostCapacity];
    char protocols[kLinkTrustProtocolCapacity][kLinkTrustProtocolBytes];
    u8 protocol_bytes[kLinkTrustProtocolCapacity];
    u16 host_count;
    u8 protocol_count;
    u32 revision;
};

// A parsed URL target. The host is lowercase without user info or port, the
// protocol is the lowercase scheme with its colon.
struct LinkTarget {
    char host[kLinkTrustHostBytes];
    char protocol[kLinkTrustProtocolBytes];
    u8 host_bytes;
    u8 protocol_bytes;
    bool web; // http: or https:
};
bool link_target_parse(Utf8View url, LinkTarget* target) noexcept;
constexpr Utf8View link_target_host(const LinkTarget& target) noexcept {
    return {target.host, target.host_bytes};
}
constexpr Utf8View link_target_protocol(const LinkTarget& target) noexcept {
    return {target.protocol, target.protocol_bytes};
}

bool link_trust_builtin_host(Utf8View host) noexcept;
bool link_trust_has_host(const LinkTrustStore& store, Utf8View host) noexcept;
bool link_trust_has_protocol(const LinkTrustStore& store, Utf8View protocol) noexcept;
// Adds a lowercase host or protocol; full stores keep their entries.
bool link_trust_add_host(LinkTrustStore* store, Utf8View host) noexcept;
bool link_trust_add_protocol(LinkTrustStore* store, Utf8View protocol) noexcept;

enum class LinkOpenDecision : u8 {
    open,
    warn_website,  // "This link is taking you to the following website"
    warn_protocol, // "This link is trying to open an application on your device"
    reject,        // not a URL the official client would open
};
// trusted_text: the link's text is its URL and it is not in a DM with a
// non-friend (the official client's markdown link rule).
LinkOpenDecision link_open_decision(const LinkTrustStore* store, Utf8View url, bool trusted_text) noexcept;
// The URL as the warning shows it: user name and password removed.
u32 link_display_url(Utf8View url, Slice<char> output) noexcept;

u32 link_trust_encode(const LinkTrustStore& store, Slice<u8> output) noexcept;
// Strict: malformed, duplicated or checksum-failing records are rejected whole.
bool link_trust_decode(ConstSlice<u8> input, LinkTrustStore* store) noexcept;
// %LOCALAPPDATA%\MaiCord\accounts\links-<profile>.trust; the directory is never created here.
bool link_trust_path(u64 profile_id, wchar_t* path, u32 capacity) noexcept;
// A missing file loads an empty store; a corrupt file loads an empty store and returns false.
bool link_trust_load_file(const wchar_t* path, LinkTrustStore* store) noexcept;
bool link_trust_save_file(const wchar_t* path, const LinkTrustStore& store) noexcept;

} // namespace maicord
