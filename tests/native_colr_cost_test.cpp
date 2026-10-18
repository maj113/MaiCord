// Manual COLR paint program tool (MaiCordNativeColrCost, cmake/reference_tools.cmake with
// -DMAICORD_BUILD_FONT_REFERENCE=ON): <font> [--program-work | --program-cost <n> |
// --program-stages <n>] compares native and Windows paint programs over a whole font and times
// them. Not part of the CTest suite.
#include "../tools/colr_reference/native_source.hpp"
#include "maicord/render/colr_paint.hpp"
#include "../tools/colr_reference/dwrite_paint_builder.hpp"
#include "maicord/core/byte_order.hpp"
#include "colr_program_equal.hpp"
#include "maicord/sys/dwrite.hpp"
namespace maicord {namespace {
NativePaintContext native;
void print(const char* value) noexcept {u32 n=0;while(value[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),value,n,&written,nullptr);}
void number(u64 value) noexcept {char data[32];u32 n=0;do{data[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){char c=data[i];data[i]=data[n-1-i];data[n-1-i]=c;}DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),data,n,&written,nullptr);}
#define CHECK(x) do{if(!(x)){print("FAIL colr cost line ");number(__LINE__);print("\n");return false;}}while(0)
u64 outline_ticks=0,outline_calls=0,source_ticks=0,source_calls=0;
u32 previous_outline=65536;u64 repeated_outlines=0,repeated_outline_ticks=0,repeated_commands=0;
u64 outline_capacity_counts[6]{},requested_capacity_counts[6]{},requested_capacity_total=0;u32 maximum_outline_commands=0,maximum_requested_capacity=0;
ColrPaintSource profiled_source{};
bool (*profiled_outline)(void*,u16,GlyphOutline*) noexcept=nullptr;
bool timed_outline(void* context,u16 glyph,GlyphOutline* output) noexcept {
    LARGE_INTEGER begin{},end{};
    if(!QueryPerformanceCounter(&begin))return false;
    const bool valid=profiled_outline(context,glyph,output);
    if(!QueryPerformanceCounter(&end))return false;
    outline_ticks+=end.QuadPart-begin.QuadPart;++outline_calls;
    if(valid){u32 bucket=0;while(bucket<5&&output->count>(64u<<bucket))++bucket;
        ++outline_capacity_counts[bucket];maximum_outline_commands=maximum(maximum_outline_commands,output->count);
        bucket=0;while(bucket<5&&output->capacity>(64u<<bucket))++bucket;++requested_capacity_counts[bucket];
        maximum_requested_capacity=maximum(maximum_requested_capacity,output->capacity);requested_capacity_total+=output->capacity;}
    if(valid&&previous_outline==glyph){++repeated_outlines;repeated_outline_ticks+=end.QuadPart-begin.QuadPart;repeated_commands+=output->count;}
    previous_outline=valid?glyph:65536;return valid;
}
bool timed_move(bool (*move)(void*,ColrPaintElement*) noexcept,void* context,ColrPaintElement* output) noexcept {
    LARGE_INTEGER begin{},end{};if(!QueryPerformanceCounter(&begin))return false;
    const bool valid=move(context,output);if(!QueryPerformanceCounter(&end))return false;
    source_ticks+=end.QuadPart-begin.QuadPart;++source_calls;return valid;
}
bool timed_first(void* context,ColrPaintElement* output) noexcept {return timed_move(profiled_source.first_child,context,output);}
bool timed_next(void* context,ColrPaintElement* output) noexcept {return timed_move(profiled_source.next_sibling,context,output);}
bool timed_stops(void* context,Slice<ColrGradientStop> output) noexcept {
    LARGE_INTEGER begin{},end{};if(!QueryPerformanceCounter(&begin))return false;
    const bool valid=profiled_source.stops(context,output);if(!QueryPerformanceCounter(&end))return false;
    source_ticks+=end.QuadPart-begin.QuadPart;++source_calls;return valid;
}
bool staged_build(ColrPaintProgram* program,u16 glyph) noexcept {
    previous_outline=65536;
    ColrPaintElement root{};ColrRect clip{};ColrPaintSource callbacks{};LARGE_INTEGER begin{},end{};
    if(!QueryPerformanceCounter(&begin))return false;
    const auto status=colr_source_begin(&native.cursor,glyph,1000000,&root,&clip,&callbacks);
    if(!QueryPerformanceCounter(&end))return false;
    source_ticks+=end.QuadPart-begin.QuadPart;++source_calls;
    if(status!=SfntStatus::ok)return false;
    profiled_source=callbacks;callbacks.first_child=timed_first;callbacks.next_sibling=timed_next;callbacks.stops=timed_stops;
    return colr_program_build(program,callbacks,root,clip,32);
}
bool run() noexcept {
    const wchar_t* p=GetCommandLineW();bool quote=false;while(*p){if(*p=='"')quote=!quote;else if(!quote&&*p==' ')break;++p;}while(*p==' ')++p;
    wchar_t path[32768]{};u32 units=0;quote=*p=='"';if(quote)++p;
    while(*p&&(quote?*p!='"':*p!=' ')){CHECK(units+1<32768);path[units++]=*p++;}CHECK(units);
    if(quote && *p=='"')++p;while(*p==' ')++p;
    // Full-corpus equality stays the default; timed work is always a bounded
    // explicit cohort, so an omitted flag cannot launch six whole-font pairs.
    bool program_work=true,program_cost=false,program_stages=false;u32 program_begin=0;
    if(*p) {
        const auto matches=[&](const wchar_t* flag) noexcept {u32 at=0;while(flag[at]&&p[at]==flag[at])++at;return !flag[at]&&!p[at];};
        const wchar_t* program_flags[]{L"--program-cost ",L"--program-stages "};
        for(u32 index=0;index<2&&!program_cost;++index){
        const auto* program_flag=program_flags[index];u32 matched=0;
        while(program_flag[matched]&&p[matched]==program_flag[matched])++matched;
        if(!program_flag[matched]){
            p+=matched;CHECK(*p>=L'0'&&*p<=L'9');
            while(*p>=L'0'&&*p<=L'9'){CHECK(program_begin<16384);program_begin=program_begin*10+(*p++-L'0');}
            CHECK(!*p);program_cost=true;program_stages=index==1;
        }}
        CHECK(program_cost||matches(L"--program-work"));program_work=!program_cost||program_stages;
    }
    ColrPaintProgram programs[2]{};IDWriteFactory* factory=nullptr;IDWriteFontFile* file=nullptr;IDWriteFontFace* face=nullptr;IDWriteFontFace7* face7=nullptr;IDWritePaintReader* reader=nullptr;
    struct Release {ColrPaintProgram* p;IDWriteFactory*& f;IDWriteFontFile*& file;IDWriteFontFace*& face;IDWriteFontFace7*& f7;IDWritePaintReader*& r;
        ~Release() noexcept {for(u32 i=0;i<2;++i)arena_release(&p[i].storage);native.close();if(r)r->Release();if(f7)f7->Release();if(face)face->Release();if(file)file->Release();if(f)f->Release();}} release{programs,factory,file,face,face7,reader};
    LARGE_INTEGER frequency{},first{},last{};CHECK(QueryPerformanceFrequency(&frequency));u64 init[2]{};
    CHECK(QueryPerformanceCounter(&first));CHECK(native.open(path));CHECK(QueryPerformanceCounter(&last));init[0]=last.QuadPart-first.QuadPart;
    CHECK(QueryPerformanceCounter(&first));CHECK(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory))));
    CHECK(SUCCEEDED(factory->CreateFontFileReference(path,nullptr,&file))&&SUCCEEDED(factory->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE,1,&file,0,DWRITE_FONT_SIMULATIONS_NONE,&face)));
    CHECK(SUCCEEDED(face->QueryInterface(__uuidof(IDWriteFontFace7),reinterpret_cast<void**>(&face7)))&&SUCCEEDED(face7->CreatePaintReader(DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,DWRITE_PAINT_FEATURE_LEVEL_COLR_V1,&reader))&&SUCCEEDED(reader->SetTextColor({1,1,1,1})));
    CHECK(QueryPerformanceCounter(&last));init[1]=last.QuadPart-first.QuadPart;
    const u32 count=native.color.bases_v1.count/6;CHECK(count&&count<=16384&&native.color.bases_v1.count==count*6);
    CHECK(program_begin<count);const u32 program_end=program_cost?minimum(count,program_begin+128):count;
    const auto build=[&](u32 side,u16 glyph) noexcept {if(!side)return native.build(programs,glyph,32);
        DWRITE_PAINT_ELEMENT root{};D2D_RECT_F clip{};return SUCCEEDED(reader->SetCurrentGlyph(glyph,&root,&clip))&&root.paintType!=DWRITE_PAINT_TYPE_NONE&&colr_paint_build(programs+1,reader,face,root,clip,32);};
    u64 bytes=0,reference_bytes=0,nodes=0;u32 kinds=0;usize maximum_bytes=0;
    for(u32 i=program_begin;i<program_end;++i){const u16 glyph=byte_load_be16(native.color.bases_v1.data+i*6);CHECK(build(0,glyph)&&build(1,glyph));const auto& a=programs[0];const auto& b=programs[1];
        if(!colr_test::equal(a,b)){print("different glyph ");number(glyph);print("\n");return false;}
        bytes+=a.storage.offset;reference_bytes+=b.storage.offset;nodes+=a.nodes;maximum_bytes=maximum(maximum_bytes,a.storage.offset);kinds|=a.paint_types;}
    print("{\"kind\":\"colr_cost\",\"glyphs\":");number(program_end-program_begin);print(",\"first\":");number(program_begin);print(",\"total_glyphs\":");number(count);print(",\"program_bytes\":");number(bytes);print(",\"paint_types\":");number(kinds);print(",\"frequency\":");number(frequency.QuadPart);print(",\"init_ticks\":[");number(init[0]);print(",");number(init[1]);print("],\"batches\":[");
    for(u32 round=0;round<(program_work?0u:6u);++round){u64 ticks[2]{},cycles[2]{};
        for(u32 order=0;order<2;++order){const u32 side=order^(round&1);ULONG64 begin=0,end=0;CHECK(QueryThreadCycleTime(GetCurrentThread(),&begin)&&QueryPerformanceCounter(&first));
            for(u32 i=program_begin;i<program_end;++i)CHECK(build(side,byte_load_be16(native.color.bases_v1.data+i*6)));
            CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&end));ticks[side]=last.QuadPart-first.QuadPart;cycles[side]=end-begin;}
        if(round)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print(",");number(cycles[0]);print(",");number(cycles[1]);print("]");}
    print("],\"program_nodes\":");number(nodes);print(",\"node_bytes\":");number(sizeof(ColrPaintNode));print(",\"maximum_program_bytes\":");number(maximum_bytes);
    if(program_stages){
        profiled_outline=native.cursor.outline_provider;native.cursor.outline_provider=timed_outline;
        CHECK(QueryPerformanceCounter(&first));
        for(u32 i=program_begin;i<program_end;++i)CHECK(staged_build(programs,byte_load_be16(native.color.bases_v1.data+i*6)));
        CHECK(QueryPerformanceCounter(&last));native.cursor.outline_provider=profiled_outline;
        const u64 ticks=last.QuadPart-first.QuadPart;u64 minimum_ticks=~0ull;
        for(u32 i=0;i<256;++i){CHECK(QueryPerformanceCounter(&first)&&QueryPerformanceCounter(&last));minimum_ticks=minimum(minimum_ticks,static_cast<u64>(last.QuadPart-first.QuadPart));}
        print(",\"profile_build_ticks\":");number(ticks);print(",\"profile_outline_ticks\":");number(outline_ticks);
        print(",\"profile_outline_calls\":");number(outline_calls);print(",\"qpc_pair_min_ticks\":");number(minimum_ticks);
        print(",\"maximum_outline_commands\":");number(maximum_outline_commands);
        print(",\"outline_capacity_counts\":[");for(u32 i=0;i<6;++i){if(i)print(",");number(outline_capacity_counts[i]);}print("]");
        print(",\"maximum_requested_capacity\":");number(maximum_requested_capacity);print(",\"requested_capacity_total\":");number(requested_capacity_total);
        print(",\"requested_capacity_counts\":[");for(u32 i=0;i<6;++i){if(i)print(",");number(requested_capacity_counts[i]);}print("]");
        print(",\"repeated_outlines\":");number(repeated_outlines);print(",\"repeated_outline_ticks\":");number(repeated_outline_ticks);print(",\"repeated_commands\":");number(repeated_commands);
        print(",\"profile_source_ticks\":");number(source_ticks);print(",\"profile_source_calls\":");number(source_calls);
    }
    print(",\"program_committed\":[");number(programs[0].storage.committed);print(",");number(programs[1].storage.committed);print("],\"reference_program_bytes\":");number(reference_bytes);print(",\"equivalent_programs\":true}\n");return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_colr_cost_entry() noexcept {ExitProcess(maicord::run()?0:1);}
