bool gpos_backtrack_dispatch_units() noexcept {
    // Four format-3 rules each own a nearest-backtrack selector. Their action
    // invokes a real single-position lookup, so equivalence covers execution.
    u8 bytes[200]{};SfntGposExecutableLookup lookups[2];SfntContext contexts[4];SfntCoverage selector_coverages[8];SfntGposSubtable basic;
    const auto put=[&](u32 at,u16 value) noexcept {bytes[at]=static_cast<u8>(value>>8);bytes[at+1]=static_cast<u8>(value);};
    const SfntGposProgramStorage storage{{lookups,2},{&basic,1},{},{contexts,4},{selector_coverages,8},{}};
    u32 exercised=0;
    for(u32 mode=0;mode<7;++mode)for(u32 filtered=0;filtered<2;++filtered){
        clear_bytes(bytes,sizeof(bytes));
        const u16 header[]{1,0,10,12,14,0,0,2,6,164,8,0,4,14,50,86,122};
        for(u32 i=0;i<sizeof(header)/2;++i)put(i*2,header[i]);put(22,filtered?8:0);if(mode==5)put(24,3);
        for(u32 i=0;i<4;++i){const u32 at=34+i*36;u16 key=static_cast<u16>(1+i*2);
            if(mode==1&&i==2)key=3;if(mode==2&&(i==1||i==2))key=static_cast<u16>(i==1?5:3);
            const u16 context[]{3,1,18,1,28,0,1,0,1};for(u32 j=0;j<9;++j)put(at+j*2,context[j]);
            put(at+18,mode==3||mode==4?2:1);put(at+20,1);put(at+22,key);
            if(mode==3||mode==4){put(at+24,static_cast<u16>(key+(mode==3)));put(at+26,0);}
            put(at+28,1);put(at+30,1);put(at+32,mode==6&&i==2?10:9);
        }
        const u16 single[]{1,0,1,8,1,8,4,11,1,1,9};for(u32 i=0;i<11;++i)put(178+i*2,single[i]);
        SfntLayout layout{};SfntGposProgram program{};
        if(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::positioning,&layout)!=SfntStatus::ok||
           sfnt_gpos_program_prepare(layout,{},12,storage,&program)!=SfntStatus::ok)return false;
        const bool certified=mode==0||mode==4||mode==6;if(((lookups[0].bounds.ready&4)!=0)!=certified)return false;
        for(u16 key=0;key<11;++key)for(u32 gap=0;gap<3;++gap)for(u32 rtl=0;rtl<2;++rtl){
            SfntGsubGlyph input_glyphs[3]{{key,SfntGlyphClass::base,0},{8,SfntGlyphClass::mark,0},{9,SfntGlyphClass::base,0}};
            u32 n=gap==0?1:gap==1?2:3;input_glyphs[n-1]={9,SfntGlyphClass::base,0};
            double nominal[]{100,100,100};u8 flags[]{2,0,0};SfntPosition positioned[3],expected[3];SfntGposExecutionFrame call_frames[2];u32 matched[2];
            const auto run=[&](u64 budget,SfntGposExecutionResult& result) noexcept {for(auto& p:positioned)p={100,0,0};
                return sfnt_gpos_execute_program_lookup(program,0,sfnt_glyph_view({input_glyphs,n}),{nominal,n},{flags,n},{},{},{positioned,n},n-1,n,rtl!=0,{},
                    SfntNullAnchorPolicy::skip,SfntComponentAnchorPolicy::strict,{{call_frames,2},{matched,2},{},{},{}},{budget,1000},&result);};
            SfntGposExecutionResult optimized{},ordinary{};
            if(run(1000,optimized)!=SfntStatus::ok)return false;copy_bytes(expected,positioned,sizeof(expected));
            const auto ready=lookups[0].bounds.ready;lookups[0].bounds.ready&=~u16{4};
            if(run(1000,ordinary)!=SfntStatus::ok||ordinary.matches!=optimized.matches||ordinary.actions!=optimized.actions||!bytes_equal(expected,positioned,sizeof(expected)))return false;
            lookups[0].bounds.ready=ready;exercised+=static_cast<u32>(optimized.actions!=0);
            if(mode==0&&gap==1&&key==7&&(optimized.actions!=1||positioned[n-1].advance!=111))return false;
            for(u64 budget=0;budget<=optimized.operations;++budget){SfntGposExecutionResult limited{};const auto status=run(budget,limited);
                if(status!=(budget==optimized.operations?SfntStatus::ok:SfntStatus::resource_limit)||limited.operations>budget)return false;}
        }
    }
    return exercised>0;
}
