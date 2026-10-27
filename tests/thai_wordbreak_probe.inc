bool thai_project_native_clusters(ConstSlice<u32> scalars,Slice<u8> boundaries) noexcept {
    char bytes[1024]{};u32 starts[257]{},count=0,units=0;CHECK(scalars.count<=256);
    for(u32 i=0;i<scalars.count;++i){starts[i]=units;count+=utf8_encode(scalars[i],bytes+count);units+=scalars[i]>65535?2u:1u;}
    CHECK(units<=256);if(!owner.native_format)CHECK(native_text_format_capture(&owner));
    auto detached=owner;detached.factory=nullptr;detached.format=nullptr;
    shaped_text_reset(&native_output.text);
    const auto result=text_shape_utf8(&detached,{bytes,count},1000000,1000000,{utf16,256},&native_output.text);
    if(!result.ok()){
        print("thai_native_shape error=");number(static_cast<u32>(result.error));
        print(" native_error=");number(static_cast<u32>(result.native_error));print(" units=");number(result.utf16_units);print("\n");
        return false;
    }
    if(!native_output.text.native_fonts()||native_output.text.cluster_count!=units){
        print("thai_native_output fonts=");number(native_output.text.native_fonts()?1u:0u);
        print(" clusters=");number(native_output.text.cluster_count);print(" expected=");number(units);
        print(" glyphs=");number(native_output.text.glyph_count);print(" runs=");number(native_output.text.run_count);print("\n");
        return false;
    }
    for(u32 i=1;i<scalars.count;++i)if(native_output.text.clusters[starts[i]]==native_output.text.clusters[starts[i]-1])boundaries[i]=0;
    shaped_text_reset(&native_output.text);return true;
}
