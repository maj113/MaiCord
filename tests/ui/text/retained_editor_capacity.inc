bool retained_editor_capacity(u32 sample) noexcept {
    const Utf8View seeds[]{utf8_literal("office AV text "),utf8_literal("한글 日本語 "),utf8_literal("abc אבג xyz "),
        utf8_literal("hello 😀 world "),utf8_literal("ภาษาไทย สวัสดีครับ "),utf8_literal("\n")};
    CHECK(sample<6);TextSystem native{};CHECK(text_system_initialize(&native,L"Segoe UI",16).ok());
    struct TextRelease {TextSystem& s;~TextRelease() noexcept {text_system_release(&s);}} text_release{native};
    TextEditLayoutOwner editor{};TextEditMetric selections[48];editor.selections=selections;editor.selection_capacity=48;
    struct Release {TextEditLayoutOwner& p;~Release() noexcept {text_edit_layout_release(&p);}} release{editor};
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
            CHECK(text_edit_layout_measure(&editor,input,native)&&editor.geometry&&observe_edit(editor));++native_cases;
            const auto builds=editor.builds;const auto counts=native_text_candidate_counts(native);const auto committed=editor.storage.arena.committed;
            CHECK(text_edit_layout_measure(&editor,input,native)&&editor.builds==builds);
            CHECK(native_text_candidate_counts(native).paragraphs_measured==counts.paragraphs_measured);
            // Caret/selection changes retain shape and font-independent geometry.
            input.anchor_utf16=units>12?units-12:0;
            CHECK(text_edit_layout_measure(&editor,input,native)&&observe_edit(editor));
            CHECK(native_text_candidate_counts(native).paragraphs_measured==counts.paragraphs_measured);
            CHECK(editor.storage.arena.committed==committed);
        }
    }
    print("retained-editor reserved/committed=");number(editor.storage.arena.reserved);print("/");number(editor.storage.arena.committed);print("\n");
    const Utf8View transitions[]{utf8_literal("A😀b"),{},utf8_literal("AV\nTo")};
    for(const auto text:transitions){
        TextEditLayoutInput input{text,text_edit_signature(text),0,0,1,2,95,40,1000000,7,{}};
        CHECK(text_edit_layout_measure(&editor,input,native)&&observe_edit(editor));
        CHECK(!text.bytes||editor.geometry);
    }
    text_bytes[kTextEditTextCapacity-1]=static_cast<char>(0xf0);
    TextEditLayoutInput malformed{{text_bytes,kTextEditTextCapacity},1,0,0,0,0,240,80,1000000,0,{}};
    CHECK(!text_edit_layout_measure(&editor,malformed,native)&&!editor.active);
    TextEditLayoutInput too_large{{text_bytes,kTextEditTextCapacity+1},1,0,0,0,0,240,80,1000000,0,{}};
    CHECK(!text_edit_layout_measure(&editor,too_large,native)&&!editor.active);
    return true;
}
