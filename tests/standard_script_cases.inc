bool native_standard_script_corpus(DWriteTextSystem system) noexcept {
    const Utf8View ordinary[]{utf8_literal("Բարև աշխարհ"),utf8_literal("և եւ ﬓ ﬔ ﬕ ﬖ ﬗ"),
        utf8_literal("մն մե մի վն մխ"),utf8_literal("բարև՜ հարց՞ բառ։"),utf8_literal("ա́ ս̄"),
        utf8_literal("մ‍ն մ‌ն"),utf8_literal("ქართული ტექსტი"),utf8_literal("ᲥᲐᲠᲗᲣᲚᲘ ქართული"),
        utf8_literal("ႠႡႢ ⴀⴁⴂ"),utf8_literal("ა́ ა̄"),utf8_literal("ა‍ბ ა‌ბ"),
        utf8_literal("Հայերեն\r\nქართული"),utf8_literal("A Հայերեն 😀 ქართული B"),utf8_literal("(ქართული) שלום Հայերեն")};
    const Utf8View extended[]{utf8_literal("ሰላም ዓለም"),utf8_literal("፩፪፫ ፲፻፼ ፡።፣፤፥፦፧፨"),
        utf8_literal("ሀ፝ ሁ፞ ሂ፟"),utf8_literal("ሀ́ ሁ̄"),utf8_literal("ሀ‍ሁ ሀ‌ሁ"),
        utf8_literal("ᎣᏏᏲ ᏣᎳᎩ"),utf8_literal("ꭰꭱꭲ ᎠᎡᎢ"),utf8_literal("Ꭰ́ Ꭱ̄"),
        utf8_literal("Ꭰ‍Ꭱ Ꭰ‌Ꭱ"),utf8_literal("ሰላም\r\nᎣᏏᏲ"),
        utf8_literal("A ሰላም 😀 ᎣᏏᏲ B"),utf8_literal("(ሰላም) שלום ᎣᏏᏲ"),
        utf8_literal("\u202eሰላም ᎣᏏᏲ\u202c"),utf8_literal("ሀ\u034f́ Ꭰ\u034f̄"),utf8_literal("ⶀⶁⶂ ꬁꬂꬃ"),
        utf8_literal("ᏰᏱᏲ ᏸᏹᏺ"),utf8_literal("፝፞፟ ሀ")};
    const ConstSlice<Utf8View> samples=extended_standard_profiles?ConstSlice<Utf8View>{extended,sizeof(extended)/sizeof(extended[0])}:ConstSlice<Utf8View>{ordinary,sizeof(ordinary)/sizeof(ordinary[0])};
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_underline,text_style_monospace,text_style_heading_3};const float widths[]{1000000,37};
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    for(u32 sample=0;sample<samples.count;++sample)for(u32 partial=0;partial<((extended_standard_profiles?(sample==3||sample==7):(sample==4||sample==9))?2u:1u);++partial)for(u32 mode=0;mode<2;++mode)for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
        ++cases;native.independent_letters=reference.independent_letters=mode!=0;const auto text=samples[sample];const TextStyleSpan style{0,partial?static_cast<u32>(utf8_decode_one(text.data,text.bytes).width):text.bytes,flag};
        const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
        const auto expected=text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrap);
        const auto actual=text_shape_utf8(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrap);
        if(!expected.ok()||!actual.ok()||!native_output.text.native_fonts()||!same(native_output,reference_output,true)||!same_visual_runs(native_output,reference_output)){
            print("standard script sample/partial/mode/flag/width/nowrap/em/error=");number(sample);print("/");number(partial);print("/");number(mode);print("/");number(flag);print("/");real(width);print("/");number(nowrap);print("/");real(system.em_size);print("/");number(static_cast<u32>(actual.error));print("\n");return false;}
        ++native_cases;if(!mode)CHECK(compare_hits(system,text,width,wrap,styles,true));
    }
    return true;
}
