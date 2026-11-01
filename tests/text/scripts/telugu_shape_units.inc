bool telugu_shape_units() noexcept {
    // A tiny first-party font with nonzero mark advances makes accidental
    // mark-zeroing visible, independent of the installed Nirmala observations.
    const u32 entries[][2]{{0x20,8},{0xa0,23},{0xc01,6},{0xc03,17},{0xc05,16},{0xc15,1},{0xc2f,2},{0xc30,4},{0xc3c,24},{0xc3e,5},{0xc3f,22},{0xc40,18},{0xc41,9},{0xc43,26},{0xc46,10},{0xc48,15},{0xc4b,14},{0xc4d,3},{0xc55,19},{0xc56,12},{0xc62,25},{0x25cc,7}};
    constexpr u32 entry_count=sizeof(entries)/sizeof(entries[0]);
    u8 cmap_bytes[16+12*entry_count]{};byte_store_be32(cmap_bytes,0x000c0000);byte_store_be32(cmap_bytes+4,sizeof(cmap_bytes));byte_store_be32(cmap_bytes+12,entry_count);
    for(u32 i=0;i<entry_count;++i){byte_store_be32(cmap_bytes+16+i*12,entries[i][0]);byte_store_be32(cmap_bytes+20+i*12,entries[i][0]);byte_store_be32(cmap_bytes+24+i*12,entries[i][1]);}
    const SfntCmap cmap{{cmap_bytes,sizeof(cmap_bytes)},{},32,12};
    const u16 reph[]{1,8,1,14,1,1,3,1,4,20,2,4},post[]{1,8,1,14,1,1,3,1,4,21,2,1};
    u8 bytes[2][24]{};const ConstSlice<u8> tables[]{encode(reph,bytes[0]),encode(post,bytes[1])};
    SfntGsubExecutableSubtable subtables[2]{};SfntGsubExecutableLookup lookups[2]{};
    for(u32 i=0;i<2;++i){CHECK(sfnt_gsub_subtable_open(tables[i],4,32,&subtables[i].basic)==SfntStatus::ok);lookups[i].type=4;lookups[i].subtables={subtables+i,1};}
    SfntGsubProgram program{{lookups,2},{},32};SfntShapePlan plan{};plan.substitutions=&program;plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* output) noexcept {*output=100;return SfntStatus::ok;};
    const u16 classes[]{2,5,3,3,3,5,6,3,9,12,3,20,20,3,24,24,3};u8 class_bytes[34]{};
    CHECK(sfnt_class_def_open(encode(classes,class_bytes),32,&plan.gdef.glyph_classes)==SfntStatus::ok);program.gdef=plan.gdef;
    SfntLookupStep steps[]{{0,sfnt_deva_pre,5},{1,sfnt_deva_below,6}};
    SfntComponentGlyph glyphs[40]{},replacement[40]{};SfntComponentNode nodes[80]{};u64 bits[2]{};
    u32 characters[40]{},targets[40]{},positions[128]{},rebased[128]{},consumed[40]{};
    SfntMarkAssociation associations[40]{};double advances[40]{};u8 flags[40]{};SfntPosition states[40]{};SfntPlacement placements[40]{};
    SfntGsubExecutionFrame frames[16]{};SfntComponentUndo journal[512]{};
    SfntShapeStorage storage{{glyphs,40},{nodes,80},{bits,2},{characters,40},{targets,40},{associations,40},{advances,40},{flags,40},{states,40},{placements,40},
        {{frames,16},{positions,128},{rebased,128},{consumed,40},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,512}},{}};
    u8 forms[128]{};forms[0x30]=sfnt_deva_pre;forms[0x15]=sfnt_deva_below;SfntShapeRunResult result{};
    const auto shape=[&](ConstSlice<u32> input,float spacing=0,bool rtl=false,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return indic_scalar_cluster(SfntIndicProfile::telugu,plan,cmap,input,{forms,128},1000,storage,&result,{steps,2},limits,false,false,spacing,rtl,false);};
    const auto same=[&](ConstSlice<u16> expected) noexcept {
        if(result.glyph_count!=expected.count)return false;
        for(u32 i=0;i<expected.count;++i)if(glyphs[i].glyph.glyph!=expected[i])return false;
        for(u32 i=0;i<result.source_units;++i)if(characters[i])return false;return true;};
    const u32 pref[]{0xc15,0xc4d,0xc30,0xc48},below[]{0xc15,0xc4d,0xc15,0xc48},orphan[]{0xc48};
    const u16 pref_glyphs[]{20,1,10,12},below_glyphs[]{1,10,12,21},orphan_glyphs[]{7,10,12};
    CHECK(shape({pref,4})==SfntStatus::ok&&same({pref_glyphs,4}));
    CHECK(shape({below,4})==SfntStatus::ok&&same({below_glyphs,4}));
    CHECK(placements[1].advance==100&&placements[2].advance==100);
    CHECK(shape({orphan,1})==SfntStatus::ok&&same({orphan_glyphs,3}));
    // A ligature created before or after pref must not impersonate a pref
    // substitution. The same prepared form map deliberately stays in place.
    const u8 wrong_stages[]{2,6};for(u8 stage:wrong_stages){steps[0].stage=stage;const u16 unmoved[]{1,20,10,12};
        CHECK(shape({pref,4})==SfntStatus::ok&&same({unmoved,4}));}
    steps[0].stage=5;
    const u32 repeated[]{0xc15,0xc4d,0xc30,0xc4d,0xc30,0xc48};
    const u16 repeated_glyphs[]{1,20,3,4,10,12};
    CHECK(shape({repeated,6})==SfntStatus::ok&&same({repeated_glyphs,6}));
    const u32 direct_join[]{0xc15,0x200d,0xc15,0xc48},circle_join[]{0x25cc,0x200d,0xc15,0xc48};
    const u16 direct_glyphs[]{1,8,10,12,1},circle_glyphs[]{7,8,10,12,1};
    CHECK(shape({direct_join,4})==SfntStatus::ok&&same({direct_glyphs,5}));
    CHECK(shape({circle_join,4})==SfntStatus::ok&&same({circle_glyphs,5}));
    const u32 vowel_nukta[]{0xc05,0xc4d,0xc15,0xc3c,0xc3e};const u16 vowel_nukta_glyphs[]{16,21,24,5};
    CHECK(shape({vowel_nukta,5})==SfntStatus::ok&&same({vowel_nukta_glyphs,4}));
    u32 vowel_group[10]{0xc15,0xc4d,0xc15,0xc3e,0xc41,0xc41,0xc41,0xc41,0xc41,0xc41};
    const u16 six_parts[]{1,5,9,9,9,9,9,21},seven_parts[]{1,21,5,9,9,9,9,9,9};
    CHECK(shape({vowel_group,9})==SfntStatus::ok&&same({six_parts,8}));
    CHECK(shape({vowel_group,10})==SfntStatus::ok&&same({seven_parts,9}));
    const u32 halant_tail[]{0xc15,0xc4d,0xc15,0xc40,0xc4d,0xc43,0xc41};
    const u16 halant_tail_glyphs[]{1,18,9,21,26,3};
    CHECK(shape({halant_tail,7})==SfntStatus::ok&&same({halant_tail_glyphs,6}));
    const u32 controls[]{0xc15,0xc01,0x200c};const u16 control_glyphs[]{1,8,6};const bool directions[]{false,true};
    for(bool rtl:directions){CHECK(shape({controls,3},.25f,rtl)==SfntStatus::ok&&same({control_glyphs,3}));
        CHECK(placements[0].advance==(rtl?100.25f:100)&&placements[2].advance==(rtl?100:100.25f)&&placements[1].advance==0);}
    storage.character_glyphs.count=1;CHECK(shape({orphan,1})==SfntStatus::ok&&same({orphan_glyphs,3}));storage.character_glyphs.count=40;
    u32 longest[31]{};longest[0]=0xc15;longest[1]=0xc48;for(u32 i=2;i<31;++i)longest[i]=0xc41;
    CHECK(shape({longest,31})==SfntStatus::ok&&result.source_units==31&&result.glyph_count==31&&glyphs[1].glyph.glyph==15);
    longest[0]=0x200d;longest[1]=0xc4d;longest[2]=0xc15;longest[3]=0xc48;
    CHECK(shape({longest,31})==SfntStatus::ok&&result.source_units==31&&result.glyph_count==30);
    CHECK(glyphs[0].glyph.glyph==8&&glyphs[1].glyph.glyph==21&&glyphs[2].glyph.glyph==15);
    CHECK(shape({pref,4})==SfntStatus::ok);const u64 total=result.lookup_operations;
    for(u64 work=0;work<=total;++work){const auto status=shape({pref,4},0,false,{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==total?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    const u32 invalid[]{0xc15,0xd800};
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({pref,4},-1)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    storage.glyphs.count=2;CHECK(shape({orphan,1})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);storage.glyphs.count=40;
    CHECK(indic_scalar_cluster(SfntIndicProfile::telugu,plan,cmap,{pref,4},{forms,127},1000,storage,&result,{steps,2},{},false,false,0,false,false)==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(shape({})==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    const u16 pair_words[]{1,18,4,0,1,12,1,1,50,1,1,1};u8 pair_bytes[24]{};SfntGposSubtable pair{};
    CHECK(sfnt_gpos_subtable_open(encode(pair_words,pair_bytes),2,32,&pair)==SfntStatus::ok);
    SfntGposExecutableLookup pair_lookup{};pair_lookup.type=2;pair_lookup.tables.basic={&pair,1};
    SfntGposProgram positioning{{&pair_lookup,1},{},32};const u16 root=0;plan.positioning=&positioning;plan.positioning_roots={&root,1};
    SfntGposExecutionFrame position_frames[4]{};u32 position_scratch[40]{};
    storage.positioning.frames={position_frames,4};storage.positioning.positions={position_scratch,40};
    const u32 run_text[]{0xc15,0xc15,0xc15};
    CHECK(indic_scalar_run(SfntIndicProfile::telugu,plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,2},{},false,.25f)==SfntStatus::ok);
    CHECK(result.glyph_count==3&&result.source_units==3&&result.position_matches==2);
    CHECK(placements[0].advance==150.25f&&placements[1].advance==150.25f&&placements[2].advance==100.25f);
    for(u32 i=0;i<3;++i)CHECK(characters[i]==i);
    const u32 run_controls[]{0x200d,0xc15,0xc4d,0x200d,0xc15};
    for(bool rtl:directions){CHECK(indic_scalar_run(SfntIndicProfile::telugu,plan,cmap,{run_controls,5},{forms,128},1000,storage,&result,{steps,2},{},rtl,.25f)==SfntStatus::ok);
        CHECK(result.glyph_count==5&&placements[0].advance==0&&placements[3].advance==0);}
    const auto utf8_case=[&](ConstSlice<u32> input) noexcept {return indic_scalar_run(SfntIndicProfile::telugu,plan,cmap,input,{forms,128},1000,storage,&result,{steps,2})==SfntStatus::ok;};
    u32 windowed[40]{};for(u32 i=0;i<40;++i)windowed[i]=i%2?0xc4du:0xc15u;
    const u32 supplementary[]{0xc15,0xc4c,0x1f600,0xc15,0xc3e};
    CHECK(utf8_case({run_text,3})&&utf8_case({run_controls,5})&&utf8_case({windowed,40})&&utf8_case({supplementary,5})&&utf8_case({}));
    const u8 malformed[][4]{{0xc0,0xaf,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0xe0,0xac,0,0}};const u32 bad_sizes[]{2,3,4,2};
    for(u32 i=0;i<4;++i)CHECK(indic_utf8_run(SfntIndicProfile::telugu,plan,cmap,{reinterpret_cast<const char*>(malformed[i]),bad_sizes[i]},{forms,128},1000,storage,&result,{steps,2},{},false,0)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(indic_scalar_run(SfntIndicProfile::telugu,plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,2})==SfntStatus::ok);const u64 run_work=result.lookup_operations;
    for(u64 work=0;work<=run_work;++work){const auto status=indic_scalar_run(SfntIndicProfile::telugu,plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,2},{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==run_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    print("PASS Telugu split vowels, font forms, pref stage identity, vowel-group thresholds, nonzero marks, controls, UTF-8 runs and bounded failures\n");return true;
}
