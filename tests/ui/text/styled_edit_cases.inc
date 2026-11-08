bool styled_edit() noexcept {
    TextEditLayoutOwner editor{};TextEditMetric selections[48];editor.selections=selections;editor.selection_capacity=48;
    struct Release {TextEditLayoutOwner& p;~Release() noexcept {text_edit_layout_release(&p);}} release{editor};
    // A required input is laid out by the native editor; any other may be declined, which is observed.
    const auto measured=[&](const TextEditLayoutInput& input,bool required=true) noexcept {
        ++cases;const bool admitted=text_edit_layout_measure(&editor,input,owner);observe_value(static_cast<u32>(admitted));
        if(required){CHECK(admitted&&editor.geometry);++native_cases;}
        if(!admitted)return true;
        const auto before=native_text_candidate_counts(owner);const auto builds=editor.builds;
        CHECK(text_edit_layout_measure(&editor,input,owner)&&editor.builds==builds&&native_text_candidate_counts(owner).paragraphs_measured==before.paragraphs_measured);
        if(input.text.bytes<=256)CHECK(shape_observed(owner,input.text,input.styles,input.width,(input.options&text_edit_layout_wrap)?TextWrapping::wrap:TextWrapping::no_wrap,input.maximum_layout_height));
        // Retained metrics must survive unrelated use of the shared shape cache.
        CHECK(text_measure_utf8(&owner, {utf8_literal("Other cached paragraph"),{},95,1000}).ok());
        CHECK(observe_edit(editor));return true;
    };
    const float widths[]{13.f,95.f,1000.f};
    for(u32 sample=0;sample<6;++sample)for(u32 style=0;style<8;++style){const auto text=styled_hit_texts[sample];TextStyleSpan spans[3];const u32 count=styled_hit_styles(text,style,spans);u32 units=0;
        CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));
        for(float width:widths)for(u32 options=0;options<8;++options){TextEditLayoutInput input{text,text_edit_signature(text),0,0,9.125f,32.25f,width,40,1000000,static_cast<u8>(options),{spans,count}};
            for(u32 p=0;p<=units;++p){input.caret_utf16=p;if(!measured(input)){print("styled editor sample/style/width/options/position=");number(sample);print("/");number(style);print("/");real(width);print("/");number(options);print("/");number(p);print("\n");return false;}}
            input.anchor_utf16=units;input.caret_utf16=0;CHECK(measured(input));
            CHECK(text_edit_layout_scroll(&editor,-33)&&observe_edit(editor));
            CHECK(text_edit_layout_scroll(&editor,21)&&observe_edit(editor));
        }
    }
    const Utf8View switches[]{utf8_literal("AV office fi AV"),utf8_literal("AV office \xf0\x9f\x98\x80"),utf8_literal("AV office fi AV"),{},utf8_literal("AV office\n\n")};
    for(u32 i=0;i<5;++i){const auto text=switches[i];const TextStyleSpan span{0,text.bytes,text_style_heading_1};TextEditLayoutInput input{text,text_edit_signature(text),0,0,1,2,95,40,1000000,7,{&span,text.bytes?1u:0u}};
        CHECK(measured(input,i==0||i==2||i==4));}
    char long_text[4097];for(u32 i=0;i<4097;++i)long_text[i]=i%3==2?'\n':i%3==1?'V':'A';
    const u32 lengths[]{765,768,771,2100,4096,4097};
    for(u32 length:lengths){const Utf8View text{long_text,length};const TextStyleSpan span{0,length,text_style_heading_3};
        TextEditLayoutInput input{text,text_edit_signature(text),0,length,1,2,95,40,1000000,7,{&span,1}};
        CHECK(measured(input,length<=4096));if(editor.active)CHECK(text_edit_layout_scroll(&editor,-21)&&observe_edit(editor));}
    print("styled_edit_storage_commit=");number(editor.storage.arena.committed);print("\n");return true;
}
