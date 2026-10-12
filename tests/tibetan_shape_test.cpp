// No argument runs the registered checks. Manual: MAICORD_TIBETAN_FONT=<font file> streams that
// font's shaping results for external comparison (QA only).
#include "maicord/text/tibetan_shape_cluster.hpp"
#include "maicord/text/tibetan_shape_run.hpp"
#include "maicord/text/tibetan_run.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/use_plan.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/byte_order.hpp"
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
    const auto shape=[&](ConstSlice<u32> input,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {return sfnt_tibetan_shape_cluster(plan,cmap,input,1000,s,&result,{},limits);};
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
    const u32 invalid[]{0xf40,0xd800},separate[]{0xf40,0xf40};
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({separate,2})==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    CHECK(shape({}, {})==SfntStatus::ok&&!result.glyph_count);
    auto small=s;small.glyphs.count=2;
    CHECK(sfnt_tibetan_shape_cluster(plan,cmap,{split,2},1000,small,&result,{})==SfntStatus::output_too_small&&!result.glyph_count);
    const u16 words[]{2,8,1,9,1,1,4};u8 substitution[sizeof(words)];
    for(u32 i=0;i<7;++i){substitution[i*2]=static_cast<u8>(words[i]>>8);substitution[i*2+1]=static_cast<u8>(words[i]);}
    SfntGsubExecutableSubtable subtable{};SfntGsubExecutableLookup lookup{};
    CHECK(sfnt_gsub_subtable_open({substitution,sizeof(substitution)},1,32,&subtable.basic)==SfntStatus::ok);
    lookup.type=1;lookup.subtables={&subtable,1};SfntGsubProgram program{{&lookup,1},{},32};plan.substitutions=&program;
    const SfntLookupStep pref[]{ {0,0,2} };
    CHECK(sfnt_tibetan_shape_cluster(plan,cmap,{stack,3},1000,s,&result,{pref,1})==SfntStatus::ok&&result.glyph_count==3);
    CHECK(s.glyphs[0].glyph.glyph==9&&s.glyphs[1].glyph.glyph==2&&s.glyphs[2].glyph.glyph==3);
    const u64 pref_work=result.lookup_operations;
    for(u64 budget=0;budget<=pref_work;++budget){
        const auto status=sfnt_tibetan_shape_cluster(plan,cmap,{stack,3},1000,s,&result,{pref,1},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==pref_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);
    }
    small=s;small.positions.count=0;
    CHECK(sfnt_tibetan_shape_cluster(plan,cmap,{stack,3},1000,small,&result,{pref,1})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
    const auto run_shape=[&](ConstSlice<u32> input,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return sfnt_tibetan_shape_run(plan,cmap,input,1000,s,&result,{},limits);};
    const u32 sequence[]{0xf40,0xf73,0xf40,0xf90,0xf72};
    CHECK(run_shape({sequence,5})==SfntStatus::ok&&result.glyph_count==6&&result.source_units==5);
    CHECK(!s.character_glyphs[0]&&!s.character_glyphs[1]&&s.character_glyphs[2]==3&&s.character_glyphs[4]==3);
    const u64 run_work=result.lookup_operations;
    for(u64 budget=0;budget<=run_work;++budget){const auto status=run_shape({sequence,5},{budget,100000,1000000});
        CHECK(result.lookup_operations<=budget&&status==(budget==run_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    u32 long_input[192];for(u32 i=0;i<192;++i)long_input[i]=i%3==0?0xf40:i%3==1?0xf90:0xf72;
    CHECK(run_shape({long_input,192})==SfntStatus::ok&&result.glyph_count==192&&result.source_units==192);
    for(u32 i=0;i<192;++i)CHECK(s.character_glyphs[i]==i/3*3);
    // Input may occupy the exact prefix of component storage: mapping finishes
    // before the shared executor writes component records.
    auto* borrowed=reinterpret_cast<u32*>(s.glyphs.data);copy_bytes(borrowed,sequence,sizeof(sequence));
    CHECK(run_shape({borrowed,5})==SfntStatus::ok&&result.glyph_count==6&&s.character_glyphs[2]==3);
    CHECK(run_shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(run_shape({sequence,5},{100000,100000,39})==SfntStatus::resource_limit&&!result.glyph_count);
    CHECK(run_shape({})==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    small=s;small.character_glyphs.count=4;
    CHECK(sfnt_tibetan_shape_run(plan,cmap,{sequence,5},1000,small,&result,{})==SfntStatus::output_too_small&&!result.glyph_count);
    small=s;small.targets.count=1;
    CHECK(sfnt_tibetan_shape_run(plan,cmap,{sequence,5},1000,small,&result,{})==SfntStatus::output_too_small&&!result.glyph_count);
    small=s;small.positions.count=0;
    CHECK(sfnt_tibetan_shape_run(plan,cmap,{sequence,5},1000,small,&result,{})==SfntStatus::output_too_small&&!result.glyph_count);
    const u32 stacks[]{0xf40,0xf90,0xf72,0xf40,0xf90,0xf72};
    CHECK(sfnt_tibetan_shape_run(plan,cmap,{stacks,6},1000,s,&result,{pref,1})==SfntStatus::ok&&result.glyph_count==6);
    for(u32 i=0;i<2;++i)CHECK(s.glyphs[i*3].glyph.glyph==9&&s.glyphs[i*3+1].glyph.glyph==2&&s.character_glyphs[i*3+2]==i*3);
    print("PASS Tibetan cluster/run shape; assertions=");number(checks);print("\n");return true;
}
bool read(void* p,u32 n) noexcept {auto* b=static_cast<u8*>(p);while(n){DWORD got=0;if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),b,n,&got,nullptr)||!got)return false;b+=got;n-=got;}return true;}
bool write(const void* p,u32 n) noexcept {auto* b=static_cast<const u8*>(p);while(n){DWORD done=0;if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),b,n,&done,nullptr)||!done)return false;b+=done;n-=done;}return true;}
bool flag(const wchar_t* name) noexcept {wchar_t v[2];return GetEnvironmentVariableW(name,v,2)==1&&v[0]=='1';}
#include "tibetan_owned_tests.inc"
bool font_stream(ConstSlice<u8> bytes,Arena& a,SfntShapeStorage s) noexcept {
    SfntFace face{};SfntCmap cmap{};SfntHorizontalMetrics metrics{};SfntGdef gdef{};ConstSlice<u8> table{};
    CHECK(sfnt_open(bytes,0,&face)==SfntStatus::ok&&sfnt_cmap_open(face,&cmap)==SfntStatus::ok&&sfnt_horizontal_metrics_open(face,&metrics)==SfntStatus::ok);
    auto status=sfnt_table(face,sfnt_tag('G','D','E','F'),&table);
    if(status!=SfntStatus::missing_table)CHECK(status==SfntStatus::ok&&sfnt_gdef_open(table,metrics.glyph_count,&gdef)==SfntStatus::ok);
    SfntGsubProgram gsub{};SfntGposProgram gpos{};SfntLookupStep steps[4096],chosen[4096];u32 step_count=0,root_count=0;u16 roots[4096];u8 scratch[4096];
    const bool independent=flag(L"MAICORD_TIBETAN_INDEPENDENT"),rtl=flag(L"MAICORD_TIBETAN_RTL"),full_run=flag(L"MAICORD_TIBETAN_RUN");
    for(u32 kind=0;kind<2;++kind){SfntLayout layout{};SfntLookupPlanResult result{};
        status=sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&table);if(status==SfntStatus::missing_table)continue;
        CHECK(status==SfntStatus::ok&&sfnt_layout_open(table,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        CHECK(sfnt_tibetan_lookup_plan(layout,{},independent,{scratch,4096},{chosen,4096},&result)==SfntStatus::ok);
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
        const auto shaped=full_run?sfnt_tibetan_shape_run(plan,cmap,{input,n},1000,s,&result,{steps,step_count},{},rtl):
            sfnt_tibetan_shape_cluster(plan,cmap,{input,n},1000,s,&result,{steps,step_count},{},rtl);
        const u32 code=static_cast<u32>(shaped);CHECK(result.glyph_count<=512);
        u32 units=0;for(u32 i=0;i<n;++i){sources[i]=shaped==SfntStatus::ok?s.character_glyphs[units]:0;units+=input[i]>0xffff?2u:1u;}
        for(u32 i=0;i<result.glyph_count;++i)glyphs[i]=s.glyphs[i].glyph.glyph;
        CHECK(write(&code,4)&&write(&result.glyph_count,4)&&write(glyphs,result.glyph_count*2)&&write(sources,n*4)&&write(s.placements.data,result.glyph_count*sizeof(SfntPlacement)));
    }return true;
}
int run() noexcept {
    Arena a{};if(!arena_initialize(&a,16*1024*1024,64*1024))return 2;
    struct Release {Arena& a;~Release() noexcept {arena_release(&a);}} release{a};auto s=storage(a);
    if(flag(L"MAICORD_TIBETAN_OWNED_TEST")){wchar_t path[32768];const u32 n=GetWindowsDirectoryW(path,32700);if(!n||n>=32700)return 2;
        const wchar_t tail[]=L"\\Fonts\\himalaya.ttf";copy_bytes(path+n,tail,sizeof(tail));return owned_stream(path,a,s,true)?0:1;}
    wchar_t path[32768];const DWORD n=GetEnvironmentVariableW(L"MAICORD_TIBETAN_FONT",path,32768);
    if(!n)return units(s)?0:1;if(n>=32768)return 2;
    if(flag(L"MAICORD_TIBETAN_OWNED"))return owned_stream(path,a,s,false)?0:1;
    const HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return 2;
    LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size)||size.QuadPart<12||size.QuadPart>0xffffffffll){CloseHandle(file);return 2;}
    const HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);
    const auto* bytes=mapping?static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;
    const bool ok=bytes&&font_stream({bytes,static_cast<u32>(size.QuadPart)},a,s);
    if(bytes)UnmapViewOfFile(bytes);if(mapping)CloseHandle(mapping);CloseHandle(file);return ok?0:3;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_tibetan_shape_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
