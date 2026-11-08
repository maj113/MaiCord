bool native_devanagari_memory_check() noexcept {
    constexpr u32 limit=8192;
    struct Large {TextGlyph glyphs[limit*2];TextRun runs[limit];u16 clusters[limit];char input[limit*3];};
    auto* memory=static_cast<Large*>(VirtualAlloc(nullptr,sizeof(Large),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    shaped_text_reset(&native_output.text);
    struct Restore {Large* memory;ShapedText native;~Restore() noexcept {shaped_text_reset(&native_output.text);native_output.text=native;VirtualFree(memory,0,MEM_RELEASE);}} restore{memory,native_output.text};
    CHECK(shaped_text_initialize(&native_output.text,{memory->glyphs,limit*2},{memory->runs,limit},{memory->clusters,limit}));
    auto system=owner;system.em_size=16;CHECK(set_format(system,L"Nirmala UI",16));auto native=system;
    const Utf8View word=utf8_literal("र्किहिन्दीक्षेत्र");const u32 repeats[]{8,64,256};
    for(u32 repeat:repeats){u32 bytes=0;
        const auto prefix=utf8_literal("AV ffi ");for(u32 i=0;i<16;++i){copy_bytes(memory->input+bytes,prefix.data,prefix.bytes);bytes+=prefix.bytes;}
        for(u32 i=0;i<repeat;++i){CHECK(word.bytes<=sizeof(memory->input)-bytes-3);copy_bytes(memory->input+bytes,word.data,word.bytes);bytes+=word.bytes;}
        bytes+=utf8_encode(0x915,memory->input+bytes);const Utf8View text{memory->input,bytes};
        {const float widths[]{240,1000000};
            for(u32 mode=0;mode<4;++mode)for(float width:widths){native.independent_letters=(mode&1)!=0;
                native.tracking=(mode&2)?.03f:0;++cases;
                const TextStyleSpan style{112+word.bytes*(repeat/2),word.bytes*(repeat/2),text_style_bold};
                CHECK(text_shape_utf8(&native, {text,{&style,1},width,1000000}, &native_output.text).ok()&&native_output.text.native_fonts());
                observe(native_output.text,text,native);++native_cases;}
        }
    }
    {const auto storage=native_text_candidate_storage_counts(system);CHECK(!storage.retries);
        print("compact scratch committed/peak=");number(storage.committed);print("/");number(storage.peak);print("\n");}
    return true;
}
