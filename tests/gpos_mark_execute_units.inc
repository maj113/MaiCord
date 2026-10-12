struct PointProbe {u32 calls;u16 glyph,point;bool fail;};
SfntStatus resolve_test_point(void* context,u16 glyph,u16 point,double* x,double* y) noexcept {
    auto* p=static_cast<PointProbe*>(context);++p->calls;p->glyph=glyph;p->point=point;if(p->fail)return SfntStatus::malformed;*x=355;*y=777;return SfntStatus::ok;
}
SfntGsubGlyph long_marks[4097];SfntPosition long_positions[4097];double long_advances[4097];u8 long_flags[4097];
u64 linear_search_steps=0,linear_mark_matches=0;
bool mark_history_units() noexcept {
    // The new-mark Windows fixtures cover source-cluster starts and unresolved
    // ownership after GSUB, including the second child in family emoji.
    u8 raw[46]{};put(raw,0,1);put(raw,2,12);put(raw,4,18);put(raw,6,1);put(raw,8,24);put(raw,10,36);
    put(raw,12,1);put(raw,14,1);put(raw,16,2);put(raw,18,1);put(raw,20,1);put(raw,22,1);
    put(raw,24,1);put(raw,28,6);put(raw,30,1);put(raw,32,20);put(raw,34,30);
    put(raw,36,1);put(raw,38,4);put(raw,40,1);put(raw,42,300);put(raw,44,700);
    SfntGposAttachment sub{};if(sfnt_gpos_attachment_open({raw,46},6,3,&sub)!=SfntStatus::ok)return false;
    const SfntGposAttachmentLookup lookup{{&sub,1},{}};
    const SfntGsubGlyph glyphs[]{{0,SfntGlyphClass::base,0},{1,SfntGlyphClass::mark,0},{2,SfntGlyphClass::mark,0}};
    const double nominal[]{600,0,0};constexpr u32 nil=0xffffffffu;
    struct Case {SfntMarkAssociation first,last;bool matches;};
    const Case cases[]{ {{0,1},{0,1},true},{{0,0},{0,1},false},{{0,0},{1,0},false},
        {{nil,nil},{0,1},true},{{0,1},{nil,nil},true},{{nil,nil},{nil,nil},true},{{0,nil},{0,nil},true} };
    for(const auto& test:cases)for(u32 split=0;split<2;++split)for(u32 start=0;start<3;++start){
        const u8 flags[]{2,1,static_cast<u8>(split?3:1)};SfntPosition positions[3];
        if(sfnt_position_initialize({nominal,3},{flags,3},false,{positions,3})!=SfntStatus::ok)return false;
        positions[1].advance_offset=-220;positions[1].ascender_offset=470;
        const SfntMarkAssociation associations[]{{nil,nil},test.first,test.last};SfntMarkExecution result{};
        if(sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,3}),{nominal,3},{flags,3},{positions,3},start,3,false,{},SfntNullAnchorPolicy::skip,&result,1000,nullptr,nullptr,{associations,3})!=SfntStatus::ok ||
           result.matches!=(test.matches?1u:0u) || positions[2].advance_offset!=(test.matches?60:0) || positions[2].ascender_offset!=(test.matches?1140:0))return false;
    }
    return true;
}
bool mark_units() noexcept {
    if(!mark_history_units())return false;
    u8 raw[64]{};
    put(raw,0,1);put(raw,2,12);put(raw,4,18);put(raw,6,1);put(raw,8,24);put(raw,10,36);
    put(raw,12,1);put(raw,14,1);put(raw,16,2);put(raw,18,1);put(raw,20,1);put(raw,22,1);
    put(raw,24,1);put(raw,28,6);put(raw,30,1);put(raw,32,20);put(raw,34,30);
    put(raw,36,1);put(raw,38,4);put(raw,40,1);put(raw,42,300);put(raw,44,700);
    SfntGposAttachment sub{};SfntGposAttachmentLookup lookup{{&sub,1},{}};SfntMarkExecution result{};
    if(sfnt_gpos_attachment_open({raw,46},4,3,&sub)!=SfntStatus::ok)return false;
    // A script can retain an earlier source owner when presentation GSUB
    // expands a lower consonant into another advancing base before this mark.
    {
        constexpr u32 nil=0xffffffffu;
        SfntGsubGlyph owned[]{{1,SfntGlyphClass::base,0},{1,SfntGlyphClass::base,0},{2,SfntGlyphClass::mark,sfnt_mark_source_target}};
        const double widths[]{600,400,0};const u8 position_flags[]{2,0,0};
        SfntMarkAssociation owners[]{{nil,nil},{nil,nil},{0,0}};u32 directory[]{0};SfntPosition placed[3]{};
        SfntPositionInstance instance{};
        const auto execute=[&](u32 start,bool rtl,u64 budget) noexcept {
            if(sfnt_position_initialize({widths,3},{position_flags,3},rtl,{placed,3})!=SfntStatus::ok)return SfntStatus::invalid_input;
            return sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({owned,3}),{widths,3},{position_flags,3},{placed,3},start,3,rtl,instance,
                SfntNullAnchorPolicy::skip,&result,budget,nullptr,nullptr,{owners,3},SfntPositionApplication::scan,false,{directory,1});};
        if(execute(0,false,1000)!=SfntStatus::ok||placed[2].advance_offset!=-120)return false;
        instance.source_mark_targets=true;
        for(u32 start=0;start<3;++start){
            if(execute(start,false,1000)!=SfntStatus::ok||result.matches!=1||placed[2].advance_offset!=-720||placed[2].ascender_offset!=670)return false;
            if(execute(start,true,1000)!=SfntStatus::ok||result.matches!=1||placed[2].advance_offset!=-680)return false;}
        if(execute(0,false,1000)!=SfntStatus::ok)return false;const u64 required=result.operations;
        for(u64 budget=0;budget<required;++budget){if(execute(0,false,budget)!=SfntStatus::resource_limit||result.operations>budget||result.matches)return false;}
        owners[2].target=nil;if(execute(0,false,1000)!=SfntStatus::ok||placed[2].advance_offset!=-120)return false;
        owners[2].target=1;if(execute(0,false,1000)!=SfntStatus::invalid_input||result.matches)return false;
        owners[2].target=0;directory[0]=2;if(execute(0,false,1000)!=SfntStatus::invalid_input||result.matches)return false;
        directory[0]=3;if(execute(0,false,1000)!=SfntStatus::invalid_input||result.matches)return false;
        directory[0]=0;owned[1].glyph=0;
        if(execute(2,false,1000)!=SfntStatus::ok||placed[2].advance_offset!=-720||result.matches!=1)return false;
        instance.source_mark_targets=false;if(execute(2,false,1000)!=SfntStatus::ok||result.matches)return false;
        instance.source_mark_targets=true;owned[0].glyph=3;
        if(execute(2,false,1000)!=SfntStatus::glyph_out_of_range||result.matches)return false;
    }
    const SfntGsubGlyph glyphs[]{{1,SfntGlyphClass::base,0},{2,SfntGlyphClass::mark,0}};double nominal[]{600,0};u8 flags[]{2,1};SfntPosition p[2],initial[2];
    bool succeeded=false;
    for(u32 budget=0;budget<100;++budget){if(sfnt_position_initialize({nominal,2},{flags,2},false,{p,2})!=SfntStatus::ok)return false;copy_bytes(initial,p,sizeof(p));
        const auto s=sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,{},SfntNullAnchorPolicy::skip,&result,budget);++budget_checks;
        if(s==SfntStatus::ok){if(result.matches!=1 || p[1].advance_offset!=-320 || p[1].ascender_offset!=670)return false;succeeded=true;break;}
        if(s!=SfntStatus::resource_limit || result.matches || result.operations>budget || !bytes_equal(p,initial,sizeof(p)))return false;}
    if(!succeeded)return false;
    // A context's single mark input can attach to a target before input_start.
    if(sfnt_position_initialize({nominal,2},{flags,2},false,{p,2})!=SfntStatus::ok || sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},1,2,false,{},SfntNullAnchorPolicy::skip,&result)!=SfntStatus::ok || result.search_steps!=2 || p[1].advance_offset!=-320)return false;
    for(u32 i=0;i<4097;++i){long_marks[i]=glyphs[i?1:0];long_advances[i]=i?0:600;long_flags[i]=i?1:2;}
    if(sfnt_position_initialize({long_advances,4097},{long_flags,4097},false,{long_positions,4097})!=SfntStatus::ok || sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({long_marks,4097}),{long_advances,4097},{long_flags,4097},{long_positions,4097},0,4097,false,{},SfntNullAnchorPolicy::skip,&result,20000)!=SfntStatus::ok)return false;
    linear_search_steps=result.search_steps;linear_mark_matches=result.matches;
    if(linear_search_steps!=4097 || linear_mark_matches!=4096)return false;
    for(u32 i=1;i<4097;++i)if(long_positions[i].advance_offset!=-320 || long_positions[i].ascender_offset!=670)return false;
    // Null policy has observable behavior; strict mode leaves the mark untouched.
    put(raw,38,0);if(sfnt_gpos_attachment_open({raw,46},4,3,&sub)!=SfntStatus::ok || sfnt_position_initialize({nominal,2},{flags,2},false,{p,2})!=SfntStatus::ok)return false;copy_bytes(initial,p,sizeof(p));
    if(sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,{},SfntNullAnchorPolicy::skip,&result)!=SfntStatus::ok || result.matches || !bytes_equal(p,initial,sizeof(p)))return false;
    if(sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,{},SfntNullAnchorPolicy::windows_parent_coordinates,&result)!=SfntStatus::ok || result.compatibility_anchors!=1 || p[1].advance_offset!=-620 || p[1].ascender_offset!=-29)return false;
    if(sfnt_gpos_attachment_open({raw,40},4,3,&sub)!=SfntStatus::ok || sfnt_position_initialize({nominal,2},{flags,2},false,{p,2})!=SfntStatus::ok)return false;copy_bytes(initial,p,sizeof(p));
    if(sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,{},SfntNullAnchorPolicy::windows_parent_coordinates,&result)!=SfntStatus::truncated || result.matches || !bytes_equal(p,initial,sizeof(p)))return false;
    put(raw,38,4);put(raw,40,2);put(raw,46,9);if(sfnt_gpos_attachment_open({raw,48},4,3,&sub)!=SfntStatus::ok)return false;
    if(sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,{},SfntNullAnchorPolicy::skip,&result)!=SfntStatus::unsupported || !bytes_equal(p,initial,sizeof(p)))return false;
    PointProbe probe{0,0,0,true};
    if(sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,{},SfntNullAnchorPolicy::skip,&result,1000,resolve_test_point,&probe)!=SfntStatus::malformed || !bytes_equal(p,initial,sizeof(p)))return false;
    probe.fail=false;if(sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,{},SfntNullAnchorPolicy::skip,&result,1000,resolve_test_point,&probe)!=SfntStatus::ok || probe.calls!=2 || probe.glyph!=1 || probe.point!=9 || p[1].advance_offset!=-265 || p[1].ascender_offset!=747)return false;
    // Variable base-anchor resolution fails before the mark is changed.
    put(raw,40,3);put(raw,46,10);put(raw,48,0);put(raw,50,0);put(raw,52,0);put(raw,54,0x8000);
    if(sfnt_gpos_attachment_open({raw,56},4,3,&sub)!=SfntStatus::ok || sfnt_position_initialize({nominal,2},{flags,2},false,{p,2})!=SfntStatus::ok)return false;copy_bytes(initial,p,sizeof(p));
    const SfntPositionInstance missing{nullptr,0,0,true,VariationArithmetic::wide};
    if(sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,missing,SfntNullAnchorPolicy::skip,&result)!=SfntStatus::missing_table || !bytes_equal(p,initial,sizeof(p)))return false;
    u8 store_bytes[32]{};put(store_bytes,0,1);byte_store_be32(store_bytes+2,12);put(store_bytes,6,1);byte_store_be32(store_bytes+8,22);put(store_bytes,12,1);put(store_bytes,14,1);put(store_bytes,18,16384);put(store_bytes,20,16384);put(store_bytes,22,1);put(store_bytes,24,1);put(store_bytes,26,1);put(store_bytes,30,41);
    ItemVariationStore store{};VariationInstance instance{};double coordinate=.5,scalar=0;if(item_variation_store_open({store_bytes,32},0,&store)!=SfntStatus::ok || variation_instance_prepare(store,{&coordinate,1},{&scalar,1},&instance)!=SfntStatus::ok)return false;
    const SfntPositionInstance varied{&instance,0,0,true,VariationArithmetic::wide};
    if(sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,varied,SfntNullAnchorPolicy::skip,&result)!=SfntStatus::ok || p[1].advance_offset!=-299.5 || p[1].ascender_offset!=670)return false;
    put(raw,38,0);if(sfnt_gpos_attachment_open({raw,56},4,3,&sub)!=SfntStatus::ok)return false;copy_bytes(initial,p,sizeof(p));
    return sfnt_position_execute_mark_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{nominal,2},{flags,2},{p,2},0,2,false,varied,SfntNullAnchorPolicy::windows_parent_coordinates,&result)==SfntStatus::unsupported && !result.matches && bytes_equal(p,initial,sizeof(p));
}
