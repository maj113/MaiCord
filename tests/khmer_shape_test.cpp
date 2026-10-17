// No argument runs the registered checks. Manual: MAICORD_KHMER_FONT=<font file> streams that
// font's shaping results for external comparison (QA only).
#include "maicord/text/khmer_shape_run.hpp"
#include "maicord/text/khmer_run.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/khmer_plan.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/byte_order.hpp"
#include "gsub_program_fixture.hpp"
#include <Windows.h>
namespace maicord {namespace {
u32 checks=0;
#include "generated/khmer_shape_fixture.inc"
#include "script_shape_test_support.inc"
bool fail(u32 line) noexcept {print("FAIL Khmer shape line ");number(line);print("\n");return false;}
#define CHECK(x) do{++checks;if(!(x))return fail(__LINE__);}while(false)
bool units(SfntShapeStorage s) noexcept {
    const u32 entries[][2]{{0x20,1},{0x1780,2},{0x179a,3},{0x17be,4},{0x17c1,5},{0x17d2,6},{0x200d,7},{0x25cc,8}};
    u8 bytes[112]{};byte_store_be32(bytes,0x000c0000);byte_store_be32(bytes+4,sizeof(bytes));byte_store_be32(bytes+12,8);
    for(u32 i=0;i<8;++i){byte_store_be32(bytes+16+i*12,entries[i][0]);byte_store_be32(bytes+20+i*12,entries[i][0]);byte_store_be32(bytes+24+i*12,entries[i][1]);}
    SfntCmap cmap{{bytes,sizeof(bytes)},{},32,12};SfntShapePlan plan{};plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* v) noexcept {*v=600;return SfntStatus::ok;};SfntShapeRunResult result{};
    const u32 text[]{0x1780,0x17d2,0x179a,0x17be},orphan[]{0x17be},control[]{0x1780,0x200d};
    const auto shape=[&](ConstSlice<u32> input,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {return sfnt_khmer_shape_run(plan,cmap,input,1000,s,&result,{},limits);};
    CHECK(shape({text,4})==SfntStatus::ok&&result.glyph_count==5&&result.source_units==4);
    const u16 expected[]{5,6,3,2,4};for(u32 i=0;i<5;++i)CHECK(s.glyphs[i].glyph.glyph==expected[i]&&s.placements[i].advance==600);
    for(u32 i=0;i<4;++i)CHECK(!s.character_glyphs[i]);
    const u64 full=result.lookup_operations;
    for(u64 budget=0;budget<=full;++budget){const auto status=shape({text,4},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==full?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    CHECK(shape({orphan,1})==SfntStatus::ok&&result.glyph_count==3&&s.glyphs[0].glyph.glyph==5&&s.glyphs[1].glyph.glyph==8&&s.glyphs[2].glyph.glyph==4);
    CHECK(shape({control,2})==SfntStatus::ok&&result.glyph_count==2&&s.glyphs[1].glyph.glyph==1&&s.placements[1].advance==0);
    const u32 invalid[]{0x1780,0xd800},separate[]{0x1780,0x1780};
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count);
    SfntLayout layout{};SfntGsubProgram gsub{};SfntGsubExecutableLookup sl[4]{};SfntGsubExecutableSubtable st[4]{};
    SfntCoverage coverage[4]{};SfntClassDef classes[4]{};
    CHECK(sfnt_layout_open({khmer_shape_gsub,sizeof(khmer_shape_gsub)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
    CHECK(prepare_gsub_program(layout,{},32,{{sl,4},{st,4},{coverage,4},{classes,4}},&gsub)==SfntStatus::ok);
    plan.substitutions=&gsub;const SfntLookupStep root[]{0,0,0};
    CHECK(sfnt_khmer_shape_run(plan,cmap,{control,2},1000,s,&result,{root,1})==SfntStatus::ok&&result.glyph_count==2);
    // Swapping a visible base and ZWJ changes which final ID is invisible.
    CHECK(s.glyphs[0].glyph.glyph==1&&s.placements[0].advance==0&&s.glyphs[1].glyph.glyph==2&&s.placements[1].advance==600);
    const u64 lookup_work=result.lookup_operations;
    for(u64 budget=0;budget<=lookup_work;++budget){
        const auto status=sfnt_khmer_shape_run(plan,cmap,{control,2},1000,s,&result,{root,1},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==lookup_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);
    }
    plan.substitutions=nullptr;
    CHECK(sfnt_gdef_open({khmer_shape_gdef,sizeof(khmer_shape_gdef)},32,&plan.gdef)==SfntStatus::ok);
    CHECK(sfnt_layout_open({khmer_shape_gpos,sizeof(khmer_shape_gpos)},SfntLayoutKind::positioning,&layout)==SfntStatus::ok);
    SfntGposProgram gpos{};SfntGposExecutableLookup pl[2]{};SfntGposSubtable basic[2]{};
    SfntGposAttachment attachments[1]{};SfntContext contexts[1]{};
    CHECK(sfnt_gpos_program_prepare(layout,plan.gdef,32,{{pl,2},{basic,2},{attachments,1},{contexts,1},{coverage,4},{classes,4}},&gpos)==SfntStatus::ok);
    const u16 proots[]{0,1};plan.positioning=&gpos;plan.positioning_roots={proots,2};
    const u32 positioned[]{0x1780,0x17be,0x200d};
    CHECK(shape({positioned,3})==SfntStatus::ok&&result.glyph_count==4);
    CHECK(s.placements[2].advance==0&&s.placements[2].advance_offset==11&&s.placements[2].ascender_offset==17);
    CHECK(s.placements[3].advance==0&&s.placements[3].advance_offset==44&&s.placements[3].ascender_offset==19);
    for(u32 work=0;work<16;++work){const auto status=shape({text,4},{1000000,100000,work});
        CHECK(status==SfntStatus::resource_limit&&!result.glyph_count&&!result.source_units);}
    plan.positioning=nullptr;plan.positioning_roots={};plan.gdef={};plan.substitutions=&gsub;
    const SfntLookupStep ligature[]{1,0,0},decompose[]{{1,0,0},{2,0,1}};
    const auto run=[&](ConstSlice<u32> input,ConstSlice<SfntLookupStep> schedule=ConstSlice<SfntLookupStep>{},SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return sfnt_khmer_shape_run(plan,cmap,input,1000,s,&result,schedule,limits);};
    CHECK(run({separate,2})==SfntStatus::ok&&result.glyph_count==2&&s.character_glyphs[0]==0&&s.character_glyphs[1]==1);
    CHECK(run({separate,2},{ligature,1})==SfntStatus::ok&&result.glyph_count==1&&s.glyphs[0].glyph.glyph==3&&!s.character_glyphs[1]);
    CHECK(run({separate,2},{decompose,2})==SfntStatus::ok&&result.glyph_count==2&&s.glyphs[0].glyph.glyph==2&&s.glyphs[1].glyph.glyph==2&&s.character_glyphs[1]==1);
    const u64 run_work=result.lookup_operations;
    for(u64 budget=0;budget<=run_work;++budget){const auto status=run({separate,2},{decompose,2},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==run_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    bool stage_passed=false;
    for(u32 budget=0;budget<100;++budget){const auto status=run({separate,2},{decompose,2},{1000000,100000,budget});
        CHECK(status==SfntStatus::ok||status==SfntStatus::resource_limit);
        if(status==SfntStatus::ok)stage_passed=true;else CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    CHECK(stage_passed);
    for(u32 kind=0;kind<4;++kind){auto short_buffer=s;
        if(kind==0)short_buffer.targets.count=1;if(kind==1)short_buffer.character_glyphs.count=1;
        if(kind==2)short_buffer.flags.count=1;if(kind==3)short_buffer.positions.count=1;
        CHECK(sfnt_khmer_shape_run(plan,cmap,{separate,2},1000,short_buffer,&result,{})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);}
    const u32 supplementary[]{0x1780,0xe0100,0x1780};
    CHECK(run({supplementary,3})==SfntStatus::ok&&result.glyph_count==2&&result.source_units==4&&
        !s.character_glyphs[0]&&!s.character_glyphs[1]&&!s.character_glyphs[2]&&s.character_glyphs[3]==1);
    auto* borrowed=reinterpret_cast<u32*>(s.components.data);copy_bytes(borrowed,separate,sizeof(separate));
    CHECK(run({borrowed,2},{decompose,2})==SfntStatus::ok&&result.glyph_count==2&&s.character_glyphs[1]==1);
    CHECK(run({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(run({})==SfntStatus::ok&&!result.glyph_count);
    u32 long_run[256];for(u32 i=0;i<256;++i)long_run[i]=i%2?0x17be:0x1780;
    CHECK(run({long_run,256})==SfntStatus::ok&&result.glyph_count==384&&result.source_units==256);
    for(u32 i=0;i<256;++i)CHECK(s.character_glyphs[i]==(i/2)*3);
    print("PASS Khmer run shape units; assertions=");number(checks);print("\n");return true;
}
bool read(void* p,u32 n) noexcept {auto* b=static_cast<u8*>(p);while(n){DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),b,n,&got,nullptr)||!got)return false;b+=got;n-=got;}return true;}
bool write(const void* p,u32 n) noexcept {auto* b=static_cast<const u8*>(p);while(n){DWORD done=0;if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),b,n,&done,nullptr)||!done)return false;b+=done;n-=done;}return true;}
bool flag(const wchar_t* name) noexcept {wchar_t v[2];return GetEnvironmentVariableW(name,v,2)==1&&v[0]=='1';}
#include "khmer_owned_tests.inc"
bool font_stream(ConstSlice<u8> bytes,Arena& a,SfntShapeStorage s) noexcept {
    SfntFace face{};SfntCmap cmap{};SfntHorizontalMetrics metrics{};SfntGdef gdef{};ConstSlice<u8> table{};
    CHECK(sfnt_open(bytes,0,&face)==SfntStatus::ok&&sfnt_cmap_open(face,&cmap)==SfntStatus::ok&&sfnt_horizontal_metrics_open(face,&metrics)==SfntStatus::ok);
    auto status=sfnt_table(face,sfnt_tag('G','D','E','F'),&table);
    if(status!=SfntStatus::missing_table)CHECK(status==SfntStatus::ok&&sfnt_gdef_open(table,metrics.glyph_count,&gdef)==SfntStatus::ok);
    SfntGsubProgram gsub{};SfntGposProgram gpos{};SfntLookupStep steps[4096],chosen[4096];u32 step_count=0,root_count=0;u16 roots[4096];u8 scratch[4096];
    const bool rtl=flag(L"MAICORD_KHMER_RTL");
    for(u32 kind=0;kind<2;++kind){SfntLayout layout{};SfntLookupPlanResult result{};
        status=sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&table);if(status==SfntStatus::missing_table)continue;
        CHECK(status==SfntStatus::ok&&sfnt_layout_open(table,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,4096},{chosen,4096},&result)==SfntStatus::ok&&result.language_found);
        if(!kind){step_count=result.lookup_count;copy_bytes(steps,chosen,step_count*sizeof(SfntLookupStep));SfntGsubProgramRequirements n{};
            CHECK(gsub_program_needs(layout,&n)==SfntStatus::ok);
            CHECK(prepare_gsub_program(layout,gdef,metrics.glyph_count,{allocate<SfntGsubExecutableLookup>(a,n.lookups),allocate<SfntGsubExecutableSubtable>(a,n.subtables),allocate<SfntCoverage>(a,n.coverages),allocate<SfntClassDef>(a,n.classes)},&gsub,true,true)==SfntStatus::ok);
        }else{root_count=result.lookup_count;for(u32 i=0;i<root_count;++i)roots[i]=chosen[i].lookup;SfntGposProgramRequirements n{};
            CHECK(sfnt_gpos_program_requirements(layout,&n)==SfntStatus::ok);
            CHECK(sfnt_gpos_program_prepare(layout,gdef,metrics.glyph_count,{allocate<SfntGposExecutableLookup>(a,n.lookups),allocate<SfntGposSubtable>(a,n.basic),allocate<SfntGposAttachment>(a,n.attachments),allocate<SfntContext>(a,n.contexts),allocate<SfntCoverage>(a,n.coverages),allocate<SfntClassDef>(a,n.classes)},&gpos)==SfntStatus::ok);
        }
    }
    SfntShapePlan plan{};plan.substitutions=&gsub;plan.positioning=&gpos;plan.positioning_roots={roots,root_count};plan.gdef=gdef;plan.units_per_em=metrics.units_per_em;plan.metric_context=&metrics;plan.refresh_substitution_classes=true;
    plan.advance=+[](void* p,u16 glyph,double* value) noexcept {SfntGlyphMetrics m{};const auto status=sfnt_horizontal_metrics(*static_cast<SfntHorizontalMetrics*>(p),glyph,&m);*value=m.advance;return status;};
    u8 header[12];CHECK(read(header,12)&&bytes_equal(header,"MDSHP1\0\0",8));const u32 cases=byte_load_le32(header+8);CHECK(cases&&cases<=100000&&write(header,12));
    for(u32 row=0;row<cases;++row){u32 n=0,input[256],sources[256];u16 glyphs[512];SfntShapeRunResult result{};
        CHECK(read(&n,4)&&n&&n<=256&&read(input,n*4));
        const auto shaped=sfnt_khmer_shape_run(plan,cmap,{input,n},1000,s,&result,{steps,step_count},{},rtl);
        const u32 code=static_cast<u32>(shaped);CHECK(result.glyph_count<=512);
        u32 units=0;for(u32 i=0;i<n;++i){sources[i]=shaped==SfntStatus::ok?s.character_glyphs[units]:0;units+=input[i]>0xffff?2u:1u;}
        for(u32 i=0;i<result.glyph_count;++i)glyphs[i]=s.glyphs[i].glyph.glyph;
        CHECK(write(&code,4)&&write(&result.glyph_count,4)&&write(glyphs,result.glyph_count*2)&&write(sources,n*4)&&write(s.placements.data,result.glyph_count*sizeof(SfntPlacement)));
    }return true;
}
int run() noexcept {
    Arena a{};if(!arena_initialize(&a,16*1024*1024,64*1024))return 2;
    struct Release {Arena& a;~Release() noexcept {arena_release(&a);}} release{a};auto s=storage(a);
    if(flag(L"MAICORD_KHMER_OWNED_TEST")){wchar_t path[32768];const u32 n=GetWindowsDirectoryW(path,32700);if(!n||n>=32700)return 2;
        const wchar_t tail[]=L"\\Fonts\\leelawui.ttf";copy_bytes(path+n,tail,sizeof(tail));return owned_stream(path,a,s,true)?0:1;}
    wchar_t path[32768];const DWORD n=GetEnvironmentVariableW(L"MAICORD_KHMER_FONT",path,32768);
    if(!n)return units(s)?0:1;if(n>=32768)return 2;
    if(flag(L"MAICORD_KHMER_OWNED"))return owned_stream(path,a,s,false)?0:1;
    const HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return 2;
    LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size)||size.QuadPart<12||size.QuadPart>0xffffffffll){CloseHandle(file);return 2;}
    const HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);
    const auto* bytes=mapping?static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;
    const bool ok=bytes&&font_stream({bytes,static_cast<u32>(size.QuadPart)},a,s);
    if(bytes)UnmapViewOfFile(bytes);if(mapping)CloseHandle(mapping);CloseHandle(file);return ok?0:3;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_khmer_shape_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
