// Test-only catalog loader over the live public font APIs. Production catalogs
// are built per face by src/ui; this fixture builds the same single-face record.
// default_only keeps the default axis instance. Requires <Windows.h> and
// maicord/text/font_catalog.hpp.
struct CatalogFixture {
    NativeFontCatalog catalog{};
    wchar_t path[512]{};u16 name[128]{};NativeFontProperties properties[16]{};FontAxisSetting settings[32]{};
    bool add(const wchar_t* file_path,u32 face_index=0,bool default_only=false) noexcept {
        catalog={};u32 path_units=0;while(file_path[path_units])++path_units;if(path_units>=512)return false;
        const HANDLE file=CreateFileW(file_path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_RANDOM_ACCESS,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
        struct Close {HANDLE file;const u8* data=nullptr;~Close() noexcept {if(data)UnmapViewOfFile(data);CloseHandle(file);}} close{file};
        NativeFontFileStamp stamp{};FILE_BASIC_INFO basic{};LARGE_INTEGER size{};
        if(!GetFileInformationByHandleEx(file,FileIdInfo,&stamp.identity,sizeof(stamp.identity))||!GetFileInformationByHandleEx(file,FileBasicInfo,&basic,sizeof(basic))||
           !GetFileSizeEx(file,&size)||size.QuadPart<=0||size.QuadPart>0xffffffffll)return false;
        stamp.bytes=static_cast<u64>(size.QuadPart);copy_bytes(&stamp.write,&basic.LastWriteTime,8);copy_bytes(&stamp.change,&basic.ChangeTime,8);
        const HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping)return false;
        close.data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CloseHandle(mapping);if(!close.data)return false;
        SfntFace face{};if(sfnt_open({close.data,static_cast<u32>(size.QuadPart)},face_index,&face)!=SfntStatus::ok)return false;
        static u16 scratch[32768];NativeFontFamily family{};NativeFontCandidates candidates{};
        if(native_font_family_candidates(face,{scratch,32768},&family,&candidates)!=SfntStatus::ok||family.units>=128||candidates.count>16||candidates.axes.axis_count>2)return false;
        u32 first=0;const u32 axes=candidates.axes.axis_count,count=default_only?1u:candidates.count;
        if(default_only&&axes){bool found=false;
            for(u32 j=0;j<candidates.count&&!found;++j){FontAxisSetting values[2];u32 actual=0;if(native_font_candidate_settings(candidates,j,{values,2},&actual)!=SfntStatus::ok)return false;
                bool normal=true;for(u32 k=0;k<actual;++k){FontAxis axis{};font_axis(candidates.axes,k,&axis);normal=normal&&values[k].value==axis.normal/65536.0;}
                if(normal){first=j;found=true;}}
            if(!found)return false;}
        for(u32 j=0;j<count;++j){NativeFontCandidate candidate{};u32 actual=0;const u32 selected=default_only?first:j;
            if(count*axes>32||native_font_candidate(candidates,selected,&candidate)!=SfntStatus::ok||
               native_font_candidate_settings(candidates,selected,{settings+j*axes,axes},&actual)!=SfntStatus::ok)return false;
            properties[j]=candidate.properties;}
        copy_bytes(path,file_path,(path_units+1)*2);copy_bytes(name,scratch,family.units*2);
        catalog={stamp,path,{name,family.units},properties,settings,face_index,count,axes};return true;
    }
};
