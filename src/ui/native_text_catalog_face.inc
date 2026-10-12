NativeFontResult load_face(NativeTextCatalogs& owner,const wchar_t* path,u32 index,bool default_only,
    const NativeTextFontQuery& query,bool exact,Arena& scratch,NativeFontCatalog* output) noexcept {
    *output={};auto* session=owner.coverage.session;auto* fonts=native_font_session_fonts(session);NativeFontHandle handle{};
    auto result=native_font_session_face(session,path,index,&handle);if(!result.ok())return result;
    struct Release {NativeFontStore* fonts;NativeFontHandle handle;~Release() noexcept {native_font_release(fonts,handle);}} release{fonts,handle};
    const auto& face=native_font_get(*fonts,handle)->geometry.face;
    arena_reset(&scratch);u32 units=512;auto* name=arena_allocate<u16>(&scratch,units);
    if(!name)return {NativeFontError::capacity,SfntStatus::output_too_small,0};
    NativeFontFamily family{};NativeFontCandidates candidates{};
    auto status=native_font_family_candidates(face,{name,units},&family,&candidates);
    if(status==SfntStatus::output_too_small){units=32768;name=arena_allocate<u16>(&scratch,units);
        if(!name)return {NativeFontError::capacity,SfntStatus::output_too_small,0};
        status=native_font_family_candidates(face,{name,units},&family,&candidates);}
    if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};
    const bool requested=same_name({name,family.units},query.family);
    if(!default_only&&!requested)return {};
    if(!candidates.count||candidates.count>8192||candidates.axes.axis_count>2)return {NativeFontError::font,SfntStatus::resource_limit,0};
    if(!default_only&&candidates.count>0x200000u-owner.next_candidate)return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    u32 default_index=0;bool matched=!exact,found_default=!default_only;
    for(u32 i=0;i<candidates.count&&(!matched||!found_default);++i){NativeFontCandidate candidate{};status=native_font_candidate(candidates,i,&candidate);
        if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};
        if(exact&&bytes_equal(&candidate.properties,&query.properties,4))matched=true;
        if(default_only&&!found_default){FontAxisSetting settings[2];u32 count=0;
            status=native_font_candidate_settings(candidates,i,{settings,2},&count);if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};
            bool normal=true;for(u32 j=0;j<count;++j){FontAxis axis{};font_axis(candidates.axes,j,&axis);normal&=settings[j].value==axis.normal/65536.0;}
            if(normal){default_index=i;found_default=true;}}
    }
    if(!matched)return {};if(!found_default)return {NativeFontError::font,SfntStatus::unsupported,0};
    u32 path_units=0;while(path_units<32768&&path[path_units])++path_units;
    if(!path_units||path_units==32768)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    const auto& opened=fonts->files[fonts->faces[handle.slot-1].file_slot];FILE_BASIC_INFO basic{};
    if(!GetFileInformationByHandleEx(opened.file,FileBasicInfo,&basic,sizeof(basic)))return {NativeFontError::io,SfntStatus::ok,GetLastError()};
    const auto mark=arena_mark(&owner.storage);bool published=false;
    struct Rollback {Arena& storage;ArenaMark mark;bool& published;~Rollback() noexcept {if(!published)arena_rewind(&storage,mark);}} rollback{owner.storage,mark,published};
    NativeFontCatalog catalog{};const u32 count=default_only?1:candidates.count,settings=count*candidates.axes.axis_count,strings=path_units+family.units+2;
    catalog.files={arena_allocate<NativeFontCatalogFile>(&owner.storage),1};catalog.faces={arena_allocate<NativeFontCatalogFace>(&owner.storage),1};
    catalog.candidates={arena_allocate<NativeFontCatalogCandidate>(&owner.storage,count),count};
    catalog.settings={settings?arena_allocate<FontAxisSetting>(&owner.storage,settings):nullptr,settings};
    catalog.strings={arena_allocate<u16>(&owner.storage,strings),strings};
    if(!catalog.files.data||!catalog.faces.data||!catalog.candidates.data||(settings&&!catalog.settings.data)||!catalog.strings.data)
        return {NativeFontError::capacity,SfntStatus::output_too_small,0};
    catalog.count={1,1,count,settings,strings};auto& file=catalog.files[0];file={};copy_bytes(&file.identity,&opened.identity,sizeof(opened.identity));file.bytes=opened.bytes.count;file.path_units=static_cast<u16>(path_units);
    copy_bytes(&file.last_write,&basic.LastWriteTime,8);copy_bytes(&file.change_time,&basic.ChangeTime,8);
    copy_bytes(catalog.strings.data,path,(path_units+1)*2);copy_bytes(catalog.strings.data+path_units+1,name,family.units*2);catalog.strings[strings-1]=0;
    catalog.faces[0]={0,index,path_units+1,family.units,0,count};
    for(u32 i=0;i<count;++i){const u32 selected=default_only?default_index:i;NativeFontCandidate candidate{};status=native_font_candidate(candidates,selected,&candidate);
        if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};
        const u32 at=i*candidates.axes.axis_count;u32 actual=0;
        status=native_font_candidate_settings(candidates,selected,{settings?catalog.settings.data+at:nullptr,candidates.axes.axis_count},&actual);
        if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};catalog.candidates[i]={candidate.properties,0,candidate.named_index,at,actual};
    }
    *output=catalog;if(requested)result=native_font_coverage_remember(&owner.coverage,*output,0,handle);
    if(!result.ok()){*output={};return result;}published=true;return {};
}
