namespace path_stat_tests {
// Public-API reference stamp from an already open handle.
bool public_stamp(HANDLE file,NativeFontFileStamp* out) noexcept {
    *out={};FILE_STANDARD_INFO standard{};FILE_BASIC_INFO basic{};
    if(GetFileType(file)!=FILE_TYPE_DISK||!GetFileInformationByHandleEx(file,FileIdInfo,&out->identity,sizeof(out->identity))||
       !GetFileInformationByHandleEx(file,FileStandardInfo,&standard,sizeof(standard))||
       !GetFileInformationByHandleEx(file,FileBasicInfo,&basic,sizeof(basic))||standard.Directory||standard.EndOfFile.QuadPart<0)return false;
    out->bytes=static_cast<u64>(standard.EndOfFile.QuadPart);copy_bytes(&out->write,&basic.LastWriteTime,8);copy_bytes(&out->change,&basic.ChangeTime,8);return true;
}
bool path_stamp(const wchar_t* path,NativeFontFileStamp* out) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    const bool ok=public_stamp(file,out);CloseHandle(file);return ok;
}
// Explicit caller-owned ordinary/junction/symlink fixtures exercise the actual
// open, not a mocked flag result. Run once per path in a bounded child.
bool proof(const wchar_t* path,bool expected_proof) noexcept {
    const ConstSlice<wchar_t> input{path,length(path)};wchar_t scratch[512];
    NativeFontFileStamp expected{},actual{};CHECK(path_stamp(path,&expected));
    bool certified=true;HANDLE file=font_path_stat::open_read(input,{scratch,512},&certified);
    CHECK(file!=INVALID_HANDLE_VALUE);const bool same=font_path_stat::read_handle(file,&actual)&&bytes_equal(&actual,&expected,sizeof(actual));
    CHECK(CloseHandle(file)&&same&&certified==expected_proof);
    // Insufficient scratch must fall back without manufacturing a certificate.
    certified=true;file=font_path_stat::open_read(input,{scratch,1},&certified);
    CHECK(file!=INVALID_HANDLE_VALUE);CHECK(CloseHandle(file)&&!certified);
    print("open proof and exact stamp PASS\n");return true;
}
bool run() noexcept {
    wchar_t scratch[512];font_path_stat::Name prepared{};
    const auto view=[](const wchar_t* p) noexcept {return ConstSlice<wchar_t>{p,length(p)};};
    using Convert=LONG (WINAPI*)(const wchar_t*,font_path_stat::Name*,wchar_t**,void*);
    using Free=void (WINAPI*)(font_path_stat::Name*);
    const auto convert=reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlDosPathNameToNtPathName_U_WithStatus"));
    const auto free_name=reinterpret_cast<Free>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlFreeUnicodeString"));CHECK(convert&&free_name);
    const wchar_t* allowed[]{L"C:\\Windows\\Fonts\\arial.ttf",L"c:\\Font Space\\a.b.ttf",L"D:\\.hidden\\x",L"Z:\\COM10.ttf"};
    for(auto* p:allowed){const auto path=view(p);CHECK(font_path_stat::prepare(path,{scratch,512},&prepared)&&prepared.bytes==(path.count+4)*2&&
        bytes_equal(scratch,L"\\??\\",8)&&bytes_equal(scratch+4,p,path.count*2));
        font_path_stat::Name normalized{};CHECK(convert(p,&normalized,nullptr,nullptr)==0);
        const bool same=normalized.bytes==prepared.bytes&&bytes_equal(normalized.text,prepared.text,prepared.bytes);free_name(&normalized);CHECK(same);}
    // The guarded subset must preserve the public wrapper's actual conversion.
    wchar_t varied[]=L"C:\\Font Dir\\Alpha.beta.ttf";
    for(u32 at=3;at<length(varied);++at){const wchar_t saved=varied[at];
        for(wchar_t c=1;c<128;++c){varied[at]=c;if(!font_path_stat::prepare(view(varied),{scratch,512},&prepared))continue;
            font_path_stat::Name normalized{};CHECK(convert(varied,&normalized,nullptr,nullptr)==0);
            const bool same=normalized.bytes==prepared.bytes&&bytes_equal(normalized.text,prepared.text,prepared.bytes);free_name(&normalized);CHECK(same);}
        varied[at]=saved;
    }
    const wchar_t* declined[]{L"C:arial.ttf",L"\\Windows\\Fonts\\arial.ttf",L"\\\\server\\fonts\\a.ttf",L"\\\\?\\C:\\a.ttf",
        L"C:/a.ttf",L"C:\\a\\..\\b.ttf",L"C:\\a\\.\\b.ttf",L"C:\\a\\\\b.ttf",L"C:\\a.\\b.ttf",L"C:\\a \\b.ttf",L"C:\\a.ttf ",
        L"C:\\NUL",L"C:\\nul.ttf",L"C:\\NUL .ttf",L"C:\\CON\\a.ttf",L"C:\\COM0.ttf",L"C:\\lpt9.otf",L"C:\\CONIN$",L"C:\\CLOCK$",
        L"C:\\COM\u00b9.ttf",L"C:\\caf\u00e9.ttf",L"C:\\x:stream",L"C:\\*",L"C:\\?",L"C:\\x|y",L"C:\\x<y",L"C:\\x>y",L"C:\\x\"y",L"C:\\"};
    for(auto* p:declined){scratch[0]=123;CHECK(!font_path_stat::prepare(view(p),{scratch,512},&prepared)&&!prepared.text&&scratch[0]==123);}
    const auto ordinary=view(allowed[0]);
    for(u32 size=0;size<ordinary.count+4;++size)CHECK(!font_path_stat::prepare(ordinary,{scratch,size},&prepared));
    CHECK(!font_path_stat::prepare(ordinary,{nullptr,512},&prepared)&&!font_path_stat::prepare(ordinary,{scratch,512},nullptr));
    copy_bytes(scratch+4,ordinary.data,(ordinary.count+1)*2);
    CHECK(!font_path_stat::prepare({scratch+4,ordinary.count},{scratch,512},&prepared)&&bytes_equal(scratch+4,ordinary.data,(ordinary.count+1)*2));
    CHECK(!font_path_stat::prepare({scratch+4,ordinary.count},{scratch+ordinary.count+4,40},&prepared));
    wchar_t long_path[261];copy_bytes(long_path,L"C:\\",6);for(u32 i=3;i<260;++i)long_path[i]=L'a';long_path[260]=0;
    CHECK(font_path_stat::prepare({long_path,259},{scratch,512},&prepared)&&!font_path_stat::prepare({long_path,260},{scratch,512},&prepared));
    Temporary temp;CHECK(temp.open());const u8 data[]{1,2,3};CHECK(write(temp.paths[0],{data,3}));
    const auto file=view(temp.paths[0]);NativeFontFileStamp expected{};CHECK(path_stamp(file.data,&expected));
    NativeFontFileStamp stamp{};
    const u32 scratch_sizes[]{0,1,512};
    for(u32 available:scratch_sizes){
        bool certified=true;HANDLE opened=font_path_stat::open_read(file,{scratch,available},&certified);CHECK(opened!=INVALID_HANDLE_VALUE);
        CHECK(available==512||!certified);
        NativeFontFileStamp observed{};const bool same=font_path_stat::read_handle(opened,&observed)&&bytes_equal(&observed,&expected,sizeof(expected));
        u8 bytes[3]{};DWORD read=0;const bool loaded=ReadFile(opened,bytes,3,&read,nullptr)!=0;
        HANDLE blocked=CreateFileW(file.data,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        const u32 blocked_error=blocked==INVALID_HANDLE_VALUE?GetLastError():0;if(blocked!=INVALID_HANDLE_VALUE)CloseHandle(blocked);
        const bool closed=CloseHandle(opened)!=0;CHECK(same&&loaded&&read==3&&bytes_equal(bytes,data,3)&&closed&&blocked_error==ERROR_SHARING_VIOLATION);
    }
    // Compare real Win32-normalized variants with direct mode's fallback.
    wchar_t alias[512];copy_bytes(alias,file.data,(file.count+1)*2);alias[file.count]=L' ';alias[file.count+1]=0;
    bool certified=true;HANDLE normalized=font_path_stat::open_read(view(alias),{scratch,512},&certified);CHECK(normalized!=INVALID_HANDLE_VALUE&&!certified);
    const bool same_open=font_path_stat::read_handle(normalized,&stamp)&&bytes_equal(&stamp,&expected,sizeof(stamp));CHECK(CloseHandle(normalized)&&same_open);
    copy_bytes(scratch,file.data,(file.count+1)*2);
    certified=true;HANDLE aliased=font_path_stat::open_read({scratch,file.count},{scratch,512},&certified);CHECK(aliased!=INVALID_HANDLE_VALUE&&!certified);
    const bool intact=bytes_equal(scratch,file.data,(file.count+1)*2);CHECK(CloseHandle(aliased)&&intact);
    HANDLE writer=CreateFileW(file.data,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);CHECK(writer!=INVALID_HANDLE_VALUE);
    certified=true;HANDLE denied=font_path_stat::open_read(file,{scratch,512},&certified);const u32 denied_error=denied==INVALID_HANDLE_VALUE?GetLastError():0;
    if(denied!=INVALID_HANDLE_VALUE)CloseHandle(denied);CHECK(denied==INVALID_HANDLE_VALUE&&denied_error==ERROR_SHARING_VIOLATION&&!certified);
    CHECK(CloseHandle(writer)&&DeleteFileW(file.data));
    certified=true;CHECK(font_path_stat::open_read(file,{scratch,512},&certified)==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_FILE_NOT_FOUND&&!certified);
    return true;
}
}
