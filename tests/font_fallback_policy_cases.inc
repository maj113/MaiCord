struct PolicyFamily {const u16* name;u32 units;ConstSlice<u32> order;};
struct PolicyContext {
    NativeFallbackCascade cascade;PolicyFamily* families;RangeContext fonts;ConstSlice<u32> base;
    u32 initial=native_fallback_missing;bool bound=false;
};
SfntStatus policy_route(void* context,u32 cp,u32* output) noexcept {
    auto& c=*static_cast<PolicyContext*>(context);const auto status=native_fallback_cascade_find(c.cascade,cp,output);
    if(status==SfntStatus::ok&&!c.bound){c.initial=*output;c.bound=true;}return status;
}
SfntStatus policy_font(void* context,u32 cp,u32* output) noexcept {
    auto& c=*static_cast<PolicyContext*>(context);*output=native_fallback_missing;if(!c.bound)return SfntStatus::invalid_input;
    if(c.initial==native_fallback_missing)return SfntStatus::ok;const auto& r=c.cascade.ranges[c.initial];
    for(u32 i=0;i<r.count;++i){c.fonts.order=c.families[c.cascade.tokens[r.offset+i]].order;
        const auto status=range_font(&c.fonts,cp,output);if(status!=SfntStatus::ok||*output!=native_fallback_missing)return status;}
    return SfntStatus::ok;
}
SfntStatus policy_base(void* context,u32 cp,u32* output) noexcept {
    auto& c=*static_cast<PolicyContext*>(context);c.fonts.base=c.base;return range_base(&c.fonts,cp,output);
}
SfntStatus policy_cover(void* context,u32 font,u32 cp,bool* output) noexcept {
    return range_coverage(&static_cast<PolicyContext*>(context)->fonts,font,cp,output);
}
bool policy_fixture(Reader& reader) noexcept {
    u32 nr=0,nt=0,nfamilies=0,nfiles=0,nexpected=0,nblocks=0,differences=0;
    CHECK(reader.integer(&nr)&&nr&&nr<=4096&&reader.integer(&nt)&&nt<=32768&&reader.integer(&nfamilies)&&nfamilies&&nfamilies<=64&&
        reader.integer(&nfiles)&&nfiles&&nfiles<=256&&reader.integer(&nexpected)&&nexpected<=1024&&reader.integer(&nblocks)&&nblocks<=4096);
    NativeFontCatalogBuilder builder{};Arena arena{};Coverage coverage{};
    struct Cleanup {NativeFontCatalogBuilder& b;Arena& a;Coverage& c;~Cleanup() noexcept {
        native_font_coverage_release(&c.cache);if(c.session)native_font_session_release(&c.session);native_font_catalog_builder_release(&b);arena_release(&a);}} cleanup{builder,arena,coverage};
    CHECK(native_font_catalog_builder_initialize(&builder)&&arena_initialize(&arena,4*1024*1024,0,4096));
    auto* ranges=arena_allocate<NativeFallbackRange>(&arena,nr);auto* tokens=arena_allocate<u16>(&arena,nt);
    CHECK(ranges&&(!nt||tokens)&&reader.read(ranges,nr*8)&&reader.read(tokens,nt*2));NativeFallbackCascade cascade{};
    CHECK(native_fallback_cascade_open({ranges,nr},{tokens,nt},nfamilies,&cascade)==SfntStatus::ok);
    u16 names[32768];wchar_t path[32768];u32 units=0,files[256];
    for(u32 i=0;i<nfiles;++i){CHECK(reader.text(path,&units));CHECK(native_font_catalog_builder_add(&builder,path,{names,32768},files+i).ok());}
    auto* families=arena_allocate<PolicyFamily>(&arena,nfamilies);CHECK(families);
    for(u32 i=0;i<nfamilies;++i){CHECK(reader.text(path,&units)&&units);auto* saved=arena_allocate<u16>(&arena,units);CHECK(saved);copy_bytes(saved,path,units*2);families[i]={saved,units,{}};}
    Expected expected[1024]{};
    for(u32 i=0;i<nexpected;++i){auto& e=expected[i];CHECK(reader.integer(&e.file)&&e.file<nfiles&&reader.integer(&e.index)&&reader.integer(&e.simulations)&&reader.read(&e.properties,4)&&reader.integer(&e.axes)&&e.axes<=2);
        for(u32 j=0;j<e.axes;++j)CHECK(reader.integer(e.tags+j)&&reader.integer(e.bits+j));}
    const auto& catalog=builder.catalog;coverage.catalog=&catalog;const u32 count=catalog.count.candidates;
    CHECK(native_font_session_create(&coverage.session)&&native_font_coverage_initialize(coverage.session,{coverage.slots,16},&coverage.cache));
    auto* props=arena_allocate<NativeFontProperties>(&arena,count);auto* indices=arena_allocate<u32>(&arena,count);auto* scratch_order=arena_allocate<u32>(&arena,count);
    CHECK(props&&indices&&scratch_order);const auto mark=arena_mark(&arena);
    for(u32 block=0;block<nblocks;++block){arena_rewind(&arena,mark);NativeFontProperties request{};u32 mode=0,n=0;
        CHECK(reader.read(&request,4)&&reader.text(path,&units));const u32 base_units=units;
        auto* base_name=arena_allocate<u16>(&arena,base_units);CHECK(!base_units||base_name);copy_bytes(base_name,path,base_units*2);
        CHECK(reader.integer(&mode)&&mode<=2&&reader.integer(&n)&&(!mode||n==0x110000));
        char suffix[1024];u32 suffix_bytes=0;if(mode==2)CHECK(reader.integer(&suffix_bytes)&&suffix_bytes<=sizeof(suffix)&&reader.read(suffix,suffix_bytes));
        for(u32 i=0;i<nfamilies;++i){auto& family=families[i];u32 length=0;
            const auto status=native_font_catalog_fallback_order(catalog,{family.name,family.units},request,{props,count},{indices,count},{scratch_order,count},&length);
            CHECK(status==SfntStatus::ok||status==SfntStatus::unsupported_sequence);
            auto* order=arena_allocate<u32>(&arena,length);CHECK(!length||order);copy_bytes(order,scratch_order,length*4);family.order={order,length};}
        ConstSlice<u32> base{};
        if(base_units){u32 length=0;const auto status=native_font_catalog_fallback_order(catalog,{base_name,base_units},request,{props,count},{indices,count},{scratch_order,count},&length);
            CHECK(status==SfntStatus::ok||status==SfntStatus::unsupported_sequence);auto* order=arena_allocate<u32>(&arena,length);CHECK(!length||order);copy_bytes(order,scratch_order,length*4);base={order,length};}
        for(u32 query=0;query<n;++query){char text[4096];u32 bytes=0,id=0,mapped=0;
            if(mode){CHECK(reader.integer(&id));if(mode==2)CHECK(reader.integer(&mapped));if(query>=0xd800&&query<=0xdfff){CHECK(id==native_fallback_missing&&!mapped);continue;}
                bytes=utf8_encode(query,text);if(mode==2){copy_bytes(text+bytes,suffix,suffix_bytes);bytes+=suffix_bytes;}else mapped=query>0xffff?2u:1u;}
            else CHECK(reader.integer(&bytes)&&bytes&&bytes<=sizeof(text)&&reader.integer(&id)&&reader.integer(&mapped)&&reader.read(text,bytes));
            CHECK(id<nexpected||id==native_fallback_missing);
            PolicyContext context{cascade,families,{&catalog,{},request,&coverage,0},base};NativeFallbackRangeResult result{};
            const auto status=native_font_fallback_map({text,bytes},base_units?policy_base:nullptr,policy_route,policy_font,policy_cover,&context,&result);
            CHECK(status==SfntStatus::ok);u32 expected_bytes=0;CHECK(utf8_byte_offset_for_utf16_position({text,bytes},mapped,&expected_bytes));
            if(result.units!=mapped||result.bytes!=expected_bytes){if(differences<64){print("policy block=");number(block);print(" query=");number(query);print(" scalar=");number(utf8_decode_one(text,bytes).codepoint);print(" units=");number(result.units);print(" expected=");number(mapped);print("\n");}++differences;}
            if(id==native_fallback_missing)CHECK(result.choice.font==native_fallback_missing);
            else{CHECK(result.choice.font<catalog.count.candidates);NativeFontCatalogEntry entry{};NativeFontMatch match{};
                CHECK(native_font_catalog_entry(catalog,result.choice.font,&entry).ok()&&native_font_match({&catalog.candidates[result.choice.font].properties,1},request,&match)==SfntStatus::ok);
                const auto& e=expected[id];CHECK(entry.file==catalog.files.data+files[e.file]&&entry.face_index==e.index&&match.simulations==e.simulations&&bytes_equal(&match.properties,&e.properties,4)&&entry.settings.count==e.axes);
                for(u32 k=0;k<e.axes;++k){const float value=static_cast<float>(entry.settings[k].value);u32 bits=0;copy_bytes(&bits,&value,4);CHECK(entry.settings[k].tag==e.tags[k]&&bits==e.bits[k]);}}
            ++cases;++range_cases;
        }++groups;
    }
    CHECK(native_font_coverage_release(&coverage.cache)&&!native_font_session_stats(coverage.session).live.files&&native_font_session_release(&coverage.session));
    print("policy differences=");number(differences);print("\n");CHECK(!differences);return true;
}
