void filter_fixture(u8* data,u32 mode) noexcept {
    clear_bytes(data,2048);clear_bytes(seen,sizeof(seen));put(data,0,1);put(data,2,2);put(data,12,14);put(data,14,1);put(data,16,1);byte_store_be32(data+18,8);
    const u16 values[]{0,1,255,256,32767,65535};
    if(mode==0){put(data,22,1);put(data,24,6);for(u32 i=0;i<6;++i){put(data,26+i*2,values[i]);seen[values[i]]=1;}}
    else if(mode==1){put(data,22,2);put(data,24,4);const u16 ranges[]{0,1,0,255,256,2,32767,32767,4,65535,65535,5};
        for(u32 i=0;i<12;++i)put(data,26+i*2,ranges[i]);for(u16 value:values)seen[value]=1;}
    else if(mode==2){put(data,22,2);put(data,24,1);put(data,28,65535);for(u32 i=0;i<65536;++i)seen[i]=1;}
    else if(mode==3)put(data,22,1);
    else if(mode==4){put(data,22,1);put(data,24,256);for(u32 i=0;i<256;++i){const u16 glyph=static_cast<u16>(i*257);put(data,26+i*2,glyph);seen[glyph]=1;}}
    else{put(data,22,2);put(data,24,256);for(u32 i=0;i<256;++i){put(data,26+i*6,static_cast<u16>(i*256));put(data,28+i*6,static_cast<u16>(i*256+127));put(data,30+i*6,static_cast<u16>(i*128));
        for(u32 j=0;j<128;++j)seen[i*256+j]=1;}}
}
bool filter_membership_units() noexcept {
    u8 bytes[2048];u64 checks=0;
    const u16 flags[]{0x10,0x110,0x118};
    for(u32 mode=0;mode<6;++mode){filter_fixture(bytes,mode);SfntGdef gdef{};
        if(sfnt_gdef_open({bytes,sizeof(bytes)},65536,&gdef)!=SfntStatus::ok)return false;
        for(u16 flag:flags){SfntLookupFilter filter{};
            if(sfnt_lookup_filter_prepare(gdef,flag,0,&filter)!=SfntStatus::ok)return false;
            for(u32 glyph=0;glyph<65536;++glyph){const bool expected=flag==0x118||!seen[glyph];
                if(sfnt_lookup_ignores(gdef,filter,static_cast<u16>(glyph),SfntGlyphClass::mark)!=expected)return false;++checks;}}
    }
    print(utf8_literal("filter_membership_checks="));number(checks);print(utf8_literal("\n"));return true;
}
