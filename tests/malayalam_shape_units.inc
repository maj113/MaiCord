bool malayalam_shape_units() noexcept {
    // First-party font: separate reph/pref/below ligatures and deliberately
    // nonzero mark advances expose ordering, stage identity and zeroing errors.
    const u32 entries[][2]{{0x20,8},{0xa0,8},{0x953,24},{0x954,24},{0xd02,31},{0xd05,28},{0xd15,1},{0xd30,4},{0xd31,27},{0xd32,26},{0xd3b,24},
        {0xd3e,25},{0xd3f,29},{0xd43,30},{0xd46,10},{0xd47,19},{0xd4d,3},{0xd4e,20},{0xd57,12},{0xd62,9},{0x25cc,7}};
    constexpr u32 entry_count=sizeof(entries)/sizeof(entries[0]);
    u8 cmap_bytes[16+12*entry_count]{};byte_store_be32(cmap_bytes,0x000c0000);byte_store_be32(cmap_bytes+4,sizeof(cmap_bytes));byte_store_be32(cmap_bytes+12,entry_count);
    for(u32 i=0;i<entry_count;++i){byte_store_be32(cmap_bytes+16+i*12,entries[i][0]);byte_store_be32(cmap_bytes+20+i*12,entries[i][0]);byte_store_be32(cmap_bytes+24+i*12,entries[i][1]);}
    const SfntCmap cmap{{cmap_bytes,sizeof(cmap_bytes)},{},32,12};
    const u16 reph[]{1,8,1,14,1,1,4,1,4,20,2,3},pref[]{1,8,1,14,1,1,3,1,4,22,2,4},below[]{1,8,1,14,1,1,3,1,4,21,2,26};
    u8 bytes[3][24]{};const ConstSlice<u8> tables[]{encode(reph,bytes[0]),encode(pref,bytes[1]),encode(below,bytes[2])};
    SfntGsubExecutableSubtable subtables[3]{};SfntGsubExecutableLookup lookups[3]{};
    for(u32 i=0;i<3;++i){CHECK(sfnt_gsub_subtable_open(tables[i],4,32,&subtables[i].basic)==SfntStatus::ok);lookups[i].type=4;lookups[i].subtables={subtables+i,1};}
    SfntGsubProgram program{{lookups,3},{},32};SfntShapePlan plan{};plan.substitutions=&program;plan.units_per_em=1000;plan.refresh_substitution_classes=true;
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
    u8 forms[128]{};forms[0x30]=sfnt_deva_pre;forms[0x32]=sfnt_deva_below;SfntShapeRunResult result{};
    const auto shape=[&](ConstSlice<u32> input,float spacing=0,bool rtl=false,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return sfnt_malayalam_shape_cluster(plan,cmap,input,{forms,128},1000,storage,&result,{steps,3},limits,false,spacing,rtl);};
    const auto same=[&](ConstSlice<u16> expected) noexcept {
        if(result.glyph_count!=expected.count)return false;
        for(u32 i=0;i<expected.count;++i)if(glyphs[i].glyph.glyph!=expected[i])return false;
        for(u32 i=0;i<result.source_units;++i)if(characters[i])return false;return true;};
    const u32 simple[]{0xd15,0xd4a},pref_text[]{0xd15,0xd4d,0xd30,0xd4a},dot[]{0xd4e,0xd15,0xd4a},orphan[]{0xd4e},
        multiple[]{0xd15,0xd46,0xd4b},below_text[]{0xd15,0xd4d,0xd32,0xd3b,0xd62};
    const u16 simple_glyphs[]{10,1,25},pref_glyphs[]{10,22,1,25},dot_glyphs[]{10,1,20,25},orphan_glyphs[]{7,20},
        multiple_glyphs[]{10,19,1,25},below_glyphs[]{1,21,9,24};
    CHECK(shape({simple,2})==SfntStatus::ok&&same({simple_glyphs,3})&&result.source_units==2);
    CHECK(shape({pref_text,4})==SfntStatus::ok&&same({pref_glyphs,4}));
    const u8 wrong_stages[]{4,6};for(u8 stage:wrong_stages){steps[1].stage=stage;const u16 unmoved[]{10,1,22,25};
        CHECK(shape({pref_text,4})==SfntStatus::ok&&same({unmoved,4}));}steps[1].stage=5;
    CHECK(shape({dot,3})==SfntStatus::ok&&same({dot_glyphs,4}));
    CHECK(shape({orphan,1})==SfntStatus::ok&&same({orphan_glyphs,2}));
    CHECK(shape({multiple,3})==SfntStatus::ok&&same({multiple_glyphs,4}));
    const u32 dot_halant[]{0xd4e,0xd15,0xd4d},dot_control[]{0xd4e,0xd15,0x200c};
    const u16 dot_halant_glyphs[]{1,3,20},dot_control_glyphs[]{1,8,20};
    CHECK(shape({dot_halant,3})==SfntStatus::ok&&same({dot_halant_glyphs,3}));
    CHECK(shape({dot_control,3})==SfntStatus::ok&&same({dot_control_glyphs,3})&&placements[1].advance==0);
    const u32 joined_orphan[]{0x200d,0xd4e},placeholder_orphan[]{0xa0,0xd4e},placeholder_joined_orphan[]{0xa0,0x200d,0xd4e};
    const u16 joined_orphan_glyphs[]{8,20,7},placeholder_joined_orphan_glyphs[]{8,8,20,7};
    CHECK(shape({joined_orphan,2})==SfntStatus::ok&&same({joined_orphan_glyphs,3})&&placements[0].advance==0);
    CHECK(shape({placeholder_orphan,2})==SfntStatus::ok&&same({joined_orphan_glyphs,3})&&placements[0].advance==100);
    CHECK(shape({placeholder_joined_orphan,3})==SfntStatus::ok&&same({placeholder_joined_orphan_glyphs,4})&&placements[1].advance==0);
    const u32 joined_main[]{0x200d,0xd4e,0xd15,0xd46},placeholder_main[]{0xa0,0xd4e,0xd15,0xd46};
    const u16 joined_main_glyphs[]{10,8,20,1,7},placeholder_main_glyphs[]{20,10,1,8};
    CHECK(shape({joined_main,4})==SfntStatus::ok&&same({joined_main_glyphs,5}));
    CHECK(shape({placeholder_main,4})==SfntStatus::ok&&same({placeholder_main_glyphs,4}));
    class_bytes[21]=1; // Reph's nominal glyph may have a base GDEF class.
    CHECK(shape({joined_main,4})==SfntStatus::ok&&same({joined_main_glyphs,5}));
    const u16 nominal_reph_main[]{10,20,1,8};
    CHECK(shape({placeholder_main,4})==SfntStatus::ok&&same({nominal_reph_main,4}));
    class_bytes[21]=3;

    const u32 joined_post[]{0x200d,0xd4e,0xd30},joined_post_left[]{0x200d,0xd4e,0xd30,0xd46},reph_post_control[]{0xd4e,0x25cc,0x200d,0xd30};
    const u16 actual_post[]{8,7,20,4},actual_post_left[]{7,8,10,20,4},reph_post_control_glyphs[]{7,20,8,4};
    forms[0x30]=sfnt_deva_post;
    CHECK(shape({joined_post,3})==SfntStatus::ok&&same({actual_post,4}));
    CHECK(shape({joined_post_left,4})==SfntStatus::ok&&same({actual_post_left,5}));
    CHECK(shape({reph_post_control,4})==SfntStatus::ok&&same({reph_post_control_glyphs,4})&&placements[2].advance==0);
    const u16 default_post[]{8,20,7,4},default_post_left[]{7,8,20,10,4};
    forms[0x30]=sfnt_deva_post|indic_default_position;
    CHECK(shape({joined_post,3})==SfntStatus::ok&&same({default_post,4}));
    CHECK(shape({joined_post_left,4})==SfntStatus::ok&&same({default_post_left,5}));
    const u16 default_below[]{8,20,4,7},default_below_left[]{7,8,20,4,10};
    forms[0x30]=sfnt_deva_below|indic_default_position;
    CHECK(shape({joined_post,3})==SfntStatus::ok&&same({default_below,4}));
    CHECK(shape({joined_post_left,4})==SfntStatus::ok&&same({default_below_left,5}));
    forms[0x30]=sfnt_deva_pre;
    const u32 prefix_controls[]{0xa0,0x200c,0x200d};
    const u16 controlled_pre[]{1,22,8},controlled_pre_left[]{1,10,22,8};
    for(u32 control:prefix_controls){const u32 text[]{0xd15,control,0xd4d,0xd30,0xd46};
        CHECK(shape({text,4})==SfntStatus::ok&&same({controlled_pre,3}));
        CHECK(shape({text,5})==SfntStatus::ok&&same({controlled_pre_left,4}));}
    const u32 direct_post_lower[]{0xd15,0xd4d,0xa0,0xd30,0xd62};const u16 direct_post_lower_glyphs[]{1,3,8,4,9};
    forms[0x30]=sfnt_deva_post;
    CHECK(shape({direct_post_lower,5})==SfntStatus::ok&&same({direct_post_lower_glyphs,5}));
    const u32 broken_post_left[]{0xd15,0xd4d,0xa0,0xd30,0xd46};const u16 broken_post_left_glyphs[]{1,3,10,8,4};
    forms[0x30]=sfnt_deva_post|indic_default_position;
    CHECK(shape({broken_post_left,5})==SfntStatus::ok&&same({broken_post_left_glyphs,5}));
    forms[0x30]=sfnt_deva_pre;
    const u32 controlled_tail[]{0xd05,0xd3f,0xd4a,0xd4d,0xd43,0xd02,0x200c};
    const u16 controlled_tail_glyphs[]{10,28,29,30,8,3,8,31};
    CHECK(shape({controlled_tail,7})==SfntStatus::ok&&same({controlled_tail_glyphs,8})&&placements[4].advance==0&&placements[6].advance==0);
    const u32 accent_group[]{0xd15,0xd4d,0xd15,0xd4d,0x954};
    const u16 normal_accent[]{1,3,1,3,24},fallback_accent[]{1,3,1,24,3};
    CHECK(shape({accent_group,5})==SfntStatus::ok&&same({normal_accent,5}));
    forms[0x2f]=sfnt_deva_post|indic_default_position;
    CHECK(shape({accent_group,5})==SfntStatus::ok&&same({fallback_accent,5}));
    const u32 circle_accent_group[]{0x200d,0xd4d,0xd15,0xd4d,0x954};const u16 circle_accent_glyphs[]{7,8,3,1,24,3};
    CHECK(shape({circle_accent_group,5})==SfntStatus::ok&&same({circle_accent_glyphs,6}));
    const u32 dot_accent_group[]{0xd4e,0xd15,0xd4d,0x954};const u16 dot_accent_glyphs[]{1,24,3,20};
    CHECK(shape({dot_accent_group,4})==SfntStatus::ok&&same({dot_accent_glyphs,4}));
    forms[0x30]=sfnt_deva_post|indic_default_position;
    const u32 direct_post_accent[]{0xd15,0x200d,0xd30,0xd4d,0x954};const u16 direct_post_accent_glyphs[]{1,8,4,3,24};
    CHECK(shape({direct_post_accent,5})==SfntStatus::ok&&same({direct_post_accent_glyphs,5}));
    const u32 first_post_accent[]{0xd30,0x200d,0xd15,0xd4d,0x954};const u16 first_post_accent_glyphs[]{4,8,1,24,3};
    CHECK(shape({first_post_accent,5})==SfntStatus::ok&&same({first_post_accent_glyphs,5}));
    forms[0x30]=sfnt_deva_pre;forms[0x2f]=0;
    const u32 direct_chain[]{0xd15,0x200d,0xd15,0x200d,0xd15,0x200d,0xd46},pre_chain[]{0xd30,0xd4d,0xd30,0xd4d,0xd30,0xd4d},vowel_main[]{0xd05,0x200c,0xd15,0xd46};
    const u16 direct_chain_glyphs[]{1,10,8,1,8,1,8},pre_chain_glyphs[]{4,22,3,4,3},vowel_main_glyphs[]{10,28,8,1};
    CHECK(shape({direct_chain,7})==SfntStatus::ok&&same({direct_chain_glyphs,7}));
    CHECK(shape({pre_chain,6})==SfntStatus::ok&&same({pre_chain_glyphs,5}));
    CHECK(shape({vowel_main,4})==SfntStatus::ok&&same({vowel_main_glyphs,4}));
    CHECK(shape({below_text,5})==SfntStatus::ok&&same({below_glyphs,4}));
    for(u32 i=0;i<4;++i)CHECK(placements[i].advance==100);
    // A font may form a subjoined glyph only in context. The left vowel uses
    // the preceding main glyph; explicit reph still follows the full group.
    forms[0x32]=0;
    const u32 contextual[]{0xd15,0xd4d,0xd32,0xd46},contextual_reph[]{0xd4e,0xd15,0xd4d,0xd32};
    const u16 contextual_glyphs[]{10,1,21},contextual_reph_glyphs[]{1,21,20};
    CHECK(shape({contextual,4})==SfntStatus::ok&&same({contextual_glyphs,3}));
    CHECK(shape({contextual_reph,4})==SfntStatus::ok&&same({contextual_reph_glyphs,3}));forms[0x32]=sfnt_deva_below;
    // Default positional forms preserve their fallback provenance. Their
    // explicit halant follows the consonant before font substitutions.
    u8 saved_forms[128]{};copy_bytes(saved_forms,forms,128);indic_form_defaults(SfntIndicProfile::malayalam,forms);
    const u16 fallback_pre_glyphs[]{10,1,4,3,25};
    CHECK(shape({pref_text,4})==SfntStatus::ok&&same({fallback_pre_glyphs,5}));
    const u32 fallback_control[]{0xd15,0xd4d,0xd30,0x200d},fallback_main[]{0xd31,0xd4d,0xd30};
    const u16 fallback_control_glyphs[]{1,4,8,3},fallback_main_glyphs[]{27,3,4};
    CHECK(shape({fallback_control,4})==SfntStatus::ok&&same({fallback_control_glyphs,4})&&placements[2].advance==0);
    CHECK(shape({fallback_main,3})==SfntStatus::ok&&same({fallback_main_glyphs,3}));
    copy_bytes(forms,saved_forms,128);
    const bool directions[]{false,true};const u32 control[]{0xd15,0xd4d,0x200c};const u16 control_glyphs[]{1,3,8};
    for(bool rtl:directions){CHECK(shape({pref_text,4},.25f,rtl)==SfntStatus::ok&&same({pref_glyphs,4}));
        CHECK(placements[rtl?0:3].advance==100.25f);
        CHECK(shape({control,3},.25f,rtl)==SfntStatus::ok&&same({control_glyphs,3})&&placements[2].advance==0);}
    CHECK(shape({})==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    const u32 two_clusters[]{0xd15,0xd15},invalid[]{0x110000};
    CHECK(shape({two_clusters,2})==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    CHECK(shape({invalid,1})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(shape({nullptr,1})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(sfnt_malayalam_shape_cluster(plan,cmap,{simple,2},{forms,127},1000,storage,&result,{steps,3})==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(shape({pref_text,4},0,false,{0,100000,1000000})==SfntStatus::resource_limit&&!result.glyph_count&&!result.source_units);
    const auto saved=storage.targets;storage.targets={targets,1};
    CHECK(shape({pref_text,4})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);storage.targets=saved;
    const u16 pair_words[]{1,18,4,0,1,12,1,1,50,1,1,1};u8 pair_bytes[24]{};SfntGposSubtable pair{};
    CHECK(sfnt_gpos_subtable_open(encode(pair_words,pair_bytes),2,32,&pair)==SfntStatus::ok);
    SfntGposExecutableLookup pair_lookup{};pair_lookup.type=2;pair_lookup.tables.basic={&pair,1};
    SfntGposProgram positioning{{&pair_lookup,1},{},32};const u16 root=0;plan.positioning=&positioning;plan.positioning_roots={&root,1};
    SfntGposExecutionFrame position_frames[4]{};u32 position_scratch[40]{};
    storage.positioning.frames={position_frames,4};storage.positioning.positions={position_scratch,40};
    const u32 run_text[]{0xd15,0xd15,0xd15};
    CHECK(sfnt_malayalam_shape_run(plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,3},{},false,.25f)==SfntStatus::ok);
    CHECK(result.glyph_count==3&&result.source_units==3&&result.position_matches==2);
    CHECK(placements[0].advance==150.25f&&placements[1].advance==150.25f&&placements[2].advance==100.25f);
    for(u32 i=0;i<3;++i)CHECK(characters[i]==i);
    const u32 run_controls[]{0x200d,0xd15,0xd4d,0x200d,0xd15};
    for(bool rtl:directions){CHECK(sfnt_malayalam_shape_run(plan,cmap,{run_controls,5},{forms,128},1000,storage,&result,{steps,3},{},rtl,.25f)==SfntStatus::ok);
        CHECK(result.glyph_count==5&&placements[0].advance==0&&placements[3].advance==0);}
    const auto utf8_case=[&](ConstSlice<u32> input) noexcept {
        CHECK(sfnt_malayalam_shape_run(plan,cmap,input,{forms,128},1000,storage,&result,{steps,3})==SfntStatus::ok);
        const auto saved=result;u16 saved_glyphs[40]{};u32 saved_sources[40]{};SfntPlacement saved_placements[40]{};
        CHECK(saved.glyph_count<=40&&saved.source_units<=40);
        for(u32 i=0;i<saved.glyph_count;++i)saved_glyphs[i]=glyphs[i].glyph.glyph;
        copy_bytes(saved_sources,characters,saved.source_units*4);copy_bytes(saved_placements,placements,saved.glyph_count*sizeof(SfntPlacement));
        char text[512]{};u32 bytes_written=0;for(u32 i=0;i<input.count;++i){CHECK(bytes_written+4<=512);bytes_written+=utf8_encode(input[i],text+bytes_written);}
        CHECK(native_malayalam_run(plan,cmap,{text,bytes_written},{forms,128},1000,storage,&result,{steps,3})==SfntStatus::ok);
        CHECK(result.glyph_count==saved.glyph_count&&result.source_units==saved.source_units&&result.lookup_operations==saved.lookup_operations);
        for(u32 i=0;i<saved.glyph_count;++i)CHECK(glyphs[i].glyph.glyph==saved_glyphs[i]);
        CHECK(bytes_equal(saved_sources,characters,saved.source_units*4)&&bytes_equal(saved_placements,placements,saved.glyph_count*sizeof(SfntPlacement)));return true;};
    u32 windowed[40]{};for(u32 i=0;i<40;++i)windowed[i]=0xd15u;
    const u32 supplementary[]{0xd15,0xd4c,0x1f600,0xd15,0xd3e};
    CHECK(utf8_case({run_text,3})&&utf8_case({run_controls,5})&&utf8_case({windowed,40})&&utf8_case({supplementary,5})&&utf8_case({}));
    const u8 malformed[][4]{{0xc0,0xaf,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0xe0,0xac,0,0}};const u32 bad_sizes[]{2,3,4,2};
    for(u32 i=0;i<4;++i)CHECK(native_malayalam_run(plan,cmap,{reinterpret_cast<const char*>(malformed[i]),bad_sizes[i]},{forms,128},1000,storage,&result,{steps,3})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    CHECK(sfnt_malayalam_shape_run(plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,3})==SfntStatus::ok);const u64 run_work=result.lookup_operations;
    for(u64 work=0;work<=run_work;++work){const auto status=sfnt_malayalam_shape_run(plan,cmap,{run_text,3},{forms,128},1000,storage,&result,{steps,3},{work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==run_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    print("PASS Malayalam split/left vowels, pref stage identity, explicit reph, below forms, mark advances, controls, sources and bounded failures\n");return true;
}
