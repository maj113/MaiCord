// The trim plan alone: its lines tile the source in order and are observed with the placed layout.
bool observe_multiline_plan(const AppSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,float height,TextWrapping wrapping) noexcept {
    NativeParagraphLineFit fit[256]{};NativeParagraphLayout placed{};
    CHECK(native_text_prepare_trim_plan(system,{text,styles,width,height,wrapping},{fit,256},&placed));
    const auto clusters=native_text_candidate_prepared_clusters(system);
    observe_value(placed);observe_bytes(fit,placed.line_count*sizeof(*fit));
    u32 units=0,source=0;CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units)&&placed.line_count<=256);
    for(u32 i=0;i<placed.line_count;++i){const auto& a=fit[i];
        const u32 end=a.cluster_count?clusters[a.first_cluster+a.cluster_count-1].source_end:source;
        CHECK(end>=source&&a.visible_clusters<=a.cluster_count);source=end;}
    CHECK(source==units);return true;
}
bool native_multiline_trim_corpus(AppSystem system) noexcept {
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
    u32 unavailable=0,total=0,plan_failures=0;
    for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample)for(u32 mode=0;mode<2;++mode)for(u32 flag_index=0;flag_index<sizeof(flags)/sizeof(flags[0]);++flag_index)for(u32 part=0;part<(flags[flag_index]?2u:1u);++part)for(u32 wrap=0;wrap<2;++wrap)for(u32 wi=0;wi<4;++wi)for(u32 hi=0;hi<4;++hi)for(u32 pattern=0;pattern<(multiline_hit_profiles?5u:1u);++pattern){
        const u32 flag=flags[flag_index];
        // Rotate dimensions across every text/style/typography/wrapping group.
        // Keep the full heading grid for the sign-only/newline regression.
        if(multiline_quick&&!(sample==0&&flag==text_style_heading_1)&&
            (wi!=((sample+flag_index+part+2*mode+wrap)&3)||hi!=((sample+2*flag_index+part+mode+2*wrap)&3)))continue;
        ++matrix_seen;
        const float width=widths[wi],height=heights[hi];
        ++cases;++total;const auto text=texts[sample];system.independent_letters=mode!=0;
        const TextStyleSpan span{part?3u:0u,text.bytes-(part?3u:0u),flag};const ConstSlice<TextStyleSpan> styles{&span,flag?1u:0u};const auto wrapping=wrap?TextWrapping::wrap:TextWrapping::no_wrap;
        const u32 first_hit=pattern==2?3u:pattern==3?text.bytes-1:pattern==4?1u:0u,length=pattern==1||pattern==3?1u:pattern==4?2u:text.bytes-first_hit;
        const TextHitSpan hit{first_hit,length,65535,0};
        const auto a=text_shape_utf8(&system, {text,styles,width,height,wrapping,TextOverflow::ellipsis,multiline_hit_profiles?ConstSlice<TextHitSpan>{&hit,1}:ConstSlice<TextHitSpan>{}}, &native_output.text);
        const bool available=a.ok()&&native_output.text.native_fonts();
        if(!available)++unavailable;else{observe(native_output.text,text,system);++native_cases;}
        const bool plan_ok=!multiline_plan_profiles||observe_multiline_plan(system,text,styles,width,height,wrapping);if(!plan_ok)++plan_failures;
        if(multiline_plan_profiles?!plan_ok:!available){print("multiline style part=");number(part);print("\n");if(multiline_hit_profiles){print("multiline hit pattern=");number(pattern);print("\n");}print("multiline sample/mode/flag/wrap/width/height/em/error=");number(sample);print("/");number(mode);print("/");number(flag);print("/");number(wrap);print("/");real(width);print("/");real(height);print("/");real(system.em_size);print("/");number(static_cast<u32>(a.error));print("\n");}
    }
    if(!multiline_plan_profiles){
        // Output storage of exactly the needed size suffices; the same shape then reflows unclipped.
        const auto text=texts[0];system.independent_letters=false;
        const TextStyleSpan underline{0,text.bytes,text_style_underline};
        CHECK(text_shape_utf8(&system, {text,{&underline,1},system.em_size*5,1000000,TextWrapping::no_wrap,TextOverflow::ellipsis}, &second_output.text).ok());
        const u32 glyphs=native_output.text.glyph_capacity,runs=native_output.text.run_capacity,decorations=native_output.text.decoration_capacity;
        native_output.text.glyph_capacity=second_output.text.glyph_count;native_output.text.run_capacity=second_output.text.run_count;native_output.text.decoration_capacity=second_output.text.decoration_count;
        const auto exact_fit=text_shape_utf8(&system, {text,{&underline,1},system.em_size*5,1000000,TextWrapping::no_wrap,TextOverflow::ellipsis}, &native_output.text);
        native_output.text.glyph_capacity=glyphs;native_output.text.run_capacity=runs;native_output.text.decoration_capacity=decorations;
        CHECK(exact_fit.ok()&&native_output.text.native_fonts()&&native_output.text.glyph_count==second_output.text.glyph_count&&native_output.text.run_count==second_output.text.run_count&&
            bytes_equal(native_output.text.glyphs,second_output.text.glyphs,native_output.text.glyph_count*sizeof(TextGlyph)));
        observe(native_output.text,text,system);
        CHECK(text_shape_utf8(&system, {text,{&underline,1},1000000,1000000,TextWrapping::wrap,TextOverflow::clip}, &native_output.text).ok());
        CHECK(native_output.text.native_fonts());observe(native_output.text,text,system);
    }
    if(multiline_plan_profiles){
        NativeParagraphLineFit fit[256]{};NativeParagraphLayout layout{};NativeParagraphCluster saved[256]{};
        const auto text=texts[0];const auto& selected=system;
        CHECK(native_text_prepare_trim_plan(selected,{text,{},system.em_size*2,1,TextWrapping::wrap},{fit,256},&layout));
        const auto original=native_text_candidate_prepared_clusters(selected);CHECK(original.count&&original.count<=256&&layout.line_count>1);
        const u32 count=original.count;copy_bytes(saved,original.data,count*sizeof(NativeParagraphCluster));
        CHECK(!native_text_prepare_trim_plan(selected,{text,{},system.em_size*2,1,TextWrapping::wrap},{fit,1},&layout));
        CHECK(!layout.line_count&&!layout.run_count&&!layout.width&&!layout.height);
        CHECK(native_text_prepare_trim_plan(selected,{text,{},1000000,1000000,TextWrapping::wrap},{fit,256},&layout));
        const auto complete=native_text_candidate_prepared_clusters(selected);CHECK(complete.count==count&&bytes_equal(saved,complete.data,count*sizeof(NativeParagraphCluster))&&layout.line_count==2);
        for(u32 i=0;i<layout.line_count;++i)CHECK(!(fit[i].flags&(native_line_fit_hidden|native_line_fit_force_sign|native_line_fit_sign|native_line_fit_trimmed)));
    }
    print("multiline total/unavailable=");number(total);print("/");number(unavailable);print("\n");
    print("multiline plan failures=");number(plan_failures);print("\n");
    return multiline_plan_profiles?!plan_failures:!unavailable;
}
