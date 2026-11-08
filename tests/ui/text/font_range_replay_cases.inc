bool native_font_range_replay_cases() noexcept {
    CHECK(native_script_storage_cases());
    struct Large {TextGlyph glyphs[4096];TextRun runs[4096];u16 clusters[4096];char input[4096];};
    auto* storage=static_cast<Large*>(VirtualAlloc(nullptr,sizeof(Large),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(storage);
    shaped_text_reset(&native_output.text);
    struct Restore {Large* memory;ShapedText native;~Restore() noexcept {
        shaped_text_reset(&native_output.text);native_output.text=native;VirtualFree(memory,0,MEM_RELEASE);
    }} restore{storage,native_output.text};
    CHECK(shaped_text_initialize(&native_output.text,{storage->glyphs,4096},{storage->runs,4096},{storage->clusters,4096}));
    auto native=owner;
    // Two alternating fallback faces: prepared-span replay, overflow, one slot,
    // no slots, alignment tails and the frontend's maximum input length.
    const auto pair=utf8_literal("한a");for(u32 i=0;i<1024;++i)copy_bytes(storage->input+i*4,pair.data,4);
    const u32 lengths[]{4,239,240,243,244,247,248,256,312,313,314,315,316,319,320,4000,4064,4092,4093,4094,4095,4096};const float widths[]{180.f,1000000.f};
    for(u32 bytes:lengths){char saved[3]{};const u32 tail=bytes%4;
        for(u32 i=0;i<tail;++i){saved[i]=storage->input[bytes-tail+i];storage->input[bytes-tail+i]='a';}
        const Utf8View text{storage->input,bytes};
        for(float width:widths){++cases;
            CHECK(text_shape_utf8(&native, {text,{},width,1000000}, &native_output.text).ok()&&native_output.text.native_fonts());
            observe(native_output.text,text,native);++native_cases;}
        for(u32 i=0;i<tail;++i)storage->input[bytes-tail+i]=saved[i];
    }
    return true;
}
