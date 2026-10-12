bool native_font_range_replay_cases() noexcept {
    CHECK(native_script_storage_cases());
    struct Large {TextGlyph glyphs[4096];TextRun runs[4096];u16 clusters[4096];wchar_t utf16[4096];char input[4096];};
    auto* storage=static_cast<Large*>(VirtualAlloc(nullptr,sizeof(Large)*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(storage);
    shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
    struct Restore {Large* memory;ShapedText native,reference;~Restore() noexcept {
        shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
        native_output.text=native;reference_output.text=reference;VirtualFree(memory,0,MEM_RELEASE);
    }} restore{storage,native_output.text,reference_output.text};
    CHECK(shaped_text_initialize(&native_output.text,{storage[0].glyphs,4096},{storage[0].runs,4096},{storage[0].clusters,4096}));
    CHECK(shaped_text_initialize(&reference_output.text,{storage[1].glyphs,4096},{storage[1].runs,4096},{storage[1].clusters,4096}));
    CHECK(native_text_format_capture(&owner));auto native=owner,reference=owner;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    // Two alternating fallback faces: prepared-span replay, overflow, one slot,
    // no slots, alignment tails and the frontend's maximum input length.
    const auto pair=utf8_literal("한a");for(u32 i=0;i<1024;++i)copy_bytes(storage[0].input+i*4,pair.data,4);
    const u32 lengths[]{4,239,240,243,244,247,248,256,312,313,314,315,316,319,320,4000,4064,4092,4093,4094,4095,4096};const float widths[]{180.f,1000000.f};
    for(u32 bytes:lengths){char saved[3]{};const u32 tail=bytes%4;
        for(u32 i=0;i<tail;++i){saved[i]=storage[0].input[bytes-tail+i];storage[0].input[bytes-tail+i]='a';}
        const Utf8View text{storage[0].input,bytes};
        for(float width:widths){++cases;
            CHECK(text_shape_utf8(&native,text,width,1000000,{storage[0].utf16,4096},&native_output.text).ok()&&native_output.text.native_fonts());
            CHECK(text_shape_utf8(&reference,text,width,1000000,{storage[1].utf16,4096},&reference_output.text).ok());
            CHECK(same(native_output,reference_output,true)&&same_visual_runs(native_output,reference_output));++native_cases;}
        for(u32 i=0;i<tail;++i)storage[0].input[bytes-tail+i]=saved[i];
    }
    return true;
}
