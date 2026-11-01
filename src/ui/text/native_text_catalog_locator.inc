bool system_file(NativeTextCatalogs& owner,const wchar_t* path,u32 index,const NativeTextFontQuery& query,bool exact,Arena& scratch,NativeSystemFontFile** output,bool* fresh) noexcept {
    *output=nullptr;
    for(auto* file=owner.files;file;file=file->next){
        if(file->catalog.face_index==index&&CompareStringOrdinal(path,-1,file->catalog.path,-1,TRUE)==CSTR_EQUAL){
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
