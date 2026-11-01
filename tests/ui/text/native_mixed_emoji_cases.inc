bool native_mixed_emoji_capacity() noexcept {
    char bytes[4096];wchar_t wide[4096];const auto atom=utf8_literal("A\xf0\x9f\x98\x80");
    for(u32 i=0;i<819;++i)copy_bytes(bytes+i*5,atom.data,5);bytes[4095]='B';const Utf8View text{bytes,4096};
    CHECK(native_text_candidate_style_capacity(text,{})==1639);
    auto system=owner;CHECK(native_text_format_capture(&system));system.factory=nullptr;system.format=nullptr;
    auto reference=owner;reference.native_text=nullptr;
    DWriteEditLayoutOwner layouts[2]{};TextEditMetric selections[2][48];
    for(u32 i=0;i<2;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
    struct Release {DWriteEditLayoutOwner* p;~Release() noexcept {text_edit_layout_release(p);text_edit_layout_release(p+1);}} release{layouts};
    const u32 carets[]{0,1228,2458};const u32 flags[]{0,text_style_underline|text_style_link};
    for(u32 flag:flags){const TextStyleSpan style{0,text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
        const auto a=text_measure_utf8(&system,text,styles,1000000,1000000,{wide,4096}),b=text_measure_utf8(&reference,text,styles,1000000,1000000,{wide,4096});
        CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==1&&a.utf16_units==2458);
        for(u32 caret:carets){++cases;const TextEditLayoutInput input{text,text_edit_signature(text),0,caret,0,0,1000000,40,1000000,0,styles};
            CHECK(text_edit_layout_measure(layouts,input,system)&&layouts[0].geometry);++native_cases;
            CHECK(text_edit_layout_measure(layouts+1,input,reference)&&same_edit(layouts[0],layouts[1]));}
    }
    return true;
}
bool native_mixed_emoji_corpus() noexcept {
    CHECK(native_mixed_emoji_capacity());
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas",L"Bahnschrift"};
    const Utf8View texts[]{utf8_literal("A\xf0\x9f\x98\x80" "B"),utf8_literal("hello \xf0\x9f\x98\x80 world"),
        utf8_literal("\xf0\x9f\x98\x80 hello"),utf8_literal("hello \xf0\x9f\x98\x80"),
        utf8_literal("hello \xf0\x9f\x98\x80 \xf0\x9f\x98\x83 world"),
        utf8_literal("a \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6 z"),
        utf8_literal("a 1\xef\xb8\x8f\xe2\x83\xa3 z"),utf8_literal("a \xe2\x9d\xa4\xef\xb8\x8f z"),
        utf8_literal("a\t\xf0\x9f\x98\x80\r\nb"),utf8_literal("\xf0\x9f\x98\x80 \xf0\x9f\x98\x83"),
        utf8_literal("12(\xf0\x9f\x98\x80)34"),utf8_literal("caf\xc3\xa9 \xf0\x9f\x98\x80 na\xc3\xafve"),
        utf8_literal("A\xf0\x9f\x8f\xbb" "B"),utf8_literal("A#\xe2\x83\xa3" "B"),utf8_literal("a1\xe2\x83\xa3z")};
    const float sizes[]{17.5f,14,1000};
    for(u32 family_index=0;family_index<4;++family_index)for(float em:sizes){const auto* family=families[family_index];
        // Every family/text runs at fractional em. Scaling needs one family
        // and the plain emoji, ZWJ family and keycap boundary examples.
        if(em!=17.5f&&family_index)continue;
        IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
        struct Release {IDWriteTextFormat*& p;~Release() noexcept {com_release(p);}} release{format};
        auto reference=owner;reference.native_text=nullptr;reference.format=format;reference.em_size=em;
        auto system=owner;system.factory=nullptr;system.format=nullptr;system.em_size=em;
        u16 name[32];u32 count=0;while(family[count]){name[count]=static_cast<u16>(family[count]);++count;}
        CHECK(native_text_format_create(system.native_text,{{name,count},0,{400,5,0},em,em*4,true},&system.native_format));
        if(em==17.5f)for(u32 i=0;i<sizeof(texts)/sizeof(texts[0]);++i){
            if(!emoji_dispatch_case(system,reference,texts[i],true,true,true)){print("mixed index/em/family_initial=");number(i);print("/");real(em);print("/");number(family[0]);print("\n");return false;}
            auto observed=system;observed.factory=owner.factory;observed.format=format;
            CHECK(emoji_dispatch_cuts(observed,reference,texts[i],true));}
        else {const u32 scale_samples[]{0,5,6};for(u32 i:scale_samples)CHECK(emoji_dispatch_case(system,reference,texts[i],false,true,true));}
    }
    return true;
}
