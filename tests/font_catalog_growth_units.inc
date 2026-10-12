bool catalog_growth_cases() noexcept {
    Temporary temp;CHECK(temp.open());const ConstSlice<u8> font{font_family_fixture+24,byte_load_le32(font_family_fixture+12)};
    CHECK(write(temp.paths[0],font));u8 collection[4096]{};byte_store_be32(collection,sfnt_tag('t','t','c','f'));byte_store_be32(collection+4,0x10000);byte_store_be32(collection+8,3);u32 size=24;
    for(u32 i=0;i<3;++i){const u32 offset=size;CHECK(offset+font.count+4<=sizeof(collection));byte_store_be32(collection+12+i*4,offset);copy_bytes(collection+offset,font.data,font.count);
        for(u32 j=0;j<byte_load_be16(font.data+4);++j){auto* at=collection+offset+12+j*16+8;byte_store_be32(at,byte_load_be32(at)+offset);}size=(offset+font.count+3)&~3u;}
    CHECK(write(temp.paths[1],{collection,size}));u32 total=0;
    for(u32 stop=0;stop<=total;++stop){Storage backing;u16 names[128];u32 index=0;
        CHECK(native_font_catalog_add(&backing.catalog,temp.paths[0],{names,128},&index).ok());auto& c=backing.catalog;const auto saved=c.count;
        const auto file=c.files[0];const auto face=c.faces[0];const auto candidate=c.candidates[0];u16 strings[2048];copy_bytes(strings,c.strings.data,c.count.strings*2);
        c.files.count=c.count.files;c.faces.count=c.count.faces;c.candidates.count=c.count.candidates;c.settings.count=c.count.settings;c.strings.count=c.count.strings;
        struct Growth {NativeFontCatalog* catalog;const wchar_t* path;u32 calls,stop;bool held;};Growth state{&c,temp.paths[1],0,stop,true};
        const auto extend=[](void* context,NativeFontCatalogCounts required) noexcept {
            auto& s=*static_cast<Growth*>(context);++s.calls;
            HANDLE writer=CreateFileW(s.path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            s.held=s.held&&writer==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION;if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
            if(s.calls==s.stop)return false;if(required.files>4||required.faces>8||required.candidates>16||required.settings>32||required.strings>2048)return false;
            auto& c=*s.catalog;c.files.count=maximum(c.files.count,required.files);c.faces.count=maximum(c.faces.count,required.faces);c.candidates.count=maximum(c.candidates.count,required.candidates);c.settings.count=maximum(c.settings.count,required.settings);c.strings.count=maximum(c.strings.count,required.strings);return true;
        };
        NativeFontCatalogCounts needed{};const auto result=native_font_catalog_append(&c,temp.paths[1],{names,128},&index,65536,1000000,&needed,false,{}, {&state,extend});
        CHECK(state.held&&state.calls>0);CHECK(bytes_equal(c.files.data,&file,sizeof(file))&&bytes_equal(c.faces.data,&face,sizeof(face))&&bytes_equal(c.candidates.data,&candidate,sizeof(candidate))&&bytes_equal(c.strings.data,strings,saved.strings*2));
        if(!stop){CHECK(result.ok()&&index==1&&c.count.files==2&&c.count.faces==4&&c.count.candidates==4);total=state.calls;CHECK(total>=4);}
        else CHECK(result.error==NativeFontError::capacity&&index==0xffffffffu&&state.calls==stop&&bytes_equal(&c.count,&saved,sizeof(saved)));
        HANDLE writer=CreateFileW(temp.paths[1],GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(writer!=INVALID_HANDLE_VALUE);CloseHandle(writer);
    }return true;
}
