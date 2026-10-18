// Test-only catalog loader over the live public font APIs. Production catalogs
// are built per face by src/ui; these fixtures need whole files and collections.
// Aliased physical files keep the first path; default_only keeps face zero's
// default axis instance. Requires <Windows.h> and maicord/text/font_catalog.hpp.
struct CatalogFixture {
    NativeFontCatalog catalog{};u8* memory=nullptr;
    static constexpr u32 file_limit=1024,face_limit=4096,candidate_limit=8192,setting_limit=16384,string_limit=262144;
    CatalogFixture() noexcept {
        const usize files=sizeof(NativeFontCatalogFile)*file_limit,faces=sizeof(NativeFontCatalogFace)*face_limit,candidates=sizeof(NativeFontCatalogCandidate)*candidate_limit,settings=sizeof(FontAxisSetting)*setting_limit;
        memory=static_cast<u8*>(VirtualAlloc(nullptr,files+faces+candidates+settings+string_limit*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return;u8* at=memory;
        catalog.settings={reinterpret_cast<FontAxisSetting*>(at),setting_limit};at+=settings;
        catalog.files={reinterpret_cast<NativeFontCatalogFile*>(at),file_limit};at+=files;
        catalog.faces={reinterpret_cast<NativeFontCatalogFace*>(at),face_limit};at+=faces;
        catalog.candidates={reinterpret_cast<NativeFontCatalogCandidate*>(at),candidate_limit};at+=candidates;
        catalog.strings={reinterpret_cast<u16*>(at),string_limit};
    }
    ~CatalogFixture() noexcept {if(memory)VirtualFree(memory,0,MEM_RELEASE);}
    CatalogFixture(const CatalogFixture&)=delete;CatalogFixture& operator=(const CatalogFixture&)=delete;
    bool add(const wchar_t* path,u32* index,bool default_only=false) noexcept {
        *index=0xffffffffu;if(!memory)return false;auto& c=catalog;auto n=c.count;
        const HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_RANDOM_ACCESS,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
        struct Close {HANDLE file;const u8* data=nullptr;~Close() noexcept {if(data)UnmapViewOfFile(data);CloseHandle(file);}} close{file};
        NativeFontCatalogFile item{};FILE_BASIC_INFO basic{};LARGE_INTEGER size{};
        if(!GetFileInformationByHandleEx(file,FileIdInfo,&item.identity,sizeof(item.identity))||!GetFileInformationByHandleEx(file,FileBasicInfo,&basic,sizeof(basic))||
           !GetFileSizeEx(file,&size)||size.QuadPart<=0||size.QuadPart>0xffffffffll)return false;
        item.bytes=static_cast<u32>(size.QuadPart);copy_bytes(&item.last_write,&basic.LastWriteTime,8);copy_bytes(&item.change_time,&basic.ChangeTime,8);
        for(u32 i=0;i<n.files;++i)if(bytes_equal(&c.files[i].identity,&item.identity,sizeof(item.identity))){*index=i;return true;}
        const HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping)return false;
        close.data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CloseHandle(mapping);if(!close.data)return false;
        const ConstSlice<u8> bytes{close.data,item.bytes};SfntFace face{};if(sfnt_open(bytes,0,&face)!=SfntStatus::ok)return false;
        u32 path_units=0;while(path[path_units])++path_units;const u32 face_count=default_only?1u:face.face_count;
        if(n.files>=file_limit||path_units>=32768||path_units+1>string_limit-n.strings||face_count>face_limit-n.faces)return false;
        item.path_offset=n.strings;item.path_units=static_cast<u16>(path_units);
        copy_bytes(c.strings.data+n.strings,path,(path_units+1)*2);n.strings+=path_units+1;
        static u16 name[32768];
        for(u32 i=0;i<face_count;++i){if(i&&sfnt_open(bytes,i,&face)!=SfntStatus::ok)return false;
            NativeFontFamily family{};NativeFontCandidates candidates{};if(native_font_family_candidates(face,{name,32768},&family,&candidates)!=SfntStatus::ok)return false;
            u32 first=0;const u32 count=default_only?1u:candidates.count;
            if(default_only&&candidates.axes.axis_count){bool found=false;
                for(u32 j=0;j<candidates.count&&!found;++j){FontAxisSetting settings[2];u32 axes=0;if(native_font_candidate_settings(candidates,j,{settings,2},&axes)!=SfntStatus::ok)return false;
                    bool normal=true;for(u32 k=0;k<axes;++k){FontAxis axis{};font_axis(candidates.axes,k,&axis);normal=normal&&settings[k].value==axis.normal/65536.0;}
                    if(normal){first=j;found=true;}}
                if(!found)return false;}
            if(count>candidate_limit-n.candidates||family.units+1>string_limit-n.strings)return false;
            NativeFontCatalogFace record{n.files,i,n.strings,family.units,n.candidates,count};
            copy_bytes(c.strings.data+n.strings,name,family.units*2);c.strings[n.strings+family.units]=0;n.strings+=family.units+1;
            for(u32 j=0;j<count;++j){const u32 selected=default_only?first:j;NativeFontCandidate candidate{};u32 axes=0;
                if(native_font_candidate(candidates,selected,&candidate)!=SfntStatus::ok||candidates.axes.axis_count>setting_limit-n.settings||
                   native_font_candidate_settings(candidates,selected,{c.settings.data+n.settings,setting_limit-n.settings},&axes)!=SfntStatus::ok)return false;
                c.candidates[n.candidates++]={candidate.properties,n.faces,candidate.named_index,n.settings,axes};n.settings+=axes;}
            c.faces[n.faces++]=record;}
        c.files[n.files++]=item;c.count=n;*index=n.files-1;return true;
    }
};
