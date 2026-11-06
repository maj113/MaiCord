bool native_devanagari_memory_check() noexcept {
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
    const Utf8View word=utf8_literal("र्किहिन्दीक्षेत्र");const u32 repeats[]{8,64,256};
    for(u32 repeat:repeats){u32 bytes=0;
        const auto prefix=utf8_literal("AV ffi ");for(u32 i=0;i<16;++i){copy_bytes(memory[0].input+bytes,prefix.data,prefix.bytes);bytes+=prefix.bytes;}
        for(u32 i=0;i<repeat;++i){CHECK(word.bytes<=sizeof(memory[0].input)-bytes-3);copy_bytes(memory[0].input+bytes,word.data,word.bytes);bytes+=word.bytes;}
        bytes+=utf8_encode(0x915,memory[0].input+bytes);const Utf8View text{memory[0].input,bytes};
        {const float widths[]{240,1000000};
            for(u32 mode=0;mode<4;++mode)for(float width:widths){native.independent_letters=reference.independent_letters=(mode&1)!=0;
                native.tracking=reference.tracking=(mode&2)?.03f:0;++cases;
                const TextStyleSpan style{112+word.bytes*(repeat/2),word.bytes*(repeat/2),text_style_bold};
                CHECK(text_shape_utf8(&native,text,{&style,1},width,1000000,{memory[0].wide,limit},&native_output.text).ok()&&native_output.text.native_fonts());
                CHECK(text_shape_utf8(&reference,text,{&style,1},width,1000000,{memory[1].wide,limit},&reference_output.text).ok());
                CHECK(same(native_output,reference_output,true));++native_cases;}
        }
    }
    {const auto storage=native_text_candidate_storage_counts(system);CHECK(!storage.retries);
        print("compact scratch committed/peak=");number(storage.committed);print("/");number(storage.peak);print("\n");}
    return true;
}
