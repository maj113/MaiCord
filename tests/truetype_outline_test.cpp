#include "maicord/text/truetype_outline.hpp"
#include "maicord/text/font_geometry.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

u8 data[4096];
SfntOutlinePoint points[128];
u32 ends[64];
u16 active[64];
GlyphPathCommand commands[256];
u32 cursor;

void put16(u8* p, u16 value) noexcept { p[0] = static_cast<u8>(value >> 8); p[1] = static_cast<u8>(value); }
void put32(u8* p, u32 value) noexcept { byte_store_be32(p, value); }
void record(u32 i, u32 tag, u32 offset, u32 length) noexcept {
    u8* p = data + 12 + i * 16;
    put32(p, tag); put32(p + 8, offset); put32(p + 12, length);
}
void location(u32 glyph) noexcept { put32(data + 272 + glyph * 4, cursor - 512); }

struct Point { i16 x, y; bool on; };
void simple(u32 glyph, const Point* source, u32 count) noexcept {
    location(glyph);
    u8* p = data + cursor;
    put16(p, 1); put16(p + 6, 100); put16(p + 8, 100);
    put16(p + 10, static_cast<u16>(count - 1)); put16(p + 12, 0);
    for (u32 i = 0; i < count; ++i) p[14 + i] = source[i].on ? 1 : 0;
    u32 at = 14 + count;
    for (u32 axis = 0; axis < 2; ++axis) {
        i16 previous = 0;
        for (u32 i = 0; i < count; ++i) {
            const i16 value = axis ? source[i].y : source[i].x;
            put16(p + at, static_cast<u16>(value - previous)); at += 2; previous = value;
        }
    }
    cursor += (at + 1u) & ~1u;
}

void fixture() noexcept {
    clear_bytes(data, sizeof(data));
    put32(data, 0x10000); put16(data + 4, 6);
    record(0, sfnt_tag('g','l','y','f'), 512, 0);
    record(1, sfnt_tag('h','e','a','d'), 128, 54);
    record(2, sfnt_tag('h','h','e','a'), 184, 36);
    record(3, sfnt_tag('h','m','t','x'), 224, 32);
    record(4, sfnt_tag('l','o','c','a'), 272, 36);
    record(5, sfnt_tag('m','a','x','p'), 320, 32);
    put32(data + 128, 0x10000); put32(data + 140, 0x5f0f3cf5);
    put16(data + 144, 2); put16(data + 146, 1000); put16(data + 178, 1);
    put32(data + 184, 0x10000); put16(data + 218, 8);
    for (u32 i = 0; i < 8; ++i) put16(data + 224 + i * 4, 500);
    put32(data + 320, 0x10000); put16(data + 324, 8);
    cursor = 512;
    const Point triangle[]{{0,0,true},{100,0,true},{0,100,true}};
    simple(0, triangle, 3);
    location(1);
    u8* p = data + cursor; put16(p, 0xffff); put16(p + 10, 3); put16(p + 12, 0);
    put16(p + 14, 50); put16(p + 16, static_cast<u16>(-25)); cursor += 18;
    location(2);
    p = data + cursor; put16(p, 0xffff); put16(p + 10, 0x23); put16(p + 12, 0);
    put16(p + 18, 1); put16(p + 20, 0); put16(p + 22, 1); put16(p + 24, 0); cursor += 26;
    location(3);
    p = data + cursor; put16(p, 0xffff); put16(p + 10, 0x83); put16(p + 12, 0);
    put16(p + 18, 8192); put16(p + 20, 4096); put16(p + 22, static_cast<u16>(-4096));
    put16(p + 24, 16384); cursor += 26;
    location(4);
    const Point curve[]{{0,0,true},{50,100,false},{100,0,true}};
    simple(5, curve, 3);
    const Point one[]{{25,25,true}};
    simple(6, one, 1);
    const Point off[]{{0,0,false},{50,100,false},{100,0,false}};
    simple(7, off, 3);
    location(8);
    put32(data + 24, cursor - 512);
}

SfntStatus open(SfntTrueType* font) noexcept {
    SfntFace face{};
    const auto status = sfnt_open({data, sizeof(data)}, 0, &face);
    return status == SfntStatus::ok ? sfnt_truetype_open(face, font) : status;
}

SfntOutlineScratch scratch() noexcept {
    return {{points,128},{ends,64},{active,64},0,0,10000};
}
GlyphOutline outline() noexcept {
    GlyphOutline out{}; out.commands = commands; out.capacity = 256; return out;
}
bool nearly_equal(float actual, float expected) noexcept {
    return actual >= expected - 1.e-7f && actual <= expected + 1.e-7f;
}

bool capacity_cases() noexcept {
    u8 offsets[8]{},glyph[16]{};SfntTrueType font{};
    font.loca={offsets,8};font.glyf={glyph,16};font.loca_stride=4;font.metrics.glyph_count=1;
    if(sfnt_truetype_outline_capacity(font,0)!=1||sfnt_truetype_outline_capacity(font,1))return false;
    put32(offsets+4,14);put16(glyph,2);put16(glyph+10,1);put16(glyph+12,5);
    if(sfnt_truetype_outline_capacity(font,0)!=10)return false;
    for(u32 length=0;length<8;++length){font.loca.count=length;if(sfnt_truetype_outline_capacity(font,0))return false;}font.loca.count=8;
    for(u32 length=0;length<14;++length){font.glyf.count=length;if(sfnt_truetype_outline_capacity(font,0))return false;}font.glyf.count=16;
    put32(offsets+4,11);if(sfnt_truetype_outline_capacity(font,0))return false;
    put32(offsets+4,14);put16(glyph,0xffff);if(sfnt_truetype_outline_capacity(font,0))return false;
    put16(glyph,0);if(sfnt_truetype_outline_capacity(font,0)!=1)return false;
    put16(glyph,1);put16(glyph+10,0xffff);if(sfnt_truetype_outline_capacity(font,0)!=65538)return false;
    font.loca_stride=3;if(sfnt_truetype_outline_capacity(font,0))return false;
    font.loca_stride=2;font.loca.count=4;put16(offsets,0);put16(offsets+2,7);
    if(sfnt_truetype_outline_capacity(font,0)!=65538)return false;
    put16(offsets,8);if(sfnt_truetype_outline_capacity(font,0))return false;
    fixture();if(open(&font)!=SfntStatus::ok)return false;
    for(u16 id=0;id<8;++id){const u32 bound=sfnt_truetype_outline_capacity(font,id);if(!bound)continue;
        auto work=scratch();auto full=outline();if(sfnt_truetype_outline(font,id,&work,&full,SfntOutlineMode::colr)!=SfntStatus::ok||full.count>bound)return false;
        GlyphPathCommand expected[256];copy_bytes(expected,commands,full.count*sizeof(GlyphPathCommand));const u32 count=full.count;
        auto compact=outline();compact.capacity=minimum<u32>(bound,256);work=scratch();
        if(sfnt_truetype_outline(font,id,&work,&compact,SfntOutlineMode::colr)!=SfntStatus::ok||compact.count!=count||!bytes_equal(expected,commands,count*sizeof(GlyphPathCommand)))return false;
    }
    return true;
}

bool valid_cases() noexcept {
    fixture(); SfntTrueType font{};
    if (open(&font) != SfntStatus::ok) return false;
    auto s = scratch(); auto out = outline();
    if (sfnt_truetype_outline(font, 0, &s, &out) != SfntStatus::ok || out.count != 4 ||
        out.fill_rule != GlyphFillRule::winding || !out.has_bounds ||
        !nearly_equal(out.maximum_x, .1f) || !nearly_equal(out.minimum_y, -.1f) || out.commands[3].kind != GlyphPathCommandKind::end_closed)
        return false;
    if (sfnt_truetype_outline(font, 1, &s, &out) != SfntStatus::ok || out.count != 4 ||
        !nearly_equal(out.commands[0].x1, .05f) || !nearly_equal(out.commands[0].y1, .025f) ||
        !nearly_equal(out.commands[1].x1, .15f)) return false;
    if (sfnt_truetype_outline(font, 2, &s, &out) != SfntStatus::ok || out.count != 8 ||
        s.point_count != 6 || !nearly_equal(out.commands[4].x1, .1f) || !nearly_equal(out.commands[5].x1, .2f)) return false;
    if (sfnt_truetype_outline(font, 3, &s, &out) != SfntStatus::ok || out.count != 4 ||
        !nearly_equal(out.commands[1].x1, .05f) || !nearly_equal(out.commands[1].y1, -.025f) ||
        !nearly_equal(out.commands[2].x1, -.025f) || !nearly_equal(out.commands[2].y1, -.1f)) return false;
    if (sfnt_truetype_outline(font, 4, &s, &out) != SfntStatus::ok || out.count || out.has_bounds) return false;
    if (sfnt_truetype_outline(font, 5, &s, &out) != SfntStatus::ok || out.count != 3 ||
        out.commands[1].kind != GlyphPathCommandKind::cubic || !nearly_equal(out.commands[1].x1, 1.f/30.f) ||
        !nearly_equal(out.commands[1].y1, -1.f/15.f) || !nearly_equal(out.commands[1].x2, 1.f/15.f) ||
        !nearly_equal(out.commands[1].x3, .1f)) return false;
    if (sfnt_truetype_outline(font, 6, &s, &out) != SfntStatus::ok || out.count || out.has_bounds) return false;
    if (sfnt_truetype_outline(font, 7, &s, &out) != SfntStatus::ok || out.count != 5 ||
        !nearly_equal(out.commands[0].x1, .05f) || !nearly_equal(out.commands[3].x3, .05f)) return false;
    // Short loca indexes hold half-byte offsets; compare all glyph commands.
    SfntFace face{};FontGeometry prepared_font{};FontGeometryInstance instance{};GlyphPathCommand expected[256];
    if(sfnt_open({data,sizeof(data)},0,&face)!=SfntStatus::ok || font_geometry_open(face,&prepared_font)!=SfntStatus::ok || font_geometry_instance(prepared_font,{},{},&instance,0)!=SfntStatus::ok || instance.variation)return false;
    for(u16 glyph=0;glyph<8;++glyph){s=scratch();out=outline();if(sfnt_truetype_outline(font,glyph,&s,&out)!=SfntStatus::ok)return false;const u32 count=out.count;copy_bytes(expected,commands,count*sizeof(GlyphPathCommand));
        FontGeometryScratch geometry_scratch{scratch(),{}, {}};if(font_geometry_outline(instance,glyph,&geometry_scratch,&out)!=SfntStatus::ok || out.count!=count || !bytes_equal(expected,commands,count*sizeof(GlyphPathCommand)))return false;}
    for (u32 i = 0; i < 9; ++i) put16(data + 272 + i * 2, static_cast<u16>(byte_load_be32(data + 272 + i * 4) / 2));
    put16(data + 178, 0); put32(data + 12 + 4 * 16 + 12, 18);
    if (open(&font) != SfntStatus::ok || font.loca_stride != 2) return false;
    return sfnt_truetype_outline(font, 3, &s, &out) == SfntStatus::ok && out.count == 4;
}

bool color_commands_cases() noexcept {
    fixture();SfntTrueType font{};if(open(&font)!=SfntStatus::ok)return false;
    for(u16 glyph=0;glyph<8;++glyph){
        auto s=scratch();auto out=outline();
        if(sfnt_truetype_outline(font,glyph,&s,&out,SfntOutlineMode::colr)!=SfntStatus::ok||
           out.has_bounds||out.minimum_x||out.minimum_y||out.maximum_x||out.maximum_y||out.fill_rule!=GlyphFillRule::winding)return false;
        const u32 count=out.count;
        for(u32 capacity=0;capacity<count;++capacity){out.capacity=capacity;
            if(sfnt_truetype_outline(font,glyph,&s,&out,SfntOutlineMode::colr)!=SfntStatus::output_too_small||
               out.count||out.has_bounds||!out.rejected||s.point_count||s.contour_count)return false;}
        for(u32 work=0;work<16;++work){
            s=scratch();s.work_limit=work;out=outline();
            const auto status=sfnt_truetype_outline(font,glyph,&s,&out,SfntOutlineMode::colr);
            if(out.has_bounds||(status!=SfntStatus::ok&&(out.count||s.point_count||s.contour_count)))return false;
        }
    }
    return true;
}

bool rejected_cases() noexcept {
    fixture(); SfntTrueType font{};
    if (open(&font) != SfntStatus::ok) return false;
    auto s = scratch(); auto out = outline();
    out.capacity = 2;
    if (sfnt_truetype_outline(font, 0, &s, &out) != SfntStatus::output_too_small ||
        out.count || out.has_bounds || !out.rejected) return false;
    out.capacity = 256; s.points.count = 2;
    if (sfnt_truetype_outline(font, 0, &s, &out) != SfntStatus::output_too_small || s.point_count) return false;
    s = scratch(); s.work_limit = 1;
    if (sfnt_truetype_outline(font, 0, &s, &out) != SfntStatus::resource_limit) return false;
    s = scratch(); s.active_glyphs.count = 1;
    if (sfnt_truetype_outline(font, 1, &s, &out) != SfntStatus::resource_limit) return false;
    s = scratch();
    if (sfnt_truetype_outline(font, 8, &s, &out) != SfntStatus::glyph_out_of_range) return false;
    // Self-referencing compound is rejected without treating repeated siblings
    // in valid_cases as a cycle.
    const u32 compound = 512 + byte_load_be32(data + 276);
    put16(data + compound + 12, 1);
    if (sfnt_truetype_outline(font, 1, &s, &out) != SfntStatus::malformed) return false;
    put16(data + compound + 12, 0);
    const u32 attachment = 512 + byte_load_be32(data + 280);
    put16(data + attachment + 22, 3);
    if (sfnt_truetype_outline(font, 2, &s, &out) != SfntStatus::unsupported || out.count) return false;
    // A repeat count cannot overrun the declared point array.
    data[512 + 14] = 9; data[512 + 15] = 255;
    if (sfnt_truetype_outline(font, 0, &s, &out) != SfntStatus::malformed) return false;
    fixture(); put32(data + 280, 1); // non-monotonic loca
    return open(&font) == SfntStatus::ok && sfnt_truetype_outline(font,1,&s,&out)==SfntStatus::malformed && !out.count && !s.point_count;
}

bool location_cases() noexcept {
    for(u32 stride=2;stride<=4;stride+=2) {
        fixture();
        if(stride==2){for(u32 i=0;i<=8;++i)put16(data+272+i*2,static_cast<u16>(byte_load_be32(data+272+i*4)/2));
            put16(data+178,0);put32(data+12+4*16+12,18);}
        SfntTrueType font{};if(open(&font)!=SfntStatus::ok)return false;
        auto s=scratch();auto out=outline();
        if(sfnt_truetype_outline(font,0,&s,&out)!=SfntStatus::ok||out.count>8)return false;
        GlyphPathCommand expected[8];const u32 count=out.count;copy_bytes(expected,commands,count*sizeof(*commands));
        const auto offset=[&](u32 glyph,u32 value) noexcept {if(stride==2)put16(data+272+glyph*2,static_cast<u16>(value/2));else put32(data+272+glyph*4,value);};
        offset(8,font.glyf.count+2);
        if(open(&font)!=SfntStatus::ok||sfnt_truetype_outline_capacity(font,7)||
           sfnt_truetype_outline(font,7,&s,&out)!=SfntStatus::malformed||out.count||out.has_bounds||s.point_count||s.contour_count)return false;
        if(sfnt_truetype_outline(font,0,&s,&out)!=SfntStatus::ok||out.count!=count||!bytes_equal(expected,commands,count*sizeof(*commands)))return false;
        offset(7,0);
        if(open(&font)!=SfntStatus::ok||sfnt_truetype_outline_capacity(font,6)||
           sfnt_truetype_outline(font,6,&s,&out)!=SfntStatus::malformed||out.count||s.point_count||s.contour_count)return false;
        bool present=true;if(sfnt_truetype_has_instructions(font,&present)!=SfntStatus::malformed||present)return false;
        const u32 child_end=stride==2?byte_load_be16(data+274)*2u:byte_load_be32(data+276);
        offset(0,child_end+2);
        if(open(&font)!=SfntStatus::ok||sfnt_truetype_outline(font,1,&s,&out)!=SfntStatus::malformed||out.count||s.point_count||s.contour_count)return false;
        put32(data+12+4*16+12,8*stride);
        if(open(&font)!=SfntStatus::truncated||font.loca.data)return false;
    }return true;
}

bool component_precision_cases() noexcept {
    fixture();
    const u32 at = 512 + byte_load_be32(data + 284);
    // At point (100,0), (100 * 1/16384) rounds to 0, and at (0,100)
    // the negative product also rounds to 0. Each product rounds separately.
    put16(data + at + 18, 1); put16(data + at + 20, 1);
    put16(data + at + 22, 1); put16(data + at + 24, 1);
    SfntTrueType font{}; if (open(&font) != SfntStatus::ok) return false;
    auto s = scratch(); auto out = outline();
    if (sfnt_truetype_outline(font, 3, &s, &out) != SfntStatus::ok ||
        out.maximum_x != 0 || out.minimum_y != 0) return false;
    // The root's hmtx bearing is applied once, not to every component.
    put16(data + 144, 0); put16(data + 224 + 3 * 4 + 2, 20);
    if (open(&font) != SfntStatus::ok) return false;
    return sfnt_truetype_outline(font, 3, &s, &out) == SfntStatus::ok && nearly_equal(out.minimum_x, .02f);
}

u32 float_bits(float value) noexcept {
    u32 bits = 0; copy_bytes(&bits, &value, sizeof(value)); return bits;
}

bool exact_rounding_cases() noexcept {
    fixture();
    cursor = 512 + byte_load_be32(data + 272 + 7 * 4);
    const Point curve[]{{291,-18,true},{245,-18,false},{180,46,false},{180,92,true}};
    simple(7, curve, 4); location(8); put32(data + 24, cursor - 512); put16(data + 146, 2048);
    SfntTrueType font{}; if (open(&font) != SfntStatus::ok) return false;
    auto s = scratch(); auto out = outline();
    if (sfnt_truetype_outline(font, 7, &s, &out) != SfntStatus::ok ||
        float_bits(out.commands[1].x1) != 1040329386u ||
        float_bits(out.commands[1].x2) != 1038756522u) return false;
    // An implied midpoint is formed before the non-power-of-two em scale.
    fixture(); cursor = 512 + byte_load_be32(data + 272 + 7 * 4);
    const Point offcurve[]{{2,0,false},{5,7,false},{3,0,false}};
    simple(7, offcurve, 3); location(8); put32(data + 24, cursor - 512);
    if (open(&font) != SfntStatus::ok || sfnt_truetype_outline(font, 7, &s, &out) != SfntStatus::ok ||
        float_bits(out.commands[0].x1) != 992204555u || float_bits(out.commands[0].y1) != 0) return false;
    // USE_MY_METRICS selects the child's origin, excluding its XY offset.
    fixture(); put16(data + 144, 0); put16(data + 226, 13); put16(data + 230, 20);
    const u32 compound = 512 + byte_load_be32(data + 276);
    put16(data + compound + 10, 0x203);
    if (open(&font) != SfntStatus::ok || sfnt_truetype_outline(font, 1, &s, &out) != SfntStatus::ok) return false;
    return nearly_equal(out.commands[0].x1, .063f);
}

bool color_precision_cases() noexcept {
    fixture();SfntTrueType font{};if(open(&font)!=SfntStatus::ok)return false;
    auto s=scratch();auto out=outline();
    if(sfnt_truetype_outline(font,5,&s,&out)!=SfntStatus::ok)return false;
    if(float_bits(out.commands[1].x1)!=0x3d088888)return false;
    if(sfnt_truetype_outline(font,5,&s,&out,SfntOutlineMode::colr)!=SfntStatus::ok)return false;
    if(float_bits(out.commands[1].x1)!=0x3d088889)return false;
    if(sfnt_truetype_outline(font,6,&s,&out)!=SfntStatus::ok || out.count!=0)return false;
    if(sfnt_truetype_outline(font,6,&s,&out,SfntOutlineMode::colr)!=SfntStatus::ok || out.count!=2 ||
       out.commands[0].kind!=GlyphPathCommandKind::begin_filled || out.commands[1].kind!=GlyphPathCommandKind::end_closed)return false;
    if(sfnt_truetype_outline(font,1,&s,&out,SfntOutlineMode::colr)!=SfntStatus::ok)return false;
    const auto first=out.commands[0];
    return sfnt_truetype_outline(font,1,&s,&out)==SfntStatus::ok && first.x1==out.commands[0].x1 && first.y1==out.commands[0].y1;
}

bool coordinate_stream_cases() noexcept {
    constexpr u32 maximum_points=65536,glyph_capacity=14+512+maximum_points*4;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,4*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!memory)return false;
    struct Release {u8* p;~Release() noexcept {VirtualFree(p,0,MEM_RELEASE);}} release{memory};
    auto* decoded=reinterpret_cast<SfntOutlinePoint*>(memory+((glyph_capacity+3)&~3u));
    auto* contour_end=reinterpret_cast<u32*>(decoded+maximum_points);
    auto* active_glyphs=reinterpret_cast<u16*>(contour_end+1);
    auto* output_commands=reinterpret_cast<GlyphPathCommand*>(active_glyphs+64);
    auto* expected_x=reinterpret_cast<float*>(output_commands+maximum_points+1);
    auto* expected_y=expected_x+maximum_points;
    u8 locations[8]{};
    SfntTrueType font{};font.loca={locations,8};font.loca_stride=4;
    font.metrics.units_per_em=16;font.metrics.glyph_count=1;font.bearing_equals_minimum=true;
    for(u32 kind=0;kind<4;++kind){
        const u32 count=kind==3?16:maximum_points;
        put16(memory,1);put16(memory+10,static_cast<u16>(count-1));put16(memory+12,0);
        const u8 repeated=kind==2?31:9;
        u32 at=14;
        if(kind==3){for(u32 i=0;i<count;++i)memory[at++]=static_cast<u8>(1|((i&3)<<1)|((i&12)<<2));}
        else for(u32 i=0;i<count/256;++i){memory[at++]=repeated;memory[at++]=255;}
        for(u32 axis=0;axis<2;++axis){
            i64 sum=0;auto* expected=axis?expected_y:expected_x;
            for(u32 i=0;i<count;++i){
                const u8 flags=kind==3?memory[14+i]:repeated;
                const u8 short_bit=axis?4:2,same_bit=axis?32:16;
                i32 delta=0;
                if(flags&short_bit){memory[at++]=255;delta=flags&same_bit?255:-255;}
                else if(!(flags&same_bit)){
                    delta=(axis!=(kind==1))?32767:-32768;
                    put16(memory+at,static_cast<u16>(delta));at+=2;
                }
                sum+=delta;expected[i]=static_cast<float>(sum);
            }
        }
        font.glyf={memory,at};put32(locations+4,at);
        for(u32 i=0;i<count;++i)decoded[i]={42,-9,255,{255,255,255}};
        SfntOutlineScratch s{{decoded,maximum_points},{contour_end,1},{active_glyphs,64},0,0,count+1};
        GlyphOutline out{output_commands,maximum_points+1};
        if(sfnt_truetype_outline(font,0,&s,&out)!=SfntStatus::ok||s.point_count!=count||out.count!=count+1||!out.has_bounds)return false;
        for(u32 i=0;i<count;++i){
            if(decoded[i].x!=expected_x[i]||decoded[i].y!=expected_y[i]||decoded[i].reserved[0]||decoded[i].reserved[1]||decoded[i].reserved[2]||
               output_commands[i].x1!=expected_x[i]/16.f||output_commands[i].y1!=-expected_y[i]/16.f)return false;
        }
        if(output_commands[count].kind!=GlyphPathCommandKind::end_closed)return false;
        // A missing final coordinate byte must fail before publishing commands.
        font.glyf.count=at-1;put32(locations+4,at-1);
        if(sfnt_truetype_outline(font,0,&s,&out)!=SfntStatus::truncated||out.count||out.has_bounds||!out.rejected||s.point_count||s.contour_count)return false;
        font.glyf.count=at;put32(locations+4,at);s.work_limit=count;
        if(sfnt_truetype_outline(font,0,&s,&out)!=SfntStatus::resource_limit||out.count||s.point_count||s.contour_count)return false;
        s.work_limit=count+1;out.capacity=count;
        if(sfnt_truetype_outline(font,0,&s,&out)!=SfntStatus::output_too_small||out.count||out.has_bounds||!out.rejected||s.point_count||s.contour_count)return false;
    }
    return true;
}

bool guarded_mutations() noexcept {
    fixture();
    auto* memory = static_cast<u8*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    if (!memory) return false;
    if (!VirtualAlloc(memory, 4096, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(memory, 0, MEM_RELEASE); return false;
    }
    const u32 length = cursor;
    u32 random = 0x19770613;
    bool bounded=true;
    for (u32 iteration = 0; iteration < 30000; ++iteration) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        const u32 size = iteration < length ? iteration : length;
        u8* input = memory + 4096 - size;
        copy_bytes(input, data, size);
        if (size && iteration >= length) input[random % size] ^= static_cast<u8>((random >> 17) | 1);
        SfntFace face{}; SfntTrueType font{};
        if (sfnt_open({input,size},0,&face) != SfntStatus::ok || sfnt_truetype_open(face,&font) != SfntStatus::ok) continue;
        auto s = scratch(); auto out = outline();
        sfnt_truetype_outline(font, static_cast<u16>(iteration % 8), &s, &out);
        const u16 glyph=static_cast<u16>(iteration%8);const u32 bound=sfnt_truetype_outline_capacity(font,glyph);
        const auto status=sfnt_truetype_outline(font,glyph,&s,&out,SfntOutlineMode::colr);
        if(status==SfntStatus::ok&&bound&&out.count>bound){bounded=false;break;}
        s.require_instructions=true;sfnt_truetype_outline(font,glyph,&s,&out);
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    return bounded;
}

} // namespace

bool truetype_outline_tests() noexcept {
    return capacity_cases() && valid_cases() && rejected_cases() && location_cases() && component_precision_cases() && exact_rounding_cases() && color_precision_cases() && color_commands_cases() && coordinate_stream_cases() && guarded_mutations();
}

} // namespace maicord
