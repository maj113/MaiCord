// First-party GSUB: empty script/feature lists, one type-1 identity lookup,
// coverage {65535}. MingLiU-ExtB contains the same unreachable sentinel pattern.
bool unreachable_units() noexcept {
    u8 bytes[]{0,1,0,0,0,10,0,12,0,14,0,0,0,0,0,1,0,4,
        0,1,0,0,0,1,0,8,0,1,0,6,0,0,0,1,0,1,255,255};
    SfntLayout layout{};SfntGsubProgram program{};
    if(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::substitution,&layout)!=SfntStatus::ok)return false;
    const auto build=[&](bool compatible,u32 work=1000000) noexcept {
        return prepare_gsub_program(layout,{},64,storage(),&program,false,compatible,work);};
    if(build(false)!=SfntStatus::malformed||program.lookups.count)return false;
    bool prepared=false;
    for(u32 work=0;work<32;++work){++budget_checks;const auto status=build(true,work);
        if(status==SfntStatus::ok){prepared=true;break;}
        if(status!=SfntStatus::resource_limit||program.lookups.count)return false;}
    if(!prepared||program.glyph_limit!=64||program.lookups.count!=1||
        program.lookups[0].subtables[0].basic.coverage.covered_count||!sfnt_lookup_bounds_disjoint(program.lookups[0].bounds,0,63))return false;
    SfntGsubGlyph identity[64];for(u32 i=0;i<64;++i)identity[i]={static_cast<u16>(i),SfntGlyphClass::base,0};
    SfntGsubExecutionResult result{};
    if(!records.open(identity,64,64)||records.execute(program,0,0,64,0,scratch(),{64,0},&result)!=SfntStatus::ok||result.edits||result.operations!=64)return false;
    for(u32 i=0;i<64;++i)if(records.glyph(i)!=i)return false;
    records.glyphs[0].glyph.glyph=64;
    if(records.execute(program,0,0,64,0,scratch(),{64,0},&result)!=SfntStatus::invalid_input)return false;
    // A real input with an invalid output is reachable, and must still fail.
    bytes[36]=0;bytes[37]=63;bytes[31]=1;
    if(build(true)!=SfntStatus::malformed||program.lookups.count)return false;
    // Valid identity substitution retains its coverage and normal execution.
    bytes[31]=0;if(build(true)!=SfntStatus::ok||program.lookups[0].subtables[0].basic.coverage.covered_count!=1)return false;
    identity[0]={63,SfntGlyphClass::base,0};
    if(!records.open(identity,1,64)||records.execute(program,0,0,1,0,scratch(),{1000000,1000000},&result)!=SfntStatus::ok||result.edits!=1||records.glyph(0)!=63)return false;
    // Invalid coverage ordering/format and short payloads are never ignored.
    bytes[33]=3;if(build(true)==SfntStatus::ok||program.lookups.count)return false;bytes[33]=1;
    bytes[36]=bytes[37]=255;
    for(u32 size=0;size<sizeof(bytes);++size){SfntLayout short_layout{};
        if(sfnt_layout_open({bytes,size},SfntLayoutKind::substitution,&short_layout)!=SfntStatus::ok)continue;
        if(prepare_gsub_program(short_layout,{},64,storage(),&program,false,true)==SfntStatus::ok||program.lookups.count)return false;}
    return true;
}
