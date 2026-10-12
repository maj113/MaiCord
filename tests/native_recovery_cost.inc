// QA-only: separate durable-format cloning from replay and retirement cost.
namespace recovery_cost {
void number(u64 value) noexcept {
    char digits[21]{};u32 at=20;do{digits[--at]=static_cast<char>('0'+value%10);value/=10;}while(value);print(digits+at);
}
void field(const char* name,u64 value) noexcept {print(",\"");print(name);print("\":");number(value);}
u64 ticks() noexcept {LARGE_INTEGER t{};QueryPerformanceCounter(&t);return static_cast<u64>(t.QuadPart);}
u64 cycles() noexcept {ULONG64 n=0;QueryThreadCycleTime(GetCurrentThread(),&n);return n;}
bool memory(u64* private_bytes,u64* working_set) noexcept {
    PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof(m);
    if(!K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m)))return false;
    *private_bytes=m.PrivateUsage;*working_set=m.WorkingSetSize;return true;
}
NativeTextFormatDescription description(u32 index) noexcept {
    static const u16 family[]{'S','e','g','o','e',' ','U','I'};
    return {{family,8},0,{400,5,0},16.f+index*.25f,64,true};
}
struct Trial {
    TextSystem original{};u64 begin=0,begin_cycles=0,clone_end=0,clone_cycles=0,replay_end=0;
    u64 private_bytes=0,working_set=0;u32 count=0,mode=0,calls=0;bool sample_memory=false;
};
__declspec(noinline) bool staged(const TextSystem& system,void* opaque) noexcept {
    auto& t=*static_cast<Trial*>(opaque);t.clone_end=ticks();t.clone_cycles=cycles();++t.calls;
    if(t.sample_memory&&!memory(&t.private_bytes,&t.working_set))return false;
    if(t.mode==2){auto draw=output();const bool okay=native_text_candidate_line(system,utf8_literal("AV office"),1000,1000,&draw,nullptr);
        const bool valid=okay&&draw.run_count&&draw.native_fonts()==system.native_fonts;release_output(draw);if(!valid)return false;}
    t.replay_end=ticks();return t.mode!=0;
}
bool run(u32 count,u32 mode) noexcept {
    CHECK(count&&count<=1024&&mode<=2);
    TextSystem system{};CHECK(native_font_session_create(&system.native_fonts)&&native_text_context_initialize(&system.native_text,system.native_fonts));
    struct Release {TextSystem& s;~Release() noexcept {native_text_context_release(&s.native_text);native_font_session_release(&s.native_fonts);}} release{system};
    for(u32 i=0;i<count;++i){u32 handle=0;CHECK(native_text_format_create(system.native_text,description(i),&handle)&&handle==i+1);}
    system.native_format=1;system.em_size=16;
    // Replay measurements start with a live old generation. Keep its output
    // independently retained through staging and retirement.
    GenerationOutput retained;
    if(mode==2)CHECK(native_text_candidate_line(system,utf8_literal("AV office"),1000,1000,&retained.text,nullptr));
    const auto old_run=mode==2?retained.text.runs[0].native_face:NativeFontRunHandle{};
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    for(u32 sample=0;sample<7;++sample){
        Trial trial{};trial.original=system;trial.count=count;trial.mode=mode;
        const u64 identity=native_text_format_identity(system);
        trial.begin_cycles=cycles();trial.begin=ticks();
        const bool okay=native_text_context_rebuild(system,staged,&trial);
        const u64 end=ticks(),end_cycles=cycles();
        CHECK(okay==(mode!=0)&&trial.calls==1&&trial.clone_end>=trial.begin&&trial.replay_end>=trial.clone_end&&end>=trial.replay_end);
        CHECK((native_text_format_identity(system)!=identity)==(mode!=0));
        // Re-intern every declaration outside timing. Stable handles and fresh
        // identities must survive page boundaries and successive commits.
        for(u32 i=0;i<count;++i){u32 handle=0;CHECK(native_text_format_create(system.native_text,description(i),&handle)&&handle==i+1);auto alias=system;alias.native_format=handle;CHECK(native_text_format_identity(alias));}
        if(mode==2)CHECK(native_font_run_get(*native_font_session_runs(system.native_fonts),old_run));
        print("{\"kind\":\"native_recovery_cost\"");field("formats",count);field("mode",mode);field("sample",sample);field("frequency",frequency.QuadPart);
        field("clone_ticks",trial.clone_end-trial.begin);field("replay_ticks",trial.replay_end-trial.clone_end);field("retire_ticks",end-trial.replay_end);
        field("total_ticks",end-trial.begin);field("clone_cycles",trial.clone_cycles-trial.begin_cycles);field("total_cycles",end_cycles-trial.begin_cycles);print("}\n");
    }
    // Memory snapshots are a separate, untimed rebuild. The callback observes
    // both generations live; this is a checkpoint, not an OS peak counter.
    u64 before=0,before_ws=0,after=0,after_ws=0;CHECK(memory(&before,&before_ws));
    Trial trial{};trial.original=system;trial.count=count;trial.mode=mode;trial.sample_memory=true;
    CHECK(native_text_context_rebuild(system,staged,&trial)==(mode!=0)&&trial.calls==1&&memory(&after,&after_ws));
    print("{\"kind\":\"native_recovery_memory\"");field("formats",count);field("mode",mode);field("before_private",before);field("staged_private",trial.private_bytes);field("after_private",after);
    field("before_working_set",before_ws);field("staged_working_set",trial.working_set);field("after_working_set",after_ws);print("}\n");
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));return true;
}
}
