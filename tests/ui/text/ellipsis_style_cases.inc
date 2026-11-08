bool native_ellipsis_style_corpus(AppSystem system) noexcept {
    u32 differences=0;
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
        ++cases;const auto text=samples[sample];system.independent_letters=mode!=0;
        const TextStyleSpan style{part?3u:0u,text.bytes-(part?3u:0u),flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
        const u32 first=pattern==2?3u:pattern==3?text.bytes-1:pattern==4?1u:0u,length=pattern==1||pattern==3?1u:pattern==4?2u:text.bytes-first;
        const TextHitSpan hit{first,length,65535,0};
        const auto a=text_shape_utf8(&system, {text,styles,width,1000000,TextWrapping::no_wrap,TextOverflow::ellipsis,ellipsis_hit_profiles?ConstSlice<TextHitSpan>{&hit,1}:ConstSlice<TextHitSpan>{}}, &native_output.text);
        if(!a.ok()||!native_output.text.native_fonts()){
            if(ellipsis_hit_profiles){print("ellipsis hit pattern=");number(pattern);print("\n");}
            print("styled ellipsis sample/flag/part/mode/width/em/error=");number(sample);print("/");number(flag);print("/");number(part);print("/");number(mode);print("/");real(width);print("/");real(system.em_size);print("/");number(static_cast<u32>(a.error));print("\n");++differences;continue;}
        observe(native_output.text,text,system);
        ++native_cases;
    }
    return differences==0;
}
