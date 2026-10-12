#pragma once
#include "maicord/text/latin_paragraph.hpp"
namespace maicord {
// One resolved script/font item. The printable callback and paragraph collector
// share caller storage; no callback state or duplicate glyph array is retained.
struct NativeParagraphLineSource {
    void* context;
    SfntStatus (*shape)(void*,Utf8View,float,const NativeAsciiLineStorage&,NativeAsciiLineResult*) noexcept;
};
SfntStatus native_script_paragraph_shape(const SfntFontLayoutMetrics&,Utf8View,float,
    const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,NativeParagraphLineSource,bool* nominal=nullptr) noexcept;
}
