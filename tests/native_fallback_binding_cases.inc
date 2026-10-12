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
bool native_fallback_binding_benchmark(bool simulated=false,bool system_policy=false,bool hebrew=false,bool wrapped=false,bool independent=false,bool mixed=false,bool decorated=false,bool emoji=false) noexcept {
    IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(hebrew?L"Consolas":system_policy?L"Arial Rounded MT Bold":simulated?L"Segoe UI Historic":L"Georgia",nullptr,simulated?DWRITE_FONT_WEIGHT_SEMI_BOLD:DWRITE_FONT_WEIGHT_NORMAL,simulated?DWRITE_FONT_STYLE_ITALIC:DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-US",&format)));
    struct Release {IDWriteTextFormat*& p;~Release() noexcept{com_release(p);}} release{format};
    auto native=owner;native.format=format;native.em_size=16;CHECK(native_text_format_capture(&native));auto reference=native;reference.native_text=nullptr;native.factory=nullptr;native.format=nullptr;
    const Utf8View fallback_texts[]{utf8_literal("AV \xcb\x91 To office caf\xc3\xa9"),utf8_literal("AV \xcb\x91 AV\tTo\r\n\xcb\x91"),utf8_literal("AV \xcb\x91 \xce\xb1\xce\xb2 \xd0\x90\xd0\x91"),utf8_literal("AV \xcb\x91\xf0\x9f\x98\x80\xcb\x91 To")};
    const Utf8View simulation_texts[]{utf8_literal("AV To office"),utf8_literal("AV\tTo\r\nAV  "),utf8_literal("A\xcc\x81" "e\xcc\x81 To"),utf8_literal("AV \xf0\x9f\x98\x80 To")};
    const Utf8View system_texts[]{utf8_literal("AV \xce\xb1\xce\xb2 \xd0\x90\xd0\x91 To"),utf8_literal("AV\t\xce\xb1\r\n\xd0\x91 To  "),utf8_literal("A\xcc\x81 \xce\xb1\xcc\x88 \xd0\x91\xcc\x86 B"),utf8_literal("AV ffi \xce\xb1\xf0\x9f\x98\x80\xd0\x91 office To")};
    const Utf8View hebrew_texts[]{utf8_literal("\xd7\x90\xd7\x91 \xd7\x92\xd7\x93"),utf8_literal("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"),
        utf8_literal("\xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 \xd7\x95\xd6\xb9"),utf8_literal("\xd7\x90\xd6\xb0\xd6\xb0 \xd7\x91")};
    const Utf8View wrapped_texts[]{
        utf8_literal("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"),
        utf8_literal("\xd7\x90\xd7\x91 \xd7\x92\xd7\x93  \xd7\x94\xd7\x95   \xd7\x96\xd7\x97    "),
        utf8_literal("\xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 \xd7\x95\xd6\xb9 \xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 \xd7\x95\xd6\xb9"),
        utf8_literal("\xd7\x90\xd6\xb0\xd6\xb0 \xd7\x91 \xd7\x90\xd6\xb0\xd6\xb1\xd6\xb0 \xd7\x91")};
    const Utf8View mixed_texts[]{utf8_literal("abc \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d xyz"),utf8_literal("\xd7\x90 (abc) 123 \xd7\x91"),
        utf8_literal("A\xcc\x81 \xd7\x90\xd6\xb0\xd6\xb0 B\xcc\x88"),utf8_literal("abc\t\xd7\x90\r\n\xd7\x91 xyz  ")};
    const Utf8View emoji_texts[]{utf8_literal("abc \xd7\x90 \xf0\x9f\x98\x80 xyz"),utf8_literal("\xd7\x90 1\xef\xb8\x8f\xe2\x83\xa3\xf0\x9f\x98\x80 \xd7\x91"),
        utf8_literal("A \xd7\x90 \xf0\x9f\x91\xa9\xf0\x9f\x8f\xbd\xe2\x80\x8d\xf0\x9f\x92\xbb B"),utf8_literal("abc \xd7\x90 \xe2\x9d\xa4\xef\xb8\x8e \xd7\x91 xyz")};
    const float width=wrapped?37.f:mixed?1000.f:95.f;native.independent_letters=reference.independent_letters=independent;
    const auto* texts=emoji?emoji_texts:mixed?mixed_texts:wrapped?wrapped_texts:hebrew?hebrew_texts:system_policy?system_texts:simulated?simulation_texts:fallback_texts;
    TextStyleSpan paint[4][3]{};
    if(decorated)for(u32 i=0;i<4;++i){u32 offsets[64],count=0;
        for(u32 at=0;at<texts[i].bytes;){offsets[count++]=at;at+=utf8_decode_one(texts[i].data+at,texts[i].bytes-at).width;}offsets[count]=texts[i].bytes;
        const u32 first=offsets[count/3],last=offsets[2*count/3];
        paint[i][0]={0,first,text_style_bold};paint[i][1]={first,last-first,text_style_underline};paint[i][2]={last,texts[i].bytes-last,text_style_monospace};
    }
    const auto styles_at=[&](u32 i) noexcept {return ConstSlice<TextStyleSpan>{paint[i],decorated?3u:0u};};
    for(u32 i=0;i<4;++i){CHECK(text_shape_utf8_styled(&native,texts[i],styles_at(i),width,1000000,{utf16,256},&native_output.text).ok()&&native_output.text.native_fonts());
        CHECK(text_shape_utf8_styled(&reference,texts[i],styles_at(i),width,1000000,{utf16,256},&reference_output.text).ok()&&same(native_output,reference_output,!mixed));}
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    for(u32 mode=0;mode<2;++mode)for(u32 draw=0;draw<2;++draw){
        for(u32 batch=0;batch<9;++batch){u64 ticks[2]{},cycles[2]{};
            for(u32 order=0;order<2;++order){const u32 legacy=order^(batch&1);auto system=legacy?reference:native;
                // Warm the physical faces and the selected paragraph before
                // comparing repeated versus alternating text revisions.
                for(u32 i=0;i<4;++i)CHECK(text_measure_utf8_styled(&system,texts[i],styles_at(i),width,1000000,{utf16,256}).ok());
                CHECK(text_measure_utf8_styled(&system,texts[0],styles_at(0),width,1000000,{utf16,256}).ok());
                LARGE_INTEGER start{},end{};ULONG64 first=0,last=0;CHECK(QueryThreadCycleTime(GetCurrentThread(),&first)&&QueryPerformanceCounter(&start));
                for(u32 i=0;i<2048;++i){const u32 selected=mode?i%4:0;const auto text=texts[selected];const auto styles=styles_at(selected);
                    if(draw){CHECK(text_shape_utf8_styled(&system,text,styles,width,1000000,{utf16,256},&native_output.text).ok());if(!legacy)CHECK(native_output.text.native_fonts());}
                    else CHECK(text_measure_utf8_styled(&system,text,styles,width,1000000,{utf16,256}).ok());}
                CHECK(QueryPerformanceCounter(&end)&&QueryThreadCycleTime(GetCurrentThread(),&last));ticks[legacy]=static_cast<u64>(end.QuadPart-start.QuadPart);cycles[legacy]=last-first;
            }
            print("{\"mode\":");number(mode);print(",\"draw\":");number(draw);print(",\"batch\":");number(batch);print(",\"iterations\":2048,\"frequency\":");number(static_cast<u64>(frequency.QuadPart));
            print(",\"native_ticks\":");number(ticks[0]);print(",\"reference_ticks\":");number(ticks[1]);print(",\"native_cycles\":");number(cycles[0]);print(",\"reference_cycles\":");number(cycles[1]);print("}\n");
        }
    }return true;
}
