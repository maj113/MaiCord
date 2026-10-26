#include "compact_run.hpp"
#include "maicord/text/gpos_execute.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {
SfntStatus decode_run_scalars(const Utf8View& text_view,const SfntShapeStorage& storage,ConstSlice<u32>& scalars) noexcept {
    const Utf8View text=text_view; // the scalar stores below must not force reloads
    auto* output=reinterpret_cast<u32*>(storage.components.data);u32 count=0;reset_value(&scalars);
    const u64 capacity=static_cast<u64>(storage.components.count)*sizeof(SfntComponentNode)/sizeof(u32);
    for(u32 byte=0;byte<text.bytes;){const auto decoded=utf8_decode_one(text.data+byte,text.bytes-byte);
        if(!decoded.valid)return SfntStatus::invalid_input;if(count>=capacity)return SfntStatus::output_too_small;
        output[count++]=decoded.codepoint;byte+=decoded.width;}
    scalars={output,count};return SfntStatus::ok;
}
SfntStatus script_run_begin(const Utf8View& text,const SfntShapeStorage& storage,const SfntShapeLimits& limits,float spacing,ConstSlice<u32>* scalars) noexcept {
    if((text.bytes&&!text.data)||(storage.components.count&&!storage.components.data))return SfntStatus::invalid_input;
    if(!sfnt_trailing_spacing_valid(spacing))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    return scalars?decode_run_scalars(text,storage,*scalars):SfntStatus::ok;
}
SfntStatus script_run_finish(SfntStatus status,const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,float spacing,bool rtl) noexcept {
    if(status!=SfntStatus::ok||!spacing)return status;
    status=sfnt_trailing_spacing({storage.placements.data,result->glyph_count},{storage.flags.data,result->glyph_count},spacing,limits.stage_work,rtl);
    if(status!=SfntStatus::ok)result->glyph_count=result->source_units=result->target_count=0;
    return status;
}
SfntStatus compact_shape_result(const NativeAsciiStorage& storage,const SfntShapeRunResult& shaped,NativeAsciiResult* result) noexcept {
    // compact_run_begin already validated and cleared the result and borrowed views.
    if(shaped.glyph_count>storage.glyphs.count||shaped.glyph_count>storage.placements.count||shaped.source_units>storage.character_glyphs.count)
        return SfntStatus::output_too_small;
    for(u32 i=0;i<shaped.glyph_count;++i)if(!storage.shaping.glyphs[i].glyph.glyph)return SfntStatus::unsupported_sequence;
    if(storage.character_glyphs.data!=storage.shaping.character_glyphs.data)
        copy_bytes(storage.character_glyphs.data,storage.shaping.character_glyphs.data,static_cast<usize>(shaped.source_units)*sizeof(u32));
    if(storage.placements.data!=storage.shaping.placements.data)
        copy_bytes(storage.placements.data,storage.shaping.placements.data,static_cast<usize>(shaped.glyph_count)*sizeof(SfntPlacement));
    for(u32 i=0;i<shaped.glyph_count;++i)storage.glyphs[i]=storage.shaping.glyphs[i].glyph.glyph;
    *result={shaped.glyph_count,shaped.source_units,0,shaped.source_units?1u:0u,shaped.lookup_operations};return SfntStatus::ok;
}
SfntStatus compact_run_begin(const NativeAsciiStorage& storage,NativeAsciiResult* result) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
        (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
    return SfntStatus::ok;
}
}
