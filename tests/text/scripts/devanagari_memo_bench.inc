// Paired measurements of the same retained font and low-level shaping path.
// No paragraph cache, font preparation, allocation or comparison in the timer.
bool memo_benchmark(SfntShapeStorage scratch,const OwnedProfile& owned) noexcept {
    constexpr u32 capacity=4096;
    struct Output {u16 glyphs[capacity];SfntPlacement placements[capacity];u32 characters[capacity];};
    auto* outputs=static_cast<Output*>(VirtualAlloc(nullptr,2*sizeof(Output),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(outputs);
    struct Release {void* p;~Release() noexcept {VirtualFree(p,0,MEM_RELEASE);}} release{outputs};
    const auto* parent=native_run_plan_get(owned.store,owned.profile.indic.run);CHECK(parent);auto plan=*parent;
    plan.substitution_roots={};plan.positioning_roots=owned.profile.indic.positioning();plan.refresh_substitution_classes=true;
    const char* names[]{"repeated","natural","unique"};char source[8192];LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    for(u32 corpus=0;corpus<3;++corpus){u32 bytes=0;
        if(corpus==2){for(u32 i=0;i<256;++i){const u32 scalars[]{0x915+i%32,0x94d,0x915+i/32,0x93f};
            for(auto scalar:scalars)bytes+=utf8_encode(scalar,source+bytes);}}
        else{const auto word=corpus?utf8_literal("नमस्ते मित्रों। आज हम अपने समुदाय में नई बातें सीख रहे हैं। कृपया अपना संदेश लिखें और चर्चा में शामिल हों। "):utf8_literal("र्किहिन्दीक्षेत्र");
            const u32 repetitions=corpus?8u:64u;for(u32 i=0;i<repetitions;++i){CHECK(bytes+word.bytes<sizeof(source));copy_bytes(source+bytes,word.data,word.bytes);bytes+=word.bytes;}}
        NativeAsciiResult results[2]{};
        const auto shape=[&](u32 side) noexcept {auto& out=outputs[side];return native_devanagari_run(plan,*owned.profile.indic.cmap,{owned.profile.forms,128},owned.profile.indic.substitutions(),
            {source,bytes},16,{scratch,{out.glyphs,capacity},{out.placements,capacity},{out.characters,capacity}},&results[side],{},0,side!=0);};
        CHECK(shape(0)==SfntStatus::ok&&shape(1)==SfntStatus::ok);
        CHECK(results[0].glyph_count==results[1].glyph_count&&results[0].source_units==results[1].source_units&&results[0].shaped_ranges==results[1].shaped_ranges);
        CHECK(bytes_equal(outputs[0].glyphs,outputs[1].glyphs,results[0].glyph_count*2)&&bytes_equal(outputs[0].placements,outputs[1].placements,results[0].glyph_count*sizeof(SfntPlacement))&&
            bytes_equal(outputs[0].characters,outputs[1].characters,results[0].source_units*4));
        constexpr u32 iterations=24;
        print("{\"kind\":\"deva_memo_cpu\",\"corpus\":\"");print(names[corpus]);print("\",\"bytes\":");number(bytes);print(",\"units\":");number(results[0].source_units);
        print(",\"glyphs\":");number(results[0].glyph_count);print(",\"clusters\":");number(results[0].shaped_ranges);
        print(",\"baseline_work\":");number(results[0].lookup_operations);print(",\"memo_work\":");number(results[1].lookup_operations);
        print(",\"iterations\":");number(iterations);print(",\"frequency\":");number(frequency.QuadPart);print(",\"batches\":[");
        for(u32 batch=0;batch<5;++batch){u64 ticks[2]{},cycles[2]{};
            for(u32 order=0;order<2;++order){const u32 side=order^(batch&1);CHECK(shape(side)==SfntStatus::ok);
                LARGE_INTEGER first{},last{};ULONG64 before=0,after=0;CHECK(QueryThreadCycleTime(GetCurrentThread(),&before)&&QueryPerformanceCounter(&first));
                for(u32 i=0;i<iterations;++i)CHECK(shape(side)==SfntStatus::ok);
                CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&after));ticks[side]=last.QuadPart-first.QuadPart;cycles[side]=after-before;}
            if(batch)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print(",");number(cycles[0]);print(",");number(cycles[1]);print("]");
        }print("]}\n");
    }return true;
}
