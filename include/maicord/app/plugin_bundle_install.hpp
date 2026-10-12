#pragma once
#include "maicord/app/plugin_catalog.hpp"

namespace maicord {
enum class NativePluginBundleInstallPhase : u8 { empty, staging, prepared, publishing, cleanup, complete };
enum class NativePluginBundleInstallError : u8 {
    none, invalid_input, busy, memory, thread, path, manifest, integrity,
    catalog, conflict, retained_limit, open, copy, flush, commit, unsupported,
    cancelled, release, wait
};
struct NativePluginBundleInstallResult {
    NativePluginBundleInstallPhase phase{};
    NativePluginBundleInstallError error{};
    NativePluginCatalogResult filesystem{};
    u64 owner_id{};
    bool update{}, idempotent{}, committed{}, cleanup_pending{};
    bool ok() const noexcept { return error==NativePluginBundleInstallError::none; }
};
// Stable-address, zero-initialized UI object; do not copy or free before release.
// One occasional worker stages the local manifest and declared DLL, waits for
// explicit UI unload/context authorization, then commits. No code is loaded here.
struct NativePluginBundleInstall {
    HANDLE thread{}, notification{}, command{};
    void* runtime{};
    volatile LONG phase{}, cancelled{}, commit_requested{};
};
// destination is the canonical local-data-root\MaiCord\plugins directory.
// Existing ancestors are pinned; only the final MaiCord/plugins may be created.
// Files are limited by the manifest/DLL limits. Content-addressed managed DLLs
// are limited to 2 files/128MiB per owner before new creation. Unknown files,
// legacy DLLs and conflicting destinations are never overwritten or deleted.
// A file is managed only if it has the reserved owner/hash basename, matches the
// authenticated prior manifest/file identity, and no other manifest references
// it. Orphans left by a hard crash are never guessed at or removed.
NativePluginBundleInstallError native_plugin_bundle_install_start(NativePluginBundleInstall*,
    ConstSlice<wchar_t> manifest_path, ConstSlice<wchar_t> destination) noexcept;
NativePluginBundleInstallResult native_plugin_bundle_install_status(const NativePluginBundleInstall*) noexcept;
bool native_plugin_bundle_install_manifest(const NativePluginBundleInstall*, NativePluginManifest*) noexcept;
// Copies metadata only: the staged snapshot plus the committed row. Output
// must be caller-owned disjoint storage with no pending catalog cleanup.
bool native_plugin_bundle_install_catalog(const NativePluginBundleInstall*, NativePluginCatalog*,
    Slice<NativePluginCatalogEntry>) noexcept;
// UI must first revalidate its request/lifetime and successfully unload owner.
// Fresh install uses no-replace rename; update keeps the old manifest and
// uses documented FileRenameInfoEx/POSIX. Unsupported update fails closed.
// Exclusive owner lock protects cooperating installers, not privileged same-
// user POSIX/ACL mutation. Manifest-last visibility is not power-loss durability.
NativePluginBundleInstallError native_plugin_bundle_install_commit(NativePluginBundleInstall*) noexcept;
void native_plugin_bundle_install_cancel(NativePluginBundleInstall*) noexcept;
// Consume prepared/failed-cleanup notification without leaving it in a wait set.
void native_plugin_bundle_install_acknowledge(NativePluginBundleInstall*) noexcept;
// Failed filesystem cleanup stays pending in the runtime. Retry starts a
// cleanup-only worker; release never performs filesystem IO on the UI thread.
NativePluginBundleInstallError native_plugin_bundle_install_retry_cleanup(NativePluginBundleInstall*) noexcept;
bool native_plugin_bundle_install_release(NativePluginBundleInstall*,u32 timeout_ms=0) noexcept;
struct NativePluginBundleInstallResources {
    u64 committed_bytes{};
    u32 owned_bytes{}, copy_scratch_bytes{}, attached_handles{}, peak_checkpoint_handles{};
    bool published{};
};
// attached/peak counts are handle tokens held (including CNG), sampled only
// at stable checkpoints; they are not a global OS handle count or a transient
// peak. The hash walker also uses 8KiB of stack scratch and never buffers a
// whole DLL.
NativePluginBundleInstallResources native_plugin_bundle_install_resources(const NativePluginBundleInstall*) noexcept;
#if defined(MAICORD_PLUGIN_BUNDLE_INSTALL_FIXTURE)
// Test-only control, compiled only into the backend test. Failure1 forces
// unsupported extended rename; failure2 keeps filesystem cleanup pending;
// failure4 refuses the final rename with access denied before anything is
// committed.
void native_plugin_bundle_install_fixture_failures(u32 mask) noexcept;
#endif
} // namespace maicord
