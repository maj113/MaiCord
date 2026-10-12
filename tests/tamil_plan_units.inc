bool tamil_plan_units(SfntIndicProfile profile=SfntIndicProfile::tamil) noexcept {
    const auto* info=sfnt_indic_script(profile);CHECK(info);
    const ConstSlice<u8> fixtures[]{{deva_plan_fixture,sizeof(deva_plan_fixture)},{deva_required_fixture,sizeof(deva_required_fixture)}};
    const SfntLookupStep expected[]{{6,0,0},{5,1,1},{3,0,2},{0,2,3},{2,8,7},{1,0,12},{4,0,12}};
    for(u32 required=0;required<2;++required){u8 bytes[1024];CHECK(fixtures[required].count<=sizeof(bytes));copy_bytes(bytes,fixtures[required].data,fixtures[required].count);
        const u32 tag_at=byte_load_be16(bytes+4)+2;byte_store_be32(bytes+tag_at,info->tag);
        SfntLayout layout{};CHECK(sfnt_layout_open({bytes,fixtures[required].count},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        u16 scratch[8];SfntLookupStep steps[64],saved[64];SfntLookupPlanResult full{},result{};
        CHECK(sfnt_indic_lookup_plan(layout,{},profile,false,{scratch,8},{steps,64},&full)==SfntStatus::ok);
        CHECK(full.script_tag==info->tag&&full.language_found&&!full.language_tag&&full.lookup_count==(required?19u:7u));
        if(!required)CHECK(bytes_equal(steps,expected,sizeof(expected)));
        else{u32 at=0;for(u32 stage=0;stage<13;++stage){if(stage==11)continue;
            for(const auto& wanted:expected)if(wanted.stage==stage){CHECK(bytes_equal(steps+at,&wanted,sizeof(wanted)));++at;}
            CHECK(steps[at].lookup==7&&steps[at].stage==stage);++at;}CHECK(at==full.lookup_count);}
        copy_bytes(saved,steps,full.lookup_count*sizeof(SfntLookupStep));
        CHECK(sfnt_indic_lookup_plan(layout,{},profile,true,{scratch,8},{steps,64},&result)==SfntStatus::ok&&result.lookup_count==full.lookup_count&&bytes_equal(steps,saved,full.lookup_count*sizeof(SfntLookupStep)));
        for(u64 budget=0;budget<=full.operations;++budget){for(auto& step:steps)step={65535,255,255};
            const auto status=sfnt_indic_lookup_plan(layout,{},profile,false,{scratch,8},{steps,64},&result,budget);
            CHECK(result.operations<=budget&&status==(budget==full.operations?SfntStatus::ok:SfntStatus::resource_limit));
            if(status!=SfntStatus::ok)for(const auto& step:steps)CHECK(step.lookup==65535&&step.feature_mask==255&&step.stage==255);}
        for(auto& step:steps)step={65535,255,255};
        CHECK(sfnt_indic_lookup_plan(layout,{},profile,false,{scratch,8},{steps,full.lookup_count-1},&result)==SfntStatus::output_too_small&&result.lookup_count==full.lookup_count);
        for(const auto& step:steps)CHECK(step.lookup==65535&&step.feature_mask==255&&step.stage==255);
        CHECK(sfnt_indic_lookup_plan(layout,{},profile,false,{scratch,7},{steps,64},&result)==SfntStatus::output_too_small);
        CHECK(sfnt_indic_form_lookup_plan(layout,{},profile,sfnt_deva_half,{scratch,8},{steps,64},&result)==SfntStatus::ok);
        CHECK(result.lookup_count==2&&steps[0].lookup==2&&steps[1].lookup==6&&!steps[0].stage&&!steps[1].stage&&!steps[0].feature_mask&&!steps[1].feature_mask);
        const u64 form_work=result.operations;
        for(u64 budget=0;budget<=form_work;++budget){for(auto& step:steps)step={65535,255,255};
            const auto status=sfnt_indic_form_lookup_plan(layout,{},profile,sfnt_deva_half,{scratch,8},{steps,64},&result,budget);
            CHECK(result.operations<=budget&&status==(budget==form_work?SfntStatus::ok:SfntStatus::resource_limit));
            if(status!=SfntStatus::ok)for(const auto& step:steps)CHECK(step.lookup==65535&&step.feature_mask==255&&step.stage==255);}
        CHECK(sfnt_indic_form_lookup_plan(layout,{},profile,sfnt_deva_pre,{scratch,8},{steps,64},&result)==SfntStatus::ok&&!result.lookup_count);
        CHECK(sfnt_devanagari_lookup_plan(layout,{},false,{scratch,8},{steps,64},&result)==SfntStatus::ok&&!result.language_found);
        for(auto& step:steps)step={65535,255,255};
        CHECK(sfnt_indic_lookup_plan(layout,{},static_cast<SfntIndicProfile>(255),false,{scratch,8},{steps,64},&result)==SfntStatus::invalid_input&&!result.operations&&!result.lookup_count);
        CHECK(sfnt_indic_form_lookup_plan(layout,{},static_cast<SfntIndicProfile>(255),sfnt_deva_half,{scratch,8},{steps,64},&result)==SfntStatus::invalid_input&&!result.operations);
        for(const auto& step:steps)CHECK(step.lookup==65535&&step.feature_mask==255&&step.stage==255);
        const u32 alternatives[]{profile==SfntIndicProfile::malayalam?sfnt_tag('m','l','y','m'):profile==SfntIndicProfile::kannada?sfnt_tag('k','n','d','a'):profile==SfntIndicProfile::telugu?sfnt_tag('t','e','l','u'):profile==SfntIndicProfile::odia?sfnt_tag('o','r','y','a'):profile==SfntIndicProfile::gurmukhi?sfnt_tag('g','u','r','u'):profile==SfntIndicProfile::gujarati?sfnt_tag('g','u','j','r'):profile==SfntIndicProfile::tamil?sfnt_tag('t','a','m','l'):sfnt_tag('b','e','n','g'),sfnt_tag('D','F','L','T'),sfnt_tag('d','e','v','2')};for(u32 tag:alternatives){
            byte_store_be32(bytes+tag_at,tag);
            CHECK(sfnt_layout_open({bytes,fixtures[required].count},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
            CHECK(sfnt_indic_lookup_plan(layout,{},profile,false,{scratch,8},{steps,64},&result)==SfntStatus::ok&&!result.language_found&&!result.lookup_count);}
    }
    print(profile==SfntIndicProfile::malayalam?"PASS Malayalam modern-script feature schedule, forms, isolation and atomic limits\n":profile==SfntIndicProfile::kannada?"PASS Kannada modern-script feature schedule, forms, isolation and atomic limits\n":profile==SfntIndicProfile::telugu?"PASS Telugu modern-script feature schedule, forms, isolation and atomic limits\n":profile==SfntIndicProfile::odia?"PASS Odia modern-script feature schedule, forms, isolation and atomic limits\n":profile==SfntIndicProfile::gurmukhi?"PASS Gurmukhi modern-script feature schedule, forms, isolation and atomic limits\n":profile==SfntIndicProfile::gujarati?"PASS Gujarati modern-script feature schedule, forms, isolation and atomic limits\n":profile==SfntIndicProfile::tamil?"PASS Tamil modern-script feature schedule, forms, isolation and atomic limits\n":"PASS Bengali modern-script feature schedule, forms, isolation and atomic limits\n");return true;
}
bool tamil_font_plan(ConstSlice<u8> bytes,u32 index) noexcept {
    SfntFace face{};CHECK(sfnt_open(bytes,index,&face)==SfntStatus::ok);
    u16 scratch[4096];SfntLookupStep steps[4096];
    for(u32 kind=0;kind<2;++kind){ConstSlice<u8> data{};SfntLayout layout{};
        CHECK(sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&data)==SfntStatus::ok);
        CHECK(sfnt_layout_open(data,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        for(u32 mode=0;mode<2;++mode){SfntLookupPlanResult result{};
            CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::tamil,mode!=0,{scratch,4096},{steps,4096},&result)==SfntStatus::ok&&result.language_found);
            print("{\"face\":");number(index);print(",\"kind\":");number(kind);print(",\"independent\":");number(mode);print(",\"script\":");number(result.script_tag);print(",\"language\":");number(result.language_tag);print(",\"steps\":[");
            for(u32 i=0;i<result.lookup_count;++i){if(i)print(",");print("[");number(steps[i].lookup);print(",");number(steps[i].feature_mask);print(",");number(steps[i].stage);print("]");}print("]}\n");}
    }return true;
}
