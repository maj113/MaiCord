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
    // DLLs the post-commit trim or an uninstall deleted, and owner-named files
    // kept because their content does not match the name (referenced DLLs are not counted).
    u32 removed{}, kept{};
    // committed: for an uninstall, the manifest was deleted.
    bool update{}, idempotent{}, committed{}, cleanup_pending{}, uninstall{};
    bool ok() const noexcept { return error==NativePluginBundleInstallError::none; }
};
struct NativePluginBundleInstallOptions {
    // Keep the replaced managed DLL as the previous version: 3 may exist while
    // staged; after commit the owner's DLLs are trimmed to {installed, replaced},
    // each removal hash-verified against its name and unreferenced.
    bool retain_previous{};
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
// are limited to 2 files/128MiB per owner before new creation (3 files/192MiB
// with retain_previous). Unknown files,
// legacy DLLs and conflicting destinations are never overwritten or deleted.
// A file is managed only if it has the reserved owner/hash basename, matches the
// authenticated prior manifest/file identity, and no other manifest references
// it. The installer never guesses at orphans left by a hard crash; only
// native_plugin_bundle_recover removes them, for an owner a caller's journal names.
NativePluginBundleInstallError native_plugin_bundle_install_start(NativePluginBundleInstall*,
    ConstSlice<wchar_t> manifest_path, ConstSlice<wchar_t> destination) noexcept;
NativePluginBundleInstallError native_plugin_bundle_install_start(NativePluginBundleInstall*,
    ConstSlice<wchar_t> manifest_path, ConstSlice<wchar_t> destination, NativePluginBundleInstallOptions options) noexcept;
// Uninstall with the same object, owner lock and handshake. Commit deletes the
// owner's manifest through its pinned handle after an identity and content
// recheck; cleanup deletes the owner's managed DLLs whose content matches
// their names and that nothing references. A missing manifest is `manifest`.
NativePluginBundleInstallError native_plugin_bundle_uninstall_start(NativePluginBundleInstall*, u64 owner_id,
    ConstSlice<wchar_t> destination) noexcept;
// Startup recovery for an owner the plugin manager's journal names, before
// anything is loaded; synchronous, under the owner lock (`busy` otherwise).
// Deletes the staged "<owner16>.maicord-plugin.json.new<16 hex>" manifests and
// every unreferenced "<owner16>-<sha256>.dll" outside keep whose content
// matches its name.
struct NativePluginBundleKeep {
    u8 sha256[2][32];
    u32 count;
};
struct NativePluginBundleRecoveryResult {
    NativePluginBundleInstallError error;
    NativePluginCatalogResult filesystem;
    u32 removed_dlls, removed_manifests, kept;
    bool ok() const noexcept { return error==NativePluginBundleInstallError::none; }
};
NativePluginBundleRecoveryResult native_plugin_bundle_recover(u64 owner_id, ConstSlice<wchar_t> destination,
    const NativePluginBundleKeep& keep) noexcept;
NativePluginBundleInstallResult native_plugin_bundle_install_status(const NativePluginBundleInstall*) noexcept;
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
// failure4 refuses the final rename (or an uninstall's delete) with access
// denied before anything is committed; failure8 is a hard crash just before
// the commit: the created DLL and the staged manifest stay on disk.
void native_plugin_bundle_install_fixture_failures(u32 mask) noexcept;
#endif
} // namespace maicord
