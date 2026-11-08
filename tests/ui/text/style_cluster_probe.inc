bool style_cluster_edit_transitions() noexcept;
// A styled candidate shape, then the shipping shape and measure of the same request, which agree. An input the
// candidate need not draw may be declined by the shipping path too (observed).
bool styled_layout(const AppSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,TextWrapping wrap,bool required,bool* admitted=nullptr) noexcept {
    shaped_text_reset(&native_output.text);const bool native=native_text_candidate_styled(system,{text,styles,width,1000000,wrap},&native_output.text,nullptr);
    CHECK(native||!required);observe_value(static_cast<u32>(native));if(native)++native_cases;if(admitted)*admitted=native;
    auto shaping=static_cast<TextSystem>(system);const auto shaped=text_shape_utf8(&shaping,{text,styles,width,1000000,wrap},&native_output.text);observe(shaped);
    if(!shaped.ok()){CHECK(!native);return true;}
    CHECK(native_output.text.native_fonts());observe(native_output.text,text,system);
    const auto measured=text_measure_utf8(&shaping,{text,styles,width,1000000,wrap});observe_measure(measured);
    CHECK(measured.ok()&&same_metrics(measured,native_output.text));return true;
}
bool inspect_style_clusters() noexcept {
    const Utf8View texts[]{utf8_literal("ffi"),utf8_literal("office"),utf8_literal("Ae\xcc\x81V"),utf8_literal("A\xcc\x81\xcc\xa3V"),utf8_literal("A\xcc\x81\xcc\x81V"),utf8_literal("A\r\nV"),utf8_literal("A\xe2\x80\x8dV"),utf8_literal("e\xcc\x81"),utf8_literal("e\xcc\x81\xcc\x88")};
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas"};
    const u32 flags[]{text_style_link,text_style_syntax_keyword,text_style_underline,text_style_strikethrough,text_style_bold,text_style_italic,text_style_heading_1,text_style_monospace,text_style_spoiler,text_style_mention};
    for(u32 family=0;family<3;++family){auto system=owner;CHECK(set_format(system,families[family],16));
        for(u32 sample=0;sample<9;++sample){const auto text=texts[sample];u32 offsets[32],count=0;
            for(u32 at=0;at<text.bytes;){offsets[count++]=at;const auto cp=utf8_decode_one(text.data+at,text.bytes-at);CHECK(cp.valid);at+=cp.width;}offsets[count]=text.bytes;
            for(u32 first=0;first<count;++first)for(u32 last=first+1;last<=count;++last)for(u32 flag=0;flag<10;++flag){++cases;const TextStyleSpan style{offsets[first],offsets[last]-offsets[first],flags[flag]};
                // Some selected styles lack a ZWJ cmap entry, so a separate fallback/control item is itemized: that
                // font-itemization boundary is independent of style cuts.
                const bool required=family==0&&sample!=6&&(sample!=5||(first!=2&&last!=2));
                if(!styled_layout(system,text,{&style,1},1000000,TextWrapping::wrap,required)){
                    print("cluster style family/sample/first/last/flag=");number(family);print("/");number(sample);print("/");number(first);print("/");number(last);print("/");number(flag);print("\n");return false;}
            }
        }
    }return style_cluster_edit_transitions();
}
bool style_cluster_corpus(AppSystem system,bool required_font) noexcept {
    const Utf8View texts[]{utf8_literal("ffi"),utf8_literal("office"),utf8_literal("Ae\xcc\x81V"),utf8_literal("A\xcc\x81\xcc\xa3V"),utf8_literal("A\xcc\x81\xcc\x81V"),utf8_literal("A\r\nV"),utf8_literal("A\xe2\x80\x8dV"),utf8_literal("e\xcc\x81"),utf8_literal("e\xcc\x81\xcc\x88"),
        utf8_literal("e\xcc\x81 A\tA\xcc\x81\nV"),utf8_literal("A\xcd\x8f\xcc\x81V"),utf8_literal("A\xe2\x80\x8c\xcc\x81V"),utf8_literal("A\xe2\x80\x8d\xcc\x81V")};
    const u32 flags[]{text_style_link,text_style_syntax_keyword,text_style_underline,text_style_strikethrough,text_style_bold,text_style_italic,text_style_heading_1,text_style_monospace,text_style_spoiler,text_style_mention};
    struct Scenario {float width;TextWrapping wrap;};
    const Scenario scenarios[]{{8,TextWrapping::wrap},{37,TextWrapping::wrap},{1000000,TextWrapping::no_wrap}};
    struct ScaleCase {u32 sample,first,last,flag;};
    const ScaleCase scale_cases[]{{2,2,3,text_style_underline},{0,1,2,text_style_bold},{3,0,3,text_style_heading_1}};
    for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample){const auto text=texts[sample];u32 offsets[32],count=0;
        for(u32 at=0;at<text.bytes;){offsets[count++]=at;const auto cp=utf8_decode_one(text.data+at,text.bytes-at);CHECK(cp.valid);at+=cp.width;}offsets[count]=text.bytes;
        const auto execute=[&](u32 first,u32 last,u32 flag,u32 mode,const Scenario& scenario) noexcept {
            const float width=scenario.width;const auto wrap=scenario.wrap;const bool nowrap=wrap==TextWrapping::no_wrap;
            ++cases;system.independent_letters=(mode&1)!=0;system.fixed_weight=(mode&2)!=0;
            const TextStyleSpan style{offsets[first],offsets[last]-offsets[first],flag};
            const bool required=required_font&&sample<10&&sample!=6&&(sample!=5||(first!=2&&last!=2));
            bool native=false;
            if(!styled_layout(system,text,{&style,1},width,wrap,required,&native)){
                print("cluster sample/first/last/flag/mode/width/nowrap/em=");number(sample);print("/");number(first);print("/");number(last);print("/");number(flag);print("/");number(mode);print("/");real(width);print("/");number(nowrap);print("/");real(system.em_size);print("\n");return false;}
            // Drawing and retained editing have distinct admission policies.
            // Every admitted editor layout still receives raw-range checks.
            if(native&&!system.independent_letters){NativeParagraphHits editor_hits{};SfntFontLineParts parts[3];NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{hit_advances,1280},&editor_hits,{parts,3}};TextMeasureResult measured{};
                if(native_text_candidate_hits(system,{text,{&style,1},width,1000,wrap,TextOverflow::clip},storage,&measured))CHECK(compare_hits(system,text,width,wrap,{&style,1}));}
            return true;
        };
        if(system.em_size==17.5f){for(u32 first=0;first<count;++first)for(u32 last=first+1;last<=count;++last)for(u32 flag:flags)for(u32 mode=0;mode<4;++mode)for(const auto& scenario:scenarios)CHECK(execute(first,last,flag,mode,scenario));}
        else for(const auto& scale:scale_cases)if(scale.sample==sample)CHECK(execute(scale.first,scale.last,scale.flag,0,scenarios[1]));
    }return true;
}
bool style_cluster_edit_transitions() noexcept {
    TextEditLayoutOwner layout{};TextEditMetric selections[48];layout.selections=selections;layout.selection_capacity=48;
    struct Release {TextEditLayoutOwner& p;~Release() noexcept {text_edit_layout_release(&p);}} release{layout};
    const auto text=utf8_literal("Ae\xcc\x81V");const TextStyleSpan styles[]{{0,text.bytes,text_style_link},{2,2,text_style_underline},{1,1,text_style_bold},{2,2,text_style_heading_1},{0,text.bytes,text_style_link}};
    for(u32 style=0;style<5;++style)for(u32 options=0;options<8;++options)for(u32 caret=0;caret<=4;++caret){++cases;
        const TextEditLayoutInput input{text,text_edit_signature(text),caret,4,9.125f,32.25f,13,40,1000000,static_cast<u8>(options),{styles+style,1}};
        // The native editor admits whole-text links and a weight change on the base letter alone; a decoration or
        // size change that starts on the combining mark inside the cluster is declined.
        const bool admitted=style!=1&&style!=3;
        CHECK(text_edit_layout_measure(&layout,input,owner)==admitted);if(!admitted)continue;
        CHECK(layout.geometry&&observe_edit(layout));++native_cases;
        const auto builds=layout.builds;CHECK(text_edit_layout_measure(&layout,input,owner)&&layout.builds==builds&&observe_edit(layout));
    }return true;
}
bool style_continuity_corpus(AppSystem system) noexcept {
    const Utf8View texts[]{utf8_literal("office"),utf8_literal("ce"),utf8_literal("AV"),utf8_literal("To"),utf8_literal("AV office ce"),utf8_literal("a 12/34 ce"),utf8_literal("fi ce fi"),utf8_literal("ceffice")};
    const u32 flags[]{text_style_link,text_style_underline,text_style_strikethrough,text_style_spoiler};const float widths[]{8.f,37.f,1000000.f};
    for(u32 sample=0;sample<8;++sample){const auto text=texts[sample];
        for(u32 first=0;first<text.bytes;++first)for(u32 last=first+1;last<=text.bytes;++last)for(u32 flag:flags)for(u32 split=0;split<2;++split)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            ++cases;TextStyleSpan spans[2]{{first,last-first,flag},{last,text.bytes-last,text_style_syntax_keyword}};const u32 count=split&&last<text.bytes?2u:1u;const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            shaped_text_reset(&native_output.text);const bool native=native_text_candidate_styled(system,{text,{spans,count},width,1000000,wrap},&native_output.text,nullptr);
            const auto measured=text_measure_utf8(&system, {text,{spans,count},width,1000000,wrap});
            if(!native||!measured.ok()||!same_metrics(measured,native_output.text)){
                print("continuity sample/first/last/flag/split/width/nowrap/em=");number(sample);print("/");number(first);print("/");number(last);print("/");number(flag);print("/");number(split);print("/");real(width);print("/");number(nowrap);print("/");real(system.em_size);print("\n");return false;}
            ++native_cases;observe(native_output.text,text,system);observe_measure(measured);
        }
    }return true;
}
