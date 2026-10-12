bool native_output_capacity_corpus() noexcept {
    DWriteTextSystem native{};CHECK(text_system_initialize(static_cast<TextSystem*>(&native),L"Segoe UI",16,L"en-US").ok());
    struct Release {DWriteTextSystem& s;~Release() noexcept {shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);oracle_faces_release();text_system_release(&s);}} release{native};
    auto reference=owner;reference.native_text=nullptr;
    const Utf8View samples[]{utf8_literal("AV office abc"),utf8_literal("A 😀 אב\nA 😀 אב"),utf8_literal("one\ntwo\nthree")};
    for(auto text:samples)for(u32 mode=0;mode<3;++mode){
        const auto glyph_capacity=native_output.text.glyph_capacity,run_capacity=native_output.text.run_capacity,cluster_capacity=native_output.text.cluster_capacity;
        struct Restore {ShapedText& a;ShapedText& b;u32 glyphs,runs,clusters;~Restore() noexcept {a.glyph_capacity=b.glyph_capacity=glyphs;a.run_capacity=b.run_capacity=runs;a.cluster_capacity=b.cluster_capacity=clusters;}} restore{native_output.text,reference_output.text,glyph_capacity,run_capacity,cluster_capacity};
        if(mode==0)native_output.text.glyph_capacity=reference_output.text.glyph_capacity=1;
        if(mode==1)native_output.text.run_capacity=reference_output.text.run_capacity=1;
        if(mode==2)native_output.text.cluster_capacity=reference_output.text.cluster_capacity=1;
        const auto a=text_shape_utf8(&native,text,45,1000,{utf16,256},&native_output.text);
        const auto b=text_shape_utf8(&reference,text,45,1000,{utf16,256},&reference_output.text);
        CHECK(a.error==TextShapeError::output_too_small&&b.error==TextShapeError::output_too_small);
        CHECK(!native_output.text.glyph_count&&!native_output.text.run_count&&!native_output.text.native_fonts());
        native_output.text.glyph_capacity=glyph_capacity;native_output.text.run_capacity=run_capacity;native_output.text.cluster_capacity=cluster_capacity;
        reference_output.text.glyph_capacity=glyph_capacity;reference_output.text.run_capacity=run_capacity;reference_output.text.cluster_capacity=cluster_capacity;
        CHECK(text_shape_utf8(&native,text,45,1000,{utf16,256},&native_output.text).ok());
        CHECK(text_shape_utf8(&reference,text,45,1000,{utf16,256},&reference_output.text).ok()&&same(native_output,reference_output));
        CHECK(!native_text_candidate_take_output_failure(native));
        shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);++cases;++native_cases;
    }
    return true;
}
bool native_bootstrap_corpus() noexcept {
    const DWriteTextSystem empty{};DWriteTextSystem invalid{};
    CHECK(text_system_initialize(static_cast<TextSystem*>(nullptr),L"Segoe UI",16,L"en-US").error==TextShapeError::invalid_input);
    const wchar_t* bad_locales[]{L"",L"en",L"en-GB",L"fr-FR"};
    for(const auto* locale:bad_locales){CHECK(text_system_initialize(static_cast<TextSystem*>(&invalid),L"Segoe UI",16,locale).error==TextShapeError::invalid_input);
        CHECK(bytes_equal(&invalid,&empty,sizeof(empty)));}
    wchar_t long_name[257];for(u32 i=0;i<256;++i)long_name[i]=L'A';long_name[256]=0;
    CHECK(text_system_initialize(static_cast<TextSystem*>(&invalid),long_name,16,L"en-US").error==TextShapeError::invalid_input);
    CHECK(text_system_initialize(static_cast<TextSystem*>(&invalid),L"Segoe UI",250001,L"en-US").error==TextShapeError::invalid_input);
    CHECK(bytes_equal(&invalid,&empty,sizeof(empty)));text_system_release(&invalid);text_system_release(&invalid);
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas"};const float sizes[]{16,17.5f};
    // Oracle keys borrow session addresses; expire them before a fresh session can reuse an address.
    for(const auto* family:families)for(float em:sizes){DWriteTextSystem system{};IDWriteTextFormat* format=nullptr;
        struct Release {DWriteTextSystem& s;IDWriteTextFormat*& f;~Release() noexcept {oracle_faces_release();text_system_release(&s);com_release(f);}} release{system,format};
        CHECK(text_system_initialize(static_cast<TextSystem*>(&system),family,em,L"en-US").ok());
        CHECK(!system.factory&&!system.format&&!system.emoji_font_policy&&system.native_fonts&&system.native_text&&system.native_format&&system.em_size==em);
        CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
        auto reference=owner;reference.native_text=nullptr;reference.format=format;reference.em_size=em;
        const auto text=utf8_literal("AV office\tTo \xf0\x9f\x98\x80");
        CHECK(text_shape_utf8(&system,text,95,1000,{utf16,256},&native_output.text).ok()&&native_output.text.native_fonts());
        CHECK(text_shape_utf8(&reference,text,95,1000,{utf16,256},&reference_output.text).ok()&&same(native_output,reference_output));
        shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);++cases;++native_cases;
    }
    return true;
}
bool native_emoji_size_corpus() noexcept {
    DWriteTextSystem native{};CHECK(text_system_initialize(static_cast<TextSystem*>(&native),L"Segoe UI",16,L"en-US").ok());
    struct Release {DWriteTextSystem& s;~Release() noexcept {shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);oracle_faces_release();text_system_release(&s);}} release{native};
    const Utf8View samples[]{utf8_literal("💯"),utf8_literal("😀"),utf8_literal("👨‍👩‍👧‍👦"),utf8_literal("👩🏽‍🚀"),utf8_literal("🇸🇮"),utf8_literal("1️⃣"),utf8_literal("❤️"),utf8_literal("🏳️‍🌈"),utf8_literal("©️"),utf8_literal("A 😀 V"),utf8_literal("אב 😀"),utf8_literal("😀\n💯")};
    const u32 flags[]{text_style_picker_emoji,text_style_jumbo_emoji,text_style_picker_emoji|text_style_jumbo_emoji,text_style_picker_emoji|text_style_bold|text_style_italic,text_style_jumbo_emoji|text_style_underline};
    const float widths[]{28,130},sizes[]{14,16,24};
    auto reference=owner;reference.native_text=nullptr;
    for(float em:sizes){
        IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
        struct FormatRelease {IDWriteTextFormat*& p;~FormatRelease() noexcept {com_release(p);}} format_release{format};
        const u16 family[]{'S','e','g','o','e',' ','U','I'};
        CHECK(native_text_format_create(native.native_text,{{family,8},0,{400,5,0},em,em*4,true},&native.native_format));
        native.em_size=em;reference.em_size=em;reference.format=format;
        for(auto text:samples)for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            ++cases;const TextStyleSpan style{0,text.bytes,flag};const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            CHECK(text_shape_utf8_styled(&native,text,{&style,1},width,1000,{utf16,256},&native_output.text,wrap).ok());
            CHECK(text_shape_utf8_styled(&reference,text,{&style,1},width,1000,{utf16,256},&reference_output.text,wrap).ok());
            CHECK(native_output.text.native_fonts()&&same(native_output,reference_output));++native_cases;
            const auto a=text_measure_utf8_styled(&native,text,{&style,1},width,1000,{utf16,256},wrap),b=text_measure_utf8_styled(&reference,text,{&style,1},width,1000,{utf16,256},wrap);
            CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count);
            const TextHitSpan hit{0,text.bytes,65535,0};
            CHECK(text_shape_utf8_interactive(&native,text,{&style,1},{&hit,1},width,1000,{utf16,256},&native_output.text,wrap).ok());
            CHECK(text_shape_utf8_interactive(&reference,text,{&style,1},{&hit,1},width,1000,{utf16,256},&reference_output.text,wrap).ok()&&same(native_output,reference_output));
            shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
        }
    }
    return true;
}
bool native_custom_emoji_corpus() noexcept {
    DWriteTextSystem native{};CHECK(text_system_initialize(static_cast<TextSystem*>(&native),L"Segoe UI",16,L"en-US").ok());
    struct Release {DWriteTextSystem& s;~Release() noexcept {shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);oracle_faces_release();text_system_release(&s);}} release{native};
    auto reference=owner;reference.native_text=nullptr;
    const Utf8View samples[]{utf8_literal("A ￼ B"),utf8_literal("אב ￼ גד"),utf8_literal("😀 ￼\nV")};
    const u32 flags[]{text_style_custom_emoji,text_style_custom_emoji|text_style_jumbo_emoji,text_style_custom_emoji|text_style_picker_emoji};
    const float widths[]{16,45,130};
    for(auto text:samples){u32 first=0;while(first<text.bytes&&static_cast<u8>(text.data[first])!=0xEF)++first;CHECK(first+3<=text.bytes);
        for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            ++cases;const TextStyleSpan style{first,3,flag};const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            const TextHitSpan hit{first,3,65535,0};
            CHECK(text_shape_utf8_interactive(&native,text,{&style,1},{&hit,1},width,1000,{utf16,256},&native_output.text,wrap).ok());
            CHECK(text_shape_utf8_interactive(&reference,text,{&style,1},{&hit,1},width,1000,{utf16,256},&reference_output.text,wrap).ok());
            CHECK(native_output.text.native_fonts()&&same(native_output,reference_output));++native_cases;
            const auto a=text_measure_utf8_styled(&native,text,{&style,1},width,1000,{utf16,256},wrap),b=text_measure_utf8_styled(&reference,text,{&style,1},width,1000,{utf16,256},wrap);
            CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count);
            shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
        }
    }
    return true;
}
bool native_format_corpus() noexcept {
    CHECK(native_bootstrap_corpus());
    CHECK(native_output_capacity_corpus());
    CHECK(native_emoji_size_corpus());
    CHECK(native_custom_emoji_corpus());
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas"};const float sizes[]{14,17.5f,1000},stops[]{3.5f,64,91.25f};
    const Utf8View texts[]{utf8_literal("AV office caf\xc3\xa9 To"),utf8_literal("Ae\xcc\x81V"),utf8_literal("AV\tTo\r\nAV\n"),utf8_literal("AV office ce")};
    const float widths[]{8,95,1000000};const u32 flags[]{0,text_style_underline,text_style_bold|text_style_underline,text_style_link};
    DWriteEditLayoutOwner layouts[4]{};TextEditMetric selections[4][48];for(u32 i=0;i<4;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
    struct LayoutRelease {DWriteEditLayoutOwner* p;~LayoutRelease() noexcept {for(u32 i=0;i<4;++i)text_edit_layout_release(p+i);}} layout_release{layouts};
    for(const auto* family:families)for(float em:sizes)for(float stop:stops){
        IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
        struct Release {IDWriteTextFormat*& p;~Release() noexcept {com_release(p);}} release{format};CHECK(format->GetIncrementalTabStop()==em*4);CHECK(SUCCEEDED(format->SetIncrementalTabStop(stop)));
        u16 name[256];u32 count=0;while(family[count]){name[count]=static_cast<u16>(family[count]);++count;}
        auto system=owner;system.factory=nullptr;system.format=nullptr;system.em_size=em;
        CHECK(native_text_format_create(system.native_text,{{name,count},0,{400,5,0},em,stop,true},&system.native_format));
        auto reference=owner;reference.native_text=nullptr;reference.format=format;reference.em_size=em;
        const auto stable_text=utf8_literal("AV\tTo");const TextEditLayoutInput stable{stable_text,text_edit_signature(stable_text),0,5,0,0,1000000,40,1000000,0,{}};
        const auto format_builds=layouts[2].builds;text_edit_layout_invalidate(layouts+3);
        CHECK(text_edit_layout_measure(layouts+2,stable,system)&&layouts[2].builds==format_builds+1&&layouts[2].geometry);
        CHECK(text_edit_layout_measure(layouts+3,stable,reference)&&same_edit(layouts[2],layouts[3]));
        text_edit_layout_invalidate(layouts+1);
        for(const auto text:texts)for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            ++cases;const TextStyleSpan style{0,flag==text_style_link?1u:text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            CHECK(text_shape_utf8_styled(&system,text,styles,width,1000000,{utf16,256},&native_output.text,wrap).ok());
            CHECK(native_output.text.native_fonts());++native_cases;
            CHECK(text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrap).ok());CHECK(same(native_output,reference_output));
            const auto a=text_measure_utf8_styled(&system,text,styles,width,1000000,{utf16,256},wrap),b=text_measure_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},wrap);
            CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count);
            const TextHitSpan hit{0,text.bytes,65535,0};
            CHECK(text_shape_utf8_interactive(&system,text,styles,{&hit,1},width,1000000,{utf16,256},&native_output.text,wrap).ok()&&native_output.text.native_fonts());
            CHECK(text_shape_utf8_interactive(&reference,text,styles,{&hit,1},width,1000000,{utf16,256},&reference_output.text,wrap).ok());CHECK(same(native_output,reference_output));
            const u32 units=native_output.text.cluster_count;
            // Persistent owners alternate format handles while legacy pointers
            // on the native side remain null. Include caret-only cache reuse.
            for(u32 caret=0;caret<=units;caret+=units?units:1){
                const TextEditLayoutInput input{text,text_edit_signature(text),0,caret,9.125f,32.25f,width,40,1000000,static_cast<u8>(nowrap?0:text_edit_layout_wrap),styles};
                CHECK(text_edit_layout_measure(layouts,input,system)&&layouts[0].geometry);
                CHECK(text_edit_layout_measure(layouts+1,input,reference)&&same_edit(layouts[0],layouts[1]));
                const auto builds=layouts[0].builds;CHECK(text_edit_layout_measure(layouts,input,system)&&layouts[0].builds==builds);
            }
        }
    }
    // Recreated contexts start again at local handle one. Their external cache
    // keys must still differ, including when the allocator reuses an address.
    u64 previous_identity=0;
    for(u32 generation=0;generation<2;++generation){
        DWriteTextSystem recreated{};recreated.native_fonts=owner.native_fonts;recreated.em_size=16;
        CHECK(native_text_context_initialize(&recreated.native_text,recreated.native_fonts));
        struct ContextRelease {DWriteTextSystem& s;~ContextRelease() noexcept {native_text_context_release(&s.native_text);}} context_release{recreated};
        const wchar_t* family=families[generation];u16 name[32];u32 count=0;while(family[count]){name[count]=static_cast<u16>(family[count]);++count;}
        const float tab=generation?64.f:3.5f;
        CHECK(native_text_format_create(recreated.native_text,{{name,count},0,{400,5,0},16,tab,true},&recreated.native_format)&&recreated.native_format==1);
        const auto identity=native_text_format_identity(recreated);CHECK(identity&&identity!=previous_identity);previous_identity=identity;
        IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-US",&format)));
        struct FormatRelease {IDWriteTextFormat*& p;~FormatRelease() noexcept {com_release(p);}} format_release{format};CHECK(SUCCEEDED(format->SetIncrementalTabStop(tab)));
        auto reference=owner;reference.native_text=nullptr;reference.format=format;
        const auto text=utf8_literal("AV\tTo");const TextEditLayoutInput input{text,text_edit_signature(text),0,5,0,0,95,40,1000000,0,{}};
        const auto builds=layouts[2].builds;text_edit_layout_invalidate(layouts+3);++cases;
        CHECK(text_edit_layout_measure(layouts+2,input,recreated)&&layouts[2].geometry&&layouts[2].builds==builds+1);++native_cases;
        CHECK(text_edit_layout_measure(layouts+3,input,reference)&&same_edit(layouts[2],layouts[3]));
    }
    auto unavailable=owner;CHECK(native_text_format_capture(&unavailable));unavailable.factory=nullptr;unavailable.format=nullptr;
    CHECK(text_shape_utf8(&unavailable,utf8_literal("a\xf0\x9f\x98\x80"),100,100,{utf16,256},&native_output.text).ok());
    CHECK(native_output.text.native_fonts()&&native_output.text.run_count==2&&native_output.text.cluster_count==3);return true;
}
