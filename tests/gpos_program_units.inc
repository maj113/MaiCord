u64 gpos_budget_checks=0,gpos_capacity_checks=0,gpos_guard_checks=0;u32 gpos_cycle_depth=0;
u64 gpos_prepare_budgets=0,gpos_prepare_guards=0;
u64 gpos_selected_lookups=0,gpos_all_lookups=0;
bool gpos_selection_units() noexcept {
    u8 bytes[100]{};const auto put=[&](u32 at,u16 value) noexcept {bytes[at]=static_cast<u8>(value>>8);bytes[at+1]=static_cast<u8>(value);};
    put(0,1);put(4,10);put(6,12);put(8,14);put(14,3);
    for(u32 i=0;i<3;++i){put(16+i*2,static_cast<u16>(8+i*26));const u32 at=22+i*26;
        put(at,7);put(at+4,1);put(at+6,8);put(at+8,3);put(at+10,1);put(at+12,1);put(at+14,12);
        put(at+18,static_cast<u16>(i==0?1:i==1?0:2));put(at+20,1);put(at+22,1);put(at+24,1);}
    SfntLayout layout{};if(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::positioning,&layout)!=SfntStatus::ok)return false;
    const u16 roots[]{0,0};u64 bits=0;u16 queue[3]{};SfntLookupSelectionResult result{};
    if(sfnt_lookup_selection(layout,{roots,2},{&bits,1},{queue,3},&result)!=SfntStatus::ok||bits!=3||result.lookups!=2||queue[0]!=0||queue[1]!=1)return false;
    const u32 full=result.operations;for(u32 work=0;work<=full;++work){const auto status=sfnt_lookup_selection(layout,{roots,2},{&bits,1},{queue,3},&result,work);
        if(status!=(work<full?SfntStatus::resource_limit:SfntStatus::ok)||result.operations>work)return false;++gpos_prepare_budgets;}
    SfntGposProgramRequirements need{};if(sfnt_gpos_program_requirements(layout,&need,1000000,{&bits,1})!=SfntStatus::ok||need.lookups!=3||need.contexts!=2||need.coverages!=2)return false;
    SfntGposExecutableLookup lookups[3];SfntContext contexts[2];SfntCoverage selected_coverages[2];SfntGposProgram program{};
    SfntGposProgramStorage storage{{lookups,3},{},{},{contexts,2},{selected_coverages,2},{}};
    if(sfnt_gpos_program_prepare(layout,{},2,storage,&program,1000000,1000000,{&bits,1})!=SfntStatus::ok||lookups[0].type!=7||lookups[1].type!=7||lookups[2].type)return false;
    SfntGposExecutableLookup shared_lookups[3];SfntContext shared_context;SfntCoverage shared_coverage;SfntGposProgram shared{};
    SfntGposProgramStorage shared_storage{{shared_lookups,3},{},{},{&shared_context,1},{&shared_coverage,1},{}};
    bits=7;if(sfnt_gpos_program_requirements(layout,&need,1000000,{&bits,1},&program)!=SfntStatus::ok||need.lookups!=3||need.contexts!=1||need.coverages!=1)return false;
    if(sfnt_gpos_program_prepare(layout,{},2,shared_storage,&shared,1000000,1000000,{&bits,1},&program)!=SfntStatus::ok||
       shared_lookups[0].tables.context.data!=lookups[0].tables.context.data||shared_lookups[1].tables.context.data!=lookups[1].tables.context.data||shared_lookups[2].tables.context.data!=&shared_context)return false;
    const SfntGposExecutableLookup* entries[3];SfntGposExecutableLookup added{};SfntGposProgram compact{};
    SfntGposProgramStorage compact_storage{{&added,1},{},{},{&shared_context,1},{&shared_coverage,1},{},{entries,3}};
    if(need.records!=1||sfnt_gpos_program_prepare(layout,{},2,compact_storage,&compact,1000000,1000000,{&bits,1},&program)!=SfntStatus::ok||
       compact.lookups.data||entries[0]!=lookups||entries[1]!=lookups+1||entries[2]!=&added)return false;
    const SfntGposExecutableLookup stable[]{lookups[0],lookups[1]};
    const SfntGposExecutableLookup* failed_entries[3];
    for(u32 mode=0;mode<2;++mode){auto small=compact_storage;small.entries.data=failed_entries;if(mode)small.entries.count=2;else small.lookups.count=0;SfntGposProgram failed{};
        if(sfnt_gpos_program_prepare(layout,{},2,small,&failed,1000000,1000000,{&bits,1},&program)!=SfntStatus::output_too_small||failed.lookups.count||!bytes_equal(stable,lookups,sizeof(stable)))return false;}
    const SfntGposExecutableLookup* aliases[3];SfntGposProgram borrowed{};
    if(sfnt_gpos_program_requirements(layout,&need,1000000,{&bits,1},&compact)!=SfntStatus::ok||need.records||need.contexts||
       sfnt_gpos_program_prepare(layout,{},2,{{},{},{},{},{},{},{aliases,3}},&borrowed,0,1000000,{&bits,1},&compact)!=SfntStatus::ok||!bytes_equal(aliases,entries,sizeof(aliases)))return false;
    bits=3;
    if(sfnt_gpos_program_requirements(layout,&need,1000000,{&bits,1},&program)!=SfntStatus::ok||need.basic||need.attachments||need.contexts||need.coverages||need.classes)return false;
    if(sfnt_gpos_program_prepare(layout,{},2,{{shared_lookups,3},{},{},{},{},{}},&shared,0,1000000,{&bits,1},&program)!=SfntStatus::ok||shared_lookups[2].type)return false;
    SfntGdef other{};other.table={bytes,sizeof(bytes)};
    if(sfnt_gpos_program_prepare(layout,other,2,shared_storage,&shared,1000000,1000000,{&bits,1},&program)!=SfntStatus::invalid_input||shared.lookups.count)return false;
    if(sfnt_gpos_program_prepare(layout,{},3,shared_storage,&shared,1000000,1000000,{&bits,1},&program)!=SfntStatus::invalid_input||shared.lookups.count)return false;
    auto wrong=program;wrong.lookups.count=2;
    if(sfnt_gpos_program_requirements(layout,&need,1000000,{&bits,1},&wrong)!=SfntStatus::invalid_input||need.lookups)return false;
    if(sfnt_gpos_program_prepare(layout,{},2,shared_storage,&shared,1000000,1000000,{&bits,1},&wrong)!=SfntStatus::invalid_input||shared.lookups.count)return false;
    wrong=program;wrong.lookups.data=nullptr;
    if(sfnt_gpos_program_prepare(layout,{},2,shared_storage,&shared,1000000,1000000,{&bits,1},&wrong)!=SfntStatus::invalid_input||shared.lookups.count)return false;
    bits=1;if(sfnt_gpos_program_prepare(layout,{},2,shared_storage,&shared,1000000,1000000,{&bits,1},&program)!=SfntStatus::invalid_input||shared.lookups.count)return false;
    bits=1;if(sfnt_gpos_program_prepare(layout,{},2,storage,&program,1000000,1000000,{&bits,1})!=SfntStatus::invalid_input||program.lookups.count)return false;
    if(sfnt_lookup_selection(layout,{roots,2},{&bits,1},{queue,2},&result)!=SfntStatus::output_too_small)return false;
    const u16 invalid=3;if(sfnt_lookup_selection(layout,{&invalid,1},{&bits,1},{queue,3},&result)!=SfntStatus::invalid_input)return false;
    put(40,3);if(sfnt_lookup_selection(layout,{roots,2},{&bits,1},{queue,3},&result)!=SfntStatus::malformed)return false;
    return true;
}
bool gpos_shared_coverage_units() noexcept {
    if(!gpos_selection_units())return false;
    // Two distinct immutable coverage arrays with equal extrema but a hole.
    // Mode 1 covers that hole in the second subtable; mode 2 aliases the first.
    const u16 words[]{1,0,10,12,14,0,0,1,4,7,0,2,10,26,
        3,1,0,8,1,2,1,3, 3,1,0,8,1,2,1,3};
    u8 bytes[sizeof(words)];SfntGposExecutableLookup lookup{};SfntContext contexts[2];SfntCoverage shared_coverages[2];
    SfntGposProgramStorage storage{{&lookup,1},{},{},{contexts,2},{shared_coverages,2},{}};
    for(u32 mode=0;mode<3;++mode){
        for(u32 i=0;i<sizeof(words)/2;++i){bytes[2*i]=static_cast<u8>(words[i]>>8);bytes[2*i+1]=static_cast<u8>(words[i]);}
        if(mode==1)bytes[57]=2;if(mode==2)bytes[27]=10;
        SfntLayout layout{};SfntGposProgram program{};
        if(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::positioning,&layout)!=SfntStatus::ok||
           sfnt_gpos_program_prepare(layout,{},4,storage,&program)!=SfntStatus::ok||lookup.bounds.ready!=(mode==1?1:3))return false;
        for(u16 id=1;id<=3;++id){const SfntGsubGlyph glyph{id,SfntGlyphClass::base,0};const double nominal=100;const u8 flag=2;
            SfntPosition position{};SfntGposExecutionResult optimized{},ordinary{};SfntGposExecutionFrame frame{};u32 selected=0;
            const auto run=[&](u64 budget,SfntGposExecutionResult& result) noexcept {position={100,0,0};return sfnt_gpos_execute_program_lookup(program,0,sfnt_glyph_view({&glyph,1}),
                {&nominal,1},{&flag,1},{},{},{&position,1},0,1,false,{},SfntNullAnchorPolicy::skip,SfntComponentAnchorPolicy::strict,
                {{&frame,1},{&selected,1},{},{},{}},{budget,1000},&result);};
            if(run(1000,optimized)!=SfntStatus::ok||position.advance!=100||position.advance_offset||position.ascender_offset)return false;
            // The ordinary scan keeps neither the shared certificate nor a digest.
            const auto bounds=lookup.bounds;lookup.bounds.ready=1;lookup.bounds.digest={};
            if(run(1000,ordinary)!=SfntStatus::ok||ordinary.matches!=optimized.matches||ordinary.actions!=optimized.actions||position.advance!=100)return false;
            lookup.bounds=bounds;
            if(optimized.matches!=static_cast<u64>(id!=2||mode==1))return false;
            if(mode!=1&&id==2&&optimized.operations>=ordinary.operations)return false;
            for(u64 budget=0;budget<=optimized.operations;++budget){SfntGposExecutionResult limited{};const auto status=run(budget,limited);
                if(status!=(budget==optimized.operations?SfntStatus::ok:SfntStatus::resource_limit)||limited.operations>budget)return false;}
        }
    }
    return true;
}
bool gpos_prepare_units(const SfntLayout& layout,const SfntGdef& gdef,u32 limit) noexcept {
    static SfntGposExecutableLookup lookups[32];static SfntGposSubtable basics[64];static SfntGposAttachment attachments[64];static SfntContext contexts[64];static SfntCoverage cv[256];static SfntClassDef cl[64];
    SfntGposProgramStorage storage{{lookups,32},{basics,64},{attachments,64},{contexts,64},{cv,256},{cl,64}};SfntGposProgram program{};SfntGposProgramRequirements need{};
    if(sfnt_gpos_program_requirements(layout,&need,0)!=SfntStatus::resource_limit || need.lookups || sfnt_gpos_program_prepare(layout,gdef,limit,{},&program)!=SfntStatus::output_too_small || program.lookups.count)return false;
    if(sfnt_gpos_program_requirements(layout,&need)!=SfntStatus::ok)return false;
    const auto capacity=[&](auto SfntGposProgramStorage::* member,u32 required) noexcept {
        for(u32 count=0;count<required;++count){auto small=storage;auto& slice=small.*member;slice.count=count;
            u8 guard[sizeof(*slice.data)];copy_bytes(guard,slice.data+count,sizeof(guard));
            const auto status=sfnt_gpos_program_prepare(layout,gdef,limit,small,&program);++gpos_capacity_checks;
            if(status!=SfntStatus::output_too_small||program.lookups.count||!bytes_equal(guard,slice.data+count,sizeof(guard)))return false;
        }return true;
    };
    if(!capacity(&SfntGposProgramStorage::lookups,need.lookups)||!capacity(&SfntGposProgramStorage::basic,need.basic)||
       !capacity(&SfntGposProgramStorage::attachments,need.attachments)||!capacity(&SfntGposProgramStorage::contexts,need.contexts)||
       !capacity(&SfntGposProgramStorage::coverages,need.coverages)||!capacity(&SfntGposProgramStorage::classes,need.classes))return false;
    const u32 directory=need.lookups+need.basic+need.attachments+need.contexts;
    for(u32 budget=0;budget<=directory;++budget){const auto status=sfnt_gpos_program_prepare(layout,gdef,limit,storage,&program,1000000,budget);++gpos_prepare_budgets;
        if(status!=(budget==directory?SfntStatus::ok:SfntStatus::resource_limit)||(status!=SfntStatus::ok&&program.lookups.count))return false;}
    bool completed=false;for(u32 budget=0;budget<10000;++budget){const auto status=sfnt_gpos_program_prepare(layout,gdef,limit,storage,&program,budget);++gpos_prepare_budgets;
        if(status==SfntStatus::ok){completed=true;break;}if(status!=SfntStatus::resource_limit || program.lookups.count)return false;}
    if(!completed)return false;SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize,bytes=(layout.table.count/page+1)*page;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,bytes+page,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;
    DWORD old=0;bool ok=VirtualProtect(memory+bytes,page,PAGE_NOACCESS,&old)!=0;u32 random=0x41a593c7;
    for(u32 i=0;i<64 && ok;++i){random^=random<<13;random^=random>>17;random^=random<<5;const u32 size=i?random%(layout.table.count+1):layout.table.count;u8* data=memory+bytes-size;copy_bytes(data,layout.table.data,size);if(i>32 && size)data[(random>>8)%size]^=static_cast<u8>(random>>16);
        SfntLayout changed{};auto status=sfnt_layout_open({data,size},SfntLayoutKind::positioning,&changed);if(status==SfntStatus::ok)status=sfnt_gpos_program_prepare(changed,gdef,limit,storage,&program,1000,1000);
        if(!i && status!=SfntStatus::ok)ok=false;++gpos_prepare_guards;}
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
bool gpos_program_units(const SfntGposProgram& program,u16 index,SfntGlyphView view,ConstSlice<double> nominal,ConstSlice<u8> flags,ConstSlice<SfntMarkAssociation> associations,ConstSlice<u32> targets,Slice<SfntPosition> state) noexcept {
    const u32 n=sfnt_glyph_count(view);if(n>64)return false;SfntPosition before[64],complete[64];copy_bytes(before,state.data,n*sizeof(SfntPosition));
    u64 bits=0;double prefix[64];SfntGposExecutionScratch scratch{{gpos_frames,32},{gpos_positions,256},{&bits,1},{prefix,targets.count},{cursive_chains,64}};
    auto execute=[&](u64 work,SfntGposExecutionScratch storage,SfntGposExecutionResult* out) noexcept {copy_bytes(state.data,before,n*sizeof(SfntPosition));return sfnt_gpos_execute_program_lookup(program,index,view,nominal,flags,associations,targets,state,0,n,false,{},SfntNullAnchorPolicy::windows_parent_coordinates,SfntComponentAnchorPolicy::skip_missing,storage,{work,100000},out);};
    SfntGposExecutionResult r{};if(execute(1000000,scratch,&r)!=SfntStatus::ok || r.operations>10000)return false;const u64 full=r.operations;const u32 depth=r.max_depth;copy_bytes(complete,state.data,n*sizeof(SfntPosition));
    for(u64 budget=0;budget<=full;++budget){const auto s=execute(budget,scratch,&r);++gpos_budget_checks;
        if(s!=(budget<full?SfntStatus::resource_limit:SfntStatus::ok) || r.operations>budget || (s==SfntStatus::ok && !bytes_equal(state.data,complete,n*sizeof(SfntPosition))))return false;}
    if(depth){auto small=scratch;small.frames.count=depth-1;if(execute(1000000,small,&r)!=SfntStatus::resource_limit)return false;++gpos_capacity_checks;}
    // Exact scratch arrays end at inaccessible pages. Determine the minimum
    // packed-position capacity using the published failure contract first.
    u32 need=0;bool found=false;
    for(;need<=256;++need){auto small=scratch;small.positions.count=need;const auto s=execute(1000000,small,&r);++gpos_capacity_checks;if(s==SfntStatus::ok){found=true;break;}if(s!=SfntStatus::output_too_small)return false;}
    if(!found)return false;SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,6*page,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;
    bool ok=true;DWORD old=0;for(u32 i=1;i<6;i+=2)if(!VirtualProtect(memory+i*page,page,PAGE_NOACCESS,&old))ok=false;
    if(ok){auto exact=scratch;exact.positions={reinterpret_cast<u32*>(memory+page-need*4),need};exact.frames={reinterpret_cast<SfntGposExecutionFrame*>(memory+3*page-depth*sizeof(SfntGposExecutionFrame)),depth};exact.cursive_chains={reinterpret_cast<SfntCursiveChainNode*>(memory+5*page-n*sizeof(SfntCursiveChainNode)),n};
        ok=execute(1000000,exact,&r)==SfntStatus::ok && bytes_equal(state.data,complete,n*sizeof(SfntPosition));++gpos_guard_checks;}
    VirtualFree(memory,0,MEM_RELEASE);copy_bytes(state.data,before,n*sizeof(SfntPosition));return ok;
}
bool gpos_cycle_units() noexcept {
    // A valid single-input context referring to itself. Action storage is
    // borrowed, exactly like the validated table view returned by preparation.
    // Build through the parser rather than forging an executable context.
    const u8 context_bytes[]{0,3,0,1,0,1,0,12,0,0,0,0,0,1,0,1,0,1};
    SfntCoverage coverage{};SfntContext context{};SfntGposExecutableLookup lookup{};
    if(sfnt_context_open({context_bytes,sizeof(context_bytes)},SfntContextKind::sequence,2,1,{&coverage,1},{},&context)!=SfntStatus::ok)return false;
    lookup.type=7;lookup.tables.context={&context,1};SfntGposProgram program{{&lookup,1},{},2};SfntGsubGlyph glyph{1,SfntGlyphClass::base,0};
    const double nominal=600;const u8 flag=2;SfntPosition position{600,0,0};SfntGposExecutionResult result{};SfntGposExecutionFrame cycle_frames[8];u32 cycle_positions[9];
    SfntGlyphBuffer buffer{};if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(&glyph),sizeof(glyph)},sizeof(glyph),1,&buffer)!=SfntStatus::ok)return false;
    const auto status=sfnt_gpos_execute_program_lookup(program,0,sfnt_glyph_buffer_view(buffer),{&nominal,1},{&flag,1},{},{},{&position,1},0,1,false,{},SfntNullAnchorPolicy::skip,SfntComponentAnchorPolicy::strict,{{cycle_frames,8},{cycle_positions,9},{},{},{}},{10000,1000},&result);
    gpos_cycle_depth=result.max_depth;return status==SfntStatus::resource_limit && result.max_depth==8 && position.advance==600 && position.advance_offset==0 && position.ascender_offset==0;
}
