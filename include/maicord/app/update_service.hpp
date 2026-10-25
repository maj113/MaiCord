#pragma once

#include "maicord/app/update_changelog.hpp"
#include "maicord/app/update_fetch.hpp"
#include "maicord/app/update_handoff.hpp"
#include "maicord/app/update_install.hpp"

namespace maicord {

// Application updater: GitHub fetch -> signed manifest -> durable admission
// -> package download -> stage -> generation install, plus the restart,
// rollback, startup dispatcher and child confirmation around the generation
// and handoff backends. Trust comes only from the compiled publisher key.
inline constexpr u32 kUpdatePathCapacity = 512;
inline constexpr u64 kUpdateFreeSpaceReserveBytes = 16ull * 1024 * 1024;

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

// <base>\enrollment.bin, channel, interval, generations\ (journal + g-*), work\
// (acceptance record, download, stage). All on one local fixed volume.
struct UpdatePaths {
    wchar_t base[kUpdatePathCapacity], root[kUpdatePathCapacity], work[kUpdatePathCapacity];
    wchar_t enrollment[kUpdatePathCapacity], channel[kUpdatePathCapacity], interval[kUpdatePathCapacity];
    wchar_t journal[kUpdatePathCapacity];
    wchar_t acceptance[kUpdatePathCapacity], download[kUpdatePathCapacity];
    u32 root_count, work_count, acceptance_count, download_count;
};
bool update_paths_build(ConstSlice<wchar_t> base, UpdatePaths*) noexcept;
bool update_paths_default(UpdatePaths*) noexcept; // %LOCALAPPDATA%\MaiCord\updates

enum class UpdateServiceError : u8 {
    none, configuration, storage, enrollment, busy, transport, rate_limited, not_found, redirect, oversize,
    size, signature, expired, manifest, acceptance, stage, install, disk, launch, startup, state, unavailable,
};
Utf8View update_service_error_text(UpdateServiceError) noexcept;

struct UpdateInstalledState {
    u64 active_version, previous_version, candidate_version;
    UpdateGenerationPhase phase;
    bool journal;
};
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
    u8 inventory[32];
    UpdateGenerationIdentity installation;
    UpdateGenerationTransaction transaction;
    UpdateInstallContext install;
    UpdatePreparationContext preparation;
    UpdatePreparationResult preparation_result;
    UpdateStage stage;
    UpdatePackage package;
    UpdateHandoff handoff;
    UpdateProgress progress;
    // Optional: a changelog buffer (null skips the changelog) and an observer.
    UpdateChangelogDownload* changelog;
    UpdateOfferObserver observer;
    HANDLE download;
    u32 document_bytes, signature_bytes;
    char document[kUpdateManifestMaximumBytes];
    u8 signature[64];
    u32 scratch[256];
    bool initialized, open;
};
UpdateServiceError update_service_initialize(UpdateService*, const UpdateServiceConfig&, const UpdatePaths&) noexcept;
void update_service_release(UpdateService*) noexcept;
// Opens the enrolled generation root under its exclusive gate (busy when
// another process holds it). provision admits a missing journal once.
UpdateServiceError update_service_open(UpdateService*, bool provision) noexcept;
void update_service_close(UpdateService*) noexcept;
// A protocol-1 (0.4.x) previous or candidate generation reports version 0:
// it is never reverted to or activated.
UpdateServiceError update_service_inspect(UpdateService*, UpdateInstalledState*) noexcept;
UpdateCheckResult update_service_check(UpdateService*, const UpdateTransport&, UpdateChannel, u64 now_unix) noexcept;
// Launches the prepared (live activation) or newer/reverted stable package and
// waits for its startup confirmation. none means the caller should exit. A
// protocol-1 package launches only while the journal is still v2.
UpdateServiceError update_service_restart(UpdateService*) noexcept;
// Stable with a retained older previous package: make it active again.
UpdateServiceError update_service_revert(UpdateService*) noexcept;
// Startup: launch an authenticated newer active generation. true when it
// confirmed startup and this process should exit.
bool update_service_dispatch(UpdateService*) noexcept;
// Child startup: independently validate the own module path, journal and
// signed installed package, then import the handoff capabilities.
UpdateServiceError update_service_child_open(const UpdateServiceConfig&, const UpdatePaths&,
    ConstSlice<wchar_t> module_path, ConstSlice<wchar_t> command_line, UpdateHandoffChild*) noexcept;

UpdateChannel update_channel_load(const UpdatePaths&, UpdateChannel fallback) noexcept;
bool update_channel_save(const UpdatePaths&, UpdateChannel) noexcept;
// One byte beside the channel; a missing, old or invalid file gives the fallback.
UpdateCheckInterval update_interval_load(const UpdatePaths&, UpdateCheckInterval fallback) noexcept;
bool update_interval_save(const UpdatePaths&, UpdateCheckInterval) noexcept;

} // namespace maicord
