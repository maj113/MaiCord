// Retained caret/anchor updates do not mutate the reference COM layout. Hold
// its identity alive so allocator pointer reuse cannot reuse an old receipt.
struct ReferenceEditShapeKey {
    u64 format,text,style;
    u32 units,bytes,width,height,em,wrap;
};
struct ReferenceEditQueryValue {u32 position;TextEditMetric metric;};
struct ReferenceEditQueryEntry {
    u32 generation,kind,key[7];
    ReferenceEditQueryValue value;
    bool boundary_control;
};
inline IDWriteTextLayout* reference_edit_layout=nullptr;
inline ReferenceEditShapeKey reference_edit_shape{};
inline ReferenceEditQueryEntry reference_edit_queries[512]{};
inline u32 reference_edit_generation=1;
inline u64 reference_edit_requests=0,reference_edit_reuses=0,reference_edit_controls=0,reference_edit_direct=0;
inline u32 reference_edit_bits(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);return bits;}
inline void reference_edit_queries_release() noexcept {
    com_release(reference_edit_layout);reference_edit_shape={};
    if(!++reference_edit_generation){clear_bytes(reference_edit_queries,sizeof(reference_edit_queries));reference_edit_generation=1;}
}
inline void reference_edit_queries_report() noexcept {
    if(!reference_edit_requests)return;
    print("reference_edit_requests=");number(reference_edit_requests);print(" reference_edit_reuses=");number(reference_edit_reuses);
    print(" reference_edit_controls=");number(reference_edit_controls);print(" reference_edit_direct=");number(reference_edit_direct);print("\n");
}
template<class Execute>
bool reference_edit_query(const DWriteEditLayoutOwner& edit,u32 kind,const u32 (&key)[7],bool boundary,
    Execute execute,ReferenceEditQueryValue* output) noexcept {
    ++reference_edit_requests;
    if(!edit.active||!edit.text_bytes||!edit.layout||edit.geometry){++reference_edit_direct;return execute(output);}
    const ReferenceEditShapeKey shape{edit.format,edit.text_signature,edit.style_signature,edit.utf16_units,edit.text_bytes,
        reference_edit_bits(edit.width),reference_edit_bits(edit.maximum_layout_height),reference_edit_bits(edit.em_size),static_cast<u32>(edit.options&text_edit_layout_wrap)};
    if(reference_edit_layout!=edit.layout||!bytes_equal(&reference_edit_shape,&shape,sizeof(shape))){
        reference_edit_queries_release();reference_edit_layout=edit.layout;reference_edit_layout->AddRef();reference_edit_shape=shape;
    }
    u32 hash=kind*0x9e3779b9u;
    for(u32 word:key){hash^=word;hash*=0x7feb352du;hash^=hash>>15;}
    ReferenceEditQueryEntry* available=nullptr;
    for(u32 probe=0;probe<4;++probe){auto& entry=reference_edit_queries[(hash+probe)&511u];
        if(entry.generation!=reference_edit_generation){available=&entry;break;}
        if(entry.kind!=kind||!bytes_equal(entry.key,key,sizeof(entry.key)))continue;
        if((reference_edit_requests&255u)==0||(boundary&&!entry.boundary_control)){
            ReferenceEditQueryValue checked{};++reference_edit_controls;
            CHECK(execute(&checked)&&checked.position==entry.value.position&&bytes_equal(&checked.metric,&entry.value.metric,sizeof(checked.metric)));
            entry.boundary_control=entry.boundary_control||boundary;
        }else ++reference_edit_reuses;
        *output=entry.value;return true;
    }
    ReferenceEditQueryValue value{};++reference_edit_direct;
    if(!execute(&value))return false;
    if(available){available->generation=reference_edit_generation;available->kind=kind;copy_bytes(available->key,key,sizeof(available->key));
        available->value=value;available->boundary_control=false;}
    *output=value;return true;
}
inline bool reference_edit_line_count(const DWriteEditLayoutOwner& edit,u32* count) noexcept {
    const u32 key[7]{};ReferenceEditQueryValue value{};
    CHECK(reference_edit_query(edit,0,key,false,[&](ReferenceEditQueryValue* out) noexcept {out->position=text_edit_layout_line_count(edit);return !edit.active||!edit.text_bytes||out->position!=0;},&value));
    *count=value.position;return true;
}
inline bool reference_edit_vertical(const DWriteEditLayoutOwner& edit,bool down,u32* position) noexcept {
    // move_vertical reads measured caret coordinates/height, subtracts origin,
    // and clamps to UTF16 units. Scroll/viewport effects enter through origin.
    const u32 key[7]{static_cast<u32>(down),reference_edit_bits(edit.caret.left),reference_edit_bits(edit.caret.top),
        reference_edit_bits(edit.caret.height),reference_edit_bits(edit.origin_x),reference_edit_bits(edit.origin_y),edit.utf16_units};
    ReferenceEditQueryValue value{};
    CHECK(reference_edit_query(edit,1,key,false,[&](ReferenceEditQueryValue* out) noexcept {return text_edit_layout_move_vertical(edit,down,&out->position);},&value));
    *position=value.position;return true;
}
inline bool reference_edit_point(const DWriteEditLayoutOwner& edit,float x,float y,u32* position) noexcept {
    // hit_test subtracts exact origins and clamps the result to UTF16 units.
    const u32 key[7]{reference_edit_bits(x),reference_edit_bits(y),reference_edit_bits(edit.origin_x),reference_edit_bits(edit.origin_y),edit.utf16_units,0,0};
    ReferenceEditQueryValue value{};
    CHECK(reference_edit_query(edit,2,key,false,[&](ReferenceEditQueryValue* out) noexcept {return text_edit_layout_hit_test(edit,x,y,&out->position);},&value));
    *position=value.position;return true;
}
inline bool reference_edit_position(const DWriteEditLayoutOwner& edit,u32 position,TextEditMetric* metric) noexcept {
    // position_geometry translates the raw position by current origins and
    // fixes width to1; both source boundaries receive first-hit controls.
    const u32 key[7]{position,reference_edit_bits(edit.origin_x),reference_edit_bits(edit.origin_y),edit.utf16_units,0,0,0};
    ReferenceEditQueryValue value{};
    CHECK(reference_edit_query(edit,3,key,position==0||position==edit.utf16_units,
        [&](ReferenceEditQueryValue* out) noexcept {return text_edit_layout_position_geometry(edit,position,&out->metric);},&value));
    *metric=value.metric;return true;
}
inline bool same_edit(const DWriteEditLayoutOwner& a,const DWriteEditLayoutOwner& b) noexcept {
    CHECK(a.active==b.active&&a.text_bytes==b.text_bytes&&a.anchor==b.anchor&&a.caret_offset==b.caret_offset&&a.utf16_units==b.utf16_units&&a.selection_count==b.selection_count&&a.manual_scroll==b.manual_scroll);
    CHECK(bytes_equal(&a.caret,&b.caret,sizeof(a.caret))&&bytes_equal(a.selections,b.selections,a.selection_count*sizeof(*a.selections)));
    CHECK(a.origin_x==b.origin_x&&a.origin_y==b.origin_y&&a.layout_height==b.layout_height&&a.scroll_x==b.scroll_x&&a.scroll_y==b.scroll_y);
    const auto lines=text_edit_layout_line_count(a);u32 reference_lines=0;CHECK(reference_edit_line_count(b,&reference_lines)&&lines==reference_lines);
    for(u32 down=0;down<2;++down){u32 x=0,y=0;CHECK(text_edit_layout_move_vertical(a,down!=0,&x)&&reference_edit_vertical(b,down!=0,&y)&&x==y);}
    const float xs[]{a.origin_x-1,a.caret.left,a.origin_x+a.width*.5f,a.origin_x+a.width+1};
    const float ys[]{a.viewport_top-1,a.caret.top,a.viewport_top+a.height*.5f,a.viewport_top+a.height+1};
    for(float x:xs)for(float y:ys){u32 p=0,q=0;CHECK(text_edit_layout_hit_test(a,x,y,&p)&&reference_edit_point(b,x,y,&q)&&p==q);}
    const u32 positions[]{0,a.caret_offset,a.utf16_units};
    for(u32 p:positions){TextEditMetric m{},n{};CHECK(text_edit_layout_position_geometry(a,p,&m)&&reference_edit_position(b,p,&n)&&bytes_equal(&m,&n,sizeof(m)));}
    return true;
}
inline bool edit_hits() noexcept {
    DWriteEditLayoutOwner native{},legacy{};TextEditMetric ns[48],ls[48];native.selections=ns;legacy.selections=ls;native.selection_capacity=legacy.selection_capacity=48;
    struct Release {DWriteEditLayoutOwner& a;DWriteEditLayoutOwner& b;~Release() noexcept {text_edit_layout_release(&a);text_edit_layout_release(&b);}} release{native,legacy};
    auto reference=owner;reference.native_text=nullptr;
    const auto measured=[&](TextEditLayoutInput input,bool required) noexcept {
        ++cases;CHECK(text_edit_layout_measure(&native,input,owner)&&text_edit_layout_measure(&legacy,input,reference));
        CHECK(same_edit(native,legacy));if(required)CHECK(native.geometry&&!native.layout&&legacy.layout);
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
        CHECK(text_edit_layout_scroll(&native,-33)&&text_edit_layout_scroll(&legacy,-33)&&same_edit(native,legacy));
        CHECK(text_edit_layout_scroll(&native,21)&&text_edit_layout_scroll(&legacy,21)&&same_edit(native,legacy));
    }
    // Storage must survive backend switches, empty edits, invalid input and a
    // large selection whose presentation is clipped to the visible viewport.
    const Utf8View transitions[]{utf8_literal("AV\t\nAV"),utf8_literal("Hello \xf0\x9f\x98\x80"),utf8_literal("ffi\r\n"),{},utf8_literal("abc")};
    for(const auto text:transitions){TextEditLayoutInput input{text,text_edit_signature(text),0,0,1,2,95,40,1000000,7,{}};CHECK(measured(input,false));}
    char long_text[4097];for(u32 i=0;i<4097;++i)long_text[i]=i&1?'\n':'a';
    const u32 lengths[]{1024,4096,4097,4};for(u32 length:lengths){Utf8View text{long_text,length};TextEditLayoutInput input{text,text_edit_signature(text),0,length,1,2,95,40,1000000,7,{}};
        CHECK(measured(input,true));CHECK(text_edit_layout_scroll(&native,-21)&&text_edit_layout_scroll(&legacy,-21)&&same_edit(native,legacy));}
    const u64 before=native_text_candidate_counts(owner).paragraphs_measured;
    auto independent=owner;independent.independent_letters=true;auto independent_reference=independent;independent_reference.native_text=nullptr;
    const auto text=utf8_literal("AV ffi");TextEditLayoutInput input{text,text_edit_signature(text),0,0,1,2,95,40,1000000,7,{}};
    text_edit_layout_invalidate(&native);text_edit_layout_invalidate(&legacy);
    CHECK(text_edit_layout_measure(&native,input,independent)&&native.geometry&&!native.layout&&native_text_candidate_counts(owner).paragraphs_measured>before);
    CHECK(text_edit_layout_measure(&legacy,input,independent_reference)&&same_edit(native,legacy));
    input.caret_utf16=100;CHECK(!text_edit_layout_measure(&native,input,owner)&&!native.active);
    print("native_edit_storage_commit=");number(native.storage.arena.committed);print(" legacy_utf16_commit=");number(legacy.storage.arena.committed);print("\n");
    return true;
}
