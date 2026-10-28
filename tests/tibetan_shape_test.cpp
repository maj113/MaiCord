// MAICORD_TIBETAN_OWNED_TEST=1 runs the owned-units checks against the Windows Himalaya font.
#include "maicord/text/tibetan_shape_run.hpp"
#include "maicord/text/cluster_run.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/use_plan.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/byte_order.hpp"
#include "gsub_program_fixture.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
u32 checks=0;
#include "script_shape_test_support.inc"
bool fail(u32 line) noexcept {print("FAIL Tibetan shape line ");number(line);print("\n");return false;}
#define CHECK(x) do{++checks;if(!(x))return fail(__LINE__);}while(false)
bool units(SfntShapeStorage s) noexcept {
    const u32 entries[][2]{{0x20,1},{0xf3f,5},{0xf40,2},{0xf71,6},{0xf72,3},{0xf73,7},{0xf7f,14},{0xf80,8},{0xf90,4},{0xfb2,9},{0x200c,10},{0x200d,11},{0x25cc,12},{0xfe00,13}};
    u8 bytes[184]{};byte_store_be32(bytes,0x000c0000);byte_store_be32(bytes+4,sizeof(bytes));byte_store_be32(bytes+12,14);
    for(u32 i=0;i<14;++i){byte_store_be32(bytes+16+i*12,entries[i][0]);byte_store_be32(bytes+20+i*12,entries[i][0]);byte_store_be32(bytes+24+i*12,entries[i][1]);}
    SfntCmap cmap{{bytes,sizeof(bytes)},{},32,12};SfntShapePlan plan{};plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* value) noexcept {*value=600;return SfntStatus::ok;};SfntShapeRunResult result{};
    const auto shape=[&](ConstSlice<u32> input,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {return sfnt_tibetan_shape_run(plan,cmap,input,1000,s,&result,{},limits);};
    const u32 stack[]{0xf40,0xf90,0xf72},split[]{0xf40,0xf73},orphan[]{0xf73},pre[]{0xf40,0xf3f},spacing[]{0xf7f},control[]{0xf40,0x200d};
    CHECK(shape({stack,3})==SfntStatus::ok&&result.glyph_count==3&&s.glyphs[0].glyph.glyph==2&&s.glyphs[1].glyph.glyph==4&&s.glyphs[2].glyph.glyph==3);
    for(u32 i=0;i<3;++i)CHECK(!s.character_glyphs[i]);
    CHECK(shape({split,2})==SfntStatus::ok&&result.glyph_count==3&&s.glyphs[1].glyph.glyph==6&&s.glyphs[2].glyph.glyph==3);
    CHECK(shape({orphan,1})==SfntStatus::ok&&result.glyph_count==3&&s.glyphs[0].glyph.glyph==12&&s.glyphs[1].glyph.glyph==6);
    CHECK(shape({pre,2})==SfntStatus::ok&&result.glyph_count==2&&s.glyphs[0].glyph.glyph==5&&s.glyphs[1].glyph.glyph==2);
    CHECK(shape({spacing,1})==SfntStatus::ok&&result.glyph_count==1&&s.glyphs[0].glyph.glyph==14);
    CHECK(shape({control,2})==SfntStatus::ok&&result.glyph_count==2&&s.glyphs[1].glyph.glyph==1&&s.placements[1].advance==0);
    const u64 full=result.lookup_operations;
    for(u64 budget=0;budget<=full;++budget){const auto status=shape({control,2},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==full?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    const u32 invalid[]{0xf40,0xd800};
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({}, {})==SfntStatus::ok&&!result.glyph_count);
    const u16 words[]{2,8,1,9,1,1,4};u8 substitution[sizeof(words)];
    for(u32 i=0;i<7;++i){substitution[i*2]=static_cast<u8>(words[i]>>8);substitution[i*2+1]=static_cast<u8>(words[i]);}
    SfntGsubExecutableSubtable subtable{};SfntGsubExecutableLookup lookup{};
    CHECK(sfnt_gsub_subtable_open({substitution,sizeof(substitution)},1,32,&subtable.basic)==SfntStatus::ok);
    lookup.type=1;lookup.subtables={&subtable,1};SfntGsubProgram program{{&lookup,1},{},32};plan.substitutions=&program;
    const SfntLookupStep pref[]{ {0,0,2} };
    CHECK(sfnt_tibetan_shape_run(plan,cmap,{stack,3},1000,s,&result,{pref,1})==SfntStatus::ok&&result.glyph_count==3);
    CHECK(s.glyphs[0].glyph.glyph==9&&s.glyphs[1].glyph.glyph==2&&s.glyphs[2].glyph.glyph==3);
    const u64 pref_work=result.lookup_operations;
    for(u64 budget=0;budget<=pref_work;++budget){
        const auto status=sfnt_tibetan_shape_run(plan,cmap,{stack,3},1000,s,&result,{pref,1},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==pref_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);
    }
    const u32 sequence[]{0xf40,0xf73,0xf40,0xf90,0xf72};
    CHECK(shape({sequence,5})==SfntStatus::ok&&result.glyph_count==6&&result.source_units==5);
    CHECK(!s.character_glyphs[0]&&!s.character_glyphs[1]&&s.character_glyphs[2]==3&&s.character_glyphs[4]==3);
    const u64 run_work=result.lookup_operations;
    for(u64 budget=0;budget<=run_work;++budget){const auto status=shape({sequence,5},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==run_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    u32 long_input[192];for(u32 i=0;i<192;++i)long_input[i]=i%3==0?0xf40:i%3==1?0xf90:0xf72;
    CHECK(shape({long_input,192})==SfntStatus::ok&&result.glyph_count==192&&result.source_units==192);
    for(u32 i=0;i<192;++i)CHECK(s.character_glyphs[i]==i/3*3);
    // Input may occupy the exact prefix of component storage: mapping finishes
    // before the shared executor writes component records.
    auto* borrowed=reinterpret_cast<u32*>(s.glyphs.data);copy_bytes(borrowed,sequence,sizeof(sequence));
    CHECK(shape({borrowed,5})==SfntStatus::ok&&result.glyph_count==6&&s.character_glyphs[2]==3);
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(shape({sequence,5},{100000,100000,39})==SfntStatus::resource_limit&&!result.glyph_count);
    CHECK(shape({})==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    auto small=s;small.character_glyphs.count=4;
    CHECK(sfnt_tibetan_shape_run(plan,cmap,{sequence,5},1000,small,&result,{})==SfntStatus::output_too_small&&!result.glyph_count);
    small=s;small.targets.count=1;
    CHECK(sfnt_tibetan_shape_run(plan,cmap,{sequence,5},1000,small,&result,{})==SfntStatus::output_too_small&&!result.glyph_count);
    small=s;small.positions.count=0;
    CHECK(sfnt_tibetan_shape_run(plan,cmap,{sequence,5},1000,small,&result,{})==SfntStatus::output_too_small&&!result.glyph_count);
    const u32 stacks[]{0xf40,0xf90,0xf72,0xf40,0xf90,0xf72};
    CHECK(sfnt_tibetan_shape_run(plan,cmap,{stacks,6},1000,s,&result,{pref,1})==SfntStatus::ok&&result.glyph_count==6);
    for(u32 i=0;i<2;++i)CHECK(s.glyphs[i*3].glyph.glyph==9&&s.glyphs[i*3+1].glyph.glyph==2&&s.character_glyphs[i*3+2]==i*3);
    print("PASS Tibetan run shape; assertions=");number(checks);print("\n");return true;
}
bool flag(const wchar_t* name) noexcept {wchar_t v[2];return GetEnvironmentVariableW(name,v,2)==1&&v[0]=='1';}
#include "tibetan_owned_tests.inc"
int run() noexcept {
    Arena a{};if(!arena_initialize(&a,16*1024*1024,64*1024))return 2;
    struct Release {Arena& a;~Release() noexcept {arena_release(&a);}} release{a};auto s=storage(a);
    if(!flag(L"MAICORD_TIBETAN_OWNED_TEST"))return units(s)?0:1;
    wchar_t path[32768];const u32 n=GetWindowsDirectoryW(path,32700);if(!n||n>=32700)return 2;
    const wchar_t tail[]=L"\\Fonts\\himalaya.ttf";copy_bytes(path+n,tail,sizeof(tail));return owned_stream(path,a,s)?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_tibetan_shape_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
