bool native_ellipsis_style_corpus(DWriteTextSystem system) noexcept {
    u32 differences=0;
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    const Utf8View samples[]{utf8_literal("AV office ffi XYZ"),utf8_literal("AV a\xcc\x81" "bcdef"),
        utf8_literal("AV \xd7\x90\xd7\x91\xd7\x92 XYZ"),
        utf8_literal("AV \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 XYZ"),
        utf8_literal("AV \xf0\x9f\x98\x80 XYZ"),utf8_literal("AV \xe4\xb8\xad\xe6\x96\x87 XYZ")};
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_monospace,text_style_heading_3,text_style_link,
        text_style_underline,text_style_strikethrough,text_style_underline|text_style_strikethrough,text_style_monospace|text_style_underline,
        text_style_spoiler,text_style_mention,text_style_quote,text_style_code_block|text_style_monospace,text_style_list_bullet};
    const float widths[]{1,8,20,45,95,1000000};
    for(u32 mode=0;mode<2;++mode)for(u32 sample=0;sample<sizeof(samples)/sizeof(samples[0]);++sample)for(u32 flag:flags)for(u32 part=0;part<(flag?2u:1u);++part)for(float width:widths)for(u32 pattern=0;pattern<(ellipsis_hit_profiles?5u:1u);++pattern){
        ++matrix_seen;
        ++cases;const auto text=samples[sample];native.independent_letters=reference.independent_letters=mode!=0;
        const TextStyleSpan style{part?3u:0u,text.bytes-(part?3u:0u),flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
        const u32 first=pattern==2?3u:pattern==3?text.bytes-1:pattern==4?1u:0u,length=pattern==1||pattern==3?1u:pattern==4?2u:text.bytes-first;
        const TextHitSpan hit{first,length,65535,0};
        const auto b=ellipsis_hit_profiles?text_shape_utf8_interactive(&reference,text,styles,{&hit,1},width,1000000,{utf16,256},&reference_output.text,TextWrapping::no_wrap,TextOverflow::ellipsis):
            text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,TextWrapping::no_wrap,TextOverflow::ellipsis);
        const auto a=ellipsis_hit_profiles?text_shape_utf8_interactive(&native,text,styles,{&hit,1},width,1000000,{utf16,256},&native_output.text,TextWrapping::no_wrap,TextOverflow::ellipsis):
            text_shape_utf8_styled(&native,text,styles,width,1000000,{utf16,256},&native_output.text,TextWrapping::no_wrap,TextOverflow::ellipsis);
        if(!a.ok()||!b.ok()||!native_output.text.native_fonts()||!same(native_output,reference_output,true)){
            if(ellipsis_hit_profiles){print("ellipsis hit pattern=");number(pattern);print("\n");}
            if(sample==0&&flag==text_style_monospace&&mode==0){print("reference trimmed mono decorations=");for(u32 d=0;d<reference_output.text.decoration_count;++d){const auto& r=reference_output.text.decorations[d];print("[");real(r.left);print(",");real(r.top);print(",");real(r.right);print(",");real(r.bottom);print("]");}print("\n");}
            if(a.ok()&&b.ok()){const auto prepared=native_text_candidate_prepared_clusters(native);print("native style cluster flags=");for(u32 c=0;c<prepared.count;++c){number(prepared[c].source_first);print(":");number(prepared[c].flags);print(" ");}print("\n");CHECK(inspect_style_layout(reference,text,styles,1000000,TextWrapping::no_wrap));
                const auto whole=text_measure_utf8_styled(&native,text,styles,1000000,1000000,{utf16,256},TextWrapping::no_wrap);
                print("native whole width/extent=");real(whole.width);print("/");real(whole.width_including_trailing_whitespace);print("\n");}
            print("styled ellipsis sample/flag/part/mode/width/em/errors=");number(sample);print("/");number(flag);print("/");number(part);print("/");number(mode);print("/");real(width);print("/");real(system.em_size);print("/");number(static_cast<u32>(a.error));print("/");number(static_cast<u32>(b.error));print("\n");++differences;continue;}
        ++native_cases;
    }
    return differences==0;
}
