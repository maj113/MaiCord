bool native_system_fallback_corpus(AppSystem system,bool private_font,bool retained_editor) noexcept {
    const Utf8View texts[]{utf8_literal("A\xce\xb1" "B"),utf8_literal("AV \xce\xb1\xce\xb2 \xd0\x90\xd0\x91 To"),
        utf8_literal("A\xcb\x91" "B"),utf8_literal("AV\t\xce\xb1\r\n\xd0\x91 To  "),
        utf8_literal("A\xcc\x81 \xce\xb1\xcc\x88 \xd0\x91\xcc\x86 B"),utf8_literal("A\xcc\x81\xce\xb1\xd0\x91"),
        utf8_literal("AV \xce\xb1\xf0\x9f\x98\x80\xd0\x91 To"),utf8_literal("AV ffi \xce\xb1\xf0\x9f\x98\x80\xd0\x91 office To")};
    const float widths[]{8,37,95,1000000};const u32 flags[]{0,text_style_link,text_style_underline,text_style_italic,text_style_bold,text_style_bold|text_style_italic,text_style_monospace};
    system.fixed_weight=private_font;
    TextEditLayoutOwner editor{};TextEditMetric selections[48];editor.selections=selections;editor.selection_capacity=48;
    struct Release {TextEditLayoutOwner& editor;~Release() noexcept{text_edit_layout_release(&editor);}} release{editor};
    for(u32 independent=0;independent<2;++independent){system.independent_letters=independent!=0;
        for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample)for(u32 flag:flags)for(float w:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            const auto text=texts[sample];const float width=w==1000000?w:w*(system.em_size/16);const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            const TextStyleSpan style{0,sample==5&&flag==text_style_link?1u:text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
            const auto diagnostic=[&]() noexcept {print("system fallback sample/flag/em/width/nowrap/independent=");number(sample);print("/");number(flag);print("/");real(system.em_size);print("/");real(width);print("/");number(nowrap);print("/");number(independent);print("\n");};
            ++cases;
            if(!layout(system,text,styles,width,wrap)){diagnostic();return false;}++native_cases;
            if(!compare_hits(system,text,width,wrap,styles)){diagnostic();return false;}
            if(!flag&&w==1000000&&!nowrap&&!independent)CHECK(draw_source(system,native_output.text));
            // App editors receive renderer->glyph_text.system, which is Segoe
            // UI with zero tracking and ordinary typography. Profile and
            // independent-letter settings belong to a message-drawing copy.
            if(!retained_editor||independent)continue;
            const TextEditLayoutInput input{text,text_edit_signature(text),0,native_output.text.cluster_count,0,0,width,40,1000000,static_cast<u8>(nowrap?0:text_edit_layout_wrap),styles};
            CHECK(text_edit_layout_measure(&editor,input,system)&&editor.geometry&&observe_edit(editor));
        }
    }return true;
}
