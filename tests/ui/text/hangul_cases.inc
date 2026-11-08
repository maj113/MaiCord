bool native_hangul_corpus(AppSystem system) noexcept {
    const Utf8View samples[]{utf8_literal("한국어 메시지"),utf8_literal("한글"),utf8_literal("각 각"),
        utf8_literal("A 한글 😀 B"),utf8_literal("한글\r\n한국어"),utf8_literal("가 각 힣"),
        utf8_literal("ㄱㅏ ㄴㅏ"),utf8_literal("ﾡￂ ﾤￂ"),utf8_literal("ᄀ ᅡ ᆨ"),
        utf8_literal("ᄓᅡᇫ"),utf8_literal("가〮 각〯"),utf8_literal("ᅟᅠᆨ"),utf8_literal("가́ 한글"),
        utf8_literal("가ᇫ 가ᇫ"),utf8_literal("가 ᄓᅡᇫ 가"),utf8_literal("〮 가〮〯"),
        utf8_literal("ᄓᅡᇫ〮 가〮"),utf8_literal("각ᇫ")};
    const u32 flags[]{0,text_style_bold,text_style_heading_3,text_style_underline};
    const float widths[]{1000000,37};
    for(u32 sample=0;sample<sizeof(samples)/sizeof(samples[0]);++sample)for(u32 mode=0;mode<2;++mode)for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
        ++cases;system.independent_letters=mode!=0;const auto text=samples[sample];const TextStyleSpan style{0,text.bytes,flag};
        const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
        if(!shape_observed(system,text,styles,width,wrap)){
            print("Hangul sample/mode/flag/width/nowrap/em=");number(sample);print("/");number(mode);print("/");number(flag);print("/");real(width);print("/");number(nowrap);print("/");real(system.em_size);print("\n");return false;}
        ++native_cases;
        if(!mode)CHECK(compare_hits(system,text,width,wrap,styles));
    }
    return true;
}
