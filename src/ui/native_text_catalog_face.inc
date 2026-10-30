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
    // candidates_open guarantees one to two axes' worth of instances; a family
    // with more named instances than any real font is refused.
    if(candidates.count>1024)return {NativeFontError::font,SfntStatus::resource_limit,0};
    if(!default_only&&candidates.count>0x200000u-owner.next_candidate)return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    u32 default_index=0;bool matched=!exact,found_default=!default_only;
    for(u32 i=0;i<candidates.count&&(!matched||!found_default);++i){NativeFontCandidate candidate{};status=native_font_candidate(candidates,i,&candidate);
        if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};
        if(exact&&candidate.properties==query.properties)matched=true;
        if(!found_default){FontAxisSetting settings[2];u32 count=0;
            status=native_font_candidate_settings(candidates,i,{settings,2},&count);if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};
            bool normal=true;for(u32 j=0;j<count;++j){FontAxis axis{};font_axis(candidates.axes,j,&axis);normal&=settings[j].value==axis.normal/65536.0;}
            if(normal){default_index=i;found_default=true;}}
    }
    if(!matched)return {};if(!found_default)return {NativeFontError::font,SfntStatus::unsupported,0};
    u32 path_units=0;while(path[path_units])++path_units;
    const auto& opened=fonts->files[fonts->faces[handle.slot-1].file_slot];FILE_BASIC_INFO basic{};
    if(!GetFileInformationByHandleEx(opened.file,FileBasicInfo,&basic,sizeof(basic)))return {NativeFontError::io,SfntStatus::ok,GetLastError()};
    const auto mark=arena_mark(&owner.storage);bool published=false;
    struct Rollback {Arena& storage;ArenaMark mark;bool& published;~Rollback() noexcept {if(!published)arena_rewind(&storage,mark);}} rollback{owner.storage,mark,published};
    // One block: settings, properties, then the path (with its NUL) and family name.
    const u32 count=default_only?1:candidates.count,axes=candidates.axes.axis_count,settings=count*axes;
    auto* block=static_cast<u8*>(arena_allocate(&owner.storage,settings*sizeof(FontAxisSetting)+count*sizeof(NativeFontProperties)+(path_units+1+family.units)*2,alignof(FontAxisSetting)));
    if(!block)return {NativeFontError::capacity,SfntStatus::output_too_small,0};
    auto* setting_data=reinterpret_cast<FontAxisSetting*>(block);auto* properties=reinterpret_cast<NativeFontProperties*>(setting_data+settings);auto* strings=reinterpret_cast<u16*>(properties+count);
    NativeFontCatalog catalog{{},reinterpret_cast<const wchar_t*>(strings),{strings+path_units+1,family.units},properties,setting_data,index,count,axes};
    copy_bytes(&catalog.stamp.identity,&opened.identity,sizeof(opened.identity));catalog.stamp.bytes=opened.bytes.count;
    copy_bytes(&catalog.stamp.write,&basic.LastWriteTime,8);copy_bytes(&catalog.stamp.change,&basic.ChangeTime,8);
    copy_bytes(strings,path,(path_units+1)*2);copy_bytes(strings+path_units+1,name,family.units*2);
    for(u32 i=0;i<count;++i){const u32 selected=default_only?default_index:i;NativeFontCandidate candidate{};u32 actual=0;
        status=native_font_candidate(candidates,selected,&candidate);if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};
        status=native_font_candidate_settings(candidates,selected,{setting_data+i*axes,axes},&actual);if(status!=SfntStatus::ok)return {NativeFontError::font,status,0};
        properties[i]=candidate.properties;
    }
    *output=catalog;if(requested)result=native_font_coverage_remember(&owner.coverage,*output,handle);
    if(!result.ok()){*output={};return result;}published=true;return {};
}
