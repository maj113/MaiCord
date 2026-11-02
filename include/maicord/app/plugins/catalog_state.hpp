#pragma once

#include "maicord/app/account/login/acceptance_record.hpp"
#include "maicord/app/plugins/catalog_index.hpp"

// The catalog's one state file (%LOCALAPPDATA%\MaiCord\catalog-state.bin): every installed plugin, theme and
// effect with its version and SHA-256, the permissions and hosts the user approved, whether it is on, automatic updates,
// the previous version kept for rollback, and the catalog's replay floor. Pure encode and decode plus the
// record IO of acceptance_record.hpp (atomic replace).
namespace maicord {

inline constexpr u32 kCatalogStateMaximumEntries = 128;
inline constexpr u32 kCatalogStateVersion = 2;
inline constexpr u32 kCatalogStateHeaderBytes = 80;
inline constexpr u32 kCatalogStateEntryBytes = 256;
inline constexpr u32 kCatalogStateRecordBytes =
    kCatalogStateHeaderBytes + kCatalogStateMaximumEntries * kCatalogStateEntryBytes;
inline constexpr u32 kCatalogStateHostSlots = 8;

// Entry flags.
inline constexpr u32 kCatalogEntryEnabled = 1u << 0;        // plugins: enable at startup
inline constexpr u32 kCatalogEntryAutoUpdateOff = 1u << 1;
inline constexpr u32 kCatalogEntryUnreviewed = 1u << 2;     // themes and effects from a link or file: never updated
inline constexpr u32 kCatalogEntrySideloaded = 1u << 3;     // a plugin DLL loaded in Developer Mode: never updated
inline constexpr u32 kCatalogEntryCrashed = 1u << 4;        // stays off until the user turns it on again
inline constexpr u32 kCatalogEntryOverran = 1u << 5;
inline constexpr u32 kCatalogEntryHung = 1u << 6;
inline constexpr u32 kCatalogEntryRevoked = 1u << 7;        // revoked as malicious or blocked: never loads again
inline constexpr u32 kCatalogEntryBroken = 1u << 8;         // a broken revocation turned it off once; a new build clears it
inline constexpr u32 kCatalogEntryKnownFlags = (1u << 9) - 1;
inline constexpr u32 kCatalogEntryOffFlags = kCatalogEntryCrashed | kCatalogEntryOverran | kCatalogEntryHung | kCatalogEntryRevoked;
// Header flags.
inline constexpr u32 kCatalogStateAutoUpdateOff = 1u << 0; // "Update official plugins automatically" is off
inline constexpr u32 kCatalogStateKnownFlags = 1;

struct CatalogStateEntry {
    CatalogKind kind;
    u8 slug_bytes, version_bytes, previous_version_bytes;
    u16 flags;
    // Plugins: bit i = the installed (previous) build's descriptor.network[i] is approved. The file is
    // content-addressed, so its host order never changes.
    u8 host_bits, previous_host_bits;
    u64 owner_id;        // plugins: the descriptor's owner
    u64 installed_unix;  // when this version was installed
    u64 approved_permissions;
    u64 approved_hosts[kCatalogStateHostSlots]; // catalog_state_host_hash of approved hosts; 0 = free
    u64 skipped_code;    // "Skip this version": no automatic update to this version code
    u8 sha256[32];
    u8 previous_sha256[32]; // zero: no rollback
    char slug[kCatalogSlugMaximumBytes];
    char version[kCatalogVersionTextMaximumBytes + 1];
    char previous_version[kCatalogVersionTextMaximumBytes + 1];
};
struct CatalogState {
    u32 count, flags;
    // The replay floor: the newest index accepted, and when the catalog was last checked.
    u64 sequence, checked_unix;
    u8 digest[32];
    CatalogStateEntry entries[kCatalogStateMaximumEntries];
};

inline Utf8View catalog_entry_slug(const CatalogStateEntry& e) noexcept { return {e.slug, e.slug_bytes}; }
inline Utf8View catalog_entry_version(const CatalogStateEntry& e) noexcept { return {e.version, e.version_bytes}; }
inline bool catalog_entry_has_previous(const CatalogStateEntry& e) noexcept { return e.previous_version_bytes != 0; }

// Known flags, kinds and slugs in the index grammar, unique (kind, slug) and plugin owners, nonzero
// digests, versions in the index grammar (empty for unreviewed themes), and zeroed unused entries.
bool catalog_state_valid(const CatalogState&) noexcept;
// Exact kCatalogStateRecordBytes little-endian record: magic "MAICATST", version, count, flags, an
// FNV-1a checksum of the rest, the floor, then 128 entries of 256 bytes. Encode leaves record unchanged on
// failure; decode zeroes *state on a failure after the header checks.
AcceptanceRecordResult catalog_state_encode(const CatalogState&, Slice<u8> record) noexcept;
AcceptanceRecordResult catalog_state_decode(ConstSlice<u8> record, CatalogState*) noexcept;
// A missing file is an empty state (ok); any other failure leaves *state empty.
AcceptanceRecordResult catalog_state_load(const wchar_t* path, CatalogState*) noexcept;
AcceptanceRecordResult catalog_state_save(const wchar_t* path, u32 path_characters, const CatalogState&) noexcept;

CatalogStateEntry* catalog_state_find(CatalogState&, CatalogKind, const Utf8View& slug) noexcept;
CatalogStateEntry* catalog_state_find_owner(CatalogState&, u64 owner_id) noexcept;
// A new zeroed entry for (kind, slug), or null when the state is full or the slug is invalid.
CatalogStateEntry* catalog_state_add(CatalogState&, CatalogKind, const Utf8View& slug) noexcept;
void catalog_state_remove(CatalogState&, CatalogStateEntry*) noexcept;
// Records version and sha256 as installed now, keeping the replaced ones for rollback when keep_previous.
void catalog_state_install(CatalogStateEntry*, const Utf8View& version, const u8* sha256, u64 now_unix, bool keep_previous) noexcept;
// Swaps the installed and previous versions (rollback).
void catalog_state_swap_previous(CatalogStateEntry*) noexcept;

// Never zero: a free host slot is.
u64 catalog_state_host_hash(Utf8View host) noexcept;
// Bit i set when hosts[i] was approved.
u32 catalog_state_approved_hosts(const CatalogStateEntry&, ConstSlice<Utf8View> hosts) noexcept;
// Adds permissions and the hosts of bits to the approval; hosts not listed now go first when the slots run out.
void catalog_state_approve(CatalogStateEntry*, u64 permissions, ConstSlice<Utf8View> hosts, u32 bits) noexcept;

} // namespace maicord
