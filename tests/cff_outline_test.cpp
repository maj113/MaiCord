#include "maicord/text/cff_outline.hpp"
#include "maicord/text/font_geometry.hpp"
#include "maicord/core/byte_order.hpp"
#include "../src/text/cff_internal.hpp"
#include <Windows.h>

namespace maicord {
namespace {

#include "generated/cff_oblique_fixture.inc"
#include "generated/cff_bold_hashes.inc"
u8 bytes[3072];
GlyphPathCommand commands[256];
constexpr u32 cff_at = 512, cff_size = 2048;

void put16(u8* p, u16 v) noexcept { p[0]=static_cast<u8>(v>>8); p[1]=static_cast<u8>(v); }
void put32(u8* p, u32 v) noexcept { byte_store_be32(p,v); }
void table(u32 i, u32 tag, u32 at, u32 size) noexcept {
    auto* p=bytes+12+i*16; put32(p,tag); put32(p+8,at); put32(p+12,size);
}
u32 single_index(u8* p, ConstSlice<u8> object) noexcept {
    put16(p,1); p[2]=2; put16(p+3,1); put16(p+5,static_cast<u16>(object.count+1));
    copy_bytes(p+7,object.data,object.count); return object.count+7;
}
void fixture(ConstSlice<u8> program) noexcept {
    clear_bytes(bytes,sizeof(bytes));
    put32(bytes,sfnt_tag('O','T','T','O')); put16(bytes+4,5);
    table(0,sfnt_tag('C','F','F',' '),cff_at,cff_size);
    table(1,sfnt_tag('h','e','a','d'),128,54);
    table(2,sfnt_tag('h','h','e','a'),184,36);
    table(3,sfnt_tag('h','m','t','x'),224,4);
    table(4,sfnt_tag('m','a','x','p'),256,6);
    put32(bytes+128,0x10000); put32(bytes+140,0x5f0f3cf5); put16(bytes+146,1000);
    put32(bytes+184,0x10000); put16(bytes+218,1); put16(bytes+224,500);
    put32(bytes+256,0x5000); put16(bytes+260,1);
    u8* p=bytes+cff_at; p[0]=1; p[2]=4; p[3]=4;
    const u8 name[]{'T'};
    u32 at=4+single_index(p+4,{name,1});
    // Fixed offsets keep malformed-program tests independent of the writer.
    const u8 top[]{29,0,0,0,128,17,29,0,0,0,6,29,0,0,7,8,18};
    at+=single_index(p+at,{top,sizeof(top)});
    put16(p+at,0); at+=2; // String INDEX
    const u8 subr[]{149,139,5,11};
    single_index(p+at,{subr,sizeof(subr)});
    single_index(p+128,program);
    const u8 priv[]{29,0,0,0,16,19};
    copy_bytes(p+1800,priv,sizeof(priv));
    single_index(p+1816,{subr,sizeof(subr)});
}
SfntStatus open(ConstSlice<u8> input, CffFont* font) noexcept {
    SfntFace face{}; auto status=sfnt_open(input,0,&face);
    return status==SfntStatus::ok?cff_open(face,font):status;
}
SfntStatus run(ConstSlice<u8> program, GlyphOutline* out, u32 work=10000) noexcept {
    fixture(program);SfntFace face{};FontGeometry font{};FontGeometryInstance instance{};
    auto status=sfnt_open({bytes,sizeof(bytes)},0,&face);if(status!=SfntStatus::ok)return status;
    status=font_geometry_open(face,&font);if(status!=SfntStatus::ok)return status;
    status=font_geometry_instance(font,{},{},&instance,0);if(status!=SfntStatus::ok)return status;
    return font_geometry_outline(instance,0,nullptr,out,work);
}
GlyphOutline outline() noexcept { GlyphOutline out{}; out.commands=commands; out.capacity=256; return out; }
bool nearly_equal(float a,float b) noexcept { return a>=b-1.e-7f && a<=b+1.e-7f; }
bool rejected(ConstSlice<u8> program,SfntStatus expected,u32 work=10000) noexcept {
    auto out=outline(); const auto status=run(program,&out,work);
    return status==expected && !out.count && !out.has_bounds && out.rejected && out.commands==commands && out.capacity==256;
}

bool geometry() noexcept {
    const u8 line[]{149,159,21,239,139,139,239,39,139,5,14};
    auto out=outline();
    if(run({line,sizeof(line)},&out)!=SfntStatus::ok || out.count!=5 || !out.has_bounds ||
       out.fill_rule!=GlyphFillRule::winding || !nearly_equal(commands[0].x1,.01f) || !nearly_equal(commands[0].y1,-.02f) ||
       !nearly_equal(out.maximum_x,.11f) || !nearly_equal(out.minimum_y,-.12f) || commands[4].kind!=GlyphPathCommandKind::end_closed) return false;
    const u8 curve[]{139,139,21,149,159,169,179,189,199,8,14};
    if(run({curve,sizeof(curve)},&out)!=SfntStatus::ok || out.count!=3 ||
       commands[1].kind!=GlyphPathCommandKind::cubic || !nearly_equal(commands[1].x2,.04f) || !nearly_equal(commands[1].y3,-.12f)) return false;
    // Fractional Type2 16.16 numbers survive until conversion to output floats.
    const u8 fraction[]{255,0,10,64,0,255,0,20,192,0,21,149,139,5,14};
    if(run({fraction,sizeof(fraction)},&out)!=SfntStatus::ok || !nearly_equal(commands[0].x1,.01025f) || !nearly_equal(commands[0].y1,-.02075f)) return false;
    const u8 empty[]{159,169,21,14};
    if(run({empty,sizeof(empty)},&out)!=SfntStatus::ok || out.count || out.has_bounds) return false;
    const u8 subrs[]{139,139,21,32,10,32,29,14};
    if(run({subrs,sizeof(subrs)},&out)!=SfntStatus::ok || out.count!=4 || !nearly_equal(commands[2].x1,.02f)) return false;
    u32 zero_bits=1; copy_bytes(&zero_bits,&commands[0].y1,sizeof(zero_bits));
    if(zero_bits) return false;
    // Width and two hint bits must not turn the mask bytes into operators.
    const u8 hints[]{248,136,149,149,1,159,149,3,19,0xc0,139,139,21,149,139,5,14};
    if(run({hints,sizeof(hints)},&out)!=SfntStatus::ok || out.count!=3) return false;
    out.capacity=2;
    if(run({line,sizeof(line)},&out)!=SfntStatus::output_too_small || out.count || !out.rejected) return false;
    CffFont font{};
    if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::ok) return false;
    out=outline();
    return cff_outline(font,1,&out)==SfntStatus::glyph_out_of_range && !out.count && out.rejected;
}

bool invalid_programs() noexcept {
    const u8 missing_end[]{139,139,21,149,139,5};
    const u8 missing_move[]{149,139,5,14};
    const u8 short_number[]{255,0,0};
    const u8 short_mask[]{149,149,1,19};
    const u8 odd_lines[]{139,139,21,149,5,14};
    const u8 top_return[]{11};
    const u8 underflow[]{12,10,14};
    const u8 division[]{140,139,12,12,14};
    const u8 transient[]{140,171,12,20,14};
    const u8 unknown[]{2};
    const u8 mixed_seac[]{139,139,21,149,139,5,139,139,21,139,139,204,247,86,14};
    if(!rejected({missing_end,sizeof(missing_end)},SfntStatus::truncated) ||
       !rejected({missing_move,sizeof(missing_move)},SfntStatus::malformed) ||
       !rejected({short_number,sizeof(short_number)},SfntStatus::truncated) ||
       !rejected({short_mask,sizeof(short_mask)},SfntStatus::truncated) ||
       !rejected({odd_lines,sizeof(odd_lines)},SfntStatus::malformed) ||
       !rejected({top_return,sizeof(top_return)},SfntStatus::malformed) ||
       !rejected({underflow,sizeof(underflow)},SfntStatus::malformed) ||
       !rejected({division,sizeof(division)},SfntStatus::malformed) ||
       !rejected({transient,sizeof(transient)},SfntStatus::malformed) ||
       !rejected({unknown,sizeof(unknown)},SfntStatus::unsupported) ||
       !rejected({mixed_seac,sizeof(mixed_seac)},SfntStatus::malformed) ||
       !rejected({missing_end,sizeof(missing_end)},SfntStatus::resource_limit,1)) return false;
    u8 overflow[50]; for(u32 i=0;i<49;++i)overflow[i]=139; overflow[49]=14;
    if(!rejected({overflow,sizeof(overflow)},SfntStatus::malformed)) return false;
    const u8 recursive[]{32,10,14}; fixture({recursive,sizeof(recursive)});
    bytes[cff_at+1816+7]=32; bytes[cff_at+1816+8]=10;
    CffFont font{}; auto out=outline();
    return open({bytes,sizeof(bytes)},&font)==SfntStatus::ok &&
           cff_outline(font,0,&out)==SfntStatus::resource_limit && !out.count && out.rejected;
}

bool flex_round_trip() noexcept {
    const u8 program[]{139,139,21,140,141,142,143,144,145,146,12,34,14};
    fixture({program,sizeof(program)});
    const u8 top[]{29,0,0,0,128,17,29,0,0,0,6,29,0,0,7,8,18,
                   30,0x0a,0x00,0x11,0xff,139,139,30,0x0a,0x00,0x11,0xff,139,139,12,7};
    auto* p=bytes+cff_at;
    const u32 at=12+single_index(p+12,{top,sizeof(top)});
    put16(p+at,0); put16(p+at+2,0);
    CffFont font{}; auto out=outline();
    if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::ok ||
       cff_outline(font,0,&out)!=SfntStatus::ok || out.count!=4) return false;
    u32 midpoint=0, endpoint=0;
    copy_bytes(&midpoint,&commands[1].x3,sizeof(midpoint));
    copy_bytes(&endpoint,&commands[2].x3,sizeof(endpoint));
    // Exact Windows-oracle bits at uniform scale 1.1. An ordinary cubic must
    // retain its separate path: these values differ by one/four 16.16 units.
    return midpoint==1006391493u && endpoint==1021397025u;
}

bool resolved_metrics() noexcept {
    const u8 path[]{149,159,21,239,139,139,239,39,139,5,14};
    const u8 explicit_move[]{248,36,149,159,21,239,139,5,14};
    const u8 explicit_end[]{248,36,14};
    const u8 explicit_stem[]{248,36,149,149,1,149,159,21,239,139,5,14};
    const ConstSlice<u8> programs[]{{path,sizeof(path)},{explicit_move,sizeof(explicit_move)},
                                   {explicit_end,sizeof(explicit_end)},{explicit_stem,sizeof(explicit_stem)}};
    for (u32 i=0;i<4;++i) {
        fixture(programs[i]);
        // defaultWidthX=500, nominalWidthX=100. All explicit widths are 400.
        bytes[cff_at+29]=5;
        const u8 priv[]{248,136,20,239,21};
        copy_bytes(bytes+cff_at+1800,priv,sizeof(priv));
        CffFont font{}; auto out=outline(); CffGlyphMetrics metrics{};
        if (open({bytes,sizeof(bytes)},&font)!=SfntStatus::ok ||
            cff_outline(font,0,&out,10000,&metrics)!=SfntStatus::ok ||
            metrics.advance_x!=500 || metrics.advance_y!=0 || metrics.has_bounds!=(i!=2)) return false;
        if (i!=2 && (metrics.minimum_x!=10 || metrics.minimum_y!=20 || metrics.maximum_x!=110)) return false;
        if (i==0 && metrics.maximum_y!=120) return false;
        out.capacity=1;
        if (i!=2 && (cff_outline(font,0,&out,10000,&metrics)!=SfntStatus::output_too_small ||
                    metrics.advance_x || metrics.has_bounds || metrics.maximum_x || out.count)) return false;
    }
    return true;
}

bool structures() noexcept {
    const u8 end[]{14}; fixture({end,1}); CffFont font{};
    if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::ok) return false;
    bytes[cff_at]=2;
    if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::unsupported || font.table.data) return false;
    fixture({end,1}); bytes[cff_at+4+2]=0;
    if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::malformed) return false;
    fixture({end,1}); bytes[cff_at+128+4]=2; // INDEX must start at offset one.
    if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::malformed) return false;
    fixture({end,1}); bytes[cff_at+128+6]=0; // Decreasing INDEX offsets.
    if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::malformed) return false;
    cff_detail::Dictionary dict{};
    const u8 real[]{30,0xe0,0xa1,0x25,0xc2,0xff,30,0x0f,30,0x0f,30,0x0f,30,0x0f,30,0x0f,12,7};
    if(cff_detail::dictionary({real,sizeof(real)},&dict)!=SfntStatus::ok ||
       dict.matrix[0] < -.00125000001 || dict.matrix[0] > -.00124999999) return false;
    const u8 bad_real[]{30,0x1d,0xff};
    if(cff_detail::dictionary({bad_real,sizeof(bad_real)},&dict)!=SfntStatus::malformed) return false;
    const u8 truncated_real[]{30,0x12};
    if(cff_detail::dictionary({truncated_real,sizeof(truncated_real)},&dict)!=SfntStatus::truncated) return false;
    // StandardEncoding mappings and each predefined/custom charset encoding.
    font={}; font.glyph_count=229; u16 glyph=0;
    if(cff_detail::standard_glyph(font,65,&glyph)!=SfntStatus::ok || glyph!=34 ||
       cff_detail::standard_glyph(font,194,&glyph)!=SfntStatus::ok || glyph!=125 ||
       cff_detail::standard_glyph(font,225,&glyph)!=SfntStatus::ok || glyph!=138) return false;
    font.charset=1; font.glyph_count=166;
    if(cff_detail::standard_glyph(font,32,&glyph)!=SfntStatus::ok || glyph!=1) return false;
    font.charset=2; font.glyph_count=87;
    if(cff_detail::standard_glyph(font,32,&glyph)!=SfntStatus::ok || glyph!=1) return false;
    for(u32 format=0;format<3;++format) {
        u8 charset[]{0,0,0,0,static_cast<u8>(format),0,34,0,0};
        font.table={charset,sizeof(charset)}; font.charset=4; font.glyph_count=2;
        if(cff_detail::standard_glyph(font,65,&glyph)!=SfntStatus::ok || glyph!=1) return false;
    }
    return true;
}

bool cid_selection() noexcept {
    const u8 program[]{139,139,21,32,10,14};
    const u8 top[]{29,0,0,0,128,17,139,139,139,12,30,
                   29,0,0,0,160,12,36,29,0,0,0,200,12,37};
    const u8 fd[]{29,0,0,0,6,29,0,0,7,8,18};
    for(u32 format=0;format<2;++format) {
        fixture({program,sizeof(program)}); auto* p=bytes+cff_at;
        u32 at=12+single_index(p+12,{top,sizeof(top)});
        put16(p+at,0); put16(p+at+2,0);
        single_index(p+160,{fd,sizeof(fd)});
        if(format) { p[200]=3; put16(p+201,1); put16(p+203,0); p[205]=0; put16(p+206,1); }
        else { p[200]=0; p[201]=0; }
        CffFont font{}; auto out=outline();
        if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::ok || !font.cid ||
           cff_outline(font,0,&out)!=SfntStatus::ok || out.count!=3 || !nearly_equal(commands[1].x1,.01f)) return false;
        // FD indices must resolve before any charstring execution.
        p[format?205:201]=1;
        if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::malformed) return false;
        p[format?205:201]=0;
        if(format) {
            put16(p+206,2);
            if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::malformed) return false;
            put16(p+206,1); put16(p+203,1);
            if(open({bytes,sizeof(bytes)},&font)!=SfntStatus::malformed) return false;
        }
    }
    return true;
}

bool oblique_corpus() noexcept {
    struct Reader {
        u32 at=8;
        bool word(u32* value) noexcept {if(sizeof(cff_oblique_fixture)-at<4)return false;*value=byte_load_le32(cff_oblique_fixture+at);at+=4;return true;}
        bool take(u32 count,ConstSlice<u8>* value) noexcept {if(count>sizeof(cff_oblique_fixture)-at)return false;*value={cff_oblique_fixture+at,count};at+=count;return true;}
    } reader;
    u32 fonts=0,glyphs=0;if(!bytes_equal(cff_oblique_fixture,"MCCFFS1\0",8)||!reader.word(&fonts)||fonts!=8)return false;
    for(u32 f=0;f<fonts;++f){u32 size=0,count=0;ConstSlice<u8> data{};CffFont font{};
        if(!reader.word(&size)||!reader.word(&count)||!reader.take(size,&data)||open(data,&font)!=SfntStatus::ok||count!=font.glyph_count)return false;
        for(u32 i=0;i<count;++i){u32 id=0,n=0,fill=0,bounds=0;u32 expected_bounds[4];ConstSlice<u8> expected{};
            if(!reader.word(&id)||!reader.word(&n)||!reader.word(&fill)||!reader.word(&bounds)||id!=i||n>256)return false;
            for(auto& b:expected_bounds)if(!reader.word(&b))return false;if(!reader.take(n*28,&expected))return false;
            auto out=outline();CffGlyphMetrics raw{},styled{};
            if(cff_outline(font,static_cast<u16>(i),&out,1000000,&raw)!=SfntStatus::ok||cff_outline(font,static_cast<u16>(i),&out,1000000,&styled,true)!=SfntStatus::ok)return false;
            const float actual_bounds[]{out.minimum_x,out.minimum_y,out.maximum_x,out.maximum_y};
            if(out.count!=n||static_cast<u32>(out.fill_rule)!=fill||out.has_bounds!=(bounds!=0)||!bytes_equal(expected_bounds,actual_bounds,16)||!bytes_equal(expected.data,out.commands,n*28))return false;
            if(raw.advance_x!=styled.advance_x||raw.advance_y!=styled.advance_y||raw.minimum_x!=styled.minimum_x||raw.minimum_y!=styled.minimum_y||raw.maximum_x!=styled.maximum_x||raw.maximum_y!=styled.maximum_y||raw.has_bounds!=styled.has_bounds)return false;
            if(cff_outline(font,static_cast<u16>(i),&out,0,&styled,true)!=SfntStatus::resource_limit||out.count||out.has_bounds||styled.has_bounds)return false;
            if(n){out.capacity=n-1;if(cff_outline(font,static_cast<u16>(i),&out,1000000,&styled,true)!=SfntStatus::output_too_small||out.count||out.has_bounds||styled.has_bounds)return false;}
            for(u32 simulation=0;simulation<2;++simulation){
                out=outline();
                if(cff_outline_bold(font,static_cast<u16>(i),&out,1000000,&styled,simulation!=0)!=SfntStatus::ok)return false;
                u64 hash=14695981039346656037ull;
                const auto hash_bytes=[&](const void* p,u32 size) noexcept {const auto* b=static_cast<const u8*>(p);for(u32 at=0;at<size;++at){hash^=b[at];hash*=1099511628211ull;}};
                const u32 header[]{out.count,static_cast<u32>(out.fill_rule),out.has_bounds?1u:0u};
                hash_bytes(header,sizeof(header));hash_bytes(&out.minimum_x,16);hash_bytes(out.commands,out.count*28);
                if(hash!=cff_bold_hashes[f*176+simulation*88+i])return false;
                if(raw.advance_x!=styled.advance_x||raw.advance_y!=styled.advance_y||raw.minimum_x!=styled.minimum_x||raw.minimum_y!=styled.minimum_y||raw.maximum_x!=styled.maximum_x||raw.maximum_y!=styled.maximum_y||raw.has_bounds!=styled.has_bounds)return false;
                if(n){out.capacity=n-1;if(cff_outline_bold(font,static_cast<u16>(i),&out,1000000,&styled,simulation!=0)!=SfntStatus::output_too_small||out.count||out.has_bounds||styled.has_bounds)return false;}
                // Exhaust each prefix of this glyph's budget, including the
                // contour pass. Never publish fixed-point bytes on failure.
                if(!f&&i==1){bool succeeded=false;for(u32 budget=0;budget<512;++budget){out=outline();
                    const auto status=cff_outline_bold(font,1,&out,budget,&styled,simulation!=0);
                    if(status==SfntStatus::ok){succeeded=true;break;}
                    if(status!=SfntStatus::resource_limit||out.count||out.has_bounds||!out.rejected||styled.has_bounds||styled.advance_x)return false;
                }if(!succeeded)return false;}
            }
            ++glyphs;
        }
    }
    return reader.at==sizeof(cff_oblique_fixture)&&glyphs==704;
}
bool guarded_inputs() noexcept {
    u8* memory=static_cast<u8*>(VirtualAlloc(nullptr,8192,MEM_RESERVE,PAGE_NOACCESS));
    if(!memory) return false;
    if(!VirtualAlloc(memory,4096,MEM_COMMIT,PAGE_READWRITE)) { VirtualFree(memory,0,MEM_RELEASE); return false; }
    const u8 program[]{139,139,21,149,159,169,179,189,199,8,14};
    u32 random=0x0cf11779; bool ok=true;
    for(u32 iteration=0;iteration<20000 && ok;++iteration) {
        fixture({program,sizeof(program)});
        random^=random<<13; random^=random>>17; random^=random<<5;
        const u32 length=cff_at+cff_size;
        // Alternate table/program mutation with truncation exactly at the guard.
        const u32 size=iteration<length?iteration:length;
        if(iteration>=length) {
            const u32 at=(iteration&1u)?cff_at+128+7+random%sizeof(program):random%length;
            bytes[at]^=static_cast<u8>((random>>16)|1u);
        }
        if(size>=cff_at) put32(bytes+24,size-cff_at);
        u8* input=memory+4096-size; copy_bytes(input,bytes,size);
        CffFont font{};
        if(open({input,size},&font)==SfntStatus::ok) {
            auto out=outline(); CffGlyphMetrics metrics{};
            const auto decoder=(iteration&4)?cff_outline_bold:cff_outline;
            auto status=decoder(font,0,&out,2048,&metrics,(iteration&2)!=0);
            ok=out.count<=out.capacity && (status==SfntStatus::ok ||
                (!out.count && !out.has_bounds && out.rejected && !metrics.has_bounds && !metrics.advance_x));
        }
    }
    VirtualFree(memory,0,MEM_RELEASE); return ok;
}

} // namespace
bool cff_outline_tests() noexcept { return geometry() && invalid_programs() && flex_round_trip() && resolved_metrics() && structures() && cid_selection() && oblique_corpus() && guarded_inputs(); }
} // namespace maicord
