SfntGsubExecutableLookup direct_lookups[128];
SfntGsubExecutableSubtable direct_subtables[1024];SfntCoverage direct_coverages[1024];SfntClassDef direct_classes[128];
u64 direct_entries=0,direct_limits=0;
bool direct_preparation(const SfntLayout& layout,const SfntGdef& gdef,u32 limit,bool ignore_empty) noexcept {
    u32 subat=0,covat=0,classat=0;
    for(u16 index=0;index<layout.lookup_count;++index){
        SfntGsubProgramRequirements need{};if(sfnt_gsub_lookup_requirements(layout,index,&need)!=SfntStatus::ok||need.lookups!=1)return false;
        if(need.subtables>1024-subat||need.coverages>1024-covat||need.classes>128-classat)return false;
        const SfntGsubLookupStorage storage{{direct_subtables+subat,need.subtables},{direct_coverages+covat,need.coverages},{direct_classes+classat,need.classes}};
        SfntGsubExecutableLookup entry{};
        const auto capacity=[&](auto SfntGsubLookupStorage::* member,u32 required) noexcept {
            for(u32 count=0;count<required;++count){auto small=storage;auto& slice=small.*member;slice.count=count;
                u8 guard[sizeof(*slice.data)];copy_bytes(guard,slice.data+count,sizeof(guard));
                const auto status=sfnt_gsub_prepare_lookup(layout,index,gdef,limit,small,&entry,1000000,1000000,ignore_empty);++direct_limits;
                if(status!=SfntStatus::output_too_small||entry.type||!bytes_equal(guard,slice.data+count,sizeof(guard)))return false;
            }return true;
        };
        if(!capacity(&SfntGsubLookupStorage::subtables,need.subtables)||!capacity(&SfntGsubLookupStorage::coverages,need.coverages)||!capacity(&SfntGsubLookupStorage::classes,need.classes))return false;
        for(u32 budget=0;budget<=need.subtables+1;++budget){
            const auto expected=budget==need.subtables+1?SfntStatus::ok:SfntStatus::resource_limit;SfntGsubProgramRequirements bounded{};
            if(sfnt_gsub_lookup_requirements(layout,index,&bounded,budget)!=expected||(expected!=SfntStatus::ok&&bounded.lookups)||
               sfnt_gsub_prepare_lookup(layout,index,gdef,limit,storage,&entry,1000000,budget,ignore_empty)!=expected||(expected!=SfntStatus::ok&&entry.type))return false;
            ++direct_limits;
        }
        const auto& eager=lookups[index];if(entry.type!=eager.type||entry.subtables.count!=eager.subtables.count||entry.bounds.first!=eager.bounds.first||entry.bounds.last!=eager.bounds.last)return false;
        direct_lookups[index]=entry;subat+=need.subtables;covat+=need.coverages;classat+=need.classes;++direct_entries;
    }
    SfntGsubExecutableLookup rejected{};SfntGsubProgramRequirements need{};
    return sfnt_gsub_lookup_requirements(layout,layout.lookup_count,&need)==SfntStatus::invalid_input&&!need.lookups&&
        sfnt_gsub_prepare_lookup(layout,layout.lookup_count,gdef,limit,{},&rejected)==SfntStatus::invalid_input&&!rejected.type;
}
