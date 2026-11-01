bool tracking_compare() noexcept {
    if(same(native_output,reference_output))return true;
    print("{\"tracking_placements\":[");const Output* outputs[]{&native_output,&reference_output};
    for(u32 o=0;o<2;++o){if(o)print(",");print("{\"glyphs\":[");const auto& t=outputs[o]->text;
        for(u32 i=0;i<t.glyph_count;++i){if(i)print(",");const auto& g=t.glyphs[i];print("[");number(g.index);print(",");real(g.advance);print(",");real(g.advance_offset);print(",");real(g.ascender_offset);print("]");}
        print("],\"runs\":[");for(u32 i=0;i<t.run_count;++i){if(i)print(",");const auto& r=t.runs[i];print("[");number(r.first_glyph);print(",");number(r.glyph_count);print(",");real(r.em_size);print(",");real(r.baseline_x);print("]");}print("]}");}
    print("]}\n");return false;
}
bool native_tracking_corpus(DWriteTextSystem system) noexcept {
    const Utf8View texts[]{utf8_literal("AV To office"),utf8_literal("A  office  "),utf8_literal("AV\tTo\r\nAV"),
        utf8_literal("A\n\n"),utf8_literal("A\t\t "),utf8_literal("Ax\xcc\x81\xcc\xa3 To"),
        utf8_literal("Ae\xcc\x81V"),utf8_literal("Aa\xe2\x80\x8d\xcc\x81 To")};
    const float widths[]{8,95,1000000},tracking[]{0,.01f,.04f,.02f,.03f,0};
    for(u32 independent=0;independent<2;++independent)for(const auto text:texts)for(u32 style=0;style<6;++style)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
        const u32 flags[]{0,text_style_underline,text_style_heading_1,text_style_heading_1,text_style_link,text_style_strikethrough|text_style_subtext};
        const u32 first=style==3?1u:0u;const TextStyleSpan span{first,text.bytes-first,flags[style]};const ConstSlice<TextStyleSpan> styles{&span,style?1u:0u};
        const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;system.independent_letters=independent!=0;
        bool ascii=true;for(u32 i=0;i<text.bytes;++i)ascii=ascii&&static_cast<u8>(text.data[i])<128;
        bool baseline_native=false;u32 iteration=0;
        // Tracking changes while format, text and style identities stay fixed.
        // Some faces cannot cover these marks at zero tracking. Keep those
        // native-only declines explicit; spacing must preserve all acceptances.
        for(float amount:tracking){system.tracking=amount;auto reference=system;reference.native_text=nullptr;
            auto native=system;native.factory=nullptr;native.format=nullptr;++cases;
            const auto a=text_shape_utf8(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrapping);
            if(!iteration++)baseline_native=a.ok();
            if(!a.ok()){CHECK(!ascii&&!baseline_native&&a.error==TextShapeError::unsupported&&!native_output.text.native_fonts());continue;}
            CHECK(a.ok()&&native_output.text.native_fonts());++native_cases;
            CHECK(text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrapping).ok());CHECK(tracking_compare());
            const auto am=text_measure_utf8(&native,text,styles,width,1000000,{utf16,256},wrapping),bm=text_measure_utf8(&reference,text,styles,width,1000000,{utf16,256},wrapping);
            CHECK(am.ok()&&bm.ok()&&bytes_equal(&am.width,&bm.width,12)&&am.line_count==bm.line_count);
            const TextHitSpan hit{0,text.bytes,65535,0};
            CHECK(text_shape_utf8(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrapping,TextOverflow::clip,{&hit,1}).ok()&&native_output.text.native_fonts());
            CHECK(text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrapping,TextOverflow::clip,{&hit,1}).ok());CHECK(same(native_output,reference_output));
        }
    }
    // Exercise line reuse and line-to-paragraph promotion without an intervening
    // interactive call changing the retained layout kind.
    const float promotion_widths[]{1000000,95,8,1000000};
    for(u32 independent=0;independent<2;++independent)for(float width:promotion_widths)for(float amount:tracking){
        system.independent_letters=independent!=0;system.tracking=amount;
        CHECK(compare(system,utf8_literal("AV To office AV To"),true,width));
    }
    // Probe adjacent binary32 widths around both a word-prefix and full-line
    // threshold. Spacing must use the same accumulation for fitting and metrics.
    const auto threshold_text=utf8_literal("AV To office AV");const float amounts[]{.01f,.04f};
    for(u32 independent=0;independent<2;++independent)for(float amount:amounts)for(u32 prefix=0;prefix<2;++prefix){
        system.independent_letters=independent!=0;system.tracking=amount;auto reference=system;reference.native_text=nullptr;
        const Utf8View measured_text{threshold_text.data,prefix?5u:threshold_text.bytes};const TextStyleSpan measure_style{1,measured_text.bytes-1,text_style_heading_1};
        const auto measured=text_measure_utf8(&reference,measured_text,{&measure_style,1},1000000,1000000,{utf16,256},TextWrapping::no_wrap);
        u32 width_bits=0;copy_bytes(&width_bits,&measured.width,4);CHECK(measured.ok()&&width_bits>1&&width_bits<0x7f7fffffu);
        const TextStyleSpan style{1,threshold_text.bytes-1,text_style_heading_1};
        for(i32 delta=-1;delta<=1;++delta){const u32 bits=static_cast<u32>(static_cast<i64>(width_bits)+delta);float width=0;copy_bytes(&width,&bits,4);
            auto native=system;native.factory=nullptr;native.format=nullptr;++cases;
            CHECK(text_shape_utf8(&native,threshold_text,{&style,1},width,1000000,{utf16,256},&native_output.text).ok()&&native_output.text.native_fonts());++native_cases;
            CHECK(text_shape_utf8(&reference,threshold_text,{&style,1},width,1000000,{utf16,256},&reference_output.text).ok());CHECK(tracking_compare());
        }
    }
    return true;
}
