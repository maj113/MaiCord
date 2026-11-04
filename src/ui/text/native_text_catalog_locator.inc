bool system_file(NativeTextCatalogs& owner,const wchar_t* path,u32 index,const NativeTextFontQuery& query,bool exact,Arena& scratch,NativeSystemFontFile** output,bool* fresh) noexcept {
    *output=nullptr;
    for(auto* file=owner.files;file;file=file->next){
        if(file->catalog.face_index==index&&ordinal_equal(path,-1,file->catalog.path,-1,true)){
            if(contains_family(file->catalog,query,exact))*output=file;return true;
        }
    }
    const auto mark=arena_mark(&owner.storage);auto* file=arena_allocate_zero<NativeSystemFontFile>(&owner.storage);
    if(!file)return catalog_failed(owner,0,{NativeFontError::capacity,SfntStatus::output_too_small,0});
    const auto result=load_face(owner,path,index,false,query,exact,scratch,&file->catalog);
    if(!result.ok()||!file->catalog.count){arena_rewind(&owner.storage,mark);
        if(result.error==NativeFontError::font&&result.font_status==SfntStatus::unsupported){++owner.unsupported;return true;}
        return result.ok()||catalog_failed(owner,0,result);
    }
    file->next=owner.files;file->first_candidate=owner.next_candidate;owner.next_candidate+=file->catalog.count;owner.files=file;*output=file;*fresh=true;return true;
}
#if !defined(_WIN32)
// The platform locator (fontconfig, else the directory scan) lives from the first system family build until release.
SystemFontLocator* system_locator(NativeTextCatalogs& owner) noexcept {
    if(!owner.locator)owner.locator=arena_allocate_zero<SystemFontLocator>(&owner.storage);
    return owner.locator;
}
// Fontconfig holds about 3 MB while it is open: close it when the entry point that needed it is done.
struct LocatorIdle {
    NativeTextCatalogs* owner;
    ~LocatorIdle() noexcept {if(owner&&owner->locator)system_font_locator_idle(owner->locator);}
};
// The CJK order of the fallback table for the user's language (tools/font_reference/linux_fallback.py):
// 0 unspecified, 1 ja, 2 ko, 3 zh-cn, 4 zh-tw, 5 zh-hk.
u32 fallback_locale_for_language() noexcept {
    char language[16];const u32 length=system_font_language(language);
    if(length<2)return 0;
    if(language[0]=='j'&&language[1]=='a')return 1;
    if(language[0]=='k'&&language[1]=='o')return 2;
    if(language[0]!='z'||language[1]!='h')return 0;
    if(length==5&&language[3]=='t'&&language[4]=='w')return 4;
    if(length==5&&language[3]=='h'&&language[4]=='k')return 5;
    if(length==5&&language[3]=='m'&&language[4]=='o')return 5;
    return 3;
}
bool system_family_file(NativeTextCatalogs& owner,const NativeTextFontQuery& query,Arena& scratch,const NativeFontCatalog** output,u32* base,bool* fresh) noexcept {
    *output=nullptr;*base=0;
    for(auto* file=owner.files;file;file=file->next)if(contains_family(file->catalog,query,true)){
        *output=&file->catalog;*base=file->first_candidate;return true;
    }
    if(query.family.count>=kSystemFontNameCapacity)return true;
    auto* locator=system_locator(owner);
    if(!locator)return catalog_failed(owner,0,{NativeFontError::capacity,SfntStatus::output_too_small,0});
    // A family that is not installed is a miss, not another family: the face of the family the query names is the
    // only face whose catalog family_order accepts, so the locator is asked for that family alone.
    SystemFontFile located;
    if(!system_font_locator_find(locator,{query.family,static_cast<u16>(query.properties.weight),query.properties.style!=0},&located))return true;
    NativeSystemFontFile* file=nullptr;if(!system_file(owner,located.path,located.index,query,false,scratch,&file,fresh))return false;
    if(file){*output=&file->catalog;*base=file->first_candidate;}
    return true;
}
#else
bool system_family_file(NativeTextCatalogs& owner,const NativeTextFontQuery& query,Arena& scratch,const NativeFontCatalog** output,u32* base,bool* fresh) noexcept {
    *output=nullptr;*base=0;
    for(auto* file=owner.files;file;file=file->next)if(contains_family(file->catalog,query,true)){
        *output=&file->catalog;*base=file->first_candidate;return true;
    }
    if(query.family.count>=LF_FACESIZE)return true;
    if(!owner.locator){const auto mark=arena_mark(&owner.storage);auto* created=arena_allocate_zero<SystemFontLocator>(&owner.storage);
        if(!created||!created->initialize()){if(created)created->release();arena_rewind(&owner.storage,mark);
            return catalog_failed(owner,0,{NativeFontError::io,SfntStatus::ok,ERROR_NOT_SUPPORTED});}
        owner.locator=created;}
    auto& locator=*owner.locator;
    LOGFONTW description{};description.lfHeight=-16;description.lfWeight=query.properties.weight;description.lfItalic=query.properties.style!=0;
    description.lfCharSet=DEFAULT_CHARSET;description.lfOutPrecision=OUT_OUTLINE_PRECIS;copy_bytes(description.lfFaceName,query.family.data,query.family.count*2);
    static constexpr wchar_t light[]=L" Light",semibold[]=L" Semibold";
    const ConstSlice<wchar_t> suffix=query.properties.weight==300?ConstSlice<wchar_t>{light,sizeof(light)/2}:ConstSlice<wchar_t>{semibold,sizeof(semibold)/2};
    const bool alias=(query.properties.weight==300||query.properties.weight==600)&&query.family.count+suffix.count<=LF_FACESIZE;
    for(u32 attempt=0;attempt<(alias?2u:1u);++attempt){const bool exact=alias&&!attempt;
        description.lfWeight=query.properties.weight==600&&!exact?700:query.properties.weight;
        if(exact)copy_bytes(description.lfFaceName+query.family.count,suffix.data,suffix.count*2);else description.lfFaceName[query.family.count]=0;
        SystemFontLocator::File located;u32 index=0;if(!locator.find(description,located,&index,exact))continue;
        NativeSystemFontFile* file=nullptr;if(!system_file(owner,located.path,index,query,exact&&query.properties.weight==600,scratch,&file,fresh))return false;
        if(file){*output=&file->catalog;*base=file->first_candidate;return true;}
    }return true;
}
#endif
