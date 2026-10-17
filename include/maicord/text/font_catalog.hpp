#pragma once
#include "maicord/text/font_family.hpp"
#include "maicord/text/font_store.hpp"
#include "maicord/text/base.hpp"
namespace maicord {
// Read-locked identity, byte size, write time and metadata change time.
struct NativeFontFileStamp {Win32FileIdInfo identity;u64 bytes;Win32FileTime write,change;};
static_assert(sizeof(NativeFontFileStamp)==48);
struct NativeFontCatalogFile {
    // Accepted files are <4 GiB, paths <32768 units and TTCs <=65536 faces.
    // Encode count minus one so both timestamps fit without growing the record.
    Win32FileIdInfo identity;u32 bytes,path_offset;Win32FileTime last_write,change_time;
    u32 first_face;u16 path_units,face_count_minus_one;
    u32 face_count() const noexcept {return static_cast<u32>(face_count_minus_one)+1;}
};
struct NativeFontCatalogFace {u32 file,index,name_offset,name_units,first_candidate,candidate_count;};
struct NativeFontCatalogCandidate {NativeFontProperties properties;u32 face,named_index,first_setting,setting_count;};
struct NativeFontCatalogCounts {u32 files,faces,candidates,settings,strings;};
struct NativeFontCatalog {
    Slice<NativeFontCatalogFile> files;Slice<NativeFontCatalogFace> faces;
    Slice<NativeFontCatalogCandidate> candidates;Slice<FontAxisSetting> settings;Slice<u16> strings;
    NativeFontCatalogCounts count;
};
struct NativeFontCatalogMatch {u32 candidate;NativeFontProperties properties;u32 simulations;};
// Readers need a catalog that is no longer being written. Views keep no mappings or handles.
ConstSlice<u16> native_font_catalog_path(const NativeFontCatalog&,u32 file) noexcept;
ConstSlice<u16> native_font_catalog_name(const NativeFontCatalog&,u32 face) noexcept;
ConstSlice<FontAxisSetting> native_font_catalog_settings(const NativeFontCatalog&,u32 candidate) noexcept;
struct NativeFontCatalogEntry {
    ConstSlice<u16> path;ConstSlice<FontAxisSetting> settings;
    const NativeFontCatalogFile* file;u32 face_index;
};
// Validated borrowed descriptor; the catalog and its arrays must stay alive and
// unchanged. Path has an additional NUL in catalog storage. Error clears output.
NativeFontResult native_font_catalog_entry(const NativeFontCatalog&,u32 candidate,NativeFontCatalogEntry*) noexcept;
// Check an already loaded face against a descriptor returned by entry(). No references
// change. A changed identity, size, write/change time or collection-face index
// returns ERROR_RETRY.
NativeFontResult native_font_catalog_validate(const NativeFontCatalogEntry&,const NativeFontStore&,NativeFontHandle) noexcept;
static_assert(sizeof(NativeFontCatalogFile)==56);
static_assert(sizeof(NativeFontCatalogFace)==24);
static_assert(sizeof(NativeFontCatalogCandidate)==20);
}
