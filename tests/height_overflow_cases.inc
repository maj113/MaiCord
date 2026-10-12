// Native height suppression keeps hidden logical lines, without drawing runs.
bool native_height_overflow_corpus(DWriteTextSystem system) noexcept {
    const Utf8View texts[]{
        utf8_literal("\x61\x0a\x62\x0a\x63"),
        utf8_literal("\x41\x56\x0a\x54\x6f\x0a\x66\x66\x69"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\x0a\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\x0a\xe4\xb8\xad\xe6\x96\x87"),
        utf8_literal("\xd7\x90\xd7\x91\xd7\x92\x0a\xd7\x93\xd7\x94\xd7\x95\x0a\xd7\x96\xd7\x97\xd7\x98"),
        utf8_literal("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7\x0a\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85\x0a\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85"),
        utf8_literal("\xf0\x9f\x98\x80\x0a\xe2\x9d\xa4\xef\xb8\x8f\x0a\xf0\x9f\x87\xb8\xf0\x9f\x87\xae"),
        utf8_literal("\x61\xcc\x81\x0a\xce\xb1\xcc\x81\x0a\xd0\xb8\xcc\x86"),
        utf8_literal("\x61\x0d\x0a\x62\x0d\x0a\x0d\x0a\x63"),
        utf8_literal("\x0a\x0a\x0a\x61"),
        utf8_literal("\x61\x0a"),
        utf8_literal("\x0a"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\x0a\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\x0a\xe4\xb8\xad\xe6\x96\x87\x0a\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e")};
    const float heights[]{1,14,18.6210918f,18.62109375f,18.6210957f,20,37.2421837f,37.2421875f,37.2421913f,40,1000000};const u32 flags[]{0,text_style_bold,text_style_italic,text_style_underline,text_style_monospace,text_style_heading_3};
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    u32 sample=0;for(auto text:texts){for(u32 mode=0;mode<2;++mode)for(u32 pattern=0;pattern<2;++pattern)for(u32 flag:flags)for(float height:heights){
        ++cases;native.independent_letters=reference.independent_letters=mode!=0;const u32 first=pattern?utf8_decode_one(text.data,text.bytes).width:0u;const TextStyleSpan span{first,text.bytes-first,flag};const ConstSlice<TextStyleSpan> styles{&span,flag&&span.byte_count?1u:0u};
        const auto a=text_shape_utf8_styled(&native,text,styles,1000000,height,{utf16,256},&native_output.text,TextWrapping::wrap,TextOverflow::ellipsis);
        const auto b=text_shape_utf8_styled(&reference,text,styles,1000000,height,{utf16,256},&reference_output.text,TextWrapping::wrap,TextOverflow::ellipsis);
        if(!a.ok()||!native_output.text.native_fonts()||!b.ok()||!same(native_output,reference_output,true)){
            print("height overflow sample/pattern/flag/height/em/mode/errors=");number(sample);print("/");number(pattern);print("/");number(flag);print("/");real(height);print("/");real(system.em_size);print("/");number(mode);print("/");number(static_cast<u32>(a.error));print("/");number(static_cast<u32>(b.error));print("\n");return false;}
        ++native_cases;
    }++sample;}
    const auto text=utf8_literal("a\nb\nc");const auto glyph_capacity=native_output.text.glyph_capacity,run_capacity=native_output.text.run_capacity;
    native_output.text.glyph_capacity=1;native_output.text.run_capacity=2;
    const auto result=text_shape_utf8_styled(&native,text,{},1000000,1,{utf16,256},&native_output.text,TextWrapping::wrap,TextOverflow::ellipsis);
    native_output.text.glyph_capacity=glyph_capacity;native_output.text.run_capacity=run_capacity;
    CHECK(result.ok()&&native_output.text.native_fonts()&&native_output.text.glyph_count==1&&native_output.text.line_count==3&&native_output.text.clusters[2]==0xffffu);
    // Reflow the same retained shape back to ordinary unclipped publication.
    CHECK(text_shape_utf8_styled(&native,text,{},1000000,1,{utf16,256},&native_output.text,TextWrapping::wrap,TextOverflow::clip).ok());
    CHECK(native_output.text.native_fonts()&&native_output.text.glyph_count==3&&native_output.text.line_count==3);
    return true;
}
