#include "unicode_line_geometry.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/unicode/unicode.hpp"
#include "../opentype/layout_float.hpp"
namespace maicord {
namespace {
// Cluster geometry of a shaped line, specialized by what the source text must supply.
using Geometry=NativeLineText;
template<Geometry kind> SfntStatus geometry(const Utf8View& text,const NativeAsciiLineStorage& s,NativeAsciiLineResult& line) noexcept {
    const u32 units=line.run.source_units;
    if(units&&(!line.run.glyph_count||s.run.character_glyphs[0]))return SfntStatus::unsupported_sequence;
    // Accumulate in locals: stores to the cluster array must not force reloads of line fields or slice pointers.
    const auto glyph_ids=s.run.character_glyphs;const auto placements=s.run.placements;const auto clusters=s.clusters;const u32 glyph_count=line.run.glyph_count;
    float pen=0,total=0;u32 at=0,byte=0,count=0,trailing=0,spaces=0,flags=native_line_ordered_x;
    // A nominal ASCII run maps each character to its own glyph (native_ascii_run): one cluster for each glyph.
    if constexpr(kind==Geometry::printable_ascii)if(line.run.nominal_ranges){
        if(units>clusters.count)return SfntStatus::output_too_small;
        for(;at<units;++at){const float advance=placements[at].advance,left=pen,width=layout_add(0,advance);
            pen=layout_add(pen,advance);total=layout_add(total,width);clusters[at]={at+1,left,width};
            if(width<0||pen<left)flags&=~native_line_ordered_x;trailing=(trailing+1)&(0u-static_cast<u32>(text.data[at]==' '));}
        count=units;spaces=trailing;}
    while(at<units){const u32 glyph=glyph_ids[at];u32 end=at+1;while(end<units&&glyph_ids[end]==glyph)++end;
        const u32 glyph_end=end<units?glyph_ids[end]:glyph_count;
        if(glyph_end<=glyph||glyph_end>glyph_count)return SfntStatus::unsupported_sequence;
        if(count==clusters.count)return SfntStatus::output_too_small;
        // Whitespace follows the cluster's first scalar; the cluster's scalars are consumed to keep the byte cursor in step.
        bool space=false;
        if constexpr(kind==Geometry::printable_ascii)space=text.data[at]==' ';
        else if constexpr(kind==Geometry::unicode)for(u32 source=at;source<end;){u32 scalar=static_cast<u8>(text.data[byte]),width=1;
            if(scalar>=0x80){const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);if(!cp.valid)return SfntStatus::invalid_input;scalar=cp.codepoint;width=cp.width;}
            if(source==at)space=scalar<0x80?scalar==' '||scalar-9u<5:unicode_properties(scalar)->has(UnicodeFlag::white_space);
            source+=scalar>65535?2u:1u;byte+=width;if(source>end)return SfntStatus::unsupported_sequence;}
        float width=0;const float left=pen;
        for(u32 i=glyph;i<glyph_end;++i){const float advance=placements[i].advance;width=layout_add(width,advance);pen=layout_add(pen,advance);}
        total=layout_add(total,width);
        clusters[count++]={end,left,width};if(width<0||pen<left)flags&=~native_line_ordered_x;
        // The source units and the clusters of the spaces that end the line. A branch on the class would mispredict at each word.
        const u32 keep=0u-static_cast<u32>(space);trailing=(trailing+end-at)&keep;spaces=(spaces+1)&keep;
        at=end;
    }
    // The sums at the last cluster that is not a space. The clusters and the glyphs of the spaces that end the line give
    // the same additions in the same order.
    float ink=total,last_nonspace=0,tail_pen=0;
    if constexpr(kind!=Geometry::paragraph){last_nonspace=pen;
        if(spaces){last_nonspace=clusters[count-spaces].left;ink=0;for(u32 i=0;i<count-spaces;++i)ink=layout_add(ink,clusters[i].width);
            for(u32 i=glyph_ids[units-trailing];i<glyph_count;++i)tail_pen=layout_add(tail_pen,placements[i].advance);}}
    // Non-finite sums absorb every later addition, so one check covers every advance and width.
    if(!is_finite(pen)||!is_finite(total)||!is_finite(tail_pen))return SfntStatus::resource_limit;
    line.cluster_count=count;line.geometry_flags=flags;line.width_including_trailing=total;line.trailing_source_units=trailing;
    if constexpr(kind!=Geometry::paragraph)line.width=ink;
    line.caret_end=layout_add(last_nonspace,tail_pen);return is_finite(line.caret_end)?SfntStatus::ok:SfntStatus::resource_limit;
}
}
SfntStatus native_unicode_line_geometry(const SfntFontLayoutMetrics& metrics,const Utf8View& text_view,float em,const NativeAsciiLineStorage& s,NativeAsciiLineResult* output,NativeLineText kind) noexcept {
    const Utf8View text=text_view; // *output is written below
    auto& line=*output;if(kind==NativeLineText::paragraph)return geometry<Geometry::paragraph>(text,s,line);
    const auto status=sfnt_single_font_line(metrics,em,&line.vertical);if(status!=SfntStatus::ok)return status;
    return kind==NativeLineText::printable_ascii?geometry<Geometry::printable_ascii>(text,s,line):geometry<Geometry::unicode>(text,s,line);
}
SfntStatus native_finish_line(const NativeRunPlanStore& store,NativeRunPlanHandle handle,const Utf8View& text,float em,const NativeAsciiLineStorage& s,
    NativeAsciiLineResult& line,NativeAsciiLineResult* output,SfntShapeLimits limits,float trailing_spacing,bool rtl,NativeLineText kind) noexcept {
    u32 spacing_bits=0;copy_bytes(&spacing_bits,&trailing_spacing,4);
    if(spacing_bits&0x7fffffffu){const auto status=sfnt_trailing_spacing({s.run.placements.data,line.run.glyph_count},
        line.run.nominal_ranges?ConstSlice<u8>{}:ConstSlice<u8>{s.run.shaping.flags.data,line.run.glyph_count},trailing_spacing,limits.stage_work,rtl);
        if(status!=SfntStatus::ok)return status;}
    if(s.clusters.count&&!s.clusters.data)return SfntStatus::invalid_input;
    const auto* metrics=native_run_plan_metrics(store,handle);if(!metrics)return SfntStatus::invalid_input;
    const auto status=native_unicode_line_geometry(*metrics,text,em,s,&line,kind);if(status==SfntStatus::ok)*output=line;return status;
}
SfntStatus native_paragraph_append_control(const NativeAsciiParagraphStorage& s,NativeParagraphShape& shaped,u32 units,u32 flags) noexcept {
    if(shaped.cluster_count==s.clusters.count||units>s.source_glyphs.count-shaped.source_units)return SfntStatus::output_too_small;
    s.clusters[shaped.cluster_count++]={shaped.source_units,shaped.source_units+units,shaped.glyph_count,0,0,flags};
    for(u32 i=0;i<units;++i)s.source_glyphs[shaped.source_units+i]=0xffffffffu;
    shaped.source_units+=units;return SfntStatus::ok;
}
SfntStatus native_paragraph_append_line(const NativeAsciiParagraphStorage& s,const NativeAsciiLineResult& line,NativeParagraphShape& shaped) noexcept {
    if(!(line.geometry_flags&native_line_ordered_x))return SfntStatus::unsupported_sequence;
    const u32 glyphs=line.run.glyph_count,units=line.run.source_units;
    if(glyphs>s.glyphs.count-shaped.glyph_count||glyphs>s.placements.count-shaped.glyph_count||
       units>s.source_glyphs.count-shaped.source_units||line.cluster_count>s.clusters.count-shaped.cluster_count)return SfntStatus::output_too_small;
    // The loops read their pointers and counts from locals: without them each store forces a reload.
    const auto from=s.work.run;const auto* const lines=s.work.clusters.data;const u32 glyph_first=shaped.glyph_count,source_first=shaped.source_units,count=line.cluster_count;
    u16* const ids=s.glyphs.data+glyph_first;SfntPlacement* const placed=s.placements.data+glyph_first;u32* const sources=s.source_glyphs.data+source_first;auto* const out=s.clusters.data+shaped.cluster_count;
    for(u32 i=0;i<glyphs;++i){ids[i]=from.glyphs[i];placed[i]=from.placements[i];}
    for(u32 i=0;i<units;++i)sources[i]=glyph_first+from.character_glyphs[i];
    u32 first=0;for(u32 i=0;i<count;++i){const auto& c=lines[i];const u32 g=from.character_glyphs[first],end=c.source_end==units?glyphs:from.character_glyphs[c.source_end];
        out[i]={source_first+first,source_first+c.source_end,glyph_first+g,end-g,c.width,0};first=c.source_end;}
    shaped.cluster_count+=count;shaped.source_units+=units;shaped.glyph_count+=glyphs;return SfntStatus::ok;
}
}
