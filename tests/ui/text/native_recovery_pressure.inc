volatile u32 pressure_expect_commit_failure;
bool recovery_session_pressure(const wchar_t* path) noexcept {
    ReplacementFont replacement;CHECK(replacement.open(path));
    TextSystem system{};CHECK(native_font_session_create(&system.native_fonts)&&native_text_context_initialize(&system.native_text,system.native_fonts));system.em_size=16;
    struct Release {TextSystem& s;~Release() noexcept {native_text_context_release(&s.native_text);native_font_session_release(&s.native_fonts);}} release{system};
    u32 scope=0;CHECK(native_text_register_font_path(system.native_text,replacement.file,&scope));
    const u16 missing[]{'N','o','-','M','a','i','C','o','r','d','-','F','a','m','i','l','y'},family[]{'C','o','m','p','a','g','n','o','n'};
    CHECK(native_text_format_create(system.native_text,{{missing,17},scope,{400,5,0},16,64,true},&system.native_format));
    GenerationOutput old,draw;CHECK(native_text_candidate_line(system,utf8_literal("AV office"),1000,1000,&old.text,nullptr));
    auto alias=system;const u64 identity=native_text_format_identity(alias);const auto old_run=old.text.runs[0].native_face;
    auto* session=system.native_fonts;const auto initial=native_font_session_stats(session);CHECK(initial.live.faces==1&&initial.instances==1);
    struct Pressure {
        NativeFontSession* session;wchar_t paths[63][MAX_PATH]{};
        NativeFontHandle faces[63]{};NativeFontInstanceHandle instances[63]{};NativeFontRunHandle runs[63]{};
        ~Pressure() noexcept {for(u32 i=0;i<63;++i){
            if(runs[i].slot)released=native_font_run_release(native_font_session_runs(session),runs[i])&&released;
            if(instances[i].slot)released=native_font_instance_release(native_font_session_instances(session),instances[i])&&released;
            if(faces[i].slot)released=native_font_release(native_font_session_fonts(session),faces[i])&&released;
            if(paths[i][0])released=(DeleteFileW(paths[i])!=FALSE)&&released;
        }}
    } pressure{session};
    wchar_t temp[MAX_PATH]{};const u32 units=GetTempPathW(MAX_PATH,temp);CHECK(units&&units<MAX_PATH);
    const NativeFontFace* first_face=nullptr;const NativeFontInstance* first_instance=nullptr;const NativeFontRun* first_run=nullptr;
    for(u32 i=0;i<63;++i){CHECK(GetTempFileNameW(temp,L"mfp",0,pressure.paths[i])&&CopyFileW(path,pressure.paths[i],FALSE));
        CHECK(native_font_session_face(session,pressure.paths[i],0,&pressure.faces[i]).ok());
        CHECK(native_font_session_instance(session,pressure.faces[i],{},false,{},&pressure.instances[i]).ok());
        NativeFontSelection source{};source.face=pressure.faces[i];CHECK(native_font_session_run(session,pressure.instances[i],source,0,&pressure.runs[i]).ok());
        if(!i){first_face=native_font_get(*native_font_session_fonts(session),pressure.faces[0]);first_instance=native_font_instance_get(*native_font_session_instances(session),pressure.instances[0]);first_run=native_font_run_get(*native_font_session_runs(session),pressure.runs[0]);}
    }
    const auto full=native_font_session_stats(session);CHECK(full.live.files==64&&full.live.faces==64&&full.instances==64);
    MEMORY_BASIC_INFORMATION region{};CHECK(VirtualQuery(session,&region,sizeof(region))==sizeof(region)&&region.State==MEM_COMMIT&&region.RegionSize==4096);
    CHECK(VirtualQuery(reinterpret_cast<u8*>(session)+4096,&region,sizeof(region))==sizeof(region)&&region.State==MEM_RESERVE&&region.AllocationBase==session);
    const auto* run_store=native_font_session_runs(session);CHECK(run_store->slots.count==64);for(u32 i=0;i<run_store->slots.count;++i)CHECK(run_store->slots[i].references);
    CHECK(replacement.replace(path));
    CHECK(native_text_format_create(system.native_text,{{family,9},scope,{400,5,0},16,64,true},&system.native_format));
    recovery_marker(system.native_text,session,64);
    const bool recovered=native_text_candidate_line(system,utf8_literal("AV office"),1000,1000,&draw.text,nullptr);
    if(pressure_expect_commit_failure){
        CHECK(!recovered&&!draw.text.native_fonts()&&!draw.text.run_count&&native_text_format_identity(alias)==identity);
        CHECK(native_font_run_get(*native_font_session_runs(session),old_run)&&native_font_session_stats(session).live.faces==64);
        CHECK(VirtualQuery(session,&region,sizeof(region))==sizeof(region)&&region.State==MEM_COMMIT&&region.RegionSize==4096);
        CHECK(native_font_run_release(native_font_session_runs(session),pressure.runs[62]));pressure.runs[62]={};
        CHECK(native_font_instance_release(native_font_session_instances(session),pressure.instances[62]));pressure.instances[62]={};
        CHECK(native_font_release(native_font_session_fonts(session),pressure.faces[62]));pressure.faces[62]={};
        CHECK(native_text_candidate_line(system,utf8_literal("AV office"),1000,1000,&draw.text,nullptr));
    }else CHECK(recovered);
    CHECK(draw.text.run_count&&draw.text.native_fonts()==session&&native_text_format_identity(alias)!=identity);
    CHECK(native_font_run_get(*native_font_session_runs(session),old_run));
    CHECK(native_font_get(*native_font_session_fonts(session),pressure.faces[0])==first_face&&
        native_font_instance_get(*native_font_session_instances(session),pressure.instances[0])==first_instance&&
        native_font_run_get(*native_font_session_runs(session),pressure.runs[0])==first_run);
    const u32 live=pressure_expect_commit_failure?64u:65u;
    const auto grown=native_font_session_stats(session);CHECK(grown.live.faces==live&&grown.instances==live&&native_font_session_runs(session)->slots.count>=live);
    CHECK(VirtualQuery(session,&region,sizeof(region))==sizeof(region)&&region.State==MEM_COMMIT&&region.RegionSize==(pressure_expect_commit_failure?4096u:8192u));
    recovery_marker(system.native_text,session,65);
    print(pressure_expect_commit_failure?"PASS injected file-prefix commit failure: rollback, retained output, and recovery after releasing one slot\n":
        "PASS generation recovery with 64 occupied file/face/instance/run slots; all retained views and old output survive\n");return true;
}
