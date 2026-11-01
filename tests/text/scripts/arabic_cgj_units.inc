bool cgj_storage_checks(SfntShapePlan plan,const SfntCmap& cmap,SfntShapeStorage storage) noexcept {
    // Font substitutions are absent here: exercise preparation, sparse source
    // ownership, failure publication and scratch reuse independently of GSUB.
    plan.substitution_roots={};plan.positioning_roots={};
    const u32 scalars[]{0x628,0x34f,0x628},sources[]{0,4,8},boundaries[]{0,4,8};SfntClusterGlyph input[3];
    for(u32 i=0;i<3;++i){u16 glyph=0;if(sfnt_cmap_glyph(cmap,scalars[i],0,&glyph)!=SfntStatus::ok)return false;
        input[i]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),0},sources[i]};}
    bool rtl=true;u16 space=0;if(sfnt_cmap_glyph(cmap,32,0,&space)!=SfntStatus::ok)return false;
    const auto shape=[&](SfntShapeStorage s,SfntShapeRunResult* result,u32 work=1000000u) noexcept {
        copy_bytes(s.targets.data,boundaries,sizeof(boundaries));
        return sfnt_arabic_shape_run(plan,cmap,{input,3},{scalars,3},10,{s.targets.data,3},rtl,14,s,result,{}, {1000000,100000,work});};
    for(u32 direction=0;direction<2;++direction){rtl=direction==0;
    SfntShapeRunResult result{};SfntPlacement expected[2]{};
    if(shape(storage,&result)!=SfntStatus::ok||result.glyph_count!=2||result.source_units!=10)return false;
    copy_bytes(expected,storage.placements.data,sizeof(expected));
    const auto exact=[&]() noexcept {if(result.glyph_count!=2||result.source_units!=10||!bytes_equal(expected,storage.placements.data,sizeof(expected)))return false;
        for(u32 i=0;i<2;++i)if(storage.glyphs[i].glyph.glyph!=(!rtl&&i?space:input[i*2].glyph.glyph))return false;
        for(u32 i=0;i<10;++i)if(storage.character_glyphs[i]!=(i<8?0u:1u))return false;return true;};
    bool completed=false;
    for(u32 work=0;work<=512;++work){result.glyph_count=result.source_units=99;const auto status=shape(storage,&result,work);++budget_checks;
        if(status==SfntStatus::ok){if(!exact())return false;completed=true;break;}
        if(status!=SfntStatus::resource_limit||result.glyph_count||result.source_units)return false;
        if(shape(storage,&result)!=SfntStatus::ok||!exact())return false;}
    if(!completed)return false;
    for(u32 kind=0;kind<3;++kind){auto small=storage;
        if(!kind)small.substitutions.component_journal.count=0;
        else if(kind==1)small.substitutions.replacements.count=sizeof(input)-1;
        else small.components.count=2;
        result.glyph_count=result.source_units=99;++budget_checks;
        if(shape(small,&result)!=SfntStatus::output_too_small||result.glyph_count||result.source_units)return false;
        if(shape(storage,&result)!=SfntStatus::ok||!exact())return false;}
    for(u32 i=0;i<3;++i)if(input[i].source!=sources[i])return false;
    }
    return true;
}
