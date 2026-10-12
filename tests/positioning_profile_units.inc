u32 compiled_lookups(const SfntGposProgram* program) noexcept {
    u32 count=0;if(program)for(u32 i=0;i<program->lookups.count;++i){const auto* lookup=sfnt_gpos_lookup(*program,static_cast<u16>(i));count+=lookup&&lookup->type;}return count;
}
const void* positioning_payload(const SfntGposExecutableLookup& lookup) noexcept {
    return lookup.type<=2?static_cast<const void*>(lookup.tables.basic.data):lookup.type<=6?static_cast<const void*>(lookup.tables.attachment.data):static_cast<const void*>(lookup.tables.context.data);
}
bool positioning_profiles(NativeRunPlanHandle handle) noexcept {
    Arena state{};CHECK(arena_initialize(&state,1024*1024,0,4096));
    struct Release {Arena& state;~Release() noexcept {arena_release(&state);}} release_state{state};
    const auto* parent=native_run_plan_get(store,handle);CHECK(parent);
    const auto* instance=native_font_instance_get(instance_store,plans[handle.slot-1].instance);CHECK(instance);
    const auto* face=native_font_get(font_store,instance->face);CHECK(face);
    u32 substitutions=0;for(u32 i=0;i<parent->substitutions->lookups.count;++i)substitutions+=sfnt_gsub_lookup(*parent->substitutions,static_cast<u16>(i))!=nullptr;
    print("gsub_total=");number(parent->substitutions->lookups.count);print("\ngsub_compiled=");number(substitutions);print("\n");
    ConstSlice<u8> bytes{};auto status=sfnt_table(face->geometry.face,sfnt_tag('G','P','O','S'),&bytes);
    SfntLayout layout{};SfntGposProgramRequirements full{};
    if(status!=SfntStatus::missing_table){CHECK(status==SfntStatus::ok&&sfnt_layout_open(bytes,SfntLayoutKind::positioning,&layout)==SfntStatus::ok);
        CHECK(sfnt_gpos_program_requirements(layout,&full)==SfntStatus::ok);}
    print("gpos_total=");number(full.lookups);print("\ngpos_base=");number(compiled_lookups(parent->positioning));
    const u32 scripts[]{sfnt_tag('l','a','t','n'),sfnt_tag('h','e','b','r'),sfnt_tag('c','y','r','l'),sfnt_tag('k','a','n','a'),0};
    for(u32 script:scripts){arena_reset(&state);CHECK(arena_allocate<u8>(&state,3));const auto mark=state.offset,temporary_mark=scratch.offset;
        NativeAlphabeticPlan profile{};CHECK(native_alphabetic_plan_prepare(store,handle,script,false,&state,&scratch,&profile)==SfntStatus::ok);
        CHECK(scratch.offset==temporary_mark&&plans[handle.slot-1].references==1);
        const usize end=state.offset;print("\nprofile=");number(script);print(" bytes=");number(end-mark);
        print(" lookups=");number(compiled_lookups(profile.positioning_program));print(" inherited=");number(profile.positioning_program==parent->positioning);
        u32 shared=0;
        if(parent->positioning&&profile.positioning_program)for(u32 i=0;i<parent->positioning->lookups.count;++i){const auto* a=sfnt_gpos_lookup(*parent->positioning,static_cast<u16>(i));const auto* b=sfnt_gpos_lookup(*profile.positioning_program,static_cast<u16>(i));
            if(a&&b&&a->type&&b->type){CHECK(a==b&&positioning_payload(*a)==positioning_payload(*b));++shared;}}
        print(" shared=");number(shared);
        for(u32 i=0;i<profile.positioning_roots.count;++i){const auto root=profile.positioning_roots[i];CHECK(profile.positioning_program&&root<profile.positioning_program->lookups.count&&sfnt_gpos_lookup(*profile.positioning_program,root)&&sfnt_gpos_lookup(*profile.positioning_program,root)->type);}
        if(end==mark)continue;
        CHECK(arena_rewind(&state,{mark}));
        for(usize available=0;available<end-mark;available=available?available*2:1){Arena limited=state;limited.reserved=mark+available;NativeAlphabeticPlan failed{};
            status=native_alphabetic_plan_prepare(store,handle,script,false,&limited,&scratch,&failed);
            CHECK(status==SfntStatus::resource_limit&&!failed.run.slot&&limited.offset==mark&&scratch.offset==temporary_mark);
            CHECK(native_run_plan_get(store,handle)==parent&&plans[handle.slot-1].references==1&&shape(handle,false,false,0,nullptr));}
        Arena limited=state;limited.reserved=end-1;NativeAlphabeticPlan failed{};
        CHECK(native_alphabetic_plan_prepare(store,handle,script,false,&limited,&scratch,&failed)==SfntStatus::resource_limit&&!failed.run.slot&&limited.offset==mark);
        CHECK(native_alphabetic_plan_prepare(store,handle,script,false,&state,&scratch,&profile)==SfntStatus::ok&&state.offset==end&&scratch.offset==temporary_mark);
    }
    print("\n");return true;
}
bool positioning_profile_cost(NativeRunPlanHandle handle) noexcept {
    Arena state{};CHECK(arena_initialize(&state,1024*1024,0,4096));
    struct Release {Arena& state;~Release() noexcept {arena_release(&state);}} release_state{state};
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    const u32 scripts[]{sfnt_tag('h','e','b','r'),sfnt_tag('c','y','r','l'),0};
    for(u32 script:scripts){NativeAlphabeticPlan profile{};CHECK(native_alphabetic_plan_prepare(store,handle,script,false,&state,&scratch,&profile)==SfntStatus::ok);
        const auto used=state.offset;LARGE_INTEGER begin{},end{};u64 first_cycles=0,last_cycles=0;
        CHECK(QueryPerformanceCounter(&begin)&&QueryThreadCycleTime(GetCurrentThread(),&first_cycles));
        for(u32 i=0;i<1024;++i){arena_reset(&state);CHECK(native_alphabetic_plan_prepare(store,handle,script,false,&state,&scratch,&profile)==SfntStatus::ok&&state.offset==used);}
        CHECK(QueryThreadCycleTime(GetCurrentThread(),&last_cycles)&&QueryPerformanceCounter(&end));
        print("profile_cost=");number(script);print(" ticks=");number(end.QuadPart-begin.QuadPart);print(" cycles=");number(last_cycles-first_cycles);
        print(" frequency=");number(frequency.QuadPart);print(" iterations=1024 bytes=");number(used);print("\n");arena_reset(&state);
    }return true;
}
bool run_plan_cost(NativeRunPlanHandle handle) noexcept {
    const auto instance=plans[handle.slot-1].instance;
    CHECK(native_font_instance_retain(&instance_store,instance)&&native_run_plan_release(&store,handle));
    LARGE_INTEGER frequency{},begin{},end{};u64 first_cycles=0,last_cycles=0;usize committed=0,used=0;
    CHECK(QueryPerformanceFrequency(&frequency)&&QueryPerformanceCounter(&begin)&&QueryThreadCycleTime(GetCurrentThread(),&first_cycles));
    for(u32 i=0;i<256;++i){NativeRunPlanHandle next{};CHECK(native_run_plan_acquire(&store,instance,&scratch,&next).ok());
        const auto& state=plans[next.slot-1].state;if(i)CHECK(committed==state.committed+(store.substitutions?store.substitutions->committed:0)&&used==state.offset+(store.substitutions?store.substitutions->offset:0));else{committed=state.committed+(store.substitutions?store.substitutions->committed:0);used=state.offset+(store.substitutions?store.substitutions->offset:0);}
        CHECK(native_run_plan_release(&store,next));}
    CHECK(QueryThreadCycleTime(GetCurrentThread(),&last_cycles)&&QueryPerformanceCounter(&end));
    CHECK(native_font_instance_release(&instance_store,instance)&&!native_font_stats(font_store).files);
    print("plan_cost ticks=");number(end.QuadPart-begin.QuadPart);print(" cycles=");number(last_cycles-first_cycles);print(" frequency=");number(frequency.QuadPart);
    print(" iterations=256 committed=");number(committed);print(" used=");number(used);print("\n");return true;
}

bool emoji_plan_cost(NativeRunPlanHandle handle) noexcept {
    const auto instance=plans[handle.slot-1].instance;
    CHECK(native_font_instance_retain(&instance_store,instance)&&native_run_plan_release(&store,handle));
    Arena profiles{};CHECK(arena_initialize(&profiles,1024*1024,0,4096));
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    u64 ticks[2]{},cycles[2]{};usize committed[2]{},used[2]{};
    for(u32 batch=0;batch<8;++batch){const u32 side=(batch^(batch>>1))&1;LARGE_INTEGER begin{},end{};u64 first=0,last=0;
        CHECK(QueryPerformanceCounter(&begin)&&QueryThreadCycleTime(GetCurrentThread(),&first));
        for(u32 i=0;i<64;++i){NativeRunPlanHandle next{};NativeEmojiPlan profile{};arena_reset(&profiles);
            CHECK(native_run_plan_acquire(&store,instance,&scratch,&next,1000000,16*1024*1024,0,side?NativeRunPlanKind::emoji:NativeRunPlanKind::text).ok());
            CHECK(native_emoji_plan_prepare(store,next,&profiles,&scratch,&profile)==SfntStatus::ok);
            const auto& state=plans[next.slot-1].state;committed[side]=state.committed+(store.substitutions?store.substitutions->committed:0);used[side]=state.offset+profiles.offset+(store.substitutions?store.substitutions->offset:0);
            CHECK(native_run_plan_release(&store,next));}
        CHECK(QueryThreadCycleTime(GetCurrentThread(),&last)&&QueryPerformanceCounter(&end));ticks[side]+=end.QuadPart-begin.QuadPart;cycles[side]+=last-first;
    }
    arena_release(&profiles);CHECK(native_font_instance_release(&instance_store,instance)&&!native_font_stats(font_store).files);
    for(u32 side=0;side<2;++side){print("emoji_plan_cost side=");number(side);print(" ticks=");number(ticks[side]);print(" cycles=");number(cycles[side]);
        print(" frequency=");number(frequency.QuadPart);print(" iterations=256 committed=");number(committed[side]);print(" used=");number(used[side]);print("\n");}
    return true;
}
