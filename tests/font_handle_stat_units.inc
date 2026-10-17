namespace handle_stat_tests {
u32 calls=0,attributes=0,device=7,returned=104;LONG status=0;i64 size=17;
LONG WINAPI query(HANDLE file,font_path_stat::Io* io,void* buffer,u32 bytes,u32 kind) noexcept {
    ++calls;if(file!=reinterpret_cast<HANDLE>(17)||bytes!=104||kind!=77)return static_cast<LONG>(0xc000000du);
    auto& s=*static_cast<font_path_stat::Stat*>(buffer);s={};s.bytes=size;s.device_type=device;s.attributes=attributes;
    s.volume=31;s.write=37;s.change=41;for(u32 i=0;i<16;++i)s.identity.Identifier[i]=static_cast<u8>(i+1);
    io->bytes=returned;return status;
}
// Public-API reference stamp from an already open handle.
bool public_stamp(HANDLE file,NativeFontFileStamp* out) noexcept {
    *out={};FILE_STANDARD_INFO standard{};FILE_BASIC_INFO basic{};
    if(GetFileType(file)!=FILE_TYPE_DISK||!GetFileInformationByHandleEx(file,FileIdInfo,&out->identity,sizeof(out->identity))||
       !GetFileInformationByHandleEx(file,FileStandardInfo,&standard,sizeof(standard))||
       !GetFileInformationByHandleEx(file,FileBasicInfo,&basic,sizeof(basic))||standard.Directory||standard.EndOfFile.QuadPart<0)return false;
    out->bytes=static_cast<u64>(standard.EndOfFile.QuadPart);copy_bytes(&out->write,&basic.LastWriteTime,8);copy_bytes(&out->change,&basic.ChangeTime,8);return true;
}
bool file_case(const wchar_t* path,bool force_fallback,bool font) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    struct Close {HANDLE file;~Close() noexcept {CloseHandle(file);}} close{file};
    NativeFontFileStamp expected{},actual{};CHECK(public_stamp(file,&expected));
    const bool projected=font_path_stat::read_handle(file,&actual);
    CHECK(!projected||bytes_equal(&actual,&expected,sizeof(actual)));if(force_fallback)CHECK(!projected);
    if(font){
        NativeFontFileSlot files[2]{};NativeFontFaceSlot faces[2]{};NativeFontStore store{{files,2},{faces,2}};NativeFontHandle face{};
        CHECK(native_font_acquire(&store,path,0,&face).ok());
        const auto* opened=native_font_get(store,face);CHECK(opened&&files[0].bytes.count==expected.bytes&&bytes_equal(&files[0].identity,&expected.identity,sizeof(expected.identity)));
        CHECK(native_font_release(&store,face)&&!native_font_stats(store).files);
    }
    print(projected?"handle stat native identity PASS\n":"handle stat public fallback identity PASS\n");return true;
}
bool run(bool force_fallback=false) noexcept {
    NativeFontFileStamp out{},zero{};const HANDLE fake=reinterpret_cast<HANDLE>(17);
    CHECK(font_path_stat::read_handle(query,fake,&out)&&out.identity.VolumeSerialNumber==31&&out.bytes==17&&out.write.dwLowDateTime==37&&out.change.dwLowDateTime==41);
    for(u32 i=0;i<16;++i)CHECK(out.identity.FileId.Identifier[i]==i+1);
    const auto decline=[&](font_path_stat::HandleQuery q=query,HANDLE handle=reinterpret_cast<HANDLE>(17)) noexcept {
        out.bytes=123;return !font_path_stat::read_handle(q,handle,&out)&&bytes_equal(&out,&zero,sizeof(out));
    };
    calls=0;CHECK(decline(nullptr)&&decline(query,nullptr)&&decline(query,INVALID_HANDLE_VALUE)&&!calls);
    CHECK(!font_path_stat::read_handle(query,fake,nullptr)&&!calls);
    const u32 lengths[]{0,72,103,105},statuses[]{0x103,0xc0000003,0xc000000d};
    for(u32 n: lengths){returned=n;CHECK(decline());}returned=104;
    for(u32 code: statuses){status=static_cast<LONG>(code);CHECK(decline());}status=0;
    device=3;CHECK(decline());device=7;size=-1;CHECK(decline());size=0;CHECK(font_path_stat::read_handle(query,fake,&out)&&!out.bytes);size=17;
    attributes=FILE_ATTRIBUTE_DIRECTORY;CHECK(decline());attributes=FILE_ATTRIBUTE_REPARSE_POINT;CHECK(decline());attributes=0;
    Temporary temporary;CHECK(temporary.open());CHECK(file_case(temporary.paths[0],force_fallback,false));
    const u8 bytes[]{1,2,3};CHECK(write(temporary.paths[0],{bytes,3})&&file_case(temporary.paths[0],force_fallback,false));
    wchar_t path[1024]{};u32 n=GetWindowsDirectoryW(path,900);CHECK(n&&n<900);copy_bytes(path+n,L"\\Fonts\\segoeui.ttf",sizeof(L"\\Fonts\\segoeui.ttf"));
    CHECK(file_case(path,force_fallback,true));
    n=GetModuleFileNameW(nullptr,path,900);CHECK(n&&n<900);while(n&&path[n-1]!=L'\\')--n;CHECK(n);copy_bytes(path+n,L"fonts\\BioRhyme.ttf",sizeof(L"fonts\\BioRhyme.ttf"));
    CHECK(file_case(path,force_fallback,true));
    print("handle stat guards, acquisition and read-locked stamps PASS\n");return true;
}
}
