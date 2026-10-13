#include "maicord/text/arabic_run.hpp"
#include "maicord/text/latin_run.hpp"
#include "maicord/text/arabic_shape_run.hpp"
#include "maicord/text/arabic_joining.hpp"
#include "maicord/text/grapheme.hpp"
#include "maicord/text/base.hpp"

namespace maicord {
SfntStatus native_arabic_run(const NativeArabicPlanStore& store,NativeArabicPlanHandle handle,
    Utf8View text,bool independent,bool rtl,float em,SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    const auto plan=native_arabic_plan_get(store,handle,independent);
    if(!plan.shape || !plan.cmap || (text.bytes && !text.data) || !(em>0) || em>65536 ||
       (storage.components.count && !storage.components.data) || (storage.positions.count && !storage.positions.data) ||
       (storage.targets.count && !storage.targets.data))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    if(plan.generic)return native_latin_shape_run(*store.runs,store.slots[handle.slot-1].run,text,independent,rtl,em,storage,result,limits);
    const u64 scalar_capacity=static_cast<u64>(storage.components.count)*sizeof(SfntComponentNode)/sizeof(u32);
    const u64 mapped_capacity=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntClusterGlyph);
    auto* scalars=reinterpret_cast<u32*>(storage.components.data);auto* mapped=reinterpret_cast<SfntClusterGlyph*>(storage.positions.data);
    UnicodeGraphemeState graphemes{};u32 count=0,units=0,starts=0;
    for(u32 byte=0;byte<text.bytes;){const auto decoded=utf8_decode_one(text.data+byte,text.bytes-byte);
        if(!decoded.valid)return SfntStatus::invalid_input;
        if(count>=scalar_capacity || count>=mapped_capacity)return SfntStatus::output_too_small;
        bool boundary=false;if(!unicode_grapheme_next(&graphemes,decoded.codepoint,&boundary))return SfntStatus::invalid_input;
        if(boundary){if(starts==storage.targets.count)return SfntStatus::output_too_small;storage.targets[starts++]=units;}
        const u32 mapped_scalar=rtl?unicode_bidi_mirror(decoded.codepoint):decoded.codepoint;
        u16 glyph=0;const auto status=sfnt_cmap_glyph(*plan.cmap,mapped_scalar,0,&glyph);if(status!=SfntStatus::ok)return status;
        scalars[count]=decoded.codepoint;mapped[count]={{glyph,sfnt_gdef_glyph_class(plan.shape->gdef,glyph),0},units};
        ++count;units+=decoded.codepoint>0xffff?2u:1u;byte+=decoded.width;
    }
    if(count&&rtl){const u64 span=static_cast<u64>(count-1)*sizeof(SfntClusterGlyph)+1;if(span>0xffffffffu)return SfntStatus::output_too_small;
        const auto status=arabic_joining_forms({scalars,count},{&mapped[0].glyph.reserved,static_cast<u32>(span)},sizeof(SfntClusterGlyph),limits.stage_work);
        if(status!=ArabicJoiningStatus::ok)return status==ArabicJoiningStatus::work_limit?SfntStatus::resource_limit:status==ArabicJoiningStatus::output_too_small?SfntStatus::output_too_small:SfntStatus::invalid_input;
    }
    return sfnt_arabic_shape_run(*plan.shape,*plan.cmap,{mapped,count},{scalars,count},units,{storage.targets.data,starts},rtl,em,storage,result,plan.steps,limits);
}
SfntStatus native_arabic_run(const NativeArabicPlanStore& store,NativeArabicPlanHandle handle,Utf8View text,bool independent,bool rtl,float em,
    const NativeAsciiStorage& s,NativeAsciiResult* out,SfntShapeLimits limits) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};
    if((s.glyphs.count&&!s.glyphs.data)||(s.placements.count&&!s.placements.data)||(s.character_glyphs.count&&!s.character_glyphs.data))return SfntStatus::invalid_input;
    SfntShapeRunResult result{};const auto status=native_arabic_run(store,handle,text,independent,rtl,em,s.shaping,&result,limits);if(status!=SfntStatus::ok)return status;
    if(result.glyph_count>s.glyphs.count||result.glyph_count>s.placements.count||result.source_units>s.character_glyphs.count)return SfntStatus::output_too_small;
    // Validate before compacting the aliased glyph prefix.
    for(u32 i=0;i<result.glyph_count;++i)if(!s.shaping.glyphs[i].glyph.glyph)return SfntStatus::unsupported_sequence;
    if(s.character_glyphs.data!=s.shaping.character_glyphs.data)copy_bytes(s.character_glyphs.data,s.shaping.character_glyphs.data,static_cast<usize>(result.source_units)*sizeof(u32));
    if(s.placements.data!=s.shaping.placements.data)copy_bytes(s.placements.data,s.shaping.placements.data,static_cast<usize>(result.glyph_count)*sizeof(SfntPlacement));
    for(u32 i=0;i<result.glyph_count;++i)s.glyphs[i]=s.shaping.glyphs[i].glyph.glyph;
    *out={result.glyph_count,result.source_units,0,1,result.lookup_operations};return SfntStatus::ok;
}

}
