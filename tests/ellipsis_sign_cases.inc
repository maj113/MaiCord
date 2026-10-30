// The omission sign inherits the base format before per-range styling and
// typography. Compare its font and shaped payload to a detached native shape.
bool native_ellipsis_sign_corpus(DWriteTextSystem system) noexcept {
    auto native=system,reference=system;
    native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    native.independent_letters=false;native.tracking=0;
    const auto sign=utf8_literal("\xe2\x80\xa6");
    IDWriteInlineObject* object=nullptr;CHECK(SUCCEEDED(system.factory->CreateEllipsisTrimmingSign(system.format,&object)));
    struct ReleaseObject {IDWriteInlineObject*& p;~ReleaseObject() noexcept {com_release(p);}} release_object{object};
    DWRITE_INLINE_OBJECT_METRICS metrics{};CHECK(SUCCEEDED(object->GetMetrics(&metrics)));
    const auto source=utf8_literal("abcdefghijklmnopqrstuvwxyz");
    const u32 styles[]{0,text_style_bold,text_style_italic,text_style_monospace,
        text_style_heading_3,text_style_underline,text_style_strikethrough,text_style_link};
    const float tracking_values[]{0.f,.125f};
    for(u32 flag:styles)for(u32 mode=0;mode<2;++mode)for(float tracking:tracking_values) {
        ++cases;reference.independent_letters=mode!=0;reference.tracking=tracking;
        const TextStyleSpan span{0,source.bytes,flag};
        const auto a=text_shape_utf8(&native,sign,{},1000000,1000000,{utf16,256},&native_output.text);
        const auto b=text_shape_utf8(&reference,source,{&span,flag?1u:0u},1,1000000,{utf16,256},
            &reference_output.text,TextWrapping::no_wrap,TextOverflow::ellipsis);
        if(!a.ok()||!native_output.text.native_fonts()||!b.ok()||!reference_output.text.run_count){
            print("ellipsis source errors=");number(static_cast<u32>(a.error));print("/");number(static_cast<u32>(b.error));print(" native=");number(native_output.text.native_fonts()!=nullptr);print(" reference runs=");number(reference_output.text.run_count);print("\n");return false;}
        const auto& n=native_output.text;const auto& r=reference_output.text;
        // The inline object's baseline is zero; its internal text layout has
        // its own baseline, used when it draws relative to the host line.
        CHECK(n.width==metrics.width&&metrics.baseline==0&&metrics.height==0);
        const auto& tail=r.runs[r.run_count-1];
        if(n.run_count!=1||tail.text_position!=0||tail.text_length!=1||tail.glyph_count!=n.glyph_count||
            tail.em_size!=n.runs[0].em_size||tail.bidi_level!=n.runs[0].bidi_level||
            !same_native_face(n,n.runs[0],reference_run_face(tail))||
            !bytes_equal(n.glyphs,r.glyphs+tail.first_glyph,n.glyph_count*sizeof(TextGlyph))) {
            print("ellipsis sign flag/mode/tracking/em=");number(flag);print("/");number(mode);print("/");real(tracking);print("/");real(system.em_size);print("\n");
            print("native/reference sign glyph counts=");number(n.glyph_count);print("/");number(tail.glyph_count);print("\n");return false;
        }
        ++native_cases;
    }
    return true;
}
