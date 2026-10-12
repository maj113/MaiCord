#include "compact_run.hpp"
namespace maicord {
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
