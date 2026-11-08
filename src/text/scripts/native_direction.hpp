#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/shaping/shape_run.hpp"
#include "maicord/text/unicode/unicode.hpp"
// HarfBuzz's handling of a right-to-left script's run laid out left to right (hb_ensure_native_direction): its
// graphemes are reversed, shaped right to left and published in visual order. Shared by the Arabic and emoji runs.
namespace maicord::native_direction {
// HarfBuzz's grapheme continuation (hb_set_unicode_props): marks, ZWJ, an Extended_Pictographic after ZWJ,
// emoji modifiers, halfwidth sound marks and tags continue the previous character's grapheme.
inline bool continues(u32 cp,u32 previous) noexcept {
    if(cp==0x200d||cp-0x1f3fbu<5||cp-0xff9eu<2||cp-0xe0020u<96)return true;
    const auto* p=unicode_properties(cp);const auto category=p->general_category();
    return category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me||
        (previous==0x200d&&p->has(UnicodeFlag::extended_pictographic));
}
// Whether HarfBuzz reverses a left-to-right run of a right-to-left script: unless it has a number or regional
// indicator and no letter.
inline bool reverses(Utf8View text) noexcept {
    bool letter=false,number=false;
    for(u32 byte=0;byte<text.bytes;){const auto decoded=utf8_decode_one(text.data+byte,text.bytes-byte);if(!decoded.valid)return true;byte+=decoded.width;
        const auto category=unicode_properties(decoded.codepoint)->general_category();
        letter|=category==UnicodeGeneralCategory::lu||category==UnicodeGeneralCategory::ll||category==UnicodeGeneralCategory::lt||category==UnicodeGeneralCategory::lm||category==UnicodeGeneralCategory::lo;
        number|=category==UnicodeGeneralCategory::nd||decoded.codepoint-0x1f1e6u<26;}
    return letter||!number;
}
template<class T> void reverse(T* values,u32 first,u32 end) noexcept {while(first+1<end){--end;const T value=values[first];values[first++]=values[end];values[end]=value;}}
// Publishes a run shaped right to left from its reversed graphemes left to right: glyphs in reverse, placements
// mirrored, and each original unit mapped to the leftmost glyph of its cluster (a cluster [first,next) of the
// right-to-left result becomes [n-next,n-first)); the units follow the original text again.
inline SfntStatus publish_left_to_right(Utf8View text,u32 units,const SfntShapeStorage& storage,u32 n,u64 work) noexcept {
    auto* map=storage.character_glyphs.data;
    if(static_cast<u64>(n)+units>work)return SfntStatus::resource_limit;
    reverse(storage.glyphs.data,0,n);reverse(storage.placements.data,0,n);reverse(storage.flags.data,0,n);
    for(u32 i=0;i<n;++i){storage.placements[i].advance_offset=0.0f-storage.placements[i].advance_offset;storage.flags[i]&=static_cast<u8>(~sfnt_position_cluster_start);}
    constexpr u32 absent=0xffffffffu;
    if(units){u32 next=n,current=absent;
        for(u32 u=units;u--;){const u32 first=map[u];if(first==absent)continue;if(current==absent)current=first;else if(first!=current){next=current;current=first;}
            map[u]=n-next;if(next>0)storage.flags[n-next]|=sfnt_position_cluster_start;}}
    // Units follow the reversed graphemes; restore their order: reverse all, then each grapheme back.
    reverse(map,0,units);
    u32 previous=0;for(u32 byte=0,unit=0,first=0;byte<text.bytes;){const auto decoded=utf8_decode_one(text.data+byte,text.bytes-byte);
        if(byte&&!continues(decoded.codepoint,previous)){reverse(map,first,unit);first=unit;}
        unit+=decoded.codepoint>0xffff?2u:1u;byte+=decoded.width;previous=decoded.codepoint;if(byte==text.bytes)reverse(map,first,unit);}
    return SfntStatus::ok;
}
}
