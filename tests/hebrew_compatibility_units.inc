#include "generated/hebrew_compat_fixture.inc"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_types.hpp"
bool hebrew_compatibility(const wchar_t* path,u32 path_units) noexcept {
    u32 last=0xffffffffu;
    for(const auto& test:hebrew_compat_cases){++cases;
        if(test.font!=last){if(plan.slot){CHECK(native_run_plan_release(&store,plan));plan={};}
            const auto bytes=hebrew_compat_fonts[test.font];HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
            DWORD written=0;const bool wrote=WriteFile(file,bytes.data,bytes.count,&written,nullptr)!=0;CloseHandle(file);CHECK(wrote&&written==bytes.count);last=test.font;}
        CHECK(acquire(path,path_units,0));arena_reset(&scratch);const auto s=shared(storage());NativeAsciiResult result{};
        CHECK(shape(test.text,sfnt_tag('h','e','b','r'),test.mode,17.5f,s,&result)==SfntStatus::ok);
        CHECK(result.glyph_count==test.glyphs.count&&result.source_units==test.clusters.count&&
            bytes_equal(s.glyphs.data,test.glyphs.data,test.glyphs.count*2)&&bytes_equal(s.character_glyphs.data,test.clusters.data,test.clusters.count*4)&&
            bytes_equal(s.placements.data,test.placements.data,test.placements.count*4));
        const auto& profile=profiles[3][test.mode&1];auto shortened=s;shortened.shaping.components.count=0;
        CHECK(native_alphabetic_run(store,profile,test.text,17.5f,shortened,&result,{},false,false,(test.mode&4)!=0)==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
        CHECK(native_alphabetic_run(store,profile,test.text,17.5f,s,&result,{1000000,100000,0},false,false,(test.mode&4)!=0)==SfntStatus::resource_limit&&!result.glyph_count&&!result.source_units);
    }return true;
}
