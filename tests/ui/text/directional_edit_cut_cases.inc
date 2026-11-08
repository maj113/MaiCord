bool directional_edit_cuts() noexcept {
    const auto& candidate=owner;
    TextEditLayoutOwner editor{};TextEditMetric selections[48];editor.selections=selections;editor.selection_capacity=48;
    struct Release {TextEditLayoutOwner& p;~Release() noexcept {text_edit_layout_release(&p);}} release{editor};
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
                if(!text_edit_layout_measure(&editor,input,candidate)||!editor.geometry||!observe_edit(editor)){print("retained cluster cut sample/byte/pattern/flag/width/options/anchor/caret=");number(sample);print("/");number(starts[cut]);print("/");number(pattern);print("/");number(flag);print("/");real(width);print("/");number(options);print("/");number(a);print("/");number(p);print("\n");return false;}
                ++native_cases;
            }
            const auto builds=editor.builds;CHECK(text_edit_layout_measure(&editor,input,candidate)&&editor.builds==builds);
            CHECK(text_measure_utf8(&owner, {utf8_literal("Other cached paragraph"),{},95,1000}).ok()&&observe_edit(editor));
            CHECK(text_edit_layout_scroll(&editor,-33)&&observe_edit(editor));
            CHECK(text_edit_layout_scroll(&editor,21)&&observe_edit(editor));
        }
    }
    return true;
}
