#pragma once

#include "maicord/app/acceptance_record.hpp"
#include "maicord/app/plugin_index.hpp"
#include "maicord/app/update_fetch.hpp"

namespace maicord {

// Custom plugin catalog providers, %LOCALAPPDATA%\MaiCord\plugin-manager\providers.bin.
// The compiled official provider is never stored as an entry; the header
// keeps its two switches.
inline constexpr u32 kPluginProviderMaximum = 8;
inline constexpr u64 kPluginProviderFile = 0;     // state.bin: installed from a file
inline constexpr u64 kPluginProviderOfficial = 1; // the compiled official provider
inline constexpr u32 kPluginProviderHostCapacity = 64;
inline constexpr u32 kPluginProviderPathCapacity = 192;
inline constexpr u32 kPluginProviderNameCapacity = 64;
inline constexpr u32 kPluginProvidersVersion = 1;
inline constexpr u32 kPluginProvidersEntryBytes = 448;
inline constexpr u32 kPluginProvidersRecordBytes = 32 + kPluginProviderMaximum * kPluginProvidersEntryBytes; // 3616
// Entry flags.
inline constexpr u32 kPluginProviderSigned = 1u << 0;     // key and fingerprint hold the TOFU pin
inline constexpr u32 kPluginProviderEnabled = 1u << 1;
inline constexpr u32 kPluginProviderCheckDaily = 1u << 2;
inline constexpr u32 kPluginProviderKnownFlags = (1u << 3) - 1;
// Header switches for the official provider; zero keeps the defaults
// (checks and automatic updates on).
inline constexpr u32 kPluginOfficialChecksOff = 1u << 0;
inline constexpr u32 kPluginOfficialAutoUpdateOff = 1u << 1;
inline constexpr u32 kPluginOfficialKnownFlags = (1u << 2) - 1;

struct PluginProviderRecord {
    u64 id;    // first 8 bytes of SHA-256(domain || canonical base); never 0 or 1
    u64 added; // unix seconds
    u32 flags;
    u32 host_bytes, path_bytes, name_bytes;
    char host[kPluginProviderHostCapacity]; // lowercase, update_origin_host_valid
    char path[kPluginProviderPathCapacity]; // canonical base path: "/", or "/a/b/"
    u8 key[65];                             // signed: the pinned SEC1 point; otherwise zero
    u8 fingerprint[32];                     // signed: the shown fingerprint; otherwise zero
    char name[kPluginProviderNameCapacity]; // presentation copy of provider.name
};
struct PluginProviders {
    u32 count;
    u32 official_flags;
    PluginProviderRecord entries[kPluginProviderMaximum];
};

// Unique ids, known flags, valid hosts, base paths and names, the key and
// fingerprint present exactly when signed, and zero bytes past every text
// length and in unused entries.
bool plugin_providers_valid(const PluginProviders& providers) noexcept;
// Exact 3616-byte little-endian record: magic "MAIPPROV", version, size,
// count, official switches, hash64 checksum (accidents, not authentication),
// then eight 448-byte entries. Encode/decode leave the output unchanged on
// failure (`format`, `version`, `checksum`, `size`, `invalid_input`).
AcceptanceRecordResult plugin_providers_encode(const PluginProviders& providers, Slice<u8> record) noexcept;
AcceptanceRecordResult plugin_providers_decode(ConstSlice<u8> record, PluginProviders* providers) noexcept;
// Trusted local path rules of acceptance_record.hpp. load reports `missing`
// for an absent file; save replaces the file through file_persistence_write_atomic.
AcceptanceRecordResult plugin_providers_load(ConstSlice<wchar_t> path, PluginProviders* providers) noexcept;
AcceptanceRecordResult plugin_providers_save(ConstSlice<wchar_t> path, const PluginProviders& providers) noexcept;

// ---- provider addresses, key fingerprints and the add-provider probe ---------------
// A custom provider is the base URL "https://<host><path>" of
// <base>v1/index.json(.sig). id: first 8 bytes (big-endian) of
// SHA-256(kPluginProviderIdDomain || canonical base). Key fingerprint:
// SHA-256(kPluginProviderKeyDomain || 65-byte point); a share link's
// "#sha256=<64 hex>" pin is the full fingerprint.
inline constexpr Utf8View kPluginProviderIdDomain = utf8_literal("MaiCord plugin provider id v1\n");
inline constexpr Utf8View kPluginProviderKeyDomain = utf8_literal("MaiCord plugin provider key v1\n");
inline constexpr u32 kPluginProviderAddressMaximumBytes = 512; // before trimming
inline constexpr u32 kPluginProviderBaseMaximumBytes = 8 + kPluginProviderHostCapacity + kPluginProviderPathCapacity;
inline constexpr u32 kPluginProviderFingerprintTextBytes = 39; // "AB12 CD34 EF56 7890 1122 3344 5566 7788"

enum class PluginProviderAddressError : u8 {
    none,
    invalid_input,
    length,    // empty after trimming, longer than 512 bytes, or a path over 192 bytes with its trailing '/'
    character, // a space, control or non-ASCII byte inside the address
    scheme,    // not https://
    userinfo,  // user@host
    port,      // host:port (even :443)
    host,      // not a DNS name of 2+ LDH labels and at most 64 bytes, an IP literal or localhost
    path,      // an empty, "." or ".." segment, percent-encoding or a byte outside the path characters
    query,     // any '?'
    fragment,  // a fragment other than the #sha256= pin
    pin,       // #sha256= without exactly 64 hex digits
};
// The canonical form: lowercase host, a path of "/" or "/seg/.../" with the
// trailing '/' added when missing, no query, and the pin split off.
struct PluginProviderAddress {
    u64 id;
    u32 host_bytes, path_bytes;
    char host[kPluginProviderHostCapacity];
    char path[kPluginProviderPathCapacity];
    u8 pin[32]; // the #sha256= fingerprint when has_pin
    bool has_pin;
};
// Leading and trailing spaces and tabs are trimmed. Text, workspace and
// output must not overlap; output is unchanged on failure.
PluginProviderAddressError plugin_provider_address_parse(Utf8View text, KdfSha256Workspace& workspace,
    PluginProviderAddress* output) noexcept;
// "https://<host><path>"; bytes written, 0 when it does not fit.
u32 plugin_provider_address_text(const PluginProviderAddress& address, Slice<char> output) noexcept;
// <path>v1/index.json, or its .sig, on the provider origin.
bool plugin_provider_index_location(Utf8View host, Utf8View path, bool signature, UpdateLocation* output) noexcept;
// SHA-256(kPluginProviderKeyDomain || key) of a 65-byte SEC1 point.
bool plugin_provider_fingerprint(KdfSha256Workspace& workspace, const u8* key, u8* fingerprint) noexcept;
// The first 16 fingerprint bytes as 8 groups of 4 uppercase hex digits
// separated by spaces; bytes written (39), 0 when output is too small.
u32 plugin_provider_fingerprint_text(const u8* fingerprint, Slice<char> output) noexcept;

enum class PluginProviderProbeError : u8 {
    none,
    invalid_input,
    fetch,      // the index or signature request failed (fetch_error, status, retry_at_unix)
    unreadable, // the unverified document is not JSON or names a malformed provider.key
    unsigned_provider, // no provider.key and unsigned providers are not allowed (Developer Mode off)
    document,   // the index failed verification under its own key, or the strict parse (index_error)
    pin,        // the address carried a pin that is not this key's fingerprint, or the provider is unsigned
};
struct PluginProviderProbe {
    PluginProviderProbeError error;
    UpdateFetchError fetch_error;
    PluginIndexError index_error;
    u16 status;
    u32 native_error;
    u32 requests;          // update_fetch calls made: 1 for an unsigned provider, 2 when signed
    u64 retry_at_unix;
    bool signed_provider;  // the index names provider.key and verifies under it
    bool pin_matched;      // the address carried a pin and it equals the fingerprint
    u8 key[65];            // signed: the key to pin (TOFU), still unconfirmed by the user
    u8 fingerprint[32];
    u64 sequence;
    u32 plugin_count;
};
// The add-provider check: fetches the index (at most 256 KiB), reads
// provider.key unverified, fetches the .sig only when a key is named and
// verifies under that key (custom_unsigned when allowed and keyless). Nothing
// is admitted or written; work keeps the verified index for
// plugin_index_add_provider. A pin mismatch is an error, never a prompt.
PluginProviderProbe plugin_provider_probe(const PluginProviderAddress& address, const UpdateTransport& transport,
    KdfSha256Workspace& workspace, PluginIndexWork* work, bool allow_unsigned, u64 now_unix) noexcept;

enum class PluginProviderInsertError : u8 { none, invalid_input, capacity, duplicate };
// Appends the confirmed provider (enabled, checked daily, signed with the
// probe's key when it had one). providers is unchanged on failure.
PluginProviderInsertError plugin_providers_insert(PluginProviders* providers, const PluginProviderAddress& address,
    const PluginProviderProbe& probe, Utf8View name, u64 now_unix) noexcept;
// The entry of id, or null.
const PluginProviderRecord* plugin_providers_find(const PluginProviders& providers, u64 id) noexcept;

// Shared by the plugin manager's record files: validates path and ancestors,
// then reads exactly record.count bytes, or replaces the file atomically.
AcceptanceRecordResult plugin_record_read(ConstSlice<wchar_t> path, Slice<u8> record) noexcept;
AcceptanceRecordResult plugin_record_write(ConstSlice<wchar_t> path, ConstSlice<u8> record) noexcept;
// The plugins.bin header convention: hash64 over every byte but the
// checksum at offset 24.
u64 plugin_record_checksum(ConstSlice<u8> record) noexcept;

} // namespace maicord
