bool native_fallback_binding_corpus() noexcept {
    const Utf8View texts[]{utf8_literal("\xcb\x91"),utf8_literal("a\xcb\x91" "a"),utf8_literal("AV \xcb\x91 To"),
        utf8_literal("a\xcb\x91\xcc\x81" "a"),utf8_literal("\xcb\x91\tAV\r\n\xcb\x91 "),utf8_literal("\xcb\x91 AV \xcb\x91 AV \xcb\x91"),
        utf8_literal("A \xcb\x91 \xce\xb1 \xd0\x90"),utf8_literal("A\xcb\x91\xf0\x9f\x98\x80\xcb\x91" "B")};
    const float sizes[]{14,17.5f,32},widths[]{8,37,95,1000000};
    const u32 flags[]{0,text_style_link,text_style_underline,text_style_strikethrough,text_style_heading_3};
    DWriteEditLayoutOwner layouts[2]{};TextEditMetric selections[2][48];for(u32 i=0;i<2;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
    struct LayoutRelease {DWriteEditLayoutOwner* p;~LayoutRelease() noexcept{for(u32 i=0;i<2;++i)text_edit_layout_release(p+i);}} layout_release{layouts};
    for(u32 italic=0;italic<2;++italic)for(float em:sizes){IDWriteTextFormat* format=nullptr;
        CHECK(SUCCEEDED(owner.factory->CreateTextFormat(L"Georgia",nullptr,DWRITE_FONT_WEIGHT_NORMAL,italic?DWRITE_FONT_STYLE_ITALIC:DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
        struct Release {IDWriteTextFormat*& p;~Release() noexcept{com_release(p);}} release{format};
        auto system=owner;system.format=format;system.em_size=em;CHECK(native_text_format_capture(&system));
        auto reference=system;reference.native_text=nullptr;system.factory=nullptr;system.format=nullptr;
        for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample)for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            const auto text=texts[sample];const TextStyleSpan style{0,sample==3?3u:text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
            const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;++cases;
            if(!text_shape_utf8_styled(&system,text,styles,width,1000000,{utf16,256},&native_output.text,wrap).ok()||!native_output.text.native_fonts()){
                print("fallback refused sample/flag/em/width/nowrap=");number(sample);print("/");number(flag);print("/");real(em);print("/");real(width);print("/");number(nowrap);print("\n");return false;}
            ++native_cases;
            CHECK(text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrap).ok());
            if(!same(native_output,reference_output,true)){print("fallback mismatch sample/flag/em/width/nowrap=");number(sample);print("/");number(flag);print("/");real(em);print("/");real(width);print("/");number(nowrap);print("\n");
                const ShapedText* results[]{&native_output.text,&reference_output.text};for(u32 side=0;side<2;++side)for(u32 r=0;r<results[side]->run_count;++r){const auto& run=results[side]->runs[r];IDWriteFontFace* face=nullptr;
                    CHECK(native_run_reference(owner.factory,*results[side],run,&face));IDWriteFontFace3* named=nullptr;CHECK(SUCCEEDED(face->QueryInterface(__uuidof(IDWriteFontFace3),reinterpret_cast<void**>(&named))));
                    print(side?"reference run=":"native run=");number(run.text_position);print("/");number(run.text_length);print(" weight=");number(named->GetWeight());print("\n");com_release(named);com_release(face);}
                return false;}
            const auto a=text_measure_utf8_styled(&system,text,styles,width,1000000,{utf16,256},wrap),b=text_measure_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},wrap);
            CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count);
            auto hit_system=system;hit_system.factory=owner.factory;hit_system.format=format;CHECK(compare_hits(hit_system,text,width,wrap,styles));
            if(!flag&&width==1000000&&!nowrap)for(u32 r=0;r<native_output.text.run_count;++r){auto draw=native_output.text;draw.runs+=r;draw.run_count=1;CHECK(draw_source(hit_system,draw));}
            const TextEditLayoutInput input{text,text_edit_signature(text),0,native_output.text.cluster_count,0,0,width,40,1000000,static_cast<u8>(nowrap?0:text_edit_layout_wrap),styles};
            CHECK(text_edit_layout_measure(layouts,input,system)&&layouts[0].geometry);
            CHECK(text_edit_layout_measure(layouts+1,input,reference)&&same_edit(layouts[0],layouts[1]));
        }
    }return true;
}
