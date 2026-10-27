bool benchmark_smoke=false;
// Odia samples unless telugu, kannada or arabic is requested.
bool styled_paragraph_benchmark(bool telugu,bool kannada,bool arabic) noexcept {
    // The suite smoke run keeps every correctness check, mode and both
    // native/legacy orders; the benchmark tool requests all ten batches.
    const u32 batches=benchmark_smoke?2u:10u;
    CHECK(native_text_format_capture(&owner));
    const Utf8View odia_samples[]{utf8_literal("AV office cafe AV\nଓଡ଼ିଆ ଭାଷା କି କୋ କୌ ର୍କି କ୍ଷେତ୍ର ଶ୍ରଦ୍ଧା 😀"),
        utf8_literal("AV office cafe To\nଓଡ଼ିଆ ଭାଷା କୀ କୁ କୂ ନମସ୍କାର କ୍‍ଷ କ୍‌ଷ 😀")};
    const Utf8View telugu_samples[]{utf8_literal("AV office cafe AV\nతెలుగు భాష కి కై క్రై క్ర్రై క్షేత్రం శ్రద్ధ 😀"),
        utf8_literal("AV office cafe To\nతెలుగు భాష కీ కు కూ నమస్కారం క్‍ష క్‌ష 😀")};
    const Utf8View kannada_samples[]{utf8_literal("AV office cafe AV\nಕನ್ನಡ ಭಾಷೆ ಕಿ ಕೈ ರ್ಕಿ ರ್ಕೋ ಕ್ಷೇತ್ರ ಶ್ರದ್ಧೆ 😀"),
        utf8_literal("AV office cafe To\nಕನ್ನಡ ಭಾಷೆ ಕೀ ಕು ಕೂ ನಮಸ್ಕಾರ ಕ್‍ಷ ಕ್‌ಷ 😀")};
    const Utf8View arabic_samples[]{utf8_literal("AV office cafe AV\nمرحبا بالجميع في القناة العربية 😀"),
        utf8_literal("AV office cafe To\nالقناة ترحب بالجميع اليوم 😀")};
    const auto* samples=arabic?arabic_samples:kannada?kannada_samples:telugu?telugu_samples:odia_samples;
    const TextStyleSpan spans[2][2]{{{3,6,text_style_bold|text_style_heading_2},{10,5,text_style_italic}},{{3,6,text_style_italic|text_style_heading_3},{10,5,text_style_bold}}};
    LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    auto reference=owner;reference.native_text=nullptr;
    for(u32 sample=0;sample<2;++sample)for(u32 style=0;style<2;++style){shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
        CHECK(native_text_candidate_styled(owner,samples[sample],{spans[style],2},120,1000,&native_output.text,nullptr,TextWrapping::wrap));
        CHECK(text_shape_utf8_styled(&reference,samples[sample],{spans[style],2},120,1000,{utf16,256},&reference_output.text).ok()&&same(native_output,reference_output));}
    for(u32 mode=0;mode<4;++mode){u64 cycles[10][2]{};print(arabic?"{\"kind\":\"arabic_paragraph_benchmark\",\"mode\":" : kannada?"{\"kind\":\"kannada_paragraph_benchmark\",\"mode\":" : telugu?"{\"kind\":\"telugu_paragraph_benchmark\",\"mode\":" : "{\"kind\":\"odia_paragraph_benchmark\",\"mode\":");number(mode);print(",\"frequency\":");number(frequency.QuadPart);print(",\"iterations\":128,\"batches\":[");
        for(u32 batch=0;batch<batches;++batch){u64 ticks[2]{};
            for(u32 order=0;order<2;++order){const u32 legacy=order^(batch&1);auto system=owner;if(legacy)system.native_text=nullptr;else{system.factory=nullptr;system.format=nullptr;}auto* out=legacy?&reference_output.text:&native_output.text;
                CHECK(text_shape_utf8_styled(&system,samples[0],{spans[0],2},120,1000,{utf16,256},out).ok());const auto before=native_text_candidate_counts(owner);LARGE_INTEGER start{},end{};ULONG64 cycle_start=0,cycle_end=0;CHECK(QueryThreadCycleTime(GetCurrentThread(),&cycle_start));CHECK(QueryPerformanceCounter(&start));
                for(u32 i=0;i<128;++i){const float width=mode==3?120.f+static_cast<float>(i%32):120.f;
                    CHECK(text_shape_utf8_styled(&system,samples[mode==1?(i&1):0],{spans[mode==2?(i&1):0],2},width,1000,{utf16,256},out).ok());}
                CHECK(QueryPerformanceCounter(&end));CHECK(QueryThreadCycleTime(GetCurrentThread(),&cycle_end)&&cycle_end>=cycle_start);cycles[batch][legacy]=cycle_end-cycle_start;ticks[legacy]=end.QuadPart-start.QuadPart;const auto after=native_text_candidate_counts(owner);
                CHECK(after.paragraphs_shaped-before.paragraphs_shaped==(legacy?0u:128u));
            }
            if(batch)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print("]");
        }print("],\"cycle_batches\":[");for(u32 batch=0;batch<batches;++batch){if(batch)print(",");print("[");number(cycles[batch][0]);print(",");number(cycles[batch][1]);print("]");}print("]}\n");
    }return true;
}
