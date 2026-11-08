bool tsf_point_geometry() noexcept {
    const Utf8View extra[]{utf8_literal("AV ffi\r\nTo"),utf8_literal(""),utf8_literal("a\tbc\n\nxyz")};
    const ConstSlice<Utf8View> sets[]{{directional_editor_samples,sizeof(directional_editor_samples)/sizeof(*directional_editor_samples)},
        {extra,sizeof(extra)/sizeof(*extra)}};
    const auto& candidate=owner;
    const float widths[]{30.f,120.f};
    for(const auto set:sets)for(u32 index=0;index<set.count;++index)for(float width:widths) {
        const auto text=set[index];
        TextEditLayoutOwner native{};
        TextEditMetric ns[48]{};native.selections=ns;native.selection_capacity=48;
        struct Release {TextEditLayoutOwner& a;~Release() noexcept {text_edit_layout_release(&a);}} release{native};
        u32 units=0;CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));
        TextEditLayoutInput input{text,text_edit_signature(text),0,units,9.125f,32.25f,width,40,1000000,7,{}};
        CHECK(text_edit_layout_measure(&native,input,candidate));
        // TSF points around every caret box, rounded to the nearest boundary or not: position and inside flag.
        for(u32 p=0;p<=units;++p) {
            TextEditMetric metric{};CHECK(text_edit_layout_position_geometry(native,p,&metric));observe_value(metric);
            const float xs[]{metric.left-1,metric.left+metric.width*.25f,metric.left+metric.width*.75f,metric.left+metric.width+1};
            const float ys[]{metric.top-1,metric.top+metric.height*.5f,metric.top+metric.height+1};
            for(float x:xs)for(float y:ys)for(u32 round=0;round<2;++round) {
                u32 a=0;bool inside=false;
                CHECK(text_edit_layout_point_geometry(native,x,y,round!=0,&a,&inside));
                const u32 value[2]{a,inside};observe_value(value);++cases;
            }
        }
    }
    return true;
}
