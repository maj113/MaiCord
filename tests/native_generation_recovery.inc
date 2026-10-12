// Every buffer retained across retirement is disjoint from the next output.
struct GenerationOutput {
    TextGlyph glyphs[256];TextRun runs[128];u16 clusters[256];ShapedText text{};
    GenerationOutput() noexcept {text.glyphs=glyphs;text.glyph_capacity=256;text.runs=runs;text.run_capacity=128;text.clusters=clusters;text.cluster_capacity=256;}
    ~GenerationOutput() noexcept {release_output(text);}
};
volatile usize recovery_observation;
volatile u32 recovery_expect_mismatch=0;
__declspec(noinline) void recovery_marker(const void* root,const void* backing,u32 phase) noexcept {recovery_observation=reinterpret_cast<usize>(root)^reinterpret_cast<usize>(backing)^phase;}
bool recovery_format_sharing() noexcept {
    for(u32 large_first=0;large_first<2;++large_first){
        TextSystem system{};CHECK(native_font_session_create(&system.native_fonts)&&native_text_context_initialize(&system.native_text,system.native_fonts));
        struct Release {TextSystem& s;~Release() noexcept {native_text_context_release(&s.native_text);native_font_session_release(&s.native_fonts);}} release{system};
        u16 names[7][255]{};NativeTextFormatDescription descriptions[140]{};u64 identities[140]{};
        for(u32 n=0;n<7;++n)for(auto& unit:names[n])unit=static_cast<u16>('A'+n);
        for(u32 i=0;i<140;++i){const u32 family=i%7,units=family?8u+family:(large_first?255u:8u);
            descriptions[i]={{names[family],units},0,{static_cast<u16>(400+100*((i/7)%3)),5,0},16.f+i,64.f+(i%5),!!(i%2)};
            u32 handle=0;CHECK(native_text_format_create(system.native_text,descriptions[i],&handle)&&handle==i+1);}
        system.native_format=1;system.em_size=16;
        struct Trial {NativeTextFormatDescription* descriptions;bool accept;} trial{descriptions,true};
        for(u32 pass=0;pass<3;++pass){
            for(u32 i=0;i<140;++i){auto alias=system;alias.native_format=i+1;identities[i]=native_text_format_identity(alias);CHECK(identities[i]);}
            trial.accept=pass!=1;
            const bool accepted=native_text_context_rebuild(system,[](const TextSystem& staged,void* opaque) noexcept {
                auto& trial=*static_cast<Trial*>(opaque);
                for(u32 i=0;i<140;++i){u32 handle=0;CHECK(native_text_format_create(staged.native_text,trial.descriptions[i],&handle)&&handle==i+1);}
                return trial.accept;
            },&trial);
            CHECK(accepted==trial.accept);
            for(u32 i=0;i<140;++i){u32 handle=0;CHECK(native_text_format_create(system.native_text,descriptions[i],&handle)&&handle==i+1);
                auto alias=system;alias.native_format=handle;CHECK((native_text_format_identity(alias)!=identities[i])==accepted);}
        }
    }
    print("PASS recovery family sharing: interleaved names/properties, 255-unit first name, page boundaries, rollback, repeated commits\n");return true;
}
bool generation_recovery(const wchar_t* path) noexcept {
    TextSystem system{};CHECK(native_font_session_create(&system.native_fonts)&&native_text_context_initialize(&system.native_text,system.native_fonts));system.em_size=16;
    struct Release {TextSystem& s;~Release() noexcept {native_text_context_release(&s.native_text);native_font_session_release(&s.native_fonts);}} release{system};
    const u16 family[]{'S','e','g','o','e',' ','U','I'},private_name[]{'C','o','m','p','a','g','n','o','n'};
    for(u32 i=0;i<130;++i){u32 handle=0;CHECK(native_text_format_create(system.native_text,{{family,8},0,{400,5,0},16.f+i,64,true},&handle)&&handle==i+1);}
    for(u32 i=1;i<29;++i){wchar_t unused[]{L'x',static_cast<wchar_t>(L'A'+i),0};u32 scope=0;CHECK(native_text_register_font_path(system.native_text,unused,&scope)&&scope==i);}
    u32 first=0,second=0;CHECK(native_text_register_font_path(system.native_text,L"duplicate-private-path-a",&first)&&first==29&&
          native_text_register_font_path(system.native_text,L"duplicate-private-path-b",&second)&&second==30);
    u32 scope=0;CHECK(native_text_register_font_path(system.native_text,path,&scope)&&scope==31);
    CHECK(native_text_format_create(system.native_text,{{private_name,9},31,{400,5,0},16,64,true},&system.native_format)&&system.native_format==131);
    auto alias=system;alias.native_format=1;const auto before_id=native_text_format_identity(alias);const auto private_id=native_text_format_identity(system);auto* root=system.native_text;
    GenerationOutput old,draw;CHECK(native_text_candidate_line(alias,utf8_literal("AV office"),1000,1000,&old.text,nullptr));
    auto* old_fonts=old.text.native_fonts();const auto old_run=old.text.runs[0].native_face;CHECK(native_font_run_get(*native_font_session_runs(old_fonts),old_run));
    struct Trial {ShapedText* output;const void* staged=nullptr;u32 calls=0;bool accept=false;} trial{&draw.text};
    const auto attempt=[](const TextSystem& staged,void* opaque) noexcept {
        auto& t=*static_cast<Trial*>(opaque);++t.calls;t.staged=staged.native_text;
        CHECK(!native_text_context_rebuild(staged,[](const TextSystem&,void*) noexcept {return true;},nullptr));
        if(!t.accept)return false;
        return native_text_candidate_line(staged,utf8_literal("A \xce\xb1\xf0\x9f\x98\x80\xd0\x91 To"),1000,1000,t.output,nullptr);
    };
    CHECK(!native_text_context_rebuild(system,attempt,&trial)&&trial.calls==1&&system.native_text==root&&native_text_format_identity(alias)==before_id&&native_text_format_identity(system)==private_id);
    MEMORY_BASIC_INFORMATION region{};CHECK(VirtualQuery(trial.staged,&region,sizeof(region))==sizeof(region)&&region.State==MEM_FREE);
    trial.accept=true;CHECK(native_text_context_rebuild(system,attempt,&trial)&&trial.calls==2&&system.native_text==root&&alias.native_text==root);
    const auto* first_backing=trial.staged;recovery_marker(root,first_backing,1);
    CHECK(native_text_format_identity(alias)!=before_id&&native_text_format_identity(system)!=private_id);
    CHECK(draw.text.run_count&&draw.text.native_fonts()==old_fonts&&native_font_run_get(*native_font_session_runs(old_fonts),old_run));
    CHECK(VirtualQuery(root,&region,sizeof(region))==sizeof(region)&&region.State==MEM_COMMIT&&region.RegionSize==4096);
    CHECK(VirtualQuery(first_backing,&region,sizeof(region))==sizeof(region)&&region.State==MEM_COMMIT);
    release_output(draw.text);draw.text=output();
    // Rebased first-format pointers, overflow index pages, private scope 31 and
    // borrowed coverage all remain usable through the original context aliases.
    const u32 handles[]{1,64,65,129,130,131};for(u32 handle:handles){
        auto current=system;current.native_format=handle;current.em_size=handle==131?16.f:15.f+handle;TextMeasureResult result{};
        CHECK(native_text_candidate_line(current,utf8_literal("AV office"),1000,1000,nullptr,&result)&&result.ok());
    }
    const auto changed_id=native_text_format_identity(alias);trial.accept=false;
    CHECK(!native_text_context_rebuild(alias,attempt,&trial)&&native_text_format_identity(alias)==changed_id&&native_font_run_get(*native_font_session_runs(old_fonts),old_run));
    trial.accept=true;CHECK(native_text_context_rebuild(alias,attempt,&trial));recovery_marker(root,trial.staged,2);
    CHECK(VirtualQuery(first_backing,&region,sizeof(region))==sizeof(region)&&region.State==MEM_FREE&&native_text_format_identity(alias)!=changed_id);
    release_output(draw.text);draw.text=output();CHECK(exercise(alias));
    native_text_context_release(&system.native_text);CHECK(!system.native_text&&native_font_run_get(*native_font_session_runs(old_fonts),old_run));
    CHECK(VirtualQuery(root,&region,sizeof(region))==sizeof(region)&&region.State==MEM_FREE);
    CHECK(VirtualQuery(trial.staged,&region,sizeof(region))==sizeof(region)&&region.State==MEM_FREE);
    print("PASS transactional generation: 131 formats, 32 scopes, aliases, rollback, two commits, retirement, old output\n");return true;
}
struct ReplacementFont {
    wchar_t reserved[MAX_PATH]{},live[MAX_PATH]{},retired[MAX_PATH]{},file[MAX_PATH]{},old_file[MAX_PATH]{};
    ~ReplacementFont() noexcept {if(*file)DeleteFileW(file);if(*old_file)DeleteFileW(old_file);if(*live)RemoveDirectoryW(live);if(*retired)RemoveDirectoryW(retired);if(*reserved)DeleteFileW(reserved);}
    bool open(const wchar_t* source) noexcept {
        wchar_t temp[MAX_PATH];const u32 n=GetTempPathW(MAX_PATH,temp);if(!n||n>=MAX_PATH||!GetTempFileNameW(temp,L"mgr",0,reserved))return false;
        u32 size=0;while(reserved[size])++size;if(size+16>=MAX_PATH)return false;
        copy_bytes(live,reserved,size*2);copy_bytes(live+size,L".live",12);copy_bytes(retired,reserved,size*2);copy_bytes(retired+size,L".old",10);
        copy_bytes(file,live,(size+5)*2);copy_bytes(file+size+5,L"\\font.otf",20);copy_bytes(old_file,retired,(size+4)*2);copy_bytes(old_file+size+4,L"\\font.otf",20);
        return CreateDirectoryW(live,nullptr)&&CopyFileW(source,file,TRUE);
    }
    bool replace(const wchar_t* source) noexcept {CHECK(MoveFileExW(live,retired,0));CHECK(CreateDirectoryW(live,nullptr));CHECK(CopyFileW(source,file,TRUE));return true;}
};
bool automatic_generation_recovery(const wchar_t* path) noexcept {
    ReplacementFont temp;CHECK(temp.open(path));TextSystem system{};CHECK(native_font_session_create(&system.native_fonts)&&native_text_context_initialize(&system.native_text,system.native_fonts));system.em_size=16;
    struct Release {TextSystem& s;~Release() noexcept {native_text_context_release(&s.native_text);native_font_session_release(&s.native_fonts);}} release{system};
    u32 scope=0;CHECK(native_text_register_font_path(system.native_text,temp.file,&scope));const u16 family[]{'C','o','m','p','a','g','n','o','n'};
    const u16 missing[]{'N','o','-','M','a','i','C','o','r','d','-','F','a','m','i','l','y'};
    // Build the private catalog through a missing-family request. Its output
    // uses system fallback, leaving the unselected private file replaceable.
    CHECK(native_text_format_create(system.native_text,{{missing,17},scope,{400,5,0},16,64,true},&system.native_format));
    auto alias=system;const auto identity=native_text_format_identity(alias);auto* root=system.native_text;
    GenerationOutput old,draw;CHECK(native_text_candidate_line(system,utf8_literal("AV office"),1000,1000,&old.text,nullptr));
    const auto old_run=old.text.runs[0].native_face;auto* old_fonts=old.text.native_fonts();
    CHECK(temp.replace(path));
    // Existing CFF bold simulation is unsupported. Failed staged preparation
    // must keep the original generation and independently retained output.
    CHECK(native_text_format_create(system.native_text,{{family,9},scope,{600,5,0},16,64,true},&system.native_format));
    CHECK(!native_text_candidate_line(system,utf8_literal("AV office"),1000,1000,&draw.text,nullptr)&&!draw.text.native_fonts()&&!draw.text.run_count);
    CHECK(native_text_format_identity(alias)==identity&&native_font_run_get(*native_font_session_runs(old_fonts),old_run));
    CHECK(native_text_format_create(system.native_text,{{family,9},scope,{400,5,0},16,64,true},&system.native_format));
    const bool recovered=native_text_candidate_line(system,utf8_literal("AV office"),1000,1000,&draw.text,nullptr);
    if(recovery_expect_mismatch){
        CHECK(!recovered&&!draw.text.native_fonts()&&!draw.text.run_count&&native_text_format_identity(alias)==identity&&native_font_run_get(*native_font_session_runs(old_fonts),old_run));
        print("PASS injected second acquisition mismatch: bounded failure, original generation and output retained\n");return true;
    }
    CHECK(recovered&&draw.text.run_count&&draw.text.native_fonts()==old_fonts);
    CHECK(system.native_text==root&&alias.native_text==root&&native_text_format_identity(alias)!=identity&&native_font_run_get(*native_font_session_runs(old_fonts),old_run));
    const auto* old_value=native_font_run_get(*native_font_session_runs(old_fonts),old_run);
    const auto* new_value=native_font_run_get(*native_font_session_runs(draw.text.native_fonts()),draw.text.runs[0].native_face);CHECK(old_value&&new_value);
    const auto* a=native_font_session_fonts(old_fonts);const auto* b=native_font_session_fonts(draw.text.native_fonts());
    const auto& a_file=a->files[a->faces[old_value->source.face.slot-1].file_slot];const auto& b_file=b->files[b->faces[new_value->source.face.slot-1].file_slot];
    CHECK(!bytes_equal(&a_file.identity,&b_file.identity,sizeof(a_file.identity)));
    TextMeasureResult measured{};CHECK(native_text_candidate_line(alias,utf8_literal("AV office"),1000,1000,nullptr,&measured)&&measured.ok());
    CHECK(measured.width==old.text.width&&measured.height==old.text.height);
    print("PASS automatic stale acquisition recovery after unselected ancestor replacement; old output retained, fallback geometry exact\n");return true;
}
