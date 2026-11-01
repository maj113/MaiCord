bool tsf_point_geometry() noexcept {
    const Utf8View extra[]{utf8_literal("AV ffi\r\nTo"),utf8_literal(""),utf8_literal("a\tbc\n\nxyz")};
    const ConstSlice<Utf8View> sets[]{{directional_editor_samples,sizeof(directional_editor_samples)/sizeof(*directional_editor_samples)},
        {extra,sizeof(extra)/sizeof(*extra)}};
    CHECK(native_text_format_capture(&owner));
    auto candidate=owner,reference=owner;candidate.factory=nullptr;candidate.format=nullptr;reference.native_text=nullptr;
    const float widths[]{30.f,120.f};
    for(const auto set:sets)for(u32 index=0;index<set.count;++index)for(float width:widths) {
        const auto text=set[index];
        DWriteEditLayoutOwner native{},legacy{};
        TextEditMetric ns[48]{},ls[48]{};native.selections=ns;legacy.selections=ls;native.selection_capacity=legacy.selection_capacity=48;
        struct Release {DWriteEditLayoutOwner& a;DWriteEditLayoutOwner& b;~Release() noexcept {text_edit_layout_release(&a);text_edit_layout_release(&b);}} release{native,legacy};
        u32 units=0;CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));
        TextEditLayoutInput input{text,text_edit_signature(text),0,units,9.125f,32.25f,width,40,1000000,7,{}};
        CHECK(text_edit_layout_measure(&native,input,candidate)&&text_edit_layout_measure(&legacy,input,reference));
        CHECK(!native.layout);
        for(u32 p=0;p<=units;++p) {
            TextEditMetric metric{};CHECK(text_edit_layout_position_geometry(legacy,p,&metric));
            const float xs[]{metric.left-1,metric.left+metric.width*.25f,metric.left+metric.width*.75f,metric.left+metric.width+1};
            const float ys[]{metric.top-1,metric.top+metric.height*.5f,metric.top+metric.height+1};
            for(float x:xs)for(float y:ys)for(u32 round=0;round<2;++round) {
                u32 a=0,b=0;bool ai=false,bi=false;
                CHECK(text_edit_layout_point_geometry(native,x,y,round!=0,&a,&ai)&&
                    text_edit_layout_point_geometry(legacy,x,y,round!=0,&b,&bi));
                if(a!=b||ai!=bi){print("point mismatch p=");number(p);print(" round=");number(round);print(" a=");number(a);print(" b=");number(b);print("\n");}
                CHECK(a==b&&ai==bi);++cases;
            }
        }
    }
    return true;
}
