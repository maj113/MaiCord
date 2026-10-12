bool native_script_storage_cases() noexcept {
    #include "generated/script_dispatch_fixture.inc"
    for(u32 value=0;value<256;++value){const u8 profile=script_dispatch_profile[value],admission=script_dispatch_admission[value];
        CHECK(native_text_script_profile(static_cast<UnicodeScript>(value),false)==(value==static_cast<u8>(UnicodeScript::zzzz)?16u:((admission&1)?profile:0)));
        CHECK(native_text_script_profile(static_cast<UnicodeScript>(value),true)==((admission&2)?profile:0));}
    struct alignas(8) TestBinding {u64 value;};TestBinding bindings[2]{{17},{29}};
    NativeStyledSpan<TestBinding> spans[2]{};u8 scripts[2]{};NativeSpanAnalysis<NativeStyledSpan<TestBinding>> analysis{scripts,spans};
    const u16 positions[]{0,1,4095,4096,8000,8191,8192,16384,32768,65535};
    for(u32 script=0;script<256;++script)for(u32 flags=0;flags<8;++flags)for(u16 position:positions){
        spans[0].first_unit=position;spans[0].end_unit=65535;spans[0].set_binding(bindings);
        const u16 value=static_cast<u16>((script<<3)|flags);analysis[0]=value;
        CHECK(scripts[0]==script&&spans[0].shaping_flags()==flags&&spans[0].first_unit==position&&spans[0].end_unit==65535&&static_cast<u16>(analysis[0])==value);
        spans[0].first_unit=32768;CHECK(static_cast<u16>(analysis[0])==value&&spans[0].binding()==bindings&&spans[0].binding()->value==17);
        spans[0].set_binding(bindings+1);CHECK(static_cast<u16>(analysis[0])==value&&spans[0].binding()->value==29);
        spans[1]=spans[0];analysis[1]=analysis[0];CHECK(static_cast<u16>(analysis[1])==value&&spans[1].first_unit==32768&&spans[1].binding()==bindings+1);
        analysis[1]&=static_cast<u16>(~1u);CHECK(static_cast<u16>(analysis[1])==(value&~1u));
        analysis[1]|=1;CHECK(static_cast<u16>(analysis[1])==(value|1u));
        // Retained analysis uses exactly one byte each for script and level.
        const u16 retained=static_cast<u16>((scripts[0]<<8)|125u);
        const u32 restored=(static_cast<u32>(retained&0xff00u)<<3)|(static_cast<u32>(spans[0].shaping_flags())<<8)|(retained&255u);
        CHECK((restored>>8)==value&&(restored&255u)==125u);
    }
    print("script storage: 256 identities, 8 flag combinations, 10 source positions PASS\n");return true;
}
