u64 shaping_bounds_checks=0;
#define BOUNDS_CHECK(x) do{++shaping_bounds_checks;if(!(x)){print(utf8_literal("bounds failure line="));number(__LINE__);print(utf8_literal("\n"));return false;}}while(0)
bool shaping_bounds_units() noexcept {
    // The last substitution's coverage excludes all original input glyphs.
    // It becomes applicable only after the preceding lookup edits them.
    const u8 a[]{0,2,0,8,0,1,0,10,0,1,0,1,0,1};
    const u8 b[]{0,2,0,8,0,1,0,20,0,1,0,1,0,10};
    const u8 p[]{0,1,0,8,0,4,0,5,0,1,0,1,0,20};
    SfntGsubExecutableSubtable substitutions[2]{};
    BOUNDS_CHECK(sfnt_gsub_subtable_open({a,sizeof(a)},1,32,&substitutions[0].basic)==SfntStatus::ok);
    BOUNDS_CHECK(sfnt_gsub_subtable_open({b,sizeof(b)},1,32,&substitutions[1].basic)==SfntStatus::ok);
    SfntGsubExecutableLookup sublookups[3]{};sublookups[0].type=1;sublookups[0].bounds={30,30,1};
    sublookups[1].type=1;sublookups[1].subtables={substitutions,1};sublookups[1].bounds={1,1,1};
    sublookups[2].type=1;sublookups[2].subtables={substitutions+1,1};sublookups[2].bounds={10,10,1};
    SfntGsubProgram sub{{sublookups,3},{},32};
    SfntGposSubtable position{};BOUNDS_CHECK(sfnt_gpos_subtable_open({p,sizeof(p)},1,32,&position)==SfntStatus::ok);
    SfntGposExecutableLookup poslookups[3]{};poslookups[0].type=1;poslookups[0].bounds={30,30,1};
    poslookups[1].type=1;poslookups[1].tables.basic={&position,1};poslookups[1].bounds={20,20,1};
    poslookups[2].type=1;poslookups[2].bounds={30,30,1};SfntGposProgram pos{{poslookups,3},{},32};
    const u16 roots[]{0,1,2};SfntShapePlan plan{};plan.substitutions=&sub;plan.positioning=&pos;
    plan.substitution_roots={roots,3};plan.positioning_roots={roots,3};plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* value) noexcept {*value=500;return SfntStatus::ok;};
    SfntClusterGlyph input[4];for(u32 i=0;i<4;++i)input[i]={{1,SfntGlyphClass::base,0},i};
    u64 bitmap[2],chain[1];u32 characters[4],targets[64];SfntMarkAssociation associations[4];
    double advances[4],prefix[4];u8 flags[4];SfntPosition state[4];SfntPlacement output[4];
    SfntShapeStorage storage{{glyphs,128},{nodes,64},{bitmap,2},{characters,4},{targets,64},{associations,4},
        {advances,4},{flags,4},{state,4},{output,4},scratch(),{{gpos_frames,32},{gpos_positions,256},{chain,1},{prefix,4},{cursive_chains,64}}};
    SfntShapeRunResult result{};
    const auto execute=[&](u64 budget=1000000) noexcept {return sfnt_shape_run(plan,{input,4},4,{},false,1000,storage,&result,{budget,100000,1000000});};
    for(u32 gate=0;gate<2;++gate){
        for(u32 i=0;i<3;++i){sublookups[i].bounds.ready=static_cast<u16>(gate);poslookups[i].bounds.ready=static_cast<u16>(gate);}
        BOUNDS_CHECK(execute()==SfntStatus::ok&&result.glyph_count==4&&result.substitution_edits==8);
        for(u32 i=0;i<4;++i)BOUNDS_CHECK(glyphs[i].glyph.glyph==20&&characters[i]==i&&output[i].advance==505&&!output[i].advance_offset&&!output[i].ascender_offset);
        const u64 work=result.lookup_operations;
        for(u64 budget=0;budget<work;++budget)BOUNDS_CHECK(execute(budget)==SfntStatus::resource_limit&&!result.glyph_count&&result.lookup_operations<=budget);
        BOUNDS_CHECK(execute(work)==SfntStatus::ok&&result.lookup_operations==work);
    }
    input[3].glyph.glyph=32;BOUNDS_CHECK(execute()==SfntStatus::invalid_input&&!result.glyph_count);input[3].glyph.glyph=1;
    input[3].glyph.glyph_class=static_cast<SfntGlyphClass>(5);BOUNDS_CHECK(execute()==SfntStatus::invalid_input&&!result.glyph_count);input[3].glyph.glyph_class=SfntGlyphClass::base;
    input[3].source=4;BOUNDS_CHECK(execute()==SfntStatus::invalid_input&&!result.glyph_count);
    input[3].source=3;plan.substitution_roots={};plan.positioning_roots={};
    u32 calls=0;plan.metric_context=&calls;
    plan.advance=+[](void* context,u16 glyph,double* value) noexcept {++*static_cast<u32*>(context);*value=500+glyph;return SfntStatus::ok;};
    const u32 capacities[]{4,31,32,64};for(u32 capacity:capacities){
        storage.targets.count=capacity;plan.cache_advances=true;calls=0;
        BOUNDS_CHECK(execute()==SfntStatus::ok&&calls==(capacity>=32?1u:4u));
        for(u32 i=0;i<4;++i)BOUNDS_CHECK(output[i].advance==501&&characters[i]==i);
        // Aliased hash slots must compare the actual glyph before reuse.
        input[1].glyph.glyph=input[3].glyph.glyph=65;calls=0;
        BOUNDS_CHECK(execute()==SfntStatus::ok&&calls==4);
        for(u32 i=0;i<4;++i)BOUNDS_CHECK(output[i].advance==((i&1)?565:501));
        input[1].glyph.glyph=input[3].glyph.glyph=1;
    }
    storage.targets.count=64;plan.cache_advances=false;calls=0;
    BOUNDS_CHECK(execute()==SfntStatus::ok&&calls==4);
    plan.cache_advances=true;input[1].glyph.glyph=65;
    plan.advance=+[](void*,u16 glyph,double* value) noexcept {*value=501;return glyph==65?SfntStatus::malformed:SfntStatus::ok;};
    BOUNDS_CHECK(execute()==SfntStatus::malformed&&!result.glyph_count&&!result.source_units);
    // Coverage contains a base glyph while the actual mark lies outside it.
    // Reclassifying that covered glyph as a mark must restore the attachment.
    for(u16 type=4;type<=6;++type){
        u8 raw[50]{};const auto put=[&](u32 at,u16 value) noexcept {raw[at]=static_cast<u8>(value>>8);raw[at+1]=static_cast<u8>(value);};
        put(0,1);put(2,12);put(4,18);put(6,1);put(8,24);put(10,36);
        put(12,1);put(14,1);put(16,2);put(18,1);put(20,1);put(22,1);
        put(24,1);put(28,6);put(30,1);put(32,20);put(34,30);
        put(36,1);put(38,4);put(40,1);
        const u32 anchor=type==5?44u:40u;if(type==5){put(42,4);put(44,1);}put(anchor+2,300);put(anchor+4,700);
        SfntGposAttachment attachment{};
        BOUNDS_CHECK(sfnt_gpos_attachment_open({raw,anchor+6},type,32,&attachment)==SfntStatus::ok);
        poslookups[0]={};poslookups[0].type=1;poslookups[1]={};poslookups[1].type=static_cast<u8>(type);
        poslookups[1].tables.attachment={&attachment,1};sfnt_lookup_bounds_add(poslookups[1].bounds,attachment.coverage[0]);
        plan={};plan.positioning=&pos;plan.positioning_roots={roots,2};plan.units_per_em=1000;
        plan.advance=+[](void*,u16,double* value) noexcept {*value=500;return SfntStatus::ok;};
        SfntShapeGlyphPolicy policy{};
        policy.prepare=+[](void*,Slice<SfntComponentGlyph> records,Slice<u8> flags,u64) noexcept {
            for(u32 i=0;i<records.count;++i)flags[i]=records[i].glyph.glyph_class==SfntGlyphClass::mark;return SfntStatus::ok;};
        input[0]={{1,type==4?SfntGlyphClass::base:type==5?SfntGlyphClass::ligature:SfntGlyphClass::mark,0},0};
        input[2]={{10,SfntGlyphClass::mark,0},2};input[3]={{15,SfntGlyphClass::base,0},3};
        for(u32 covered=0;covered<2;++covered){if(type==6)input[0].glyph.glyph_class=covered?SfntGlyphClass::mark:SfntGlyphClass::base;input[1]={{2,covered?SfntGlyphClass::mark:SfntGlyphClass::base,0},1};
            SfntPlacement expected[4]{};u64 scanned_work=0;
            for(u32 gate=0;gate<2;++gate){poslookups[1].bounds.ready=static_cast<u16>(gate);
                const auto shape=[&](u64 work) noexcept {return sfnt_shape_run(plan,{input,4},4,{},false,1000,storage,&result,{work,100000,1000000},{},{},policy);};
                BOUNDS_CHECK(shape(1000000)==SfntStatus::ok&&result.glyph_count==4&&result.position_matches==covered);
                if(!gate){copy_bytes(expected,output,sizeof(expected));scanned_work=result.lookup_operations;}
                else {BOUNDS_CHECK(bytes_equal(expected,output,sizeof(expected)));BOUNDS_CHECK(covered||result.lookup_operations<scanned_work);}
                const u64 work=result.lookup_operations;
                for(u64 budget=0;budget<work;++budget)BOUNDS_CHECK(shape(budget)==SfntStatus::resource_limit&&!result.glyph_count&&result.lookup_operations<=budget);
            }
        }
    }
    return true;
}
#undef BOUNDS_CHECK
