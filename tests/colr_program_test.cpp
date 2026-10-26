#include "maicord/render/colr_program.hpp"
#include "maicord/render/lottie_program.hpp"
#include "maicord/core/utf.hpp"
#include "colr_program_equal.hpp"
#include "../src/render/colr_segment_bounds.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
bool packed_bounds_cases() noexcept {
    const auto check=[](const float* points,u32 count) noexcept {
        GlyphOutline expected{},actual{};
        for(u32 i=0;i<count*2;++i){const float x=points[i*2],y=points[i*2+1];
            if(!expected.has_bounds){expected.minimum_x=expected.maximum_x=x;expected.minimum_y=expected.maximum_y=y;expected.has_bounds=true;}
            else {expected.minimum_x=minimum(expected.minimum_x,x);expected.minimum_y=minimum(expected.minimum_y,y);
                expected.maximum_x=maximum(expected.maximum_x,x);expected.maximum_y=maximum(expected.maximum_y,y);}}
        colr_detail::segment_bounds(points,count,actual);
        return expected.has_bounds==actual.has_bounds&&bytes_equal(&expected.minimum_x,&actual.minimum_x,16);
    };
    float values[128]{};u32 random=0x29334891;
    const u32 edge[]{0,0x80000000u,1,0x80000001u,0x007fffffu,0x807fffffu,0x7f800000u,0xff800000u,0x7fc00001u,0xffc00001u,0x7f800001u,0xff800001u,0x3f800000u,0xbf800000u};
    for(u32 trial=0;trial<4096;++trial){const u32 count=trial%33;
        for(u32 i=0;i<count*4;++i){random^=random<<13;random^=random>>17;random^=random<<5;
            const u32 bits=trial<256?edge[(trial+i)%14]:random;copy_bytes(values+i,&bits,4);}
        if(!check(values,count))return false;
    }
    Win32SystemInfo info{};GetSystemInfo(info.native());
    auto* allocation=static_cast<u8*>(VirtualAlloc(nullptr,info.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!allocation)return false;
    struct Release {u8* p;~Release() noexcept {VirtualFree(p,0,MEM_RELEASE);}} release{allocation};
    DWORD old=0;if(!VirtualProtect(allocation+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&old))return false;
    for(u32 shift=0;shift<16;shift+=4)for(u32 count=1;count<=64;++count){
        auto* points=reinterpret_cast<float*>(allocation+info.dwPageSize-count*16-shift);
        for(u32 i=0;i<count*4;++i)points[i]=static_cast<float>(static_cast<i32>(i%7)-3);
        if(!check(points,count))return false;
    }
    return true;
}
struct Source { u32 child=0,outlines=0,commands=0,stop_count=0; bool fail=false,cycle=false; u32 bound=0,last_capacity=0; };
void solid(Source& source,ColrPaintElement* output) noexcept {
    *output={};output->kind=ColrPaintKind::solid_glyph;
    output->paint.solidGlyph={7,{{source.child?0.f:1.f,0,source.child?1.f:0.f,1}}};
}
bool first(void* context,ColrPaintElement* output) noexcept {
    auto& source=*static_cast<Source*>(context);if(source.fail)return false;
    if(source.cycle){*output={};output->kind=ColrPaintKind::transform;output->paint.transform={1,0,0,1,0,0};return true;}
    if(source.stop_count){*output={};output->kind=ColrPaintKind::linear_gradient;
        output->paint.linearGradient={0,source.stop_count,0,0,1,0,0,1};return true;}
    source.child=0;solid(source,output);return true;
}
bool next(void* context,ColrPaintElement* output) noexcept {auto& source=*static_cast<Source*>(context);++source.child;solid(source,output);return true;}
bool parent(void*) noexcept {return true;}
bool stops(void*,Slice<ColrGradientStop> output) noexcept {
    for(u32 i=0;i<output.count;++i)output[i]={i?1.f:0.f,{1,0,0,1}};return true;
}
bool outline(void* context,u16 glyph,GlyphOutline* output) noexcept {
    auto& source=*static_cast<Source*>(context);++source.outlines;
    source.last_capacity=output->capacity;
    if(glyph!=7 || output->capacity<4)return false;
    if(source.commands) {
        if(source.commands<3 || source.commands>output->capacity)return false;
        output->commands[0]={0,0,0,0,0,0,GlyphPathCommandKind::begin_filled,{}};
        for(u32 i=1;i+1<source.commands;++i)
            output->commands[i]={static_cast<float>(i&1),static_cast<float>((i>>1)&1),0,0,0,0,GlyphPathCommandKind::line,{}};
        output->commands[source.commands-1]={0,0,0,0,0,0,GlyphPathCommandKind::end_closed,{}};
        output->count=source.commands;output->fill_rule=GlyphFillRule::winding;return true;
    }
    output->commands[0]={0,0,0,0,0,0,GlyphPathCommandKind::begin_filled,{}};
    output->commands[1]={1,0,0,0,0,0,GlyphPathCommandKind::line,{}};
    output->commands[2]={1,1,0,0,0,0,GlyphPathCommandKind::line,{}};
    output->commands[3]={0,0,0,0,0,0,GlyphPathCommandKind::end_closed,{}};
    output->count=4;output->fill_rule=GlyphFillRule::winding;return true;
}
bool edge_comparison_cases() noexcept {
    using colr_test::Edge;
    using colr_test::EdgeSequence;
    const Edge triangle[]{{{0,0},{1,0}},{{1,0},{1,1}},{{1,1},{0,0}}};
    Edge closed[]{triangle[0],triangle[1],triangle[2],{{0,0},{0,0}}};
    const EdgeSequence a{triangle,3,nullptr,3},b{closed,4,nullptr,4};
    if(!colr_test::edge_sequence(a,b)||!colr_test::edge_sequence(b,a))return false;
    closed[3]={{2,3},{2,3}};
    if(colr_test::edge_sequence(a,b)||colr_test::edge_sequence(b,a))return false;
    closed[3]={{0,0},{1,1}};
    if(colr_test::edge_sequence(a,b))return false;
    closed[3]={{0,0},{0,0}};
    const u32 reordered[]{1,0,2,3},invalid[]{0,1,2,4},missing[]{0,1,3},valid[]{0,1,2,3};
    return colr_test::edge_sequence(a,{closed,4,valid,4})&&
        !colr_test::edge_sequence(a,{closed,4,reordered,4})&&
        !colr_test::edge_sequence(a,{closed,4,invalid,4})&&
        !colr_test::edge_sequence(a,{closed,4,missing,3});
}
bool closing_edge_cases() noexcept {
    u32 mode=0;
    ColrPaintSource callbacks{&mode,first,next,parent,stops,
        +[](void* context,u16 glyph,GlyphOutline* output) noexcept {
            if(glyph!=7||output->capacity<10)return false;const u32 mode=*static_cast<u32*>(context);u32 count=0;
            const auto add=[&](GlyphPathCommandKind kind,float x=0,float y=0) noexcept {output->commands[count++]={x,y,0,0,0,0,kind,{}};};
            if(mode!=2){add(mode==4?GlyphPathCommandKind::begin_hollow:GlyphPathCommandKind::begin_filled);
                add(GlyphPathCommandKind::line,1,0);add(GlyphPathCommandKind::line,1,1);
                if(mode)add(GlyphPathCommandKind::line);add(GlyphPathCommandKind::end_closed);}
            if(mode==2||mode==3){add(GlyphPathCommandKind::begin_filled,2,3);add(GlyphPathCommandKind::end_closed);}
            output->count=count;output->fill_rule=GlyphFillRule::winding;return true;
        }};
    ColrPaintElement root{};root.kind=ColrPaintKind::solid_glyph;root.paint.solidGlyph={7,{{1,0,0,1}}};
    ColrPaintProgram implicit{},explicit_close{};
    struct Release {ColrPaintProgram &a,&b;~Release() noexcept {arena_release(&a.storage);arena_release(&b.storage);}} release{implicit,explicit_close};
    if(!colr_program_build(&implicit,callbacks,root,{},32))return false;
    for(mode=1;mode<=4;++mode){if(!colr_program_build(&explicit_close,callbacks,root,{},32))return false;
        const auto* paint=reinterpret_cast<const ColrPaintOp*>(explicit_close.storage.base+explicit_close.first);
        const auto* mask=reinterpret_cast<const ColrMask*>(explicit_close.storage.base+paint->reserved);
        if(mask->segment_count!=(mode==1?3u:mode==2?1u:mode==3?4u:0u))return false;
        if(mode==1&&!colr_test::equal(implicit,explicit_close))return false;
    }return edge_comparison_cases();
}
bool matrix_validation_cases() noexcept {
    u32 visits=0;
    ColrPaintSource callbacks{&visits,
        +[](void* context,ColrPaintElement* output) noexcept {
            ++*static_cast<u32*>(context);*output={};output->kind=ColrPaintKind::solid;
            output->paint.solid.value={1,0,0,1};return true;
        },next,parent,stops,outline};
    ColrPaintProgram program{};
    struct Release {ColrPaintProgram& p;~Release() noexcept {arena_release(&p.storage);}} release{program};
    const float threshold=1e-12f;u32 bits=0;copy_bytes(&bits,&threshold,4);
    const u32 magnitudes[]{0,1,0x00800000u,bits-1,bits,bits+1,0x3f800000u,0x7f800000u,0x7fc00001u};
    const u32 signs[]{0u,0x80000000u};
    for(u32 sign:signs)for(u32 magnitude:magnitudes) {
        const u32 encoded=magnitude|sign;float value=0;copy_bytes(&value,&encoded,4);
        ColrPaintElement root{};root.kind=ColrPaintKind::transform;
        root.paint.transform={value,0,0,1,0,0};visits=0;
        // Preserve the strict finite cutoff and the existing /fp:fast
        // nonfinite behavior; these callbacks observe traversal, not pixels.
        const bool traversed=magnitude>bits;
        const bool built=colr_program_build(&program,callbacks,root,{},32);
        const auto* node=built?reinterpret_cast<const ColrPaintOp*>(program.storage.base+program.first):nullptr;
        if(!built || program.nodes!=1 || program.stack!=1 || visits!=(traversed?1u:0u) ||
           node->kind!=(traversed?0u:7u)) {
            char message[]="matrix boundary 00000000 build=0 visits=0 kind=0\n";
            constexpr char hex[]="0123456789abcdef";
            for(u32 i=0;i<8;++i)message[16+i]=hex[(encoded>>(28-i*4))&15];
            message[31]=built?'1':'0';message[40]=hex[visits&15];message[47]=node?hex[node->kind&15]:'x';
            DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,sizeof(message)-1,&written,nullptr);
            return false;
        }
    }
    // An invertible transform reaches the gradient with its complete inverse.
    callbacks.first_child=+[](void* context,ColrPaintElement* output) noexcept {
        ++*static_cast<u32*>(context);*output={};output->kind=ColrPaintKind::linear_gradient;
        output->paint.linearGradient={0,2,0,0,1,0,0,1};return true;
    };
    ColrPaintElement root{};root.kind=ColrPaintKind::transform;root.paint.transform={2,0,0,4,6,8};visits=0;
    if(!colr_program_build(&program,callbacks,root,{},32)||visits!=1||program.nodes!=1)return false;
    const auto* gradient=reinterpret_cast<const ColrPaintNode*>(program.storage.base+program.first);
    const float expected[]{.5f,0,0,.25f,-3,-2};
    if(gradient->kind!=1)return false;
    for(u32 i=0;i<6;++i)if(gradient->inverse[i]!=expected[i])return false;
    // Singular non-diagonal transforms never reach the inverse calculation.
    root.paint.transform={1,2,2,4,6,8};visits=0;
    return colr_program_build(&program,callbacks,root,{},32)&&visits==0&&program.nodes==1&&
        reinterpret_cast<const ColrPaintOp*>(program.storage.base+program.first)->kind==7;
}
// Each gradient's geometry reaches the program node in the layout the shader reads.
bool gradient_value_cases() noexcept {
    ColrPaintElement element{};
    const ColrPaintSource callbacks{&element,
        +[](void* context,ColrPaintElement* output) noexcept {*output=*static_cast<ColrPaintElement*>(context);return true;},
        +[](void*,ColrPaintElement*) noexcept {return false;},
        +[](void*) noexcept {return true;},
        +[](void*,Slice<ColrGradientStop>) noexcept {return true;},
        +[](void*,u16,GlyphOutline*) noexcept {return false;}};
    ColrPaintProgram program{};
    struct Release {ColrPaintProgram& program;~Release() noexcept {arena_release(&program.storage);}} release{program};
    const auto node=[&]() noexcept {
        return colr_program_build(&program,callbacks,element,{},32)&&program.nodes==1?
            reinterpret_cast<const ColrPaintNode*>(program.storage.base+program.first):nullptr;
    };
    const auto matches=[](const ColrPaintNode* n,u32 kind,u32 mode,const float (&values)[6]) noexcept {
        if(!n||n->kind!=kind||n->mode!=mode||n->count!=2)return false;
        for(u32 i=0;i<6;++i)if(n->values[i]!=values[i])return false;
        return true;
    };
    element.kind=ColrPaintKind::linear_gradient;element.paint.linearGradient={1,2,1,2,3,4,5,6};
    if(!matches(node(),1,1,{1,2,3,4,5,6}))return false;
    element.kind=ColrPaintKind::radial_gradient;element.paint.radialGradient={2,2,1,2,3,4,5,6};
    if(!matches(node(),2,2,{1,2,4,5,3,6}))return false;
    element.kind=ColrPaintKind::sweep_gradient;element.paint.sweepGradient={0,2,1,2,3,4};
    return matches(node(),3,0,{1,2,3,4,0,0});
}
bool segment_capacity_cases() noexcept {
    Source source{};ColrPaintSource callbacks{&source,first,next,parent,stops,outline};
    ColrPaintElement root{};solid(source,&root);
    ColrPaintProgram cold{},warm{},limited{};
    struct Release {ColrPaintProgram &a,&b,&c;~Release() noexcept {arena_release(&a.storage);arena_release(&b.storage);arena_release(&c.storage);}} release{cold,warm,limited};
    if(!arena_initialize(&cold.storage,65536,0,4096) || !arena_initialize(&warm.storage,65536,65536,4096) ||
       !arena_initialize(&limited.storage,4096,0,4096))return false;
    const u32 counts[]{244,245,246,247,248,1024,2048};
    for(u32 stops=0;stops<4;++stops) {
        source.stop_count=stops;root={};
        if(stops){root.kind=ColrPaintKind::glyph;root.paint.glyph.glyphIndex=7;}
        else solid(source,&root);
        for(u32 count:counts) {
            source.commands=count;
            if(!colr_program_build(&cold,callbacks,root,{},1) || !colr_program_build(&warm,callbacks,root,{},1) ||
               cold.storage.offset!=warm.storage.offset || cold.storage.peak!=warm.storage.peak ||
               cold.storage.offset>cold.storage.committed || cold.mask_pixels!=warm.mask_pixels)return false;
            for(usize i=0;i<cold.storage.offset;++i)if(cold.storage.base[i]!=warm.storage.base[i])return false;
        }
        source.commands=1024;
        // Gradient payloads shift segment starts through every four-byte
        // alignment within a segment; a page may end with unusable tail bytes.
        const usize start=(stops?sizeof(ColrPaintNode):sizeof(ColrSolidPaintNode))+
            sizeof(ColrMask)+sizeof(ColrGradientStop)*stops;
        const usize consumed=4096-((4096-start)%16);
        const usize peak=maximum(limited.storage.peak,consumed);
        if(colr_program_build(&limited,callbacks,root,{},1) || limited.storage.offset!=consumed || limited.storage.peak!=peak ||
           limited.storage.committed!=4096)return false;
        // Failure must preserve the consumed cursor and permit a smaller rebuild.
        source.commands=4;
        if(!colr_program_build(&limited,callbacks,root,{},1) || limited.mask_count!=1 || !limited.bounds.has_bounds)return false;
    }
    return true;
}
bool row_bin_cases() noexcept {
    struct Fixture {u32 mode;float height;} fixture{};
    ColrPaintSource callbacks{&fixture,first,next,parent,stops,
        +[](void* context,u16 glyph,GlyphOutline* output) noexcept {
            const auto& f=*static_cast<Fixture*>(context);
            if(glyph!=7||output->capacity<34)return false;
            output->commands[0]={-1,-.25f,0,0,0,0,GlyphPathCommandKind::begin_filled,{}};
            const u32 count=f.mode==3?4u:34u;
            for(u32 i=1;i+1<count;++i){
                const float y=f.mode==0?-.25f:f.mode==1?(i&1?f.height:-.25f):
                    f.mode==2?static_cast<float>((i*19)%31)*f.height/30-.25f:(i==1?f.height:-.25f);
                output->commands[i]={static_cast<float>(i%7)-3,y,0,0,0,0,GlyphPathCommandKind::line,{}};
            }
            output->commands[count-1]={0,0,0,0,0,0,GlyphPathCommandKind::end_closed,{}};
            output->count=count;output->fill_rule=GlyphFillRule::winding;return true;
        }};
    ColrPaintElement root{};root.kind=ColrPaintKind::solid_glyph;root.paint.solidGlyph={7,{{1,0,0,1}}};
    ColrPaintProgram program{};
    struct Release {ColrPaintProgram& p;~Release() noexcept {arena_release(&p.storage);}} release{program};
    const float heights[]{0,.25f,1,2,16,32,63.5f,64};
    for(u32 mode=0;mode<4;++mode)for(float height:heights){
        const float scale=mode==3&&height==63.5f?1024.f:1.f;
        fixture={mode,height};
        const auto fail=[&](char stage) noexcept {
            char message[]="row bins mode=0 stage=0 height=00000000\n";u32 bits=0;copy_bytes(&bits,&height,4);
            constexpr char hex[]="0123456789abcdef";message[14]=static_cast<char>('0'+mode);message[22]=stage;
            for(u32 i=0;i<8;++i)message[31+i]=hex[(bits>>(28-i*4))&15];
            DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,sizeof(message)-1,&written,nullptr);return false;
        };
        if(!colr_program_build(&program,callbacks,root,{},scale)||program.mask_count!=1)return fail('b');
        const auto* node=reinterpret_cast<const ColrPaintOp*>(program.storage.base+program.first);
        const auto* job=reinterpret_cast<const ColrMask*>(program.storage.base+node->reserved);
        const auto* edges=reinterpret_cast<const float*>(program.storage.base+job->segments);
        const auto* rows=reinterpret_cast<const u32*>(program.storage.base+job->rows);
        const i32 top=job->top;u32 next_offset=rows[0];
        // Independent row-first oracle: every list must contain exactly the
        // intersecting edge indices, in their original order, without gaps.
        for(u32 y=0;y<job->height;++y){
            const u32 offset=rows[y*2];
            if((offset!=next_offset&&(!y||offset!=rows[(y-1)*2]))||offset>program.storage.offset||
               rows[y*2+1]>(program.storage.offset-offset)/4)return fail('r');
            const auto* indices=reinterpret_cast<const u32*>(program.storage.base+offset);u32 found=0;
            for(u32 i=0;i<job->segment_count;++i){
                const i32 first=maximum(0,static_cast<i32>(minimum(edges[i*4+1],edges[i*4+3])*scale-top-1.5f));
                const i32 last=minimum(static_cast<i32>(job->height)-1,static_cast<i32>(maximum(edges[i*4+1],edges[i*4+3])*scale-top+1.5f));
                if(static_cast<i32>(y)>=first&&static_cast<i32>(y)<=last){
                    if(found>=rows[y*2+1]||indices[found++]!=i)return fail('i');
                }
            }
            if(found!=rows[y*2+1])return fail('c');if(offset==next_offset)next_offset+=found*4;
        }
    }
    // Fail after row endpoints have been counted, before allocating the edge
    // lists. A smaller rebuild must overwrite that temporary representation.
    ColrPaintProgram limited{};Release release_limited{limited};
    if(!arena_initialize(&limited.storage,4096,0,4096))return false;
    fixture={1,64};
    if(!colr_program_build(&limited,callbacks,root,{},1)||limited.storage.offset>=4096)return false;
    fixture={2,256};
    if(colr_program_build(&limited,callbacks,root,{},1)||limited.mask_count!=1||
       !limited.storage.offset||limited.storage.offset>limited.storage.committed)return false;
    fixture={0,0};
    if(!colr_program_build(&limited,callbacks,root,{},1)||limited.mask_count!=1)return false;
    const auto* node=reinterpret_cast<const ColrPaintOp*>(limited.storage.base+limited.first);
    const auto* job=reinterpret_cast<const ColrMask*>(limited.storage.base+node->reserved);
    const auto* rows=reinterpret_cast<const u32*>(limited.storage.base+job->rows);
    for(u32 y=0;y<job->height;++y){
        if(rows[y*2+1]!=job->segment_count)return false;
        const auto* indices=reinterpret_cast<const u32*>(limited.storage.base+rows[y*2]);
        for(u32 i=0;i<job->segment_count;++i)if(indices[i]!=i)return false;
    }
    return true;
}
bool outline_capacity_cases() noexcept {
    Source source{};ColrPaintSource callbacks{&source,first,next,parent,stops,outline};
    ColrPaintElement root{};root.kind=ColrPaintKind::layers;root.paint.layers.childCount=2;
    ColrPaintProgram reference{},candidate{};
    struct Release {ColrPaintProgram &a,&b;~Release() noexcept {arena_release(&a.storage);arena_release(&b.storage);}} release{reference,candidate};
    if(!colr_program_build(&reference,callbacks,root,{},32)||source.last_capacity!=2048||source.outlines!=1)return false;
    callbacks.outline_capacity=+[](void* context,u16) noexcept {return static_cast<Source*>(context)->bound;};
    const u32 bounds[]{0,4,7,64,2048,2049,0xffffffffu};
    for(u32 bound:bounds){source.bound=bound;source.outlines=0;
        if(!colr_program_build(&candidate,callbacks,root,{},32)||source.outlines!=1||
           source.last_capacity!=(bound?minimum<u32>(bound,2048):2048)||candidate.storage.offset!=reference.storage.offset||
           !bytes_equal(candidate.storage.base,reference.storage.base,static_cast<u32>(reference.storage.offset)))return false;
    }
    source.bound=1;if(colr_program_build(&candidate,callbacks,root,{},32))return false;
    source.bound=4;
    if(!colr_program_build(&candidate,callbacks,root,{},32)||candidate.storage.offset!=reference.storage.offset||
       !bytes_equal(candidate.storage.base,reference.storage.base,static_cast<u32>(reference.storage.offset))||
       !colr_test::equal(candidate,reference))return false;
    auto* node=reinterpret_cast<ColrPaintOp*>(candidate.storage.base+candidate.first);
    auto* job=reinterpret_cast<ColrMask*>(candidate.storage.base+node->reserved);
    auto* rows=reinterpret_cast<u32*>(candidate.storage.base+job->rows);
    auto* index=reinterpret_cast<u32*>(candidate.storage.base+rows[0]);const u32 saved_index=*index;
    ++*index;const bool changed_edge=!colr_test::equal(candidate,reference);*index=saved_index;
    const u32 saved_mask=node->reserved;node->reserved=static_cast<u32>(candidate.storage.offset);
    const bool truncated_mask=!colr_test::equal(candidate,reference);node->reserved=saved_mask;
    const u32 height=job->height;job->height=0x80000000u;
    const bool invalid_rows=!colr_test::equal(candidate,candidate);job->height=height;
    return changed_edge&&truncated_mask&&invalid_rows&&colr_test::equal(candidate,reference);
}
}
bool colr_program_tests() noexcept {
    constexpr auto vector=utf8_literal(R"({"w":320,"h":320,"layers":[{"ty":4,"ks":{"o":{"k":100}},"shapes":[{"ty":"gr","it":[{"ty":"rc","p":{"k":[160,160]},"s":{"k":[160,160]},"r":{"k":12}},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}},{"ty":"tr","o":{"k":100}}]}]}]})");
    ColrPaintProgram vector_program{};float vector_extent=0;LottieTiming timing{};
    const auto compile=[&](auto json,u32 frame_index,float* extent=nullptr) noexcept {
        return lottie_program_compile_frame({reinterpret_cast<const u8*>(json.data),json.bytes},frame_index,128,&timing,
            +[](void* program) noexcept {return static_cast<ColrPaintProgram*>(program);},&vector_program,extent);
    };
    bool vector_ok=compile(vector,0,&vector_extent) && vector_extent==1 && vector_program.mask_count==1 &&
        vector_program.bounds.has_bounds && vector_program.bounds.maximum_x==1;
    constexpr auto moving=utf8_literal(R"({"w":320,"h":320,"fr":60,"ip":0,"op":11,"layers":[{"ty":4,"shapes":[{"ty":"rc","p":{"a":1,"k":[{"t":0,"s":[64,160],"e":[256,160],"o":{"x":0.333333,"y":0.333333},"i":{"x":0.666667,"y":0.666667}},{"t":10,"s":[256,160]}]},"s":{"k":[64,64]}},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}}]}]})");
    i32 positions[3]{};
    for(u32 i=0;i<3 && vector_ok;++i){
        vector_ok=compile(moving,i*5);
        if(vector_ok){const auto* node=reinterpret_cast<const ColrPaintOp*>(vector_program.storage.base+vector_program.first);
            const auto* mask=reinterpret_cast<const ColrMask*>(vector_program.storage.base+node->reserved);
            positions[i]=mask->left;}
    }
    vector_ok=vector_ok && positions[0]>=11 && positions[0]<=13 && positions[1]>=50 && positions[1]<=52 &&
        positions[2]>=88 && positions[2]<=90;
    constexpr auto invalid=utf8_literal(R"({"w":4097,"h":320,"layers":[]})");
    constexpr auto open_fill=utf8_literal(R"({"w":320,"h":320,"layers":[{"ty":4,"shapes":[{"ty":"sh","ks":{"k":{"c":false,"v":[[48,48],[272,48],[272,272]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}}},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}}]}]})");
    vector_ok=vector_ok && compile(open_fill,0);
    if(vector_ok) {
        const auto* node=reinterpret_cast<const ColrPaintOp*>(vector_program.storage.base+vector_program.first);
        const auto* mask=reinterpret_cast<const ColrMask*>(vector_program.storage.base+node->reserved);
        // Two authored edges plus the implicit closing edge are required for
        // winding coverage; losing the third made real open-path fills vanish.
        vector_ok=node->kind==8 && mask->segment_count==3;
    }
    // Morphing paths exercise both the unused end buffers at frame zero and
    // all six assigned coordinate/tangent prefixes during interpolation.
    constexpr auto morphing=utf8_literal(R"({"w":320,"h":320,"fr":60,"ip":0,"op":11,"layers":[{"ty":4,"shapes":[{"ty":"sh","ks":{"k":[{"t":0,"s":[{"c":true,"v":[[32,32],[96,32],[96,96]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}],"e":[{"c":true,"v":[[192,32],[256,32],[256,96]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}]},{"t":10,"s":[{"c":true,"v":[[192,32],[256,32],[256,96]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}]}]}},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}}]}]})");
    for(u32 frame=0;frame<3&&vector_ok;++frame) {
        vector_ok=compile(morphing,frame*5)&&vector_program.mask_count==1;
        if(vector_ok) {
            const auto* node=reinterpret_cast<const ColrPaintOp*>(vector_program.storage.base+vector_program.first);
            const auto* mask=reinterpret_cast<const ColrMask*>(vector_program.storage.base+node->reserved);
            const i32 left=12+32*static_cast<i32>(frame);
            vector_ok=node->kind==8&&mask->segment_count==3&&mask->left>=left-1&&mask->left<=left+1;
        }
    }
    constexpr auto empty_merge=utf8_literal(R"({"w":320,"h":320,"layers":[{"ty":4,"shapes":[{"ty":"gr","it":[{"ty":"rc","p":{"k":[160,160]},"s":{"k":[160,160]}},{"ty":"gr","it":[{"ty":"tr","p":{"k":[0,0]}}]},{"ty":"mm","mm":4},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}}]}]}]})");
    vector_ok=vector_ok && compile(empty_merge,0) &&
        vector_program.mask_count==1;
    constexpr auto empty_intersection=utf8_literal(R"({"w":320,"h":320,"layers":[{"ty":4,"shapes":[{"ty":"gr","it":[{"ty":"rc","p":{"k":[160,160]},"s":{"k":[160,160]}},{"ty":"gr","it":[{"ty":"rc","p":{"k":[64,64]},"s":{"k":[16,16]}},{"ty":"rc","p":{"k":[256,256]},"s":{"k":[16,16]}},{"ty":"mm","mm":4}]},{"ty":"mm","mm":4},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}}]}]}]})");
    vector_ok=vector_ok && compile(empty_intersection,0) &&
        vector_program.mask_count==0;
    vector_ok=vector_ok && !compile(invalid,0);
    constexpr auto scope=utf8_literal(R"({"w":320,"h":320,"layers":[{"ind":1,"ty":3,"ks":{"o":{"k":0}}},{"ind":2,"parent":1,"ty":4,"shapes":[{"ty":"gr","it":[{"ty":"rc","p":{"k":[160,160]},"s":{"k":[160,160]}}]},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}}]}]})");
    vector_ok=vector_ok && compile(scope,0) && vector_program.mask_count==1;
    constexpr auto cycle=utf8_literal(R"({"w":320,"h":320,"layers":[{"ind":1,"parent":1,"ty":4,"shapes":[]}]})");
    vector_ok=vector_ok && !compile(cycle,0);
    // Track mattes: the white td source only clips the layer bound below it.
    // Drawing it (the old behaviour) painted a white bar over the sticker.
    // Exactly one painted mask spanning rows [low, high) of the 128 px raster.
    const auto half=[&](i32 low,i32 high) noexcept {
        if(vector_program.mask_count!=1)return false;
        const auto* node=reinterpret_cast<const ColrPaintOp*>(vector_program.storage.base+vector_program.first);
        const auto* mask=reinterpret_cast<const ColrMask*>(vector_program.storage.base+node->reserved);
        const i32 bottom=mask->top+static_cast<i32>(mask->height);
        return mask->top>=low-2 && mask->top<=low+2 && bottom>=high-2 && bottom<=high+2;
    };
    constexpr auto matte=utf8_literal(R"({"w":320,"h":320,"layers":[{"ind":1,"ty":4,"td":1,"shapes":[{"ty":"rc","p":{"k":[160,80]},"s":{"k":[320,160]}},{"ty":"fl","c":{"k":[1,1,1,1]},"o":{"k":100}}]},{"ind":2,"ty":4,"tt":1,"shapes":[{"ty":"rc","p":{"k":[160,160]},"s":{"k":[320,320]}},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}}]}]})");
    const bool matte_ok=compile(matte,0) && half(0,64);
    constexpr auto inverted=utf8_literal(R"({"w":320,"h":320,"layers":[{"ind":1,"ty":1,"td":1,"sw":320,"sh":160,"sc":"#ffffff"},{"ind":2,"ty":4,"tt":2,"tp":1,"shapes":[{"ty":"rc","p":{"k":[160,160]},"s":{"k":[320,320]}},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}}]}]})");
    const bool inverted_ok=compile(inverted,0) && half(64,128);
    constexpr auto masked=utf8_literal(R"({"w":320,"h":320,"layers":[{"ty":4,"hasMask":true,"masksProperties":[{"mode":"s","inv":false,"pt":{"k":{"c":true,"v":[[0,160],[320,160],[320,320],[0,320]],"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]}}}],"shapes":[{"ty":"rc","p":{"k":[160,160]},"s":{"k":[320,320]}},{"ty":"fl","c":{"k":[1,0,0,1]},"o":{"k":100}}]}]})");
    const bool mask_ok=compile(masked,0) && half(0,64);
    constexpr auto solid=utf8_literal(R"({"w":320,"h":320,"layers":[{"ty":1,"sw":320,"sh":160,"sc":"#00ff00"}]})");
    const bool solid_ok=compile(solid,0) && half(0,64);
    vector_ok=vector_ok && matte_ok && inverted_ok && mask_ok && solid_ok;
    arena_release(&vector_program.storage);
    Source source{};ColrPaintSource callbacks{&source,first,next,parent,stops,outline};ColrPaintProgram program{};
    ColrPaintElement root{};root.kind=ColrPaintKind::layers;root.paint.layers.childCount=2;
    bool ok=colr_program_build(&program,callbacks,root,{0,0,1,1},32);
    ok=ok && program.nodes==4 && program.stack==1 && program.peak_stack==2 && program.mask_count==1 && source.outlines==1 &&
        program.bounds.has_bounds && program.bounds.maximum_x==1 && program.bounds.maximum_y==1;
    if(ok) {
        const auto* a=reinterpret_cast<const ColrPaintOp*>(program.storage.base+program.first);
        const auto* b=reinterpret_cast<const ColrPaintOp*>(program.storage.base+a->next);
        const auto* c=reinterpret_cast<const ColrPaintOp*>(program.storage.base+b->next);
        ok=a->kind==8 && b->kind==8 && a->reserved==b->reserved && c->kind==6 && c->mode==3 && c->count==1 && c->next-b->next==sizeof(ColrPaintOp);
    }
    source.fail=true;ok=ok && !colr_program_build(&program,callbacks,root,{0,0,1,1},32);
    source.fail=false;source.cycle=true;root.kind=ColrPaintKind::transform;root.paint.transform={1,0,0,1,0,0};
    ok=ok && !colr_program_build(&program,callbacks,root,{},32);
    root.paint.transform={0,0,0,0,0,0};
    ok=ok && colr_program_build(&program,callbacks,root,{},32) && program.nodes==1 && program.stack==1 && program.storage.offset==sizeof(ColrPaintOp);
    root={};root.kind=ColrPaintKind::solid;root.paint.solid.value={.25f,.5f,.75f,1};
    ok=ok && colr_program_build(&program,callbacks,root,{},32) && program.nodes==1 &&
        program.storage.offset==sizeof(ColrSolidPaintNode) &&
        bytes_equal(program.storage.base+sizeof(ColrPaintOp),&root.paint.solid.value,sizeof(ColrColor));
    root={};root.kind=ColrPaintKind::glyph;root.paint.glyph.glyphIndex=7;
    callbacks.first_child=+[](void*,ColrPaintElement* output) noexcept {*output={};return true;};
    ok=ok && colr_program_build(&program,callbacks,root,{},32) && program.nodes==2;
    if(ok){const auto* mask=reinterpret_cast<const ColrPaintOp*>(program.storage.base+program.last);
        const auto* job=reinterpret_cast<const ColrMask*>(program.storage.base+mask->data);
        ok=mask->kind==4 && job->segments-program.last==sizeof(ColrPaintOp);}
    callbacks.first_child=first;
    root={};root.kind=ColrPaintKind::linear_gradient;root.paint.linearGradient={2,2,0,0,1,0,0,1};
    ok=ok && colr_program_build(&program,callbacks,root,{0,0,1,1},32) && program.nodes==2;
    root.paint.linearGradient.gradientStopCount=0;
    ok=ok && !colr_program_build(&program,callbacks,root,{},32);
    callbacks.stops=nullptr;ok=ok && !colr_program_build(&program,callbacks,root,{},32);
    arena_release(&program.storage);return vector_ok && ok && segment_capacity_cases() && row_bin_cases() && outline_capacity_cases() && packed_bounds_cases() && matrix_validation_cases() && gradient_value_cases() && closing_edge_cases();
}
bool colr_source_tests() noexcept;
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_colr_program_tests_entry() noexcept {
    ExitProcess(!maicord::colr_program_tests() ? 1u : !maicord::colr_source_tests() ? 2u : 0u);
}
