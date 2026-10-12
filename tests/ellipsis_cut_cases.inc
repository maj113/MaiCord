// Isolate cut selection from shaping with the reference's original clusters.
// End-to-end native ellipsis publication remains a separate acceptance gate.
bool native_ellipsis_cut_corpus(DWriteTextSystem system) noexcept {
    Output trimmed{};CHECK(shaped_text_initialize(&trimmed.text,{trimmed.glyphs,1024},{trimmed.runs,256},{trimmed.clusters,256}));
    struct ReleaseTrimmed {ShapedText& text;~ReleaseTrimmed() noexcept {shaped_text_reset(&text);}} release_trimmed{trimmed.text};
    auto native=system;native.factory=nullptr;native.format=nullptr;
    wchar_t audit_value[2]{};const bool audit=GetEnvironmentVariableW(L"MAICORD_ELLIPSIS_CUT_AUDIT",audit_value,2)==1&&audit_value[0]==L'1';u32 differences=0;
    const auto add=[](float a,float b) noexcept {return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a),_mm_set_ss(b)));};
    const Utf8View samples[]{utf8_literal("a"),utf8_literal("a "),utf8_literal("   "),
        utf8_literal("  abcdef"),utf8_literal("a    b"),utf8_literal("a\tb"),utf8_literal("\tabcdef"),
        utf8_literal("office"),utf8_literal("ffiXYZ"),utf8_literal("AVATAR"),
        utf8_literal("abcdefghijklmnopqrstuvwxyz"),utf8_literal("a b c d e"),
        utf8_literal("a\xcc\x81" "bcdef"),utf8_literal("\xd7\x90\xd7\x91\xd7\x92\xd7\x93\xd7\x94"),
        utf8_literal("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"),
        utf8_literal("\xf0\x9f\x98\x80\xf0\x9f\x98\x81\xf0\x9f\x98\x82"),
        utf8_literal("\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbbXYZ"),
        utf8_literal("abc \xd7\x90\xd7\x91\xd7\x92 xyz"),
        utf8_literal("\xd7\x90\xd7\x91\xd7\x92 abc \xd7\x93\xd7\x94"),
        utf8_literal("A \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 B"),
        utf8_literal("\xd7\x90\xd7\x91 123 \xd7\x92\xd7\x93"),
        utf8_literal("A(\xd7\x90\xd7\x91\xd7\x92)B"),
        utf8_literal("\xd7\x90\xd7\x91\t\xd7\x92\xd7\x93"),
        utf8_literal("\xd7\x90\xd7\x91\xf0\x9f\x98\x80\xd7\x92\xd7\x93"),
        utf8_literal("abc \xd7\x90\xd7\x91   ")};
    auto reference=system;reference.native_text=nullptr;
    IDWriteInlineObject* sign=nullptr;
    CHECK(SUCCEEDED(system.factory->CreateEllipsisTrimmingSign(system.format,&sign)));
    struct ReleaseSign {IDWriteInlineObject*& p;~ReleaseSign() noexcept {com_release(p);}} release_sign{sign};
    DWRITE_INLINE_OBJECT_METRICS sign_metrics{};CHECK(SUCCEEDED(sign->GetMetrics(&sign_metrics)));
    CHECK(text_shape_utf8_styled(&reference,utf8_literal("\xe2\x80\xa6"),{},1000000,1000000,{utf16,256},&native_output.text).ok());
    CHECK(native_output.text.run_count==1);
    u32 sample=0;
    for(auto text:samples){
        u32 units=0;for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);CHECK(cp.valid);
            if(cp.codepoint>65535){const u32 c=cp.codepoint-65536;utf16[units++]=static_cast<wchar_t>(0xd800+(c>>10));utf16[units++]=static_cast<wchar_t>(0xdc00+(c&1023));}
            else utf16[units++]=static_cast<wchar_t>(cp.codepoint);at+=cp.width;}
        IDWriteTextLayout* layout=nullptr;CHECK(SUCCEEDED(system.factory->CreateTextLayout(utf16,units,system.format,1000000,1000000,&layout)));
        struct ReleaseLayout {IDWriteTextLayout*& p;~ReleaseLayout() noexcept {com_release(p);}} release_layout{layout};
        CHECK(SUCCEEDED(layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)));
        DWRITE_CLUSTER_METRICS metrics[256]{};u32 count=0;CHECK(SUCCEEDED(layout->GetClusterMetrics(metrics,256,&count))&&count);
        DWRITE_TEXT_METRICS whole_metrics{};CHECK(SUCCEEDED(layout->GetMetrics(&whole_metrics)));
        NativeParagraphCluster clusters[256]{};float tabs[256]{};u32 position=0,tab_count=0;
        float widths[800]{1,8,14,20,23,30,45,60,95,150,1000000};u32 width_count=11;float pen=0;
        for(u32 i=0;i<count;++i){const auto& c=metrics[i];const bool tab=utf16[position]==L'\t';
            clusters[i]={position,position+c.length,0,0,tab?0:c.width,tab?native_paragraph_tab:c.isWhitespace?native_paragraph_space:0};position+=c.length;
            if(tab)tabs[tab_count++]=c.width;
            pen=add(pen,c.width);const float boundary=add(pen,sign_metrics.width);u32 bits=0;copy_bytes(&bits,&boundary,4);
            for(u32 adjacent=bits-1;adjacent<=bits+1;++adjacent){copy_bytes(widths+width_count,&adjacent,4);++width_count;}
        }
        const DWRITE_TRIMMING option{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};CHECK(SUCCEEDED(layout->SetTrimming(&option,sign)));
        for(u32 w=0;w<width_count;++w){++cases;const float width=widths[w];CHECK(SUCCEEDED(layout->SetMaxWidth(width)));
            DWRITE_LINE_METRICS line{};u32 actual=0;CHECK(SUCCEEDED(layout->GetLineMetrics(&line,1,&actual))&&actual==1);
            NativeLineTrimCut cut{};CHECK(native_line_trim_cut({clusters,count},whole_metrics.width,whole_metrics.widthIncludingTrailingWhitespace,width,sign_metrics.width,false,{tabs,tab_count},&cut)==SfntStatus::ok);
            CHECK(text_shape_utf8_styled(&reference,text,{},width,1000000,{utf16,256},&reference_output.text,TextWrapping::no_wrap,TextOverflow::ellipsis).ok());
            const auto& drawn=reference_output.text;u32 visible=0;CHECK(drawn.run_count);
            const auto& last=drawn.runs[drawn.run_count-1];const auto& sign_shape=native_output.text;
            const bool has_sign=last.text_position==0&&last.text_length==1&&last.glyph_count==sign_shape.glyph_count&&
                bytes_equal(drawn.glyphs+last.first_glyph,sign_shape.glyphs,last.glyph_count*sizeof(TextGlyph))&&same_native_face(sign_shape,sign_shape.runs[0],reference_run_face(last));
            for(u32 i=0;i<drawn.run_count-(has_sign?1u:0u);++i)visible=maximum(visible,drawn.runs[i].text_position+drawn.runs[i].text_length);
            const u32 selected=cut.clusters?clusters[cut.clusters-1].source_end:0;
            if(ellipsis_line_profiles){
                const float heights[]{1.f,1000000.f};for(float height:heights){
                    const auto result=text_shape_utf8_styled(&native,text,{},width,height,{utf16,256},&trimmed.text,TextWrapping::no_wrap,TextOverflow::ellipsis);
                    if(!result.ok()||!trimmed.text.native_fonts()||!same(trimmed,reference_output,true)){
                        print("native ellipsis sample/width/em/error=");number(sample);print("/");real(width);print("/");real(system.em_size);print("/");number(static_cast<u32>(result.error));print("\n");return false;}
                }
            }
            if(audit){
                print("{\"cut\":");number(sample);print(",\"em\":");real(system.em_size);print(",\"width\":");real(width);print(",\"sign_width\":");real(sign_metrics.width);
                print(",\"whole_width\":");real(whole_metrics.width);print(",\"whole_extent\":");real(whole_metrics.widthIncludingTrailingWhitespace);print(",\"draw_width\":");real(drawn.width);print(",\"draw_extent\":");real(drawn.width_including_trailing_whitespace);
                print(",\"selected\":");number(selected);print(",\"visible\":");number(visible);print(",\"has_sign\":");number(has_sign);print(",\"trial_sign\":");number(cut.sign);
                print(",\"clusters\":[");for(u32 i=0;i<count;++i){if(i)print(",");print("[");number(clusters[i].source_end);print(",");real(metrics[i].width);print(",");number(clusters[i].flags);print("]");}
                print("],\"runs\":[");for(u32 i=0;i<drawn.run_count;++i){if(i)print(",");const auto& r=drawn.runs[i];print("[");number(r.text_position);print(",");number(r.text_length);print(",");number(r.first_glyph);print(",");number(r.glyph_count);print(",");real(r.baseline_x);print("]");}
                print("],\"glyphs\":[");for(u32 i=0;i<drawn.glyph_count;++i){if(i)print(",");print("[");number(drawn.glyphs[i].index);print(",");real(drawn.glyphs[i].advance);print("]");}print("]}\n");
            }
            if(cut.sign!=has_sign||selected!=visible||cut.width!=drawn.width||cut.extent!=drawn.width_including_trailing_whitespace){
                ++differences;if(audit)continue;
                print("ellipsis cut sample/width/em/native/ref units/sign=");number(sample);print("/");real(width);print("/");real(system.em_size);print("/");number(selected);print("/");number(visible);print("/");number(cut.sign);print("/");number(has_sign);print("\n");
                for(u32 i=0;i<count;++i){print("cluster length/width/space=");number(metrics[i].length);print("/");real(metrics[i].width);print("/");number(metrics[i].isWhitespace);print("\n");}
                for(u32 i=0;i<drawn.run_count;++i){const auto& r=drawn.runs[i];print("run first/length/glyphs=");number(r.text_position);print("/");number(r.text_length);print("/");number(r.glyph_count);print("\n");}
                print("draw/sign glyphs=");for(u32 i=0;i<drawn.glyph_count;++i){number(drawn.glyphs[i].index);print(" ");}print("/");for(u32 i=0;i<sign_shape.glyph_count;++i){number(sign_shape.glyphs[i].index);print(" ");}print("\n");
                return false;}
            ++native_cases;
        }
        ++sample;
    }
    return differences==0;
}
