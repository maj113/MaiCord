bool historic_corpus(DWriteTextSystem system) noexcept {
    CHECK(native_text_format_capture(&system));auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    const auto compare=[&](Utf8View text) noexcept {
        const float widths[]{40,120,1000000};const u32 flags[]{0,text_style_bold,text_style_italic};
        for(float width:widths)for(u32 flag:flags){TextStyleSpan style{0,text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
            shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
            CHECK(text_shape_utf8_styled(&native,text,styles,width,1000000,{utf16,256},&native_output.text).ok());
            CHECK(text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text).ok());
            CHECK(native_output.text.native_fonts());
            if(!same(native_output,reference_output,true,true)){
                print("historic width/style=");real(width);print("/");number(flag);print("\n");
                const ShapedText* values[]{&native_output.text,&reference_output.text};
                for(const auto* value:values)for(u32 i=0;i<value->run_count;++i){IDWriteFontFace* face=nullptr;
                    CHECK(native_run_reference(owner.factory,*value,value->runs[i],&face));print_face_file(face);com_release(face);}
                return false;
            }++cases;++native_cases;
        }return true;
    };
    const u32 ranges[][2]{{0x13000,0x13430},{0x16a0,0x1700}};
    for(const auto& range:ranges)for(u32 first=range[0];first<range[1];first+=8){char text[64];u32 bytes=0;
        for(u32 cp=first;cp<minimum(first+8,range[1]);++cp){bytes+=utf8_encode(cp,text+bytes);if(cp&1)text[bytes++]=' ';}
        if(!compare({text,bytes})){print("historic first scalar=");number(first);print("\n");return false;}}
    const Utf8View mixed[]{utf8_literal("AV 𓀀𓃀𓋹 ᚠᚢᚦ 😀"),utf8_literal("אבג (ᚠᚢᚦ) 𓀀 123"),utf8_literal("ᚠ\u0308 ᚢ\u0301\n𓀀𓃀"),utf8_literal("𓀀\u200d𓃀 ᚠ\u200cᚢ")};
    for(const auto text:mixed)CHECK(compare(text));return true;
}
bool historic_cases() noexcept {
    CHECK(historic_corpus(owner));
    const wchar_t* families[]{L"Segoe UI Historic",L"Consolas"};
    for(const auto* family:families){
        auto system=owner;system.format=nullptr;
        CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-US",&system.format)));
        const bool ok=historic_corpus(system);com_release(system.format);CHECK(ok);
    }return true;
}
