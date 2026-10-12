struct SystemFontLocator {
    struct Realization {DWORD bytes,flags,cache,instance,files;WORD face,simulations;};
    struct File {FILETIME write;LARGE_INTEGER size;wchar_t path[1024];};
    static_assert(sizeof(Realization)==24&&sizeof(File)==2064);
    using Realize=BOOL(WINAPI*)(HDC,Realization*);
    using Locate=BOOL(WINAPI*)(DWORD,DWORD,void*,SIZE_T,SIZE_T*);
    HMODULE module=nullptr;HDC dc=nullptr;Realize realize=nullptr;Locate locate=nullptr;
    ~SystemFontLocator() noexcept {if(dc)DeleteDC(dc);if(module)FreeLibrary(module);}
    bool initialize() noexcept {
        module=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!module)return false;
        realize=reinterpret_cast<Realize>(GetProcAddress(module,"GetFontRealizationInfo"));
        locate=reinterpret_cast<Locate>(GetProcAddress(module,"GetFontFileInfo"));
        return realize&&locate&&(dc=CreateCompatibleDC(nullptr));
    }
    bool find(const LOGFONTW& description,File& file,u32* index,bool exact_name) noexcept {
        file={};const HFONT font=CreateFontIndirectW(&description);if(!font)return false;const auto previous=SelectObject(dc,font);
        struct Restore {HDC dc;HGDIOBJ previous;HFONT font;~Restore() noexcept {if(previous&&previous!=HGDI_ERROR)SelectObject(dc,previous);DeleteObject(font);}} restore{dc,previous,font};
        if(!previous||previous==HGDI_ERROR)return false;
        if(exact_name){wchar_t realized[LF_FACESIZE]{};
            if(!GetTextFaceW(dc,LF_FACESIZE,realized)||CompareStringOrdinal(description.lfFaceName,-1,realized,-1,TRUE)!=CSTR_EQUAL)return false;}
        Realization info{sizeof(Realization)};SIZE_T needed=0;
        if(!realize(dc,&info)||info.files!=1)return false;*index=info.face;
        // Named instances in a noncollection variable font share physical face zero.
        if(info.face&&GetFontData(dc,0x66637474,0,nullptr,0)==GDI_ERROR&&GetFontData(dc,0x72617666,0,nullptr,0)!=GDI_ERROR)*index=0;
        return locate(info.instance,0,&file,sizeof(file),&needed)&&
            needed<=sizeof(file)&&file.path[0]&&!file.path[1023]&&file.size.QuadPart>0&&file.size.QuadPart<=0xffffffffu;
    }
};
bool system_file(NativeTextCatalogs& owner,const wchar_t* path,u32 index,const NativeTextFontQuery& query,bool exact,Arena& scratch,NativeSystemFontFile** output,bool* fresh) noexcept {
    *output=nullptr;
    for(auto* file=owner.files;file;file=file->next){const auto saved=native_font_catalog_path(file->catalog,0);
        if(file->catalog.faces[0].index==index&&CompareStringOrdinal(path,-1,reinterpret_cast<const wchar_t*>(saved.data),saved.count,TRUE)==CSTR_EQUAL){
            if(contains_family(file->catalog,query,exact))*output=file;return true;
        }
    }
    const auto mark=arena_mark(&owner.storage);auto* file=arena_allocate_zero<NativeSystemFontFile>(&owner.storage);
    if(!file)return catalog_failed(owner,0,{NativeFontError::capacity,SfntStatus::output_too_small,0});
    const auto result=load_face(owner,path,index,false,query,exact,scratch,&file->catalog);
    if(!result.ok()||!file->catalog.count.candidates){arena_rewind(&owner.storage,mark);
        if(result.error==NativeFontError::font&&result.font_status==SfntStatus::unsupported){++owner.unsupported;return true;}
        return result.ok()||catalog_failed(owner,0,result);
    }
    file->next=owner.files;file->first_candidate=owner.next_candidate;owner.next_candidate+=file->catalog.count.candidates;owner.files=file;*output=file;*fresh=true;return true;
}
bool system_family_file(NativeTextCatalogs& owner,const NativeTextFontQuery& query,Arena& scratch,const NativeFontCatalog** output,u32* base,bool* fresh) noexcept {
    *output=nullptr;*base=0;
    for(auto* file=owner.files;file;file=file->next)if(contains_family(file->catalog,query,true)){
        *output=&file->catalog;*base=file->first_candidate;return true;
    }
    if(query.family.count>=LF_FACESIZE)return true;
    SystemFontLocator locator;if(!locator.initialize())return catalog_failed(owner,0,{NativeFontError::io,SfntStatus::ok,ERROR_NOT_SUPPORTED});
    LOGFONTW description{};description.lfHeight=-16;description.lfWeight=query.properties.weight;description.lfItalic=query.properties.style!=0;
    description.lfCharSet=DEFAULT_CHARSET;description.lfOutPrecision=OUT_OUTLINE_PRECIS;copy_bytes(description.lfFaceName,query.family.data,query.family.count*2);
    constexpr wchar_t light[]=L" Light",semibold[]=L" Semibold";
    const ConstSlice<wchar_t> suffix=query.properties.weight==300?ConstSlice<wchar_t>{light,sizeof(light)/2}:ConstSlice<wchar_t>{semibold,sizeof(semibold)/2};
    const bool alias=(query.properties.weight==300||query.properties.weight==600)&&query.family.count+suffix.count<=LF_FACESIZE;
    for(u32 attempt=0;attempt<(alias?2u:1u);++attempt){const bool exact=alias&&!attempt;
        description.lfWeight=query.properties.weight==600&&!exact?700:query.properties.weight;
        if(exact)copy_bytes(description.lfFaceName+query.family.count,suffix.data,suffix.count*2);else description.lfFaceName[query.family.count]=0;
        SystemFontLocator::File located{};u32 index=0;if(!locator.find(description,located,&index,exact))continue;
        NativeSystemFontFile* file=nullptr;if(!system_file(owner,located.path,index,query,exact&&query.properties.weight==600,scratch,&file,fresh))return false;
        if(file){*output=&file->catalog;*base=file->first_candidate;return true;}
    }return true;
}
