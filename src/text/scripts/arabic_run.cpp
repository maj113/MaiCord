#include "../shaping/compact_run.hpp"
#include "maicord/text/scripts/arabic_run.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/scripts/latin_run.hpp"
#include "maicord/text/scripts/arabic_shape_run.hpp"
#include "maicord/text/unicode/unicode.hpp"
#include "native_direction.hpp"

namespace maicord {
using native_direction::continues;using native_direction::reverses;using native_direction::reverse;
SfntStatus native_arabic_run(const NativeArabicPlanStore& store,NativeArabicPlanHandle handle,
    Utf8View text,bool independent,bool rtl,float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,SfntShapeLimits limits,ArabicJoiningContext context,bool paragraph_start) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    const auto plan=native_arabic_plan_get(store,handle,independent);
    if(!plan.shape || !plan.cmap || (text.bytes && !text.data) || !(em>0) || em>65536 ||
       (storage.components.count && !storage.components.data) || (storage.positions.count && !storage.positions.data) ||
       (storage.targets.count && !storage.targets.data))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    // A left-to-right run of numbers keeps its direction (hb_ensure_native_direction); it has nothing to join.
    const bool reversed=!rtl&&reverses(text);
    if(!rtl&&!reversed)return native_latin_shape_run(*store.runs,store.slots[handle.slot-1].run,text,independent,rtl,em,storage,result,limits);
    const u64 scalar_capacity=static_cast<u64>(storage.components.count)*sizeof(SfntComponentNode)/sizeof(u32);
    const u64 mapped_capacity=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntClusterGlyph);
    auto* scalars=reinterpret_cast<u32*>(storage.components.data);auto* mapped=reinterpret_cast<SfntClusterGlyph*>(storage.positions.data);
    u32 count=0,units=0,starts=0,previous=0;
    // Initial clusters are HarfBuzz's graphemes; a reversed run marks their starts in the glyph padding
    // until the graphemes are reordered. Right to left, a character HarfBuzz does not mirror (no mirror,
    // or none in the font) takes rtlm (hb_ot_mirror_chars); the flags hold that until the joining forms
    // are written. A reversed left-to-right run mirrors nothing: its target direction stays left to right.
    for(u32 byte=0;byte<text.bytes;){const auto decoded=utf8_decode_one(text.data+byte,text.bytes-byte);
        if(!decoded.valid)return SfntStatus::invalid_input;
        if(count>=scalar_capacity || count>=mapped_capacity || count>=storage.flags.count)return SfntStatus::output_too_small;
        const bool boundary=!count||!continues(decoded.codepoint,previous);
        if(!reversed&&boundary){if(starts==storage.targets.count)return SfntStatus::output_too_small;storage.targets[starts++]=units;}
        const u32 mirror=reversed?decoded.codepoint:unicode_bidi_mirror(decoded.codepoint);u16 glyph=0;
        if(mirror!=decoded.codepoint){const auto status=sfnt_cmap_glyph(*plan.cmap,mirror,0,&glyph);if(status!=SfntStatus::ok)return status;}
        storage.flags[count]=static_cast<u8>(!reversed&&!glyph);
        if(!glyph){const auto status=sfnt_cmap_glyph(*plan.cmap,decoded.codepoint,0,&glyph);if(status!=SfntStatus::ok)return status;}
        scalars[count]=decoded.codepoint;mapped[count]={{glyph,sfnt_gdef_glyph_class(plan.shape->gdef,glyph),static_cast<u8>(reversed&&boundary)},units};
        ++count;units+=decoded.codepoint>0xffff?2u:1u;byte+=decoded.width;previous=decoded.codepoint;
    }
    if(reversed){
        // HarfBuzz shapes the graphemes in reverse order, right to left; each keeps its own order.
        reverse(scalars,0,count);reverse(mapped,0,count);reverse(storage.flags.data,0,count);
        for(u32 first=0;first<count;){u32 last=first;while(!mapped[last].glyph.reserved)++last;
            reverse(scalars,first,last+1);reverse(mapped,first,last+1);reverse(storage.flags.data,first,last+1);first=last+1;}
        u32 source=0;for(u32 i=0;i<count;++i){if(mapped[i].glyph.reserved){if(starts==storage.targets.count)return SfntStatus::output_too_small;storage.targets[starts++]=source;}
            mapped[i].glyph.reserved=0;mapped[i].source=source;source+=scalars[i]>0xffff?2u:1u;}
    }
    if(count){const u64 span=static_cast<u64>(count-1)*sizeof(SfntClusterGlyph)+1;if(span>0xffffffffu)return SfntStatus::output_too_small;
        const auto status=arabic_joining_forms({scalars,count},{&mapped[0].glyph.reserved,static_cast<u32>(span)},sizeof(SfntClusterGlyph),limits.stage_work,context);
        if(status!=ArabicJoiningStatus::ok)return status==ArabicJoiningStatus::work_limit?SfntStatus::resource_limit:status==ArabicJoiningStatus::output_too_small?SfntStatus::output_too_small:SfntStatus::invalid_input;
        for(u32 i=0;i<count;++i)if(storage.flags[i])mapped[i].glyph.reserved|=sfnt_arabic_rtlm_mask;
    }
    // HarfBuzz inserts the dotted circle before reversing; a reversed run whose paragraph begins with a mark
    // keeps it alone (its mark is no longer first).
    auto status=sfnt_arabic_shape_run(*plan.shape,*plan.cmap,{mapped,count},{scalars,count},units,{storage.targets.data,starts},paragraph_start&&!reversed,em,storage,result,plan.steps,limits);
    if(status!=SfntStatus::ok||!reversed)return status;
    status=native_direction::publish_left_to_right(text,units,storage,result->glyph_count,limits.stage_work);
    if(status!=SfntStatus::ok)reset_value(result);
    return status;
}
SfntStatus native_arabic_run(const NativeArabicPlanStore& store,NativeArabicPlanHandle handle,Utf8View text,bool independent,bool rtl,float em,
    const NativeAsciiStorage& s,NativeAsciiResult* out,SfntShapeLimits limits,ArabicJoiningContext context,bool paragraph_start) noexcept {
    return compact_run(s,out,[&](const SfntShapeStorage& shaping,SfntShapeRunResult* shaped) noexcept {return native_arabic_run(store,handle,text,independent,rtl,em,shaping,shaped,limits,context,paragraph_start);});
}

}
