// The native editor's retained state and queries, observed: caret, selections, origin and scroll, line count,
// vertical moves, a grid of point hits and the geometry of the start, caret and end.
inline bool observe_edit(const TextEditLayoutOwner& a) noexcept {
    const u32 state[8]{a.active,a.text_bytes,a.anchor,a.caret_offset,a.utf16_units,a.selection_count,a.manual_scroll,a.geometry};observe_value(state);
    observe_value(a.caret);observe_bytes(a.selections,a.selection_count*sizeof(*a.selections));
    const float frame[5]{a.origin_x,a.origin_y,a.layout_height,a.scroll_x,a.scroll_y};observe_value(frame);
    observe_value(text_edit_layout_line_count(a));
    for(u32 down=0;down<2;++down){u32 position=0;CHECK(text_edit_layout_move_vertical(a,down!=0,&position));observe_value(position);}
    const float xs[]{a.origin_x-1,a.caret.left,a.origin_x+a.width*.5f,a.origin_x+a.width+1};
    const float ys[]{a.viewport_top-1,a.caret.top,a.viewport_top+a.height*.5f,a.viewport_top+a.height+1};
    for(float x:xs)for(float y:ys){u32 position=0;CHECK(text_edit_layout_hit_test(a,x,y,&position));observe_value(position);}
    const u32 positions[]{0,a.caret_offset,a.utf16_units};
    for(u32 p:positions){TextEditMetric metric{};CHECK(text_edit_layout_position_geometry(a,p,&metric));observe_value(metric);}
    return true;
}
inline bool edit_hits() noexcept {
    TextEditLayoutOwner native{};TextEditMetric selections[48];native.selections=selections;native.selection_capacity=48;
    struct Release {TextEditLayoutOwner& a;~Release() noexcept {text_edit_layout_release(&a);}} release{native};
    const auto measured=[&](TextEditLayoutInput input,bool required) noexcept {
        ++cases;CHECK(text_edit_layout_measure(&native,input,owner)&&observe_edit(native));if(required)CHECK(native.geometry);
        const auto before=native_text_candidate_counts(owner);const auto builds=native.builds;
        CHECK(text_edit_layout_measure(&native,input,owner)&&native.builds==builds);
        CHECK(native_text_candidate_counts(owner).paragraphs_measured==before.paragraphs_measured);
        return true;
    };
    const float widths[]{13.f,95.f,1000.f};
    const ConstSlice<Utf8View> sets[]{{paragraph_samples,sizeof(paragraph_samples)/sizeof(paragraph_samples[0])},{latin_paragraph_samples,sizeof(latin_paragraph_samples)/sizeof(latin_paragraph_samples[0])}};
    for(const auto set:sets)for(u32 sample=0;sample<set.count;++sample)for(float width:widths)for(u32 options=0;options<8;++options){const auto text=set[sample];
        u32 units=0;CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));
        TextEditLayoutInput input{text,text_edit_signature(text),0,0,9.125f,32.25f,width,40,1000000,static_cast<u8>(options),{}};
        for(u32 p=0;p<=units;++p){input.caret_utf16=p;CHECK(measured(input,true));}
        input.anchor_utf16=units;input.caret_utf16=0;CHECK(measured(input,true));
        CHECK(text_edit_layout_scroll(&native,-33)&&observe_edit(native));
        CHECK(text_edit_layout_scroll(&native,21)&&observe_edit(native));
    }
    // Storage must survive backend switches, empty edits, invalid input and a
    // large selection whose presentation is clipped to the visible viewport.
    const Utf8View transitions[]{utf8_literal("AV\t\nAV"),utf8_literal("Hello \xf0\x9f\x98\x80"),utf8_literal("ffi\r\n"),{},utf8_literal("abc")};
    for(const auto text:transitions){TextEditLayoutInput input{text,text_edit_signature(text),0,0,1,2,95,40,1000000,7,{}};CHECK(measured(input,false));}
    char long_text[4097];for(u32 i=0;i<4097;++i)long_text[i]=i&1?'\n':'a';
    const u32 lengths[]{1024,4096,4097,4};for(u32 length:lengths){Utf8View text{long_text,length};TextEditLayoutInput input{text,text_edit_signature(text),0,length,1,2,95,40,1000000,7,{}};
        CHECK(measured(input,true));CHECK(text_edit_layout_scroll(&native,-21)&&observe_edit(native));}
    const u64 before=native_text_candidate_counts(owner).paragraphs_measured;
    auto independent=owner;independent.independent_letters=true;
    const auto text=utf8_literal("AV ffi");TextEditLayoutInput input{text,text_edit_signature(text),0,0,1,2,95,40,1000000,7,{}};
    text_edit_layout_invalidate(&native);
    CHECK(text_edit_layout_measure(&native,input,independent)&&native.geometry&&native_text_candidate_counts(owner).paragraphs_measured>before&&observe_edit(native));
    input.caret_utf16=100;CHECK(!text_edit_layout_measure(&native,input,owner)&&!native.active);
    print("native_edit_storage_commit=");number(native.storage.arena.committed);print("\n");
    return true;
}
