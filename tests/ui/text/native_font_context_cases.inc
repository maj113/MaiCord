// The same layout from 21 independent contexts: every context draws what the first one draws, and an output keeps
// its font runs valid after the context that produced it is released.
bool same_layout(const ShapedText& a,const ShapedText& b) noexcept {
    if(a.glyph_count!=b.glyph_count||a.run_count!=b.run_count||a.cluster_count!=b.cluster_count||a.decoration_count!=b.decoration_count||a.line_count!=b.line_count||
        !bytes_equal(&a.width,&b.width,12)||!bytes_equal(a.glyphs,b.glyphs,a.glyph_count*sizeof(TextGlyph))||!bytes_equal(a.clusters,b.clusters,a.cluster_count*2)||
        !bytes_equal(a.decorations,b.decorations,a.decoration_count*sizeof(TextDecoration)))return false;
    for(u32 i=0;i<a.run_count;++i){const auto& x=a.runs[i];const auto& y=b.runs[i];
        if(!bytes_equal(&x.baseline_x,&y.baseline_x,__builtin_offsetof(TextRun,bidi_level)-__builtin_offsetof(TextRun,baseline_x))||x.bidi_level!=y.bidi_level||x.sideways!=y.sideways||x.native_font!=y.native_font)return false;}
    return true;
}
bool native_font_context_layouts() noexcept {
    TextSystem contexts[21]{};
    struct Cleanup {TextSystem* contexts;~Cleanup() noexcept {shaped_text_reset(&native_output.text);shaped_text_reset(&second_output.text);for(u32 i=0;i<21;++i)text_system_release(contexts+i);}} cleanup{contexts};
    Output* outputs[]{&native_output,&second_output};for(auto* out:outputs){CHECK(shaped_text_initialize(&out->text,{out->glyphs,1024},{out->runs,256},{out->clusters,256},{out->decorations,256},{out->hits,4096}));}
    const auto shape=[](TextSystem& system,const FreshEngineCase& sample,Output& output) noexcept {
        const TextStyleSpan styles[]{{0,2,text_style_bold|text_style_underline},{3,6,text_style_italic|text_style_strikethrough}};
        system.tracking=sample.mode==3?.03f:0;
        const bool shaped=text_shape_utf8(&system, {sample.text,{styles,sample.mode==1?2u:0u},sample.mode==2?70.f:160.f,1000,
            sample.mode==2?TextWrapping::no_wrap:TextWrapping::wrap,sample.mode==2?TextOverflow::ellipsis:TextOverflow::clip}, &output.text).ok();
        if(shaped)observe(output.text,sample.text,system);return shaped&&output.text.native_fonts();};
    u32 comparisons=0;
    for(u32 i=0;i<21;++i){CHECK(text_system_initialize(contexts+i,L"Segoe UI",16).ok());++cases;
        CHECK(shape(contexts[i],fresh_engine_cases[0],native_output)&&shape(contexts[0],fresh_engine_cases[0],second_output)&&same_layout(native_output.text,second_output.text));++comparisons;
    }
    // The output of the released first context survives; the others still agree with it.
    CHECK(shape(contexts[0],fresh_engine_cases[0],second_output));text_system_release(contexts);
    CHECK(shape(contexts[1],fresh_engine_cases[0],native_output)&&same_layout(native_output.text,second_output.text));++comparisons;
    constexpr u32 cases_to_check[]{1,6,7,30,31,32,33,34,35,37,38};
    for(u32 index:cases_to_check){const auto& sample=fresh_engine_cases[index];CHECK(shape(contexts[1],sample,second_output));
        for(u32 i=2;i<21;++i){++cases;CHECK(shape(contexts[i],sample,native_output)&&same_layout(native_output.text,second_output.text));++comparisons;}}
    for(u32 i=1;i<21;++i)text_system_release(contexts+i);
    print("{\"kind\":\"font_context_layouts\",\"contexts\":21,\"exact_comparisons\":");number(comparisons);print(",\"publisher_output_survives\":true}\n");return true;
}


