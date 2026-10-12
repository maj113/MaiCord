bool native_devanagari_memory_benchmark(bool verify_only=false) noexcept {
    constexpr u32 limit=8192;
    struct Large {TextGlyph glyphs[limit*2];TextRun runs[limit];u16 clusters[limit];wchar_t wide[limit];char input[limit*3];};
    auto* memory=static_cast<Large*>(VirtualAlloc(nullptr,sizeof(Large)*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
    struct Restore {Large* memory;ShapedText native,reference;~Restore() noexcept {shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
        native_output.text=native;reference_output.text=reference;VirtualFree(memory,0,MEM_RELEASE);}} restore{memory,native_output.text,reference_output.text};
    CHECK(shaped_text_initialize(&native_output.text,{memory[0].glyphs,limit*2},{memory[0].runs,limit},{memory[0].clusters,limit}));
    CHECK(shaped_text_initialize(&reference_output.text,{memory[1].glyphs,limit*2},{memory[1].runs,limit},{memory[1].clusters,limit}));
    IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(L"Nirmala UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-US",&format)));
    struct Release {IDWriteTextFormat*& p;~Release() noexcept {com_release(p);}} release{format};
    auto system=owner;system.format=format;system.em_size=16;CHECK(native_text_format_capture(&system));
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    const Utf8View word=utf8_literal("र्किहिन्दीक्षेत्र");const u32 repeats[]{8,64,256};LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    for(u32 repeat:repeats){u32 bytes=0;
        if(verify_only){const auto prefix=utf8_literal("AV ffi ");for(u32 i=0;i<16;++i){copy_bytes(memory[0].input+bytes,prefix.data,prefix.bytes);bytes+=prefix.bytes;}}
        for(u32 i=0;i<repeat;++i){CHECK(word.bytes<=sizeof(memory[0].input)-bytes-3);copy_bytes(memory[0].input+bytes,word.data,word.bytes);bytes+=word.bytes;}
        const u32 tail=bytes;bytes+=utf8_encode(0x915,memory[0].input+bytes);const Utf8View text{memory[0].input,bytes};
        if(verify_only){const float widths[]{240,1000000};
            for(u32 mode=0;mode<4;++mode)for(float width:widths){native.independent_letters=reference.independent_letters=(mode&1)!=0;
                native.tracking=reference.tracking=(mode&2)?.03f:0;++cases;
                const TextStyleSpan style{112+word.bytes*(repeat/2),word.bytes*(repeat/2),text_style_bold};
                CHECK(text_shape_utf8_styled(&native,text,{&style,1},width,1000000,{memory[0].wide,limit},&native_output.text).ok()&&native_output.text.native_fonts());
                CHECK(text_shape_utf8_styled(&reference,text,{&style,1},width,1000000,{memory[1].wide,limit},&reference_output.text).ok());
                CHECK(same(native_output,reference_output,true));++native_cases;}
            continue;}
        for(u32 variant=0;variant<2;++variant){utf8_encode(0x915+variant,memory[0].input+tail);
            CHECK(text_shape_utf8(&native,text,240,1000000,{memory[0].wide,limit},&native_output.text).ok()&&native_output.text.native_fonts());
            CHECK(text_shape_utf8(&reference,text,240,1000000,{memory[1].wide,limit},&reference_output.text).ok());CHECK(same(native_output,reference_output,true));}
        const auto storage=native_text_candidate_storage_counts(system);
        print("{\"kind\":\"deva_storage\",\"bytes\":");number(bytes);print(",\"units\":");number(native_output.text.cluster_count);
        print(",\"committed\":");number(storage.committed);print(",\"peak\":");number(storage.peak);print(",\"retries\":");number(storage.retries);print("}\n");
        const u32 iterations=repeat==256?8u:repeat==64?16u:64u;
        for(u32 mode=0;mode<2;++mode){print("{\"kind\":\"deva_cpu\",\"bytes\":");number(bytes);print(",\"mode\":");number(mode);print(",\"iterations\":");number(iterations);
            print(",\"frequency\":");number(frequency.QuadPart);print(",\"batches\":[");
            for(u32 batch=0;batch<5;++batch){u64 ticks[2]{},cycles[2]{};
                for(u32 order=0;order<2;++order){const u32 side=order^(batch&1);auto& active=side?reference:native;auto& output=side?reference_output:native_output;
                    utf8_encode(0x915,memory[0].input+tail);CHECK(text_shape_utf8(&active,text,240,1000000,{memory[side].wide,limit},&output.text).ok());
                    LARGE_INTEGER first{},last{};ULONG64 before=0,after=0;CHECK(QueryThreadCycleTime(GetCurrentThread(),&before)&&QueryPerformanceCounter(&first));
                    for(u32 i=0;i<iterations;++i){if(mode)utf8_encode(0x915+((i+1)&1),memory[0].input+tail);
                        CHECK(text_shape_utf8(&active,text,240,1000000,{memory[side].wide,limit},&output.text).ok()&&(side||output.text.native_fonts()));}
                    CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&after));ticks[side]=last.QuadPart-first.QuadPart;cycles[side]=after-before;
                }
                if(batch)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print(",");number(cycles[0]);print(",");number(cycles[1]);print("]");
            }print("]}\n");}
    }
    if(verify_only){const auto storage=native_text_candidate_storage_counts(system);CHECK(!storage.retries);
        print("compact scratch committed/peak=");number(storage.committed);print("/");number(storage.peak);print("\n");}
    return true;
}
