#include "compact_run.hpp"
namespace maicord {
SfntStatus decode_run_scalars(Utf8View text,const SfntShapeStorage& storage,ConstSlice<u32>& scalars) noexcept {
    auto* output=reinterpret_cast<u32*>(storage.components.data);u32 count=0;scalars={};
    const u64 capacity=static_cast<u64>(storage.components.count)*sizeof(SfntComponentNode)/sizeof(u32);
    for(u32 byte=0;byte<text.bytes;){const auto decoded=utf8_decode_one(text.data+byte,text.bytes-byte);
        if(!decoded.valid)return SfntStatus::invalid_input;if(count>=capacity)return SfntStatus::output_too_small;
        output[count++]=decoded.codepoint;byte+=decoded.width;}
    scalars={output,count};return SfntStatus::ok;
}
SfntStatus compact_shape_result(const NativeAsciiStorage& storage,const SfntShapeRunResult& shaped,NativeAsciiResult* result) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
       (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
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
}
