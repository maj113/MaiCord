bool odia_shape_units() noexcept {
    // A tiny first-party font with nonzero mark advances makes accidental
    // mark-zeroing visible, independent of the installed Nirmala observations.
    const u32 entries[][2]{{0x20,8},{0xa0,23},{0xb01,6},{0xb03,17},{0xb05,16},{0xb15,1},{0xb2f,2},{0xb30,4},{0xb3e,5},{0xb3f,22},{0xb40,18},{0xb41,9},{0xb47,10},{0xb4b,14},{0xb4c,15},{0xb4d,3},{0xb55,19},{0xb56,11},{0xb57,12},{0x25cc,7}};
    constexpr u32 entry_count=sizeof(entries)/sizeof(entries[0]);
    u8 cmap_bytes[16+12*entry_count]{};byte_store_be32(cmap_bytes,0x000c0000);byte_store_be32(cmap_bytes+4,sizeof(cmap_bytes));byte_store_be32(cmap_bytes+12,entry_count);
    for(u32 i=0;i<entry_count;++i){byte_store_be32(cmap_bytes+16+i*12,entries[i][0]);byte_store_be32(cmap_bytes+20+i*12,entries[i][0]);byte_store_be32(cmap_bytes+24+i*12,entries[i][1]);}
    const SfntCmap cmap{{cmap_bytes,sizeof(cmap_bytes)},{},32,12};
    const u16 reph[]{1,8,1,14,1,1,4,1,4,20,2,3},post[]{1,8,1,14,1,1,3,1,4,21,2,2};
    u8 bytes[2][24]{};const ConstSlice<u8> tables[]{encode(reph,bytes[0]),encode(post,bytes[1])};
    SfntGsubExecutableSubtable subtables[2]{};SfntGsubExecutableLookup lookups[2]{};
    for(u32 i=0;i<2;++i){CHECK(sfnt_gsub_subtable_open(tables[i],4,32,&subtables[i].basic)==SfntStatus::ok);lookups[i].type=4;lookups[i].subtables={subtables+i,1};}
    SfntGsubProgram program{{lookups,2},{},32};SfntShapePlan plan{};plan.substitutions=&program;plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* output) noexcept {*output=100;return SfntStatus::ok;};
    const u16 classes[]{2,4,3,3,3,5,6,3,9,12,3,20,20,3};u8 class_bytes[28]{};
    CHECK(sfnt_class_def_open(encode(classes,class_bytes),32,&plan.gdef.glyph_classes)==SfntStatus::ok);program.gdef=plan.gdef;
    const SfntLookupStep steps[]{{0,sfnt_deva_reph,3},{1,sfnt_deva_post,8}};
    SfntComponentGlyph glyphs[40]{},replacement[40]{};SfntComponentNode nodes[80]{};u64 bits[2]{};
    u32 characters[40]{},targets[40]{},positions[128]{},rebased[128]{},consumed[40]{};
    SfntMarkAssociation associations[40]{};double advances[40]{};u8 flags[40]{};SfntPosition states[40]{};SfntPlacement placements[40]{};
    SfntGsubExecutionFrame frames[16]{};SfntComponentUndo journal[512]{};
    SfntShapeStorage storage{{glyphs,40},{nodes,80},{bits,2},{characters,40},{targets,40},{associations,40},{advances,40},{flags,40},{states,40},{placements,40},
        {{frames,16},{positions,128},{rebased,128},{consumed,40},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,512}},{}};
    u8 forms[128]{};forms[0x30]=sfnt_deva_reph;forms[0x2f]=sfnt_deva_post;SfntShapeRunResult result{};
    const auto shape=[&](ConstSlice<u32> input,float spacing=0,bool rtl=false,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return sfnt_odia_shape_cluster(plan,cmap,input,{forms,128},1000,storage,&result,{steps,2},limits,false,spacing,rtl);};
    const auto same=[&](ConstSlice<u16> expected) noexcept {
        if(result.glyph_count!=expected.count)return false;
        for(u32 i=0;i<expected.count;++i)if(glyphs[i].glyph.glyph!=expected[i])return false;
        for(u32 i=0;i<result.source_units;++i)if(characters[i])return false;return true;};
    const u32 split[]{0xb30,0xb4d,0xb15,0xb4c},post_tail[]{0xb15,0xb4d,0xb2f,0xb41,0xb01},orphan[]{0xb4c};
    const u16 split_glyphs[]{10,1,20,12},post_glyphs[]{1,6,9,21},orphan_glyphs[]{10,7,12};
    CHECK(shape({split,4})==SfntStatus::ok&&same({split_glyphs,4}));
    CHECK(shape({post_tail,5})==SfntStatus::ok&&same({post_glyphs,4}));
    CHECK(placements[1].advance==100&&placements[2].advance==100);
    // Frozen Windows reductions, expressed through this font's independent
    // glyph IDs. These exercise replacement identity and ordering without GSUB.
    const u32 upper_tail[]{0xb15,0xb41,0xb40,0xb01},long_tail[]{0xb15,0xb3e,0xb40,0xb3e,0xb40,0xb3e,0xb01};
    const u16 upper_expected[]{1,6,6,9},long_expected[]{1,6,6,5,18,18,5};
    CHECK(shape({upper_tail,4})==SfntStatus::ok&&same({upper_expected,4}));
    CHECK(shape({long_tail,7})==SfntStatus::ok&&same({long_expected,7}));
    const u32 overline_tail[]{0xb15,0xb55,0xb55,0xb03},halant_overline[]{0xb15,0xb55,0xb4d,0xb55,0xb57};
    const u16 overline_expected[]{1,17,19,17},halant_expected[]{1,12,19,3,19};
    CHECK(shape({overline_tail,4})==SfntStatus::ok&&same({overline_expected,4}));
    CHECK(shape({halant_overline,5})==SfntStatus::ok&&same({halant_expected,5}));
    const u32 left_overline_tail[]{0xb15,0xb47,0xb55,0xb57,0xb03};
    const u16 left_overline_expected[]{10,19,1,12,17};
    CHECK(shape({left_overline_tail,5})==SfntStatus::ok&&same({left_overline_expected,5}));
    const u32 vowel_join[]{0xb05,0x200d,0xb15,0xb3f},placeholder_join[]{0xb05,0x200d,0xb4d,0xb15,0xa0,0xb3f};
    const u16 vowel_expected[]{16,8,1,22},placeholder_expected[]{16,22,8,3,1,23};
    forms[0x15]=sfnt_deva_below;
    CHECK(shape({vowel_join,4})==SfntStatus::ok&&same({vowel_expected,4}));
    CHECK(shape({placeholder_join,6})==SfntStatus::ok&&same({placeholder_expected,6}));
    forms[0x15]=0;
    storage.character_glyphs.count=1;CHECK(shape({orphan,1})==SfntStatus::ok&&same({orphan_glyphs,3}));storage.character_glyphs.count=40;
    u32 longest[31]{};longest[0]=0xb15;for(u32 i=1;i<29;++i)longest[i]=0xb3e;longest[29]=0xb4b;longest[30]=0xb4c;
    CHECK(shape({longest,31})==SfntStatus::ok&&result.source_units==31&&result.glyph_count==31);
    CHECK(glyphs[0].glyph.glyph==1&&glyphs[29].glyph.glyph==14&&glyphs[30].glyph.glyph==15);
    longest[0]=0x200d;longest[1]=0xb4d;longest[2]=0xb15;
    CHECK(shape({longest,31})==SfntStatus::ok&&result.source_units==31&&result.glyph_count==31);
    u32 circles=0,left_parts=0;for(u32 i=0;i<result.glyph_count;++i){circles+=glyphs[i].glyph.glyph==7;left_parts+=glyphs[i].glyph.glyph==10;}
    CHECK(!circles&&!left_parts);
    storage.glyphs.count=30;CHECK(shape({longest,31})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);storage.glyphs.count=40;
    const u32 controls[]{0xb15,0xb01,0x200c};const u16 control_glyphs[]{1,6,8};const bool directions[]{false,true};
    for(bool rtl:directions){CHECK(shape({controls,3},.25f,rtl)==SfntStatus::ok&&same({control_glyphs,3}));
        CHECK(placements[0].advance==(rtl?100.25f:100)&&placements[1].advance==(rtl?100:100.25f)&&placements[2].advance==0);}
    CHECK(shape({split,4})==SfntStatus::ok);const u64 total=result.lookup_operations;
    for(u64 work=0;work<=total;++work){const auto status=shape({split,4},0,false,{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==total?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    const u32 multiple[]{0xb15,0xb15},invalid[]{0xb15,0xd800};
    CHECK(shape({multiple,2})==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({split,4},-1)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    storage.glyphs.count=2;CHECK(shape({orphan,1})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);storage.glyphs.count=40;
    CHECK(sfnt_odia_shape_cluster(plan,cmap,{split,4},{forms,127},1000,storage,&result,{steps,2})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(shape({})==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    const u16 pair_words[]{1,18,4,0,1,12,1,1,50,1,1,1};u8 pair_bytes[24]{};SfntGposSubtable pair{};
    CHECK(sfnt_gpos_subtable_open(encode(pair_words,pair_bytes),2,32,&pair)==SfntStatus::ok);
    SfntGposExecutableLookup pair_lookup{};pair_lookup.type=2;pair_lookup.tables.basic={&pair,1};
    SfntGposProgram positioning{{&pair_lookup,1},{},32};const u16 root=0;plan.positioning=&positioning;plan.positioning_roots={&root,1};
    SfntGposExecutionFrame position_frames[4]{};u32 position_scratch[40]{};
    storage.positioning.frames={position_frames,4};storage.positioning.positions={position_scratch,40};
    const u32 run_text[]{0xb15,0xb15,0xb15};
    CHECK(sfnt_odia_shape_run(plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,2},{},false,.25f)==SfntStatus::ok);
    CHECK(result.glyph_count==3&&result.source_units==3&&result.position_matches==2);
    CHECK(placements[0].advance==150.25f&&placements[1].advance==150.25f&&placements[2].advance==100.25f);
    for(u32 i=0;i<3;++i)CHECK(characters[i]==i);
    const u32 run_controls[]{0x200d,0xb15,0xb4d,0x200d,0xb15};
    for(bool rtl:directions){CHECK(sfnt_odia_shape_run(plan,cmap,{run_controls,5},{forms,128},1000,storage,&result,{steps,2},{},rtl,.25f)==SfntStatus::ok);
        CHECK(result.glyph_count==5&&placements[0].advance==0&&placements[3].advance==0);}
    const auto utf8_case=[&](ConstSlice<u32> input) noexcept {
        CHECK(sfnt_odia_shape_run(plan,cmap,input,{forms,128},1000,storage,&result,{steps,2})==SfntStatus::ok);
        const auto saved=result;u16 saved_glyphs[40]{};u32 saved_sources[40]{};SfntPlacement saved_placements[40]{};
        CHECK(saved.glyph_count<=40&&saved.source_units<=40);
        for(u32 i=0;i<saved.glyph_count;++i)saved_glyphs[i]=glyphs[i].glyph.glyph;
        copy_bytes(saved_sources,characters,saved.source_units*4);copy_bytes(saved_placements,placements,saved.glyph_count*sizeof(SfntPlacement));
        char text[512]{};u32 bytes_written=0;for(u32 i=0;i<input.count;++i){CHECK(bytes_written+4<=512);bytes_written+=utf8_encode(input[i],text+bytes_written);}
        CHECK(native_odia_run(plan,cmap,{text,bytes_written},{forms,128},1000,storage,&result,{steps,2})==SfntStatus::ok);
        CHECK(result.glyph_count==saved.glyph_count&&result.source_units==saved.source_units&&result.lookup_operations==saved.lookup_operations);
        for(u32 i=0;i<saved.glyph_count;++i)CHECK(glyphs[i].glyph.glyph==saved_glyphs[i]);
        CHECK(bytes_equal(saved_sources,characters,saved.source_units*4)&&bytes_equal(saved_placements,placements,saved.glyph_count*sizeof(SfntPlacement)));return true;};
    u32 windowed[40]{};for(u32 i=0;i<40;++i)windowed[i]=i%2?0xb4du:0xb15u;
    const u32 supplementary[]{0xb15,0xb4c,0x1f600,0xb15,0xb3e};
    CHECK(utf8_case({run_text,3})&&utf8_case({run_controls,5})&&utf8_case({windowed,40})&&utf8_case({supplementary,5})&&utf8_case({}));
    const u8 malformed[][4]{{0xc0,0xaf,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0xe0,0xac,0,0}};const u32 bad_sizes[]{2,3,4,2};
    for(u32 i=0;i<4;++i)CHECK(native_odia_run(plan,cmap,{reinterpret_cast<const char*>(malformed[i]),bad_sizes[i]},{forms,128},1000,storage,&result,{steps,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(sfnt_odia_shape_run(plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,2})==SfntStatus::ok);const u64 run_work=result.lookup_operations;
    for(u64 work=0;work<=run_work;++work){const auto status=sfnt_odia_shape_run(plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,2},{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==run_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    print("PASS Odia split vowels, reph, ordering, nonzero marks, controls, run GPOS, UTF-8 and bounded failures\n");return true;
}
