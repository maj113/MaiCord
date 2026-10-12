#include "profile_positioning_storage.inc"
#include "script_shape_owner.inc"
bool owned_units(ScriptShapeOwner& owner,Arena& state,SfntShapeStorage s) noexcept {
    auto& scratch=owner.scratch;arena_reset(&scratch);auto* sentinel=arena_allocate<u32>(&scratch);CHECK(sentinel);*sentinel=0x12345678;
    const auto before_state=arena_mark(&state),before_scratch=arena_mark(&scratch);const u32 refs=owner.plans[0].references;NativeKhmerPlan prepared{};
    const auto unchanged=[&]() noexcept {return state.offset==before_state.offset&&scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&
        owner.plans[0].references==refs&&!prepared.run.slot&&!prepared.cmap&&!prepared.substitution_count&&!prepared.positioning_count;};
    CHECK(native_khmer_plan_prepare(owner.store,owner.run,false,&state,&scratch,&prepared,0)==SfntStatus::resource_limit&&unchanged());
    CHECK(native_khmer_plan_prepare(owner.store,owner.run,false,&state,&state,&prepared)==SfntStatus::invalid_input&&unchanged());
    auto stale=owner.run;++stale.generation;
    CHECK(native_khmer_plan_prepare(owner.store,stale,false,&state,&scratch,&prepared)==SfntStatus::invalid_input&&unchanged());
    Arena tiny{};CHECK(arena_initialize(&tiny,4096,4096,4096));struct Release {Arena& a;~Release() noexcept {arena_release(&a);}} release{tiny};
    CHECK(arena_allocate(&tiny,4095,1));const auto mark=arena_mark(&tiny);
    CHECK(native_khmer_plan_prepare(owner.store,owner.run,false,&tiny,&scratch,&prepared)==SfntStatus::resource_limit&&unchanged()&&tiny.offset==mark.offset);
    CHECK(native_khmer_plan_prepare(owner.store,owner.run,false,&state,&tiny,&prepared)==SfntStatus::output_too_small&&unchanged()&&tiny.offset==mark.offset);
    usize retained=0,total_retained=0;
    for(u32 mode=0;mode<2;++mode){const usize before=state.offset;
        CHECK(native_khmer_plan_prepare(owner.store,owner.run,mode!=0,&state,&scratch,&prepared)==SfntStatus::ok);
        CHECK(prepared.independent_letters==(mode!=0)&&scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&owner.plans[0].references==refs);
        const usize bytes=prepared.substitution_count*sizeof(SfntLookupStep)+prepared.positioning_count*sizeof(u16);retained+=bytes;
        CHECK(profile_positioning_storage(owner.store,owner.run,prepared.substitutions(),prepared.positioning(),state,before,prepared.positioning_program,mode!=0));
        total_retained+=state.offset-before;
        auto invalid=prepared;++invalid.run.generation;invalid.cmap=reinterpret_cast<const SfntCmap*>(1);SfntShapeRunResult result{};
        CHECK(native_khmer_run(owner.store,invalid,{},1000,SfntShapeStorage{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        char text[16];u32 bytes_text=utf8_encode(0x1780,text);bytes_text+=utf8_encode(0x17be,text+bytes_text);
        CHECK(native_khmer_run(owner.store,prepared,{text,bytes_text},1000,s,&result)==SfntStatus::ok&&result.source_units==2&&result.glyph_count==3);
        u16 expected[3];SfntPlacement placements[3];u32 sources[2];for(u32 i=0;i<3;++i)expected[i]=s.glyphs[i].glyph.glyph;
        copy_bytes(placements,s.placements.data,sizeof(placements));copy_bytes(sources,s.character_glyphs.data,sizeof(sources));
        u16 glyphs[8];SfntPlacement placed[8];u32 characters[8];
        for(u32 alias=0;alias<8;++alias){NativeAsciiStorage compact{{},s,
            alias&1?Slice<u16>{reinterpret_cast<u16*>(s.glyphs.data),s.glyphs.count}:Slice<u16>{glyphs,8},
            alias&2?s.placements:Slice<SfntPlacement>{placed,8},alias&4?s.character_glyphs:Slice<u32>{characters,8}};NativeAsciiResult published{};
            CHECK(native_khmer_run(owner.store,prepared,{text,bytes_text},1000,compact,&published)==SfntStatus::ok&&published.glyph_count==3&&published.source_units==2&&published.shaped_ranges==1);
            CHECK(bytes_equal(compact.glyphs.data,expected,sizeof(expected))&&bytes_equal(compact.placements.data,placements,sizeof(placements))&&bytes_equal(compact.character_glyphs.data,sources,sizeof(sources)));}
        NativeAsciiStorage compact{{},s,{glyphs,1},{placed,8},{characters,8}};NativeAsciiResult published{};
        CHECK(native_khmer_run(owner.store,prepared,{text,bytes_text},1000,compact,&published)==SfntStatus::output_too_small&&!published.glyph_count&&!published.source_units);
        compact.glyphs.count=8;bytes_text=utf8_encode(0x1f600,text);
        CHECK(native_khmer_run(owner.store,prepared,{text,bytes_text},1000,compact,&published)==SfntStatus::unsupported_sequence&&!published.glyph_count&&!published.source_units);
        bytes_text=utf8_encode(0x1780,text);bytes_text+=utf8_encode(0x1f600,text+bytes_text);bytes_text+=utf8_encode(0x1780,text+bytes_text);
        CHECK(native_khmer_run(owner.store,prepared,{text,bytes_text},1000,s,&result)==SfntStatus::ok&&result.source_units==4&&result.glyph_count==3);
        const u32 supplementary[]{0,1,1,2};CHECK(bytes_equal(s.character_glyphs.data,supplementary,sizeof(supplementary)));
        const u8 malformed[][4]{{0xe0,0x80,0x80,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0x80,0,0,0},{0xe0,0xa0,0,0}};
        const u32 lengths[]{3,3,4,1,2};
        for(u32 i=0;i<5;++i)CHECK(native_khmer_run(owner.store,prepared,{reinterpret_cast<const char*>(malformed[i]),lengths[i]},1000,s,&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        auto short_storage=s;short_storage.components.count=0;
        CHECK(native_khmer_run(owner.store,prepared,{text,bytes_text},1000,short_storage,&result)==SfntStatus::output_too_small&&!result.glyph_count);
        CHECK(native_khmer_run(owner.store,prepared,{text,bytes_text},1000,s,&result,{1000000,100000,bytes_text-1})==SfntStatus::resource_limit&&!result.glyph_count);
        CHECK(native_khmer_run(owner.store,prepared,{},1000,compact,&published)==SfntStatus::ok&&!published.glyph_count&&!published.source_units&&!published.shaped_ranges);
    }
    CHECK(owner.close());SfntShapeRunResult result{};
    CHECK(native_khmer_run(owner.store,prepared,{},1000,SfntShapeStorage{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    wchar_t fallback_path[1024];const u32 directory=GetWindowsDirectoryW(fallback_path,1000);CHECK(directory&&directory<1000);
    const wchar_t suffix[]=L"\\Fonts\\consola.ttf";copy_bytes(fallback_path+directory,suffix,sizeof(suffix));
    ScriptShapeOwner fallback;CHECK(fallback.open(fallback_path));NativeKhmerPlan nominal{};
    CHECK(native_khmer_plan_prepare(fallback.store,fallback.run,false,&state,&fallback.scratch,&nominal)==SfntStatus::ok&&!nominal.admitted);
    char controls[8];u32 controls_bytes=utf8_encode(0x34f,controls);controls_bytes+=utf8_encode(0x34f,controls+controls_bytes);
    u16 space=0;CHECK(sfnt_cmap_glyph(*nominal.cmap,0x20,0,&space)==SfntStatus::ok&&space);
    CHECK(native_khmer_run(fallback.store,nominal,{controls,controls_bytes},1000,s,&result)==SfntStatus::ok&&result.glyph_count==2&&result.source_units==2);
    CHECK(s.glyphs[0].glyph.glyph==space&&s.glyphs[1].glyph.glyph==space&&!s.character_glyphs[0]&&!s.character_glyphs[1]&&!s.placements[0].advance&&!s.placements[1].advance);
    controls_bytes=utf8_encode(0x200d,controls);controls_bytes+=utf8_encode(0x200c,controls+controls_bytes);
    CHECK(native_khmer_run(fallback.store,nominal,{controls,controls_bytes},1000,s,&result)==SfntStatus::ok&&s.character_glyphs[1]==1);
    char generic_text[24];u32 generic_bytes=0;const u32 generic_scalars[]{0x1780,0x17b4,0x17b5,0x17d2,0x179a};
    for(u32 scalar:generic_scalars)generic_bytes+=utf8_encode(scalar,generic_text+generic_bytes);
    CHECK(native_khmer_run(fallback.store,nominal,{generic_text,generic_bytes},1000,s,&result)==SfntStatus::ok&&result.glyph_count==5&&result.source_units==5);
    const u32 generic_groups[]{0,0,0,0,4};CHECK(bytes_equal(s.character_glyphs.data,generic_groups,sizeof(generic_groups)));
    for(u32 i=1;i<=2;++i){u16 expected=0;CHECK(sfnt_cmap_glyph(*nominal.cmap,generic_scalars[i],0,&expected)==SfntStatus::ok);
        CHECK(s.glyphs[i].glyph.glyph==expected&&s.placements[i].advance>0);}
    CHECK(fallback.close());
    print("PASS Khmer owned units; assertions=");number(checks);print(" retained_root_bytes_both_modes=");number(retained);print(" retained_profile_bytes_both_modes=");number(total_retained);print(" profile_bytes=");number(sizeof(NativeKhmerPlan));print("\n");return true;
}
bool owned_stream(const wchar_t* path,Arena& a,SfntShapeStorage s,bool units_only) noexcept {
    ScriptShapeOwner owner;CHECK(owner.open(path));if(units_only)return owned_units(owner,a,s);
    NativeKhmerPlan prepared{};CHECK(native_khmer_plan_prepare(owner.store,owner.run,flag(L"MAICORD_KHMER_INDEPENDENT"),&a,&owner.scratch,&prepared)==SfntStatus::ok);
    const bool rtl=flag(L"MAICORD_KHMER_RTL"),compact=flag(L"MAICORD_KHMER_COMPACT");
    u8 header[12];CHECK(read(header,12)&&bytes_equal(header,"MDSHP1\0\0",8));const u32 cases=byte_load_le32(header+8);CHECK(cases&&cases<=100000&&write(header,12));
    for(u32 row=0;row<cases;++row){u32 n=0,input[256],sources[256];u16 glyphs[512];char text[1024];SfntShapeRunResult result{};
        CHECK(read(&n,4)&&n&&n<=256&&read(input,n*4));u32 bytes=0;
        for(u32 i=0;i<n;++i){const u8 width=utf8_encode(input[i],text+bytes);CHECK(width);bytes+=width;}
        SfntStatus status;
        if(compact){NativeAsciiStorage target{{},s,{reinterpret_cast<u16*>(s.glyphs.data),s.glyphs.count},s.placements,s.character_glyphs};NativeAsciiResult output{};
            status=native_khmer_run(owner.store,prepared,{text,bytes},1000,target,&output,{},rtl);
            result.glyph_count=output.glyph_count;result.source_units=output.source_units;copy_bytes(glyphs,target.glyphs.data,result.glyph_count*2);
        }else{status=native_khmer_run(owner.store,prepared,{text,bytes},1000,s,&result,{},rtl);
            for(u32 i=0;i<result.glyph_count;++i)glyphs[i]=s.glyphs[i].glyph.glyph;}
        CHECK(result.glyph_count<=512);const u32 code=static_cast<u32>(status);u32 units=0;
        for(u32 i=0;i<n;++i){sources[i]=status==SfntStatus::ok?s.character_glyphs[units]:0;units+=input[i]>0xffff?2u:1u;}
        CHECK(write(&code,4)&&write(&result.glyph_count,4)&&write(glyphs,result.glyph_count*2)&&write(sources,n*4)&&write(s.placements.data,result.glyph_count*sizeof(SfntPlacement)));
    }return owner.close();
}
