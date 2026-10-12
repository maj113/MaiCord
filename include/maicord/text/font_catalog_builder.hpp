#pragma once
#include "maicord/text/font_catalog.hpp"
#include "maicord/core/arena.hpp"
namespace maicord {
struct NativeFontCatalogBuilder {
    Arena files,faces,candidates,settings,strings;
    NativeFontCatalog catalog;NativeFontCatalogCounts limits;
};
// Caller starts with a zero-initialized builder at a stable address. One memory
// reservation holds all pools; it grows only after a reported capacity shortage. Existing
// prefixes stay at stable addresses until release. Do not copy the live builder.
// Pool arenas are borrowed slices: never reset or release them independently.
// Large name scratch lends its unused tail to compact metadata reads. If the
// remaining name capacity is insufficient, retry with the full name buffer and
// mapped metadata. No extra allocation or smaller accepted name limit.
// Capacity growth resumes in place while the file stays open, without rereading
// successful faces. Name-scratch expansion can still require a bounded retry.
// No OS font service is used.
bool native_font_catalog_builder_initialize(NativeFontCatalogBuilder*,NativeFontCatalogCounts limits={1024,4096,8192,16384,262144}) noexcept;
void native_font_catalog_builder_release(NativeFontCatalogBuilder*) noexcept;
// Optional stamp is captured while the same read handle protects parsed bytes.
// It is usable on success or unsupported-font status, including empty files.
// Optional no_reparse belongs to that same read handle. Only success or
// unsupported-font status permits combining it with the returned stamp.
NativeFontResult native_font_catalog_builder_add(NativeFontCatalogBuilder*,const wchar_t*,Slice<u16> name_scratch,u32* file_index,u32 maximum_faces=65536,u32 work_per_face=1000000,bool default_face_only=false,NativeFontFileStamp* stamp=nullptr,bool* no_reparse=nullptr) noexcept;
// Copy only used records and strings into one caller-provided arena. Catalogs contain
// offsets, so no pointer fixups or source mappings are kept. Destination must
// not overlap any source storage/output. Output arrays have capacity equal to used
// counts and are read-only once built; further appends need a builder.
// On failure, output clears and the destination allocation offset rolls back (committed
// pages may remain). On success the builder can be released immediately. The arena's
// lifetime is up to the caller.
NativeFontResult native_font_catalog_publish(const NativeFontCatalog&,Arena*,NativeFontCatalog*) noexcept;
}
