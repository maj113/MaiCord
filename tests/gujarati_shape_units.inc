bool gujarati_shape_units() noexcept {
    const u32 entries[][2]{{0x20,8},{0xa81,6},{0xa95,1},{0xab0,2},{0xabc,13},{0xabe,5},{0xabf,9},{0xacd,3},{0xafd,14},{0x25cc,7}};
    u8 cmap_bytes[136]{};byte_store_be32(cmap_bytes,0x000c0000);byte_store_be32(cmap_bytes+4,sizeof(cmap_bytes));byte_store_be32(cmap_bytes+12,10);
    for(u32 i=0;i<10;++i){byte_store_be32(cmap_bytes+16+i*12,entries[i][0]);byte_store_be32(cmap_bytes+20+i*12,entries[i][0]);byte_store_be32(cmap_bytes+24+i*12,entries[i][1]);}
    const SfntCmap cmap{{cmap_bytes,sizeof(cmap_bytes)},{},32,12};
    const u16 reph[]{1,8,1,14,1,1,2,1,4,20,2,3},half[]{1,8,1,14,1,1,1,1,4,21,2,3};
    u8 bytes[2][32]{};const ConstSlice<u8> tables[]{encode(reph,bytes[0]),encode(half,bytes[1])};
    SfntGsubExecutableSubtable subtables[2]{};SfntGsubExecutableLookup lookups[2]{};
    for(u32 i=0;i<2;++i){CHECK(sfnt_gsub_subtable_open(tables[i],4,32,&subtables[i].basic)==SfntStatus::ok);lookups[i].type=4;lookups[i].subtables={subtables+i,1};}
    SfntGsubProgram program{{lookups,2},{},32};SfntShapePlan plan{};plan.substitutions=&program;plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* output) noexcept {*output=1000;return SfntStatus::ok;};
    const u16 classes[]{2,3,5,6,3,9,9,3,13,14,3};u8 class_bytes[22]{};
    CHECK(sfnt_class_def_open(encode(classes,class_bytes),32,&plan.gdef.glyph_classes)==SfntStatus::ok);program.gdef=plan.gdef;
    const SfntLookupStep steps[]{{0,sfnt_deva_reph,3},{1,sfnt_deva_half,7}};
    SfntComponentGlyph glyphs[32]{},replacement[32]{};SfntComponentNode nodes[64]{};u64 bits[2]{};
    u32 characters[32]{},targets[32]{},positions[128]{},rebased[128]{},consumed[32]{};
    SfntMarkAssociation associations[32]{};double advances[32]{};u8 flags[32]{};SfntPosition states[32]{};SfntPlacement placements[32]{};
    SfntGsubExecutionFrame frames[16]{};SfntComponentUndo journal[512]{};
    SfntShapeStorage storage{{glyphs,32},{nodes,64},{bits,2},{characters,32},{targets,32},{associations,32},{advances,32},{flags,32},{states,32},{placements,32},
        {{frames,16},{positions,128},{rebased,128},{consumed,32},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,512}},{}};
    u8 forms[128]{};forms[0x30]=sfnt_deva_reph;forms[0x15]=sfnt_deva_half;SfntShapeRunResult result{};
    const auto shape=[&](ConstSlice<u32> input,float spacing=0,bool rtl=false,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return sfnt_gujarati_shape_cluster(plan,cmap,input,{forms,128},1000,storage,&result,{steps,2},limits,false,spacing,rtl);};
    const u32 reph_text[]{0xab0,0xacd,0xa95,0xabf},half_nukta[]{0xa95,0xacd,0xabc,0xa95,0xabf},full_nukta[]{0xab0,0xacd,0xabc,0xa95,0xabf};
    CHECK(shape({reph_text,4})==SfntStatus::ok&&result.glyph_count==3&&result.source_units==4);
    CHECK(glyphs[0].glyph.glyph==9&&glyphs[1].glyph.glyph==1&&glyphs[2].glyph.glyph==20);
    CHECK(shape({half_nukta,5})==SfntStatus::ok&&result.glyph_count==5);
    const u16 half_expected[]{9,1,13,3,1};for(u32 i=0;i<5;++i)CHECK(glyphs[i].glyph.glyph==half_expected[i]&&!characters[i]);
    CHECK(shape({full_nukta,5})==SfntStatus::ok&&result.glyph_count==5);
    const u16 full_expected[]{2,13,3,9,1};for(u32 i=0;i<5;++i)CHECK(glyphs[i].glyph.glyph==full_expected[i]&&!characters[i]);
    const u32 post[]{0xa95,0xabe},joined[]{0xa95,0xacd,0x200d},orphan[]{0xabf};
    CHECK(shape({post,2},.25f)==SfntStatus::ok&&placements[0].advance==1000&&placements[1].advance==.25f);
    CHECK(shape({post,2},.25f,true)==SfntStatus::ok&&placements[0].advance==1000.25f&&placements[1].advance==0);
    const bool directions[]{false,true};for(bool rtl:directions){CHECK(shape({joined,3},.25f,rtl)==SfntStatus::ok&&result.glyph_count==2);
        CHECK(glyphs[0].glyph.glyph==21&&glyphs[1].glyph.glyph==8&&placements[0].advance==1000.25f&&placements[1].advance==0);}
    const u32 late_nukta[]{0xa95,0xacd,0xafd},control_tail[]{0xa95,0xacd,0xa81,0x200c};
    CHECK(shape({late_nukta,3})==SfntStatus::ok&&result.glyph_count==3&&glyphs[0].glyph.glyph==1&&glyphs[1].glyph.glyph==14&&glyphs[2].glyph.glyph==3);
    CHECK(shape({control_tail,4})==SfntStatus::ok&&result.glyph_count==4&&glyphs[2].glyph.glyph==8&&glyphs[3].glyph.glyph==6);
    storage.character_glyphs.count=1;
    CHECK(shape({orphan,1})==SfntStatus::ok&&result.glyph_count==2&&result.source_units==1&&glyphs[0].glyph.glyph==9&&glyphs[1].glyph.glyph==7);
    storage.character_glyphs.count=32;
    CHECK(shape({half_nukta,5})==SfntStatus::ok);const u64 total=result.lookup_operations;
    for(u64 work=0;work<=total;++work){const auto status=shape({half_nukta,5},0,false,{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==total?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    const u32 third_nukta[]{0xa95,0xabc,0xabc,0xacd,0xabc,0xa95},multiple[]{0xa95,0xa95},invalid[]{0xa95,0xd800};
    CHECK(shape({third_nukta,6})==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    CHECK(shape({multiple,2})==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({post,2},-1)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({half_nukta,5},0,false,{1000000,100000,0})==SfntStatus::resource_limit&&!result.glyph_count);
    storage.glyphs.count=1;CHECK(shape({post,2})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);storage.glyphs.count=32;
    CHECK(sfnt_gujarati_shape_cluster(plan,cmap,{post,2},{forms,127},1000,storage,&result,{steps,2})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(shape({})==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    const u16 pair_words[]{1,18,4,0,1,12,1,1,50,1,1,1};u8 pair_bytes[24]{};SfntGposSubtable pair{};
    CHECK(sfnt_gpos_subtable_open(encode(pair_words,pair_bytes),2,32,&pair)==SfntStatus::ok);
    SfntGposExecutableLookup pair_lookup{};pair_lookup.type=2;pair_lookup.tables.basic={&pair,1};
    SfntGposProgram positioning{{&pair_lookup,1},{},32};const u16 root=0;plan.positioning=&positioning;plan.positioning_roots={&root,1};
    SfntGposExecutionFrame position_frames[4]{};u32 position_scratch[32]{},run_sources[96]{};
    storage.positioning.frames={position_frames,4};storage.positioning.positions={position_scratch,32};storage.character_glyphs={run_sources,96};
    const u32 run_text[]{0xa95,0xa95,0xa95};
    CHECK(sfnt_gujarati_shape_run(plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,2},{},false,.25f)==SfntStatus::ok);
    CHECK(result.glyph_count==3&&result.source_units==3&&result.position_matches==2);
    CHECK(placements[0].advance==1050.25f&&placements[1].advance==1050.25f&&placements[2].advance==1000.25f);
    for(u32 i=0;i<3;++i)CHECK(run_sources[i]==i);
    const u32 run_controls[]{0x200d,0xa95,0xacd,0x200d,0xa95};
    for(bool rtl:directions){CHECK(sfnt_gujarati_shape_run(plan,cmap,{run_controls,5},{forms,128},1000,storage,&result,{steps,2},{},rtl,.25f)==SfntStatus::ok);
        CHECK(result.glyph_count==4&&placements[0].advance==0&&placements[1].advance==(rtl?1000.25f:1000)&&placements[2].advance==0&&placements[3].advance==(rtl?1000:1000.25f)&&run_sources[1]==1&&run_sources[4]==1);}
    const auto utf8_case=[&](ConstSlice<u32> input) noexcept {
        CHECK(sfnt_gujarati_shape_run(plan,cmap,input,{forms,128},1000,storage,&result,{steps,2})==SfntStatus::ok);
        const auto saved=result;u16 saved_glyphs[32]{};u32 saved_sources[96]{};SfntPlacement saved_placements[32]{};
        CHECK(saved.glyph_count<=32&&saved.source_units<=96);
        for(u32 i=0;i<saved.glyph_count;++i)saved_glyphs[i]=glyphs[i].glyph.glyph;
        copy_bytes(saved_sources,run_sources,saved.source_units*4);copy_bytes(saved_placements,placements,saved.glyph_count*sizeof(SfntPlacement));
        char text[512]{};u32 bytes_written=0;for(u32 i=0;i<input.count;++i){CHECK(bytes_written+4<=512);bytes_written+=utf8_encode(input[i],text+bytes_written);}
        CHECK(native_gujarati_run(plan,cmap,{text,bytes_written},{forms,128},1000,storage,&result,{steps,2})==SfntStatus::ok);
        CHECK(result.glyph_count==saved.glyph_count&&result.source_units==saved.source_units&&result.lookup_operations==saved.lookup_operations);
        for(u32 i=0;i<saved.glyph_count;++i)CHECK(glyphs[i].glyph.glyph==saved_glyphs[i]);
        CHECK(bytes_equal(saved_sources,run_sources,saved.source_units*4)&&bytes_equal(saved_placements,placements,saved.glyph_count*sizeof(SfntPlacement)));return true;
    };
    u32 windowed[40]{};for(u32 i=0;i<40;++i)windowed[i]=i%2?0xacdu:0xa95u;
    const u32 supplementary[]{0xa95,0x1f600,0xabf};
    CHECK(utf8_case({run_text,3})&&utf8_case({run_controls,5})&&utf8_case({windowed,40})&&utf8_case({supplementary,3})&&utf8_case({}));
    const u8 malformed[][4]{{0xc0,0xaf,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0xe0,0xaa,0,0}};const u32 bad_sizes[]{2,3,4,2};
    for(u32 i=0;i<4;++i)CHECK(native_gujarati_run(plan,cmap,{reinterpret_cast<const char*>(malformed[i]),bad_sizes[i]},{forms,128},1000,storage,&result,{steps,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    print("PASS Gujarati staged reordering, font-dependent nukta, run-wide GPOS, UTF-8, tracking and bounded failures\n");return true;
}
