bool bengali_shape_units() noexcept {
    const u32 entries[][2]{{0x20,8},{0x994,12},{0x995,1},{0x9b0,2},{0x9bc,13},{0x9be,5},{0x9bf,9},{0x9c7,4},{0x9cd,3},{0x9d7,6},{0x25cc,7}};
    u8 cmap_bytes[148]{};byte_store_be32(cmap_bytes,0x000c0000);byte_store_be32(cmap_bytes+4,sizeof(cmap_bytes));byte_store_be32(cmap_bytes+12,11);
    for(u32 i=0;i<11;++i){byte_store_be32(cmap_bytes+16+i*12,entries[i][0]);byte_store_be32(cmap_bytes+20+i*12,entries[i][0]);byte_store_be32(cmap_bytes+24+i*12,entries[i][1]);}
    const SfntCmap cmap{{cmap_bytes,sizeof(cmap_bytes)},{},32,12};
    const u16 reph[]{1,8,1,14,1,1,2,1,4,20,2,3},half[]{1,8,1,14,1,1,1,1,4,21,2,3},initial[]{1,6,18,1,1,4};
    u8 bytes[3][32]{};const ConstSlice<u8> tables[]{encode(reph,bytes[0]),encode(half,bytes[1]),encode(initial,bytes[2])};
    const u16 types[]{4,4,1};SfntGsubExecutableSubtable subtables[3]{};SfntGsubExecutableLookup lookups[3]{};
    for(u32 i=0;i<3;++i){CHECK(sfnt_gsub_subtable_open(tables[i],types[i],32,&subtables[i].basic)==SfntStatus::ok);lookups[i].type=types[i];lookups[i].subtables={subtables+i,1};}
    SfntGsubProgram program{{lookups,3},{},32};SfntShapePlan plan{};plan.substitutions=&program;plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* output) noexcept {*output=1000;return SfntStatus::ok;};
    const SfntLookupStep steps[]{{0,sfnt_deva_reph,3},{1,sfnt_deva_half,7},{2,sfnt_bengali_initial,12}};
    SfntComponentGlyph glyphs[32]{},replacement[32]{};SfntComponentNode nodes[64]{};u64 bits[2]{};
    u32 characters[32]{},targets[32]{},positions[128]{},rebased[128]{},consumed[32]{};
    SfntMarkAssociation associations[32]{};double advances[32]{};u8 flags[32]{};SfntPosition states[32]{};SfntPlacement placements[32]{};
    SfntGsubExecutionFrame frames[16]{};SfntComponentUndo journal[512]{};
    SfntShapeStorage storage{{glyphs,32},{nodes,64},{bits,2},{characters,32},{targets,32},{associations,32},{advances,32},{flags,32},{states,32},{placements,32},
        {{frames,16},{positions,128},{rebased,128},{consumed,32},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,512}},{}};
    u8 forms[128]{};forms[0x30]=sfnt_deva_reph;forms[0x15]=sfnt_deva_half;SfntShapeRunResult result{};
    const auto shape=[&](ConstSlice<u32> input,bool word_initial=true,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return sfnt_bengali_shape_cluster(plan,cmap,input,{forms,128},1000,storage,&result,{steps,3},limits,word_initial);};
    const u32 split[]{0x995,0x9cb},with_reph[]{0x9b0,0x9cd,0x995,0x9cb},with_half[]{0x995,0x9cd,0x995,0x9cb},orphan[]{0x9cb},nukta[]{0x995,0x9bc,0x9cb};
    CHECK(shape({split,2})==SfntStatus::ok&&result.glyph_count==3&&result.source_units==2);
    CHECK(glyphs[0].glyph.glyph==22&&glyphs[1].glyph.glyph==1&&glyphs[2].glyph.glyph==5&&!characters[0]&&!characters[1]);
    CHECK(shape({split,2},false)==SfntStatus::ok&&glyphs[0].glyph.glyph==4);
    CHECK(shape({nukta,3},false)==SfntStatus::ok&&result.glyph_count==4&&glyphs[0].glyph.glyph==4);
    CHECK(shape({nukta,3})==SfntStatus::ok&&glyphs[0].glyph.glyph==22);
    CHECK(shape({with_reph,4})==SfntStatus::ok&&result.glyph_count==4&&result.source_units==4);
    const u16 reph_expected[]{22,1,20,5};for(u32 i=0;i<4;++i)CHECK(glyphs[i].glyph.glyph==reph_expected[i]&&!characters[i]);
    CHECK(shape({with_half,4})==SfntStatus::ok&&result.glyph_count==4);
    const u16 half_expected[]{22,21,1,5};for(u32 i=0;i<4;++i)CHECK(glyphs[i].glyph.glyph==half_expected[i]&&!characters[i]);
    const u64 total=result.lookup_operations;
    for(u64 work=0;work<=total;++work){const auto status=shape({with_half,4},true,{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==total?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    CHECK(shape({orphan,1})==SfntStatus::ok&&result.glyph_count==3&&glyphs[0].glyph.glyph==22&&glyphs[1].glyph.glyph==7&&glyphs[2].glyph.glyph==5&&!characters[0]);
    storage.character_glyphs.count=1;
    CHECK(shape({orphan,1})==SfntStatus::ok&&result.glyph_count==3&&result.source_units==1);
    storage.character_glyphs.count=2;
    CHECK(shape({split,2})==SfntStatus::ok&&result.glyph_count==3&&result.source_units==2);
    u32 longest[31]{};longest[0]=0x995;for(u32 i=1;i<30;++i)longest[i]=0x9be;longest[30]=0x9cb;
    storage.character_glyphs.count=31;
    CHECK(shape({longest,31})==SfntStatus::ok&&result.glyph_count==32&&result.source_units==31);
    CHECK(glyphs[0].glyph.glyph==22&&glyphs[1].glyph.glyph==1);
    for(u32 i=0;i<31;++i)CHECK(!characters[i]);
    storage.character_glyphs.count=32;
    const u32 joined[]{0x995,0x9cd,0x200d};
    CHECK(shape({joined,3})==SfntStatus::ok&&result.glyph_count==2&&glyphs[0].glyph.glyph==21&&glyphs[1].glyph.glyph==8&&placements[1].advance==0);
    const u32 invalid[]{0x995,0xd800},two_clusters[]{0x995,0x995},independent[]{0x994};
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({two_clusters,2})==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    CHECK(shape({independent,1})==SfntStatus::ok&&result.glyph_count==1&&glyphs[0].glyph.glyph==12);
    CHECK(shape({split,2},true,{1000000,100000,0})==SfntStatus::resource_limit&&!result.glyph_count);
    auto small=storage;small.glyphs.count=1;
    CHECK(sfnt_bengali_shape_cluster(plan,cmap,{split,2},{forms,128},1000,small,&result,{steps,3})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
    CHECK(sfnt_bengali_shape_cluster(plan,cmap,{split,2},{forms,127},1000,storage,&result,{steps,3})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(shape({})==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    const u32 run_text[]{0x995,0x9cb,0x995,0x9cb};
    const u16 pair_words[]{1,18,4,0,1,12,1,4,50,1,1,5};u8 pair_bytes[24]{};SfntGposSubtable pair{};
    CHECK(sfnt_gpos_subtable_open(encode(pair_words,pair_bytes),2,32,&pair)==SfntStatus::ok);
    SfntGposExecutableLookup pair_lookup{};pair_lookup.type=2;pair_lookup.tables.basic={&pair,1};
    SfntGposProgram positioning{{&pair_lookup,1},{},32};const u16 position_root=0;plan.positioning=&positioning;plan.positioning_roots={&position_root,1};
    SfntGposExecutionFrame position_frames[4]{};u32 position_scratch[32]{};
    storage.positioning.frames={position_frames,4};storage.positioning.positions={position_scratch,32};
    CHECK(sfnt_bengali_shape_run(plan,cmap,{run_text,4},{forms,128},1000,storage,&result,{steps,3})==SfntStatus::ok&&result.glyph_count==6&&result.source_units==4);
    const u16 run_expected[]{22,1,5,4,1,5};for(u32 i=0;i<6;++i)CHECK(glyphs[i].glyph.glyph==run_expected[i]);
    CHECK(characters[0]==0&&characters[1]==0&&characters[2]==3&&characters[3]==3);
    CHECK(placements[2].advance==1050&&placements[5].advance==1000&&result.position_matches==1);
    CHECK(sfnt_bengali_shape_run(plan,cmap,{run_text,4},{forms,128},1000,storage,&result,{steps,3},{},false,.25f)==SfntStatus::ok);
    CHECK(placements[2].advance==1050.25f&&placements[5].advance==1000.25f&&result.position_matches==1);
    const u32 leading_control[]{0x200d,0x995};
    const bool directions[]{false,true};for(bool direction:directions){
        CHECK(sfnt_bengali_shape_run(plan,cmap,{joined,3},{forms,128},1000,storage,&result,{steps,3},{},direction,.25f)==SfntStatus::ok);
        CHECK(result.glyph_count==2&&placements[0].advance==1000.25f&&placements[1].advance==0);
        CHECK(sfnt_bengali_shape_run(plan,cmap,{leading_control,2},{forms,128},1000,storage,&result,{steps,3},{},direction,.25f)==SfntStatus::ok);
        CHECK(result.glyph_count==2&&placements[0].advance==0&&placements[1].advance==1000.25f);
    }
    const u32 invalid_spacing[]{0xbf800000u,0x7f800000u,0xff800000u,0x7fc00000u};
    for(u32 bits_value:invalid_spacing){float spacing=0;copy_bytes(&spacing,&bits_value,4);
        CHECK(sfnt_bengali_shape_run(plan,cmap,{run_text,4},{forms,128},1000,storage,&result,{steps,3},{},false,spacing)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);}
    CHECK(sfnt_bengali_shape_run(plan,cmap,{run_text,4},{forms,128},1000,storage,&result,{steps,3})==SfntStatus::ok);
    const u64 run_work=result.lookup_operations;
    for(u64 work=0;work<=run_work;++work){const auto status=sfnt_bengali_shape_run(plan,cmap,{run_text,4},{forms,128},1000,storage,&result,{steps,3},{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==run_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    u32 utf8_sources[64]{};storage.character_glyphs={utf8_sources,64};
    const auto utf8_case=[&](ConstSlice<u32> scalars) noexcept {
        CHECK(sfnt_bengali_shape_run(plan,cmap,scalars,{forms,128},1000,storage,&result,{steps,3})==SfntStatus::ok);
        const auto saved=result;CHECK(saved.glyph_count<=32&&saved.source_units<=64);
        u16 saved_glyphs[32]{};u32 saved_sources[64]{};SfntPlacement saved_positions[32]{};
        for(u32 i=0;i<saved.glyph_count;++i)saved_glyphs[i]=glyphs[i].glyph.glyph;
        copy_bytes(saved_sources,utf8_sources,saved.source_units*4);copy_bytes(saved_positions,placements,saved.glyph_count*sizeof(SfntPlacement));
        char text[512]{};u32 bytes=0;for(u32 i=0;i<scalars.count;++i){CHECK(bytes+4<=512);bytes+=utf8_encode(scalars[i],text+bytes);}
        CHECK(native_bengali_run(plan,cmap,{text,bytes},{forms,128},1000,storage,&result,{steps,3})==SfntStatus::ok);
        CHECK(result.glyph_count==saved.glyph_count&&result.source_units==saved.source_units&&result.lookup_operations==saved.lookup_operations);
        for(u32 i=0;i<saved.glyph_count;++i)CHECK(glyphs[i].glyph.glyph==saved_glyphs[i]);
        CHECK(bytes_equal(utf8_sources,saved_sources,saved.source_units*4)&&bytes_equal(placements,saved_positions,saved.glyph_count*sizeof(SfntPlacement)));return true;
    };
    const u32 supplementary[]{0x995,0x9cb,0x1f600,0x995,0x9cb};u32 windowed[40]{};for(u32 i=0;i<40;++i)windowed[i]=i%2?0x9cdu:0x995u;
    CHECK(utf8_case({run_text,4})&&utf8_case({supplementary,5})&&utf8_case({windowed,40})&&utf8_case({}));
    const u8 bad_utf8[][4]{{0xc0,0xaf,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0xe0,0xa6,0,0}};const u32 bad_sizes[]{2,3,4,2};
    for(u32 i=0;i<4;++i)CHECK(native_bengali_run(plan,cmap,{reinterpret_cast<const char*>(bad_utf8[i]),bad_sizes[i]},{forms,128},1000,storage,&result,{steps,3})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    print("PASS Bengali staged reph/half/init, source preservation, controls and work limits\n");return true;
}
