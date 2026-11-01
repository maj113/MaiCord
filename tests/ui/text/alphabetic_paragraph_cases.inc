bool native_alphabetic_paragraph_corpus() noexcept {
    const struct Sample {Utf8View text;const wchar_t* family;} samples[]{
        {utf8_literal("Հայերեն բարեւ աշխարհ"),L"Sylfaen"},
        {utf8_literal("ქართული გამარჯობა"),L"Sylfaen"},
        {utf8_literal("ᏣᎳᎩ ᎣᏏᏲ ᎡᎶᎯ"),L"Gadugi"},
        {utf8_literal("አማርኛ ሰላም ዓለም"),L"Nyala"},
        {utf8_literal("ᚠᚢᚦ ᚱᚲ"),L"Segoe UI Symbol"}};
    const Utf8View suffixes[]{utf8_literal(""),utf8_literal(" !?"),utf8_literal("  "),utf8_literal("́"),
        utf8_literal("\ufe0f"),utf8_literal("\n"),utf8_literal("\t"),utf8_literal(" AV"),
        utf8_literal(" אב"),utf8_literal(" 😀"),utf8_literal(" 123")};
    const float sizes[]{16,17.5f},widths[]{8,37,95,1000000},spacings[]{0,.03f};
    u32 sample_index=0;
    for(const auto& sample:samples){const wchar_t* families[]{sample.family,L"Segoe UI",L"Consolas"};
        for(const auto* family:families)for(float em:sizes){
            IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
            struct Release {IDWriteTextFormat*& f;~Release() noexcept {com_release(f);}} release{format};
            auto system=owner;system.format=format;system.em_size=em;CHECK(native_text_format_capture(&system));
            auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
            u32 variant=0;for(auto suffix:suffixes){char bytes[256]{};copy_bytes(bytes,sample.text.data,sample.text.bytes);copy_bytes(bytes+sample.text.bytes,suffix.data,suffix.bytes);
                const Utf8View text{bytes,sample.text.bytes+suffix.bytes};
                for(float spacing:spacings)for(u32 independent=0;independent<2;++independent)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap)for(u32 trimmed=0;trimmed<2;++trimmed){
                    native.tracking=reference.tracking=spacing;native.independent_letters=reference.independent_letters=independent!=0;
                    const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;const auto overflow=trimmed?TextOverflow::ellipsis:TextOverflow::clip;
                    const float height=trimmed?em*2:1000000;
                    const auto a=text_shape_utf8(&native,text,{},width,height,{utf16,256},&native_output.text,wrapping,overflow);
                    const auto b=text_shape_utf8(&reference,text,{},width,height,{utf16,256},&reference_output.text,wrapping,overflow);
                    if(!a.ok()||!b.ok()||!native_output.text.native_fonts()||!same(native_output,reference_output,true)){
                        print("Alphabetic sample/variant/em/spacing/independent/width/nowrap/trimmed=");number(sample_index);print("/");number(variant);print("/");real(em);print("/");real(spacing);print("/");number(independent);print("/");real(width);print("/");number(nowrap);print("/");number(trimmed);print("\n");return false;}
                    ++cases;++native_cases;
                }
                for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap)
                    CHECK(compare_hits(system,text,width,nowrap?TextWrapping::no_wrap:TextWrapping::wrap,{},true));
                ++variant;
            }
        }++sample_index;
    }return true;
}
