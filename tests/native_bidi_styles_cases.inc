bool native_bidi_style_range_probe(bool check=false) noexcept {
    const Utf8View texts[]{
        utf8_literal("A \xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 xyz"),utf8_literal("A \xd7\xa9\xd6\xb8\xd6\xbc xyz"),
        utf8_literal("A \xd7\x91\xd6\xb8\xd6\xbc xyz"),utf8_literal("A \xd7\x90\xd6\xb0\xd6\xb0 xyz"),
        utf8_literal("A \xd7\x90\xd6\xb7\xd6\xbc\xd6\x91 xyz"),utf8_literal("A \xd7\x90\xd6\xb0 xyz"),
        utf8_literal("A \xd7\xa9\xd6\xb8 xyz"),utf8_literal("A \xd7\xa9\xd6\xbc xyz"),
        utf8_literal("A a\xcc\x81\xcc\x88 xyz")};
    auto reference=owner;reference.native_text=nullptr;bidi_styles_profiles=true;
    auto native=owner;CHECK(native_text_format_capture(&native));native.factory=nullptr;native.format=nullptr;
    for(u32 i=0;i<sizeof(texts)/sizeof(texts[0]);++i){u32 offsets[256],count=0;
        for(u32 at=0;at<texts[i].bytes;){offsets[count++]=at;at+=utf8_decode_one(texts[i].data+at,texts[i].bytes-at).width;}offsets[count]=texts[i].bytes;
        for(u32 end=0;end<=count;++end){if(!check){print("range_case=");number(i);print(" style_end=");number(end);print("\n");}
            const TextStyleSpan style{0,offsets[end],text_style_quote};const ConstSlice<TextStyleSpan> styles{&style,end?1u:0u};
            CHECK(text_shape_utf8_styled(&reference,texts[i],styles,1,1000000,{utf16,256},&reference_output.text).ok());
            if(check){++cases;CHECK(text_shape_utf8_styled(&native,texts[i],styles,1,1000000,{utf16,256},&native_output.text).ok()&&native_output.text.native_fonts());++native_cases;
                if(!same(native_output,reference_output)){print("range_case/style_end=");number(i);print("/");number(end);print("\n");return false;}}
            else CHECK(inspect_style_layout(owner,texts[i],styles,1,TextWrapping::wrap));
        }
    }
    if(check){const Utf8View controls[]{utf8_literal("\xd7\x90\xe2\x80\x8d\xd7\x91"),utf8_literal("\xd7\x90\xe2\x80\x8c\xd7\x91"),utf8_literal("\xd7\x90\xcd\x8f \xd7\x91")};
        const u32 flags[]{text_style_underline,text_style_monospace|text_style_code_block};
        for(const auto text:controls)for(u32 flag:flags)for(u32 nowrap=0;nowrap<2;++nowrap){++cases;
            const TextStyleSpan span{0,text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&span,1};const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            CHECK(text_shape_utf8_styled(&reference,text,styles,1,1000000,{utf16,256},&reference_output.text,wrapping).ok());
            CHECK(text_shape_utf8_styled(&native,text,styles,1,1000000,{utf16,256},&native_output.text,wrapping).ok()&&native_output.text.native_fonts());++native_cases;CHECK(same(native_output,reference_output));}
    }return true;
}
bool native_bidi_styles_corpus(DWriteTextSystem system) noexcept {
    const Utf8View ordinary_texts[]{
        utf8_literal("abc \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d xyz"),
        utf8_literal("\xd7\x90 abc \xd7\x91"),
        utf8_literal("\xd7\x90 (abc) 123 \xd7\x91"),
        utf8_literal("12,345 \xd7\x90 67:89 abc"),
        utf8_literal("\xce\xb1 \xd7\x90 \xd0\x91 abc"),
        utf8_literal("abc\t\xd7\x90\r\n\xd7\x91 xyz  "),
        utf8_literal("A\xcc\x81 \xd7\x90\xd6\xb0\xd6\xb0 B\xcc\x88"),
        utf8_literal("A \xd7\xa9\xd6\xb8\xd6\xbc\xd7\x81 xyz"),
        utf8_literal("\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\x90\xd6\xb0\xd6\xb0 \xd7\x91"),
        utf8_literal("  abc \xd7\x90   xyz  "),
        utf8_literal("\xd7\x90\xc2\xa0" "abc \xd7\x91"),
        utf8_literal("abc \xd7\x90\n\nxyz \xd7\x91"),
        utf8_literal("A \xd7\x90\xd6\xb7\xd6\xbc\xd6\x91 B"),
        utf8_literal("a\xcc\x81\xcc\x88 \xd7\x90 b"),
        utf8_literal("\xce\xb1\xcc\x88\xcc\x81 \xd7\x90 \xd0\x91\xcc\x81"),
        utf8_literal("\xd7\x90\xd6\xb0 A\xcc\x81\xcc\x88 \xd7\x91\xd6\xb8\xd6\xbc"),
        utf8_literal("\xd7\x90\xe2\x80\x8d\xd7\x91 \xd7\x92\xcd\x8f\xd7\x93"),
        utf8_literal("\xd7\x90\xe2\x80\x8c\xd7\x91 \xd7\x92\xd6\xb8\xd6\xbc"),
        utf8_literal("\xd7\x90\xd6\xb0\xcd\x8f \xd7\x90\xd7\x91"),
        utf8_literal("\xd7\x90\xcd\x8f \xd7\x91"),
        utf8_literal("\xd7\x90\xe2\x80\x8d \xd7\x91"),
        utf8_literal("\xd7\x90\xe2\x80\x8c \xd7\x91"),
        utf8_literal("\xd7\x90 \xe2\x80\x8d\xd7\x91"),
        utf8_literal("\xd7\x90 \xcd\x8f\xd7\x91"),
        utf8_literal("\xd7\x90\xd6\xb0\xcd\x8f \xe2\x80\x8d"),
        utf8_literal("\xd7\x90\xe2\x80\x8d \xcd\x8f \xd7\x91"),
        utf8_literal("a \xd7\x90\xe2\x80\x8d\xd7\x91 b"),
        utf8_literal("a \xd7\x90\xe2\x80\x8c \xd7\x91 b"),
        utf8_literal("a \xd7\x90\xcd\x8f \xd7\x91 b"),
        utf8_literal("\xd7\x90\xe2\x80\x8d a \xd7\x91"),
        utf8_literal("\xd7\x90 a\xe2\x80\x8d \xd7\x91"),
        utf8_literal("\xe2\x80\x8d\xd7\x90 \xd7\x91"),
        utf8_literal("\xe2\x80\x8c\xd7\x90 \xd7\x91"),
        utf8_literal("\xcd\x8f\xd7\x90 \xd7\x91"),
        utf8_literal("a \xd7\x90\xe2\x80\x8d\n\xd7\x91 b"),
        utf8_literal("\xd7\x90 \xe2\x80\x8d\r\n\xd7\x91 x"),
        utf8_literal("\xe2\x80\x8d\n\xd7\x90"),
        utf8_literal("\xd7\x90\xcd\x8f \xd7\x91\n\xd7\x90\xe2\x80\x8d ")};
    #include "native_bidi_emoji_inputs.inc"
    #include "native_symbol_inputs.inc"
    const ConstSlice<Utf8View> texts=symbol_profiles?ConstSlice<Utf8View>{symbol_texts,sizeof(symbol_texts)/sizeof(symbol_texts[0])}:bidi_emoji_profiles?ConstSlice<Utf8View>{emoji_texts,sizeof(emoji_texts)/sizeof(emoji_texts[0])}:ConstSlice<Utf8View>{ordinary_texts,sizeof(ordinary_texts)/sizeof(ordinary_texts[0])};
    auto native=system;native.factory=nullptr;native.format=nullptr;auto reference=system;reference.native_text=nullptr;
    const u32 flags[]{text_style_bold,text_style_italic,text_style_bold|text_style_italic,text_style_link,text_style_monospace,text_style_heading_3,
        text_style_underline,text_style_strikethrough,text_style_spoiler,text_style_mention,text_style_monospace|text_style_code_block,text_style_quote,text_style_list_bullet};
    const float widths[]{1,16,37,95,1000000};
    for(u32 sample=0;sample<texts.count;++sample){const auto text=texts[sample];u32 offsets[256],count=0;
        for(u32 at=0;at<text.bytes;){offsets[count++]=at;const auto cp=utf8_decode_one(text.data+at,text.bytes-at);CHECK(cp.valid);at+=cp.width;}offsets[count]=text.bytes;
        for(u32 pattern=0;pattern<6;++pattern)for(u32 flag_index=0;flag_index<sizeof(flags)/sizeof(flags[0]);++flag_index){
            if(multiline_quick&&pattern!=(sample+flag_index)%6)continue;
            const u32 flag=flags[flag_index];TextStyleSpan spans[256]{};
            const auto build=[&](u32 shape,TextStyleSpan* out) noexcept {u32 n=0;
                const auto append=[&](u32 first,u32 last,u32 value) noexcept {if(first<last)out[n++]={offsets[first],offsets[last]-offsets[first],value};};
                if(shape==0)append(0,count/2,flag);
                else if(shape==1)append(count/2,count,flag);
                else if(shape==2)append(1,count-1,flag);
                else if(shape==3){append(0,count/3,flag);append(count/3,2*count/3,flag|text_style_bold);append(2*count/3,count,flag|text_style_italic);}
                else if(shape==4){append(0,count/3,flag);append(2*count/3,count,flag);}
                else for(u32 at=0;at<count;at+=2)append(at,at+1,flag);
                return n;};
            const u32 used=build(pattern,spans);
            // Short inputs can make two patterns produce the same span list, and
            // an empty list is independent of the flag. Such cells repeat an
            // identical native and oracle request; count them, execute once.
            // The emoji profile keeps its exact executed-case coverage guard.
            bool repeated=false;
            if(!bidi_emoji_profiles&&!multiline_quick){repeated=!used&&flag_index;
                for(u32 earlier=0;!repeated&&earlier<pattern;++earlier){TextStyleSpan prior[256];
                    repeated=build(earlier,prior)==used&&bytes_equal(prior,spans,used*sizeof(TextStyleSpan));}}
            const ConstSlice<TextStyleSpan> styles{spans,used};
            for(u32 mode=0;mode<2;++mode)for(u32 nowrap=0;nowrap<2;++nowrap)for(float requested:widths){
                if(multiline_quick&&requested!=widths[(sample+flag_index+mode+nowrap)%5])continue;
                if(!matrix_case_selected())continue;
                if(repeated){++repeated_matrix_cases;continue;}
                native.independent_letters=reference.independent_letters=mode!=0;
                const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;const float width=requested==1000000?requested:requested*system.em_size/16;
                const auto diagnostic=[&]() noexcept {print("Bidi style sample/pattern/flag/em/mode/nowrap/width=");number(sample);print("/");number(pattern);print("/");number(flag);print("/");real(system.em_size);print("/");number(mode);print("/");number(nowrap);print("/");real(width);print("\n");};
                ++cases;shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
                CHECK(text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrapping).ok());
                if(!bidi_emoji_profiles&&!symbol_profiles&&sample==23&&pattern==0&&flag==text_style_quote&&!mode&&!nowrap&&requested==1&&system.em_size==14){print("control direction probe\n");CHECK(inspect_style_layout(system,text,styles,width,wrapping));CHECK(inspect_style_layout(system,text,styles,1000000,wrapping));}
                if(!text_shape_utf8_styled(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrapping).ok()||!native_output.text.native_fonts()){
                    diagnostic();CHECK(inspect_style_layout(system,text,styles,width,wrapping));CHECK(false);}++native_cases;
                // Symbol items may partition an otherwise identical draw run.
                // Compare every glyph's exact absolute origin, physical face,
                // size, paint and direction; retain strict layout/hit checks.
                if(!same(native_output,reference_output,symbol_profiles,symbol_profiles||bidi_emoji_profiles||sample>=16)){diagnostic();CHECK(inspect_style_layout(system,text,styles,width,wrapping));if(bidi_emoji_profiles||symbol_profiles)CHECK(inspect_style_layout(system,text,styles,1000000,wrapping));return false;}
                const auto a=text_measure_utf8_styled(&native,text,styles,width,1000000,{utf16,256},wrapping);TextMeasureResult b{};
                CHECK(reference_shape_measure(reference,text,styles,width,1000000,{utf16,256},wrapping,sample==0||sample+1==texts.count,&b));
                CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count&&a.utf16_units==b.utf16_units);
            }
        }
    }return true;
}
