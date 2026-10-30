#pragma once
#include "maicord/text/font_family.hpp"
#include "maicord/text/font_store.hpp"
#include "maicord/text/base.hpp"
namespace maicord {
// Read-locked identity, byte size, write time and metadata change time.
struct NativeFontFileStamp {Win32FileIdInfo identity;u64 bytes;Win32FileTime write,change;};
static_assert(sizeof(NativeFontFileStamp)==48);
// One physical face (file plus collection index) with its resolved WWS family
// name and candidate list, built by the host from a face it just opened. The
// arrays are the host's and stay alive and unchanged while the catalog is used;
// the host sizes them from `count` and `axis_count`, so no reader validates them.
// Candidate i has properties[i] and settings [i*axis_count, (i+1)*axis_count).
// Accepted files are <4 GiB; `path` is NUL terminated. Views keep no mappings or handles.
struct NativeFontCatalog {
    NativeFontFileStamp stamp;const wchar_t* path;ConstSlice<u16> name;
    const NativeFontProperties* properties;const FontAxisSetting* settings;
    u32 face_index,count,axis_count;
};
struct NativeFontCatalogMatch {u32 candidate;NativeFontProperties properties;u32 simulations;};
constexpr ConstSlice<FontAxisSetting> native_font_catalog_settings(const NativeFontCatalog& c,u32 candidate) noexcept {return {c.settings+candidate*c.axis_count,c.axis_count};}
// Check an already loaded face against the catalog's stamp. No references
// change. A changed identity, size, write/change time or collection-face index
// returns ERROR_RETRY.
NativeFontResult native_font_catalog_validate(const NativeFontCatalog&,const NativeFontStore&,NativeFontHandle) noexcept;
static_assert(sizeof(NativeFontCatalog)==104);
}
