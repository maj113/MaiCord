bool native_common_punctuation_corpus(DWriteTextSystem system) noexcept {
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
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    u32 sample=0;
    for(auto text:samples){for(u32 mode=0;mode<2;++mode)for(u32 pattern=0;pattern<2;++pattern)for(u32 flag:flags)for(float width:widths){
        ++cases;native.independent_letters=reference.independent_letters=mode!=0;
        const u32 first=pattern?utf8_decode_one(text.data,text.bytes).width:0;
        const TextStyleSpan span{first,text.bytes-first,flag};const ConstSlice<TextStyleSpan> styles{&span,flag&&span.byte_count?1u:0u};
        const auto a=text_shape_utf8(&native,text,styles,width,1000000,{utf16,256},&native_output.text);
        const auto b=text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text);
        if(!a.ok()||!native_output.text.native_fonts()||!b.ok()||!same(native_output,reference_output,true)){
            print("Common punctuation sample/mode/pattern/flag/width/em/errors=");number(sample);print("/");number(mode);print("/");number(pattern);print("/");number(flag);print("/");real(width);print("/");real(system.em_size);print("/");number(static_cast<u32>(a.error));print("/");number(static_cast<u32>(b.error));print("\n");return false;}
        ++native_cases;
    }++sample;}
    return true;
}
