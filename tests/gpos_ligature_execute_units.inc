u64 long_ligature_scan=0,long_ligature_queries=0,ligature_budget_checks=0;
SfntStatus ligature_point(void* context,u16 glyph,u16 point,double* x,double* y) noexcept {
    auto* calls=static_cast<u32*>(context);++*calls;if(glyph!=2 || point!=9)return SfntStatus::invalid_input;*x=500;*y=600;return SfntStatus::ok;
}
SfntGsubGlyph ligature_glyphs[4098];double ligature_nominal[4098],ligature_prefix[2];
SfntPosition ligature_state[4098];SfntMarkAssociation ligature_associations[4098];
u32 ligature_fixture(u8* b) noexcept {
    clear_bytes(b,64);put(b,0,1);put(b,2,12);put(b,4,18);put(b,6,1);put(b,8,26);put(b,10,38);
    put(b,12,1);put(b,14,1);put(b,16,3);put(b,18,1);put(b,20,2);put(b,22,1);put(b,24,2);
    put(b,26,1);put(b,28,0);put(b,30,6);put(b,32,1);put(b,34,20);put(b,36,30);
    put(b,38,2);put(b,40,6);put(b,42,6);put(b,44,2);put(b,46,6);put(b,48,12);
    put(b,50,1);put(b,52,100);put(b,54,700);put(b,56,1);put(b,58,400);put(b,60,600);return 62;
}
void ligature_input(u32 n) noexcept {
    for(u32 i=0;i<n;++i){ligature_glyphs[i]={static_cast<u16>(i<2?i+1:3),i<2?SfntGlyphClass::base:SfntGlyphClass::mark,0};ligature_nominal[i]=i<2?600:0;ligature_state[i]={ligature_nominal[i],0,0};ligature_associations[i]={i<2?0xffffffffu:i&1,i<2?0:i&1};}
}
SfntStatus ligature_run(const SfntGposAttachmentLookup& lookup,u32 n,u32 start,bool rtl,Slice<double> prefix,SfntLigatureExecution* result,u64 budget=1000000,SfntComponentAnchorPolicy component=SfntComponentAnchorPolicy::skip_missing,SfntNullAnchorPolicy nulls=SfntNullAnchorPolicy::parent_coordinates) noexcept {
    const u32 targets[]{0,1};return sfnt_position_execute_ligature_lookup(lookup,{},sfnt_glyph_view({ligature_glyphs,n}),{ligature_nominal,n},{ligature_associations,n},{targets,2},{ligature_state,n},start,n,rtl,{},nulls,component,prefix,result,budget);
}
bool ligature_units() noexcept {
    u8 raw[64];const u32 size=ligature_fixture(raw);SfntGposAttachment sub{};if(sfnt_gpos_attachment_open({raw,size},5,4,&sub)!=SfntStatus::ok)return false;
    SfntGposAttachmentLookup lookup{{&sub,1},{}};SfntLigatureExecution result{};
    for(u32 rtl=0;rtl<2;++rtl){ligature_input(4098);if(ligature_run(lookup,4098,0,rtl!=0,{ligature_prefix,2},&result)!=SfntStatus::ok || result.matches!=4096 || result.scan_steps!=8196 || result.prefix_queries!=4096 || result.ordered_steps || result.missing_components)return false;
        for(u32 i=2;i<4098;++i){const double distance=i&1?600:1200,delta=i&1?380:80;
            if(ligature_state[i].advance || ligature_state[i].advance_offset!=-distance+(rtl?600-delta:delta) || ligature_state[i].ascender_offset!=(i&1?570:670))return false;}
        long_ligature_scan=result.scan_steps;long_ligature_queries=result.prefix_queries;
    }
    // Fractional advances use ordered intervals; compare exact scalar values.
    for(u32 start=0;start<4;++start){ligature_input(4);ligature_state[0].advance=503.5;ligature_state[1].advance=711.25;ligature_state[2].advance=.125;
        if(ligature_run(lookup,4,start,false,{ligature_prefix,2},&result)!=SfntStatus::ok || result.prefix_queries)return false;
        if(start<=2 && (ligature_state[2].advance_offset!=80-1214.75 || ligature_state[2].ascender_offset!=670))return false;
        if(ligature_state[3].advance_offset!=380-711.375 || ligature_state[3].ascender_offset!=570)return false;
    }
    // An integral but large leading advance makes prefix subtraction unsafe.
    ligature_input(4);ligature_state[0].advance=1e16;ligature_state[1].advance=1;ligature_associations[2]={1,0};ligature_associations[3]={1,0};
    if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result)!=SfntStatus::ok || result.prefix_queries || result.ordered_steps!=2 || ligature_state[2].advance_offset!=79 || ligature_state[3].advance_offset!=79)return false;
    // Exactness certificate accepts negative integral advances and signed zero.
    ligature_input(4);ligature_state[0].advance=-900;ligature_state[1].advance=300;ligature_state[2].advance=-0.0;
    if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result)!=SfntStatus::ok || result.prefix_queries!=2 || ligature_state[2].advance_offset!=680 || ligature_state[3].advance_offset!=80)return false;
    // No-prefix mode must preserve the same placements, without hidden storage.
    ligature_input(4);if(ligature_run(lookup,4,0,false,{},&result)!=SfntStatus::ok || result.prefix_queries || result.ordered_steps!=4 || ligature_state[2].advance_offset!=-1120 || ligature_state[3].advance_offset!=-220)return false;
    SfntPosition before[4],complete[4];ligature_input(4);copy_bytes(before,ligature_state,sizeof(before));
    if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result)!=SfntStatus::ok)return false;copy_bytes(complete,ligature_state,sizeof(complete));const u64 total=result.operations;
    for(u64 budget=0;budget<=total;++budget){ligature_input(4);const auto status=ligature_run(lookup,4,0,false,{ligature_prefix,2},&result,budget);++ligature_budget_checks;
        if(status!=(budget<total?SfntStatus::resource_limit:SfntStatus::ok) || result.operations>budget || result.matches>2)return false;
        for(u32 i=0;i<4;++i){const auto& expected=i>=2 && i-2<result.matches?complete[i]:before[i];if(!bytes_equal(&ligature_state[i],&expected,sizeof(expected)))return false;}}
    ligature_input(4);copy_bytes(before,ligature_state,sizeof(before));
    if(ligature_run(lookup,4,0,false,{ligature_prefix,1},&result)!=SfntStatus::output_too_small || !bytes_equal(before,ligature_state,sizeof(before)))return false;
    ligature_associations[3].target=3;if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result)!=SfntStatus::invalid_input || !bytes_equal(before,ligature_state,sizeof(before)))return false;
    ligature_input(4);ligature_associations[2].component=0xfffffffeu;
    if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result,1000,SfntComponentAnchorPolicy::strict)!=SfntStatus::invalid_input || result.matches)return false;
    if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result)!=SfntStatus::ok || result.matches!=1 || result.missing_components!=1 || ligature_state[2].advance_offset)return false;
    // A missing first subtable component must permit a later subtable to match.
    SfntGposAttachment pair[2]{sub,sub};u8 short_raw[64];copy_bytes(short_raw,raw,64);put(short_raw,44,1);
    if(sfnt_gpos_attachment_open({short_raw,size},5,4,&pair[0])!=SfntStatus::ok)return false;
    ligature_input(4);if(ligature_run({{pair,2},{}},4,0,false,{ligature_prefix,2},&result)!=SfntStatus::ok || result.matches!=2 || result.missing_components!=1)return false;
    // Null anchors distinguish unknown history from a known component zero.
    put(raw,46,0);if(sfnt_gpos_attachment_open({raw,size},5,4,&sub)!=SfntStatus::ok)return false;
    ligature_input(4);if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result)!=SfntStatus::ok || result.matches!=1 || ligature_state[2].advance_offset)return false;
    ligature_input(4);ligature_associations[2].component=sfnt_component_unknown;
    if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result)!=SfntStatus::ok || result.matches!=2 || result.compatibility_anchors!=1 || ligature_state[2].advance_offset!=-1220 || ligature_state[2].ascender_offset!=-18)return false;
    ligature_input(4);ligature_associations[2].component=sfnt_component_unknown;
    if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result,1000,SfntComponentAnchorPolicy::skip_missing,SfntNullAnchorPolicy::skip)!=SfntStatus::ok || result.matches!=1 || result.compatibility_anchors)return false;
    // Resolve the selected component's point on its actual target glyph.
    ligature_fixture(raw);put(raw,56,2);put(raw,62,9);if(sfnt_gpos_attachment_open({raw,64},5,4,&sub)!=SfntStatus::ok)return false;
    ligature_input(4);copy_bytes(before,ligature_state,sizeof(before));
    if(ligature_run(lookup,4,3,false,{ligature_prefix,2},&result)!=SfntStatus::unsupported || result.matches || !bytes_equal(before,ligature_state,sizeof(before)))return false;
    const u32 targets[]{0,1};u32 calls=0;
    if(sfnt_position_execute_ligature_lookup(lookup,{},sfnt_glyph_view({ligature_glyphs,4}),{ligature_nominal,4},{ligature_associations,4},{targets,2},{ligature_state,4},3,4,false,{},SfntNullAnchorPolicy::skip,SfntComponentAnchorPolicy::strict,{ligature_prefix,2},&result,1000,ligature_point,&calls)!=SfntStatus::ok || calls!=1 || result.matches!=1 || ligature_state[3].advance_offset!=-120 || ligature_state[3].ascender_offset!=570)return false;
    // The ordered fallback obeys the same per-mark commit/work contract.
    ligature_fixture(raw);if(sfnt_gpos_attachment_open({raw,size},5,4,&sub)!=SfntStatus::ok)return false;
    ligature_input(4);ligature_state[0].advance=.5;copy_bytes(before,ligature_state,sizeof(before));
    if(ligature_run(lookup,4,0,false,{ligature_prefix,2},&result)!=SfntStatus::ok)return false;copy_bytes(complete,ligature_state,sizeof(complete));const u64 ordered_total=result.operations;
    for(u64 budget=0;budget<=ordered_total;++budget){copy_bytes(ligature_state,before,sizeof(before));const auto status=ligature_run(lookup,4,0,false,{ligature_prefix,2},&result,budget);++ligature_budget_checks;
        if(status!=(budget<ordered_total?SfntStatus::resource_limit:SfntStatus::ok) || result.operations>budget || result.matches>2 || result.prefix_queries)return false;
        for(u32 i=0;i<4;++i){const auto& expected=i>=2 && i-2<result.matches?complete[i]:before[i];if(!bytes_equal(&ligature_state[i],&expected,sizeof(expected)))return false;}}
    // Exact scratch capacity at an inaccessible page and stale-prefix reuse.
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;DWORD old=0;
    if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}auto* guarded=reinterpret_cast<double*>(memory+page-2*sizeof(double));
    ligature_fixture(raw);bool ok=sfnt_gpos_attachment_open({raw,size},5,4,&sub)==SfntStatus::ok;ligature_input(4);
    ok=ok && ligature_run(lookup,4,0,false,{guarded,2},&result)==SfntStatus::ok && result.prefix_queries==2;
    ligature_input(4);ligature_state[0].advance=.5;ok=ok && ligature_run(lookup,4,0,false,{guarded,2},&result)==SfntStatus::ok && !result.prefix_queries && ligature_state[2].advance_offset==-520.5;
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
