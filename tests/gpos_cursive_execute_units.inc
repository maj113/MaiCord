u64 long_cursive_scan=0,long_cursive_edges=0,cursive_unresolved_failures=0;u32 long_chain_words=0;
bool cursive_units() noexcept {
    u8 raw[48]{};put(raw,0,1);put(raw,2,14);put(raw,4,2);put(raw,6,22);put(raw,8,28);put(raw,10,34);put(raw,12,40);
    put(raw,14,1);put(raw,16,2);put(raw,18,1);put(raw,20,2);
    const i16 xy[]{10,20,530,90,40,-30,420,180};for(u32 i=0;i<4;++i){put(raw,22+i*6,1);put(raw,24+i*6,static_cast<u16>(xy[i*2]));put(raw,26+i*6,static_cast<u16>(xy[i*2+1]));}
    SfntGposAttachment sub{};SfntGposAttachmentLookup lookup{{&sub,1},{{},1}};SfntCursiveExecution result{};if(sfnt_gpos_attachment_open({raw,46},3,3,&sub)!=SfntStatus::ok)return false;
    SfntGsubGlyph glyphs[]{{1,SfntGlyphClass::base,0},{2,SfntGlyphClass::base,0}};double nominal[]{600,600};u8 flags[]{2,2};SfntPosition p[2],initial[2];u64 bits=0;
    bool succeeded=false;
    for(u32 budget=0;budget<100;++budget){if(sfnt_position_initialize({nominal,2},{flags,2},false,{p,2})!=SfntStatus::ok)return false;p[1].ascender_offset=47;copy_bytes(initial,p,sizeof(p));
        const auto s=sfnt_position_execute_cursive_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{p,2},0,2,false,{}, {&bits,1},&result,budget);++budget_checks;
        if(s==SfntStatus::ok){if(result.matches!=1 || result.scan_steps!=4 || p[0].advance!=490 || p[0].ascender_offset!=-73 || p[1].ascender_offset!=47)return false;succeeded=true;break;}
        if(s!=SfntStatus::resource_limit || result.operations>budget || result.matches>1)return false;
        if(!result.matches && !bytes_equal(p,initial,sizeof(p)))return false;
        if(result.matches && p[0].ascender_offset==-120)++cursive_unresolved_failures;
    }
    if(!succeeded || !cursive_unresolved_failures)return false;
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;DWORD old=0;
    if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}auto* chain=reinterpret_cast<u64*>(memory+page-65*sizeof(u64));
    bool good=true;
    for(u32 backward=0;backward<2 && good;++backward)for(u32 rtl=0;rtl<2 && good;++rtl){lookup.filter.flags=static_cast<u16>(backward);
        for(u32 i=0;i<4097;++i){long_marks[i]=glyphs[i%2];long_advances[i]=600;long_flags[i]=2;}for(u32 i=0;i<65;++i)chain[i]=0xaaaaaaaaaaaaaaaaull;
        if(sfnt_position_initialize({long_advances,4097},{long_flags,4097},rtl!=0,{long_positions,4097})!=SfntStatus::ok){good=false;break;}
        if(backward){const auto s=sfnt_position_execute_cursive_lookup(lookup,{},sfnt_glyph_view({long_marks,4097}),{long_advances,4097},{long_positions,4097},0,4097,rtl!=0,{}, {chain,64},&result);if(s!=SfntStatus::output_too_small){good=false;break;}
            for(u32 i=0;i<4097;++i)if(long_positions[i].advance!=600 || long_positions[i].advance_offset || long_positions[i].ascender_offset)good=false;}
        if(sfnt_position_execute_cursive_lookup(lookup,{},sfnt_glyph_view({long_marks,4097}),{long_advances,4097},{long_positions,4097},0,4097,rtl!=0,{}, {chain,backward?65u:0u},&result,30000)!=SfntStatus::ok){good=false;break;}
        if(result.matches!=4096 || result.scan_steps!=(backward?8194u:4097u) || result.chain_words!=(backward?65u:0u)){good=false;break;}
        for(u32 i=0;i<4097;++i){const double prefix=static_cast<double>(i/2)*280+(i%2?120:0),y=backward?prefix-573440:prefix;
            const double advance=rtl?(i?(i%2?-490:-410):600):(i==4096?600:(i%2?410:490));const double offset=rtl && i?advance-600:0;
            if(long_positions[i].advance!=advance || long_positions[i].advance_offset!=offset || long_positions[i].ascender_offset!=y)good=false;}
        if(backward){for(u32 i=0;i<64;++i)if(chain[i]!=0xffffffffffffffffull)good=false;if(chain[64])good=false;}
        long_cursive_scan=result.scan_steps;long_cursive_edges=result.matches;long_chain_words=result.chain_words;
    }
    // Reusing scratch with broken chains must clear stale connection bits.
    put(raw,12,0);lookup.filter.flags=1;
    if(good && (sfnt_gpos_attachment_open({raw,46},3,3,&sub)!=SfntStatus::ok || sfnt_position_initialize({long_advances,4097},{long_flags,4097},false,{long_positions,4097})!=SfntStatus::ok || sfnt_position_execute_cursive_lookup(lookup,{},sfnt_glyph_view({long_marks,4097}),{long_advances,4097},{long_positions,4097},0,4097,false,{}, {chain,65},&result,30000)!=SfntStatus::ok || result.matches!=2048))good=false;
    if(good){for(u32 i=0;i<64;++i)if(chain[i]!=0x5555555555555555ull)good=false;if(chain[64])good=false;
        for(u32 i=0;i<4097;++i)if(long_positions[i].ascender_offset!=(i<4096 && !(i%2)?-120:0))good=false;}
    VirtualFree(memory,0,MEM_RELEASE);if(!good)return false;
    // A failing second-anchor point resolution leaves the connection unapplied.
    put(raw,34,2);put(raw,40,9);lookup.filter.flags=0;
    if(sfnt_gpos_attachment_open({raw,42},3,3,&sub)!=SfntStatus::ok || sfnt_position_initialize({nominal,2},{flags,2},false,{p,2})!=SfntStatus::ok)return false;copy_bytes(initial,p,sizeof(p));
    if(sfnt_position_execute_cursive_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{p,2},0,2,false,{}, {},&result)!=SfntStatus::unsupported || result.matches || !bytes_equal(p,initial,sizeof(p)))return false;
    PointProbe probe{0,0,0,true};if(sfnt_position_execute_cursive_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{p,2},0,2,false,{}, {},&result,1000,resolve_test_point,&probe)!=SfntStatus::malformed || result.matches || !bytes_equal(p,initial,sizeof(p)))return false;
    probe.fail=false;return sfnt_position_execute_cursive_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{p,2},0,2,false,{}, {},&result,1000,resolve_test_point,&probe)==SfntStatus::ok && probe.calls==2 && probe.glyph==2 && probe.point==9 && p[0].advance==175 && p[1].ascender_offset==-687;
}
