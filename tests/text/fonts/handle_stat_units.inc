namespace handle_stat_tests {
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
// Handles the one-query path must refuse leave the output untouched.
bool refused(HANDLE handle) noexcept {
    NativeFontFileStamp out{};out.bytes=123;return !font_path_stat::read_handle(handle,&out)&&out.bytes==123;
}
bool run(bool force_fallback=false) noexcept {
    wchar_t path[1024]{};u32 n=GetWindowsDirectoryW(path,900);CHECK(n&&n<900);
    const DWORD share=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
    HANDLE directory=CreateFileW(path,FILE_READ_ATTRIBUTES,share,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);CHECK(directory!=INVALID_HANDLE_VALUE);
    const bool directory_refused=refused(directory);CloseHandle(directory);CHECK(directory_refused);
    HANDLE null_device=CreateFileW(L"NUL",GENERIC_READ,share,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(null_device!=INVALID_HANDLE_VALUE);
    const bool device_refused=refused(null_device);CloseHandle(null_device);CHECK(device_refused);
    Temporary temporary;CHECK(temporary.open());CHECK(file_case(temporary.paths[0],force_fallback,false));
    const u8 bytes[]{1,2,3};CHECK(write(temporary.paths[0],{bytes,3})&&file_case(temporary.paths[0],force_fallback,false));
    copy_bytes(path+n,L"\\Fonts\\segoeui.ttf",sizeof(L"\\Fonts\\segoeui.ttf"));
    CHECK(file_case(path,force_fallback,true));
    n=GetModuleFileNameW(nullptr,path,900);CHECK(n&&n<900);while(n&&path[n-1]!=L'\\')--n;CHECK(n);copy_bytes(path+n,L"fonts\\BioRhyme.ttf",sizeof(L"fonts\\BioRhyme.ttf"));
    CHECK(file_case(path,force_fallback,true));
    print("handle stat guards, acquisition and read-locked stamps PASS\n");return true;
}
}
