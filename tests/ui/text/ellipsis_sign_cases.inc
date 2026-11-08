// The omission sign inherits the base format before per-range styling and typography: the trimmed line's last run
// draws the glyphs, font and size of a detached shape of U+2026 alone with the base format.
bool same_face(const ShapedText& a,const TextRun& x,const ShapedText& b,const TextRun& y) noexcept {
    const auto* p=native_font_run_get(*native_font_session_runs(a.native_fonts()),x.native_face);
    const auto* q=native_font_run_get(*native_font_session_runs(b.native_fonts()),y.native_face);
    return p&&q&&a.native_fonts()==b.native_fonts()&&p->source.face.slot==q->source.face.slot&&p->source.face.generation==q->source.face.generation&&
        p->simulations==q->simulations&&p->source.count==q->source.count&&bytes_equal(p->source.settings,q->source.settings,p->source.count*sizeof(FontAxisSetting));
}
bool native_ellipsis_sign_corpus(AppSystem system) noexcept {
    auto detached=system;detached.independent_letters=false;detached.tracking=0;
    const auto sign=utf8_literal("\xe2\x80\xa6");
    const auto source=utf8_literal("abcdefghijklmnopqrstuvwxyz");
    const u32 styles[]{0,text_style_bold,text_style_italic,text_style_monospace,
        text_style_heading_3,text_style_underline,text_style_strikethrough,text_style_link};
    const float tracking_values[]{0.f,.125f};
    for(u32 flag:styles)for(u32 mode=0;mode<2;++mode)for(float tracking:tracking_values) {
        ++cases;system.independent_letters=mode!=0;system.tracking=tracking;
        const TextStyleSpan span{0,source.bytes,flag};
        const auto a=text_shape_utf8(&detached, {sign,{},1000000,1000000}, &second_output.text);
        const auto b=text_shape_utf8(&system, {source,{&span,flag?1u:0u},1,1000000,TextWrapping::no_wrap,TextOverflow::ellipsis}, &native_output.text);
        if(!a.ok()||!second_output.text.native_fonts()||!b.ok()||!native_output.text.run_count||!native_output.text.native_fonts()){
            print("ellipsis source errors=");number(static_cast<u32>(a.error));print("/");number(static_cast<u32>(b.error));print(" trimmed runs=");number(native_output.text.run_count);print("\n");return false;}
        const auto& n=second_output.text;const auto& r=native_output.text;observe(n,sign,detached);observe(r,source,system);
        const auto& tail=r.runs[r.run_count-1];
        if(n.run_count!=1||tail.text_position!=0||tail.text_length!=1||tail.glyph_count!=n.glyph_count||
            tail.em_size!=n.runs[0].em_size||tail.bidi_level!=n.runs[0].bidi_level||!same_face(n,n.runs[0],r,tail)||
            !bytes_equal(n.glyphs,r.glyphs+tail.first_glyph,n.glyph_count*sizeof(TextGlyph))) {
            print("ellipsis sign flag/mode/tracking/em=");number(flag);print("/");number(mode);print("/");real(tracking);print("/");real(system.em_size);print("\n");
            print("detached/trimmed sign glyph counts=");number(n.glyph_count);print("/");number(tail.glyph_count);print("\n");return false;
        }
        ++native_cases;
    }
    return true;
}
