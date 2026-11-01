// Logical Hebrew items, including wrapped lines and trailing whitespace.
// Every comparison requires native execution before checking the reference.
bool native_hebrew_corpus(DWriteTextSystem system) noexcept {
    const Utf8View texts[]{
        utf8_literal("\xd7\x90"),utf8_literal("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"),
        utf8_literal("\xd7\x90\xd7\x91 \xd7\x92\xd7\x93"),utf8_literal("\xd7\x90\xd6\xb0"),
        utf8_literal("\xd7\x91\xd6\xbc"),utf8_literal("\xd7\xa9\xd7\x81"),
        utf8_literal("\xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81"),utf8_literal("\xd7\xa9\xd7\x81\xd6\xbc\xd6\xb8"),
        utf8_literal("\xd7\xa9\xd7\x82\xd6\xbc\xd6\xb7"),utf8_literal("\xd7\x95\xd6\xb9"),
        utf8_literal("\xd7\x95\xd6\xba"),utf8_literal("\xd7\x90\xd6\x91\xd6\xb0"),
        utf8_literal("\xd7\x90\xd6\xb0\xd6\x91"),utf8_literal("\xd7\x90\xcd\x8f\xd6\xb0"),
        utf8_literal("\xd7\x90\xe2\x80\x8d\xd7\x91"),utf8_literal("\xd7\x90\xe2\x80\x8c\xd7\x91"),
        utf8_literal("\xef\xac\xaa"),utf8_literal("\xef\xac\xac"),utf8_literal("\xef\xac\xb1"),
        utf8_literal("\xd7\x90\xcc\x81"),utf8_literal("\xd7\x90\xcc\x81\xd6\xb0"),
        utf8_literal("\xd7\x90\xd6\xb0\xcc\x81"),utf8_literal("\xd7\x90\xd6\xb0\xd6\xb0"),
        utf8_literal("\xd7\x90\xd6\xb0\xd6\xb1"),utf8_literal("\xd7\x90\xd6\xb1\xd6\xb0"),
        utf8_literal("\xd7\x90\xd6\xb0\xd6\xbc\xd7\x81"),
        utf8_literal("\xd7\x90 "),utf8_literal("\xd7\x90\xd7\x91   \xd7\x92\xd7\x93   "),
        utf8_literal("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\x90\xd6\xb0\xd6\xb0 \xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81"),
        utf8_literal("\xd7\x90\xd6\xb0\xd6\xb0\xd6\xb0"),
        utf8_literal("\xd7\x90\xd6\xb0\xd6\xb1\xd6\xb0"),
        utf8_literal("\xd7\x90\xcc\x81\xcc\x81\xd6\xb0"),
        utf8_literal("\xd7\x90\xd6\xb0 \xd7\x91\xd6\xbc \xd7\xa9\xd7\x81 "),
        utf8_literal("\xd7\x90\xd7\x91\xd7\x92\xd7\x93\xd7\x94\xd7\x95\xd7\x96\xd7\x97\xd7\x98\xd7\x99"),
        utf8_literal("\xd7\x90\xd7\x91 \xd7\x92\xd7\x93  \xd7\x94\xd7\x95   \xd7\x96\xd7\x97    "),
        utf8_literal("\xd7\x90\xe2\x80\x8d"),utf8_literal("\xd7\x90\xe2\x80\x8c"),
        utf8_literal("\xd7\x90 \xe2\x80\x8d"),utf8_literal("\xd7\x90\xe2\x80\x8d "),
        utf8_literal("\xd7\x90\xcd\x8f"),utf8_literal("\xd7\x90\xd6\xb0\xcd\x8f ")};
    auto native=system;native.factory=nullptr;native.format=nullptr;auto reference=system;reference.native_text=nullptr;
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_bold|text_style_italic,text_style_monospace};
    const float widths[]{1,8,16,37,95,1000000};constexpr u32 width_count=sizeof(widths)/sizeof(widths[0]);
    const u32 sample_count=hebrew_wrap_profiles?static_cast<u32>(sizeof(texts)/sizeof(texts[0])):26;
    for(u32 mode=0;mode<2;++mode){native.independent_letters=reference.independent_letters=mode!=0;
        for(u32 sample=0;sample<sample_count;++sample)for(u32 flag:flags)for(u32 nowrap=0;nowrap<2;++nowrap)for(u32 wi=hebrew_wrap_profiles?0u:width_count-1;wi<width_count;++wi){
            const auto text=texts[sample];const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            const float width=wi==width_count-1?widths[wi]:widths[wi]*(system.em_size/16);
            const TextStyleSpan style{0,text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
            const auto diagnostic=[&]() noexcept {print("Hebrew sample/em/independent/nowrap/flag/width=");number(sample);print("/");real(system.em_size);print("/");number(mode);print("/");number(nowrap);print("/");number(flag);print("/");real(width);print("\n");};
            ++cases;shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
            CHECK(text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrapping).ok());
            if(!text_shape_utf8(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrapping).ok()||!native_output.text.native_fonts()){diagnostic();CHECK(inspect_style_layout(system,text,styles,width,wrapping));CHECK(false);}++native_cases;
            if(!same(native_output,reference_output,false,true)){diagnostic();
                const ShapedText* values[]{&native_output.text,&reference_output.text};
                for(u32 v=0;v<2;++v){print(v?"ref glyphs=":"native glyphs=");for(u32 g=0;g<values[v]->glyph_count;++g){number(values[v]->glyphs[g].index);print(" ");}print("\n");}
                CHECK(inspect_style_layout(system,text,styles,width,wrapping));return false;}
            const auto a=text_measure_utf8(&native,text,styles,width,1000000,{utf16,256},wrapping),b=text_measure_utf8(&reference,text,styles,width,1000000,{utf16,256},wrapping);
            CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count&&a.utf16_units==b.utf16_units);
            if(!nowrap&&!mode)CHECK(draw_source(system,native_output.text));
        }
    }return true;
}
