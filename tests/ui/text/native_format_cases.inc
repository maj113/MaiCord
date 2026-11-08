bool native_output_capacity_corpus() noexcept {
    TextSystem native{};CHECK(text_system_initialize(&native,L"Segoe UI",16).ok());
    struct Release {TextSystem& s;~Release() noexcept {shaped_text_reset(&native_output.text);text_system_release(&s);}} release{native};
    const Utf8View samples[]{utf8_literal("AV office abc"),utf8_literal("A 😀 אב\nA 😀 אב"),utf8_literal("one\ntwo\nthree")};
    for(auto text:samples)for(u32 mode=0;mode<3;++mode){
        const auto glyph_capacity=native_output.text.glyph_capacity,run_capacity=native_output.text.run_capacity,cluster_capacity=native_output.text.cluster_capacity;
        struct Restore {ShapedText& a;u32 glyphs,runs,clusters;~Restore() noexcept {a.glyph_capacity=glyphs;a.run_capacity=runs;a.cluster_capacity=clusters;}} restore{native_output.text,glyph_capacity,run_capacity,cluster_capacity};
        if(mode==0)native_output.text.glyph_capacity=1;
        if(mode==1)native_output.text.run_capacity=1;
        if(mode==2)native_output.text.cluster_capacity=1;
        const auto a=text_shape_utf8(&native, {text,{},45,1000}, &native_output.text);
        CHECK(a.error==TextShapeError::output_too_small);
        CHECK(!native_output.text.glyph_count&&!native_output.text.run_count&&!native_output.text.native_fonts());
        native_output.text.glyph_capacity=glyph_capacity;native_output.text.run_capacity=run_capacity;native_output.text.cluster_capacity=cluster_capacity;
        CHECK(text_shape_utf8(&native, {text,{},45,1000}, &native_output.text).ok()&&native_output.text.native_fonts());observe(native_output.text,text,native);
        CHECK(!native_text_candidate_take_output_failure(native));
        shaped_text_reset(&native_output.text);++cases;++native_cases;
    }
    return true;
}
bool native_bootstrap_corpus() noexcept {
    const TextSystem empty{};TextSystem invalid{};
    CHECK(text_system_initialize(nullptr,L"Segoe UI",16).error==TextShapeError::invalid_input);
    wchar_t long_name[257];for(u32 i=0;i<256;++i)long_name[i]=L'A';long_name[256]=0;
    CHECK(text_system_initialize(&invalid,long_name,16).error==TextShapeError::invalid_input);
    CHECK(text_system_initialize(&invalid,L"Segoe UI",250001).error==TextShapeError::invalid_input);
    CHECK(bytes_equal(&invalid,&empty,sizeof(empty)));text_system_release(&invalid);text_system_release(&invalid);
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas"};const float sizes[]{16,17.5f};
    for(const auto* family:families)for(float em:sizes){TextSystem system{};
        struct Release {TextSystem& s;~Release() noexcept {shaped_text_reset(&native_output.text);text_system_release(&s);}} release{system};
        CHECK(text_system_initialize(&system,family,em).ok());
        CHECK(system.native_fonts&&system.native_text&&system.native_format&&system.em_size==em);
        const auto text=utf8_literal("AV office\tTo \xf0\x9f\x98\x80");
        CHECK(text_shape_utf8(&system, {text,{},95,1000}, &native_output.text).ok()&&native_output.text.native_fonts());observe(native_output.text,text,system);
        ++cases;++native_cases;
    }
    return true;
}
bool native_emoji_size_corpus() noexcept {
    TextSystem native{};CHECK(text_system_initialize(&native,L"Segoe UI",16).ok());
    struct Release {TextSystem& s;~Release() noexcept {shaped_text_reset(&native_output.text);text_system_release(&s);}} release{native};
    const Utf8View samples[]{utf8_literal("💯"),utf8_literal("😀"),utf8_literal("👨‍👩‍👧‍👦"),utf8_literal("👩🏽‍🚀"),utf8_literal("🇸🇮"),utf8_literal("1️⃣"),utf8_literal("❤️"),utf8_literal("🏳️‍🌈"),utf8_literal("©️"),utf8_literal("A 😀 V"),utf8_literal("אב 😀"),utf8_literal("😀\n💯")};
    const u32 flags[]{text_style_picker_emoji,text_style_jumbo_emoji,text_style_picker_emoji|text_style_jumbo_emoji,text_style_picker_emoji|text_style_bold|text_style_italic,text_style_jumbo_emoji|text_style_underline};
    const float widths[]{28,130},sizes[]{14,16,24};
    for(float em:sizes){
        const u16 family[]{'S','e','g','o','e',' ','U','I'};
        CHECK(native_text_format_create(native.native_text,{{family,8},0,{400,5,0},em,em*4,true},&native.native_format));
        native.em_size=em;
        for(auto text:samples)for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            ++cases;const TextStyleSpan style{0,text.bytes,flag};const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            CHECK(layout(native,text,{&style,1},width,wrap,1000));++native_cases;
            const TextHitSpan hit{0,text.bytes,65535,0};
            CHECK(text_shape_utf8(&native, {text,{&style,1},width,1000,wrap,TextOverflow::clip,{&hit,1}}, &native_output.text).ok()&&native_output.text.native_fonts());
            observe(native_output.text,text,native);shaped_text_reset(&native_output.text);
        }
    }
    return true;
}
bool native_custom_emoji_corpus() noexcept {
    TextSystem native{};CHECK(text_system_initialize(&native,L"Segoe UI",16).ok());
    struct Release {TextSystem& s;~Release() noexcept {shaped_text_reset(&native_output.text);text_system_release(&s);}} release{native};
    const Utf8View samples[]{utf8_literal("A ￼ B"),utf8_literal("אב ￼ גד"),utf8_literal("😀 ￼\nV")};
    const u32 flags[]{text_style_custom_emoji,text_style_custom_emoji|text_style_jumbo_emoji,text_style_custom_emoji|text_style_picker_emoji};
    const float widths[]{16,45,130};
    for(auto text:samples){u32 first=0;while(first<text.bytes&&static_cast<u8>(text.data[first])!=0xEF)++first;CHECK(first+3<=text.bytes);
        for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            ++cases;const TextStyleSpan style{first,3,flag};const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            const TextHitSpan hit{first,3,65535,0};
            CHECK(text_shape_utf8(&native, {text,{&style,1},width,1000,wrap,TextOverflow::clip,{&hit,1}}, &native_output.text).ok()&&native_output.text.native_fonts());++native_cases;
            observe(native_output.text,text,native);
            const auto a=text_measure_utf8(&native, {text,{&style,1},width,1000,wrap});observe_measure(a);
            CHECK(a.ok()&&same_metrics(a,native_output.text));
            shaped_text_reset(&native_output.text);
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
    TextEditLayoutOwner editors[2]{};TextEditMetric selections[2][48];for(u32 i=0;i<2;++i){editors[i].selections=selections[i];editors[i].selection_capacity=48;}
    struct LayoutRelease {TextEditLayoutOwner* p;~LayoutRelease() noexcept {for(u32 i=0;i<2;++i)text_edit_layout_release(p+i);}} layout_release{editors};
    for(const auto* family:families)for(float em:sizes)for(float stop:stops){
        auto system=owner;system.em_size=em;CHECK(set_format(system,family,em,400,0,stop));
        const auto stable_text=utf8_literal("AV\tTo");const TextEditLayoutInput stable{stable_text,text_edit_signature(stable_text),0,5,0,0,1000000,40,1000000,0,{}};
        const auto format_builds=editors[1].builds;
        CHECK(text_edit_layout_measure(editors+1,stable,system)&&editors[1].builds==format_builds+1&&editors[1].geometry&&observe_edit(editors[1]));
        for(const auto text:texts)for(u32 flag:flags)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){
            ++cases;const TextStyleSpan style{0,flag==text_style_link?1u:text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            CHECK(layout(system,text,styles,width,wrap));++native_cases;
            const TextHitSpan hit{0,text.bytes,65535,0};
            CHECK(text_shape_utf8(&system, {text,styles,width,1000000,wrap,TextOverflow::clip,{&hit,1}}, &native_output.text).ok()&&native_output.text.native_fonts());
            observe(native_output.text,text,system);
            const u32 units=native_output.text.cluster_count;
            // A persistent owner alternates format handles. Include caret-only cache reuse.
            for(u32 caret=0;caret<=units;caret+=units?units:1){
                const TextEditLayoutInput input{text,text_edit_signature(text),0,caret,9.125f,32.25f,width,40,1000000,static_cast<u8>(nowrap?0:text_edit_layout_wrap),styles};
                CHECK(text_edit_layout_measure(editors,input,system)&&editors[0].geometry&&observe_edit(editors[0]));
                const auto builds=editors[0].builds;CHECK(text_edit_layout_measure(editors,input,system)&&editors[0].builds==builds);
            }
        }
    }
    // Recreated contexts start again at local handle one. Their external cache
    // keys must still differ, including when the allocator reuses an address.
    u64 previous_identity=0;
    for(u32 generation=0;generation<2;++generation){
        TextSystem recreated{};recreated.native_fonts=owner.native_fonts;recreated.em_size=16;
        CHECK(native_text_context_initialize(&recreated.native_text,recreated.native_fonts));
        struct ContextRelease {TextSystem& s;~ContextRelease() noexcept {native_text_context_release(&s.native_text);}} context_release{recreated};
        const wchar_t* family=families[generation];u16 name[32];u32 count=0;while(family[count]){name[count]=static_cast<u16>(family[count]);++count;}
        const float tab=generation?64.f:3.5f;
        CHECK(native_text_format_create(recreated.native_text,{{name,count},0,{400,5,0},16,tab,true},&recreated.native_format)&&recreated.native_format==1);
        const auto identity=native_text_format_identity(recreated);CHECK(identity&&identity!=previous_identity);previous_identity=identity;
        const auto text=utf8_literal("AV\tTo");const TextEditLayoutInput input{text,text_edit_signature(text),0,5,0,0,95,40,1000000,0,{}};
        const auto builds=editors[1].builds;++cases;
        CHECK(text_edit_layout_measure(editors+1,input,recreated)&&editors[1].geometry&&editors[1].builds==builds+1&&observe_edit(editors[1]));++native_cases;
    }
    CHECK(text_shape_utf8(&owner, {utf8_literal("a\xf0\x9f\x98\x80"),{},100,100}, &native_output.text).ok());
    CHECK(native_output.text.native_fonts()&&native_output.text.run_count==2&&native_output.text.cluster_count==3);observe(native_output.text,utf8_literal("a\xf0\x9f\x98\x80"),owner);return true;
}
