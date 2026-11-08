bool native_common_punctuation_corpus(AppSystem system) noexcept {
    const Utf8View samples[]{utf8_literal("\xe2\x80\xa6"),utf8_literal("\xe2\x80\xa6\xe2\x80\xa6"),
        utf8_literal("  \xe2\x80\xa6  "),utf8_literal("(\xe2\x80\xa6)"),
        utf8_literal("\xe2\x80\x9c\xe2\x80\xa6\xe2\x80\x9d"),
        utf8_literal("123\xe2\x80\xa6"),utf8_literal("\xe2\x80\xa6 123 456"),
        utf8_literal("\xe2\x80\x94\xe2\x80\x93\xe2\x80\xa6"),
        utf8_literal("\xe2\x80\xa6\n\xe2\x80\xa6"),utf8_literal("\t\xe2\x80\xa6\t"),
        utf8_literal("\xe2\x80\xb0\xe2\x80\xb2\xe2\x80\xb3"),
        utf8_literal("\xc2\xa1\xc2\xbf\xe2\x80\xbc")};
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_monospace,text_style_underline,text_style_heading_3};
    const float widths[]{1,20,95,1000000};
    u32 sample=0;
    for(auto text:samples){for(u32 mode=0;mode<2;++mode)for(u32 pattern=0;pattern<2;++pattern)for(u32 flag:flags)for(float width:widths){
        ++cases;system.independent_letters=mode!=0;
        const u32 first=pattern?utf8_decode_one(text.data,text.bytes).width:0;
        const TextStyleSpan span{first,text.bytes-first,flag};const ConstSlice<TextStyleSpan> styles{&span,flag&&span.byte_count?1u:0u};
        if(!shape_observed(system,text,styles,width,TextWrapping::wrap)){
            print("Common punctuation sample/mode/pattern/flag/width/em=");number(sample);print("/");number(mode);print("/");number(pattern);print("/");number(flag);print("/");real(width);print("/");real(system.em_size);print("\n");return false;}
        ++native_cases;
    }++sample;}
    return true;
}
