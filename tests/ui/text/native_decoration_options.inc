bool decoration_options() noexcept {
    Arena arena{};CHECK(arena_initialize(&arena,2*1024*1024,0,4096));Output actual{},expected{};
    struct Release {Arena& arena;Output& a;Output& b;~Release() noexcept {shaped_text_reset(&a.text);shaped_text_reset(&b.text);arena_release(&arena);}} release{arena,actual,expected};
    Output* values[]{&actual,&expected};for(auto* value:values){CHECK(shaped_text_initialize(&value->text,{arena_allocate<TextGlyph>(&arena,4096),4096},{arena_allocate<TextRun>(&arena,4096),4096},{arena_allocate<u16>(&arena,4096),4096},{arena_allocate<TextDecoration>(&arena,4096),4096}));}
    const Slice<wchar_t> scratch{arena_allocate<wchar_t>(&arena,4096),4096};CHECK(scratch.data);auto reference=owner;reference.native_text=nullptr;
    const u32 line_counts[]{64,255,256,257,300,700};const float widths[]{37.f,1000000.f};const u32 flags[]{text_style_monospace|text_style_code_block,text_style_code_block|text_style_heading_3,text_style_code_block|text_style_underline|text_style_strikethrough};
    char text[4096];for(u32 lines:line_counts){const u32 bytes=lines*3;for(u32 line=0;line<lines;++line){text[line*3]='A';text[line*3+1]='V';text[line*3+2]='\n';}
        for(u32 flag:flags)for(float width:widths){++cases;const TextStyleSpan style{0,bytes,flag};shaped_text_reset(&actual.text);shaped_text_reset(&expected.text);
            CHECK(native_text_candidate_styled(owner,{text,bytes},{&style,1},width,1000000,&actual.text,nullptr,TextWrapping::wrap));++native_cases;
            CHECK(text_shape_utf8(&reference,{text,bytes},{&style,1},width,1000000,scratch,&expected.text).ok());
            if(!same(actual,expected)){print("decoration options lines/flags/width=");number(lines);print(",");number(flag);print(",");real(width);print("\n");return false;}
        }
    }
    const auto short_text=utf8_literal("AV\nAV");const TextStyleSpan style{0,short_text.bytes,text_style_underline|text_style_strikethrough};
    shaped_text_reset(&actual.text);actual.text.decoration_capacity=1;
    CHECK(!native_text_candidate_styled(owner,short_text,{&style,1},37,1000,&actual.text,nullptr,TextWrapping::wrap));
    CHECK(!actual.text.native_fonts()&&!actual.text.run_count&&!actual.text.decoration_count);
    actual.text.decoration_capacity=4096;CHECK(native_text_candidate_styled(owner,short_text,{&style,1},37,1000,&actual.text,nullptr,TextWrapping::wrap)&&actual.text.decoration_count==4);
    shaped_text_reset(&actual.text);const TextStyleSpan overlap[]{{0,3,text_style_underline},{2,2,text_style_strikethrough}};
    CHECK(!native_text_candidate_styled(owner,short_text,{overlap,2},37,1000,&actual.text,nullptr,TextWrapping::wrap));
    const TextStyleSpan split_mark{3,2,text_style_underline};const auto marked=utf8_literal("A e\xcc\x81 B");
    CHECK(native_text_candidate_styled(owner,marked,{&split_mark,1},37,1000,&actual.text,nullptr,TextWrapping::wrap));
    CHECK(text_shape_utf8(&reference,marked,{&split_mark,1},37,1000,scratch,&expected.text).ok()&&same(actual,expected));return true;
}
