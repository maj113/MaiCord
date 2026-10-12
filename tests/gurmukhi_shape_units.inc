bool gurmukhi_shape_units() noexcept {
    const u32 entries[][2]{{0x20,8},{0xa01,6},{0xa15,1},{0xa2f,2},{0xa30,4},{0xa3c,13},{0xa3e,5},{0xa3f,9},{0xa4d,3},{0xa51,10},{0xa70,11},{0xa75,12},{0x25cc,7}};
    u8 cmap_bytes[172]{};byte_store_be32(cmap_bytes,0x000c0000);byte_store_be32(cmap_bytes+4,sizeof(cmap_bytes));byte_store_be32(cmap_bytes+12,13);
    for(u32 i=0;i<13;++i){byte_store_be32(cmap_bytes+16+i*12,entries[i][0]);byte_store_be32(cmap_bytes+20+i*12,entries[i][0]);byte_store_be32(cmap_bytes+24+i*12,entries[i][1]);}
    const SfntCmap cmap{{cmap_bytes,sizeof(cmap_bytes)},{},32,12};
    const u16 below[]{1,8,1,14,1,1,3,1,4,20,2,4},half[]{1,8,1,14,1,1,1,1,4,21,2,3},post[]{1,8,1,14,1,1,3,1,4,22,2,2},tail[]{2,8,1,23,1,1,11};
    u8 bytes[4][32]{};const ConstSlice<u8> tables[]{encode(below,bytes[0]),encode(half,bytes[1]),encode(post,bytes[2]),encode(tail,bytes[3])};
    SfntGsubExecutableSubtable subtables[4]{};SfntGsubExecutableLookup lookups[4]{};
    for(u32 i=0;i<4;++i){const u16 type=i==3?1u:4u;CHECK(sfnt_gsub_subtable_open(tables[i],type,32,&subtables[i].basic)==SfntStatus::ok);lookups[i].type=type;lookups[i].subtables={subtables+i,1};}
    SfntGsubProgram program{{lookups,4},{},32};SfntShapePlan plan{};plan.substitutions=&program;plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* output) noexcept {*output=1000;return SfntStatus::ok;};
    const u16 classes[]{2,3,3,3,3,5,6,3,9,13,3};u8 class_bytes[22]{};
    CHECK(sfnt_class_def_open(encode(classes,class_bytes),32,&plan.gdef.glyph_classes)==SfntStatus::ok);program.gdef=plan.gdef;
    const SfntLookupStep steps[]{{0,sfnt_deva_below,6},{1,sfnt_deva_half,7},{2,sfnt_deva_post,8},{3,sfnt_deva_post,8}};
    SfntComponentGlyph glyphs[32]{},replacement[32]{};SfntComponentNode nodes[64]{};u64 bits[2]{};
    u32 characters[32]{},targets[32]{},positions[128]{},rebased[128]{},consumed[32]{};
    SfntMarkAssociation associations[32]{};double advances[32]{};u8 flags[32]{};SfntPosition states[32]{};SfntPlacement placements[32]{};
    SfntGsubExecutionFrame frames[16]{};SfntComponentUndo journal[512]{};
    SfntShapeStorage storage{{glyphs,32},{nodes,64},{bits,2},{characters,32},{targets,32},{associations,32},{advances,32},{flags,32},{states,32},{placements,32},
        {{frames,16},{positions,128},{rebased,128},{consumed,32},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,512}},{}};
    u8 forms[128]{};forms[0x15]=sfnt_deva_half;forms[0x30]=sfnt_deva_below;forms[0x2f]=sfnt_deva_post;SfntShapeRunResult result{};
    const auto shape=[&](ConstSlice<u32> input,float spacing=0,bool rtl=false,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return sfnt_gurmukhi_shape_cluster(plan,cmap,input,{forms,128},1000,storage,&result,{steps,4},limits,false,spacing,rtl);};
    const auto same=[&](ConstSlice<u16> expected) noexcept {
        if(result.glyph_count!=expected.count)return false;
        for(u32 i=0;i<expected.count;++i)if(glyphs[i].glyph.glyph!=expected[i])return false;
        for(u32 i=0;i<result.source_units;++i)if(characters[i])return false;
        return true;};
    const u32 conjunct[]{0xa15,0xa4d,0xa30,0xa3f},half_nukta[]{0xa15,0xa4d,0xa3c,0xa15,0xa3f};
    const u16 conjunct_glyphs[]{9,1,20},half_nukta_glyphs[]{9,1,13,3,1},full_nukta_glyphs[]{1,13,3,9,1};
    CHECK(shape({conjunct,4})==SfntStatus::ok&&same({conjunct_glyphs,3}));
    CHECK(shape({half_nukta,5})==SfntStatus::ok&&same({half_nukta_glyphs,5}));
    forms[0x15]=0;CHECK(shape({half_nukta,5})==SfntStatus::ok&&same({full_nukta_glyphs,5}));forms[0x15]=sfnt_deva_half;
    const u32 tail_text[]{0xa15,0xa4d,0xa2f,0xa70};const u16 tail_glyphs[]{1,22,11};
    CHECK(shape({tail_text,4})==SfntStatus::ok&&same({tail_glyphs,3})); // pstf must not change the tippi.
    const u32 medial_tail[]{0xa15,0xa51,0xa4d,0xa3c};const u16 medial_glyphs[]{1,10,3,13};
    CHECK(shape({medial_tail,4})==SfntStatus::ok&&same({medial_glyphs,4}));
    const u32 orphan_medial[]{0xa51},orphan_vowel[]{0xa3f};const u16 medial_single[]{10},vowel_single[]{9,7};
    CHECK(shape({orphan_medial,1})==SfntStatus::ok&&same({medial_single,1}));
    storage.character_glyphs.count=1;
    CHECK(shape({orphan_vowel,1})==SfntStatus::ok&&same({vowel_single,2}));storage.character_glyphs.count=32;
    const u32 controls[]{0xa15,0xa01,0x200c},joined[]{0xa15,0xa4d,0x200d};const u16 control_glyphs[]{1,6,8},joined_glyphs[]{21,8};
    const bool directions[]{false,true};for(bool rtl:directions){
        CHECK(shape({controls,3},.25f,rtl)==SfntStatus::ok&&same({control_glyphs,3}));
        CHECK(placements[0].advance==(rtl?1000.25f:1000)&&placements[1].advance==(rtl?1000:1000.25f)&&placements[2].advance==0);
        CHECK(shape({joined,3},.25f,rtl)==SfntStatus::ok&&same({joined_glyphs,2})&&placements[0].advance==1000.25f&&placements[1].advance==0);}
    CHECK(shape({half_nukta,5})==SfntStatus::ok);const u64 total=result.lookup_operations;
    for(u64 work=0;work<=total;++work){const auto status=shape({half_nukta,5},0,false,{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==total?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    const u32 multiple[]{0xa15,0xa15},invalid[]{0xa15,0xd800};
    CHECK(shape({multiple,2})==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({controls,3},-1)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({half_nukta,5},0,false,{1000000,100000,0})==SfntStatus::resource_limit&&!result.glyph_count);
    storage.glyphs.count=1;CHECK(shape({controls,3})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);storage.glyphs.count=32;
    CHECK(sfnt_gurmukhi_shape_cluster(plan,cmap,{controls,3},{forms,127},1000,storage,&result,{steps,4})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(shape({})==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    const u16 pair_words[]{1,18,4,0,1,12,1,1,50,1,1,1};u8 pair_bytes[24]{};SfntGposSubtable pair{};
    CHECK(sfnt_gpos_subtable_open(encode(pair_words,pair_bytes),2,32,&pair)==SfntStatus::ok);
    SfntGposExecutableLookup pair_lookup{};pair_lookup.type=2;pair_lookup.tables.basic={&pair,1};
    SfntGposProgram positioning{{&pair_lookup,1},{},32};const u16 root=0;plan.positioning=&positioning;plan.positioning_roots={&root,1};
    SfntGposExecutionFrame position_frames[4]{};u32 position_scratch[32]{},run_sources[96]{};
    storage.positioning.frames={position_frames,4};storage.positioning.positions={position_scratch,32};storage.character_glyphs={run_sources,96};
    const u32 run_text[]{0xa15,0xa15,0xa15};
    CHECK(sfnt_gurmukhi_shape_run(plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,4},{},false,.25f)==SfntStatus::ok);
    CHECK(result.glyph_count==3&&result.source_units==3&&result.position_matches==2);
    CHECK(placements[0].advance==1050.25f&&placements[1].advance==1050.25f&&placements[2].advance==1000.25f);
    for(u32 i=0;i<3;++i)CHECK(run_sources[i]==i);
    const u32 run_controls[]{0x200d,0xa15,0xa4d,0x200d,0xa15};
    for(bool rtl:directions){CHECK(sfnt_gurmukhi_shape_run(plan,cmap,{run_controls,5},{forms,128},1000,storage,&result,{steps,4},{},rtl,.25f)==SfntStatus::ok);
        CHECK(result.glyph_count==4&&placements[0].advance==0&&placements[1].advance==(rtl?1000.25f:1000)&&placements[2].advance==0&&placements[3].advance==(rtl?1000:1000.25f)&&run_sources[1]==1&&run_sources[4]==1);}
    const auto utf8_case=[&](ConstSlice<u32> input) noexcept {
        CHECK(sfnt_gurmukhi_shape_run(plan,cmap,input,{forms,128},1000,storage,&result,{steps,4})==SfntStatus::ok);
        const auto saved=result;u16 saved_glyphs[32]{};u32 saved_sources[96]{};SfntPlacement saved_placements[32]{};
        CHECK(saved.glyph_count<=32&&saved.source_units<=96);
        for(u32 i=0;i<saved.glyph_count;++i)saved_glyphs[i]=glyphs[i].glyph.glyph;
        copy_bytes(saved_sources,run_sources,saved.source_units*4);copy_bytes(saved_placements,placements,saved.glyph_count*sizeof(SfntPlacement));
        char text[512]{};u32 bytes_written=0;for(u32 i=0;i<input.count;++i){CHECK(bytes_written+4<=512);bytes_written+=utf8_encode(input[i],text+bytes_written);}
        CHECK(native_gurmukhi_run(plan,cmap,{text,bytes_written},{forms,128},1000,storage,&result,{steps,4})==SfntStatus::ok);
        CHECK(result.glyph_count==saved.glyph_count&&result.source_units==saved.source_units&&result.lookup_operations==saved.lookup_operations);
        for(u32 i=0;i<saved.glyph_count;++i)CHECK(glyphs[i].glyph.glyph==saved_glyphs[i]);
        CHECK(bytes_equal(saved_sources,run_sources,saved.source_units*4)&&bytes_equal(saved_placements,placements,saved.glyph_count*sizeof(SfntPlacement)));return true;
    };
    u32 windowed[40]{};for(u32 i=0;i<40;++i)windowed[i]=i%2?0xa4du:0xa15u;
    const u32 supplementary[]{0xa15,0x1f600,0xa3f};
    CHECK(utf8_case({run_text,3})&&utf8_case({run_controls,5})&&utf8_case({windowed,40})&&utf8_case({supplementary,3})&&utf8_case({}));
    const u8 malformed[][4]{{0xc0,0xaf,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0xe0,0xaa,0,0}};const u32 bad_sizes[]{2,3,4,2};
    for(u32 i=0;i<4;++i)CHECK(native_gurmukhi_run(plan,cmap,{reinterpret_cast<const char*>(malformed[i]),bad_sizes[i]},{forms,128},1000,storage,&result,{steps,4})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    print("PASS Gurmukhi form eligibility, medial/nukta ordering, mark advances, run GPOS, UTF-8, controls, tracking and bounded failures\n");return true;
}
