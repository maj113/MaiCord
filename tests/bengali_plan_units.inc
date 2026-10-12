bool bengali_plan_units() noexcept {
    CHECK(tamil_plan_units(SfntIndicProfile::bengali));
    u8 bytes[sizeof(deva_plan_fixture)];copy_bytes(bytes,deva_plan_fixture,sizeof(bytes));
    const u32 script=byte_load_be16(bytes+4),features=byte_load_be16(bytes+6);
    byte_store_be32(bytes+script+2,sfnt_tag('b','n','g','2'));
    byte_store_be32(bytes+features+2+3*6,sfnt_tag('i','n','i','t'));
    SfntLayout layout{};CHECK(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
    u16 scratch[8]{};SfntLookupStep steps[16]{};SfntLookupPlanResult result{};
    const SfntLookupStep expected[]{{6,0,0},{5,1,1},{3,0,2},{0,2,3},{1,0,12},{2,sfnt_bengali_initial,12},{4,0,12}};
    CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::bengali,false,{scratch,8},{steps,16},&result)==SfntStatus::ok);
    CHECK(result.lookup_count==7&&bytes_equal(steps,expected,sizeof(expected)));const u64 total=result.operations;
    for(u64 work=0;work<=total;++work){for(auto& step:steps)step={65535,255,255};
        const auto status=sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::bengali,false,{scratch,8},{steps,16},&result,work);
        CHECK(result.operations<=work&&status==(work==total?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)for(const auto& step:steps)CHECK(step.lookup==65535&&step.feature_mask==255&&step.stage==255);}
    // A shared presentation root is unmasked when another active feature uses it.
    const u32 pres=features+byte_load_be16(bytes+features+2+6*6+4);bytes[pres+6]=0;bytes[pres+7]=2;
    CHECK(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
    CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::bengali,true,{scratch,8},{steps,16},&result)==SfntStatus::ok);
    CHECK(result.lookup_count==7&&steps[5].lookup==2&&!steps[5].feature_mask&&steps[5].stage==12);
    byte_store_be32(bytes+script+2,sfnt_tag('t','m','l','2'));
    CHECK(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
    CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::tamil,false,{scratch,8},{steps,16},&result)==SfntStatus::ok);
    // Tamil ignores init; the same lookup remains active only through pres.
    CHECK(result.lookup_count==7&&steps[5].lookup==2&&!steps[5].feature_mask);
    print("PASS Bengali initial eligibility, shared roots and four-byte records\n");return true;
}
