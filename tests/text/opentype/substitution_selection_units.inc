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
    put(24,1);
    if(sfnt_nominal_participation_add(layout,4,{&bitmap,1},{required,2},&participation)!=SfntStatus::ok||required[0]!=2)return false;
    put(24,65535);byte_store_be32(bytes+34,sfnt_tag('s','a','l','t'));
    if(sfnt_nominal_participation_add(layout,4,{&bitmap,1},{required,2},&participation)!=SfntStatus::ok||required[0])return false;
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
        if(sfnt_layout_variation_select(layout,{&coordinate,1},&variations,&record)!=SfntStatus::ok)return false;
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
    return sfnt_lookup_selection(layout,{queue,1},{&bits,1},{queue,3},&selection)==SfntStatus::ok&&bits==3&&selection.lookups==2;
}
