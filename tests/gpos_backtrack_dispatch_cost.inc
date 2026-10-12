bool backtrack_cost_error(u32 line) noexcept {print(utf8_literal("backtrack cost failure line="));number(line);print(utf8_literal("\n"));return false;}
bool gpos_backtrack_dispatch_cost() noexcept {
    wchar_t path[1024]{};const u32 length=GetWindowsDirectoryW(path,900);if(!length||length>=900)return backtrack_cost_error(__LINE__);
    copy_bytes(path+length,L"\\Fonts\\LeelawUI.ttf",sizeof(L"\\Fonts\\LeelawUI.ttf"));
    NativeFontFileSlot file_slots[1]{};NativeFontFaceSlot face_slots[1]{};NativeFontStore font_store{{file_slots,1},{face_slots,1}};NativeFontHandle face_handle{};
    if(!native_font_acquire(&font_store,path,0,&face_handle).ok())return backtrack_cost_error(__LINE__);
    struct Release {NativeFontStore& store;NativeFontHandle handle;~Release() noexcept {native_font_release(&store,handle);}} release{font_store,face_handle};
    const auto* face=native_font_get(font_store,face_handle);SfntHorizontalMetrics metrics{};ConstSlice<u8> table{};SfntGdef gdef{};SfntLayout layout{};
    if(sfnt_horizontal_metrics_open(face->geometry.face,&metrics)!=SfntStatus::ok||sfnt_table(face->geometry.face,sfnt_tag('G','D','E','F'),&table)!=SfntStatus::ok||
       sfnt_gdef_open(table,metrics.glyph_count,&gdef)!=SfntStatus::ok||sfnt_table(face->geometry.face,sfnt_tag('G','P','O','S'),&table)!=SfntStatus::ok||
       sfnt_layout_open(table,SfntLayoutKind::positioning,&layout)!=SfntStatus::ok)return backtrack_cost_error(__LINE__);
    static SfntGposExecutableLookup bench_lookups[128];static SfntGposSubtable bench_basic[256];static SfntGposAttachment bench_attachments[256];
    static SfntContext bench_contexts[256];static SfntCoverage bench_coverages[2048];static SfntClassDef bench_classes[512];SfntGposProgram program{};
    if(sfnt_gpos_program_prepare(layout,gdef,metrics.glyph_count,{{bench_lookups,128},{bench_basic,256},{bench_attachments,256},{bench_contexts,256},{bench_coverages,2048},{bench_classes,512}},&program)!=SfntStatus::ok||
       program.lookups.count<=14||!(bench_lookups[14].bounds.ready&4)||bench_lookups[14].tables.context.count!=45)return backtrack_cost_error(__LINE__);
    u16 leading=0,mark=0;if(sfnt_cmap_glyph(face->cmap,0xec0,0,&leading)!=SfntStatus::ok||sfnt_cmap_glyph(face->cmap,0xebb,0,&mark)!=SfntStatus::ok)return backtrack_cost_error(__LINE__);
    SfntGsubGlyph input_glyphs[3]{{leading,sfnt_gdef_glyph_class(gdef,leading),0},{},{mark,sfnt_gdef_glyph_class(gdef,mark),0}};
    const double nominal[]{100,100,100};const u8 flags[]{2,2,0};SfntPosition placed[3];SfntGposExecutionFrame call_frames[4];u32 matched[8];SfntGposExecutionResult execution{};
    const auto run=[&](u32 selector) noexcept {const u16 key=byte_load_be16(bench_lookups[14].tables.context[selector].coverages[0].records);
        input_glyphs[1]={key,sfnt_gdef_glyph_class(gdef,key),0};for(auto& p:placed)p={100,0,0};
        return sfnt_gpos_execute_program_lookup(program,14,sfnt_glyph_view({input_glyphs,3}),{nominal,3},{flags,3},{},{},{placed,3},2,3,false,{},
            SfntNullAnchorPolicy::skip,SfntComponentAnchorPolicy::strict,{{call_frames,4},{matched,8},{},{},{}},{100000,10000},&execution);};
    const auto ready=bench_lookups[14].bounds.ready;u64 operations[2]{};
    for(u32 i=0;i<45;++i){SfntPosition expected[3];bench_lookups[14].bounds.ready=ready&~u16{4};if(run(i)!=SfntStatus::ok||!execution.actions)return backtrack_cost_error(__LINE__);
        copy_bytes(expected,placed,sizeof(expected));const auto reference=execution;operations[0]+=execution.operations;
        bench_lookups[14].bounds.ready=ready;if(run(i)!=SfntStatus::ok||execution.actions!=reference.actions||execution.matches!=reference.matches||!bytes_equal(expected,placed,sizeof(expected)))return backtrack_cost_error(__LINE__);
        operations[1]+=execution.operations;}
    u64 ticks[2]{},cycles[2]{};LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    for(u32 trial=0;trial<6;++trial)for(u32 order=0;order<2;++order){const u32 side=order^(trial&1);bench_lookups[14].bounds.ready=side?ready:ready&~u16{4};
        LARGE_INTEGER start{},end{};ULONG64 first=0,last=0;if(!QueryThreadCycleTime(GetCurrentThread(),&first)||!QueryPerformanceCounter(&start))return backtrack_cost_error(__LINE__);
        for(u32 repeat=0;repeat<32;++repeat)for(u32 i=0;i<45;++i)if(run(i)!=SfntStatus::ok)return backtrack_cost_error(__LINE__);
        if(!QueryPerformanceCounter(&end)||!QueryThreadCycleTime(GetCurrentThread(),&last))return backtrack_cost_error(__LINE__);ticks[side]+=end.QuadPart-start.QuadPart;cycles[side]+=last-first;}
    print(utf8_literal("{\"kind\":\"backtrack_dispatch_cost\",\"queries_per_side\":8640,\"frequency\":"));number(frequency.QuadPart);
    print(utf8_literal(",\"ticks\":["));number(ticks[0]);print(utf8_literal(","));number(ticks[1]);print(utf8_literal("],\"cycles\":["));number(cycles[0]);print(utf8_literal(","));number(cycles[1]);
    print(utf8_literal("],\"operations_45\":["));number(operations[0]);print(utf8_literal(","));number(operations[1]);print(utf8_literal("]}\n"));return true;
}
