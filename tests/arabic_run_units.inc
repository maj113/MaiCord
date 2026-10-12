bool utf8_run_units(SfntShapeStorage storage) noexcept {
    const auto failed=[](u32 line) noexcept {print(utf8_literal("UTF-8 unit line="));number(line);print(utf8_literal("\n"));return false;};
    SfntShapeRunResult result{};const auto call=[&](Utf8View text,SfntShapeStorage s,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {return native_arabic_run(owned_store,owned_handle,text,false,true,1000,s,&result,limits);};
    if(call({}, {})!=SfntStatus::ok || result.glyph_count || result.source_units)return failed(__LINE__);
    if(native_arabic_run(owned_store,owned_handle,{},false,false,1000,{},&result)!=SfntStatus::ok||result.glyph_count||result.source_units)return failed(__LINE__);
    if(native_arabic_run(owned_store,{}, {},false,true,1000,storage,&result)!=SfntStatus::invalid_input)return failed(__LINE__);
    if(native_arabic_run(owned_store,owned_handle,{},false,true,-1,storage,&result)!=SfntStatus::invalid_input)return failed(__LINE__);
    char text[12];u32 bytes=utf8_encode(0x1ee00,text);bytes+=utf8_encode(0x628,text+bytes);
    if(call({text,bytes},storage)!=SfntStatus::ok || result.source_units!=3 || result.glyph_count!=2 || storage.character_glyphs[0] || storage.character_glyphs[1] || storage.character_glyphs[2]!=1)return failed(__LINE__);
    if(native_arabic_run(owned_store,owned_handle,{text,bytes},false,false,1000,storage,&result)!=SfntStatus::ok||result.source_units!=3||result.glyph_count!=2||storage.character_glyphs[0]||storage.character_glyphs[1]||storage.character_glyphs[2]!=1)return failed(__LINE__);
    const u16 ltr_glyphs[]{storage.glyphs[0].glyph.glyph,storage.glyphs[1].glyph.glyph};
    const SfntPlacement ltr_placements[]{storage.placements[0],storage.placements[1]};bool ltr_complete=false;
    for(u32 budget=0;budget<256;++budget){const auto status=native_arabic_run(owned_store,owned_handle,{text,bytes},false,false,1000,storage,&result,{1000000,100000,budget});
        if(status==SfntStatus::ok){if(result.glyph_count!=2||result.source_units!=3||storage.glyphs[0].glyph.glyph!=ltr_glyphs[0]||storage.glyphs[1].glyph.glyph!=ltr_glyphs[1]||!bytes_equal(storage.placements.data,ltr_placements,sizeof(ltr_placements)))return failed(__LINE__);ltr_complete=true;break;}
        if(status!=SfntStatus::resource_limit||result.glyph_count||result.source_units)return failed(__LINE__);}
    if(!ltr_complete)return failed(__LINE__);
    auto short_ltr=storage;
    if(native_arabic_plan_get(owned_store,owned_handle,false).generic)short_ltr.glyphs.count=1;
    else short_ltr.substitutions.replacements.count=sizeof(SfntClusterGlyph);
    if(native_arabic_run(owned_store,owned_handle,{text,bytes},false,false,1000,short_ltr,&result)!=SfntStatus::output_too_small||result.glyph_count||result.source_units)return failed(__LINE__);
    for(u32 kind=0;kind<4;++kind){auto short_storage=storage;
        if(kind==0)short_storage.components={};else if(kind==1)short_storage.positions={};else if(kind==2)short_storage.targets={};else short_storage.character_glyphs.count=2;
        result.glyph_count=99;result.source_units=99;if(call({text,bytes},short_storage)!=SfntStatus::output_too_small || result.glyph_count || result.source_units)return failed(__LINE__);
    }
    if(call({text,bytes},storage,{1000000,100000,0})!=SfntStatus::resource_limit || result.glyph_count || result.source_units)return failed(__LINE__);
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return failed(__LINE__);DWORD old=0;
    if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return failed(__LINE__);}
    const u8 invalid[][4]{{0xc0,0xaf,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0xf0,0x9f,0x92,0}};const u32 lengths[]{2,3,4,3};bool ok=true;
    for(u32 i=0;i<5 && ok;++i){if(!VirtualProtect(memory,page,PAGE_READWRITE,&old)){ok=false;break;}const u32 length=i<4?lengths[i]:bytes;const void* source=i<4?static_cast<const void*>(invalid[i]):static_cast<const void*>(text);
        copy_bytes(memory+page-length,source,length);if(!VirtualProtect(memory,page,PAGE_READONLY,&old)){ok=false;break;}
        const auto status=call({reinterpret_cast<const char*>(memory+page-length),length},storage);
        ok=i<4?(status==SfntStatus::invalid_input && !result.glyph_count && !result.source_units):(status==SfntStatus::ok && result.source_units==3 && result.glyph_count==2);
        ok=ok&&bytes_equal(memory+page-length,source,length);
    }
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
