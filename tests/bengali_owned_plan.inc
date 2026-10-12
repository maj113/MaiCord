bool bengali_owned_plan(const wchar_t* path,u32 index,Arena& state,SfntShapeStorage probe,
    ConstSlice<SfntLookupStep> substitutions,ConstSlice<u16> positioning,const u8* expected_forms,SfntIndicProfile script=SfntIndicProfile::bengali) noexcept {
    OwnedProfile owned;
    CHECK(arena_initialize(&owned.scratch,16*1024*1024,65536));
    CHECK(native_font_acquire(&owned.fonts,path,index,&owned.face).ok());
    CHECK(native_font_instance_acquire(&owned.instance_store,owned.face,{},false,{},&owned.instance).ok());
    CHECK(native_run_plan_acquire(&owned.store,owned.instance,&owned.scratch,&owned.run).ok());
    arena_reset(&owned.scratch);auto* sentinel=arena_allocate<u32>(&owned.scratch);CHECK(sentinel);*sentinel=0x12345678;
    const auto state_mark=arena_mark(&state),scratch_mark=arena_mark(&owned.scratch);const u32 references=owned.plans[0].references;
    u8 output_forms[128];for(auto& bits:output_forms)bits=255;
    const auto unchanged=[&]() noexcept {for(auto bits:output_forms)if(bits!=255)return false;
        return state.offset==state_mark.offset&&owned.scratch.offset==scratch_mark.offset&&*sentinel==0x12345678&&
            owned.plans[0].references==references&&!owned.indic.run.slot&&!owned.indic.cmap&&
            !owned.indic.substitution_count&&!owned.indic.positioning_count;};
    const auto prepare=[&](SfntShapeStorage storage,Slice<u8> forms,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return native_indic_plan_prepare(owned.store,owned.run,script,&state,&owned.scratch,storage,&owned.indic,forms,limits);};
    CHECK(prepare(probe,{output_forms,128},{1000000,100000,0})==SfntStatus::resource_limit&&unchanged());
    CHECK(prepare(probe,{output_forms,128},{1,100000,1000000})==SfntStatus::resource_limit&&unchanged());
    CHECK(prepare(probe,{output_forms,1})==SfntStatus::invalid_input&&unchanged());
    CHECK(prepare(probe,{})==SfntStatus::unsupported_sequence&&unchanged());
    CHECK(prepare({},{output_forms,128})==SfntStatus::output_too_small&&unchanged());
    auto stale=owned.run;++stale.generation;
    CHECK(native_indic_plan_prepare(owned.store,stale,script,&state,&owned.scratch,probe,&owned.indic,{output_forms,128})==SfntStatus::invalid_input&&unchanged());
    CHECK(prepare(probe,{output_forms,128})==SfntStatus::ok);
    CHECK(owned.indic.profile==script&&owned.scratch.offset==scratch_mark.offset&&*sentinel==0x12345678&&owned.plans[0].references==references);
    CHECK(owned.indic.substitution_count==substitutions.count&&bytes_equal(owned.indic.substitution_steps,substitutions.data,substitutions.count*sizeof(SfntLookupStep)));
    CHECK(owned.indic.positioning_count==positioning.count&&bytes_equal(owned.indic.positioning_roots,positioning.data,positioning.count*sizeof(u16)));
    CHECK(bytes_equal(output_forms,expected_forms,128));
    const usize bytes=substitutions.count*sizeof(SfntLookupStep)+positioning.count*sizeof(u16);
    CHECK(profile_positioning_storage(owned.store,owned.run,owned.indic.substitutions(),owned.indic.positioning(),state,state_mark.offset,owned.indic.positioning_program));
    const usize retained=state.offset-state_mark.offset;
    const auto* parent=native_run_plan_get(owned.store,owned.run);CHECK(parent);
    if(owned.indic.positioning_program!=parent->positioning){
        // One byte short of the measured selected program's final allocation:
        // roots and form probes succeed, then GPOS storage must fail atomically.
        const usize end=state.offset;
        CHECK(reinterpret_cast<const u8*>(owned.indic.positioning_program)>state.base+state_mark.offset);
        CHECK(arena_rewind(&state,state_mark));owned.indic={};for(auto& bits:output_forms)bits=255;
        Arena limited=state;limited.reserved=end-1;
        CHECK(native_indic_plan_prepare(owned.store,owned.run,script,&limited,&owned.scratch,probe,&owned.indic,{output_forms,128})==SfntStatus::resource_limit);
        CHECK(limited.offset==state_mark.offset&&unchanged()&&!owned.indic.positioning_program);
        CHECK(prepare(probe,{output_forms,128})==SfntStatus::ok&&state.offset==end);
        CHECK(bytes_equal(output_forms,expected_forms,128)&&owned.scratch.offset==scratch_mark.offset&&*sentinel==0x12345678&&owned.plans[0].references==references);
    }
    print(script==SfntIndicProfile::malayalam?"PASS Malayalam owned plan face=":script==SfntIndicProfile::kannada?"PASS Kannada owned plan face=":script==SfntIndicProfile::telugu?"PASS Telugu owned plan face=":script==SfntIndicProfile::odia?"PASS Odia owned plan face=":script==SfntIndicProfile::gurmukhi?"PASS Gurmukhi owned plan face=":script==SfntIndicProfile::gujarati?"PASS Gujarati owned plan face=":"PASS Bengali owned plan face=");number(index);print(" root_bytes=");number(bytes);print(" retained_bytes=");number(retained);print(" descriptor_bytes=");number(sizeof(NativeIndicPlan));print("\n");
    const bool malayalam=script==SfntIndicProfile::malayalam,kannada=script==SfntIndicProfile::kannada,telugu=script==SfntIndicProfile::telugu,odia=script==SfntIndicProfile::odia,gurmukhi=script==SfntIndicProfile::gurmukhi,gujarati=script==SfntIndicProfile::gujarati;const u32 glyph_count=kannada?1u:telugu||gujarati||gurmukhi?2u:3u;
    NativeIndicFormPlan prepared{};const auto state_before=arena_mark(&state);
    const auto prepare_owned=[&](SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return malayalam?native_malayalam_plan_prepare(owned.store,owned.run,&state,&owned.scratch,probe,&prepared,limits):kannada?native_kannada_plan_prepare(owned.store,owned.run,&state,&owned.scratch,probe,&prepared,limits):telugu?native_telugu_plan_prepare(owned.store,owned.run,&state,&owned.scratch,probe,&prepared,limits):odia?native_odia_plan_prepare(owned.store,owned.run,&state,&owned.scratch,probe,&prepared,limits):gurmukhi?native_gurmukhi_plan_prepare(owned.store,owned.run,&state,&owned.scratch,probe,&prepared,limits):gujarati?native_gujarati_plan_prepare(owned.store,owned.run,&state,&owned.scratch,probe,&prepared,limits):native_bengali_plan_prepare(owned.store,owned.run,&state,&owned.scratch,probe,&prepared,limits);};
    const auto shape_owned=[&](const NativeIndicFormPlan& plan,Utf8View input,auto storage,auto* result) noexcept {
        return malayalam?native_malayalam_run(owned.store,plan,input,1000,storage,result):kannada?native_kannada_run(owned.store,plan,input,1000,storage,result):telugu?native_telugu_run(owned.store,plan,input,1000,storage,result):odia?native_odia_run(owned.store,plan,input,1000,storage,result):gurmukhi?native_gurmukhi_run(owned.store,plan,input,1000,storage,result):gujarati?native_gujarati_run(owned.store,plan,input,1000,storage,result):native_bengali_run(owned.store,plan,input,1000,storage,result);};
    CHECK(prepare_owned({1000000,100000,0})==SfntStatus::resource_limit);
    CHECK(!prepared.indic.run.slot&&state.offset==state_before.offset&&owned.plans[0].references==references);
    CHECK(prepare_owned()==SfntStatus::ok);
    CHECK(prepared.indic.profile==script&&bytes_equal(prepared.forms,expected_forms,128)&&owned.plans[0].references==references);
    char text[8]{};u32 length=utf8_encode(malayalam?0xd15u:kannada?0xc95u:telugu?0xc15u:odia?0xb15u:gurmukhi?0xa15u:gujarati?0xa95u:0x995u,text);length+=utf8_encode(malayalam?0xd4bu:kannada?0xcc3u:telugu?0xc43u:odia?0xb4bu:gurmukhi?0xa3fu:gujarati?0xabfu:0x9cbu,text+length);SfntShapeRunResult shaped{};
    CHECK(shape_owned(prepared,{text,length},probe,&shaped)==SfntStatus::ok&&shaped.glyph_count==glyph_count&&shaped.source_units==2);
    u16 expected_glyphs[3]{};SfntPlacement expected_positions[3]{};u32 expected_sources[2]{};
    for(u32 i=0;i<glyph_count;++i)expected_glyphs[i]=probe.glyphs[i].glyph.glyph;
    copy_bytes(expected_positions,probe.placements.data,glyph_count*sizeof(SfntPlacement));copy_bytes(expected_sources,probe.character_glyphs.data,sizeof(expected_sources));
    u16 compact_glyphs[8]{};SfntPlacement compact_positions[8]{};u32 compact_sources[8]{};NativeAsciiResult compact_result{};
    for(u32 aliases=0;aliases<8;++aliases){NativeAsciiStorage compact{{},probe,
        aliases&1?Slice<u16>{reinterpret_cast<u16*>(probe.glyphs.data),probe.glyphs.count}:Slice<u16>{compact_glyphs,8},
        aliases&2?probe.placements:Slice<SfntPlacement>{compact_positions,8},
        aliases&4?probe.character_glyphs:Slice<u32>{compact_sources,8}};
        CHECK(shape_owned(prepared,{text,length},compact,&compact_result)==SfntStatus::ok&&compact_result.glyph_count==glyph_count&&compact_result.source_units==2);
        CHECK(bytes_equal(compact.glyphs.data,expected_glyphs,glyph_count*sizeof(u16))&&bytes_equal(compact.placements.data,expected_positions,glyph_count*sizeof(SfntPlacement))&&bytes_equal(compact.character_glyphs.data,expected_sources,sizeof(expected_sources)));}
    // Paragraph export must retain the owned run's glyph/source geometry and
    // coalesce CRLF into one nondrawing cluster without splitting the syllable.
    NativeLineCluster line_clusters[8]{};NativeParagraphCluster paragraph_clusters[8]{};
    u16 paragraph_glyphs[8]{};SfntPlacement paragraph_positions[8]{};u32 paragraph_sources[8]{},boundary_scalars[8]{};u8 boundary_breaks[9]{};
    NativeAsciiParagraphStorage paragraph_storage{{{{},probe,{compact_glyphs,8},{compact_positions,8},{compact_sources,8}},{line_clusters,8}},
        {paragraph_glyphs,8},{paragraph_positions,8},{paragraph_sources,8},{paragraph_clusters,8}};
    text[length]='\r';text[length+1]='\n';NativeParagraphShape exported{};
    CHECK(indic_form_paragraph(script,owned.store,prepared,{text,length+2},1000,paragraph_storage,{{boundary_scalars,8},{boundary_breaks,9}},&exported,0,false)==SfntStatus::ok);
    CHECK(exported.source_units==4&&exported.glyph_count==glyph_count&&exported.cluster_count==2);
    CHECK(bytes_equal(paragraph_glyphs,expected_glyphs,glyph_count*sizeof(u16))&&bytes_equal(paragraph_positions,expected_positions,glyph_count*sizeof(SfntPlacement))&&bytes_equal(paragraph_sources,expected_sources,sizeof(expected_sources)));
    CHECK(paragraph_sources[2]==0xffffffffu&&paragraph_sources[3]==0xffffffffu&&paragraph_clusters[0].source_end==2&&paragraph_clusters[1].source_first==2&&paragraph_clusters[1].source_end==4&&paragraph_clusters[1].flags==native_paragraph_newline);
    NativeAsciiStorage short_output{{},probe,{compact_glyphs,glyph_count-1},{compact_positions,8},{compact_sources,8}};
    CHECK(shape_owned(prepared,{text,length},short_output,&compact_result)==SfntStatus::output_too_small&&!compact_result.glyph_count&&!compact_result.source_units);
    short_output.glyphs.count=8;const u32 missing_length=utf8_encode(0x10ffff,text);
    CHECK(shape_owned(prepared,{text,missing_length},short_output,&compact_result)==SfntStatus::unsupported_sequence&&!compact_result.glyph_count&&!compact_result.source_units);
    CHECK(shape_owned(prepared,{},short_output,&compact_result)==SfntStatus::ok&&!compact_result.glyph_count&&!compact_result.source_units);
    auto invalid=prepared;invalid.indic.profile=SfntIndicProfile::tamil;invalid.indic.cmap=reinterpret_cast<const SfntCmap*>(1);
    CHECK(shape_owned(invalid,{},SfntShapeStorage{},&shaped)==SfntStatus::invalid_input&&!shaped.glyph_count&&!shaped.source_units);
    const SfntIndicProfile admitted[]{SfntIndicProfile::bengali,SfntIndicProfile::gujarati,SfntIndicProfile::gurmukhi,SfntIndicProfile::odia,SfntIndicProfile::telugu,SfntIndicProfile::kannada,SfntIndicProfile::malayalam};
    for(auto profile:admitted)if(profile!=script){invalid.indic.profile=profile;
        CHECK(shape_owned(invalid,{},SfntShapeStorage{},&shaped)==SfntStatus::invalid_input&&!shaped.glyph_count&&!shaped.source_units);}
    for(auto profile:admitted)if(profile!=script){invalid.indic.profile=profile;NativeParagraphShape rejected{};
        CHECK(indic_form_paragraph(script,owned.store,invalid,{},1000,{},{},&rejected,0,false)==SfntStatus::invalid_input&&!rejected.glyph_count&&!rejected.source_units);}
    const SfntIndicProfile unavailable[]{SfntIndicProfile::devanagari,SfntIndicProfile::tamil,static_cast<SfntIndicProfile>(255)};
    NativeParagraphShape paragraph{};
    for(auto profile:unavailable){invalid.indic.profile=profile;shaped.glyph_count=shaped.source_units=17;
        CHECK(indic_owned_run(profile,owned.store,invalid,{},1000,SfntShapeStorage{},&shaped,{},false,0)==SfntStatus::invalid_input&&!shaped.glyph_count&&!shaped.source_units);
        compact_result.glyph_count=compact_result.source_units=17;
        CHECK(indic_owned_run(profile,owned.store,invalid,{},1000,NativeAsciiStorage{},&compact_result,{},false,0)==SfntStatus::invalid_input&&!compact_result.glyph_count&&!compact_result.source_units);
        paragraph.glyph_count=paragraph.source_units=paragraph.cluster_count=17;
        CHECK(indic_form_paragraph(profile,owned.store,invalid,{},1000,{},{},&paragraph,0,false)==SfntStatus::invalid_input&&!paragraph.glyph_count&&!paragraph.source_units&&!paragraph.cluster_count);}
    u8 empty_break[1]{};
    CHECK(indic_form_paragraph(script,owned.store,prepared,{},1000,{},{{},{empty_break,1}},&paragraph,0,false)==SfntStatus::ok&&!paragraph.glyph_count&&!paragraph.source_units&&!paragraph.cluster_count);
    CHECK(indic_form_paragraph(script,owned.store,prepared,{},1000,{},{},&paragraph,0,false)==SfntStatus::output_too_small&&!paragraph.glyph_count&&!paragraph.source_units);
    CHECK(owned.close());prepared.indic.cmap=reinterpret_cast<const SfntCmap*>(1);
    CHECK(indic_form_paragraph(script,owned.store,prepared,{},1000,{},{},&paragraph,0,false)==SfntStatus::invalid_input&&!paragraph.glyph_count&&!paragraph.source_units);
    CHECK(shape_owned(prepared,{},SfntShapeStorage{},&shaped)==SfntStatus::invalid_input&&!shaped.glyph_count&&!shaped.source_units);
    return true;
}
