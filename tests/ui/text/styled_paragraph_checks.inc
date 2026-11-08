// Odia samples unless telugu, kannada or arabic is requested.
bool styled_paragraph_checks(bool telugu,bool kannada,bool arabic) noexcept {
    // Every mode, two batches of 128 shapes each.
    constexpr u32 batches=2;
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
    for(u32 sample=0;sample<2;++sample)for(u32 style=0;style<2;++style){shaped_text_reset(&native_output.text);++cases;
        CHECK(native_text_candidate_styled(owner,{samples[sample],{spans[style],2},120,1000,TextWrapping::wrap},&native_output.text,nullptr));
        observe(native_output.text,samples[sample],owner);
        CHECK(shape_observed(owner,samples[sample],{spans[style],2},120,TextWrapping::wrap,1000));++native_cases;}
    for(u32 mode=0;mode<4;++mode){
        for(u32 batch=0;batch<batches;++batch){auto system=owner;
            CHECK(text_shape_utf8(&system, {samples[0],{spans[0],2},120,1000}, &native_output.text).ok());const auto before=native_text_candidate_counts(owner);
            for(u32 i=0;i<128;++i){const float width=mode==3?120.f+static_cast<float>(i%32):120.f;
                CHECK(text_shape_utf8(&system, {samples[mode==1?(i&1):0],{spans[mode==2?(i&1):0],2},width,1000}, &native_output.text).ok());}
            const auto after=native_text_candidate_counts(owner);
            CHECK(after.paragraphs_shaped-before.paragraphs_shaped==128u);
        }
    }return true;
}
