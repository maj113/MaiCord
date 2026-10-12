bool directional_semantic_benchmark() noexcept {
    CHECK(native_text_format_capture(&owner));
    const Utf8View texts[]{utf8_literal("A \xd7\x90 abc \xe2\x9d\xa4\xef\xb8\x8f \xd7\x91 Z\nA \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d more words."),utf8_literal("B \xd7\x90 xyz \xf0\x9f\x98\x80 \xd7\x91 A\nB \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d other words.")};
    TextStyleSpan styles[2][2][2]{};TextHitSpan hits[2][2][3]{};
    for(u32 t=0;t<2;++t){
        styles[t][0][0]={0,2,text_style_link};styles[t][0][1]={2,texts[t].bytes-2,text_style_link|text_style_underline};
        styles[t][1][0]={0,2,text_style_bold};styles[t][1][1]={2,texts[t].bytes-2,text_style_link|text_style_italic};
        hits[t][0][0]={0,texts[t].bytes,0,0};hits[t][0][1]={2,texts[t].bytes-2,1,0};hits[t][0][2]={0,2,2,0};
        hits[t][1][0]={2,2,2,0};hits[t][1][1]={0,2,0,0};hits[t][1][2]={texts[t].bytes-6,5,1,0};
        for(u32 s=0;s<2;++s)for(u32 h=0;h<2;++h)CHECK(semantic_compare(owner,texts[t],{styles[t][s],2},{hits[t][h],3},true,120,TextWrapping::wrap,true));
    }
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    for(u32 mode=0;mode<5;++mode){print("{\"kind\":\"directional_semantic_benchmark\",\"mode\":");number(mode);print(",\"frequency\":");number(frequency.QuadPart);print(",\"iterations\":128,\"batches\":[");
        for(u32 batch=0;batch<10;++batch){u64 ticks[2]{};
            for(u32 order=0;order<2;++order){const u32 legacy=order^(batch&1);auto system=owner;if(legacy)system.native_text=nullptr;else{system.factory=nullptr;system.format=nullptr;}auto* output=legacy?&reference_output.text:&native_output.text;
                CHECK(text_shape_utf8_interactive(&system,texts[0],{styles[0][0],2},{hits[0][0],3},120,1000,{utf16,256},output).ok());
                const auto before=native_text_candidate_counts(owner);LARGE_INTEGER start{},end{};CHECK(QueryPerformanceCounter(&start));
                for(u32 i=0;i<128;++i){const u32 t=mode==1?(i&1):0,s=mode==2?(i&1):0,h=mode==4?(i&1):0;
                    CHECK(text_shape_utf8_interactive(&system,texts[t],{styles[t][s],2},{hits[t][h],3},mode==3?120.f+static_cast<float>(i%32):120.f,1000,{utf16,256},output).ok());}
                CHECK(QueryPerformanceCounter(&end));ticks[legacy]=end.QuadPart-start.QuadPart;const auto after=native_text_candidate_counts(owner);CHECK(after.paragraphs_shaped-before.paragraphs_shaped==(legacy?0u:128u));
            }
            if(batch)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print("]");
        }print("]}\n");
    }return true;
}
bool semantic_benchmark() noexcept {
    CHECK(native_text_format_capture(&owner));
    const Utf8View texts[]{utf8_literal("AV office caf\xc3\xa9 AV\nA styled paragraph with several ordinary words and tabs\there."),utf8_literal("AV office caf\xc3\xa9 To\nAnother styled paragraph changes the text and wraps\there.")};
    const TextStyleSpan styles[2][2]{{{3,6,text_style_bold|text_style_heading_2|text_style_link},{10,5,text_style_italic|text_style_underline}},{{3,6,text_style_italic|text_style_heading_3|text_style_link},{10,5,text_style_bold|text_style_underline}}};
    const TextHitSpan hit_sets[2][5]{{{3,6,0,0},{0,2,1,0},{10,5,2,0},{16,20,3,0},{30,30,4,0}},{{30,30,4,0},{10,5,2,0},{0,15,1,0},{17,17,3,0},{3,6,0,0}}};
    for(u32 text=0;text<2;++text)for(u32 style=0;style<2;++style)for(u32 hit=0;hit<2;++hit)CHECK(semantic_compare(owner,texts[text],{styles[style],2},{hit_sets[hit],5},true,120,TextWrapping::wrap));
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    for(u32 mode=0;mode<5;++mode){print("{\"kind\":\"semantic_benchmark\",\"mode\":");number(mode);print(",\"frequency\":");number(frequency.QuadPart);print(",\"iterations\":128,\"batches\":[");
        for(u32 batch=0;batch<10;++batch){u64 ticks[2]{};
            for(u32 order=0;order<2;++order){const u32 legacy=order^(batch&1);auto system=owner;if(legacy)system.native_text=nullptr;auto* output=legacy?&reference_output.text:&native_output.text;
                CHECK(text_shape_utf8_interactive(&system,texts[0],{styles[0],2},{hit_sets[0],5},120,1000,{utf16,256},output).ok());
                const auto before=native_text_candidate_counts(owner);LARGE_INTEGER start{},end{};CHECK(QueryPerformanceCounter(&start));
                for(u32 i=0;i<128;++i)CHECK(text_shape_utf8_interactive(&system,texts[mode==1?(i&1):0],{styles[mode==2?(i&1):0],2},{hit_sets[mode==4?(i&1):0],5},mode==3?120.f+static_cast<float>(i%32):120.f,1000,{utf16,256},output).ok());
                CHECK(QueryPerformanceCounter(&end));ticks[legacy]=end.QuadPart-start.QuadPart;const auto after=native_text_candidate_counts(owner);CHECK(after.paragraphs_shaped-before.paragraphs_shaped==(legacy?0u:128u));
            }
            if(batch)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print("]");
        }print("]}\n");
    }return true;
}
