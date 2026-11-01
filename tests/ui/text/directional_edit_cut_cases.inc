bool directional_edit_cuts() noexcept {
    CHECK(native_text_format_capture(&owner));auto candidate=owner,reference=owner;candidate.factory=nullptr;candidate.format=nullptr;reference.native_text=nullptr;
    DWriteEditLayoutOwner layouts[2]{};TextEditMetric selections[2][48];
    for(u32 i=0;i<2;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
    struct Release {DWriteEditLayoutOwner* p;~Release() noexcept {text_edit_layout_release(p);text_edit_layout_release(p+1);}} release{layouts};
    const float widths[]{7,95,1000000};
    const u32 flags[]{text_style_underline,text_style_strikethrough,text_style_bold,text_style_italic,text_style_monospace,text_style_heading_1,text_style_heading_2,text_style_heading_3,text_style_subtext};
    const u32 base_count=sizeof(directional_editor_samples)/sizeof(directional_editor_samples[0]),sample_count=base_count+sizeof(directional_cut_holdouts)/sizeof(directional_cut_holdouts[0]);
    for(u32 sample=0;sample<sample_count;++sample){const auto text=sample<base_count?directional_editor_samples[sample]:directional_cut_holdouts[sample-base_count];u32 units=0,starts[256],ends[256],cuts=0;
        CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));UnicodeGraphemeState graphemes{};
        for(u32 byte=0;byte<text.bytes;){const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);CHECK(cp.valid);const bool boundary=unicode_grapheme_boundary(graphemes,cp.codepoint);
            if(!boundary){starts[cuts]=byte;ends[cuts++]=byte+cp.width;}byte+=cp.width;}
        for(u32 cut=0;cut<cuts;++cut)for(u32 pattern=0;pattern<2;++pattern)for(u32 flag:flags)for(float width:widths)for(u32 options=0;options<8;++options){
            const TextStyleSpan style{starts[cut],(pattern?ends[cut]:text.bytes)-starts[cut],flag};
            TextEditLayoutInput input{text,text_edit_signature(text),0,0,9.125f,32.25f,width,40,1000000,static_cast<u8>(options),{&style,1}};
            u32 cut_first=0,cut_end=0;CHECK(utf8_utf16_position_for_byte_offset(text,starts[cut],&cut_first)&&utf8_utf16_position_for_byte_offset(text,ends[cut],&cut_end));
            // Raw hit geometry already checks every position and range. This
            // owner checks empty, forward/reverse and cluster-cut transitions.
            const u32 selection_cases[][2]{{0,0},{units,units},{0,units},{units,0},{cut_first,cut_end},{cut_end,cut_first}};
            for(const auto& selection:selection_cases){const u32 a=selection[0],p=selection[1];input.anchor_utf16=a;input.caret_utf16=p;++cases;
                CHECK(text_edit_layout_measure(layouts,input,candidate)&&text_edit_layout_measure(layouts+1,input,reference));
                CHECK(layouts[0].geometry&&!layouts[0].layout&&layouts[1].layout);++native_cases;
                if(!same_edit(layouts[0],layouts[1])){print("retained cluster cut sample/byte/pattern/flag/width/options/anchor/caret=");number(sample);print("/");number(starts[cut]);print("/");number(pattern);print("/");number(flag);print("/");real(width);print("/");number(options);print("/");number(a);print("/");number(p);print("\n");return false;}
            }
            const auto builds=layouts[0].builds;CHECK(text_edit_layout_measure(layouts,input,candidate)&&layouts[0].builds==builds);
            CHECK(text_measure_utf8(&owner,utf8_literal("Other cached paragraph"),{},95,1000,{utf16,256}).ok()&&same_edit(layouts[0],layouts[1]));
            CHECK(text_edit_layout_scroll(layouts,-33)&&text_edit_layout_scroll(layouts+1,-33)&&same_edit(layouts[0],layouts[1]));
            CHECK(text_edit_layout_scroll(layouts,21)&&text_edit_layout_scroll(layouts+1,21)&&same_edit(layouts[0],layouts[1]));
        }
    }
    return true;
}
