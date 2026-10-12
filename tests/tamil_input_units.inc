bool tamil_shape_units() noexcept {
    // First-party cmap: ka=1, halant=3, e=4, aa=5, au-length=6,
    // circle=7, space=8, independent AU=9. No platform font or shaper.
    const u32 words[]{0x000c0000,124,0,9,0x20,0x20,8,0xb94,0xb94,9,0xb95,0xb95,1,0xbb7,0xbb7,2,
        0xbbe,0xbbe,5,0xbc6,0xbc6,4,0xbcd,0xbcd,3,0xbd7,0xbd7,6,0x25cc,0x25cc,7};
    u8 bytes[124]{};for(u32 i=0;i<31;++i)byte_store_be32(bytes+i*4,words[i]);
    const SfntCmap cmap{{bytes,124},{},16,12};SfntShapePlan plan{};plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* value) noexcept {*value=1000;return SfntStatus::ok;};
    SfntComponentGlyph glyphs[8]{};SfntComponentNode nodes[16]{};u64 bits[1]{};
    u32 characters[8]{},targets[8]{};SfntMarkAssociation associations[8]{};double advances[8]{};
    u8 flags[8]{};SfntPosition positions[8]{};SfntPlacement placements[8]{};
    SfntShapeStorage storage{{glyphs,8},{nodes,16},{bits,1},{characters,8},{targets,8},{associations,8},
        {advances,8},{flags,8},{positions,8},{placements,8},{},{}};SfntShapeRunResult result{};
    const u32 split[]{0xb95,0xbca},chain[]{0xb95,0xbcd,0xb95,0xbca},orphan[]{0xbca},independent[]{0xb94};
    CHECK(sfnt_tamil_shape_run(plan,cmap,{split,2},1000,storage,&result,{})==SfntStatus::ok&&result.glyph_count==3);
    CHECK(glyphs[0].glyph.glyph==4&&glyphs[1].glyph.glyph==1&&glyphs[2].glyph.glyph==5&&!characters[0]&&!characters[1]);
    CHECK(sfnt_tamil_shape_run(plan,cmap,{chain,4},1000,storage,&result,{})==SfntStatus::ok&&result.glyph_count==5);
    const u16 expected[]{1,3,4,1,5};for(u32 i=0;i<5;++i)CHECK(glyphs[i].glyph.glyph==expected[i]);
    CHECK(!characters[0]&&!characters[1]&&characters[2]==2&&characters[3]==2);
    const u64 operations=result.lookup_operations;
    for(u64 work=0;work<=operations;++work){
        const auto status=sfnt_tamil_shape_run(plan,cmap,{chain,4},1000,storage,&result,{}, {work,100000,1000000});
        CHECK(result.lookup_operations<=work&&status==(work==operations?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units&&!result.target_count);}
    CHECK(sfnt_tamil_shape_run(plan,cmap,{orphan,1},1000,storage,&result,{})==SfntStatus::ok&&result.glyph_count==3);
    CHECK(glyphs[0].glyph.glyph==4&&glyphs[1].glyph.glyph==7&&glyphs[2].glyph.glyph==5&&!characters[0]);
    CHECK(sfnt_tamil_shape_run(plan,cmap,{independent,1},1000,storage,&result,{})==SfntStatus::ok&&result.glyph_count==1&&glyphs[0].glyph.glyph==9);
    const u32 joiner[]{0xb95,0xbcd,0x200d},nonjoiner[]{0xb95,0xbcd,0x200c};
    CHECK(sfnt_tamil_shape_run(plan,cmap,{joiner,3},1000,storage,&result,{})==SfntStatus::ok&&result.glyph_count==4);
    CHECK(glyphs[2].glyph.glyph==7&&glyphs[3].glyph.glyph==8&&characters[2]==2&&placements[3].advance==0);
    CHECK(sfnt_tamil_shape_run(plan,cmap,{nonjoiner,3},1000,storage,&result,{})==SfntStatus::ok&&result.glyph_count==3&&characters[2]==2&&placements[2].advance==0);
    auto small=storage;small.glyphs.count=1;
    CHECK(sfnt_tamil_shape_run(plan,cmap,{split,2},1000,small,&result,{})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
    CHECK(sfnt_tamil_shape_run(plan,cmap,{split,2},1000,storage,&result,{}, {1000000,100000,0})==SfntStatus::resource_limit&&!result.glyph_count);
    const u32 invalid_spacing_bits[]{0xbf800000u,0x7f800000u,0xff800000u,0x7fc00001u};
    for(u32 encoded_spacing:invalid_spacing_bits){float spacing=0;copy_bytes(&spacing,&encoded_spacing,4);
        CHECK(sfnt_tamil_shape_run(plan,cmap,{split,2},1000,storage,&result,{}, {},spacing)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);}
    const SfntLookupStep masked{0,1,1};
    CHECK(sfnt_tamil_shape_run(plan,cmap,{split,2},1000,storage,&result,{&masked,1})==SfntStatus::unsupported_sequence&&!result.glyph_count);
    const u32 distinct_orphans[]{0xbc6,0xbbe};
    const u32 control_matra[]{0xb95,0xbcd,0x200d,0xbca};
    CHECK(sfnt_tamil_shape_run(plan,cmap,{control_matra,4},1000,storage,&result,{})==SfntStatus::ok&&result.glyph_count==6);
    const u16 joined_expected[]{1,3,4,7,8,5};for(u32 i=0;i<6;++i)CHECK(glyphs[i].glyph.glyph==joined_expected[i]);
    CHECK(!characters[0]&&!characters[1]&&characters[2]==2&&characters[3]==2);
    CHECK(sfnt_tamil_shape_run(plan,cmap,{distinct_orphans,2},1000,storage,&result,{})==SfntStatus::ok&&result.glyph_count==4);
    CHECK(glyphs[0].glyph.glyph==4&&glyphs[1].glyph.glyph==7&&glyphs[2].glyph.glyph==7&&glyphs[3].glyph.glyph==5&&characters[1]==2);
    const u32 multiple_pre[]{0xb95,0xbc6,0xbca};
    CHECK(sfnt_tamil_shape_run(plan,cmap,{multiple_pre,3},1000,storage,&result,{})==SfntStatus::ok&&result.glyph_count==4);
    CHECK(glyphs[0].glyph.glyph==4&&glyphs[1].glyph.glyph==4&&glyphs[2].glyph.glyph==1&&glyphs[3].glyph.glyph==5);
    const u32 supplementary[]{0xb95,0x1f600,0xbca};
    CHECK(sfnt_tamil_shape_run(plan,cmap,{supplementary,3},1000,storage,&result,{})==SfntStatus::ok&&result.source_units==4);
    CHECK(characters[1]==characters[2]&&characters[3]==2);
    const auto utf8_case=[&](ConstSlice<u32> input) noexcept {
        CHECK(sfnt_tamil_shape_run(plan,cmap,input,1000,storage,&result,{})==SfntStatus::ok);
        const auto expected=result;u16 saved_glyphs[8]{};u32 saved_sources[8]{};SfntPlacement saved_placements[8]{};
        CHECK(expected.glyph_count<=8&&expected.source_units<=8);
        for(u32 i=0;i<expected.glyph_count;++i)saved_glyphs[i]=glyphs[i].glyph.glyph;
        copy_bytes(saved_sources,characters,expected.source_units*sizeof(u32));copy_bytes(saved_placements,placements,expected.glyph_count*sizeof(SfntPlacement));
        char text[32]{};u32 length=0;
        for(u32 i=0;i<input.count;++i){char encoded[4];const u8 bytes=utf8_encode(input[i],encoded);CHECK(bytes&&length+bytes<=32);
            copy_bytes(text+length,encoded,bytes);length+=bytes;}
        CHECK(native_tamil_run(plan,cmap,{}, {text,length},1000,storage,&result)==SfntStatus::ok);
        CHECK(result.glyph_count==expected.glyph_count&&result.source_units==expected.source_units&&result.lookup_operations==expected.lookup_operations);
        for(u32 i=0;i<expected.glyph_count;++i)CHECK(saved_glyphs[i]==glyphs[i].glyph.glyph);
        CHECK(bytes_equal(saved_sources,characters,expected.source_units*sizeof(u32))&&bytes_equal(saved_placements,placements,expected.glyph_count*sizeof(SfntPlacement)));
        return true;
    };
    CHECK(utf8_case({split,2})&&utf8_case({control_matra,4})&&utf8_case({distinct_orphans,2})&&utf8_case({supplementary,3})&&utf8_case({}));
    const u8 invalid_utf8[][4]{{0xc0,0xaf,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0x80,0,0,0},{0xe0,0xae,0,0}};
    const u32 invalid_lengths[]{2,3,4,1,2};
    for(u32 i=0;i<5;++i)CHECK(native_tamil_run(plan,cmap,{}, {reinterpret_cast<const char*>(invalid_utf8[i]),invalid_lengths[i]},1000,storage,&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    char encoded[4];const u8 encoded_bytes=utf8_encode(0xb95,encoded);auto no_components=storage;no_components.components={};
    CHECK(native_tamil_run(plan,cmap,{}, {encoded,encoded_bytes},1000,no_components,&result)==SfntStatus::output_too_small&&!result.glyph_count);
    CHECK(native_tamil_run(plan,cmap,{}, {encoded,encoded_bytes},1000,storage,&result,{1000000,100000,2})==SfntStatus::resource_limit&&!result.glyph_count);
    {
        u32 text[80]{};SfntComponentGlyph long_glyphs[128]{};SfntComponentNode long_nodes[128]{};
        u64 long_bits[3]{};u32 long_characters[80]{},long_targets[128]{};SfntMarkAssociation long_associations[128]{};
        double long_advances[128]{};u8 long_flags[128]{};SfntPosition long_positions[128]{};SfntPlacement long_placements[128]{};
        SfntShapeStorage long_storage{{long_glyphs,128},{long_nodes,128},{long_bits,3},{long_characters,80},{long_targets,128},
            {long_associations,128},{long_advances,128},{long_flags,128},{long_positions,128},{long_placements,128},{},{}};
        for(u32 i=0;i<80;++i)text[i]=i%2?0xbcau:0xb95u;
        CHECK(sfnt_tamil_shape_run(plan,cmap,{text,80},1000,long_storage,&result,{})==SfntStatus::ok&&result.glyph_count==120&&result.source_units==80);
        for(u32 i=0;i<40;++i){CHECK(long_characters[i*2]==i*3&&long_characters[i*2+1]==i*3);
            CHECK(long_glyphs[i*3].glyph.glyph==4&&long_glyphs[i*3+1].glyph.glyph==1&&long_glyphs[i*3+2].glyph.glyph==5);}
        // Real first-party GSUB4 fixture, KA/H/SSA -> 11. Keep the conjunct
        // intact after a chain longer than the old local cluster buffer.
        const u16 ligature[]{1,8,1,14,1,1,1,1,4,11,3,3,2};u8 ligature_bytes[26]{};
        for(u32 i=0;i<13;++i){ligature_bytes[i*2]=static_cast<u8>(ligature[i]>>8);ligature_bytes[i*2+1]=static_cast<u8>(ligature[i]);}
        SfntGsubExecutableSubtable subtable{};CHECK(sfnt_gsub_subtable_open({ligature_bytes,26},4,16,&subtable.basic)==SfntStatus::ok);
        SfntGsubExecutableLookup lookup{};lookup.type=4;lookup.subtables={&subtable,1};SfntGsubProgram program{{&lookup,1},{},16};
        SfntGsubExecutionFrame frames[8]{};u32 match_positions[32]{},rebased[32]{},consumed[8]{};
        SfntComponentGlyph replacements[8]{};SfntComponentUndo journal[64]{};
        long_storage.substitutions={{frames,8},{match_positions,32},{rebased,32},{consumed,8},
            {reinterpret_cast<u8*>(replacements),sizeof(replacements)},{journal,64}};
        for(u32 i=0;i<30;++i)text[i]=i%2?0xbcdu:0xb95u;text[30]=0xbb7;text[31]=0xbca;
        const SfntLookupStep step{0,0,2};plan.substitutions=&program;
        CHECK(sfnt_tamil_shape_run(plan,cmap,{text,32},1000,long_storage,&result,{&step,1})==SfntStatus::ok&&result.glyph_count==31&&result.substitution_edits==1);
        CHECK(long_glyphs[28].glyph.glyph==4&&long_glyphs[29].glyph.glyph==11&&long_glyphs[30].glyph.glyph==5);
        for(u32 i=28;i<32;++i)CHECK(long_characters[i]==28);
        long_storage.character_glyphs.count=79;
        CHECK(sfnt_tamil_shape_run(plan,cmap,{text,80},1000,long_storage,&result,{})==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
        long_storage.character_glyphs.count=80;text[79]=0x110000;
        CHECK(sfnt_tamil_shape_run(plan,cmap,{text,80},1000,long_storage,&result,{})==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    }
    return true;
}
bool tamil_input_units() noexcept {
    CHECK(tamil_shape_units());
    const u32 input[]{0xb95,0xbca,0xbcb,0xbcc,0xb94,0xb92,0xbd7,0x1f600,0xbca};
    const SfntTamilInputScalar expected[]{{0xb95,0},{0xbc6,1},{0xbbe,1},{0xbc7,2},{0xbbe,2},
        {0xbc6,3},{0xbd7,3},{0xb94,4},{0xb92,5},{0xbd7,6},{0x1f600,7},{0xbc6,9},{0xbbe,9}};
    SfntTamilInputScalar output[13]{};SfntTamilInputResult result{};
    CHECK(sfnt_tamil_expand_cluster({input,9},{output,13},&result,27)==SfntStatus::ok);
    CHECK(result.scalar_count==13&&result.source_units==10&&bytes_equal(output,expected,sizeof(expected)));
    for(u64 work=0;work<27;++work){for(auto& value:output)value={0xffffffffu,0xffffffffu};
        CHECK(sfnt_tamil_expand_cluster({input,9},{output,13},&result,work)==SfntStatus::resource_limit);
        CHECK(!result.scalar_count&&!result.source_units);
        for(auto value:output)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);}
    CHECK(sfnt_tamil_expand_cluster({input,9},{output,12},&result)==SfntStatus::output_too_small);
    CHECK(result.scalar_count==13&&result.source_units==10);
    for(auto value:output)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);
    const u32 invalid[]{0xbca,0xd800},overflow[]{0xbca,0x110000};
    CHECK(sfnt_tamil_expand_cluster({invalid,2},{output,13},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_tamil_expand_cluster({overflow,2},{output,13},&result)==SfntStatus::invalid_input);
    for(auto value:output)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);
    CHECK(!result.scalar_count&&!result.source_units);
    CHECK(sfnt_tamil_expand_cluster({nullptr,1},{output,13},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_tamil_expand_cluster({input,9},{nullptr,13},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_tamil_expand_cluster({input,9},{output,13},nullptr)==SfntStatus::invalid_input);
    CHECK(sfnt_tamil_expand_cluster({reinterpret_cast<const u32*>(output),1},{output,13},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_tamil_expand_cluster({},{},&result,0)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    // A sizing pass shares the same validity/work contract without a temporary.
    CHECK(sfnt_tamil_expand_cluster({input,9},{},&result,27)==SfntStatus::output_too_small&&result.scalar_count==13);
    print("PASS Tamil split-vowel input, source provenance, capacity and work limits\n");return true;
}
