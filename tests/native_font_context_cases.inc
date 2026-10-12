bool native_font_context_layouts() noexcept {
    DWriteTextSystem contexts[21]{};
    struct Cleanup {DWriteTextSystem* contexts;~Cleanup() noexcept {shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);for(u32 i=0;i<21;++i)text_system_release(contexts+i);oracle_faces_release();}} cleanup{contexts};
    Output* outputs[]{&native_output,&reference_output};for(auto* out:outputs){CHECK(shaped_text_initialize(&out->text,{out->glyphs,1024},{out->runs,256},{out->clusters,256}));CHECK(shaped_text_bind_decorations(&out->text,{out->decorations,256}));CHECK(shaped_text_bind_hit_regions(&out->text,{out->hits,4096}));}
    CHECK(SUCCEEDED(text_compatibility_objects_initialize(&owner,L"Segoe UI",16,L"en-US")));owner.em_size=16;
    const auto shape=[](DWriteTextSystem& system,const FreshEngineCase& sample,Output& output) noexcept {
        const TextStyleSpan styles[]{{0,2,text_style_bold|text_style_underline},{3,6,text_style_italic|text_style_strikethrough}};
        system.tracking=sample.mode==3?.03f:0;
        return text_shape_utf8_styled(&system,sample.text,{styles,sample.mode==1?2u:0u},sample.mode==2?70.f:160.f,1000,{utf16,256},&output.text,
            sample.mode==2?TextWrapping::no_wrap:TextWrapping::wrap,sample.mode==2?TextOverflow::ellipsis:TextOverflow::clip).ok();};
    u32 comparisons=0;
    for(u32 i=0;i<21;++i){CHECK(text_system_initialize(static_cast<TextSystem*>(contexts+i),L"Segoe UI",16,L"en-US").ok());
        CHECK(shape(contexts[i],fresh_engine_cases[0],native_output)&&shape(owner,fresh_engine_cases[0],reference_output)&&same(native_output,reference_output,true));++comparisons;
    }
    // A retained shaped output must keep its font runs valid after the context
    // that produced it is released; remaining contexts keep independent lookup records.
    CHECK(shape(contexts[0],fresh_engine_cases[0],native_output));text_system_release(contexts);
    CHECK(shape(owner,fresh_engine_cases[0],reference_output)&&same(native_output,reference_output,true));++comparisons;
    constexpr u32 cases_to_check[]{1,6,7,30,31,32,33,34,35,37,38};
    for(u32 i=1;i<21;++i){for(u32 index:cases_to_check){const auto& sample=fresh_engine_cases[index];
        CHECK(shape(contexts[i],sample,native_output)&&shape(owner,sample,reference_output)&&same(native_output,reference_output,true));++comparisons;}
        text_system_release(contexts+i);}
    print("{\"kind\":\"font_context_layouts\",\"contexts\":21,\"exact_comparisons\":");number(comparisons);print(",\"publisher_output_survives\":true}\n");return true;
}


