#include "generated/text_variation_fixture.inc"
bool text_variations(const wchar_t* path,u32 path_units) noexcept {
    if(plan.slot){CHECK(native_run_plan_release(&store,plan));plan={};}
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    DWORD written=0;const bool saved=WriteFile(file,text_variation_font,sizeof(text_variation_font),&written,nullptr)!=0;CloseHandle(file);
    CHECK(saved&&written==sizeof(text_variation_font)&&acquire(path,path_units,0));
    const u32 tags[]{0,sfnt_tag('l','a','t','n'),sfnt_tag('h','e','b','r')};
    for(u32 tag:tags)for(u32 independent=0;independent<2;++independent){
        arena_reset(&scratch);arena_reset(&profile_state);NativeAlphabeticPlan profile{};
        CHECK(native_alphabetic_plan_prepare(store,plan,tag,independent!=0,&profile_state,&scratch,&profile)==SfntStatus::ok);
        for(const auto& test:text_variation_cases)for(u32 rtl=0;rtl<2;++rtl)for(u32 force=0;force<2;++force){
            if(!(test.profiles&(tag==0?1u:tag==sfnt_tag('l','a','t','n')?2u:4u)))continue;++cases;
            const auto glyphs=rtl?test.rtl:test.glyphs;
            arena_reset(&scratch);const auto separate=storage(),alias=shared(storage());NativeAsciiResult a{},b{};
            CHECK(native_alphabetic_run(store,profile,test.text,15.625f,separate,&a,{},true,force!=0,rtl!=0)==SfntStatus::ok);
            CHECK(native_alphabetic_run(store,profile,test.text,15.625f,alias,&b,{},true,force!=0,rtl!=0)==SfntStatus::ok);
            CHECK(bytes_equal(&a,&b,sizeof(a))&&a.glyph_count==test.glyphs.count&&a.source_units==test.sources.count);
            CHECK(bytes_equal(separate.glyphs.data,glyphs.data,glyphs.count*2)&&bytes_equal(alias.glyphs.data,glyphs.data,glyphs.count*2));
            CHECK(bytes_equal(separate.character_glyphs.data,test.sources.data,test.sources.count*4)&&bytes_equal(alias.character_glyphs.data,test.sources.data,test.sources.count*4));
            for(u32 g=0;g<a.glyph_count;++g){const SfntPlacement expected{static_cast<float>(500u+100u*glyphs[g])/64.f,0,0};
                CHECK(bytes_equal(separate.placements.data+g,&expected,sizeof(expected))&&bytes_equal(alias.placements.data+g,&expected,sizeof(expected)));}
            auto short_source=alias;short_source.character_glyphs.count=test.sources.count-1;
            CHECK(native_alphabetic_run(store,profile,test.text,15.625f,short_source,&b,{},true,force!=0,rtl!=0)==SfntStatus::output_too_small&&!b.glyph_count&&!b.source_units);
        }
        const Utf8View truncated[]{utf8_literal("A\xef"),utf8_literal("A\xef\xb8"),utf8_literal("A\xf3\xa0\x84")};
        for(const auto text:truncated){++cases;arena_reset(&scratch);const auto s=shared(storage());NativeAsciiResult result{};
            CHECK(native_alphabetic_run(store,profile,text,16,s,&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);}
    }
    print("PASS text variation selectors\n");return true;
}
