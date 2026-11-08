// The expectations are HarfBuzz 15.0.0's (tools/font_reference/harfbuzz_fixture.py hebrew); a known divergent case
// (HEBREW_DIVERGENCES) holds the engine's output. With MAICORD_FIXTURE_RECORD set, each known divergent case prints the
// engine's output as a pin_hebrew line for the generator's --pins instead of being checked.
#include "../../generated/hebrew_compat_fixture.inc"
u64 hebrew_known_divergent=0;
bool hebrew_compatibility(const wchar_t* path,u32 path_units) noexcept {
    u32 last=0xffffffffu;wchar_t flag[4];const bool recording=GetEnvironmentVariableW(L"MAICORD_FIXTURE_RECORD",flag,4)!=0;
    for(const auto& test:hebrew_compat_cases){++cases;hebrew_known_divergent+=test.divergence?1u:0u;
        if(test.font!=last){if(plan.slot){CHECK(native_run_plan_release(&store,plan));plan={};}
            const auto bytes=hebrew_compat_fonts[test.font];HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
            DWORD written=0;const bool wrote=WriteFile(file,bytes.data,bytes.count,&written,nullptr)!=0;CloseHandle(file);CHECK(wrote&&written==bytes.count);last=test.font;}
        CHECK(acquire(path,path_units,0));arena_reset(&scratch);const auto s=shared(storage());NativeAsciiResult result{};
        CHECK(shape(test.text,sfnt_tag('h','e','b','r'),test.mode,17.5f,s,&result)==SfntStatus::ok);
        const bool same=result.glyph_count==test.glyphs.count&&result.source_units==test.clusters.count&&
            bytes_equal(s.glyphs.data,test.glyphs.data,test.glyphs.count*2)&&bytes_equal(s.character_glyphs.data,test.clusters.data,test.clusters.count*4)&&
            bytes_equal(s.placements.data,test.placements.data,test.placements.count*4);
        const bool pin=recording&&test.divergence;
        if(!same||pin){print(pin?"pin_hebrew corpus=0 case=":"hebrew_case=");number(static_cast<u32>(&test-hebrew_compat_cases));print(" glyphs=");
            for(u32 i=0;i<result.glyph_count;++i){if(i)print(",");number(s.glyphs.data[i]);}print(" clusters=");for(u32 i=0;i<result.source_units;++i){if(i)print(",");number(s.character_glyphs.data[i]);}
            print(" placements=");for(u32 i=0;i<result.glyph_count*3;++i){if(i)print(",");u32 bits=0;copy_bytes(&bits,reinterpret_cast<const u8*>(s.placements.data)+i*4,4);number(bits);}print("\n");}
        if(pin)continue;
        CHECK(same);
        const auto& profile=profiles[3][test.mode&1];auto shortened=s;shortened.shaping.components.count=0;
        CHECK(native_alphabetic_run(store,profile,test.text,17.5f,shortened,&result,{},false,false,(test.mode&4)!=0)==SfntStatus::output_too_small&&!result.glyph_count&&!result.source_units);
        CHECK(native_alphabetic_run(store,profile,test.text,17.5f,s,&result,{1000000,100000,0},false,false,(test.mode&4)!=0)==SfntStatus::resource_limit&&!result.glyph_count&&!result.source_units);
    }return true;
}
