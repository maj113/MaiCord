bool native_system_fallback_corpus(DWriteTextSystem system,bool private_font,bool retained_editor) noexcept {
    const Utf8View texts[]{utf8_literal("A\xce\xb1" "B"),utf8_literal("AV \xce\xb1\xce\xb2 \xd0\x90\xd0\x91 To"),
        utf8_literal("A\xcb\x91" "B"),utf8_literal("AV\t\xce\xb1\r\n\xd0\x91 To  "),
        utf8_literal("A\xcc\x81 \xce\xb1\xcc\x88 \xd0\x91\xcc\x86 B"),utf8_literal("A\xcc\x81\xce\xb1\xd0\x91"),
        utf8_literal("AV \xce\xb1\xf0\x9f\x98\x80\xd0\x91 To"),utf8_literal("AV ffi \xce\xb1\xf0\x9f\x98\x80\xd0\x91 office To")};
    const float widths[]{8,37,95,1000000};const u32 flags[]{0,text_style_link,text_style_underline,text_style_italic,text_style_bold,text_style_bold|text_style_italic,text_style_monospace};
    system.fixed_weight=private_font;auto native=system;native.factory=nullptr;native.format=nullptr;auto reference=system;reference.native_text=nullptr;
    DWriteEditLayoutOwner layouts[2]{};TextEditMetric selections[2][48];for(u32 i=0;i<2;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
    struct Release {DWriteEditLayoutOwner* layouts;~Release() noexcept{for(u32 i=0;i<2;++i)text_edit_layout_release(layouts+i);}} release{layouts};
    for(u32 independent=0;independent<2;++independent){native.independent_letters=reference.independent_letters=system.independent_letters=independent!=0;
        for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample)for(u32 flag:flags)for(float w:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            const auto text=texts[sample];const float width=w==1000000?w:w*(system.em_size/16);const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            const TextStyleSpan style{0,sample==5&&flag==text_style_link?1u:text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
            const auto diagnostic=[&]() noexcept {print("system fallback sample/flag/em/width/nowrap/independent=");number(sample);print("/");number(flag);print("/");real(system.em_size);print("/");real(width);print("/");number(nowrap);print("/");number(independent);print("\n");};
            ++cases;
            if(!text_shape_utf8(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrap).ok()||!native_output.text.native_fonts()){diagnostic();CHECK(false);}++native_cases;
            CHECK(text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrap).ok());
            if(!same(native_output,reference_output,true)){diagnostic();CHECK(inspect_style_layout(system,text,styles,width,wrap));
                const ShapedText* values[]{&native_output.text,&reference_output.text};for(u32 side=0;side<2;++side){print(side?"reference partitions=":"native partitions=");for(u32 r=0;r<values[side]->run_count;++r){const auto& run=values[side]->runs[r];print("[");number(run.first_glyph);print(",");number(run.glyph_count);print(",");real(run.baseline_x);print("] ");}print("\n");}return false;}
            const auto a=text_measure_utf8(&native,text,styles,width,1000000,{utf16,256},wrap),b=text_measure_utf8(&reference,text,styles,width,1000000,{utf16,256},wrap);
            CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count);
            if(!compare_hits(system,text,width,wrap,styles)){diagnostic();return false;}
            if(!flag&&w==1000000&&!nowrap&&!independent)CHECK(draw_source(system,native_output.text));
            // App editors receive renderer->glyph_text.system, which is Segoe
            // UI with zero tracking and ordinary typography. Profile and
            // independent-letter settings belong to a message-drawing copy.
            if(!retained_editor||independent)continue;
            const TextEditLayoutInput input{text,text_edit_signature(text),0,native_output.text.cluster_count,0,0,width,40,1000000,static_cast<u8>(nowrap?0:text_edit_layout_wrap),styles};
            CHECK(text_edit_layout_measure(layouts,input,native)&&layouts[0].geometry);
            if(!text_edit_layout_measure(layouts+1,input,reference)||!same_edit(layouts[0],layouts[1])){diagnostic();for(u32 side=0;side<2;++side){const auto& e=layouts[side];print(side?"reference caret/height=":"native caret/height=");real(e.caret.left);print("/");real(e.caret.top);print("/");real(e.caret.width);print("/");real(e.caret.height);print("/");real(e.layout_height);print("\n");}
                for(u32 j=0;j<layouts[0].selection_count;++j)if(!bytes_equal(layouts[0].selections+j,layouts[1].selections+j,sizeof(TextEditMetric))){print("selection difference=");number(j);print("\n");for(u32 side=0;side<2;++side){u32 words[sizeof(TextEditMetric)/4];copy_bytes(words,layouts[side].selections+j,sizeof(words));for(u32 word:words){number(word);print(" ");}print("\n");}break;}
                for(u32 pos=0;pos<layouts[0].utf16_units;++pos){TextEditMetric metric_a{},metric_b{};CHECK(text_edit_layout_position_geometry(layouts[0],pos,&metric_a)&&text_edit_layout_position_geometry(layouts[1],pos,&metric_b));print("position/widths=");number(pos);print("/");real(metric_a.left);print("/");real(metric_b.left);print("\n");}CHECK(false);}
        }
    }return true;
}
