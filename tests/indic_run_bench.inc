// Warm complete runs through the retained plan; no font preparation, allocation,
// paragraph cache, hashing or output logging in the timed interval.
bool indic_run_benchmark(const NativeRunPlanStore& store,const NativeIndicFormPlan& plan,SfntShapeStorage scratch) noexcept {
    const bool gujarati=plan.indic.profile==SfntIndicProfile::gujarati;
    const Utf8View natural=gujarati?utf8_literal("ગુજરાતી ભાષા મિત્ર સમાચાર કિ કો કૌ ર્કિ "):utf8_literal("বাংলা ভাষা বন্ধু সংবাদ কো কৌ র্কি ");
    const Utf8View complex=gujarati?utf8_literal("ક્ષેત્ર શ્રદ્ધા ક્‍ષ ક્‌ષ "):utf8_literal("ক্ষেত্র শ্রদ্ধা ক্‍ষ ক্‌ষ ");
    const char* names[]{"short","natural","conjuncts","single-scalars","mark-tail","direction-tracking"};
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    for(u32 corpus=0;corpus<6;++corpus){char text[2048]{};u32 bytes=0;
        if(corpus==3){for(u32 i=0;i<256;++i)bytes+=utf8_encode(gujarati?0xa95u:0x995u,text+bytes);}
        else if(corpus==4){for(u32 repeat=0;repeat<8;++repeat){bytes+=utf8_encode(gujarati?0xa95u:0x995u,text+bytes);
            for(u32 i=0;i<30;++i)bytes+=utf8_encode(gujarati?0xabeu:0x9beu,text+bytes);}}
        else{const auto word=corpus==2?complex:natural;const u32 repetitions=corpus?6u:1u;
            for(u32 i=0;i<repetitions;++i){CHECK(bytes+word.bytes<=sizeof(text));copy_bytes(text+bytes,word.data,word.bytes);bytes+=word.bytes;}}
        NativeAsciiStorage output{{},scratch,{reinterpret_cast<u16*>(scratch.glyphs.data),scratch.glyphs.count},scratch.placements,scratch.character_glyphs};
        NativeAsciiResult result{};const bool rtl=corpus==5;const float spacing=rtl?.25f:0;
        const auto shape=[&]() noexcept {return indic_owned_run(plan.indic.profile,store,plan,{text,bytes},16,output,&result,{},rtl,spacing);};
        CHECK(shape()==SfntStatus::ok);
        u64 hash=14695981039346656037ull;
        const auto append=[&](const void* data,usize count) noexcept {const auto* p=static_cast<const u8*>(data);for(usize i=0;i<count;++i){hash^=p[i];hash*=1099511628211ull;}};
        append(output.glyphs.data,result.glyph_count*2);append(output.placements.data,result.glyph_count*sizeof(SfntPlacement));append(output.character_glyphs.data,result.source_units*4);
        print("{\"script\":\"");print(gujarati?"gujarati":"bengali");print("\",\"corpus\":\"");print(names[corpus]);print("\",\"bytes\":");number(bytes);
        print(",\"units\":");number(result.source_units);print(",\"glyphs\":");number(result.glyph_count);print(",\"hash\":");number(hash);
        constexpr u32 iterations=24;print(",\"iterations\":");number(iterations);print(",\"frequency\":");number(frequency.QuadPart);print(",\"batches\":[");
        for(u32 batch=0;batch<7;++batch){CHECK(shape()==SfntStatus::ok);LARGE_INTEGER first{},last{};ULONG64 before=0,after=0;
            CHECK(QueryThreadCycleTime(GetCurrentThread(),&before)&&QueryPerformanceCounter(&first));
            for(u32 i=0;i<iterations;++i)CHECK(shape()==SfntStatus::ok);
            CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&after));
            if(batch)print(",");print("[");number(last.QuadPart-first.QuadPart);print(",");number(after-before);print("]");}
        print("]}\n");
    }return true;
}
