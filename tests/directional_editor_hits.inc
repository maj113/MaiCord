const Utf8View directional_editor_samples[]{
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
const Utf8View directional_cut_holdouts[]{
    utf8_literal("\xd7\x90\xd7\x91" "A\xcc\x81\xcc\x88"),
    utf8_literal("\xd7\x90\xd7\x91\xe2\x9d\xa4\xef\xb8\x8eZ"),
    utf8_literal("\xd7\x90\xd7\x91\xe2\x9d\xa4\xef\xb8\x8fZ"),
    utf8_literal("\xd7\x90\xd7\x91\xce\xb1\xcc\x81"),
    utf8_literal("\xd7\x90\xd7\x91\xd0\x91\xcc\x88"),
    utf8_literal("A\xcc\x81\xcc\x88\xd7\x90\xd7\x91")};
bool directional_editor_hit_corpus(DWriteTextSystem system,bool cuts) noexcept {
    const u32 first_case=cases;
    const u64 captured_identity=native_text_format_identity(system);
    CHECK(system.native_text&&system.native_format&&captured_identity);
    const auto& texts=directional_editor_samples;
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_underline,text_style_monospace,text_style_heading_3,text_style_subtext,text_style_link,text_style_strikethrough,text_style_bold|text_style_italic,text_style_underline|text_style_strikethrough,text_style_heading_1,text_style_heading_2};
    const float widths[]{8,37,95,1000000};
    const u32 base_count=sizeof(texts)/sizeof(texts[0]),sample_count=base_count+(cuts?sizeof(directional_cut_holdouts)/sizeof(directional_cut_holdouts[0]):0u);
    for(u32 sample=0;sample<sample_count;++sample){const auto text=sample<base_count?texts[sample]:directional_cut_holdouts[sample-base_count];u32 offsets[256],scalars=0;
        for(u32 byte=0;byte<text.bytes;){offsets[scalars++]=byte;const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);CHECK(cp.valid);byte+=cp.width;}offsets[scalars]=text.bytes;
        u32 middle=offsets[scalars/2];UnicodeGraphemeState graphemes{};
        for(u32 byte=0;!cuts&&byte<text.bytes;){const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);bool boundary=false;CHECK(unicode_grapheme_next(&graphemes,cp.codepoint,&boundary));
            if(byte>=middle&&boundary){middle=byte;break;}byte+=cp.width;if(byte==text.bytes)middle=byte;}
        for(u32 pattern=0;pattern<(cuts?scalars*2:2u);++pattern)for(u32 style_index=0;style_index<(cuts?13u:8u);++style_index){const u32 flag=flags[style_index];
            const u32 first=cuts?offsets[pattern/2]:pattern?middle:0;
            const u32 end=cuts&&(pattern&1)?offsets[pattern/2+1]:text.bytes;
            const TextStyleSpan style{first,end-first,flag};
            // Flag 0 supplies no style span, so every later pattern repeats the
            // first pattern's identical request for this sample/width/wrap.
            const bool repeated=!flag&&pattern;
            for(u32 nowrap=0;nowrap<2;++nowrap)for(float requested:widths){if(cuts&&!matrix_case_selected())continue;const float width=requested==1000000?requested:requested*system.em_size/16;
                if(repeated){++repeated_matrix_cases;continue;}
                if(!compare_hits(system,text,{width},nowrap?TextWrapping::no_wrap:TextWrapping::wrap,{&style,flag&&style.byte_count?1u:0u},true,false,true)){
                    print("directional editor sample/pattern/flag/em/width/nowrap=");number(sample);print("/");number(pattern);print("/");number(flag);print("/");real(system.em_size);print("/");real(width);print("/");number(nowrap);print("\n");return false;}
            }
        }
    }
    // The corpus keeps its COM format alive for this complete font/em cell.
    // Re-read the real declaration once more before that owner releases it.
    auto control=system;
    CHECK(native_text_format_capture(&control)&&native_text_format_identity(control)==captured_identity);
    print("directional_format_capture_reuses=");number(cases-first_case);print(" end_capture_controls=1\n");
    return true;
}
