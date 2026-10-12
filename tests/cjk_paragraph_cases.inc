// All positive calls detach platform factories/formats. Font fallback, shaping,
// fitting and interaction must complete through the candidate.
bool native_han_paragraph_corpus() noexcept {
    const wchar_t* families[]{L"Microsoft YaHei UI",L"Microsoft JhengHei UI",L"Meiryo",L"Segoe UI"};
    const Utf8View texts[]{utf8_literal("中文消息测试"),utf8_literal("  中文  日本語  "),
        utf8_literal("中文。测试、（消息）！"),utf8_literal("「中文」，『測試』？"),
        utf8_literal("中文𠀀中文"),utf8_literal("一󠄀 中文"),utf8_literal("中文\n測試"),
        utf8_literal("中文\t測試"),utf8_literal("中文 abc"),utf8_literal("中文 אב"),
        utf8_literal("中文 😀"),utf8_literal("中文　測試"),utf8_literal("中文…測試"),utf8_literal("中文́測試")};
    const float sizes[]{16,17.5f},widths[]{8,37,95,1000000},spacings[]{0,.03f};
    for(const auto* family:families)for(float em:sizes){
        IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
        struct Release {IDWriteTextFormat*& f;~Release() noexcept {com_release(f);}} release{format};
        auto system=owner;system.format=format;system.em_size=em;CHECK(native_text_format_capture(&system));
        auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
        for(float spacing:spacings)for(u32 independent=0;independent<2;++independent)for(auto text:texts)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap)for(u32 trimmed=0;trimmed<2;++trimmed){
            native.tracking=reference.tracking=spacing;native.independent_letters=reference.independent_letters=independent!=0;
            const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;const auto overflow=trimmed?TextOverflow::ellipsis:TextOverflow::clip;
            const float height=trimmed?em*2:1000000;
            const auto a=text_shape_utf8(&native,text,width,height,{utf16,256},&native_output.text,wrapping,overflow);
            const auto b=text_shape_utf8(&reference,text,width,height,{utf16,256},&reference_output.text,wrapping,overflow);
            if(!a.ok()||!b.ok()||!native_output.text.native_fonts()||!same(native_output,reference_output,true)){
                print("Han em/spacing/independent/width/nowrap/trimmed=");real(em);print("/");real(spacing);print("/");number(independent);print("/");real(width);print("/");number(nowrap);print("/");number(trimmed);print("\n");return false;}
            ++cases;++native_cases;
        }
    }return true;
}
u32 cjk_variant_serial=0;
bool native_cjk_paragraph_corpus(DWriteTextSystem system,bool editor) noexcept {
    const Utf8View texts[]{
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\xe6\xb6\x88\xe6\x81\xaf\xe6\xb5\x8b\xe8\xaf\x95"),
        utf8_literal("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\xa1\xe3\x83\x83\xe3\x82\xbb\xe3\x83\xbc\xe3\x82\xb8"),
        utf8_literal("\xe3\x82\xab\xe3\x82\xbf\xe3\x82\xab\xe3\x83\x8a\x20\xe3\x81\xb2\xe3\x82\x89\xe3\x81\x8c\xe3\x81\xaa"),
        utf8_literal("\xe3\x81\x8c\xe3\x81\x8e\xe3\x81\x90\xe3\x81\x92\xe3\x81\x94\x20\xe3\x81\xb1\xe3\x81\xb4\xe3\x81\xb7\xe3\x81\xba\xe3\x81\xbd"),
        utf8_literal("\xe3\x81\x8b\xe3\x82\x99\x20\xe3\x81\xaf\xe3\x82\x9a\x20\xe3\x82\xab\xe3\x82\x99\x20\xe3\x83\x8f\xe3\x82\x9a"),
        utf8_literal("\xef\xbd\xb6\xef\xbe\x9e\x20\xef\xbe\x8a\xef\xbe\x9f\x20\xef\xbd\xb6\xef\xbe\x9e\xef\xbd\xb6\xef\xbe\x9e"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\x0a\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\x0d\x0a\x0a\xe3\x82\xab\xe3\x83\x8a"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\x09\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"),
        utf8_literal("\x41\x20\xe4\xb8\xad\xe6\x96\x87\x20\x42\x20\xe3\x82\xab\xe3\x83\x8a\x20\x43"),
        utf8_literal("\xef\xbc\x88\xe4\xb8\xad\xe6\x96\x87\xef\xbc\x89\xe3\x80\x8c\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x80\x8d\x20\x31\x32\x33"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\x20\xf0\x9f\x98\x80\x20\xe3\x82\xab\xe3\x83\x8a\x20\xe2\x9d\xa4\xef\xb8\x8f"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\x20\xd7\x90\xd7\x91\xd7\x92\x20\xe3\x82\xab\xe3\x83\x8a"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\x20\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7\x20\xe3\x82\xab\xe3\x83\x8a"),
        utf8_literal("\xe3\x81\x8b\xe3\x82\x99\xe3\x81\x8b\x20\xe3\x82\xab\xe3\x82\x99\xe3\x82\xab\x20\xef\xbd\xb6\xef\xbe\x9e\xef\xbd\xb6"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\xf0\xa0\x80\x80\xe4\xb8\xad\xe6\x96\x87"),
        utf8_literal("\xe4\xb8\x80\xf3\xa0\x84\x80\x20\xe4\xb8\xad\xe6\x96\x87"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\x20\x45\x6e\x67\x6c\x69\x73\x68\x20\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac\x20\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9"),
        utf8_literal("\x20\x20\xe4\xb8\xad\xe6\x96\x87\x20\x20\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\x20\x20"),
        utf8_literal("\xe4\xb8\xad\xe6\x96\x87\xe3\x80\x82\xe6\xb5\x8b\xe8\xaf\x95\xe3\x80\x81\xef\xbc\x88\xe6\xb6\x88\xe6\x81\xaf\xef\xbc\x89\xef\xbc\x81"),
        utf8_literal("\xe3\x82\xab\xe2\x80\x8d\xe3\x82\x99\x20\xe3\x82\xab\xe2\x80\x8c\xe3\x82\x99\x20\xe3\x82\xab\xcd\x8f\xe3\x82\x99")};
    const float widths[]{8,37,95,1000000};
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_underline,text_style_heading_3,text_style_monospace};
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    u32 sample=0,failures=0,selected=0xffffffffu;wchar_t selection[4]{};
    const auto selection_length=GetEnvironmentVariableW(L"MAICORD_CJK_SAMPLE",selection,4);
    if(selection_length){CHECK(selection_length<=2);selected=0;for(u32 i=0;i<selection_length;++i){CHECK(selection[i]>='0'&&selection[i]<='9');selected=selected*10+selection[i]-'0';}CHECK(selected<sizeof(texts)/sizeof(texts[0]));}
    u32 variant=0xffffffffu;wchar_t variant_text[8]{};
    const auto variant_length=GetEnvironmentVariableW(L"MAICORD_CJK_VARIANT",variant_text,8);
    if(variant_length){CHECK(selected!=0xffffffffu&&variant_length<8);variant=0;
        for(u32 i=0;i<variant_length;++i){CHECK(variant_text[i]>='0'&&variant_text[i]<='9');variant=variant*10+variant_text[i]-'0';}CHECK(variant<(editor?288u:576u));}
    for(const auto text:texts){if(selected!=0xffffffffu&&selected!=sample){++sample;continue;}for(u32 independent=0;independent<(editor?1u:2u);++independent)for(u32 pattern=0;pattern<2;++pattern)for(u32 flag:flags)for(u32 nowrap=0;nowrap<2;++nowrap)for(float width:widths){
        const auto ordinal=cjk_variant_serial++;if(variant!=0xffffffffu&&ordinal!=variant)continue;
        u32 first=0;if(pattern){const auto cp=utf8_decode_one(text.data,text.bytes);CHECK(cp.valid);first=cp.width;}
        const TextStyleSpan span{first,text.bytes-first,flag};const ConstSlice<TextStyleSpan> styles{&span,flag?1u:0u};
        native.independent_letters=reference.independent_letters=independent!=0;const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
        if(editor){if(!compare_hits(system,text,width,wrapping,styles,true)){print("CJK hit sample/pattern/flag/width/nowrap=");number(sample);print("/");number(pattern);print("/");number(flag);print("/");real(width);print("/");number(nowrap);print("\n");++failures;}continue;}
        ++cases;const auto a=text_shape_utf8_styled(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrapping);
        const auto b=text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrapping);
        if(!a.ok()||!native_output.text.native_fonts()||!b.ok()||!same(native_output,reference_output,true)){
            print("CJK paragraph sample/pattern/flags/width/independent/nowrap=");number(sample);print("/");number(pattern);print("/");number(flag);print("/");real(width);print("/");number(independent);print("/");number(nowrap);print(" errors=");number(static_cast<u32>(a.error));print("/");number(static_cast<u32>(b.error));print("\n");++failures;continue;
        }++native_cases;
        const auto am=text_measure_utf8_styled(&native,text,styles,width,1000000,{utf16,256},wrapping),bm=text_measure_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},wrapping);
        if(!am.ok()||!bm.ok()||!bytes_equal(&am.width,&bm.width,12)||am.line_count!=bm.line_count){++failures;print("CJK measurement failed sample=");number(sample);print("\n");}

    }++sample;}print("CJK failed cases=");number(failures);print("\n");return failures==0;
}

// Paint-only cuts must not create new editor geometry boundaries.
bool native_cjk_editor_paint_corpus(DWriteTextSystem system) noexcept {
    const Utf8View texts[]{utf8_literal("\xef\xbc\x88\xe4\xb8\xad\xe6\x96\x87\xef\xbc\x89\xe3\x80\x8c\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x80\x8d\x20\x31\x32\x33"),utf8_literal("\xe4\xb8\xad\xe6\x96\x87\xe3\x80\x82\xe6\xb5\x8b\xe8\xaf\x95\xe3\x80\x81\xef\xbc\x88\xe6\xb6\x88\xe6\x81\xaf\xef\xbc\x89\xef\xbc\x81"),utf8_literal("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\xa1\xe3\x83\x83\xe3\x82\xbb\xe3\x83\xbc\xe3\x82\xb8"),utf8_literal("\xe4\xb8\xad\xe6\x96\x87\xe6\xb6\x88\xe6\x81\xaf\xe6\xb5\x8b\xe8\xaf\x95")};
    const u32 paints[]{text_style_link,text_style_syntax_keyword,text_style_mention};
    const float widths[]{37,1000000};u32 failures=0,sample=0,selected=0xffffffffu;wchar_t selection[4]{};
    const auto selection_length=GetEnvironmentVariableW(L"MAICORD_CJK_SAMPLE",selection,4);
    if(selection_length){CHECK(selection_length==1&&selection[0]>='0'&&selection[0]<='3');selected=selection[0]-'0';}
    for(const auto text:texts){if(selected!=0xffffffffu&&selected!=sample){++sample;continue;}for(u32 cut=0;cut<text.bytes;){const auto cp=utf8_decode_one(text.data+cut,text.bytes-cut);CHECK(cp.valid);cut+=cp.width;if(cut==text.bytes)break;
        for(u32 paint:paints)for(u32 heading=0;heading<2;++heading)for(u32 side=0;side<2;++side)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            const u32 base=heading?text_style_heading_3:0;TextStyleSpan spans[2]{};u32 count=0;
            if(base||side==0)spans[count++]={0,cut,base|(side==0?paint:0)};
            if(base||side==1)spans[count++]={cut,text.bytes-cut,base|(side==1?paint:0)};
            if(!compare_hits(system,text,width,nowrap?TextWrapping::no_wrap:TextWrapping::wrap,{spans,count},true)){
                print("CJK paint sample/cut/paint/heading/side/width/nowrap=");number(sample);print("/");number(cut);print("/");number(paint);print("/");number(heading);print("/");number(side);print("/");real(width);print("/");number(nowrap);print("\n");++failures;}
        }
    }++sample;}print("CJK paint failed cases=");number(failures);print("\n");return failures==0;
}
