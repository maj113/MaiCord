#pragma once
#include "maicord/app/packages/link.hpp"

// The files of installed data packages under a data root (%LOCALAPPDATA%\MaiCord; tests pass a private folder):
//   <root>\effects\<slug>-<sha16>.mcfx            sha16 = the first 16 hex digits of the file's SHA-256
//   <root>\themes\<slug>.maicord-theme.json
// Pure file work: no catalog, no UI, any thread.
namespace maicord {

inline constexpr u32 kPackageRootUnits = 480;

bool package_sha256(ConstSlice<u8> bytes, u8 (&digest)[32]) noexcept;
// The path of an installed file. `digest` names an effect's file and is ignored for a theme. False when the
// slug is not valid or the path does not fit.
bool package_file_path(const wchar_t* root, PackageLinkKind kind, Utf8View slug, const u8* digest, Slice<wchar_t> path,
                       u32* units) noexcept;
// Writes the file atomically (creating the folder). *digest is the SHA-256 of `bytes`. A failed write leaves any
// earlier file alone.
bool package_install(const wchar_t* root, PackageLinkKind kind, Utf8View slug, ConstSlice<u8> bytes,
                     u8 (&digest)[32]) noexcept;
// Deletes the other files of an effect slug, keeping the one `keep` names (an update's older versions).
bool package_prune(const wchar_t* root, Utf8View slug, const u8 (&keep)[32]) noexcept;
// Deletes every file of the slug; true when none is left.
bool package_uninstall(const wchar_t* root, PackageLinkKind kind, Utf8View slug) noexcept;

} // namespace maicord
