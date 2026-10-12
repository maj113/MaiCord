#pragma once

#include "maicord/app/acceptance_record.hpp"

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

// Shared by the plugin manager's record files: validates path and ancestors,
// then reads exactly record.count bytes, or replaces the file atomically.
AcceptanceRecordResult plugin_record_read(ConstSlice<wchar_t> path, Slice<u8> record) noexcept;
AcceptanceRecordResult plugin_record_write(ConstSlice<wchar_t> path, ConstSlice<u8> record) noexcept;
// The plugins.bin header convention: hash64 over every byte but the
// checksum at offset 24.
u64 plugin_record_checksum(ConstSlice<u8> record) noexcept;

} // namespace maicord
