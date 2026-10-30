bool retained_editor_capacity(u32 sample) noexcept {
    const Utf8View seeds[]{utf8_literal("office AV text "),utf8_literal("한글 日本語 "),utf8_literal("abc אבג xyz "),
        utf8_literal("hello 😀 world "),utf8_literal("ภาษาไทย สวัสดีครับ "),utf8_literal("\n")};
    CHECK(sample<6);DWriteTextSystem native{};CHECK(text_system_initialize(static_cast<TextSystem*>(&native),L"Segoe UI",16).ok());
    struct TextRelease {DWriteTextSystem& s;~TextRelease() noexcept {text_system_release(&s);}} text_release{native};
    CHECK(!native.factory&&!native.format&&!native.emoji_font_policy);
    auto reference=owner;reference.native_text=nullptr;
    DWriteEditLayoutOwner layouts[2]{};TextEditMetric selections[2][48];
    for(u32 i=0;i<2;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
    struct Release {DWriteEditLayoutOwner* p;~Release() noexcept {text_edit_layout_release(p);text_edit_layout_release(p+1);}} release{layouts};
    char text_bytes[kTextEditTextCapacity+1];const u32 lengths[]{4097,8193,16000,kTextEditTextCapacity};
    for(u32 length:lengths){u32 at=0;while(seeds[sample].bytes<=length-at){copy_bytes(text_bytes+at,seeds[sample].data,seeds[sample].bytes);at+=seeds[sample].bytes;}
        while(at<length)text_bytes[at++]='a';text_bytes[length]=0;const Utf8View text{text_bytes,length};u32 units=0;
        CHECK(utf8_utf16_position_for_byte_offset(text,length,&units));
        u32 cut=length/2;while((static_cast<u8>(text_bytes[cut])&0xc0)==0x80)++cut;
        const TextStyleSpan style{cut,length-cut,text_style_bold};
        for(u32 styled=0;styled<2;++styled)for(u32 wrap=0;wrap<2;++wrap){++cases;
            print("retained-editor sample/bytes/styled/wrap=");number(sample);print("/");number(length);print("/");number(styled);print("/");number(wrap);print("\n");
            TextEditLayoutInput input{text,text_edit_signature(text),units,units,9.125f,32.25f,240,80,1000000,
                static_cast<u8>(text_edit_layout_scroll_to_caret|(wrap?text_edit_layout_wrap:0)),{&style,styled}};
            CHECK(text_edit_layout_measure(layouts,input,native)&&layouts[0].geometry&&!layouts[0].layout);
            CHECK(text_edit_layout_measure(layouts+1,input,reference)&&same_edit(layouts[0],layouts[1]));++native_cases;
            const auto builds=layouts[0].builds;const auto counts=native_text_candidate_counts(native);const auto committed=layouts[0].storage.arena.committed;
            CHECK(text_edit_layout_measure(layouts,input,native)&&layouts[0].builds==builds);
            CHECK(native_text_candidate_counts(native).paragraphs_measured==counts.paragraphs_measured);
            // Caret/selection changes retain shape and font-independent geometry.
            input.anchor_utf16=units>12?units-12:0;
            CHECK(text_edit_layout_measure(layouts,input,native)&&text_edit_layout_measure(layouts+1,input,reference)&&same_edit(layouts[0],layouts[1]));
            CHECK(native_text_candidate_counts(native).paragraphs_measured==counts.paragraphs_measured);
            CHECK(layouts[0].storage.arena.committed==committed);
        }
    }
    print("retained-editor reserved/committed=");number(layouts[0].storage.arena.reserved);print("/");number(layouts[0].storage.arena.committed);print("\n");
    const Utf8View transitions[]{utf8_literal("A😀b"),{},utf8_literal("AV\nTo")};
    for(const auto text:transitions){
        TextEditLayoutInput input{text,text_edit_signature(text),0,0,1,2,95,40,1000000,7,{}};
        CHECK(text_edit_layout_measure(layouts,input,native)&&text_edit_layout_measure(layouts+1,input,reference)&&same_edit(layouts[0],layouts[1]));
        CHECK(!layouts[0].layout&&(!text.bytes||layouts[0].geometry));
    }
    text_bytes[kTextEditTextCapacity-1]=static_cast<char>(0xf0);
    TextEditLayoutInput malformed{{text_bytes,kTextEditTextCapacity},1,0,0,0,0,240,80,1000000,0,{}};
    CHECK(!text_edit_layout_measure(layouts,malformed,native)&&!layouts[0].active);
    TextEditLayoutInput too_large{{text_bytes,kTextEditTextCapacity+1},1,0,0,0,0,240,80,1000000,0,{}};
    CHECK(!text_edit_layout_measure(layouts,too_large,native)&&!layouts[0].active);
    return true;
}
