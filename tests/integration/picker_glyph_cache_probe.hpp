#pragma once

#include "../../src/render/picker_glyph_cache.hpp"

namespace maicord {

// Exercise the production lookup with copied identities and recycled slots.
// Borrow the existing owner only while no rendering is in progress, then restore it.
inline bool picker_glyph_cache_lookup_probe(PickerGlyphCache& cache) noexcept {
    struct Restore {
        PickerGlyphCache& cache;
        PickerGlyphCache::Entry entries[3];
        u32 bytes[256];
        u16 cursor, lookup_start;
        u32 hits, misses, probes;
        explicit Restore(PickerGlyphCache& value) noexcept
            : cache(value), entries{value.entries[0], value.entries[7], value.entries[255]},
              cursor(value.cursor), lookup_start(value.lookup_start), hits(value.hits),
              misses(value.misses), probes(value.probes) {
            for (u32 i=0;i<256;++i) {
                bytes[i]=cache.entries[i].bytes;
                cache.entries[i].bytes=0;
            }
        }
        ~Restore() noexcept {
            for (u32 i=0;i<256;++i) cache.entries[i].bytes=bytes[i];
            cache.entries[0]=entries[0];cache.entries[7]=entries[1];cache.entries[255]=entries[2];
            cache.cursor=cursor;cache.lookup_start=lookup_start;
            cache.hits=hits;cache.misses=misses;cache.probes=probes;
        }
    } restore(cache);
    D3D11GlyphTextOwner owner{};
    TextSystem system{};
    system.em_size=32.f;
    const Color color{1.f,.5f,.25f,1.f};
    constexpr u32 slots[]{0,7,255};
    const auto find=[&](Utf8View input,u32 capacity=4) noexcept {
        return picker_glyph_cache_find(cache,owner,system,input,40.f,40.f,color,120,
                                       text_style_picker_emoji,capacity);
    };
    const u32 empty_probes=cache.probes;
    if(find(utf8_literal("a"))||cache.probes-empty_probes!=256)return false;
    for(u32 i=0;i<3;++i) {
        auto& e=cache.entries[slots[i]];
        e=PickerGlyphCache::Entry{};
        e.system=system;e.color=color;e.width=e.height=40.f;
        e.bytes=1;e.text[0]=static_cast<char>('a'+i);e.count=i==2?4:i;
        e.dpi=120;e.flags=text_style_picker_emoji;
    }
    for(u32 hint=0;hint<256;++hint) {
        for(u32 i=0;i<3;++i) {
            cache.lookup_start=static_cast<u16>(hint);
            const auto& e=cache.entries[slots[i]];
            const u32 before=cache.probes;
            if(find({e.text,e.bytes})!=&e)return false;
            const u32 cost=cache.probes-before;
            if(cost>slots[i]+2||cost>256||!cost||
               cache.lookup_start!=((slots[i]+1)&255))return false;
        }
        cache.lookup_start=static_cast<u16>(hint);
        const u32 before=cache.probes;
        if(find(utf8_literal("missing"))||cache.probes-before!=256)return false;
    }
    cache.lookup_start=255;
    if(find(utf8_literal("c"),3))return false;
    if(find(utf8_literal("a"),0)!=&cache.entries[0])return false;
    owner.allocator.evicted_entries=1;
    if(find(utf8_literal("a")))return false;
    owner.allocator.evicted_entries=0;owner.allocator.full_resets=1;
    if(find(utf8_literal("a")))return false;
    owner.allocator.full_resets=0;
    auto& e=cache.entries[7];
    const auto exact=e;
    for(u32 changed=0;changed<16;++changed) {
        e=exact;
        switch(changed) {
        case 0:e.text[0]='x';break;
        case 1:e.bytes=2;break;
        case 2:e.width+=1;break;
        case 3:e.height+=1;break;
        case 4:e.dpi+=1;break;
        case 5:e.flags^=text_style_bold;break;
        case 6:e.color.alpha=.5f;break;
        case 7:e.color.red=.5f;break;
        case 8:e.system.em_size+=1;break;
        case 9:e.system.tracking+=1;break;
        case 10:e.system.fixed_weight=true;break;
        case 11:e.system.native_format+=1;break;
        case 12:e.evictions+=1;break;
        case 13:e.resets+=1;break;
        case 14:e.color.green=.25f;break;
        case 15:e.color.blue=.5f;break;
        }
        cache.lookup_start=7;
        if(find(utf8_literal("b")))return false;
    }
    e=exact;
    e.system.native_format+=1;
    if(find(utf8_literal("b")))return false;
    e=exact;e.system.native_text=reinterpret_cast<NativeTextContext*>(usize{1});
    if(find(utf8_literal("b")))return false;
    e=exact;
    // The 16-bit insertion cursor produces the same ring positions across wrap.
    cache.cursor=65535;
    if(cache.cursor++%256!=255||cache.cursor++%256!=0)return false;
    cache.lookup_start=255;
    if(find(utf8_literal("c"))!=&cache.entries[255]||
       find(utf8_literal("a"))!=&cache.entries[0])return false;
    return true;
}

} // namespace maicord
