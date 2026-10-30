// Four scalars from each simple script (simple_script.hpp) except N'Ko,
// whose adjacent letters join under Windows.
constexpr u32 simple_script_samples[][4]{
    {0xa6a0,0x1684c,0x168f0,0x16994},{0xa000,0xa131,0xa262,0xa393},{0xa500,0xa54b,0xa596,0xa5e1},{0x2d30,0x2d3e,0x2d4c,0x2d5a},
    {0x1400,0x14b5,0x156a,0x161f},{0x1c50,0x1c5c,0x1c68,0x1c74},{0xa4d0,0xa4dc,0xa4e8,0xa4f4},{0x10c00,0x10c12,0x10c24,0x10c36},
    {0x1680,0x1687,0x168e,0x1695},{0x2c00,0x2c18,0x2c30,0x2c48},{0x3e2,0x2c94,0x2cb6,0x2cd8},{0x10280,0x10287,0x1028e,0x10295},
    {0x102a0,0x102ac,0x102b8,0x102c4},{0x10300,0x10309,0x10312,0x1031b},{0x10330,0x10336,0x1033c,0x10342},{0x10380,0x10387,0x1038e,0x10395},
    {0x103a0,0x103ac,0x103b8,0x103c8},{0x10450,0x1045c,0x10468,0x10474},{0x12000,0x12103,0x12206,0x12309},{0x10800,0x10810,0x1081d,0x1082a},
    {0x10840,0x10847,0x1084e,0x10855},{0x10900,0x10907,0x1090e,0x10915},{0x10920,0x10926,0x1092c,0x10932},{0x109a0,0x109b6,0x109d2,0x109e8},
    {0x10980,0x10988,0x10990,0x10998},{0x10a60,0x10a68,0x10a70,0x10a78},{0x10a80,0x10a88,0x10a90,0x10a98},{0x10b40,0x10b47,0x10b4e,0x10b55},
    {0x10b60,0x10b66,0x10b6c,0x10b72},{0x10b00,0x10b0f,0x10b1e,0x10b2d},{0x108e0,0x108e6,0x108ec,0x108f2},{0x10880,0x1088a,0x10894,0x1089e},
    {0x10860,0x10868,0x10870,0x10878},{0x10c80,0x10c9b,0x10cc3,0x10cde},{0x10400,0x10414,0x10428,0x1043c},{0x10480,0x1048a,0x10494,0x104a0},
    {0x104b0,0x104c2,0x104d8,0x104ea},{0x10000,0x10036,0x10090,0x100c4},{0x10600,0x10655,0x106aa,0x106ff},{0x12f90,0x12fa8,0x12fc0,0x12fd8},
    {0x10530,0x1053d,0x1054a,0x10557},{0x10500,0x1050a,0x10514,0x1051e},{0x10350,0x1035a,0x10364,0x1036e},{0x10570,0x10582,0x10595,0x105a8},
    {0x14400,0x14491,0x14522,0x145b3},{0x16a40,0x16a4a,0x16a54,0x16a5e},{0x11ac0,0x11ace,0x11adc,0x11aea},{0x1950,0x1958,0x1960,0x1968},
    {0x110d0,0x110d8,0x110e0,0x110e8},{0x2ea,0x3113,0x3123,0x31a5},{0x16fe1,0x1b1d2,0x1b235,0x1b298},{0x16a70,0x16a86,0x16a9c,0x16ab2},
    {0x118a0,0x118b5,0x118ca,0x118df},{0x1e800,0x1e835,0x1e86a,0x1e89f},{0x16ad0,0x16ad9,0x16ae2,0x16aeb},
};
// Base and mark pairs from the simple scripts that encode marks.
constexpr u32 simple_mark_samples[][2]{{0x2c80,0x2cef},{0x2c82,0x2cf1},{0x10350,0x10376},{0x10351,0x1037a},{0x1e800,0x1e8d0},{0x1e801,0x1e8d6},
    {0x16ad0,0x16af0},{0x16ad1,0x16af4},{0xa6a0,0xa6f0}};
bool historic_corpus(DWriteTextSystem system,bool samples) noexcept {
    CHECK(native_text_format_capture(&system));auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    const auto compare=[&](Utf8View text) noexcept {
        const float widths[]{40,120,1000000};const u32 flags[]{0,text_style_bold,text_style_italic};
        for(float width:widths)for(u32 flag:flags){TextStyleSpan style{0,text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
            shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
            CHECK(text_shape_utf8(&native,text,styles,width,1000000,{utf16,256},&native_output.text).ok());
            CHECK(text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text).ok());
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
    const u32 ranges[][2]{{0x13000,0x13430},{0x16a0,0x1700},{0x10c00,0x10c49}};
    for(const auto& range:ranges)for(u32 first=range[0];first<range[1];first+=8){char text[64];u32 bytes=0;
        for(u32 cp=first;cp<minimum(first+8,range[1]);++cp){bytes+=utf8_encode(cp,text+bytes);if(cp&1)text[bytes++]=' ';}
        if(!compare({text,bytes})){print("historic first scalar=");number(first);print("\n");return false;}}
    const Utf8View mixed[]{utf8_literal("AV 𓀀𓃀𓋹 ᚠᚢᚦ 😀"),utf8_literal("אבג (ᚠᚢᚦ) 𓀀 123"),utf8_literal("ᚠ\u0308 ᚢ\u0301\n𓀀𓃀"),utf8_literal("𓀀\u200d𓃀 ᚠ\u200cᚢ"),
        utf8_literal("pouty Shigi ᶻ 𝗓 𐰁"),utf8_literal("ab 𐰀𐰁𐰂 (12) cd"),utf8_literal("𐰀𐰁 ab 𐤀𐤁 cd")};
    for(const auto text:mixed)CHECK(compare(text));
    if(!samples)return true;
    for(const auto& sample:simple_script_samples){char text[64];u32 bytes=0,mixed_bytes=0;char context[64]{'a','b',' '};mixed_bytes=3;
        for(u32 i=0;i<4;++i){bytes+=utf8_encode(sample[i],text+bytes);if(i&1)text[bytes++]=' ';if(i<3)mixed_bytes+=utf8_encode(sample[i],context+mixed_bytes);}
        context[mixed_bytes++]=' ';context[mixed_bytes++]='c';context[mixed_bytes++]='.';
        if(!compare({text,bytes})||!compare({context,mixed_bytes})){print("simple script first scalar=");number(sample[0]);print("\n");return false;}}
    for(const auto& pair:simple_mark_samples){char text[64];u32 bytes=0;const u32 sequence[]{pair[0],pair[1],pair[0],' ',pair[1],' ',pair[0],pair[1],pair[1],'a',pair[1]};
        for(u32 cp:sequence)bytes+=utf8_encode(cp,text+bytes);
        if(!compare({text,bytes})){print("simple mark base=");number(pair[0]);print("\n");return false;}}
    return true;
}
// A display name ending in Old Turkic resolves to Segoe UI Historic, maps a
// real glyph and stays a right-to-left run after the left-to-right prefix.
bool old_turkic_name() noexcept {
    auto system=owner;CHECK(native_text_format_capture(&system));system.factory=nullptr;system.format=nullptr;
    const auto text=utf8_literal("pouty Shigi ᶻ 𝗓 𐰁");shaped_text_reset(&native_output.text);
    CHECK(text_shape_utf8(&system,text,{},1000000,1000000,{utf16,256},&native_output.text).ok()&&native_output.text.native_fonts());
    const auto& shaped=native_output.text;const u32 position=shaped.cluster_count-2;u32 found=0;
    for(u32 i=0;i<shaped.run_count;++i){const auto& run=shaped.runs[i];if(position<run.text_position||position>=run.text_position+run.text_length)continue;
        ++found;CHECK((run.bidi_level&1)&&run.glyph_count==1);IDWriteFontFace* face=nullptr;CHECK(native_run_reference(owner.factory,shaped,run,&face));
        IDWriteFontFace3* named=nullptr;IDWriteLocalizedStrings* names=nullptr;wchar_t family[64]{};u16 glyph=0;const u32 scalar=0x10c01;
        const bool ok=SUCCEEDED(face->QueryInterface(__uuidof(IDWriteFontFace3),reinterpret_cast<void**>(&named)))&&SUCCEEDED(named->GetFamilyNames(&names))&&
            SUCCEEDED(names->GetString(0,family,64))&&SUCCEEDED(face->GetGlyphIndicesW(&scalar,1,&glyph));
        com_release(names);com_release(named);com_release(face);
        CHECK(ok&&lstrcmpW(family,L"Segoe UI Historic")==0&&glyph&&shaped.glyphs[run.first_glyph].index==glyph&&shaped.glyphs[run.first_glyph].advance>0);
        CHECK(!native_empty_glyph(shaped,run.first_glyph));}
    CHECK(found==1);return true;
}
bool historic_cases() noexcept {
    CHECK(old_turkic_name());
    CHECK(historic_corpus(owner,true));
    const wchar_t* families[]{L"Segoe UI Historic",L"Consolas"};
    for(const auto* family:families){
        auto system=owner;system.format=nullptr;
        CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-US",&system.format)));
        const bool ok=historic_corpus(system,false);com_release(system.format);CHECK(ok);
    }return true;
}
