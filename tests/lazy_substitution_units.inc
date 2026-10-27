u64 substitution_digest(const SfntGsubProgram& program) noexcept {
    u64 hash=1469598103934665603ull;
    for(u32 i=0;i<program.lookups.count;++i){const auto* entry=sfnt_gsub_lookup(program,static_cast<u16>(i));if(!entry||!entry->type)continue;const auto& lookup=*entry;
        const auto* bytes=reinterpret_cast<const u8*>(lookup.subtables.data);
        for(usize j=0;j<lookup.subtables.count*sizeof(SfntGsubExecutableSubtable);++j)hash=(hash^bytes[j])*1099511628211ull;
    }return hash;
}
bool lazy_substitution_units(NativeRunPlanHandle handle) noexcept {
    const auto* shape=native_run_plan_get(store,handle);CHECK(shape&&shape->substitutions&&shape->substitutions->source&&store.substitutions);
    const auto& program=*shape->substitutions;CHECK(program.lookups.count<=1024);
    static SfntGsubExecutableLookup snapshot[1024];static const SfntGsubExecutableLookup* locations[1024];u32 failures=0,published=0;
    const auto reserved=store.substitutions->reserved;
    for(u32 i=0;i<program.lookups.count&&failures<3;++i){if(sfnt_gsub_lookup(program,static_cast<u16>(i)))continue;
        const auto mark=store.substitutions->offset,committed=store.substitutions->committed;
        for(u32 j=0;j<program.lookups.count;++j){locations[j]=sfnt_gsub_lookup(program,static_cast<u16>(j));if(locations[j])copy_bytes(snapshot+j,locations[j],sizeof(*snapshot));else snapshot[j]={};}
        const auto unchanged=[&](bool existing_only) noexcept {for(u32 j=0;j<program.lookups.count;++j){if(existing_only&&!locations[j])continue;
            const auto* current=sfnt_gsub_lookup(program,static_cast<u16>(j));if(current!=locations[j]||(current&&!bytes_equal(snapshot+j,current,sizeof(*snapshot))))return false;}return true;};
        const auto digest=substitution_digest(program);
        store.substitutions->reserved=mark;
        const auto status=sfnt_gsub_lookup_prepare(program,static_cast<u16>(i));store.substitutions->reserved=reserved;
        CHECK(status==SfntStatus::resource_limit&&store.substitutions->offset==mark&&store.substitutions->committed==committed);
        CHECK(unchanged(false)&&substitution_digest(program)==digest);++failures;
        CHECK(sfnt_gsub_lookup_prepare(program,static_cast<u16>(i))==SfntStatus::ok&&sfnt_gsub_lookup(program,static_cast<u16>(i))->type);++published;
        const auto used=store.substitutions->offset;
        CHECK(sfnt_gsub_lookup_prepare(program,static_cast<u16>(i))==SfntStatus::ok&&store.substitutions->offset==used);
        CHECK(unchanged(true));
    }
    CHECK(failures==3&&published==3);
    MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(plans[handle.slot-1].state.base,&info,sizeof(info))==sizeof(info)&&info.Protect==PAGE_READONLY);
    CHECK(release(handle)&&!store.substitutions);print("lazy_rollbacks=3 lazy_publications=3 pool_released=1\n");return true;
}
