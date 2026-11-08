// Trimming at every cut: widths at each glyph boundary of the untrimmed line plus the sign's width (and one ulp to
// either side), each trimmed line observed; --ellipsis-line-profile also clips each to a one-pixel height.
bool native_ellipsis_cut_corpus(AppSystem system) noexcept {
    Output trimmed{};CHECK(shaped_text_initialize(&trimmed.text,{trimmed.glyphs,1024},{trimmed.runs,256},{trimmed.clusters,256}));
    struct ReleaseTrimmed {ShapedText& text;~ReleaseTrimmed() noexcept {shaped_text_reset(&text);}} release_trimmed{trimmed.text};
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
    shaped_text_reset(&second_output.text);
    CHECK(text_shape_utf8(&system, {utf8_literal("\xe2\x80\xa6"),{},1000000,1000000}, &second_output.text).ok()&&second_output.text.run_count==1);
    const float sign_width=second_output.text.width;
    u32 sample=0;
    for(auto text:samples){
        CHECK(text_shape_utf8(&system, {text,{},1000000,1000000,TextWrapping::no_wrap}, &second_output.text).ok());
        float widths[800]{1,8,14,20,23,30,45,60,95,150,1000000};u32 width_count=11;float pen=0;
        for(u32 g=0;g<second_output.text.glyph_count&&width_count+3<=800;++g){pen+=second_output.text.glyphs[g].advance;
            const float boundary=pen+sign_width;u32 bits=0;copy_bytes(&bits,&boundary,4);
            for(u32 adjacent=bits-1;adjacent<=bits+1;++adjacent){copy_bytes(widths+width_count,&adjacent,4);++width_count;}}
        for(u32 w=0;w<width_count;++w){++cases;const float width=widths[w];
            const float heights[]{1000000.f,1.f};
            for(u32 h=0;h<(ellipsis_line_profiles?2u:1u);++h){
                const auto result=text_shape_utf8(&system, {text,{},width,heights[h],TextWrapping::no_wrap,TextOverflow::ellipsis}, &trimmed.text);
                if(!result.ok()||!trimmed.text.native_fonts()){
                    print("native ellipsis sample/width/em/error=");number(sample);print("/");real(width);print("/");real(system.em_size);print("/");number(static_cast<u32>(result.error));print("\n");return false;}
                observe(trimmed.text,text,system);
            }
            ++native_cases;
        }
        ++sample;
    }
    return true;
}
