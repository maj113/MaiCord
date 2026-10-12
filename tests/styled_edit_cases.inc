bool styled_edit() noexcept {
    DWriteEditLayoutOwner layouts[2]{};TextEditMetric selections[2][48];
    for(u32 i=0;i<2;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
    struct Release {DWriteEditLayoutOwner* p;~Release() noexcept {text_edit_layout_release(p);text_edit_layout_release(p+1);}} release{layouts};
    auto reference=owner;reference.native_text=nullptr;
    const auto measured=[&](const TextEditLayoutInput& input,bool required=true) noexcept {
        ++cases;CHECK(text_edit_layout_measure(layouts,input,owner)&&text_edit_layout_measure(layouts+1,input,reference));
        if(required){CHECK(layouts[0].geometry&&!layouts[0].layout&&layouts[1].layout);++native_cases;}
        const auto before=native_text_candidate_counts(owner);const auto builds=layouts[0].builds;
        CHECK(text_edit_layout_measure(layouts,input,owner)&&layouts[0].builds==builds&&native_text_candidate_counts(owner).paragraphs_measured==before.paragraphs_measured);
        if(input.text.bytes<=256){
            CHECK(text_shape_utf8_styled(&owner,input.text,input.styles,input.width,input.maximum_layout_height,{utf16,256},&native_output.text,(input.options&text_edit_layout_wrap)?TextWrapping::wrap:TextWrapping::no_wrap).ok());
            CHECK(text_shape_utf8_styled(&reference,input.text,input.styles,input.width,input.maximum_layout_height,{utf16,256},&reference_output.text,(input.options&text_edit_layout_wrap)?TextWrapping::wrap:TextWrapping::no_wrap).ok());
            CHECK(same(native_output,reference_output));}
        // Retained metrics must survive unrelated use of the shared shape cache.
        CHECK(text_measure_utf8_styled(&owner,utf8_literal("Other cached paragraph"),{},95,1000,{utf16,256}).ok());
        CHECK(same_edit(layouts[0],layouts[1]));return true;
    };
    const float widths[]{13.f,95.f,1000.f};
    for(u32 sample=0;sample<6;++sample)for(u32 style=0;style<8;++style){const auto text=styled_hit_texts[sample];TextStyleSpan spans[3];const u32 count=styled_hit_styles(text,style,spans);u32 units=0;
        CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));
        for(float width:widths)for(u32 options=0;options<8;++options){TextEditLayoutInput input{text,text_edit_signature(text),0,0,9.125f,32.25f,width,40,1000000,static_cast<u8>(options),{spans,count}};
            for(u32 p=0;p<=units;++p){input.caret_utf16=p;if(!measured(input)){print("styled editor sample/style/width/options/position=");number(sample);print("/");number(style);print("/");real(width);print("/");number(options);print("/");number(p);print("\n");return false;}}
            input.anchor_utf16=units;input.caret_utf16=0;CHECK(measured(input));
            CHECK(text_edit_layout_scroll(layouts,-33)&&text_edit_layout_scroll(layouts+1,-33)&&same_edit(layouts[0],layouts[1]));
            CHECK(text_edit_layout_scroll(layouts,21)&&text_edit_layout_scroll(layouts+1,21)&&same_edit(layouts[0],layouts[1]));
        }
    }
    const Utf8View switches[]{utf8_literal("AV office fi AV"),utf8_literal("AV office \xf0\x9f\x98\x80"),utf8_literal("AV office fi AV"),{},utf8_literal("AV office\n\n")};
    for(u32 i=0;i<5;++i){const auto text=switches[i];const TextStyleSpan span{0,text.bytes,text_style_heading_1};TextEditLayoutInput input{text,text_edit_signature(text),0,0,1,2,95,40,1000000,7,{&span,text.bytes?1u:0u}};
        CHECK(measured(input,i==0||i==2||i==4));}
    char long_text[4097];for(u32 i=0;i<4097;++i)long_text[i]=i%3==2?'\n':i%3==1?'V':'A';
    const u32 lengths[]{765,768,771,2100,4096,4097};
    for(u32 length:lengths){const Utf8View text{long_text,length};const TextStyleSpan span{0,length,text_style_heading_3};
        TextEditLayoutInput input{text,text_edit_signature(text),0,length,1,2,95,40,1000000,7,{&span,1}};
        CHECK(measured(input,length<=4096));CHECK(text_edit_layout_scroll(layouts,-21)&&text_edit_layout_scroll(layouts+1,-21)&&same_edit(layouts[0],layouts[1]));}
    print("styled_edit_storage_commit=");number(layouts[0].storage.arena.committed);print(" legacy_utf16_commit=");number(layouts[1].storage.arena.committed);print("\n");return true;
}
