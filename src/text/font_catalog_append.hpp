#pragma once
#include "maicord/text/font_catalog.hpp"
namespace maicord {
// Private growing-owner entry. Growth may extend capacities in place only;
// published counts/data and existing base addresses must remain unchanged.
// It runs while the file is open and staged faces still reference its metadata.
struct NativeFontCatalogGrowth {void* context;bool (*reserve)(void*,NativeFontCatalogCounts) noexcept;};
NativeFontResult native_font_catalog_append(NativeFontCatalog*,const wchar_t*,Slice<u16>,u32*,u32,u32,
    NativeFontCatalogCounts*,bool,Slice<u8>,NativeFontCatalogGrowth,NativeFontFileStamp* stamp=nullptr,bool* no_reparse=nullptr) noexcept;
}
