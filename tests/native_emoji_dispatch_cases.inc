u32 emoji_draw_declines=0,emoji_hit_declines=0;
bool emoji_dispatch_case(DWriteTextSystem system,DWriteTextSystem reference,Utf8View text,bool detailed,bool required=true,bool mixed=false) noexcept {
    struct Scenario {float width;TextWrapping wrap;};
    const Scenario scenarios[]{{8,TextWrapping::wrap},{37,TextWrapping::wrap},{1000000,TextWrapping::no_wrap}};
    const u32 flags[]{0,text_style_bold|text_style_italic|text_style_monospace,text_style_link,
        text_style_underline|text_style_strikethrough,text_style_heading_2|text_style_bold};
    DWriteEditLayoutOwner layouts[2]{};TextEditMetric selections[2][48];
    for(u32 i=0;i<2;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
    struct Release {DWriteEditLayoutOwner* p;~Release() noexcept {text_edit_layout_release(p);text_edit_layout_release(p+1);}} release{layouts};
    for(u32 mode=0;mode<(detailed?4u:1u);++mode)for(u32 f=0;f<(detailed?5u:1u);++f)for(const auto& scenario:scenarios){
        const float width=scenario.width;const auto wrap=scenario.wrap;
        ++cases;system.tracking=reference.tracking=mode&1?.03f:0;system.independent_letters=reference.independent_letters=(mode&2)!=0;
        const TextStyleSpan style{0,text.bytes,flags[f]};const ConstSlice<TextStyleSpan> styles{&style,flags[f]?1u:0u};
        CHECK(text_shape_utf8_styled(&system,text,styles,width,1000000,{utf16,256},&native_output.text,wrap).ok());
        CHECK((native_output.text.native_fonts()!=nullptr)==required);if(required)++native_cases;
        CHECK(text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrap).ok());CHECK(same(native_output,reference_output,mixed));
        const auto a=text_measure_utf8_styled(&system,text,styles,width,1000000,{utf16,256},wrap);TextMeasureResult b{};
        CHECK(reference_shape_measure(reference,text,styles,width,1000000,{utf16,256},wrap,width==8||(mode==3&&wrap==TextWrapping::no_wrap),&b));
        CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count&&a.utf16_units==b.utf16_units);
        const TextHitSpan hit{0,text.bytes,65535,0};
        CHECK(text_shape_utf8_interactive(&system,text,styles,{&hit,1},width,1000000,{utf16,256},&native_output.text,wrap).ok()&&((native_output.text.native_fonts()!=nullptr)==required));
        CHECK(text_shape_utf8_interactive(&reference,text,styles,{&hit,1},width,1000000,{utf16,256},&reference_output.text,wrap).ok());CHECK(same(native_output,reference_output,mixed));
        {const u32 units=native_output.text.cluster_count;
            // The app's editor resets tracking/independent typography before
            // shaping. Require native geometry for every drawing-mode input.
            auto edit_system=system;const bool native_editor=required;
            for(u32 caret=0;caret<=units;++caret){
                const TextEditLayoutInput input{text,text_edit_signature(text),0,caret,9.125f,32.25f,width,40,1000000,static_cast<u8>(wrap==TextWrapping::wrap?text_edit_layout_wrap:0),styles};
                CHECK(text_edit_layout_measure(layouts,input,edit_system)&&(layouts[0].geometry==native_editor));
                CHECK(text_edit_layout_measure(layouts+1,input,reference)&&same_edit(layouts[0],layouts[1]));
                const auto builds=layouts[0].builds;CHECK(text_edit_layout_measure(layouts,input,edit_system)&&layouts[0].builds==builds);
            }
        }
    }
    return true;
}
bool emoji_dispatch_cuts(DWriteTextSystem system,DWriteTextSystem reference,Utf8View text,bool mixed=false) noexcept {
    const u32 flags[]{text_style_link,text_style_bold|text_style_italic,text_style_underline|text_style_strikethrough,text_style_monospace,text_style_heading_2};
    const float widths[]{37,1000000};auto observed=system;system.factory=nullptr;system.format=nullptr;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);CHECK(cp.valid);
        for(u32 flag:flags)for(u32 tail=0;tail<(at+cp.width==text.bytes?1u:2u);++tail)for(float width:widths){++cases;
            const TextStyleSpan style{at,tail?text.bytes-at:cp.width,flag};const ConstSlice<TextStyleSpan> styles{&style,1};
            shaped_text_reset(&native_output.text);const bool native=native_text_candidate_styled(system,text,styles,width,1000000,&native_output.text,nullptr,TextWrapping::wrap);
            if(!mixed&&text_style_font_size(system.em_size,flag)==system.em_size)CHECK(native);if(native)++native_cases;
            else{++emoji_draw_declines;CHECK(text_shape_utf8_styled(&observed,text,styles,width,1000000,{utf16,256},&native_output.text).ok());}
            CHECK(text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text).ok());
            if(!same(native_output,reference_output)){print("emoji cut byte/flag/tail/width=");number(at);print("/");number(flag);print("/");number(tail);print("/");real(width);print("\n");return false;}
            const auto a=text_measure_utf8_styled(&observed,text,styles,width,1000000,{utf16,256});TextMeasureResult b{};
            CHECK(reference_shape_measure(reference,text,styles,width,1000000,{utf16,256},TextWrapping::wrap,at==0||at+cp.width==text.bytes,&b));
            CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count);
            NativeParagraphHits geometry{};SfntFontLineParts parts[513];TextMeasureResult measured{};
            NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{hit_advances,1280},&geometry,{parts,513}};
            if(native_text_candidate_styled(system,text,styles,width,1000000,nullptr,&measured,TextWrapping::wrap,TextOverflow::clip,{},&storage))CHECK(compare_hits(observed,text,width,TextWrapping::wrap,styles));
            else{++emoji_hit_declines;DWriteEditLayoutOwner layouts[2]{};TextEditMetric selections[2][48];
                for(u32 i=0;i<2;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
                struct Release {DWriteEditLayoutOwner* p;~Release() noexcept {text_edit_layout_release(p);text_edit_layout_release(p+1);}} release{layouts};
                const TextEditLayoutInput input{text,text_edit_signature(text),0,native_output.text.cluster_count,9,32,width,40,1000000,text_edit_layout_wrap,styles};
                CHECK(text_edit_layout_measure(layouts,input,observed)&&!layouts[0].geometry);
                CHECK(text_edit_layout_measure(layouts+1,input,reference)&&same_edit(layouts[0],layouts[1]));}
        }at+=cp.width;
    }
    return true;
}
bool native_emoji_dispatch_corpus(const wchar_t* path,bool mixed) noexcept {
    if(*path){wchar_t name[32768];u32 n=0;const bool quoted=*path=='"';if(quoted)++path;
        while(*path&&!(quoted?*path=='"':*path==' '||*path=='\t')){CHECK(n+1<32768);name[n++]=*path++;}name[n]=0;
        HANDLE file=CreateFileW(name,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
        struct Release {HANDLE h;~Release() noexcept {CloseHandle(h);}} release{file};
        const auto read=[&](void* output,u32 bytes) noexcept {DWORD done=0;return ReadFile(file,output,bytes,&done,nullptr)&&done==bytes;};
        u32 header[2];CHECK(read(header,sizeof(header))&&header[0]==0x3244454d&&header[1]&&header[1]<=100000);
        auto reference=owner;reference.native_text=nullptr;auto system=owner;system.factory=nullptr;system.format=nullptr;
        constexpr u16 family[]{'S','e','g','o','e',' ','U','I'};
        u32 selected=0;
        for(u32 i=0;i<header[1];++i){float em=0;u32 bytes=0;char text[512];CHECK(read(&em,4)&&em>0&&em<=1000000&&read(&bytes,4)&&bytes&&bytes<=sizeof(text)&&read(text,bytes));
            IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
            struct FormatRelease {IDWriteTextFormat*& p;~FormatRelease() noexcept {com_release(p);}} format_release{format};reference.format=format;reference.em_size=system.em_size=em;
            CHECK(native_text_format_create(system.native_text,{{family,8},0,{400,5,0},em,em*4,true},&system.native_format));
            TextEmojiCursor cursor{};TextEmojiRange range{};const bool whole=text_emoji_next({text,bytes},&cursor,&range)==TextEmojiNext::range&&!range.first_byte&&range.end_byte==bytes;
            bool required=mixed||whole;
            // Keep every previous positive gate. Additional mixed inputs must
            // execute consistently at all widths and match the app oracle.
            if(!required){TextMeasureResult probe{};required=native_text_candidate_line(system,{text,bytes},1000000,1000000,nullptr,&probe);}
            auto active=system;if(required)++selected;else{active.factory=owner.factory;active.format=format;}
            if(!emoji_dispatch_case(active,reference,{text,bytes},false,required,mixed||!whole)){print("emoji_corpus_index=");number(i);print("\n");return false;}}
        char tail;DWORD got=0;CHECK(ReadFile(file,&tail,1,&got,nullptr)&&!got);print("emoji_inputs=");number(header[1]);print(" selected=");number(selected);print("\n");return true;
    }
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas",L"Segoe UI Emoji"};const float sizes[]{14,17.5f,1000};
    const Utf8View texts[]{utf8_literal("\xf0\x9f\x98\x80"),utf8_literal("\xf0\x9f\x98\x80\xf0\x9f\x98\x83"),
        utf8_literal("\xe2\x9d\xa4\xef\xb8\x8f"),utf8_literal("1\xef\xb8\x8f\xe2\x83\xa3"),
        utf8_literal("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"),
        utf8_literal("\xf0\x9f\x8f\xb4\xf3\xa0\x81\xa7\xf3\xa0\x81\xa2\xf3\xa0\x81\xa5\xf3\xa0\x81\xae\xf3\xa0\x81\xa7\xf3\xa0\x81\xbf")};
    for(const auto* family:families)for(float em:sizes){
        IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_FONT_STYLE_ITALIC,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
        struct Release {IDWriteTextFormat*& p;~Release() noexcept {com_release(p);}} release{format};
        auto reference=owner;reference.native_text=nullptr;reference.format=format;reference.em_size=em;
        auto system=owner;system.factory=nullptr;system.format=nullptr;system.em_size=em;
        u16 name[32];u32 count=0;while(family[count]){name[count]=static_cast<u16>(family[count]);++count;}
        CHECK(native_text_format_create(system.native_text,{{name,count},0,{600,5,2},em,em*4,true},&system.native_format));
        for(const auto text:texts){CHECK(emoji_dispatch_case(system,reference,text,true));auto observed=system;observed.factory=owner.factory;observed.format=format;
            CHECK(emoji_dispatch_cuts(observed,reference,text));}
    }
    print("emoji_draw_declines=");number(emoji_draw_declines);print(" emoji_hit_declines=");number(emoji_hit_declines);print("\n");return true;
}
