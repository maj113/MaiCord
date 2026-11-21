#pragma once

#include "../../src/render/text/picker_glyph_cache.hpp"

namespace maicord {

// Exercise the production lookup with copied identities and recycled slots.
// Borrow the existing owner only while no rendering is in progress, then restore it.
inline bool picker_glyph_cache_lookup_probe(PickerGlyphCache& cache) noexcept {
    static PickerGlyphCache saved;
    saved = cache;
    struct Restore {
        PickerGlyphCache& cache;
        ~Restore() noexcept {
            cache = saved;
        }
    } restore{cache};
    for (auto& entry : cache.entries)
        entry.bytes = 0;
    D3D11GlyphTextOwner owner{};
    TextSystem system{};
    system.em_size=32.f;
    const Color color{1.f,.5f,.25f,1.f};
    constexpr u32 slots[]{0,7,255};
    const auto find=[&](Utf8View input,u32 capacity=4) noexcept {
        return picker_glyph_cache_find(cache,owner,system,input,40.f,40.f,color,120,
                                       text_style_picker_emoji,capacity);
    };
    if(find(utf8_literal("a")))return false;
    for(u32 i=0;i<3;++i) {
        auto& e=cache.entries[slots[i]];
        e=PickerGlyphCache::Entry{};
        e.native_text=system.native_text;e.native_format=system.native_format;
        e.em_size=system.em_size;e.tracking=system.tracking;e.fixed_weight=system.fixed_weight;
        e.color=color;e.width=e.height=40.f;
        e.bytes=1;e.text[0]=static_cast<char>('a'+i);e.count=static_cast<u8>(i==2?4:i);
        e.dpi=120;e.flags=text_style_picker_emoji;
    }
    for(u32 hint=0;hint<256;++hint) {
        for(u32 i=0;i<3;++i) {
            cache.lookup_start=static_cast<u16>(hint);
            const auto& e=cache.entries[slots[i]];
            if(find({e.text,e.bytes})!=&e||cache.lookup_start!=((slots[i]+1)&255))return false;
        }
        // Of two equal entries the lookup finds the one in the hint slot: it tests that slot first.
        if(hint!=0&&hint!=7&&hint!=255) {
            cache.entries[hint]=cache.entries[0];
            cache.lookup_start=static_cast<u16>(hint);
            const bool first=find(utf8_literal("a"))==&cache.entries[hint];
            cache.entries[hint].bytes=0;
            if(!first)return false;
        }
        cache.lookup_start=static_cast<u16>(hint);
        if(find(utf8_literal("missing")))return false;
    }
    // After the hint the lookup tests the slots in order: of two equal entries it finds the lower one.
    cache.entries[100]=cache.entries[7];
    cache.lookup_start=50;
    if(find(utf8_literal("b"))!=&cache.entries[7])return false;
    cache.entries[100].bytes=0;
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
        case 8:e.em_size+=1;break;
        case 9:e.tracking+=1;break;
        case 10:e.fixed_weight=true;break;
        case 11:e.native_format+=1;break;
        case 12:e.evictions+=1;break;
        case 13:e.resets+=1;break;
        case 14:e.color.green=.25f;break;
        case 15:e.color.blue=.5f;break;
        }
        cache.lookup_start=7;
        if(find(utf8_literal("b")))return false;
    }
    e=exact;
    e.native_format+=1;
    if(find(utf8_literal("b")))return false;
    e=exact;e.native_text=reinterpret_cast<NativeTextContext*>(usize{1});
    if(find(utf8_literal("b")))return false;
    e=exact;
    // The largest admitted UTF-8 key still fits the compact byte count.
    e.bytes=64;
    for(u32 i=0;i<64;++i)e.text[i]=static_cast<char>('a'+i%26);
    if(find({e.text,64})!=&e||find({e.text,63}))return false;
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
