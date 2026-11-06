// The retained-plan checks of one script's cluster shaper on a system font (ScriptShapeOwner): arena accounting and
// failure atomicity, the compact output aliasing every input, malformed UTF-8 and the limits; then the nominal glyphs
// of a font without the script. Include after the test's CHECK macro, print and number helpers.
#include "profile_positioning_storage.inc"
#include "script_shape_owner.inc"
#include "maicord/core/win32_kernel.hpp"
struct OwnedScript {
    NativeClusterScript script;const char* name;
    u32 first,second;        // a base and the sign after it: the run of two scalars the checks shape
    u32 pair_glyphs;         // the glyphs that run shapes to
    bool unknown_is_space;   // a scalar the font lacks exports an invisible space (Myanmar, Tibetan) instead of failing
    const wchar_t* fallback; // the font without the script, under the Windows directory
    const u32* generic;      // Khmer: five scalars of a run the generic cluster rules shape on that font
};
bool owned_script_units(const OwnedScript& c,ScriptShapeOwner& owner,Arena& state,SfntShapeStorage s) noexcept {
    const auto script=c.script;
    auto& scratch=owner.scratch;arena_reset(&scratch);auto* sentinel=arena_allocate<u32>(&scratch);CHECK(sentinel);*sentinel=0x12345678;
    const auto before_state=arena_mark(&state),before_scratch=arena_mark(&scratch);const u32 refs=owner.plans[0].references;NativeClusterPlan prepared{};
    const auto unchanged=[&]() noexcept {return state.offset==before_state.offset&&scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&
        owner.plans[0].references==refs&&!prepared.run.slot&&!prepared.cmap&&!prepared.substitution_count&&!prepared.positioning_count;};
    CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,false,&state,&scratch,&prepared,0)==SfntStatus::resource_limit&&unchanged());
    CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,false,&state,&state,&prepared)==SfntStatus::invalid_input&&unchanged());
    auto stale=owner.run;++stale.generation;
    CHECK(native_cluster_plan_prepare(script,owner.store,stale,false,&state,&scratch,&prepared)==SfntStatus::invalid_input&&unchanged());
    Arena tiny{};CHECK(arena_initialize(&tiny,4096,4096,4096));struct Release {Arena& a;~Release() noexcept {arena_release(&a);}} release{tiny};
    CHECK(arena_allocate(&tiny,4095,1));const auto mark=arena_mark(&tiny);
    CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,false,&tiny,&scratch,&prepared)==SfntStatus::resource_limit&&unchanged()&&tiny.offset==mark.offset);
    CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,false,&state,&tiny,&prepared)==SfntStatus::output_too_small&&unchanged()&&tiny.offset==mark.offset);
    usize retained=0,total_retained=0;
    for(u32 mode=0;mode<2;++mode){const usize before=state.offset;
        CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,mode!=0,&state,&scratch,&prepared)==SfntStatus::ok);
        CHECK(prepared.independent_letters==(mode!=0)&&scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&owner.plans[0].references==refs);
        const usize bytes=prepared.substitution_count*sizeof(SfntLookupStep)+prepared.positioning_count*sizeof(u16);retained+=bytes;
        CHECK(profile_positioning_storage(owner.store,owner.run,prepared.substitutions(),prepared.positioning(),state,before,prepared.positioning_program,mode!=0));
        total_retained+=state.offset-before;
        auto invalid=prepared;++invalid.run.generation;invalid.cmap=reinterpret_cast<const SfntCmap*>(1);SfntShapeRunResult result{};
        CHECK(native_cluster_run(script,owner.store,invalid,{},1000,SfntShapeStorage{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        char text[16];u32 bytes_text=utf8_encode(c.first,text);bytes_text+=utf8_encode(c.second,text+bytes_text);
        CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes_text},1000,s,&result)==SfntStatus::ok&&result.source_units==2&&result.glyph_count==c.pair_glyphs);
        u16 expected[3];SfntPlacement placements[3];u32 sources[2];for(u32 i=0;i<c.pair_glyphs;++i)expected[i]=s.glyphs[i].glyph.glyph;
        copy_bytes(placements,s.placements.data,c.pair_glyphs*sizeof(SfntPlacement));copy_bytes(sources,s.character_glyphs.data,sizeof(sources));
        u16 glyphs[8];SfntPlacement placed[8];u32 characters[8];
        for(u32 alias=0;alias<8;++alias){NativeAsciiStorage compact{{},s,
            alias&1?Slice<u16>{reinterpret_cast<u16*>(s.glyphs.data),s.glyphs.count}:Slice<u16>{glyphs,8},
            alias&2?s.placements:Slice<SfntPlacement>{placed,8},alias&4?s.character_glyphs:Slice<u32>{characters,8}};NativeAsciiResult published{};
            CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes_text},1000,compact,&published)==SfntStatus::ok&&published.glyph_count==c.pair_glyphs&&published.source_units==2&&published.shaped_ranges==1);
            CHECK(bytes_equal(compact.glyphs.data,expected,c.pair_glyphs*sizeof(u16))&&bytes_equal(compact.placements.data,placements,c.pair_glyphs*sizeof(SfntPlacement))&&bytes_equal(compact.character_glyphs.data,sources,sizeof(sources)));}
        NativeAsciiStorage compact{{},s,{glyphs,1},{placed,8},{characters,8}};NativeAsciiResult published{};
        CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes_text},1000,compact,&published)==SfntStatus::output_too_small&&!published.glyph_count&&!published.source_units);
        compact.glyphs.count=8;bytes_text=utf8_encode(0x1f600,text);
        if(c.unknown_is_space){
            // Missing cmap IDs alias the absent LRM/RLM IDs in this font. The forced shaper therefore exports an
            // invisible space, matching Windows. App fallback must still be selected before this owned call.
            u16 space=0;CHECK(sfnt_cmap_glyph(*prepared.cmap,0x20,0,&space)==SfntStatus::ok);
            CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes_text},1000,compact,&published)==SfntStatus::ok&&published.glyph_count==1&&published.source_units==2);
            CHECK(compact.glyphs[0]==space&&!compact.placements[0].advance);
        }else CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes_text},1000,compact,&published)==SfntStatus::unsupported_sequence&&!published.glyph_count&&!published.source_units);
        bytes_text=utf8_encode(c.first,text);bytes_text+=utf8_encode(0x1f600,text+bytes_text);bytes_text+=utf8_encode(c.first,text+bytes_text);
        CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes_text},1000,s,&result)==SfntStatus::ok&&result.source_units==4&&result.glyph_count==3);
        const u32 supplementary[]{0,1,1,2};CHECK(bytes_equal(s.character_glyphs.data,supplementary,sizeof(supplementary)));
        const u8 malformed[][4]{{0xe0,0x80,0x80,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0x80,0,0,0},{0xe0,0xa0,0,0}};
        const u32 lengths[]{3,3,4,1,2};
        for(u32 i=0;i<5;++i)CHECK(native_cluster_run(script,owner.store,prepared,{reinterpret_cast<const char*>(malformed[i]),lengths[i]},1000,s,&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        auto short_storage=s;short_storage.components.count=0;
        CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes_text},1000,short_storage,&result)==SfntStatus::output_too_small&&!result.glyph_count);
        CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes_text},1000,s,&result,{1000000,100000,bytes_text-1})==SfntStatus::resource_limit&&!result.glyph_count);
        CHECK(native_cluster_run(script,owner.store,prepared,{},1000,compact,&published)==SfntStatus::ok&&!published.glyph_count&&!published.source_units&&!published.shaped_ranges);
    }
    CHECK(owner.close());SfntShapeRunResult result{};
    CHECK(native_cluster_run(script,owner.store,prepared,{},1000,SfntShapeStorage{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    wchar_t fallback_path[1024];const u32 directory=GetWindowsDirectoryW(fallback_path,1000);CHECK(directory&&directory<1000);
    u32 leaf=0;while(c.fallback[leaf])++leaf;CHECK(directory+leaf<1000);copy_bytes(fallback_path+directory,c.fallback,(leaf+1)*sizeof(wchar_t));
    ScriptShapeOwner fallback;CHECK(fallback.open(fallback_path));NativeClusterPlan nominal{};
    CHECK(native_cluster_plan_prepare(script,fallback.store,fallback.run,false,&state,&fallback.scratch,&nominal)==SfntStatus::ok&&!nominal.admitted);
    if(c.unknown_is_space){
        char nominal_text[8];u32 nominal_bytes=utf8_encode(c.first,nominal_text);nominal_bytes+=utf8_encode(c.second,nominal_text+nominal_bytes);
        CHECK(native_cluster_run(script,fallback.store,nominal,{nominal_text,nominal_bytes},1000,s,&result)==SfntStatus::ok&&result.glyph_count==2&&result.source_units==2);
        CHECK(!s.character_glyphs[0]&&!s.character_glyphs[1]);
    }else{
        char controls[8];u32 controls_bytes=utf8_encode(0x34f,controls);controls_bytes+=utf8_encode(0x34f,controls+controls_bytes);
        u16 space=0;CHECK(sfnt_cmap_glyph(*nominal.cmap,0x20,0,&space)==SfntStatus::ok&&space);
        CHECK(native_cluster_run(script,fallback.store,nominal,{controls,controls_bytes},1000,s,&result)==SfntStatus::ok&&result.glyph_count==2&&result.source_units==2);
        CHECK(s.glyphs[0].glyph.glyph==space&&s.glyphs[1].glyph.glyph==space&&!s.character_glyphs[0]&&!s.character_glyphs[1]&&!s.placements[0].advance&&!s.placements[1].advance);
        controls_bytes=utf8_encode(0x200d,controls);controls_bytes+=utf8_encode(0x200c,controls+controls_bytes);
        CHECK(native_cluster_run(script,fallback.store,nominal,{controls,controls_bytes},1000,s,&result)==SfntStatus::ok&&s.character_glyphs[1]==1);
        if(c.generic){
            char generic_text[24];u32 generic_bytes=0;
            for(u32 i=0;i<5;++i)generic_bytes+=utf8_encode(c.generic[i],generic_text+generic_bytes);
            CHECK(native_cluster_run(script,fallback.store,nominal,{generic_text,generic_bytes},1000,s,&result)==SfntStatus::ok&&result.glyph_count==5&&result.source_units==5);
            const u32 generic_groups[]{0,0,0,0,4};CHECK(bytes_equal(s.character_glyphs.data,generic_groups,sizeof(generic_groups)));
            for(u32 i=1;i<=2;++i){u16 expected=0;CHECK(sfnt_cmap_glyph(*nominal.cmap,c.generic[i],0,&expected)==SfntStatus::ok);
                CHECK(s.glyphs[i].glyph.glyph==expected&&s.placements[i].advance>0);}
        }
    }
    CHECK(fallback.close());
    print("PASS ");print(c.name);print(" owned units; assertions=");number(checks);print(" retained_root_bytes_both_modes=");number(retained);print(" retained_profile_bytes_both_modes=");number(total_retained);print(" profile_bytes=");number(sizeof(NativeClusterPlan));print("\n");return true;
}
