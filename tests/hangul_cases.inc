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
