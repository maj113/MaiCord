#pragma once
#include "maicord/ui/text.hpp"
#include "maicord/core/utf.hpp"
namespace maicord {
using GlyphEffectColor = bool(*)(void*,u32 glyph) noexcept;
inline bool glyph_effect_space(u32 cp) noexcept {
    return cp<=0x20||cp==0xa0||cp==0x1680||(cp>=0x2000&&cp<=0x200a)||cp==0x202f||cp==0x205f||cp==0x3000;
}
// Ordinal of one glyph: the count of preceding non-space, non-color scalars.
// Used when glyph_effect_prepare rejects the input.
#if defined(NDEBUG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif
__declspec(noinline) inline u32 glyph_effect_ordinal(const ShapedText& text,Utf8View source,u32 glyph,GlyphEffectColor color,void* context) noexcept {
    if((source.bytes&&!source.data)||(text.cluster_count&&!text.clusters)||!color)return 0;
    u32 first=0,logical=0;
    for(u32 i=0;i<text.cluster_count;++i){const u32 mapped=text.clusters[i];if(mapped!=0xffff&&mapped<=glyph&&mapped>=first)first=mapped;}
    u16 previous=0xffff;u32 unit=0;
    for(u32 byte=0;byte<source.bytes&&unit<text.cluster_count;){const auto cp=utf8_decode_one(source.data+byte,source.bytes-byte);if(!cp.valid)break;
        const auto mapped=text.clusters[unit];if(mapped==first)break;
        if(mapped!=previous&&mapped!=0xffff&&!glyph_effect_space(cp.codepoint)&&!color(context,mapped))++logical;
        previous=mapped;byte+=cp.width;unit+=cp.codepoint>0xffff?2u:1u;}
    return logical;
}
#if defined(NDEBUG)
#pragma clang attribute pop
#endif
// Caller owns writable glyph storage. Only TextGlyph::reserved is changed.
// 0 = no cluster start, 1 = mapped only by a skipped UTF-16 unit, n+2 = first
// logical ordinal. The final pass replaces these temporary tags with ordinals.
// The cluster map includes surrogate halves, but ordinals count scalars.
__declspec(noinline) inline bool glyph_effect_prepare(const ShapedText& text,Utf8View source,GlyphEffectColor color,void* context) noexcept {
    if(text.cluster_count>0xfffd||text.glyph_count>text.glyph_capacity||text.cluster_count>text.cluster_capacity||
       (text.glyph_count&&!text.glyphs)||(text.cluster_count&&!text.clusters)||(source.bytes&&!source.data)||!color)return false;
    for(u32 i=0;i<text.glyph_count;++i)text.glyphs[i].reserved=0;
    for(u32 i=0;i<text.cluster_count;++i){const auto mapped=text.clusters[i];if(mapped!=0xffff&&mapped<text.glyph_count)text.glyphs[mapped].reserved=1;}
    u32 logical=0,unit=0;u16 previous=0xffff;
    for(u32 byte=0;byte<source.bytes&&unit<text.cluster_count;){const auto cp=utf8_decode_one(source.data+byte,source.bytes-byte);if(!cp.valid)break;
        const auto mapped=text.clusters[unit];
        if(mapped!=0xffff&&mapped<text.glyph_count&&text.glyphs[mapped].reserved==1)text.glyphs[mapped].reserved=static_cast<u16>(logical+2);
        if(mapped!=previous&&mapped!=0xffff&&!glyph_effect_space(cp.codepoint)&&!color(context,mapped))++logical;
        previous=mapped;byte+=cp.width;unit+=cp.codepoint>0xffff?2u:1u;}
    u16 current=static_cast<u16>(logical);
    for(u32 i=0;i<text.glyph_count;++i){auto& value=text.glyphs[i].reserved;if(value)current=value==1?static_cast<u16>(logical):static_cast<u16>(value-2);value=current;}
    return true;
}
}
