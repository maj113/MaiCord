inline const Utf8View directional_editor_samples[]{
        utf8_literal("\xd7\x90\xd7\x91\xd7\x92"),
        utf8_literal("A \xd7\x90\xd7\x91\xd7\x92 B"),
        utf8_literal("\xd7\x90 123 \xd7\x91"),
        utf8_literal("\xd7\x90 (abc) 12 \xd7\x91"),
        utf8_literal("\xd7\x90 \xce\xb1 \xd0\x91 \xd7\x91"),
        utf8_literal("A \xd7\x90\xd6\xb0\xd6\xb0 B"),
        utf8_literal("A \xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 xyz"),
        utf8_literal("\xd7\x90\xe2\x80\x8d \xd7\x91"),
        utf8_literal("\xd7\x90\xcd\x8f \xd7\x91"),
        utf8_literal("abc\t\xd7\x90\r\n\xd7\x91 xyz  "),
        utf8_literal("A \xd7\x90 \xe2\x9d\xa4\xef\xb8\x8f \xd7\x91 Z"),
        utf8_literal("A \xd7\x90 1\xef\xb8\x8f\xe2\x83\xa3 \xd7\x91 Z"),
        utf8_literal("A \xe2\x9d\xa4\xef\xb8\x8e B"),
        utf8_literal("\xce\xb1 \xe2\x9d\xa4\xef\xb8\x8e \xd0\x91"),
        utf8_literal("\xf0\x9f\x98\x80" "A"),
        utf8_literal("A\xef\xb8\x8f B"),
        utf8_literal("e\xcc\x81 \xd7\x90 A\xcc\x81\xcc\x88"),
        utf8_literal("\xd7\x90 \xf0\x9f\x98\x80 \xf0\x9f\x98\x83 \xd7\x91"),
        utf8_literal("  \xd7\x90 \n\n\xd7\x91  "),
        utf8_literal("A \xd7\x90\xd7\x91\xd7\x92 abc \xd7\x93\xd7\x94\xd7\x95 B")};
inline bool directional_semantic_large_origin(DWriteTextSystem system) noexcept {
    struct LargeOutput {ShapedText text;TextGlyph glyphs[4096];TextRun runs[4096];u16 clusters[4096];TextHitRegion hits[8];};
    static LargeOutput actual{},expected{};static char bytes[4005];static wchar_t wide[4096];
    for(u32 i=0;i<4000;++i)bytes[i]='A';bytes[4000]=' ';bytes[4001]='\xd7';bytes[4002]='\x90';bytes[4003]='\xd7';bytes[4004]='\x91';
    const Utf8View text{bytes,4005};const TextStyleSpan style{0,4005,text_style_link};const TextHitSpan hit{4001,4,19,0};
    LargeOutput* outputs[]{&actual,&expected};for(auto* out:outputs){CHECK(shaped_text_initialize(&out->text,{out->glyphs,4096},{out->runs,4096},{out->clusters,4096},{},{out->hits,8}));}
    struct Release {LargeOutput& actual;LargeOutput& expected;~Release() noexcept {shaped_text_reset(&actual.text);shaped_text_reset(&expected.text);}} release{actual,expected};
    auto native=system;native.factory=nullptr;native.format=nullptr;auto reference=system;reference.native_text=nullptr;
    CHECK(text_shape_utf8(&native,text,{&style,1},1000000,1000000,{wide,4096},&actual.text,TextWrapping::no_wrap,TextOverflow::clip,{&hit,1}).ok());
    CHECK(actual.text.native_fonts());CHECK(text_shape_utf8(&reference,text,{&style,1},1000000,1000000,{wide,4096},&expected.text,TextWrapping::no_wrap,TextOverflow::clip,{&hit,1}).ok());
    CHECK(actual.text.glyph_count==expected.text.glyph_count&&actual.text.cluster_count==expected.text.cluster_count);
    CHECK(bytes_equal(actual.glyphs,expected.glyphs,actual.text.glyph_count*sizeof(TextGlyph))&&bytes_equal(actual.clusters,expected.clusters,actual.text.cluster_count*sizeof(u16)));
    CHECK(expected.text.hit_region_count==1&&expected.hits[0].left>1000000);
    CHECK(actual.text.hit_region_count==1&&bytes_equal(actual.hits,expected.hits,sizeof(TextHitRegion)));return true;
}
inline bool directional_semantic_options(DWriteTextSystem system) noexcept {
    if(system.em_size==1000)CHECK(directional_semantic_large_origin(system));
    const auto text=utf8_literal("A \xd7\x90\xd7\x91\xd7\x92 B\n\xd7\x90 \xe2\x9d\xa4\xef\xb8\x8f \xd7\x91");
    const TextStyleSpan style{0,text.bytes,text_style_link};const TextHitSpan public_hit{0,text.bytes,65535,0};
    auto detached=system;detached.factory=nullptr;detached.format=nullptr;
    // Populate a drawing-only cache first. Hit publication must add the line
    // geometry it needs, then safely reuse it for changed semantic identities.
    CHECK(text_shape_utf8(&detached,text,{&style,1},37,1000000,{utf16,256},&native_output.text).ok());
    CHECK(native_output.text.native_fonts());shaped_text_reset(&native_output.text);
    CHECK(semantic_compare(system,text,{&style,1},{&public_hit,1},true,37,TextWrapping::wrap,true));
    const u32 needed=native_output.text.hit_region_count,capacity=native_output.text.hit_region_capacity;
    CHECK(needed>1);shaped_text_reset(&native_output.text);u32 units=0;CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));
    TextHitRange hit{0,units,17,0};
    const auto submit=[&](const TextHitRange& range) noexcept {return native_text_candidate_styled(detached,text,{&style,1},37,1000000,&native_output.text,nullptr,TextWrapping::wrap,TextOverflow::clip,{&range,1});};
    native_output.text.hit_region_capacity=needed-1;CHECK(!submit(hit));
    CHECK(!native_output.text.native_fonts()&&!native_output.text.run_count&&!native_output.text.glyph_count&&!native_output.text.cluster_count&&!native_output.text.decoration_count&&!native_output.text.hit_region_count);
    native_output.text.hit_region_capacity=needed;CHECK(submit(hit)&&native_output.text.hit_region_count==needed);
    for(u32 i=0;i<needed;++i)CHECK(native_output.text.hit_regions[i].semantic_index==17);
    shaped_text_reset(&native_output.text);native_output.text.hit_region_capacity=capacity;
    const TextHitRange invalid[]{{0,0,0,0},{units,1,0,0},{0,0xffffffffu,0,0},{0xffffffffu,2,0,0}};
    for(const auto& range:invalid){CHECK(!submit(range));CHECK(!native_output.text.native_fonts()&&!native_output.text.run_count&&!native_output.text.hit_region_count);}
    CHECK(semantic_compare(system,text,{&style,1},{&public_hit,1},true,37,TextWrapping::wrap,true));return true;
}
inline bool directional_semantic_corpus(DWriteTextSystem system) noexcept {
    CHECK(directional_semantic_options(system));
    const auto& texts=directional_editor_samples;
    const u32 flags[]{0,text_style_link,text_style_underline,text_style_bold|text_style_italic,text_style_monospace,text_style_heading_3,text_style_quote,text_style_mention};
    const float widths[]{1,16,37,95,1000000};
    for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample){const auto text=texts[sample];u32 offsets[256],scalars=0;
        for(u32 byte=0;byte<text.bytes;){offsets[scalars++]=byte;const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);CHECK(cp.valid);byte+=cp.width;}offsets[scalars]=text.bytes;
        for(u32 pattern=0;pattern<3;++pattern)for(u32 flag:flags){TextStyleSpan spans[3]{};u32 used=0;
            if(flag){if(pattern==0)spans[used++]={0,text.bytes,flag};
                else if(pattern==1)spans[used++]={offsets[scalars/2],text.bytes-offsets[scalars/2],flag};
                else{spans[used++]={0,offsets[maximum(1u,scalars/3)],flag};spans[used++]={offsets[2*scalars/3],text.bytes-offsets[2*scalars/3],flag|text_style_italic};}}
            for(u32 mode=0;mode<2;++mode)for(u32 nowrap=0;nowrap<2;++nowrap)for(float requested:widths){system.independent_letters=mode!=0;
                const float width=requested==1000000?requested:requested*system.em_size/16;TextHitSpan hits[kTextHitSpanMaximum];u32 count=0;
                const auto submit=[&]() noexcept {if(semantic_compare(system,text,{spans,used},{hits,count},true,width,nowrap?TextWrapping::no_wrap:TextWrapping::wrap,true))return true;
                    print("directional semantic sample/pattern/flag/em/mode/nowrap/width/first/last= ");number(sample);print("/");number(pattern);print("/");number(flag);print("/");real(system.em_size);print("/");number(mode);print("/");number(nowrap);print("/");real(width);print("/");number(hits[0].byte_offset);print("/");number(hits[count-1].byte_offset+hits[count-1].byte_count);print("\n");return false;};
                for(u32 first=0;first<scalars;++first)for(u32 last=scalars;last>first;--last){hits[count]={offsets[first],offsets[last]-offsets[first],static_cast<u16>(65535-count),0};++count;
                    if(count==kTextHitSpanMaximum){if(!submit())return false;count=0;}}
                if(count&&!submit())return false;
            }
        }
    }return true;
}
