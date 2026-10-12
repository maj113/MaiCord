bool native_hangul_corpus(DWriteTextSystem system) noexcept {
    const Utf8View samples[]{utf8_literal("한국어 메시지"),utf8_literal("한글"),utf8_literal("각 각"),
        utf8_literal("A 한글 😀 B"),utf8_literal("한글\r\n한국어"),utf8_literal("가 각 힣"),
        utf8_literal("ㄱㅏ ㄴㅏ"),utf8_literal("ﾡￂ ﾤￂ"),utf8_literal("ᄀ ᅡ ᆨ"),
        utf8_literal("ᄓᅡᇫ"),utf8_literal("가〮 각〯"),utf8_literal("ᅟᅠᆨ"),utf8_literal("가́ 한글"),
        utf8_literal("가ᇫ 가ᇫ"),utf8_literal("가 ᄓᅡᇫ 가"),utf8_literal("〮 가〮〯"),
        utf8_literal("ᄓᅡᇫ〮 가〮"),utf8_literal("각ᇫ")};
    const u32 flags[]{0,text_style_bold,text_style_heading_3,text_style_underline};
    const float widths[]{1000000,37};auto native=system,reference=system;
    native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    for(u32 sample=0;sample<sizeof(samples)/sizeof(samples[0]);++sample)for(u32 mode=0;mode<2;++mode)for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
        ++cases;native.independent_letters=reference.independent_letters=mode!=0;const auto text=samples[sample];const TextStyleSpan style{0,text.bytes,flag};
        const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
        const auto expected=text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrap);
        const auto actual=text_shape_utf8_styled(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrap);
        if(!expected.ok()||!actual.ok()||!native_output.text.native_fonts()||!same(native_output,reference_output,true)||!same_visual_runs(native_output,reference_output)){
            const ShapedText* outputs[]{&native_output.text,&reference_output.text};for(u32 side=0;side<2;++side){const auto& value=*outputs[side];print(side?"reference Hangul glyphs\n":"native Hangul glyphs\n");
                for(u32 g=0;g<value.glyph_count;++g){number(value.glyphs[g].index);print("/");real(value.glyphs[g].advance);print(" ");}print("\n");
                if(value.glyph_count)CHECK(describe_run_font(value,0));}
            print("Hangul sample/mode/flag/width/nowrap/em/error=");number(sample);print("/");number(mode);print("/");number(flag);print("/");real(width);print("/");number(nowrap);print("/");real(system.em_size);print("/");number(static_cast<u32>(actual.error));print("\n");return false;}
        ++native_cases;
        if(!mode)CHECK(compare_hits(system,text,width,wrap,styles,true));
    }
    return true;
}

bool native_hangul_benchmark() noexcept {
    CHECK(native_text_format_capture(&owner));
    const Utf8View samples[]{utf8_literal("한국어 메시지와 여러 단어를 줄 바꿈하여 표시합니다. ᄓᅡᇫ 가〮 각〯 😀"),
        utf8_literal("다른 한국어 메시지로 내용을 바꾸고 다시 배치합니다. 가ᇫ 가〯 각〮 😀")};
    auto native=owner,reference=owner;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    for(auto text:samples){CHECK(text_shape_utf8(&native,text,180,1000,{utf16,256},&native_output.text).ok()&&native_output.text.native_fonts());
        CHECK(text_shape_utf8(&reference,text,180,1000,{utf16,256},&reference_output.text).ok()&&same(native_output,reference_output,true));}
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    for(u32 mode=0;mode<3;++mode){print("{\"kind\":\"hangul_benchmark\",\"mode\":");number(mode);print(",\"iterations\":256,\"frequency\":");number(frequency.QuadPart);print(",\"batches\":[");
        for(u32 batch=0;batch<7;++batch){u64 ticks[2]{},cycles[2]{};
            for(u32 order=0;order<2;++order){const u32 side=order^(batch&1);auto& system=side?reference:native;auto& output=side?reference_output:native_output;
                CHECK(text_shape_utf8(&system,samples[0],180,1000,{utf16,256},&output.text).ok());
                LARGE_INTEGER first{},last{};ULONG64 before=0,after=0;CHECK(QueryThreadCycleTime(GetCurrentThread(),&before)&&QueryPerformanceCounter(&first));
                for(u32 i=0;i<256;++i){const float width=mode==2?180.f+static_cast<float>(i%32):180.f;
                    CHECK(text_shape_utf8(&system,samples[mode==1?(i&1):0],width,1000,{utf16,256},&output.text).ok());
                    CHECK(side||output.text.native_fonts());}
                CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&after));ticks[side]=last.QuadPart-first.QuadPart;cycles[side]=after-before;
            }
            if(batch)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print(",");number(cycles[0]);print(",");number(cycles[1]);print("]");
        }print("]}\n");
    }
    const auto stats=native_font_session_stats(owner.native_fonts);
    print("{\"kind\":\"native_font_storage\",\"mapped_bytes\":");number(stats.live.mapped_bytes);print(",\"metadata_commit\":");number(stats.metadata_committed);
    print(",\"instance_commit\":");number(stats.instance_committed);print("}\n");
    const auto scratch=native_text_candidate_storage_counts(owner);
    print("{\"kind\":\"native_scratch_storage\",\"reserved\":");number(scratch.reserved);print(",\"committed\":");number(scratch.committed);
    print(",\"peak\":");number(scratch.peak);print(",\"retries\":");number(scratch.retries);print("}\n");return true;
}
