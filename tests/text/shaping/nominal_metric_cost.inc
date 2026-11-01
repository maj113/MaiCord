bool nominal_metric_guards() noexcept {
    struct Counter {u32 calls=0;u16 rejected=65535;};
    Counter counter;SfntShapePlan plan{};plan.metric_context=&counter;
    plan.advance=[](void* opaque,u16 glyph,double* out) noexcept {
        auto& c=*static_cast<Counter*>(opaque);++c.calls;if(glyph==c.rejected)return SfntStatus::malformed;
        *out=700+glyph*.125;return SfntStatus::ok;
    };
    SfntClusterGlyph mapped[96];u16 expected_glyphs[96],actual_glyphs[96];SfntPlacement expected[96],actual[96];u32 slots[66];
    const u32 counts[]{1,7,8,31,32,64,96},capacities[]{0,31,32,64};
    for(u32 mode=0;mode<3;++mode){
        for(u32 i=0;i<96;++i)mapped[i]={{static_cast<u16>(mode==0?1:mode==1?1+(i%4)*64:i),SfntGlyphClass::unassigned,0},i};
        for(u32 count:counts){
            plan.cache_advances=false;counter.calls=0;
            CHECK(nominal_placements(plan,{mapped,count},{expected_glyphs,count},{expected,count},{},_mm_set_ss(.0175f))==SfntStatus::ok&&counter.calls==count);
            for(u32 capacity:capacities){
                for(auto& slot:slots)slot=0xfeedbeef;counter.calls=0;plan.cache_advances=true;
                CHECK(nominal_placements(plan,{mapped,count},{actual_glyphs,count},{actual,count},{slots+1,capacity},_mm_set_ss(.0175f))==SfntStatus::ok);
                CHECK(bytes_equal(expected_glyphs,actual_glyphs,count*2)&&bytes_equal(expected,actual,count*sizeof(SfntPlacement)));
                CHECK(slots[0]==0xfeedbeef&&slots[capacity+1]==0xfeedbeef);
                if(!mode&&count>=8&&capacity>=32)CHECK(counter.calls==1);
            }
        }
    }
    mapped[80].glyph.glyph=65535;
    CHECK(nominal_placements(plan,{mapped,96},{actual_glyphs,96},{actual,96},{slots,64},_mm_set_ss(1))==SfntStatus::malformed);
    return true;
}
