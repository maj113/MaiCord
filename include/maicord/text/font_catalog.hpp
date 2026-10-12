#pragma once
#include "maicord/text/font_family.hpp"
#include "maicord/text/font_store.hpp"
#include "maicord/core/win32_records.hpp"
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
// Stable caller-provided arrays with initial counts of zero. Serialize appends; readers
// need a catalog that is no longer being appended to. Views keep no mappings or handles.
// One add opens a file read-only, enumerates every collection face and copies
// native metadata, then closes it. No outline/cmap/preparation work is performed.
// Path is NUL-terminated, at most 32,767 UTF-16 units, disjoint from storage/scratch.
// All collection faces must be supported. Errors leave counts and existing data
// unchanged; unused storage and name scratch can change. Output is UINT_MAX on error.
// Physical file aliases are deduplicated, preserving the first path. Size/write/change-time
// changes for an existing file identity return ERROR_RETRY. This is not a content
// hash, refresh service or availability/precedence policy. Separate font collections
// require separate catalogs. The caller must check every add result before using the catalog.
NativeFontResult native_font_catalog_add(NativeFontCatalog*,const wchar_t* path,Slice<u16> name_scratch,
    u32* file_index,u32 maximum_faces=65536,u32 work_per_face=1000000,NativeFontCatalogCounts* needed=nullptr,bool default_face_only=false,Slice<u8> metadata_scratch={}) noexcept;
// Optional disjoint metadata scratch enables compact reads of catalog tables.
// Too-small scratch uses the general mapping path without reducing coverage.
// Optional needed starts at the current counts and reports the failing array's
// required capacity. Other counts may include staged work. It is valid only for
// capacity failures; no output/catalog/scratch overlap is permitted.
// default_face_only projects face zero's default axis instance, matching a private
// single-face reference collection. Use a separate catalog per collection; do not
// mix full-file and default-face additions of the same physical file.
// Match English WWS names case-insensitively using Windows ordinal comparison.
// No font service, file IO or allocation. Exact ties preserve file/face/instance
// insertion order; compatibility of that order is a catalog-builder obligation.
// Scratch arrays are disjoint and reusable; output clears on error. Query name is
// counted and disjoint. Missing names return unsupported_sequence, never an alias
// or fallback font. Numeric and localized name limitations follow font_family.
SfntStatus native_font_catalog_match(const NativeFontCatalog&,ConstSlice<u16> name,NativeFontProperties,
    Slice<NativeFontProperties> properties_scratch,Slice<u32> index_scratch,NativeFontCatalogMatch*,u32 maximum_faces=65536) noexcept;
ConstSlice<u16> native_font_catalog_path(const NativeFontCatalog&,u32 file) noexcept;
ConstSlice<u16> native_font_catalog_name(const NativeFontCatalog&,u32 face) noexcept;
ConstSlice<FontAxisSetting> native_font_catalog_settings(const NativeFontCatalog&,u32 candidate) noexcept;
// Acquire only the selected physical face in the native store. A successful
// handle keeps the file mapping. Stored identity/size/write/change time is checked before
// returning it; a detected change releases the temporary handle and requests a
// catalog refresh with ERROR_RETRY. Axis settings above feed instance acquisition.
NativeFontResult native_font_catalog_acquire(const NativeFontCatalog&,u32 candidate,NativeFontStore*,NativeFontHandle*) noexcept;
struct NativeFontCatalogEntry {
    ConstSlice<u16> path;ConstSlice<FontAxisSetting> settings;
    const NativeFontCatalogFile* file;u32 face_index;
};
// Validated borrowed descriptor; the catalog and its arrays must stay alive and
// unchanged. Path has an additional NUL in catalog storage. Error clears output.
NativeFontResult native_font_catalog_entry(const NativeFontCatalog&,u32 candidate,NativeFontCatalogEntry*) noexcept;
// Check an already loaded face against a descriptor returned by entry(). No references
// change. This lets a growing session acquire once, then apply the same stale-file
// policy as the fixed-store adapter. Includes the physical collection-face index.
NativeFontResult native_font_catalog_validate(const NativeFontCatalogEntry&,const NativeFontStore&,NativeFontHandle) noexcept;
static_assert(sizeof(NativeFontCatalogFile)==56);
static_assert(sizeof(NativeFontCatalogFace)==24);
static_assert(sizeof(NativeFontCatalogCandidate)==20);
}
