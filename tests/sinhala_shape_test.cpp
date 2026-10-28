// No argument runs the registered checks; MAICORD_SINHALA_OWNED_TEST=1 runs the owned-plan checks against the system font.
#include "maicord/text/sinhala_shape_run.hpp"
#include "maicord/text/cluster_run.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/byte_order.hpp"
#include "gsub_program_fixture.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
u32 checks=0;
#include "script_shape_test_support.inc"
bool fail(u32 line) noexcept {print("FAIL Sinhala shape line ");number(line);print("\n");return false;}
#define CHECK(x) do{++checks;if(!(x))return fail(__LINE__);}while(false)
bool units(SfntShapeStorage s) noexcept {
    const u32 entries[][2]{{0x20,1},{0xd9a,2},{0xdba,12},{0xdbb,3},{0xdca,4},{0xdcf,5},{0xdd9,6},{0xddd,7},{0xddf,8},{0x200d,9},{0x25cc,10}};
    u8 bytes[148]{};byte_store_be32(bytes,0x000c0000);byte_store_be32(bytes+4,sizeof(bytes));byte_store_be32(bytes+12,11);
    for(u32 i=0;i<11;++i){byte_store_be32(bytes+16+i*12,entries[i][0]);byte_store_be32(bytes+20+i*12,entries[i][0]);byte_store_be32(bytes+24+i*12,entries[i][1]);}
    SfntCmap cmap{{bytes,sizeof(bytes)},{},32,12};SfntShapePlan plan{};plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* v) noexcept {*v=600;return SfntStatus::ok;};SfntShapeRunResult result{};
    const u32 split[]{0xd9a,0xddd},orphan[]{0xddd},barrier[]{0xd9a,0xdca,0xd9a,0xdd9},control[]{0xd9a,0x200d};
    const auto shape=[&](ConstSlice<u32> input,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {return sfnt_sinhala_shape_run(plan,cmap,input,1000,s,&result,{},limits);};
    CHECK(shape({split,2})==SfntStatus::ok&&result.glyph_count==3&&result.source_units==2);
    CHECK(s.glyphs[0].glyph.glyph==6&&s.glyphs[1].glyph.glyph==2&&s.glyphs[2].glyph.glyph==7&&!s.character_glyphs[0]&&!s.character_glyphs[1]);
    CHECK(shape({orphan,1})==SfntStatus::ok&&result.glyph_count==3&&s.glyphs[0].glyph.glyph==6&&s.glyphs[1].glyph.glyph==10&&s.glyphs[2].glyph.glyph==7&&!s.character_glyphs[0]);
    CHECK(shape({barrier,4})==SfntStatus::ok&&result.glyph_count==4);
    const u16 expected[]{2,4,6,2};for(u32 i=0;i<4;++i)CHECK(s.glyphs[i].glyph.glyph==expected[i]&&!s.character_glyphs[i]);
    CHECK(shape({control,2})==SfntStatus::ok&&result.glyph_count==2&&s.glyphs[1].glyph.glyph==1&&s.placements[1].advance==0);
    const u64 full=result.lookup_operations;for(u64 budget=0;budget<=full;++budget){
        const auto status=shape({control,2},{budget,100000,1000000});CHECK(result.lookup_operations<=budget&&status==(budget==full?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);
    }
    const u32 invalid[]{0xd9a,0xd800};
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({}, {})==SfntStatus::ok&&!result.glyph_count);
    const u16 reph_words[]{1,8,1,14,1,1,3,1,4,11,3,4,9};
    const u16 pref_words[]{1,8,1,14,1,1,4,1,4,13,3,9,12};
    u8 reph_bytes[sizeof(reph_words)],pref_bytes[sizeof(pref_words)];
    for(u32 i=0;i<13;++i){reph_bytes[i*2]=static_cast<u8>(reph_words[i]>>8);reph_bytes[i*2+1]=static_cast<u8>(reph_words[i]);
        pref_bytes[i*2]=static_cast<u8>(pref_words[i]>>8);pref_bytes[i*2+1]=static_cast<u8>(pref_words[i]);}
    SfntGsubExecutableSubtable subtables[2]{};SfntGsubExecutableLookup lookups[2]{};
    CHECK(sfnt_gsub_subtable_open({reph_bytes,sizeof(reph_bytes)},4,32,&subtables[0].basic)==SfntStatus::ok);
    CHECK(sfnt_gsub_subtable_open({pref_bytes,sizeof(pref_bytes)},4,32,&subtables[1].basic)==SfntStatus::ok);
    for(u32 i=0;i<2;++i){lookups[i].type=4;lookups[i].subtables={subtables+i,1};}
    SfntGsubProgram substitutions{{lookups,2},{},32};plan.substitutions=&substitutions;
    const SfntLookupStep steps[]{{0,0,1},{1,0,2}};
    const u32 forms[]{0xdbb,0xdca,0x200d,0xd9a,0xdca,0x200d,0xdba,0xdd9};
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{forms,8},1000,s,&result,{steps,2})==SfntStatus::ok&&result.glyph_count==4);
    const u16 formed[]{6,13,2,11};for(u32 i=0;i<4;++i)CHECK(s.glyphs[i].glyph.glyph==formed[i]);
    for(u32 i=0;i<8;++i)CHECK(!s.character_glyphs[i]);
    const u64 formed_work=result.lookup_operations;
    for(u64 budget=0;budget<=formed_work;++budget){
        const auto status=sfnt_sinhala_shape_run(plan,cmap,{forms,8},1000,s,&result,{steps,2},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==formed_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);
    }
    const u32 late[]{0xd9a,0xdca,0xd9a,0xdca,0xdbb,0xdca,0x200d,0xd9a,0xdd9};
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{late,9},1000,s,&result,{steps,2})==SfntStatus::ok&&result.glyph_count==7);
    const u16 late_expected[]{2,4,2,4,6,2,11};for(u32 i=0;i<7;++i)CHECK(s.glyphs[i].glyph.glyph==late_expected[i]);
    const u32 twice[]{0xdbb,0xdca,0x200d,0xdbb,0xdca,0x200d,0xd9a,0xdd9};
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{twice,8},1000,s,&result,{steps,2})==SfntStatus::ok&&result.glyph_count==4);
    const u16 twice_expected[]{6,11,2,11};for(u32 i=0;i<4;++i)CHECK(s.glyphs[i].glyph.glyph==twice_expected[i]);
    SfntLookupStep masked[]{steps[0]};masked[0].feature_mask=1;
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{forms,8},1000,s,&result,{masked,1})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    const u32 run_input[]{0xd9a,0xddd,0xdbb,0xdca,0x200d,0xd9a,0xdd9};
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{run_input,7},1000,s,&result,{steps,2})==SfntStatus::ok&&result.glyph_count==6);
    const u16 run_expected[]{6,2,7,6,2,11};for(u32 i=0;i<6;++i)CHECK(s.glyphs[i].glyph.glyph==run_expected[i]);
    for(u32 i=0;i<7;++i)CHECK(s.character_glyphs[i]==(i<2?0u:3u));
    const u64 run_work=result.lookup_operations;
    for(u64 budget=0;budget<=run_work;++budget){
        const auto status=sfnt_sinhala_shape_run(plan,cmap,{run_input,7},1000,s,&result,{steps,2},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==run_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);
    }
    u32 long_run[128];for(u32 i=0;i<128;++i)long_run[i]=0xd9a;
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{long_run,128},1000,s,&result,{steps,2})==SfntStatus::ok&&result.glyph_count==128);
    for(u32 i=0;i<128;++i)CHECK(s.glyphs[i].glyph.glyph==2&&s.character_glyphs[i]==i&&s.placements[i].advance==600);
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{invalid,2},1000,s,&result,{steps,2})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{},1000,s,&result,{steps,2})==SfntStatus::ok&&!result.glyph_count);
    for(u32 kind=0;kind<4;++kind){auto short_run=s;
        if(kind==0)short_run.glyphs.count=3;if(kind==1)short_run.character_glyphs.count=1;
        if(kind==2)short_run.positions.count=1;if(kind==3)short_run.targets.count=1;
        CHECK(sfnt_sinhala_shape_run(plan,cmap,{run_input,7},1000,short_run,&result,{steps,2})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
    }
    const u16 late_words[]{1,8,1,14,1,1,2,1,4,11,2,3};u8 late_bytes[sizeof(late_words)];
    for(u32 i=0;i<12;++i){late_bytes[i*2]=static_cast<u8>(late_words[i]>>8);late_bytes[i*2+1]=static_cast<u8>(late_words[i]);}
    CHECK(sfnt_gsub_subtable_open({late_bytes,sizeof(late_bytes)},4,32,&subtables[0].basic)==SfntStatus::ok);
    const u32 cross[]{0xd9a,0xdd9,0xdbb};const SfntLookupStep presentation[]{{0,0,6}},early_only[]{{0,0,0}};
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{cross,3},1000,s,&result,{presentation,1})==SfntStatus::ok&&result.glyph_count==2);
    CHECK(s.glyphs[0].glyph.glyph==6&&s.glyphs[1].glyph.glyph==11&&!s.character_glyphs[0]&&!s.character_glyphs[1]&&s.character_glyphs[2]==1);
    CHECK(sfnt_sinhala_shape_run(plan,cmap,{cross,3},1000,s,&result,{early_only,1})==SfntStatus::ok&&result.glyph_count==3);
    CHECK(s.glyphs[0].glyph.glyph==6&&s.glyphs[1].glyph.glyph==2&&s.glyphs[2].glyph.glyph==3&&s.character_glyphs[2]==2);
    print("PASS Sinhala shape units; assertions=");number(checks);print("\n");return true;
}
bool flag(const wchar_t* name) noexcept {wchar_t v[2];return GetEnvironmentVariableW(name,v,2)==1&&v[0]=='1';}
#include "sinhala_owned_tests.inc"
int run() noexcept {
    Arena a{};if(!arena_initialize(&a,16*1024*1024,64*1024))return 2;
    struct Release {Arena& a;~Release() noexcept {arena_release(&a);}} release{a};auto s=storage(a);
    if(!flag(L"MAICORD_SINHALA_OWNED_TEST"))return units(s)?0:1;
    wchar_t path[32768];const u32 n=GetWindowsDirectoryW(path,32700);if(!n||n>=32700)return 2;
    const wchar_t tail[]=L"\\Fonts\\Nirmala.ttc";copy_bytes(path+n,tail,sizeof(tail));return owned_run(path,a,s)?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_sinhala_shape_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
