#include "generated/malayalam_presentation_fixture.inc"
bool malayalam_presentation_units() noexcept {
    const ConstSlice<u8> fixtures[]{{malayalam_presentation_ordered,sizeof(malayalam_presentation_ordered)},
        {malayalam_presentation_shared,sizeof(malayalam_presentation_shared)},{malayalam_presentation_repeat,sizeof(malayalam_presentation_repeat)}};
    const u16 expected[][2]{{1,0},{0,0},{0,0}};const u32 counts[]{2,2,1};
    for(u32 f=0;f<3;++f){SfntLayout layout{};CHECK(sfnt_layout_open(fixtures[f],SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        u16 scratch[8];SfntLookupStep steps[8];SfntLookupPlanResult full{},result{};
        const auto schedule=[&](u64 work=1000000,u32 capacity=8,bool ordered=true) noexcept {
            return sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::malayalam,false,{scratch,8},{steps,capacity},&result,work,ordered);};
        CHECK(schedule()==SfntStatus::ok&&result.lookup_count==counts[f]);full=result;
        for(u32 i=0;i<counts[f];++i)CHECK(steps[i].lookup==expected[f][i]&&steps[i].stage==12&&!steps[i].feature_mask);
        CHECK(schedule(1000000,8,false)==SfntStatus::ok&&result.lookup_count==(f?1u:2u)&&steps[0].lookup==0);
        for(u64 work=0;work<=full.operations;++work){for(auto& step:steps)step={65535,255,255};
            const auto status=schedule(work);CHECK(result.operations<=work&&status==(work==full.operations?SfntStatus::ok:SfntStatus::resource_limit));
            if(status!=SfntStatus::ok)for(const auto& step:steps)CHECK(step.lookup==65535&&step.feature_mask==255&&step.stage==255);}
        for(auto& step:steps)step={65535,255,255};CHECK(schedule(1000000,counts[f]-1)==SfntStatus::output_too_small&&result.lookup_count==counts[f]);
        for(const auto& step:steps)CHECK(step.lookup==65535&&step.feature_mask==255&&step.stage==255);
        CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::kannada,false,{scratch,8},{steps,8},&result,1000000,true)==SfntStatus::invalid_input&&!result.operations&&!result.lookup_count);
        layout.kind=SfntLayoutKind::positioning;CHECK(schedule()==SfntStatus::invalid_input&&!result.lookup_count);
    }
    print("PASS Malayalam fallback presentation ordering, contextual reuse, deduplication and atomic limits\n");return true;
}
