#pragma once

#include "maicord/app/update_changelog.hpp"
#include "maicord/app/update_fetch.hpp"
#include "maicord/app/update_package.hpp"

namespace maicord {

// Application updater: GitHub fetch -> signed manifest -> durable admission
// -> package download -> verified executable staged beside the running one,
// swapped in when the user chooses to install it. Trust comes only from the
// compiled publisher key.
inline constexpr u32 kUpdatePathCapacity = 512;

struct UpdateServiceConfig {
    char repository[kUpdateRepositoryMaximumBytes + 1];
    u32 repository_bytes;
    u8 public_key[65];
    u64 own_version;
    u64 maximum_artifact_bytes;
    UpdateChannel default_channel;
    bool configured; // false: compiled in but disabled (no repository or key)
};
// Empty repository and key give a valid unconfigured config; a partial or
// malformed pair, or an invalid version, is refused.
bool update_service_config(Utf8View repository, Utf8View public_key_hex, u64 own_version, UpdateServiceConfig*) noexcept;

// <base>\channel, interval and work\ (acceptance record, package download),
// and beside the executable its stage <executable>.update and the executable a
// swap replaced, <executable>.old. The stage shares the executable's
// directory, so the swap's renames stay on one volume.
struct UpdatePaths {
    wchar_t base[kUpdatePathCapacity], work[kUpdatePathCapacity];
    wchar_t channel[kUpdatePathCapacity], interval[kUpdatePathCapacity];
    wchar_t acceptance[kUpdatePathCapacity], download[kUpdatePathCapacity];
    wchar_t executable[kUpdatePathCapacity], staged[kUpdatePathCapacity], replaced[kUpdatePathCapacity];
    u32 work_count, acceptance_count, download_count, executable_count, staged_count;
};
// base and executable are absolute drive paths without a trailing separator.
bool update_paths_build(ConstSlice<wchar_t> base, ConstSlice<wchar_t> executable, UpdatePaths*) noexcept;
// %LOCALAPPDATA%\MaiCord\updates and this process's executable.
bool update_paths_default(UpdatePaths*) noexcept;
// Startup: deletes <executable>.old and a stage an earlier session left
// behind. A stage is only ever swapped in by the process that verified it; a
// package that session kept is verified and staged again by the next check.
void update_paths_cleanup(const UpdatePaths&) noexcept;

enum class UpdateServiceError : u8 {
    none, configuration, storage, transport, rate_limited, not_found, redirect, oversize,
    size, signature, expired, manifest, acceptance, stage, install, state, unavailable,
};
Utf8View update_service_error_text(UpdateServiceError) noexcept;

enum class UpdateCheckOutcome : u8 { failed, up_to_date, ready };
struct UpdateCheckResult {
    UpdateServiceError error;
    UpdateCheckOutcome outcome;
    UpdateManifestError manifest_error;
    UpdateFetchError fetch_error;
    u64 version;
    u64 retry_at_unix;
    u32 native_error;
};
struct UpdateProgress { volatile LONG64 received, total; };
// Worker-thread buffers for the optional channel changelog.
struct UpdateChangelogDownload {
    u32 document_bytes;
    char document[kUpdateChangelogMaximumBytes];
    u8 signature[64];
    u32 scratch[kUpdateChangelogStructurals];
    UpdateChangelog changelog;
};
// Called on the worker once a newer signed offer is verified, before any
// durable admission or download. changelog is null when none was published,
// fetched or verified for exactly this version and channel; both are borrowed
// for the call.
struct UpdateOfferObserver {
    void* context;
    void (*offered)(void* context, const UpdateManifest&, const UpdateChangelog* changelog) noexcept;
};

// Zero-initialize; noncopyable; large, so allocate it. One worker thread.
struct UpdateService {
    UpdateServiceConfig config;
    UpdatePaths paths;
    P256Key key;
    KdfSha256Workspace workspace;
    UpdatePreparationContext preparation;
    // The executable this service verified and staged at paths.staged; held
    // until the swap so nothing can write or delete it meanwhile.
    UpdatePackage staged;
    UpdateProgress progress;
    // Optional: a changelog buffer (null skips the changelog) and an observer.
    UpdateChangelogDownload* changelog;
    UpdateOfferObserver observer;
    HANDLE download;
    u32 document_bytes;
    char document[kUpdateManifestMaximumBytes];
    u8 signature[64];
    u32 scratch[256];
    bool initialized;
};
UpdateServiceError update_service_initialize(UpdateService*, const UpdateServiceConfig&, const UpdatePaths&) noexcept;
// Discards a stage that was not swapped in and keeps the verified package it
// came from for the next session's check.
void update_service_release(UpdateService*) noexcept;
// A newer signed offer is admitted, downloaded, verified and staged; ready
// reports the staged version. A stage from an earlier check that is at least
// the offered version stays and is ready without downloading again, and a
// package an earlier session kept is staged again, without downloading, while
// it still matches the signed offer. An offer no newer than the installed
// version deletes a kept package.
UpdateCheckResult update_service_check(UpdateService*, const UpdateTransport&, UpdateChannel, u64 now_unix) noexcept;

enum class UpdateSwapError : u8 { none, nothing_staged, release, replace, install };
struct UpdateSwapResult {
    UpdateSwapError error;
    DWORD native_error;
    // install only: the error restoring the executable when that failed too.
    DWORD restore_error;
    constexpr bool ok() const noexcept { return error == UpdateSwapError::none; }
};
// The swap: <executable> -> <executable>.old (replacing a leftover one), then
// <staged> -> <executable>, both MOVEFILE_WRITE_THROUGH. When the second move
// fails, the first is undone (or, failing that, <executable>.old is copied
// back), so the executable path names a MaiCord.exe on every path. Windows
// allows renaming the running image; its file is deleted at the next start.
UpdateSwapResult update_swap(const UpdatePaths&) noexcept;
// Exit after the user chose to install: swaps in the executable this service
// staged, then deletes its package. Never moves a stage it did not verify
// (nothing_staged); the stage handle is closed just before the moves.
UpdateSwapResult update_service_swap(UpdateService*) noexcept;

UpdateChannel update_channel_load(const UpdatePaths&, UpdateChannel fallback) noexcept;
bool update_channel_save(const UpdatePaths&, UpdateChannel) noexcept;
// One byte beside the channel; a missing, old or invalid file gives the fallback.
UpdateCheckInterval update_interval_load(const UpdatePaths&, UpdateCheckInterval fallback) noexcept;
bool update_interval_save(const UpdatePaths&, UpdateCheckInterval) noexcept;

} // namespace maicord
