bool kannada_shape_units() noexcept {
    // First-party font: separate reph/pref/below ligatures and deliberately
    // nonzero mark advances expose ordering, stage identity and zeroing errors.
    const u32 entries[][2]{{0x20,8},{0xa0,8},{0xc81,6},{0xc82,17},{0xc85,16},{0xc95,1},{0xcb0,4},{0xcbc,24},
        {0xcbe,25},{0xcbf,5},{0xcc0,18},{0xcc1,9},{0xcc2,11},{0xcc3,26},{0xcc6,10},{0xcc7,19},{0xcc8,15},
        {0xcca,23},{0xccb,14},{0xccd,3},{0xcd5,12},{0xcd6,13},{0x25cc,7}};
    constexpr u32 entry_count=sizeof(entries)/sizeof(entries[0]);
    u8 cmap_bytes[16+12*entry_count]{};byte_store_be32(cmap_bytes,0x000c0000);byte_store_be32(cmap_bytes+4,sizeof(cmap_bytes));byte_store_be32(cmap_bytes+12,entry_count);
    for(u32 i=0;i<entry_count;++i){byte_store_be32(cmap_bytes+16+i*12,entries[i][0]);byte_store_be32(cmap_bytes+20+i*12,entries[i][0]);byte_store_be32(cmap_bytes+24+i*12,entries[i][1]);}
    const SfntCmap cmap{{cmap_bytes,sizeof(cmap_bytes)},{},32,12};
    const u16 reph[]{1,8,1,14,1,1,4,1,4,20,2,3},pref[]{1,8,1,14,1,1,3,1,4,22,2,4},below[]{1,8,1,14,1,1,3,1,4,21,2,1};
    u8 bytes[3][24]{};const ConstSlice<u8> tables[]{encode(reph,bytes[0]),encode(pref,bytes[1]),encode(below,bytes[2])};
    SfntGsubExecutableSubtable subtables[3]{};SfntGsubExecutableLookup lookups[3]{};
    for(u32 i=0;i<3;++i){CHECK(sfnt_gsub_subtable_open(tables[i],4,32,&subtables[i].basic)==SfntStatus::ok);lookups[i].type=4;lookups[i].subtables={subtables+i,1};}
    SfntGsubProgram program{{lookups,3},{},32};SfntShapePlan plan{};plan.substitutions=&program;plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* output) noexcept {*output=100;return SfntStatus::ok;};
    const u16 classes[]{2,4,5,6,3,9,13,3,20,21,3,24,24,3};u8 class_bytes[28]{};
    CHECK(sfnt_class_def_open(encode(classes,class_bytes),32,&plan.gdef.glyph_classes)==SfntStatus::ok);program.gdef=plan.gdef;
    SfntLookupStep steps[]{{0,sfnt_deva_reph,3},{1,sfnt_deva_pre,5},{2,sfnt_deva_below,6}};
    SfntComponentGlyph glyphs[40]{},replacement[40]{};SfntComponentNode nodes[80]{};u64 bits[2]{};
    u32 characters[40]{},targets[40]{},positions[128]{},rebased[128]{},consumed[40]{};
    SfntMarkAssociation associations[40]{};double advances[40]{};u8 flags[40]{};SfntPosition states[40]{};SfntPlacement placements[40]{};
    SfntGsubExecutionFrame frames[16]{};SfntComponentUndo journal[512]{};
    SfntShapeStorage storage{{glyphs,40},{nodes,80},{bits,2},{characters,40},{targets,40},{associations,40},{advances,40},{flags,40},{states,40},{placements,40},
        {{frames,16},{positions,128},{rebased,128},{consumed,40},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,512}},{}};
    u8 forms[128]{};forms[0x30]=sfnt_deva_reph|sfnt_deva_pre;forms[0x15]=sfnt_deva_below;SfntShapeRunResult result{};
    const auto shape=[&](ConstSlice<u32> input,float spacing=0,bool rtl=false,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return indic_scalar_cluster(SfntIndicProfile::kannada,plan,cmap,input,{forms,128},1000,storage,&result,{steps,3},limits,false,false,spacing,rtl,false);};
    const auto same=[&](ConstSlice<u16> expected) noexcept {
        if(result.glyph_count!=expected.count)return false;
        for(u32 i=0;i<expected.count;++i)if(glyphs[i].glyph.glyph!=expected[i])return false;
        for(u32 i=0;i<result.source_units;++i)if(characters[i])return false;return true;};
    const u32 simple[]{0xc95,0xccb},subscript[]{0xc95,0xccd,0xc95,0xccb},reph_text[]{0xcb0,0xccd,0xc95,0xccb},orphan[]{0xccb};
    const u16 simple_glyphs[]{1,10,11,12},subscript_glyphs[]{1,10,11,21,12},reph_glyphs[]{1,10,11,12,20},orphan_glyphs[]{7,10,11,12};
    CHECK(shape({simple,2})==SfntStatus::ok&&same({simple_glyphs,4})&&result.source_units==2);
    CHECK(shape({subscript,4})==SfntStatus::ok&&same({subscript_glyphs,5}));
    for(u32 i=0;i<5;++i)CHECK(placements[i].advance==100);
    CHECK(shape({reph_text,4})==SfntStatus::ok&&same({reph_glyphs,5}));
    CHECK(shape({orphan,1})==SfntStatus::ok&&same({orphan_glyphs,4}));
    const u32 pre_text[]{0xc95,0xccd,0xcb0,0xccb};const u16 pre_glyphs[]{22,1,10,11,12};
    CHECK(shape({pre_text,4})==SfntStatus::ok&&same({pre_glyphs,5}));
    const u8 wrong_stages[]{4,6};for(u8 stage:wrong_stages){steps[1].stage=stage;const u16 unmoved[]{1,22,10,11,12};
        CHECK(shape({pre_text,4})==SfntStatus::ok&&same({unmoved,5}));}steps[1].stage=5;
    const u32 direct[]{0xc95,0x200d,0xc95,0xccb},space_join[]{0xc95,0xa0,0xc95,0xccb};
    const u16 direct_glyphs[]{10,11,8,21,12};
    CHECK(shape({direct,4})==SfntStatus::ok&&same({direct_glyphs,5})&&placements[2].advance==0);
    CHECK(shape({space_join,4})==SfntStatus::ok&&same({direct_glyphs,5}));
    CHECK(placements[2].advance==100); // NBSP retains spacing; ZWJ above does not.
    const u32 half[]{0xc95,0xccd,0xc95,0xccd,0x200d};const u16 half_glyphs[]{1,3,1,3,8};
    CHECK(shape({half,5})==SfntStatus::ok&&same({half_glyphs,5}));
    const u32 double_control[]{0xc95,0x200c,0x200d,0xc95},halant_nbsp[]{0xc95,0xccd,0xa0,0xc95},
        control_halant[]{0xc95,0x200c,0xccd,0xc95,0xcbf},nbsp_halant[]{0xc95,0xa0,0xccd,0xc95,0xcbf};
    const u16 double_control_glyphs[]{1,8,21},halant_nbsp_glyphs[]{1,3,8,1},
        control_halant_glyphs[]{1,8,5,21},nbsp_halant_glyphs[]{1,5,8,21};
    CHECK(shape({double_control,4})==SfntStatus::ok&&same({double_control_glyphs,3}));
    CHECK(shape({halant_nbsp,4})==SfntStatus::ok&&same({halant_nbsp_glyphs,4}));
    CHECK(shape({control_halant,5})==SfntStatus::ok&&same({control_halant_glyphs,4}));
    CHECK(shape({nbsp_halant,5})==SfntStatus::ok&&same({nbsp_halant_glyphs,4}));
    const u32 control[]{0xc95,0xc82,0x200c};const u16 control_glyphs[]{1,8,17};const bool directions[]{false,true};
    for(bool rtl:directions){CHECK(shape({control,3},.25f,rtl)==SfntStatus::ok&&same({control_glyphs,3}));
        CHECK(placements[1].advance==0&&placements[0].advance==(rtl?100.25f:100)&&placements[2].advance==(rtl?100:100.25f));}
    u32 long_input[31]{0xc95,0xccb};for(u32 i=2;i<31;++i)long_input[i]=0xcc1;
    CHECK(shape({long_input,29})==SfntStatus::ok&&result.source_units==29&&result.glyph_count==31);
    CHECK(shape({long_input,30})==SfntStatus::ok&&result.source_units==30&&result.glyph_count==30&&glyphs[1].glyph.glyph==14);
    CHECK(shape({long_input,31})==SfntStatus::ok&&result.source_units==31&&result.glyph_count==31&&glyphs[1].glyph.glyph==14);
    CHECK(shape({pre_text,4})==SfntStatus::ok);const u64 total=result.lookup_operations;
    for(u64 work=0;work<=total;++work){const auto status=shape({pre_text,4},0,false,{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==total?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    const u32 invalid[]{0xc95,0xd800};
    CHECK(shape({invalid,2})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({simple,2},-1)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    storage.glyphs.count=3;CHECK(shape({orphan,1})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);storage.glyphs.count=40;
    CHECK(indic_scalar_cluster(SfntIndicProfile::kannada,plan,cmap,{simple,2},{forms,127},1000,storage,&result,{steps,3},{},false,false,0,false,false)==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(shape({})==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    const u16 pair_words[]{1,18,4,0,1,12,1,1,50,1,1,1};u8 pair_bytes[24]{};SfntGposSubtable pair{};
    CHECK(sfnt_gpos_subtable_open(encode(pair_words,pair_bytes),2,32,&pair)==SfntStatus::ok);
    SfntGposExecutableLookup pair_lookup{};pair_lookup.type=2;pair_lookup.tables.basic={&pair,1};
    SfntGposProgram positioning{{&pair_lookup,1},{},32};const u16 root=0;plan.positioning=&positioning;plan.positioning_roots={&root,1};
    SfntGposExecutionFrame position_frames[4]{};u32 position_scratch[40]{};
    storage.positioning.frames={position_frames,4};storage.positioning.positions={position_scratch,40};
    const u32 run_text[]{0xc95,0xc95,0xc95};
    CHECK(indic_scalar_run(SfntIndicProfile::kannada,plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,3},{},false,.25f)==SfntStatus::ok);
    CHECK(result.glyph_count==3&&result.source_units==3&&result.position_matches==2);
    CHECK(placements[0].advance==150.25f&&placements[1].advance==150.25f&&placements[2].advance==100.25f);
    for(u32 i=0;i<3;++i)CHECK(characters[i]==i);
    const u32 run_controls[]{0x200d,0xc95,0xccd,0x200d,0xc95};
    for(bool rtl:directions){CHECK(indic_scalar_run(SfntIndicProfile::kannada,plan,cmap,{run_controls,5},{forms,128},1000,storage,&result,{steps,3},{},rtl,.25f)==SfntStatus::ok);
        CHECK(result.glyph_count==5&&placements[0].advance==0&&placements[3].advance==0);}
    const auto utf8_case=[&](ConstSlice<u32> input) noexcept {return indic_scalar_run(SfntIndicProfile::kannada,plan,cmap,input,{forms,128},1000,storage,&result,{steps,3})==SfntStatus::ok;};
    u32 windowed[40]{};for(u32 i=0;i<40;++i)windowed[i]=i%2?0xccdu:0xc95u;
    const u32 supplementary[]{0xc95,0xccc,0x1f600,0xc95,0xcbe};
    CHECK(utf8_case({run_text,3})&&utf8_case({run_controls,5})&&utf8_case({windowed,40})&&utf8_case({supplementary,5})&&utf8_case({}));
    const u8 malformed[][4]{{0xc0,0xaf,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0xe0,0xac,0,0}};const u32 bad_sizes[]{2,3,4,2};
    for(u32 i=0;i<4;++i)CHECK(indic_utf8_run(SfntIndicProfile::kannada,plan,cmap,{reinterpret_cast<const char*>(malformed[i]),bad_sizes[i]},{forms,128},1000,storage,&result,{steps,3},{},false,0)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(indic_scalar_run(SfntIndicProfile::kannada,plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,3})==SfntStatus::ok);const u64 run_work=result.lookup_operations;
    for(u64 work=0;work<=run_work;++work){const auto status=indic_scalar_run(SfntIndicProfile::kannada,plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,3},{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==run_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    print("PASS Kannada three-part vowels, reph/pref/below stages, controls, mark advances, sources, cross-cluster positioning, UTF-8 runs and bounded failures\n");return true;
}
