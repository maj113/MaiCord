#include "unicode_line_geometry.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/text/base.hpp"
#include "layout_float.hpp"
namespace maicord {
SfntStatus native_unicode_line_geometry(const SfntFontLayoutMetrics& metrics,const Utf8View& text_view,float em,const NativeAsciiLineStorage& s,NativeAsciiLineResult* output,bool paragraph_only) noexcept {
    const Utf8View text=text_view; // *output is written below
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
        for(u32 i=glyph;i<glyph_end;++i){const float advance=s.run.placements[i].advance;width=layout_add(width,advance);pen=layout_add(pen,advance);if(space)tail_pen=layout_add(tail_pen,advance);}
        line.width_including_trailing=layout_add(line.width_including_trailing,width);
        if(!is_finite(pen)||!is_finite(width)||!is_finite(line.width_including_trailing)||!is_finite(tail_pen))return SfntStatus::resource_limit;
        s.clusters[line.cluster_count++]={end,left,width};if(width<0||pen<left)line.geometry_flags&=~native_line_ordered_x;
        if(!paragraph_only){
        if(space)line.trailing_source_units+=end-at;else{line.width=line.width_including_trailing;line.trailing_source_units=0;last_nonspace=pen;tail_pen=0;}
        for(u32 source=at;source<end;){const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);if(!cp.valid)return SfntStatus::invalid_input;
            source+=cp.codepoint>65535?2u:1u;byte+=cp.width;if(source>end)return SfntStatus::unsupported_sequence;}
        }
        at=end;
    }
    line.caret_end=layout_add(last_nonspace,tail_pen);return is_finite(line.caret_end)?SfntStatus::ok:SfntStatus::resource_limit;
}
}
