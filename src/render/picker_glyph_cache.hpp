#pragma once
#include "maicord/render/d3d11_glyph_text.hpp"

namespace maicord {
// One bounded visible-neighborhood cache. It owns only copied quads/UTF-8,
// never font handles or pointers into a recycled picker cell.
struct PickerGlyphCache {
    struct Entry {
        TextSystem system;
        GlyphQuad quads[4];
        char text[64];
        Color color;
        float width,height,shaped_height;
        u32 bytes,count,dpi,flags,evictions,resets;
    } entries[256];
    u16 cursor,lookup_start;
    u32 hits,misses,probes;
};
// The comparison counter consumes the owner's existing tail padding.
static_assert(sizeof(PickerGlyphCache) == sizeof(PickerGlyphCache::Entry) * 256 + 16);
inline bool picker_glyph_cache_matches(const PickerGlyphCache::Entry& e,
                                       const D3D11GlyphTextOwner& owner,const TextSystem& system,
                                       Utf8View input,float width,float height,Color color,u32 dpi,u32 flags) noexcept {
    if(!e.bytes||e.bytes!=input.bytes||e.width!=width||e.height!=height||e.dpi!=dpi||e.flags!=flags||
       e.evictions!=owner.allocator.evicted_entries||e.resets!=owner.allocator.full_resets||
       e.color.red!=color.red||e.color.green!=color.green||e.color.blue!=color.blue||e.color.alpha!=color.alpha||
       e.system.native_text!=system.native_text||e.system.native_format!=system.native_format||
       e.system.em_size!=system.em_size||e.system.tracking!=system.tracking||
       e.system.fixed_weight!=system.fixed_weight)return false;
    for(u32 i=0;i<input.bytes;++i)if(e.text[i]!=input.data[i])return false;
    return true;
}
inline const PickerGlyphCache::Entry* picker_glyph_cache_find(
    PickerGlyphCache& cache,const D3D11GlyphTextOwner& owner,const TextSystem& system,
    Utf8View input,float width,float height,Color color,u32 dpi,u32 flags,u32 capacity) noexcept {
    // Picker commands usually walk consecutive ring entries. Test the next slot
    // once, then preserve the original slot-zero scan excluding that tested slot.
    // Unrelated hits cost at most one extra comparison; a miss still costs 256.
    const u32 hint=cache.lookup_start&255u;
    for(u32 probe=0;probe<256;++probe) {
        const u32 slot=probe==0?hint:(probe-1<hint?probe-1:probe);
        const auto& entry=cache.entries[slot];
        if(!picker_glyph_cache_matches(entry,owner,system,input,width,height,color,dpi,flags) ||
           entry.count>capacity)continue;
        cache.lookup_start=static_cast<u16>((slot+1)&255u);
        cache.probes+=probe+1;
        return &entry;
    }
    cache.probes+=256;
    return nullptr;
}
} // namespace maicord
