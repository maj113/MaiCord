bool native_mixed_bidi_corpus(AppSystem system) noexcept {
    const Utf8View texts[]{
        utf8_literal("abc \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d xyz"),
        utf8_literal("\xd7\x90 abc \xd7\x91"),
        utf8_literal("abc \xd7\x90"),utf8_literal("\xd7\x90 abc"),
        utf8_literal("\xd7\x90 123 \xd7\x91"),
        utf8_literal("hello (\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d) world"),
        utf8_literal("\xd7\x90 (abc) \xd7\x91"),
        utf8_literal("A \xd7\x90\xd6\xb0\xd6\xb0 B \xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 C"),
        utf8_literal("\xce\xb1 \xd7\x90 \xd0\x91 abc"),
        utf8_literal("  abc \xd7\x90   "),
        utf8_literal("abc \xd7\x90\n\xd7\x91 xyz"),
        utf8_literal("abc\t\xd7\x90 xyz"),utf8_literal("\xd7\x90\t\xd7\x91 xyz"),
        utf8_literal("abc \xd7\x90  \t xyz"),utf8_literal("\xd7\x90 abc\t"),
        utf8_literal("abc \xd7\x90\r\n\xd7\x91 xyz\n"),
        utf8_literal("one \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d two \xd7\x90\xd6\xb0\xd6\xb0 three \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d four"),
        utf8_literal("\xd7\x90 [abc (123)] \xd7\x91"),
        utf8_literal("abc (\xd7\x90 [xyz]) 42"),
        utf8_literal("\xd7\x90 ($12.50) + 45% \xd7\x91"),
        utf8_literal("12,345 \xd7\x90 67:89 abc"),
        utf8_literal("\xd7\x90 () [] {} xyz"),
        utf8_literal("(abc \xd7\x90) [\xd7\x91 xyz]"),
        utf8_literal("\xd7\x90 - + / : ; ! ? xyz"),
        utf8_literal("A\xcc\x81 \xd7\x90\xd6\xb0 B\xcc\x88"),
        utf8_literal("\xd7\x90 \xce\xb1\xcc\x81 \xd0\x91\xcc\x88 \xd7\x91"),
        utf8_literal("\xd7\x90\t(abc)\t\xd7\x91"),
        utf8_literal("\xd7\x90 (abc\nxyz) \xd7\x91"),
        utf8_literal("abc \xd7\x90\rxyz \xd7\x91"),
        utf8_literal("\xd7\x90\n\nabc\n\xd7\x91"),
        utf8_literal("\xd7\x90\xc2\xa0" "abc \xd7\x91"),
        utf8_literal("\xd7\x90\xe2\x80\x89" "abc \xd7\x91")};
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_bold|text_style_italic,text_style_monospace};
    const float widths[]{1,8,16,37,95,1000000};constexpr u32 width_count=sizeof(widths)/sizeof(widths[0]);
    for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;
        for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample)for(u32 flag:flags)for(u32 nowrap=0;nowrap<2;++nowrap)for(u32 wi=mixed_bidi_wrap?0u:width_count-1;wi<width_count;++wi){
            const auto text=texts[sample];const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            const float width=wi==width_count-1?widths[wi]:widths[wi]*(system.em_size/16);
            const TextStyleSpan style{0,text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
            const auto diagnostic=[&]() noexcept {print("Mixed bidi sample/em/independent/nowrap/flag/width=");number(sample);print("/");real(system.em_size);print("/");number(mode);print("/");number(nowrap);print("/");number(flag);print("/");real(width);print("\n");};
            ++cases;shaped_text_reset(&native_output.text);
            if(!layout(system,text,styles,width,wrapping)){diagnostic();return false;}++native_cases;
            if(!nowrap&&!mode)CHECK(draw_source(system,native_output.text));
        }
    }return true;
}
bool native_mixed_bidi_gates() noexcept {
    // Admission restrictions of the native engine: RLI/PDI (i=6) and tracked Hebrew (i=10) are admitted; RLM (i=7)
    // and unmatched brackets are declined.
    const Utf8View texts[]{utf8_literal("abc \xd7\x90 ("),utf8_literal("abc \xd7\x90 )"),
        utf8_literal("abc [\xd7\x90)"),utf8_literal("abc \xd7\x90 \xf0\x9f\x98\x80"),
        utf8_literal("abc \xd7\x90 1\xe2\x83\xa3"),utf8_literal("abc \xd7\x90 1\xef\xb8\x8f\xe2\x83\xa3"),
        utf8_literal("abc \xe2\x81\xa7\xd7\x90\xe2\x81\xa9"),utf8_literal("abc \xe2\x80\x8f\xd7\x90"),
        utf8_literal("abc \xd7\x90 xyz"),utf8_literal("abc \xd7\x90 xyz"),utf8_literal("abc \xd7\x90 xyz")};
    for(u32 i=0;i<sizeof(texts)/sizeof(texts[0]);++i){auto native=owner;
        const TextStyleSpan style{0,i==8?3u:texts[i].bytes,i==8?text_style_bold:text_style_underline};
        const ConstSlice<TextStyleSpan> styles{&style,(i==8||i==9)?1u:0u};
        if(i==10)native.tracking=1;
        ++cases;shaped_text_reset(&native_output.text);
        const bool admitted=(i>=3&&i<=6)||i==8||i==9||i==10;
        CHECK(text_shape_utf8(&native, {texts[i],styles,37,1000000}, &native_output.text).ok()==admitted);
        if(admitted){CHECK(native_output.text.native_fonts());++native_cases;observe(native_output.text,texts[i],native);}
    }return true;
}
