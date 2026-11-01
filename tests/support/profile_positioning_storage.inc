#ifndef MAICORD_PROFILE_POSITIONING_STORAGE_TEST
#define MAICORD_PROFILE_POSITIONING_STORAGE_TEST
// Derive the retained footprint from the decoded font's selected GPOS closure,
// including compact lookup records and inherited payloads. No generous byte cap
// may hide accidentally compiling or retaining the whole positioning table.
template<class T> usize profile_storage_end(usize offset,u32 count) noexcept {
    if(!count)return offset;
    return ((offset+alignof(T)-1)&~usize(alignof(T)-1u))+sizeof(T)*static_cast<usize>(count);
}
bool profile_positioning_storage(const NativeRunPlanStore& store,NativeRunPlanHandle run,
    ConstSlice<SfntLookupStep> substitutions,ConstSlice<u16> roots,const Arena& state,usize before,
    const SfntGposProgram* program,bool independent=false) noexcept {
    const auto* parent=native_run_plan_get(store,run,independent);CHECK(parent);
    usize expected=profile_storage_end<SfntLookupStep>(before,substitutions.count);
    CHECK(!substitutions.count||reinterpret_cast<const u8*>(substitutions.data)==state.base+expected-substitutions.count*sizeof(SfntLookupStep));
    expected=profile_storage_end<u16>(expected,roots.count);
    CHECK(!roots.count||reinterpret_cast<const u8*>(roots.data)==state.base+expected-roots.count*sizeof(u16));
    bool inherited=true;
    for(u32 i=0;i<roots.count;++i){const auto root=roots[i];
        const auto* prior=parent->positioning&&root<parent->positioning->lookups.count?sfnt_gpos_lookup(*parent->positioning,root):nullptr;
        if(!prior||!prior->type)inherited=false;
        CHECK(program&&root<program->lookups.count&&sfnt_gpos_lookup(*program,root)&&sfnt_gpos_lookup(*program,root)->type);
    }
    if(inherited){CHECK(program==parent->positioning&&state.offset==expected);return true;}
    const auto* instance=native_font_instance_get(*store.instances,store.slots[run.slot-1].instance);CHECK(instance);
    const auto* font=native_font_get(*store.instances->fonts,instance->face);CHECK(font);
    ConstSlice<u8> bytes{};SfntLayout layout{};
    CHECK(sfnt_table(font->geometry.face,sfnt_tag('G','P','O','S'),&bytes)==SfntStatus::ok);
    CHECK(sfnt_layout_open(bytes,SfntLayoutKind::positioning,&layout)==SfntStatus::ok);
    Arena temporary{};CHECK(arena_initialize(&temporary,1024*1024,0,4096));
    struct Release {Arena* arena;~Release() noexcept {arena_release(arena);}} release{&temporary};
    const u32 words=(layout.lookup_count+63u)/64;
    auto* bits=arena_allocate<u64>(&temporary,words);auto* queue=arena_allocate<u16>(&temporary,layout.lookup_count);
    CHECK((!words||bits)&&(!layout.lookup_count||queue));
    SfntLookupSelectionResult selection{};
    CHECK(sfnt_lookup_selection(layout,roots,{bits,words},{queue,layout.lookup_count},&selection)==SfntStatus::ok);
    SfntGposProgramRequirements need{};
    CHECK(sfnt_gpos_program_requirements(layout,&need,1000000,{bits,words},parent->positioning)==SfntStatus::ok);
    expected=profile_storage_end<SfntGposProgram>(expected,1);
    CHECK(reinterpret_cast<const u8*>(program)==state.base+expected-sizeof(SfntGposProgram));
    expected=profile_storage_end<SfntGposExecutableLookup>(expected,need.records);
    expected=profile_storage_end<SfntGposSubtable>(expected,need.basic);
    expected=profile_storage_end<SfntGposAttachment>(expected,need.attachments);
    expected=profile_storage_end<SfntContext>(expected,need.contexts);
    expected=profile_storage_end<SfntCoverage>(expected,need.coverages);
    expected=profile_storage_end<SfntClassDef>(expected,need.classes);
    expected=profile_storage_end<const SfntGposExecutableLookup*>(expected,need.lookups);
    CHECK(program->lookups.count==need.lookups&&program->entries&&state.offset==expected);
    for(u32 i=0;i<need.lookups;++i){const auto* actual=sfnt_gpos_lookup(*program,static_cast<u16>(i));
        const bool selected=(bits[i/64]&(u64{1}<<(i%64)))!=0;
        if(!selected){CHECK(!actual);continue;}
        CHECK(actual&&actual->type);
        const auto* prior=parent->positioning?sfnt_gpos_lookup(*parent->positioning,static_cast<u16>(i)):nullptr;
        if(prior&&prior->type)CHECK(actual==prior);
        else CHECK(reinterpret_cast<const u8*>(actual)>=reinterpret_cast<const u8*>(program)+sizeof(SfntGposProgram)&&
            reinterpret_cast<const u8*>(actual)+sizeof(SfntGposExecutableLookup)<=state.base+state.offset);
    }
    return true;
}
#endif
