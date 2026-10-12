#pragma once
#include "maicord/text/font_catalog.hpp"
namespace maicord {
// Context-local metadata for one physical face; never serialized as a full file.
struct NativeSystemFontFile {NativeSystemFontFile* next;NativeFontCatalog catalog;u32 first_candidate;};
}
