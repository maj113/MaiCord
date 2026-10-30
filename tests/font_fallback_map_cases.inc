struct MapMock {u32 base_calls=0,route_calls=0,fallback_calls=0,coverage_calls=0,fail=0;bool absent=false;};
bool map_has(u32 font,u32 cp) noexcept {
    if(font==0)return cp=='a'||cp==0x301||cp==0xfe0f;
    if(font==1)return cp=='b'||cp==0x200d||cp==0xe000;
    if(font==2)return cp!='a'&&cp!=0x301;
    return font==3&&cp!='b'&&cp!=0x200d;
}
SfntStatus map_base(void* context,u32 cp,u32* out) noexcept {
    auto& m=*static_cast<MapMock*>(context);++m.base_calls;if(m.fail==1)return SfntStatus::malformed;
    *out=m.absent?native_fallback_missing:map_has(0,cp)?0u:map_has(1,cp)?1u:native_fallback_missing;return SfntStatus::ok;
}
SfntStatus map_route(void* context,u32,u32* out) noexcept {auto& m=*static_cast<MapMock*>(context);++m.route_calls;if(m.fail==2)return SfntStatus::truncated;*out=0;return SfntStatus::ok;}
SfntStatus map_fallback(void* context,u32 cp,u32* out) noexcept {
    auto& m=*static_cast<MapMock*>(context);++m.fallback_calls;if(m.fail==3)return SfntStatus::unsupported;
    *out=map_has(2,cp)?2u:map_has(3,cp)?3u:native_fallback_missing;return SfntStatus::ok;
}
SfntStatus map_cover(void* context,u32 font,u32 cp,bool* out) noexcept {
    auto& m=*static_cast<MapMock*>(context);++m.coverage_calls;if(m.fail==4)return SfntStatus::invalid_input;*out=map_has(font,cp);return SfntStatus::ok;
}
bool mapping_guards() noexcept {
    // Independently captured by the two-family Windows probe, normal weight,
    // broad fallback mapping. Tokens identify the four controlled physical faces.
    struct Case {const char* text;u32 units,font;};
    const Case cases[]{{"a\xee\x80\x80",1,0},{"xa",1,2},{"x\xcc\x81",1,2},{"x\xe2\x80\x8d",2,2},
        {"x\xef\xb8\x8f",2,2},{"\xcd\x8f" "b",2,2},{"\xef\xb8\x8f" "x",1,0},
        {"\xef\xa4\x80" "a",1,2},{"xxx",3,2},{"\xcd\x8f" "ab",1,2}};
    NativeFallbackRangeResult result{};
    for(const auto& c:cases){const u32 bytes=c_string_length(c.text);MapMock m{};
        CHECK(native_font_fallback_map({c.text,bytes},map_base,map_route,map_fallback,map_cover,&m,&result)==SfntStatus::ok);
        CHECK(result.units==c.units&&result.choice.font==c.font&&result.choice.policy==(c.font<2?native_fallback_missing:0));
        if(c.font<2)CHECK(!m.route_calls&&!m.fallback_calls);
    }
    MapMock m{};CHECK(native_font_fallback_map({"a",1},map_base,map_route,map_fallback,map_cover,&m,&result)==SfntStatus::ok&&m.base_calls==1&&!m.route_calls&&!m.fallback_calls);
    m={};CHECK(native_font_fallback_map({"x",1},map_base,map_route,map_fallback,map_cover,&m,&result)==SfntStatus::ok&&m.base_calls==1&&m.route_calls==1&&m.fallback_calls==1);
    const SfntStatus errors[]{SfntStatus::malformed,SfntStatus::truncated,SfntStatus::unsupported,SfntStatus::invalid_input};
    for(u32 fail=1;fail<=4;++fail){m={};m.fail=fail;CHECK(native_font_fallback_map({"x\xe2\x80\x8d",4},map_base,map_route,map_fallback,map_cover,&m,&result)==errors[fail-1]);}
    m={};m.absent=true;CHECK(native_font_fallback_map({"a",1},map_base,map_route,map_fallback,map_cover,&m,&result)==SfntStatus::ok&&result.choice.font==3);
    const char invalid[]{'x',static_cast<char>(0xc0),static_cast<char>(0xaf)};m={};CHECK(native_font_fallback_map({invalid,3},map_base,map_route,map_fallback,map_cover,&m,&result)==SfntStatus::invalid_input);
    return true;
}
