bool component_bounds_units() noexcept {
    constexpr u32 nil=0xffffffffu;
    SfntComponentGlyph records[4]{{{1,SfntGlyphClass::base,0},9,nil,nil},{{2,SfntGlyphClass::mark,0},22,nil,nil},
        {{3,SfntGlyphClass::base,0},0,nil,nil},{{4,SfntGlyphClass::base,0},15,nil,nil}};
    SfntComponentNode storage[4],saved[4];SfntComponentForest forest{};
    if(sfnt_components_open({records,4},{storage,4},&forest)!=SfntStatus::ok)return false;
    copy_bytes(saved,storage,sizeof(storage));
    for(u32 i=0;i<4;++i)for(u64 budget=0;budget<3;++budget){u32 lo=123,hi=123;u64 used=123;
        if(sfnt_component_source_bounds(forest,i,&lo,&hi,budget,&used)!=(budget?SfntStatus::ok:SfntStatus::resource_limit)||used!=(budget?1u:0u))return false;
        if(budget?(lo!=records[i].source||hi!=lo):(lo!=nil||hi))return false;}
    if(!bytes_equal(saved,storage,sizeof(storage)))return false;
    // The mark at source 22 remains a dormant singleton attached to a base.
    // Joining the three bases must not include that mark's source extent.
    const u32 indices[]{0,2,3};SfntComponentGlyph output=records[0];SfntComponentUndo undo[64];SfntComponentEdit edit{};
    if(sfnt_components_edit(&forest,{&records[0].glyph,nullptr,4,0,sizeof(SfntComponentGlyph)},{indices,3},true,
        {reinterpret_cast<u8*>(&output),sizeof(output)},{undo,64},&edit)!=SfntStatus::ok)return false;
    copy_bytes(saved,storage,sizeof(storage));
    for(u64 budget=0;budget<5;++budget){u32 lo=123,hi=123;u64 used=123;
        if(sfnt_component_source_bounds(forest,output.component,&lo,&hi,budget,&used)!=(budget>=3?SfntStatus::ok:SfntStatus::resource_limit)||used!=minimum<u64>(budget,3))return false;
        if(budget>=3?(lo!=0||hi!=15):(lo!=nil||hi))return false;}
    u32 lo=123,hi=123;u64 used=123;
    if(sfnt_component_source_bounds(forest,1,&lo,&hi,1,&used)!=SfntStatus::ok||lo!=22||hi!=22||used!=1||!bytes_equal(saved,storage,sizeof(storage)))return false;
    if(sfnt_component_source_bounds(forest,nil,&lo,&hi,0,&used)!=SfntStatus::ok||lo!=nil||hi||used)return false;
    if(sfnt_component_source_bounds(forest,4,&lo,&hi,10,&used)!=SfntStatus::invalid_input||lo!=nil||hi||used)return false;
    // Invalid traversal and a cycle still stop within the caller's work bound.
    forest.nodes[output.component].left=4;
    if(sfnt_component_source_bounds(forest,output.component,&lo,&hi,10,&used)!=SfntStatus::invalid_input||!used||used>10||lo!=nil||hi)return false;
    forest.nodes[output.component].left=output.component;forest.nodes[output.component].right=nil;
    if(sfnt_component_source_bounds(forest,output.component,&lo,&hi,7,&used)!=SfntStatus::resource_limit||used!=7||lo!=nil||hi)return false;
    return true;
}
