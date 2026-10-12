bool substitution_selection_units() noexcept {
    u8 bytes[104]{};
    const auto put=[&](u32 at,u16 value) noexcept {bytes[at]=static_cast<u8>(value>>8);bytes[at+1]=static_cast<u8>(value);};
    put(0,1);put(4,10);put(6,32);put(8,58);put(10,1);byte_store_be32(bytes+12,sfnt_tag('D','F','L','T'));put(16,8);
    put(18,4);put(24,65535);put(26,2);put(28,0);put(30,1);
    put(32,2);byte_store_be32(bytes+34,sfnt_tag('c','c','m','p'));put(38,14);byte_store_be32(bytes+40,sfnt_tag('s','s','0','1'));put(44,20);
    put(48,1);put(50,0);put(54,1);put(56,1);put(58,2);put(60,6);put(62,26);
    for(u32 i=0;i<2;++i){const u32 at=64+i*20;put(at,1);put(at+4,1);put(at+6,8);put(at+8,1);put(at+10,6);put(at+12,1);put(at+14,1);put(at+16,1);put(at+18,static_cast<u16>(i+1));}
    SfntLayout layout{};if(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::substitution,&layout)!=SfntStatus::ok)return false;
    Arena arena{};if(!arena_initialize(&arena,65536,0,4096))return false;
    struct Release {Arena& arena;~Release() noexcept {arena_release(&arena);}} release{arena};
    u64 bitmap=0,required[2]{};SfntNominalParticipationResult participation{};
    if(sfnt_nominal_participation_add(layout,4,{&bitmap,1},{required,2},&participation)!=SfntStatus::ok)return false;
    ConstSlice<u64> selected{};bool reached=false;
    for(u32 work=0;work<256;++work){arena_reset(&arena);const auto status=native_substitution_selection(layout,{},{required,1},arena,&selected,work);
        if(status==SfntStatus::ok){if(selected.count!=1||selected[0]!=1)return false;reached=true;break;}
        if(status!=SfntStatus::resource_limit||selected.count)return false;}
    if(!reached)return false;
    SfntGsubProgramRequirements need{};if(sfnt_gsub_program_requirements(layout,&need,1000000,selected)!=SfntStatus::ok||need.lookups!=2||need.subtables!=1)return false;
    SfntGsubExecutableLookup lookups[2];SfntGsubExecutableSubtable subtable;SfntGsubProgram program{};
    if(sfnt_gsub_program_prepare(layout,{},4,{{lookups,2},{&subtable,1},{},{}},&program,1000000,1000000,false,false,selected)!=SfntStatus::ok||lookups[0].type!=1||lookups[1].type)return false;
    SfntGsubGlyph glyph{2,SfntGlyphClass::base,0};SfntGlyphBuffer buffer{};SfntGsubExecutionResult result{};
    if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(&glyph),sizeof(glyph)},sizeof(glyph),1,&buffer)!=SfntStatus::ok||
       sfnt_gsub_execute_lookup(program,1,&buffer,0,1,0,{}, {100,100},&result)!=SfntStatus::invalid_input||result.operations)return false;
    SfntGsubExecutableLookup extended_lookups[2];SfntGsubExecutableSubtable added{};SfntGsubProgram extended{};const u64 both=3;
    const auto saved_lookup=lookups[0];SfntGsubExecutableSubtable saved_payload;copy_bytes(&saved_payload,&subtable,sizeof(subtable));
    if(sfnt_gsub_program_requirements(layout,&need,1000000,{&both,1},&program)!=SfntStatus::ok||need.lookups!=2||need.subtables!=1||need.coverages||need.classes)return false;
    const SfntGsubProgramStorage addition{{extended_lookups,2},{&added,1},{},{}};
    if(sfnt_gsub_program_prepare(layout,{},4,addition,&extended,1000000,1000000,false,false,{&both,1},&program)!=SfntStatus::ok||
       extended.lookups[0].subtables.data!=&subtable||extended.lookups[1].subtables.data!=&added||!bytes_equal(&saved_lookup,lookups,sizeof(saved_lookup))||!bytes_equal(&saved_payload,&subtable,sizeof(subtable)))return false;
    u32 source_index=0;SfntGsubGlyph replacement{};SfntGsubExecutionScratch execution{};execution.consumed={&source_index,1};execution.replacements={reinterpret_cast<u8*>(&replacement),sizeof(replacement)};
    for(u16 index=0;index<2;++index){glyph={static_cast<u16>(index+1),SfntGlyphClass::base,0};
        if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(&glyph),sizeof(glyph)},sizeof(glyph),1,&buffer)!=SfntStatus::ok||
           sfnt_gsub_execute_lookup(extended,index,&buffer,0,1,0,execution,{100,100},&result)!=SfntStatus::ok||glyph.glyph!=index+2)return false;}
    auto too_small=addition;too_small.subtables.count=0;
    if(sfnt_gsub_program_prepare(layout,{},4,too_small,&extended,1000000,1000000,false,false,{&both,1},&program)!=SfntStatus::output_too_small||extended.lookups.count||
       !bytes_equal(&saved_lookup,lookups,sizeof(saved_lookup))||!bytes_equal(&saved_payload,&subtable,sizeof(subtable)))return false;
    const SfntGsubProgramStorage in_place{{lookups,2},{&added,1},{},{}};
    if(sfnt_gsub_program_prepare(layout,{},4,in_place,&extended,1000000,1000000,false,false,{&both,1},&program)!=SfntStatus::ok||
       lookups[0].subtables.data!=&subtable||lookups[1].subtables.data!=&added||!bytes_equal(&saved_payload,&subtable,sizeof(subtable)))return false;
    const SfntGsubExecutableLookup stable[]{lookups[0],lookups[1]};const u64 first_only=1;
    if(sfnt_gsub_program_prepare(layout,{},4,in_place,&extended,1000000,1000000,false,false,{&first_only,1},&program)!=SfntStatus::invalid_input||extended.lookups.count||!bytes_equal(stable,lookups,sizeof(stable)))return false;
    put(24,1);arena_reset(&arena);
    if(sfnt_nominal_participation_add(layout,4,{&bitmap,1},{required,2},&participation)!=SfntStatus::ok||required[0]!=2||
       native_substitution_selection(layout,{},{required,1},arena,&selected,1000000)!=SfntStatus::ok||selected.count)return false;
    put(24,65535);byte_store_be32(bytes+34,sfnt_tag('s','a','l','t'));arena_reset(&arena);
    if(sfnt_nominal_participation_add(layout,4,{&bitmap,1},{required,2},&participation)!=SfntStatus::ok||required[0]||
       native_substitution_selection(layout,{},{required,1},arena,&selected,1000000)!=SfntStatus::ok||selected.count!=1||selected[0])return false;
    byte_store_be32(bytes+34,sfnt_tag('c','c','m','p'));
    u8 variable[156]{};copy_bytes(variable+14,bytes+10,94);
    const auto vary=[&](u32 at,u16 value) noexcept {variable[at]=static_cast<u8>(value>>8);variable[at+1]=static_cast<u8>(value);};
    vary(0,1);vary(2,1);vary(4,14);vary(6,36);vary(8,62);byte_store_be32(variable+10,108);
    vary(108,1);byte_store_be32(variable+112,1);byte_store_be32(variable+116,16);byte_store_be32(variable+120,30);
    vary(124,1);byte_store_be32(variable+126,6);vary(130,1);vary(134,8192);vary(136,16384);
    vary(138,1);vary(142,1);byte_store_be32(variable+146,12);vary(152,1);vary(154,1);
    if(sfnt_layout_open({variable,sizeof(variable)},SfntLayoutKind::substitution,&layout)!=SfntStatus::ok||
       sfnt_nominal_participation_add(layout,4,{&bitmap,1},{required,2},&participation)!=SfntStatus::ok)return false;
    Arena state{};if(!arena_initialize(&state,65536,0,4096))return false;Release release_state{state};
    const i16 coordinates[]{0,8191,8192,16384};for(i16 coordinate:coordinates){arena_reset(&arena);arena_reset(&state);
        const u16 expected=coordinate>=8192?1u:0u;
        SfntFeatureSubstitutions variations{};u32 record=0;
        if(sfnt_layout_variation_select(layout,{&coordinate,1},&variations,&record)!=SfntStatus::ok||
           native_substitution_selection(layout,variations,{required,1},arena,&selected,1000000)!=SfntStatus::ok||selected.count!=1||selected[0]!=(1ull<<expected))return false;
        u64 active_bitmap=0,active_scratch[2]{};SfntNominalParticipationResult active{};
        if(sfnt_nominal_participation_add(layout,4,{&active_bitmap,1},{active_scratch,2},&active,1000000,{},nullptr,variations)!=SfntStatus::ok||
           active_bitmap!=(1ull<<(expected+1)))return false;
        for(u32 mode=0;mode<2;++mode){ConstSlice<u16> roots{};
            if(native_lookup_roots_prepare(layout,variations,sfnt_tag('l','a','t','n'),mode!=0,state,arena,roots,1000000)!=SfntStatus::ok||
               roots.count!=1||roots[0]!=expected)return false;}}
    return true;
}
bool substitution_cycle_selection_units() noexcept {
    u8 bytes[100]{};const auto put=[&](u32 at,u16 value) noexcept {bytes[at]=static_cast<u8>(value>>8);bytes[at+1]=static_cast<u8>(value);};
    put(0,1);put(4,10);put(6,12);put(8,14);put(14,3);
    for(u32 i=0;i<3;++i){put(16+i*2,static_cast<u16>(8+i*26));const u32 at=22+i*26;
        put(at,5);put(at+4,1);put(at+6,8);put(at+8,3);put(at+10,1);put(at+12,1);put(at+14,12);put(at+18,static_cast<u16>(i==0?1:i==1?0:2));put(at+20,1);put(at+22,1);put(at+24,1);}
    SfntLayout layout{};if(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::substitution,&layout)!=SfntStatus::ok)return false;
    u16 queue[3]{0};u64 bits=0;SfntLookupSelectionResult selection{};
    if(sfnt_lookup_selection(layout,{queue,1},{&bits,1},{queue,3},&selection)!=SfntStatus::ok||bits!=3||selection.lookups!=2)return false;
    SfntGsubExecutableLookup lookups[3];SfntGsubExecutableSubtable subtables[2];SfntCoverage coverage[2];SfntGsubProgram program{};
    SfntGsubProgramStorage storage{{lookups,3},{subtables,2},{coverage,2},{}};
    if(sfnt_gsub_program_prepare(layout,{},2,storage,&program,1000000,1000000,false,false,{&bits,1})!=SfntStatus::ok||lookups[2].type)return false;
    SfntGsubExecutableLookup inherited_lookups[3];SfntGsubProgram borrowed{};SfntGsubProgramRequirements reused{};
    if(sfnt_gsub_program_requirements(layout,&reused,1000000,{&bits,1},&program)!=SfntStatus::ok||reused.subtables||reused.coverages||reused.classes)return false;
    const SfntGsubProgramStorage borrowed_storage{{inherited_lookups,3},{},{},{}};
    if(sfnt_gsub_program_prepare(layout,{},2,borrowed_storage,&borrowed,0,1000000,false,false,{&bits,1},&program)!=SfntStatus::ok||inherited_lookups[2].type)return false;
    bool completed=false;
    for(u32 work=0;work<256;++work){const auto status=sfnt_gsub_program_prepare(layout,{},2,borrowed_storage,&borrowed,0,work,false,false,{&bits,1},&program);
        if(status==SfntStatus::ok){completed=true;break;}if(status!=SfntStatus::resource_limit||borrowed.lookups.count)return false;}
    if(!completed)return false;
    bits=1;if(sfnt_gsub_program_prepare(layout,{},2,borrowed_storage,&borrowed,0,1000000,false,false,{&bits,1},&program)!=SfntStatus::invalid_input||borrowed.lookups.count)return false;
    bits=1;if(sfnt_gsub_program_prepare(layout,{},2,storage,&program,1000000,1000000,false,false,{&bits,1})!=SfntStatus::invalid_input||program.lookups.count)return false;
    return true;
}
