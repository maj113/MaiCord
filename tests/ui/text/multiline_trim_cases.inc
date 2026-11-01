u32 multiline_control_origin_differences=0;
bool same_multiline_controls(const Output& native,const Output& reference) noexcept {
    u32 a=0,b=0;
    for(;;){while(a<native.text.run_count&&native.text.runs[a].glyph_count)++a;while(b<reference.text.run_count&&reference.text.runs[b].glyph_count)++b;
        if(a==native.text.run_count||b==reference.text.run_count){CHECK(a==native.text.run_count&&b==reference.text.run_count);return true;}
        const auto& x=native.text.runs[a++];const auto& y=reference.text.runs[b++];
        if(x.text_position!=y.text_position||x.text_length!=y.text_length||x.em_size!=y.em_size||x.style_flags!=y.style_flags||x.bidi_level!=y.bidi_level){
            print("control native/reference source/count/x/y/level=");number(x.text_position);print("/");number(y.text_position);print("/");number(x.text_length);print("/");number(y.text_length);print("/");real(x.baseline_x);print("/");real(y.baseline_x);print("/");real(x.baseline_y);print("/");real(y.baseline_y);print("/");number(x.bidi_level);print("/");number(y.bidi_level);print("\n");return false;}
        multiline_control_origin_differences+=(x.baseline_x!=y.baseline_x||x.baseline_y!=y.baseline_y);
        CHECK(same_native_face(native.text,x,reference_run_face(y)));
    }
}
bool compare_multiline_plan(DWriteTextSystem system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,float height,TextWrapping wrapping) noexcept {
    auto native=system;native.factory=nullptr;native.format=nullptr;
    NativeParagraphLineFit fit[256]{};NativeParagraphLayout placed{};
    CHECK(native_text_prepare_trim_plan(native,text,styles,width,height,wrapping,{fit,256},&placed));
    const auto clusters=native_text_candidate_prepared_clusters(native);
    IDWriteTextLayout* layout=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextLayout(utf16,reference_output.text.cluster_count,system.format,width,height,&layout)));
    struct Release {IDWriteTextLayout*& value;~Release() noexcept {com_release(value);}} release{layout};
    CHECK(SUCCEEDED(layout->SetWordWrapping(wrapping==TextWrapping::wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP)));
    IDWriteInlineObject* sign=nullptr;CHECK(SUCCEEDED(owner.factory->CreateEllipsisTrimmingSign(layout,&sign)));
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};const auto status=layout->SetTrimming(&trimming,sign);com_release(sign);CHECK(SUCCEEDED(status));
    if(system.independent_letters){IDWriteTypography* typography=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTypography(&typography)));
        const DWRITE_FONT_FEATURE_TAG tags[]{DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES,DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES,DWRITE_FONT_FEATURE_TAG_KERNING};HRESULT configured=S_OK;
        for(auto tag:tags)if(SUCCEEDED(configured))configured=typography->AddFontFeature({tag,0});
        if(SUCCEEDED(configured))configured=layout->SetTypography(typography,{0,reference_output.text.cluster_count});com_release(typography);CHECK(SUCCEEDED(configured));}
    CHECK(SUCCEEDED(text_apply_layout_styles(system,layout,text,styles)));
    DWRITE_LINE_METRICS lines[256]{};u32 count=0;CHECK(SUCCEEDED(layout->GetLineMetrics(lines,256,&count)));
    DWRITE_TEXT_METRICS metrics{};CHECK(SUCCEEDED(layout->GetMetrics(&metrics)));
    if(placed.width!=metrics.width||placed.extent!=metrics.widthIncludingTrailingWhitespace||placed.height!=metrics.height){
        print("plan/reference width/extent/height=");real(placed.width);print("/");real(metrics.width);print("/");real(placed.extent);print("/");real(metrics.widthIncludingTrailingWhitespace);print("/");real(placed.height);print("/");real(metrics.height);print("\n");
        for(u32 i=0;i<count;++i){const auto& a=fit[i];const auto& b=lines[i];print("fit/ref line source/visible/height/flags/length/height/trim=");number(a.first_cluster);print("/");number(a.visible_clusters);print("/");real(a.height);print("/");number(a.flags);print("/");number(b.length);print("/");real(b.height);print("/");number(b.isTrimmed);print("\n");}
        for(u32 i=0;i<reference_output.text.run_count;++i){const auto& r=reference_output.text.runs[i];print("reference source/count/glyphs/x/y=");number(r.text_position);print("/");number(r.text_length);print("/");number(r.glyph_count);print("/");real(r.baseline_x);print("/");real(r.baseline_y);print("\n");}
        return false;}
    if(count!=placed.line_count){print("plan line counts=");number(placed.line_count);print("/");number(count);print("\n");return false;}
    u32 source=0;
    for(u32 i=0;i<count;++i){const auto& a=fit[i];const auto& b=lines[i];
        const u32 end=a.cluster_count?clusters[a.first_cluster+a.cluster_count-1].source_end:source;
        const bool force=i+1<count&&b.height>0&&!lines[i+1].height&&!b.newlineLength;
        if(end-source!=b.length||a.height!=b.height||a.baseline!=b.baseline||((a.flags&native_line_fit_hidden)!=0)!=(b.height==0)||
            ((a.flags&native_line_fit_trimmed)!=0)!=(b.isTrimmed!=0)||((a.flags&native_line_fit_hard_end)!=0)!=(b.newlineLength!=0)||((a.flags&native_line_fit_force_sign)!=0)!=force){
            print("plan line/native length/reference length/native height/reference height/native baseline/reference baseline/flags/force=");number(i);print("/");number(end-source);print("/");number(b.length);print("/");real(a.height);print("/");real(b.height);print("/");real(a.baseline);print("/");real(b.baseline);print("/");number(a.flags);print("/");number(force);print("\n");return false;}
        if(b.height&&b.length){DWRITE_HIT_TEST_METRICS hit[256]{};u32 hits=0;CHECK(SUCCEEDED(layout->HitTestTextRange(source,b.length,0,0,hit,256,&hits)));
            u32 signs=0,cut=0;for(u32 h=0;h<hits;++h){const auto& m=hit[h];if(m.height>0&&m.width>0&&!m.isText&&m.textPosition<end){++signs;cut=m.textPosition;}}
            const u32 expected=a.visible_clusters?clusters[a.first_cluster+a.visible_clusters-1].source_end:source;
            if(signs!=((a.flags&native_line_fit_sign)?1u:0u)||(signs&&cut!=expected)){
                print("plan sign line/count/expected sign/cut/expected cut=");number(i);print("/");number(signs);print("/");number((a.flags&native_line_fit_sign)!=0);print("/");number(cut);print("/");number(expected);print("\n");
                for(u32 h=0;h<hits;++h){const auto& m=hit[h];print("hit source/length/width/height/text/trim=");number(m.textPosition);print("/");number(m.length);print("/");real(m.width);print("/");real(m.height);print("/");number(m.isText);print("/");number(m.isTrimmed);print("\n");}return false;}}
        source=end;
    }
    CHECK(source==reference_output.text.cluster_count);return true;
}
bool native_multiline_trim_corpus(DWriteTextSystem system) noexcept {
    const Utf8View texts[]{utf8_literal("AV abcdefghijklmnopqrstuvwxyz\nTo abcdefghijklmnopqrstuvwxyz"),
        utf8_literal("AV office ffi To office ffi AV office ffi To office ffi"),
        utf8_literal("AV office ffi\r\n\r\nTo office ffi\r\n"),
        utf8_literal("AV \xd7\x90\xd7\x91\xd7\x92 XYZ\nAV \xd7\x93\xd7\x94\xd7\x95 XYZ"),
        utf8_literal("AV \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 XYZ AV \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 XYZ"),
        utf8_literal("AV \xe4\xb8\xad\xe6\x96\x87 XYZ AV \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e XYZ"),
        utf8_literal("AV \xf0\x9f\x98\x80 XYZ AV \xe2\x9d\xa4\xef\xb8\x8f XYZ"),
        utf8_literal("AV\tAV\nAV abcdefghijklmnopqrstuvwxyz\nAV")};
    const float widths[]{system.em_size*2,system.em_size*5,system.em_size*12,1000000};
    const float heights[]{1,system.em_size*1.5f,system.em_size*3,1000000};
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_monospace,text_style_heading_1,text_style_heading_2,text_style_heading_3,text_style_subtext,text_style_link,
        text_style_underline,text_style_strikethrough,text_style_underline|text_style_strikethrough,text_style_monospace|text_style_underline,
        text_style_spoiler,text_style_mention,text_style_quote,text_style_monospace|text_style_code_block,text_style_list_bullet};
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    u32 unavailable=0,differences=0,total=0,plan_failures=0;
    for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample)for(u32 mode=0;mode<2;++mode)for(u32 flag_index=0;flag_index<sizeof(flags)/sizeof(flags[0]);++flag_index)for(u32 part=0;part<(flags[flag_index]?2u:1u);++part)for(u32 wrap=0;wrap<2;++wrap)for(u32 wi=0;wi<4;++wi)for(u32 hi=0;hi<4;++hi)for(u32 pattern=0;pattern<(multiline_hit_profiles?5u:1u);++pattern){
        const u32 flag=flags[flag_index];
        // Rotate dimensions across every text/style/typography/wrapping group.
        // Keep the full heading grid for the sign-only/newline regression.
        if(multiline_quick&&!(sample==0&&flag==text_style_heading_1)&&
            (wi!=((sample+flag_index+part+2*mode+wrap)&3)||hi!=((sample+2*flag_index+part+mode+2*wrap)&3)))continue;
        ++matrix_seen;
        const float width=widths[wi],height=heights[hi];
        ++cases;++total;const auto text=texts[sample];native.independent_letters=reference.independent_letters=mode!=0;
        const TextStyleSpan span{part?3u:0u,text.bytes-(part?3u:0u),flag};const ConstSlice<TextStyleSpan> styles{&span,flag?1u:0u};const auto wrapping=wrap?TextWrapping::wrap:TextWrapping::no_wrap;
        const u32 first_hit=pattern==2?3u:pattern==3?text.bytes-1:pattern==4?1u:0u,length=pattern==1||pattern==3?1u:pattern==4?2u:text.bytes-first_hit;
        const TextHitSpan hit{first_hit,length,65535,0};
        const auto b=multiline_hit_profiles?text_shape_utf8(&reference,text,styles,width,height,{utf16,256},&reference_output.text,wrapping,TextOverflow::ellipsis,{&hit,1}):
            text_shape_utf8(&reference,text,styles,width,height,{utf16,256},&reference_output.text,wrapping,TextOverflow::ellipsis);CHECK(b.ok());
        const auto a=multiline_hit_profiles?text_shape_utf8(&native,text,styles,width,height,{utf16,256},&native_output.text,wrapping,TextOverflow::ellipsis,{&hit,1}):
            text_shape_utf8(&native,text,styles,width,height,{utf16,256},&native_output.text,wrapping,TextOverflow::ellipsis);
        const bool available=a.ok()&&native_output.text.native_fonts();const bool exact=available&&same(native_output,reference_output,true)&&same_multiline_controls(native_output,reference_output);
        if(!available)++unavailable;else if(!exact)++differences;else ++native_cases;
        auto plan_system=system;plan_system.independent_letters=mode!=0;
        const bool plan_ok=!multiline_plan_profiles||compare_multiline_plan(plan_system,text,styles,width,height,wrapping);if(!plan_ok)++plan_failures;
        if(multiline_plan_profiles?!plan_ok:!exact){print("multiline style part=");number(part);print("\n");if(multiline_hit_profiles){print("multiline hit pattern=");number(pattern);print("\n");}print("multiline sample/mode/flag/wrap/width/height/em/error=");number(sample);print("/");number(mode);print("/");number(flag);print("/");number(wrap);print("/");real(width);print("/");real(height);print("/");real(system.em_size);print("/");number(static_cast<u32>(a.error));print("\n");}
    }
    if(!multiline_plan_profiles){
        const auto text=texts[0];native.independent_letters=reference.independent_letters=false;
        const TextStyleSpan underline{0,text.bytes,text_style_underline};
        CHECK(text_shape_utf8(&reference,text,{&underline,1},system.em_size*5,1000000,{utf16,256},&reference_output.text,TextWrapping::no_wrap,TextOverflow::ellipsis).ok());
        const u32 glyphs=native_output.text.glyph_capacity,runs=native_output.text.run_capacity,decorations=native_output.text.decoration_capacity;
        native_output.text.glyph_capacity=reference_output.text.glyph_count;native_output.text.run_capacity=reference_output.text.run_count;native_output.text.decoration_capacity=reference_output.text.decoration_count;
        const auto exact_fit=text_shape_utf8(&native,text,{&underline,1},system.em_size*5,1000000,{utf16,256},&native_output.text,TextWrapping::no_wrap,TextOverflow::ellipsis);
        native_output.text.glyph_capacity=glyphs;native_output.text.run_capacity=runs;native_output.text.decoration_capacity=decorations;
        CHECK(exact_fit.ok()&&native_output.text.native_fonts()&&same(native_output,reference_output,true)&&same_multiline_controls(native_output,reference_output));
        CHECK(text_shape_utf8(&native,text,{&underline,1},1000000,1000000,{utf16,256},&native_output.text,TextWrapping::wrap,TextOverflow::clip).ok());
        CHECK(text_shape_utf8(&reference,text,{&underline,1},1000000,1000000,{utf16,256},&reference_output.text,TextWrapping::wrap,TextOverflow::clip).ok());
        CHECK(native_output.text.native_fonts()&&same(native_output,reference_output,true)&&same_multiline_controls(native_output,reference_output));
    }
    if(multiline_plan_profiles){
        NativeParagraphLineFit fit[256]{};NativeParagraphLayout layout{};NativeParagraphCluster saved[256]{};
        const auto text=texts[0];auto selected=system;selected.factory=nullptr;selected.format=nullptr;
        CHECK(native_text_prepare_trim_plan(selected,text,{},system.em_size*2,1,TextWrapping::wrap,{fit,256},&layout));
        const auto original=native_text_candidate_prepared_clusters(selected);CHECK(original.count&&original.count<=256&&layout.line_count>1);
        const u32 count=original.count;copy_bytes(saved,original.data,count*sizeof(NativeParagraphCluster));
        CHECK(!native_text_prepare_trim_plan(selected,text,{},system.em_size*2,1,TextWrapping::wrap,{fit,1},&layout));
        CHECK(!layout.line_count&&!layout.run_count&&!layout.width&&!layout.height);
        CHECK(native_text_prepare_trim_plan(selected,text,{},1000000,1000000,TextWrapping::wrap,{fit,256},&layout));
        const auto complete=native_text_candidate_prepared_clusters(selected);CHECK(complete.count==count&&bytes_equal(saved,complete.data,count*sizeof(NativeParagraphCluster))&&layout.line_count==2);
        for(u32 i=0;i<layout.line_count;++i)CHECK(!(fit[i].flags&(native_line_fit_hidden|native_line_fit_force_sign|native_line_fit_sign|native_line_fit_trimmed)));
    }
    print("multiline total/unavailable/different=");number(total);print("/");number(unavailable);print("/");number(differences);print("\n");
    print("zero-glyph control origin diagnostics=");number(multiline_control_origin_differences);print("\n");
    print("multiline plan failures=");number(plan_failures);print("\n");
    return multiline_plan_profiles?!plan_failures:!unavailable&&!differences;
}
