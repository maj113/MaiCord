#pragma once

#include "maicord/app/plugin_providers.hpp"

namespace maicord {

// Catalog metadata of installed plugins (plugin-manager\state.bin) and the
// one in-flight install/update/rollback/uninstall operation
// (plugin-manager\journal.bin). The installed manifests stay schema 3 for
// older clients, and plugins.bin trust keeps its format; everything the
// catalog adds lives here. Same record convention and path rules as
// providers.bin (plugin_providers.hpp).
inline constexpr u32 kPluginManagerStateMaximumEntries = 32;
inline constexpr u32 kPluginManagerStateVersion = 1;
inline constexpr u32 kPluginManagerStateEntryBytes = 160;
inline constexpr u32 kPluginManagerStateRecordBytes =
    32 + kPluginManagerStateMaximumEntries * kPluginManagerStateEntryBytes; // 5152
inline constexpr u32 kPluginManagerSlugCapacity = 40;
// Entry flags.
inline constexpr u32 kPluginStateRevokedMalicious = 1u << 0; // never reversed by cache loss or expiry
inline constexpr u32 kPluginStateRevokedBroken = 1u << 1;
inline constexpr u32 kPluginStateWithdrawn = 1u << 2;
inline constexpr u32 kPluginStateHeld = 1u << 3;
inline constexpr u32 kPluginStateAutoUpdateOff = 1u << 4;
inline constexpr u32 kPluginStateReviewPending = 1u << 5;
inline constexpr u32 kPluginStateIncompatibleHost = 1u << 6;
inline constexpr u32 kPluginStateKnownFlags = (1u << 7) - 1;

struct PluginManagerEntry {
    u64 owner;
    u64 provider; // kPluginProviderFile, kPluginProviderOfficial or a providers.bin id
    u32 flags;
    u32 max_host_abi; // catalog-only bound from the index; 0 none
    u64 installed_code;
    u8 installed_sha256[32];
    u64 previous_code; // the retained previous version; 0 none (its SHA-256 is then zero)
    u8 previous_sha256[32];
    u64 held_code;
    u64 admitted_sequence;
    char slug[kPluginManagerSlugCapacity]; // zero-padded; empty for a file install
};
struct PluginManagerState {
    u32 count;
    PluginManagerEntry entries[kPluginManagerStateMaximumEntries];
};
// Unique owners above 1, known flags, max_host_abi <= 0xFFFF, no previous
// digest without a previous version, slugs in the index grammar or empty,
// and zero unused entries.
bool plugin_manager_state_valid(const PluginManagerState& state) noexcept;
// Exact 5152-byte little-endian record: magic "MAIPSTAT", version, size,
// count, zero reserved word, hash64 checksum, then 32 entries of 160 bytes.
// Output unchanged on failure.
AcceptanceRecordResult plugin_manager_state_encode(const PluginManagerState& state, Slice<u8> record) noexcept;
AcceptanceRecordResult plugin_manager_state_decode(ConstSlice<u8> record, PluginManagerState* state) noexcept;
AcceptanceRecordResult plugin_manager_state_load(ConstSlice<wchar_t> path, PluginManagerState* state) noexcept;
AcceptanceRecordResult plugin_manager_state_save(ConstSlice<wchar_t> path, const PluginManagerState& state) noexcept;

// ---- where the plugin manager keeps its files ------------------------------------
// base is %LOCALAPPDATA%\MaiCord:
//   plugins\        the installer's managed directory (manifests, DLLs)
//   plugins.bin     trust (plugin_preferences.hpp)
//   plugin-data\    per-plugin stores (plugin-data\<owner16>)
//   plugin-manager\ providers.bin, acceptance.bin, state.bin, journal.bin,
//                   staging\, rollback\<owner16>.maicord-plugin.json, cache\<provider16>.index
inline constexpr u32 kPluginManagerPathCapacity = 1024;
// Room for the longest file plus the record writer's ".new"/".lock" siblings.
inline constexpr u32 kPluginManagerBaseMaximumCharacters = 900;
struct PluginManagerPaths {
    wchar_t base[kPluginManagerPathCapacity], plugins[kPluginManagerPathCapacity], trust[kPluginManagerPathCapacity],
        data[kPluginManagerPathCapacity], manager[kPluginManagerPathCapacity], acceptance[kPluginManagerPathCapacity],
        providers[kPluginManagerPathCapacity], state[kPluginManagerPathCapacity], journal[kPluginManagerPathCapacity],
        staging[kPluginManagerPathCapacity], rollback[kPluginManagerPathCapacity], cache[kPluginManagerPathCapacity];
    u32 base_count, plugins_count, trust_count, data_count, manager_count, acceptance_count, providers_count, state_count,
        journal_count, staging_count, rollback_count, cache_count;
};
// base: a trusted local drive path (acceptance_record.hpp) of at most 900
// characters without a trailing separator. paths is zeroed on failure.
bool plugin_manager_paths_build(ConstSlice<wchar_t> base, PluginManagerPaths* paths) noexcept;
// Creates plugin-manager and its staging, rollback and cache directories
// under an existing base. False with the Win32 error otherwise.
bool plugin_manager_paths_ensure(const PluginManagerPaths& paths, u32* native_error = nullptr) noexcept;
// <directory>\<16 lowercase hex digits of id><suffix>, NUL-terminated;
// characters written, 0 when it does not fit.
u32 plugin_manager_id_path(const wchar_t* directory, u32 directory_count, u64 id, const wchar_t* suffix,
    wchar_t (&output)[kPluginManagerPathCapacity]) noexcept;

enum class PluginJournalOperation : u8 { none, install, update, rollback, uninstall };
enum class PluginJournalPhase : u8 { none, intent, staged, committed };
inline constexpr u32 kPluginJournalVersion = 1;
inline constexpr u32 kPluginJournalRecordBytes = 32 + 128; // header and one 128-byte operation
// operation none is the cleared journal: every other field zero.
struct PluginJournal {
    PluginJournalOperation operation;
    PluginJournalPhase phase;
    bool enable_after;
    bool had_trust;
    u64 owner;
    u64 provider;
    u64 to_code;
    u64 approved_permissions; // MC_PERM_* mask
    u8 from_sha256[32];       // zero for a fresh install
    u8 to_sha256[32];
};
bool plugin_journal_valid(const PluginJournal& journal) noexcept;
// Exact 160-byte record: magic "MAIPJRNL", version, size, count (0 cleared,
// 1 in flight), zero reserved word, hash64 checksum, then the operation.
AcceptanceRecordResult plugin_journal_encode(const PluginJournal& journal, Slice<u8> record) noexcept;
AcceptanceRecordResult plugin_journal_decode(ConstSlice<u8> record, PluginJournal* journal) noexcept;
AcceptanceRecordResult plugin_journal_load(ConstSlice<wchar_t> path, PluginJournal* journal) noexcept;
AcceptanceRecordResult plugin_journal_save(ConstSlice<wchar_t> path, const PluginJournal& journal) noexcept;

// ---- recovery planner (pure) ------------------------------------------------------------
// What settling an interrupted operation does (plugin_manager_install.hpp).
enum class PluginRecoveryAction : u8 {
    none,              // no operation was journaled
    discard,           // not committed: staged files, the new DLL and staged manifests removed
    finish,            // committed: records settled (idempotent)
    restart_uninstall, // an uninstall whose manifest is still present: run again
    finish_uninstall,  // an uninstall whose manifest is gone: remaining files and records removed
    clear,             // the installed manifest matches neither side: journal cleared, files kept
};
// Pure plan from the journal and the owner's installed manifest (its SHA-256,
// or null when there is none). Intent always discards; staged discards while
// the manifest is still the old one (absent for a fresh install) and
// finishes once it is the new one; committed only finishes.
PluginRecoveryAction plugin_manager_recovery_plan(const PluginJournal& journal, const u8* manifest_sha256) noexcept;

} // namespace maicord
