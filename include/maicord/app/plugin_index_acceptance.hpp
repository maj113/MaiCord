#pragma once

#include "maicord/app/acceptance_record.hpp"
#include "maicord/app/plugin_index.hpp"
#include "maicord/app/plugin_providers.hpp"

namespace maicord {

// Anti-replay floors of plugin catalog indexes, plugin-manager\acceptance.bin
// (+ .lock and .new siblings, acceptance_record.hpp): per provider, the
// highest admitted sequence and the digest of the index admitted at it.
// Slot 0 belongs to the official provider; slots 1-8 to custom providers,
// keyed by provider id so a removed provider's floor never governs another.
inline constexpr u32 kPluginIndexAcceptanceSlots = kPluginProviderMaximum + 1;
inline constexpr u32 kPluginIndexAcceptanceVersion = 1;
inline constexpr u32 kPluginIndexAcceptanceSlotBytes = 56;
inline constexpr u32 kPluginIndexAcceptanceRecordBytes =
    32 + kPluginIndexAcceptanceSlots * kPluginIndexAcceptanceSlotBytes; // 536

struct PluginIndexFloor {
    u64 provider; // kPluginProviderOfficial or a providers.bin id
    u64 sequence;
    u8 index_sha256[32]; // PluginIndex::digest of the index admitted at sequence
    bool present;
};
struct PluginIndexAcceptanceState {
    PluginIndexFloor slots[kPluginIndexAcceptanceSlots];
};
struct PluginIndexAcceptanceResult {
    AcceptanceRecordError error;
    DWORD native_error;
    PluginIndexError index_error; // why the document was refused (error document)
    bool written;                 // the record was replaced
    constexpr bool ok() const noexcept { return error == AcceptanceRecordError::none; }
};
// Caller-owned, zero-initialized, noncopyable by contract, one thread. The
// exclusive .lock handle serializes cooperating processes for the whole
// verify/commit. Release explicitly.
struct PluginIndexAcceptanceTransaction {
    HANDLE lock;
    PluginIndexAcceptanceState state;
    wchar_t path[kAcceptanceRecordMaximumPathCharacters + 1];
    u32 path_characters;
    bool record_present;
};

// A present slot has a nonzero sequence and the right provider (slot 0
// official, others unique custom ids); an absent slot is all zero.
bool plugin_index_acceptance_valid(const PluginIndexAcceptanceState& state) noexcept;
// Exact 536-byte little-endian record: magic "MAIPIACC", version, size, slot
// count, zero reserved word, hash64 checksum (accidents, not authentication),
// then nine 56-byte slots. Encode/decode and load leave the output unchanged on failure.
PluginIndexAcceptanceResult plugin_index_acceptance_encode(const PluginIndexAcceptanceState& state, Slice<u8> record) noexcept;
PluginIndexAcceptanceResult plugin_index_acceptance_decode(ConstSlice<u8> record, PluginIndexAcceptanceState* state) noexcept;
PluginIndexAcceptanceResult plugin_index_acceptance_load(ConstSlice<wchar_t> trusted_absolute_path,
    PluginIndexAcceptanceState* state) noexcept;

// The provider's floor, or null.
const PluginIndexFloor* plugin_index_floor(const PluginIndexAcceptanceState& state, u64 provider) noexcept;
enum class PluginIndexFloorVerdict : u8 {
    advance, // no floor yet, or a higher sequence: admit and write
    same,    // the floor's sequence and digest: up to date, nothing to write
    replay,  // below the floor, or the floor's sequence with another digest: keep the cache
};
PluginIndexFloorVerdict plugin_index_floor_check(const PluginIndexAcceptanceState& state, u64 provider, u64 sequence,
    const u8* index_sha256) noexcept;

// As update_acceptance_begin: a missing record is `missing` unless the
// caller explicitly provisions first use. Corrupt records always fail closed.
PluginIndexAcceptanceResult plugin_index_acceptance_begin(ConstSlice<wchar_t> trusted_absolute_path,
    PluginIndexAcceptanceTransaction* transaction, bool allow_first_use = false) noexcept;
// Under the lock: checks the disk still holds the transaction's snapshot,
// verifies the index (plugin_index_verify with key; plugin_index_parse_unsigned
// when key is null, for a custom_unsigned policy), applies the floor rules for
// provider, checks the snapshot again and commits {sequence, digest} unless
// it is the same floor (written false). A provider without a floor takes a
// free slot (`capacity` when none). provider kPluginProviderOfficial goes
// with the official policy kind and nothing else does. The verified index is
// left in output (zeroed on failure); the transaction changes only on success.
PluginIndexAcceptanceResult plugin_index_acceptance_verify_commit(PluginIndexAcceptanceTransaction* transaction, u64 provider,
    Utf8View document, ConstSlice<u8> signature, const P256Key* trusted_key, KdfSha256Workspace& workspace,
    const PluginIndexPolicy& policy, Slice<u32> structural_scratch, PluginIndex* output) noexcept;
// Removes a provider's floor when the provider is removed (written false when it had none).
PluginIndexAcceptanceResult plugin_index_acceptance_forget(PluginIndexAcceptanceTransaction* transaction, u64 provider) noexcept;
PluginIndexAcceptanceResult plugin_index_acceptance_release(PluginIndexAcceptanceTransaction* transaction) noexcept;

} // namespace maicord
