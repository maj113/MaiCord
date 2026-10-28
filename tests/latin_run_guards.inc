#include "generated/latin_guard_fixture.inc"
#include "generated/latin_compat_fixture.inc"
#include "hebrew_compatibility_units.inc"
#include "text_variation_units.inc"
bool profile_guards(NativeAsciiStorage alias) noexcept {
    NativeAsciiResult result{};const u32 tags[]{sfnt_tag('l','a','t','n'),sfnt_tag('g','r','e','k'),sfnt_tag('c','y','r','l'),sfnt_tag('h','e','b','r'),sfnt_tag('h','a','n','i'),sfnt_tag('k','a','n','a'),0};
    for(u32 tag:tags)for(u32 mode=0;mode<2;++mode){bool prepared=false;
        for(u32 budget=0;budget<4096&&!prepared;++budget){++cases;arena_reset(&profile_state);const auto before=arena_mark(&scratch),state_before=arena_mark(&profile_state);NativeAlphabeticPlan profile{};
            const auto status=native_alphabetic_plan_prepare(store,plan,tag,mode!=0,&profile_state,&scratch,&profile,budget);
            CHECK(arena_mark(&scratch).offset==before.offset);
            if(status==SfntStatus::ok){prepared=true;CHECK(profile.script_tag==tag&&profile.independent==(mode!=0)&&profile.run.slot==plan.slot);
                CHECK(native_alphabetic_run(store,profile,{},16,alias,&result)==SfntStatus::ok&&!result.glyph_count);
                auto bad=profile;bad.run.generation++;CHECK(native_alphabetic_run(store,bad,utf8_literal("a"),16,alias,&result)==SfntStatus::invalid_input&&!result.glyph_count);
                bad=profile;bad.script_tag=sfnt_tag('a','r','a','b');CHECK(native_alphabetic_run(store,bad,utf8_literal("a"),16,alias,&result)==SfntStatus::invalid_input&&!result.glyph_count);
                CHECK(native_alphabetic_run(store,profile,{nullptr,1},16,alias,&result)==SfntStatus::invalid_input&&!result.glyph_count);
                CHECK(native_alphabetic_run(store,profile,utf8_literal("a"),16,alias,&result,{1000000,100000,0})==SfntStatus::resource_limit&&!result.glyph_count);
            }else{CHECK(status==SfntStatus::resource_limit);NativeAlphabeticPlan empty{};CHECK(bytes_equal(&profile,&empty,sizeof(profile))&&arena_mark(&profile_state).offset==state_before.offset);}}
        CHECK(prepared);}
    NativeAlphabeticPlan rejected{};CHECK(native_alphabetic_plan_prepare(store,plan,tags[1],false,&scratch,&scratch,&rejected)==SfntStatus::invalid_input);
    CHECK(native_alphabetic_plan_prepare(store,plan,sfnt_tag('a','r','a','b'),false,&profile_state,&scratch,&rejected)==SfntStatus::invalid_input);
    return true;
}
bool compatibility(const wchar_t* path,u32 path_units) noexcept {
    u32 last=0xffffffffu;
    for(const auto& test:latin_compat_cases){++cases;
        if(test.font!=last){if(plan.slot){CHECK(native_run_plan_release(&store,plan));plan={};}
            const auto bytes=latin_compat_fonts[test.font];HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
            DWORD written=0;const bool wrote=WriteFile(file,bytes.data,bytes.count,&written,nullptr)!=0;CloseHandle(file);CHECK(wrote&&written==bytes.count);last=test.font;}
        CHECK(acquire(path,path_units,test.weight));arena_reset(&scratch);const auto s=shared(storage());if(test.font==0)CHECK(profile_guards(s));NativeAsciiResult r{};float em=0;copy_bytes(&em,&test.em,4);
        CHECK(native_latin_run(store,plan,test.text,false,em,s,&r)==SfntStatus::ok&&r.glyph_count==test.glyphs.count&&r.source_units==test.clusters.count);
        CHECK(bytes_equal(s.glyphs.data,test.glyphs.data,test.glyphs.count*2)&&bytes_equal(s.character_glyphs.data,test.clusters.data,test.clusters.count*4)&&bytes_equal(s.placements.data,test.placements.data,test.placements.count*4));
        if(r.lookup_operations){const auto budget=r.lookup_operations-1;
            CHECK(native_latin_run(store,plan,test.text,false,em,s,&r,{budget,100000,1000000})==SfntStatus::resource_limit&&!r.glyph_count&&!r.source_units);}
    }return true;
}
bool guards() noexcept {
    wchar_t directory[MAX_PATH],temporary[MAX_PATH];CHECK(GetTempPathW(MAX_PATH,directory)&&GetTempFileNameW(directory,L"mcl",0,temporary));
    struct Cleanup {wchar_t* path;u8* pages=nullptr;~Cleanup() noexcept {if(plan.slot){native_run_plan_release(&store,plan);plan={};}arena_release(&profile_state);arena_release(&scratch);if(pages)VirtualFree(pages,0,MEM_RELEASE);DeleteFileW(path);}} cleanup{temporary};
    HANDLE file=CreateFileW(temporary,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    DWORD written=0;const bool wrote=WriteFile(file,latin_guard_font,sizeof(latin_guard_font),&written,nullptr)!=0;CloseHandle(file);CHECK(wrote&&written==sizeof(latin_guard_font));
    CHECK(arena_initialize(&scratch,4*1024*1024,0,4096));u32 path_units=0;while(temporary[path_units])++path_units;CHECK(acquire(temporary,path_units,0));arena_reset(&scratch);
    const auto separate=storage();const auto alias=shared(storage());NativeAsciiResult result{};
    CHECK(arena_initialize(&profile_state,65536,0,4096));CHECK(profile_guards(alias));
    CHECK(native_latin_run(store,plan,{},false,16,alias,&result)==SfntStatus::ok&&!result.glyph_count&&!result.source_units);
    const Utf8View input=utf8_literal("ae\xcc\x81\xe2\x80\x8d\xcc\xa7");
    const auto failed=[&](Utf8View text,float em,NativeAsciiStorage s,SfntStatus expected,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        NativeAsciiResult r{99,99,99,99,99};++cases;
        const auto status=native_latin_run(store,plan,text,true,em,s,&r,limits);
        CHECK(status==expected&&!r.glyph_count&&!r.source_units&&!r.nominal_ranges&&!r.shaped_ranges);return true;
    };
    const u32 bad_em[]{0,0x80000000u,0xbf800000u,0x7f800000u,0xff800000u,0x7fc00000u,0x47800080u};
    for(u32 bits:bad_em){float em=0;copy_bytes(&em,&bits,4);CHECK(failed(input,em,alias,SfntStatus::invalid_input));}
    const Utf8View bad_text[]{{nullptr,1},utf8_literal("\x80"),utf8_literal("\xc0\xaf"),utf8_literal("\xe0\x80\xaf"),utf8_literal("\xed\xa0\x80"),utf8_literal("\xf4\x90\x80\x80"),utf8_literal("\xe2\x80")};
    for(auto text:bad_text)CHECK(failed(text,16,alias,SfntStatus::invalid_input));
    CHECK(native_latin_run(store,{},input,false,16,alias,&result)==SfntStatus::invalid_input&&!result.glyph_count);
    CHECK(native_latin_run(store,plan,input,false,16,alias,nullptr)==SfntStatus::invalid_input);
    CHECK(failed(input,16,alias,SfntStatus::resource_limit,{1000000,100000,0}));
    for(u32 field=0;field<9;++field){auto short_buffer=alias;
        switch(field){case 0:short_buffer.mapped.count=0;break;case 1:short_buffer.shaping.components.count=0;break;case 2:short_buffer.shaping.targets.count=0;break;
            case 3:short_buffer.glyphs.count=0;break;case 4:short_buffer.placements.count=0;break;case 5:short_buffer.character_glyphs.count=0;break;
            case 6:short_buffer.shaping.advances.count=0;break;case 7:short_buffer.shaping.glyphs.count=0;break;case 8:short_buffer.shaping.cluster_bits.count=0;break;}
        CHECK(failed(input,16,short_buffer,SfntStatus::output_too_small));
    }
    SYSTEM_INFO system{};GetSystemInfo(&system);const u32 page=system.dwPageSize;
    cleanup.pages=static_cast<u8*>(VirtualAlloc(nullptr,page*3,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(cleanup.pages);
    DWORD old=0;CHECK(VirtualProtect(cleanup.pages,page,PAGE_NOACCESS,&old)&&VirtualProtect(cleanup.pages+page*2,page,PAGE_NOACCESS,&old));
    const u32 lengths[]{1,2,3,7,8,9,63,64,65,127,128,255,256};
    const Utf8View sequences[]{utf8_literal("a"),utf8_literal("\xcc\x81"),utf8_literal("\xe2\x80\x8d"),utf8_literal("\xf0\x90\x80\x80")};
    for(auto sequence:sequences)for(u32 count:lengths){const u32 bytes=count*sequence.bytes;auto* text=reinterpret_cast<char*>(cleanup.pages+page*2-bytes);
        CHECK(VirtualProtect(cleanup.pages+page,page,PAGE_READWRITE,&old));for(u32 i=0;i<count;++i)copy_bytes(text+i*sequence.bytes,sequence.data,sequence.bytes);
        CHECK(VirtualProtect(cleanup.pages+page,page,PAGE_READONLY,&old));
        for(u32 mode=0;mode<2;++mode){++cases;NativeAsciiResult a{},b{};const auto sa=native_latin_run(store,plan,{text,bytes},mode!=0,17.5f,separate,&a);
            const auto sb=native_latin_run(store,plan,{text,bytes},mode!=0,17.5f,alias,&b);
            CHECK(sa==sb&&bytes_equal(&a,&b,sizeof(a)));
            if(count*(sequence.bytes==4?2u:1u)>256)CHECK(sa==SfntStatus::output_too_small&&!a.glyph_count&&!a.source_units);
            else{CHECK(sa==SfntStatus::ok);CHECK(bytes_equal(separate.glyphs.data,alias.glyphs.data,a.glyph_count*sizeof(u16)));
                CHECK(bytes_equal(separate.placements.data,alias.placements.data,a.glyph_count*sizeof(SfntPlacement))&&bytes_equal(separate.character_glyphs.data,alias.character_glyphs.data,a.source_units*sizeof(u32)));}
        }
    }
    print("guard_cases=");number(cases);print("\n");
    CHECK(native_run_plan_release(&store,plan));plan={};
    file=CreateFileW(temporary,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    const bool covered=WriteFile(file,latin_coverage_font,sizeof(latin_coverage_font),&written,nullptr)!=0;CloseHandle(file);CHECK(covered&&written==sizeof(latin_coverage_font));
    CHECK(acquire(temporary,path_units,0));arena_reset(&scratch);const auto coverage_storage=shared(storage());const auto accented=utf8_literal("e\xcc\x81");
    CHECK(native_latin_run(store,plan,accented,false,16,coverage_storage,&result)==SfntStatus::ok&&result.glyph_count==1&&result.source_units==2&&coverage_storage.glyphs[0]);
    CHECK(native_latin_run(store,plan,accented,false,16,coverage_storage,&result,{},true)==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units);
    CHECK(native_latin_run(store,plan,utf8_literal("\xc3\xa9"),false,16,coverage_storage,&result,{},true)==SfntStatus::ok&&result.glyph_count==1&&result.source_units==1);
    CHECK(compatibility(temporary,path_units));CHECK(hebrew_compatibility(temporary,path_units));CHECK(text_variations(temporary,path_units));return true;
}
