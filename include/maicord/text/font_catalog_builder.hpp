#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/text/base.hpp"
namespace maicord {
struct NativeFontCatalogBuilder {
    Arena files,faces,candidates,settings,strings;
    NativeFontCatalog catalog;NativeFontCatalogCounts limits;
};
// Caller starts with a zero-initialized builder at a stable address. One memory
// reservation holds all pools and grows only after a reported capacity shortage;
// existing prefixes keep their addresses until release. Do not copy a live builder.
// Pool arenas are borrowed slices: never reset or release them independently.
bool native_font_catalog_builder_initialize(NativeFontCatalogBuilder*,NativeFontCatalogCounts limits={1024,4096,8192,16384,262144}) noexcept;
void native_font_catalog_builder_release(NativeFontCatalogBuilder*) noexcept;
// Name scratch above 8192 units lends its unused tail to compact metadata reads;
// if that leaves too little name capacity, the call retries with the full buffer and
// mapped metadata. Capacity growth resumes in place while the file stays open,
// without rereading successful faces.
// Optional stamp is captured while the same read handle protects parsed bytes.
// It is usable on success or unsupported-font status, including empty files.
// Optional no_reparse belongs to that same read handle. Only success or
// unsupported-font status permits combining it with the returned stamp.
NativeFontResult native_font_catalog_builder_add(NativeFontCatalogBuilder*,const wchar_t*,Slice<u16> name_scratch,u32* file_index,u32 maximum_faces=65536,u32 work_per_face=1000000,bool default_face_only=false,NativeFontFileStamp* stamp=nullptr,bool* no_reparse=nullptr) noexcept;
// Copies only used records and strings into one caller-provided arena. Catalogs hold
// offsets, so the copy needs no pointer fixups and keeps no source mappings. The
// destination must not overlap any source storage or output. Output arrays are sized
// to the used counts and read-only; further appends need a builder. On failure the
// output clears and the arena offset rolls back (committed pages may remain). On
// success the builder can be released immediately; the arena's lifetime is the
// caller's.
NativeFontResult native_font_catalog_publish(const NativeFontCatalog&,Arena*,NativeFontCatalog*) noexcept;
}
