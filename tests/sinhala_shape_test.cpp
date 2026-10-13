// No argument runs the registered checks. Manual: MAICORD_SINHALA_FONT=<font file> streams that
// font's shaping results for external comparison (QA only).
#include "maicord/text/sinhala_shape_cluster.hpp"
#include "maicord/text/sinhala_shape_run.hpp"
#include "maicord/text/sinhala_run.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/use_plan.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
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
    const auto shape=[&](ConstSlice<u32> input,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {return sfnt_sinhala_shape_cluster(plan,cmap,input,1000,s,&result,{},limits);};
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
    const u32 invalid[]{0xd9a,0xd800},separate[]{0xd9a,0xd9a};
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({separate,2})==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    CHECK(shape({}, {})==SfntStatus::ok&&!result.glyph_count);
    auto small=s;small.glyphs.count=2;
    CHECK(sfnt_sinhala_shape_cluster(plan,cmap,{split,2},1000,small,&result,{})==SfntStatus::output_too_small&&!result.glyph_count);
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
    CHECK(sfnt_sinhala_shape_cluster(plan,cmap,{forms,8},1000,s,&result,{steps,2})==SfntStatus::ok&&result.glyph_count==4);
    const u16 formed[]{6,13,2,11};for(u32 i=0;i<4;++i)CHECK(s.glyphs[i].glyph.glyph==formed[i]);
    for(u32 i=0;i<8;++i)CHECK(!s.character_glyphs[i]);
    const u64 formed_work=result.lookup_operations;
    for(u64 budget=0;budget<=formed_work;++budget){
        const auto status=sfnt_sinhala_shape_cluster(plan,cmap,{forms,8},1000,s,&result,{steps,2},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==formed_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);
    }
    const u32 late[]{0xd9a,0xdca,0xd9a,0xdca,0xdbb,0xdca,0x200d,0xd9a,0xdd9};
    CHECK(sfnt_sinhala_shape_cluster(plan,cmap,{late,9},1000,s,&result,{steps,2})==SfntStatus::ok&&result.glyph_count==7);
    const u16 late_expected[]{2,4,2,4,6,2,11};for(u32 i=0;i<7;++i)CHECK(s.glyphs[i].glyph.glyph==late_expected[i]);
    const u32 twice[]{0xdbb,0xdca,0x200d,0xdbb,0xdca,0x200d,0xd9a,0xdd9};
    CHECK(sfnt_sinhala_shape_cluster(plan,cmap,{twice,8},1000,s,&result,{steps,2})==SfntStatus::ok&&result.glyph_count==4);
    const u16 twice_expected[]{6,11,2,11};for(u32 i=0;i<4;++i)CHECK(s.glyphs[i].glyph.glyph==twice_expected[i]);
    small=s;small.positions.count=1;
    CHECK(sfnt_sinhala_shape_cluster(plan,cmap,{forms,8},1000,small,&result,{steps,2})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
    small=s;small.components.count=1;
    CHECK(sfnt_sinhala_shape_cluster(plan,cmap,{forms,8},1000,small,&result,{steps,2})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
    SfntLookupStep masked[]{steps[0]};masked[0].feature_mask=1;
    CHECK(sfnt_sinhala_shape_cluster(plan,cmap,{forms,8},1000,s,&result,{masked,1})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
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
bool read(void* p,u32 n) noexcept {auto* b=static_cast<u8*>(p);while(n){DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),b,n,&got,nullptr)||!got)return false;b+=got;n-=got;}return true;}
bool write(const void* p,u32 n) noexcept {auto* b=static_cast<const u8*>(p);while(n){DWORD done=0;if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),b,n,&done,nullptr)||!done)return false;b+=done;n-=done;}return true;}
bool flag(const wchar_t* name) noexcept {wchar_t v[2];return GetEnvironmentVariableW(name,v,2)==1&&v[0]=='1';}
#include "sinhala_owned_tests.inc"
bool font_stream(ConstSlice<u8> bytes,Arena& a,SfntShapeStorage s) noexcept {
    SfntFace face{};SfntCmap cmap{};SfntHorizontalMetrics metrics{};SfntGdef gdef{};ConstSlice<u8> table{};
    CHECK(sfnt_open(bytes,0,&face)==SfntStatus::ok&&sfnt_cmap_open(face,&cmap)==SfntStatus::ok&&sfnt_horizontal_metrics_open(face,&metrics)==SfntStatus::ok);
    auto status=sfnt_table(face,sfnt_tag('G','D','E','F'),&table);
    if(status!=SfntStatus::missing_table)CHECK(status==SfntStatus::ok&&sfnt_gdef_open(table,metrics.glyph_count,&gdef)==SfntStatus::ok);
    SfntGsubProgram gsub{};SfntGposProgram gpos{};SfntLookupStep steps[4096],chosen[4096];u32 step_count=0,root_count=0;u16 roots[4096];u8 scratch[4096];
    const bool independent=flag(L"MAICORD_SINHALA_INDEPENDENT"),rtl=flag(L"MAICORD_SINHALA_RTL");
    for(u32 kind=0;kind<2;++kind){SfntLayout layout{};SfntLookupPlanResult result{};
        status=sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&table);if(status==SfntStatus::missing_table)continue;
        CHECK(status==SfntStatus::ok&&sfnt_layout_open(table,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        CHECK(sfnt_sinhala_lookup_plan(layout,{},independent,{scratch,4096},{chosen,4096},&result)==SfntStatus::ok&&result.language_found);
        if(!kind){step_count=result.lookup_count;copy_bytes(steps,chosen,step_count*sizeof(SfntLookupStep));SfntGsubProgramRequirements n{};
            CHECK(sfnt_gsub_program_requirements(layout,&n)==SfntStatus::ok);
            CHECK(sfnt_gsub_program_prepare(layout,gdef,metrics.glyph_count,{allocate<SfntGsubExecutableLookup>(a,n.lookups),allocate<SfntGsubExecutableSubtable>(a,n.subtables),allocate<SfntCoverage>(a,n.coverages),allocate<SfntClassDef>(a,n.classes)},&gsub)==SfntStatus::ok);
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
        const auto shaped=flag(L"MAICORD_SINHALA_RUN")?sfnt_sinhala_shape_run(plan,cmap,{input,n},1000,s,&result,{steps,step_count},{},rtl):
            sfnt_sinhala_shape_cluster(plan,cmap,{input,n},1000,s,&result,{steps,step_count},{},rtl);
        const u32 code=static_cast<u32>(shaped);CHECK(result.glyph_count<=512);
        u32 units=0;for(u32 i=0;i<n;++i){sources[i]=shaped==SfntStatus::ok?s.character_glyphs[units]:0;units+=input[i]>0xffff?2u:1u;}
        for(u32 i=0;i<result.glyph_count;++i)glyphs[i]=s.glyphs[i].glyph.glyph;
        CHECK(write(&code,4)&&write(&result.glyph_count,4)&&write(glyphs,result.glyph_count*2)&&write(sources,n*4)&&write(s.placements.data,result.glyph_count*sizeof(SfntPlacement)));
    }return true;
}
int run() noexcept {
    Arena a{};if(!arena_initialize(&a,16*1024*1024,64*1024))return 2;
    struct Release {Arena& a;~Release() noexcept {arena_release(&a);}} release{a};auto s=storage(a);
    if(flag(L"MAICORD_SINHALA_OWNED_TEST")){wchar_t path[32768];const u32 n=GetWindowsDirectoryW(path,32700);if(!n||n>=32700)return 2;
        const wchar_t tail[]=L"\\Fonts\\Nirmala.ttc";copy_bytes(path+n,tail,sizeof(tail));return owned_stream(path,a,s,true)?0:1;}
    wchar_t path[32768];const DWORD n=GetEnvironmentVariableW(L"MAICORD_SINHALA_FONT",path,32768);
    if(!n)return units(s)?0:1;if(n>=32768)return 2;
    if(flag(L"MAICORD_SINHALA_OWNED"))return owned_stream(path,a,s,false)?0:1;
    const HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return 2;
    LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size)||size.QuadPart<12||size.QuadPart>0xffffffffll){CloseHandle(file);return 2;}
    const HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);
    const auto* bytes=mapping?static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;
    const bool ok=bytes&&font_stream({bytes,static_cast<u32>(size.QuadPart)},a,s);
    if(bytes)UnmapViewOfFile(bytes);if(mapping)CloseHandle(mapping);CloseHandle(file);return ok?0:3;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_sinhala_shape_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
