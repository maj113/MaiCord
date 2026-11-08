bool native_fallback_binding_corpus() noexcept {
    const Utf8View texts[]{utf8_literal("\xcb\x91"),utf8_literal("a\xcb\x91" "a"),utf8_literal("AV \xcb\x91 To"),
        utf8_literal("a\xcb\x91\xcc\x81" "a"),utf8_literal("\xcb\x91\tAV\r\n\xcb\x91 "),utf8_literal("\xcb\x91 AV \xcb\x91 AV \xcb\x91"),
        utf8_literal("A \xcb\x91 \xce\xb1 \xd0\x90"),utf8_literal("A\xcb\x91\xf0\x9f\x98\x80\xcb\x91" "B")};
    const float sizes[]{14,17.5f,32},widths[]{8,37,95,1000000};
    const u32 flags[]{0,text_style_link,text_style_underline,text_style_strikethrough,text_style_heading_3};
    TextEditLayoutOwner editor{};TextEditMetric selections[48];editor.selections=selections;editor.selection_capacity=48;
    struct LayoutRelease {TextEditLayoutOwner& p;~LayoutRelease() noexcept{text_edit_layout_release(&p);}} layout_release{editor};
    for(u32 italic=0;italic<2;++italic)for(float em:sizes){
        auto system=owner;system.em_size=em;CHECK(set_format(system,L"Georgia",em,400,italic?2:0));
        for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample)for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            const auto text=texts[sample];const TextStyleSpan style{0,sample==3?3u:text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
            const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;++cases;
            if(!layout(system,text,styles,width,wrap)){
                print("fallback refused sample/flag/em/width/nowrap=");number(sample);print("/");number(flag);print("/");real(em);print("/");real(width);print("/");number(nowrap);print("\n");return false;}
            ++native_cases;
            CHECK(compare_hits(system,text,width,wrap,styles));
            if(!flag&&width==1000000&&!nowrap)for(u32 r=0;r<native_output.text.run_count;++r){auto draw=native_output.text;draw.runs+=r;draw.run_count=1;CHECK(draw_source(system,draw));}
            const TextEditLayoutInput input{text,text_edit_signature(text),0,native_output.text.cluster_count,0,0,width,40,1000000,static_cast<u8>(nowrap?0:text_edit_layout_wrap),styles};
            CHECK(text_edit_layout_measure(&editor,input,system)&&editor.geometry&&observe_edit(editor));
        }
    }return true;
}
