// Exercise the public shape dispatcher while the private catalog names an old
// file generation. Unsupported CFF bold makes staging fail deterministically.
volatile u32 request_recovery_phase;
__declspec(noinline) void request_recovery_marker(u32 phase) noexcept {request_recovery_phase=phase;}
bool native_request_recovery() noexcept {
    struct Files {
        wchar_t reserved[MAX_PATH]{},live[MAX_PATH]{},retired[MAX_PATH]{},file[MAX_PATH]{},old_file[MAX_PATH]{};
        ~Files() noexcept {if(*file)DeleteFileW(file);if(*old_file)DeleteFileW(old_file);if(*live)RemoveDirectoryW(live);if(*retired)RemoveDirectoryW(retired);if(*reserved)DeleteFileW(reserved);}
    } files;
    wchar_t temp[MAX_PATH]{};const u32 n=GetTempPathW(MAX_PATH,temp);CHECK(n&&n<MAX_PATH&&GetTempFileNameW(temp,L"mrr",0,files.reserved));
    u32 length=0;while(files.reserved[length])++length;CHECK(length+16<MAX_PATH);
    copy_bytes(files.live,files.reserved,length*2);copy_bytes(files.live+length,L".live",12);
    copy_bytes(files.retired,files.reserved,length*2);copy_bytes(files.retired+length,L".old",10);
    copy_bytes(files.file,files.live,(length+5)*2);copy_bytes(files.file+length+5,L"\\font.otf",20);
    copy_bytes(files.old_file,files.retired,(length+4)*2);copy_bytes(files.old_file+length+4,L"\\font.otf",20);
    CHECK(CreateDirectoryW(files.live,nullptr)&&CopyFileW(L"assets/fonts/Compagnon.otf",files.file,TRUE));
    DWriteTextSystem system{};
    struct Release {DWriteTextSystem& s;~Release() noexcept {shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);text_system_release(&s);}} release{system};
    CHECK(native_font_session_create(&system.native_fonts)&&native_text_context_initialize(&system.native_text,system.native_fonts));system.em_size=16;
    Output* outputs[]{&native_output,&reference_output};for(auto* out:outputs){
        CHECK(shaped_text_initialize(&out->text,{out->glyphs,1024},{out->runs,256},{out->clusters,256},{out->decorations,256},{out->hits,4096}));
    }
    u32 scope=0;CHECK(native_text_register_font_path(system.native_text,files.file,&scope));
    const u16 missing[]{'N','o','-','M','a','i','C','o','r','d','-','F','a','m','i','l','y'},family[]{'C','o','m','p','a','g','n','o','n'};
    CHECK(native_text_format_create(system.native_text,{{missing,17},scope,{400,5,0},16,64,true},&system.native_format));
    auto alias=system;const u64 identity=native_text_format_identity(alias);
    const auto text=utf8_literal("AV office words that wrap across several lines");
    CHECK(text_shape_utf8(&system,text,{},1000,1000,{utf16,256},&reference_output.text).ok());
    const auto old_run=reference_output.text.runs[0].native_face;auto* fonts=system.native_fonts;
    CHECK(MoveFileExW(files.live,files.retired,0)&&CreateDirectoryW(files.live,nullptr)&&CopyFileW(L"assets/fonts/Compagnon.otf",files.file,TRUE));
    CHECK(native_text_format_create(system.native_text,{{family,9},scope,{600,5,0},16,64,true},&system.native_format));
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    const TextStyleSpan style{0,text.bytes,text_style_underline};
    for(u32 mode=0;mode<3;++mode){request_recovery_marker(mode+1);
        u64 samples[7][2]{};
        for(u32 i=0;i<7;++i){LARGE_INTEGER first{},last{};ULONG64 before=0,after=0;
            CHECK(QueryThreadCycleTime(GetCurrentThread(),&before)&&QueryPerformanceCounter(&first));
            const auto result=text_shape_utf8(&system,text,mode==1?ConstSlice<TextStyleSpan>{&style,1}:ConstSlice<TextStyleSpan>{},60,20,{utf16,256},&native_output.text,TextWrapping::wrap,mode==2?TextOverflow::clip:TextOverflow::ellipsis);
            CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&after));
            CHECK(!result.ok()&&result.error==TextShapeError::unsupported&&!native_output.text.native_fonts()&&!native_output.text.run_count);
            CHECK(native_text_format_identity(alias)==identity&&native_font_run_get(*native_font_session_runs(fonts),old_run));
            samples[i][0]=last.QuadPart-first.QuadPart;samples[i][1]=after-before;
        }
        print("{\"kind\":\"request_recovery\",\"mode\":");number(mode);print(",\"frequency\":");number(frequency.QuadPart);print(",\"samples\":[");
        for(u32 i=0;i<7;++i){if(i)print(",");print("[");number(samples[i][0]);print(",");number(samples[i][1]);print("]");}print("]}\n");
    }
    request_recovery_marker(4);
    CHECK(native_text_format_create(system.native_text,{{family,9},scope,{400,5,0},16,64,true},&system.native_format));
    CHECK(text_shape_utf8(&system,text,{},60,20,{utf16,256},&native_output.text,TextWrapping::wrap,TextOverflow::ellipsis).ok());
    CHECK(native_text_format_identity(alias)!=identity&&system.native_fonts==fonts&&native_font_run_get(*native_font_session_runs(fonts),old_run)&&native_output.text.run_count);
    request_recovery_marker(5);
    print("PASS public request recovery: plain/styled ellipsis, clip, repeated rejection, later commit, retained old run\n");return true;
}
