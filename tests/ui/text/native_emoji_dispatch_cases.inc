u32 emoji_draw_declines=0,emoji_hit_declines=0;
// One emoji input under every style, tracking and typography mode: drawn, measured and hit natively when `required`
// (otherwise whatever the engine makes of it is observed), and edited by the native editor exactly when required.
bool emoji_dispatch_case(AppSystem system,Utf8View text,bool detailed,bool required=true) noexcept {
    struct Scenario {float width;TextWrapping wrap;};
    const Scenario scenarios[]{{8,TextWrapping::wrap},{37,TextWrapping::wrap},{1000000,TextWrapping::no_wrap}};
    const u32 flags[]{0,text_style_bold|text_style_italic|text_style_monospace,text_style_link,
        text_style_underline|text_style_strikethrough,text_style_heading_2|text_style_bold};
    TextEditLayoutOwner editor{};TextEditMetric selections[48];editor.selections=selections;editor.selection_capacity=48;
    struct Release {TextEditLayoutOwner& p;~Release() noexcept {text_edit_layout_release(&p);}} release{editor};
    for(u32 mode=0;mode<(detailed?4u:1u);++mode)for(u32 f=0;f<(detailed?5u:1u);++f)for(const auto& scenario:scenarios){
        const float width=scenario.width;const auto wrap=scenario.wrap;
        ++cases;system.tracking=mode&1?.03f:0;system.independent_letters=(mode&2)!=0;
        const TextStyleSpan style{0,text.bytes,flags[f]};const ConstSlice<TextStyleSpan> styles{&style,flags[f]?1u:0u};
        const auto shaped=text_shape_utf8(&system, {text,styles,width,1000000,wrap}, &native_output.text);observe(shaped);
        CHECK(!required||(shaped.ok()&&native_output.text.native_fonts()));if(required)++native_cases;
        if(shaped.ok())observe(native_output.text,text,system);
        const auto a=text_measure_utf8(&system, {text,styles,width,1000000,wrap});observe_measure(a);
        CHECK(a.error==shaped.error&&(!a.ok()||same_metrics(a,native_output.text)));
        const TextHitSpan hit{0,text.bytes,65535,0};
        const auto hits=text_shape_utf8(&system, {text,styles,width,1000000,wrap,TextOverflow::clip,{&hit,1}}, &native_output.text);observe(hits);
        CHECK(!required||(hits.ok()&&native_output.text.native_fonts()));if(hits.ok())observe(native_output.text,text,system);
        if(!hits.ok())continue;
        const u32 units=native_output.text.cluster_count;
        // The app's editor resets tracking/independent typography before shaping.
        for(u32 caret=0;caret<=units;++caret){
            const TextEditLayoutInput input{text,text_edit_signature(text),0,caret,9.125f,32.25f,width,40,1000000,static_cast<u8>(wrap==TextWrapping::wrap?text_edit_layout_wrap:0),styles};
            CHECK(text_edit_layout_measure(&editor,input,system)==required);if(!required)continue;
            CHECK(editor.geometry&&observe_edit(editor));
            const auto builds=editor.builds;CHECK(text_edit_layout_measure(&editor,input,system)&&editor.builds==builds);
        }
    }
    return true;
}
// A style cut at every scalar: the styled candidate draws it unless a font-size change interrupts an emoji item (or
// the text mixes scripts); drawing and editing that the candidates decline are observed through the shipping paths.
bool emoji_dispatch_cuts(AppSystem system,Utf8View text,bool mixed=false) noexcept {
    const u32 flags[]{text_style_link,text_style_bold|text_style_italic,text_style_underline|text_style_strikethrough,text_style_monospace,text_style_heading_2};
    const float widths[]{37,1000000};
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);CHECK(cp.valid);
        for(u32 flag:flags)for(u32 tail=0;tail<(at+cp.width==text.bytes?1u:2u);++tail)for(float width:widths){++cases;
            const TextStyleSpan style{at,tail?text.bytes-at:cp.width,flag};const ConstSlice<TextStyleSpan> styles{&style,1};
            shaped_text_reset(&native_output.text);const bool native=native_text_candidate_styled(system,{text,styles,width,1000000,TextWrapping::wrap},&native_output.text,nullptr);
            if(!mixed&&text_style_font_size(system.em_size,flag)==system.em_size)CHECK(native);observe_value(static_cast<u32>(native));
            TextShapeResult shaped{native_output.text.cluster_count,TextShapeError::none};
            if(native)++native_cases;else{++emoji_draw_declines;shaped=text_shape_utf8(&system, {text,styles,width,1000000}, &native_output.text);observe(shaped);}
            if(shaped.ok())observe(native_output.text,text,system);
            const auto a=text_measure_utf8(&system, {text,styles,width,1000000});observe_measure(a);
            CHECK(a.error==shaped.error&&(!a.ok()||same_metrics(a,native_output.text)));
            NativeParagraphHits geometry{};SfntFontLineParts parts[513];TextMeasureResult measured{};
            NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{hit_advances,1280},&geometry,{parts,513}};
            if(native_text_candidate_hits(system,{text,styles,width,1000000,TextWrapping::wrap,TextOverflow::clip},storage,&measured))CHECK(compare_hits(system,text,width,TextWrapping::wrap,styles));
            else{++emoji_hit_declines;TextEditLayoutOwner editor{};TextEditMetric selections[48];editor.selections=selections;editor.selection_capacity=48;
                struct Release {TextEditLayoutOwner& p;~Release() noexcept {text_edit_layout_release(&p);}} release{editor};
                const TextEditLayoutInput input{text,text_edit_signature(text),0,native_output.text.cluster_count,9,32,width,40,1000000,text_edit_layout_wrap,styles};
                CHECK(!text_edit_layout_measure(&editor,input,system));}
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
        auto system=owner;
        u32 selected=0;
        for(u32 i=0;i<header[1];++i){float em=0;u32 bytes=0;char text[512];CHECK(read(&em,4)&&em>0&&em<=1000000&&read(&bytes,4)&&bytes&&bytes<=sizeof(text)&&read(text,bytes));
            system.em_size=em;CHECK(set_format(system,L"Segoe UI",em));
            TextEmojiCursor cursor{};TextEmojiRange range{};const bool whole=text_emoji_next({text,bytes},&cursor,&range)==TextEmojiNext::range&&!range.first_byte&&range.end_byte==bytes;
            bool required=mixed||whole;
            // Mixed inputs the line candidate accepts must also execute natively at all widths.
            if(!required){TextMeasureResult probe{};required=native_text_candidate_line(system,{{text,bytes},{},1000000,1000000},nullptr,&probe);}
            if(required)++selected;
            if(!emoji_dispatch_case(system,{text,bytes},false,required)){print("emoji_corpus_index=");number(i);print("\n");return false;}}
        char tail;DWORD got=0;CHECK(ReadFile(file,&tail,1,&got,nullptr)&&!got);print("emoji_inputs=");number(header[1]);print(" selected=");number(selected);print("\n");return true;
    }
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas",L"Segoe UI Emoji"};const float sizes[]{14,17.5f,1000};
    const Utf8View texts[]{utf8_literal("\xf0\x9f\x98\x80"),utf8_literal("\xf0\x9f\x98\x80\xf0\x9f\x98\x83"),
        utf8_literal("\xe2\x9d\xa4\xef\xb8\x8f"),utf8_literal("1\xef\xb8\x8f\xe2\x83\xa3"),
        utf8_literal("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"),
        utf8_literal("\xf0\x9f\x8f\xb4\xf3\xa0\x81\xa7\xf3\xa0\x81\xa2\xf3\xa0\x81\xa5\xf3\xa0\x81\xae\xf3\xa0\x81\xa7\xf3\xa0\x81\xbf")};
    for(const auto* family:families)for(float em:sizes){
        auto system=owner;system.em_size=em;CHECK(set_format(system,family,em,600,2));
        for(const auto text:texts){CHECK(emoji_dispatch_case(system,text,true));CHECK(emoji_dispatch_cuts(system,text));}
    }
    print("emoji_draw_declines=");number(emoji_draw_declines);print(" emoji_hit_declines=");number(emoji_hit_declines);print("\n");return true;
}
