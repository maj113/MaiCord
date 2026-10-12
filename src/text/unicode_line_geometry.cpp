#include "unicode_line_geometry.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/core/utf.hpp"
#include <emmintrin.h>
namespace maicord {namespace {
float add(float a,float b) noexcept {return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a),_mm_set_ss(b)));}
bool finite(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);return (bits&0x7f800000u)!=0x7f800000u;}
}
SfntStatus native_unicode_line_geometry(const SfntFontLayoutMetrics& metrics,Utf8View text,float em,const NativeAsciiLineStorage& s,NativeAsciiLineResult* output,bool paragraph_only) noexcept {
    auto& line=*output;if(!paragraph_only){const auto status=sfnt_single_font_line(metrics,em,&line.vertical);if(status!=SfntStatus::ok)return status;}
    const u32 units=line.run.source_units;
    if(units&&(!line.run.glyph_count||s.run.character_glyphs[0]))return SfntStatus::unsupported_sequence;
    float pen=0,last_nonspace=0,tail_pen=0;u32 byte=0;line.geometry_flags=native_line_ordered_x;
    for(u32 at=0;at<units;){const u32 glyph=s.run.character_glyphs[at];u32 end=at+1;while(end<units&&s.run.character_glyphs[end]==glyph)++end;
        const u32 glyph_end=end<units?s.run.character_glyphs[end]:line.run.glyph_count;
        if(glyph_end<=glyph||glyph_end>line.run.glyph_count)return SfntStatus::unsupported_sequence;
        if(line.cluster_count==s.clusters.count)return SfntStatus::output_too_small;
        bool space=false;if(!paragraph_only){const auto first=utf8_decode_one(text.data+byte,text.bytes-byte);if(!first.valid)return SfntStatus::invalid_input;
            space=unicode_properties(first.codepoint)->has(UnicodeFlag::white_space);}float width=0;const float left=pen;
        for(u32 i=glyph;i<glyph_end;++i){const float advance=s.run.placements[i].advance;width=add(width,advance);pen=add(pen,advance);if(space)tail_pen=add(tail_pen,advance);}
        line.width_including_trailing=add(line.width_including_trailing,width);
        if(!finite(pen)||!finite(width)||!finite(line.width_including_trailing)||!finite(tail_pen))return SfntStatus::resource_limit;
        s.clusters[line.cluster_count++]={end,left,width};if(width<0||pen<left)line.geometry_flags&=~native_line_ordered_x;
        if(!paragraph_only){
        if(space)line.trailing_source_units+=end-at;else{line.width=line.width_including_trailing;line.trailing_source_units=0;last_nonspace=pen;tail_pen=0;}
        for(u32 source=at;source<end;){const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);if(!cp.valid)return SfntStatus::invalid_input;
            source+=cp.codepoint>65535?2u:1u;byte+=cp.width;if(source>end)return SfntStatus::unsupported_sequence;}
        }
        at=end;
    }
    line.caret_end=add(last_nonspace,tail_pen);return finite(line.caret_end)?SfntStatus::ok:SfntStatus::resource_limit;
}
}
