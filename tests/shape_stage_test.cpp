#include "maicord/text/shape_run.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
void print(const char* text) noexcept {u32 size=0;while(text[size])++size;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,size,&written,nullptr);}
bool failed(u32 line) noexcept {char digits[16];u32 count=0;do{digits[count++]=static_cast<char>('0'+line%10);line/=10;}while(line);print("FAIL shape stages line ");while(count){char next[]{digits[--count],0};print(next);}print("\n");return false;}
#define CHECK(value) do {if(!(value))return failed(__LINE__);} while(false)
SfntComponentGlyph glyphs[16],replacements[16];SfntComponentNode nodes[32];SfntComponentUndo journal[128];
SfntGsubExecutionFrame frames[8];u32 positions[32],rebased[32],consumed[16],characters[16],targets[32];
u64 bits[2];SfntMarkAssociation associations[16];double advances[16];u8 flags[16];SfntPosition positions_out[16];SfntPlacement placements[16];
SfntShapeStorage storage() noexcept {return {{glyphs,16},{nodes,32},{bits,2},{characters,16},{targets,32},{associations,16},
    {advances,16},{flags,16},{positions_out,16},{placements,16},
    {{frames,8},{positions,32},{rebased,32},{consumed,16},{reinterpret_cast<u8*>(replacements),sizeof(replacements)},{journal,128}},{}};}
struct Policy {u16 visited[256]{};u32 calls=0,mode=0;};
SfntStatus stage(void* opaque,u16 index,SfntGlyphBuffer* buffer,SfntClusterMap*,SfntComponentForest* forest,SfntGsubExecutionScratch,u64 work,u64* used) noexcept {
    auto& p=*static_cast<Policy*>(opaque);*used=0;if(p.calls>=256)return SfntStatus::invalid_input;p.visited[p.calls++]=index;
    if(index==5&&!p.mode){
        if(!work)return SfntStatus::resource_limit;*used=1;const auto view=sfnt_glyph_buffer_view(*buffer);
        if(sfnt_glyph_count(view)!=1)return SfntStatus::malformed;
        const auto& glyph=*reinterpret_cast<const SfntComponentGlyph*>(&sfnt_glyph_at(view,0));
        if(glyph.glyph.glyph!=5||glyph.component>=forest->nodes.count||forest->nodes[glyph.component].count!=3)return SfntStatus::malformed;
        u32 first=0,last=0;u64 spent=0;const auto status=sfnt_component_source_bounds(*forest,glyph.component,&first,&last,work-1,&spent);*used+=spent;
        if(status!=SfntStatus::ok)return status;return first==0&&last==2?SfntStatus::ok:SfntStatus::malformed;
    }
    if(index!=1)return SfntStatus::ok;
    if(p.mode==1)return SfntStatus::unsupported_sequence;
    if(p.mode==2){*used=work+1;return SfntStatus::ok;}
    if(p.mode==3)return SfntStatus::ok;
    const u32 count=sfnt_glyph_count(sfnt_glyph_buffer_view(*buffer));
    const u64 needed=static_cast<u64>(count)*2+1;if(needed>work)return SfntStatus::resource_limit;
    const auto status=sfnt_glyph_buffer_seek(buffer,count,work);if(status!=SfntStatus::ok)return status;*used=needed;
    auto* records=reinterpret_cast<SfntComponentGlyph*>(buffer->storage.data);
    if(p.mode==4){records[0].glyph.glyph=9;return SfntStatus::ok;}
    if(p.mode>=5){
        if(p.mode==5)records[0].source=1;
        if(p.mode==6)records[0].glyph.glyph=16;
        if(p.mode==7)records[0].glyph.glyph_class=static_cast<SfntGlyphClass>(5);
        if(p.mode==8)records[0].component=forest->nodes.count;
        if(p.mode==9)records[0].attachment=forest->nodes.count;
        return SfntStatus::ok;
    }
    if(count!=2||records[0].glyph.glyph!=4||records[1].glyph.glyph!=3)return SfntStatus::malformed;
    const auto saved=records[0];records[0]=records[1];records[1]=saved;return SfntStatus::ok;
}
template<u32 N> ConstSlice<u8> encode(const u16 (&words)[N],u8* bytes) noexcept {
    for(u32 i=0;i<N;++i){bytes[i*2]=static_cast<u8>(words[i]>>8);bytes[i*2+1]=static_cast<u8>(words[i]);}return {bytes,N*2};
}
bool validation_intervals() noexcept {
    // Three edits separated by no-op roots. A newly introduced glyph must be
    // seen by later roots even while whole-run validation is reused.
    const u16 words[5][6]{{1,6,8,1,1,1},{1,6,1,1,1,1},{1,6,1,1,1,9},{1,6,1,1,1,1},{1,6,1,1,1,10}};
    u8 data[5][12]{};SfntGsubExecutableSubtable subtables[5]{};SfntGsubExecutableLookup lookups[5]{};
    u16 roots[]{0,1,2,3,4};SfntLookupStep steps[]{{0,0,0},{1,0,0},{2,0,0},{3,0,0},{4,0,1}};
    for(u32 i=0;i<5;++i){CHECK(sfnt_gsub_subtable_open(encode(words[i],data[i]),1,16,&subtables[i].basic)==SfntStatus::ok);
        lookups[i].type=1;lookups[i].subtables={&subtables[i],1};sfnt_lookup_bounds_add(lookups[i].bounds,subtables[i].basic.coverage);}
    SfntGsubProgram program{{lookups,5},{},16};SfntShapePlan plan{};plan.substitutions=&program;plan.substitution_roots={roots,5};plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* out) noexcept {*out=100;return SfntStatus::ok;};
    const SfntClusterGlyph input[]{{{1,SfntGlyphClass::base,0},0}};const u32 boundaries[]{0};SfntShapeRunResult result{};
    for(u32 refresh=0;refresh<2;++refresh)for(u32 ready=0;ready<2;++ready){plan.refresh_substitution_classes=refresh!=0;
        for(auto& lookup:lookups)lookup.bounds.ready=static_cast<u16>(ready);
        const auto shape=[&](u64 budget) noexcept {return sfnt_shape_run(plan,{input,1},1,{boundaries,1},false,1000,storage(),&result,{budget,100000,1000000});};
        CHECK(shape(100000)==SfntStatus::ok&&result.substitution_edits==3&&result.glyph_count==1&&glyphs[0].glyph.glyph==11);
        CHECK(characters[0]==0&&placements[0].advance==100);const u64 full_work=result.lookup_operations;
        for(u64 budget=0;budget<=full_work;++budget){const auto status=shape(budget);CHECK(result.lookup_operations<=budget);
            CHECK(status==(budget==full_work?SfntStatus::ok:SfntStatus::resource_limit));}
    }
    // A prepared subtable from a larger font cannot publish an out-of-range
    // replacement, including through the validated continuation.
    for(u32 limit=9;limit<=10;++limit){program.glyph_limit=limit;plan.substitution_roots={roots,limit==9?1u:3u};
        CHECK(sfnt_shape_run(plan,{input,1},1,{boundaries,1},false,1000,storage(),&result)==SfntStatus::invalid_input);
        CHECK(!result.glyph_count&&glyphs[0].glyph.glyph==(limit==9?1:9));}
    program.glyph_limit=16;
    // A callback after substitutions invalidates that permission again. Reject
    // corrupt source, glyph, class and component IDs before the final lookup.
    plan.substitution_roots={};plan.refresh_substitution_classes=false;
    for(u32 mode=5;mode<=9;++mode){Policy policy{};policy.mode=mode;SfntShapeStagePolicy stages{&policy,stage,2};
        CHECK(sfnt_shape_run(plan,{input,1},1,{boundaries,1},false,1000,storage(),&result,{}, {steps,5},{},{},nullptr,&stages)==SfntStatus::invalid_input);
        CHECK(policy.calls==2&&!result.glyph_count);}
    return true;
}
bool run() noexcept {
    CHECK(validation_intervals());
    // First ligature: 1,2 -> 4. After script reordering, 3,4 -> 5.
    const u16 first[]{1,8,1,14,1,1,1,1,4,4,2,2};
    const u16 second[]{1,8,1,14,1,1,3,1,4,5,2,4};u8 data[2][32]{};
    SfntGsubExecutableSubtable subtables[2]{};SfntGsubExecutableLookup lookups[2]{};
    CHECK(sfnt_gsub_subtable_open(encode(first,data[0]),4,16,&subtables[0].basic)==SfntStatus::ok);
    CHECK(sfnt_gsub_subtable_open(encode(second,data[1]),4,16,&subtables[1].basic)==SfntStatus::ok);
    for(u32 i=0;i<2;++i){lookups[i].subtables={&subtables[i],1};lookups[i].type=4;lookups[i].bounds.ready=1;sfnt_lookup_bounds_add(lookups[i].bounds,subtables[i].basic.coverage);}
    SfntGsubProgram program{{lookups,2},{},16};SfntShapePlan plan{};plan.substitutions=&program;plan.units_per_em=1000;
    plan.advance=+[](void*,u16,double* out) noexcept {*out=100;return SfntStatus::ok;};
    const SfntClusterGlyph input[]{{{1,SfntGlyphClass::base,0},0},{{2,SfntGlyphClass::base,0},1},{{3,SfntGlyphClass::base,0},2}};
    const u32 boundaries[]{0};const SfntLookupStep steps[]{{0,0,0},{1,0,4}};
    Policy policy{};SfntShapeStagePolicy stages{&policy,stage,6};SfntShapeRunResult result{};
    const auto shape=[&](u64 budget=1000000) noexcept {return sfnt_shape_run(plan,{input,3},3,{boundaries,1},false,1000,storage(),&result,{budget,100000,1000000},{steps,2},{},{},nullptr,&stages);};
    CHECK(shape()==SfntStatus::ok&&result.glyph_count==1&&glyphs[0].glyph.glyph==5&&result.substitution_edits==2);
    CHECK(result.source_units==3&&characters[0]==0&&characters[1]==0&&characters[2]==0&&placements[0].advance==100);
    CHECK(policy.calls==6);for(u16 i=0;i<6;++i)CHECK(policy.visited[i]==i);
    const u64 full_work=result.lookup_operations;
    // Deferred placement keeps the completed shaping/positioning state, while
    // leaving final source maps and placement buffers untouched until exported.
    const auto expected_position=positions_out[0];const auto expected_placement=placements[0];
    placements[0]={-17,-19,-23};const auto sentinel=placements[0];policy={};
    for(u32 i=0;i<3;++i)characters[i]=0xffffffffu;
    CHECK(sfnt_shape_run(plan,{input,3},3,{boundaries,1},false,1000,storage(),&result,{}, {steps,2},{},{},nullptr,&stages,true)==SfntStatus::ok);
    CHECK(result.glyph_count==1&&result.source_units==3&&result.lookup_operations==full_work&&glyphs[0].glyph.glyph==5);
    CHECK(bytes_equal(&positions_out[0],&expected_position,sizeof(expected_position))&&bytes_equal(&placements[0],&sentinel,sizeof(sentinel)));
    CHECK(characters[0]==0xffffffffu&&characters[1]==0xffffffffu&&characters[2]==0xffffffffu);
    CHECK(sfnt_position_finalize({positions_out,1},{flags,1},1000,plan.units_per_em,{placements,1})==SfntStatus::ok);
    CHECK(bytes_equal(&placements[0],&expected_placement,sizeof(expected_placement)));
    for(u64 budget=0;budget<=full_work;++budget){policy={};const auto status=shape(budget);
        CHECK(result.lookup_operations<=budget);CHECK(status==(budget==full_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units);}
    policy={};policy.mode=1;CHECK(shape()==SfntStatus::unsupported_sequence&&!result.glyph_count&&!result.source_units&&policy.calls==2);
    policy={};policy.mode=2;CHECK(shape()==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    policy={};stages.count=0;CHECK(shape()==SfntStatus::invalid_input&&!policy.calls);
    stages.count=4;CHECK(shape()==SfntStatus::invalid_input&&!policy.calls);
    stages.count=257;CHECK(shape()==SfntStatus::invalid_input&&!policy.calls);stages.count=6;
    const SfntLookupStep backwards[]{{0,0,4},{1,0,0}};
    CHECK(sfnt_shape_run(plan,{input,3},3,{boundaries,1},false,1000,storage(),&result,{}, {backwards,2},{},{},nullptr,&stages)==SfntStatus::invalid_input);
    CHECK(sfnt_shape_run(plan,{input,3},3,{boundaries,1},false,1000,storage(),&result,{}, {steps,2})==SfntStatus::invalid_input);
    // Empty stages still run, including when the font has no substitutions.
    plan.substitutions=nullptr;policy={};policy.mode=3;stages.count=256;
    CHECK(sfnt_shape_run(plan,{input,1},1,{boundaries,1},false,1000,storage(),&result,{}, {},{},{},nullptr,&stages)==SfntStatus::ok);
    CHECK(policy.calls==256&&result.lookup_operations==256&&result.glyph_count==1);
    for(u16 i=0;i<256;++i)CHECK(policy.visited[i]==i);
    policy.calls=0;CHECK(sfnt_shape_run(plan,{},0,{},false,1000,storage(),&result,{}, {},{},{},nullptr,&stages)==SfntStatus::ok&&!policy.calls);
    // A callback introducing glyph 9 must invalidate cached coverage bounds:
    // the preceding no-op lookup saw only glyph 1; the next must yield 10.
    const u16 single[]{1,6,1,1,1,9};
    for(u32 i=0;i<2;++i){CHECK(sfnt_gsub_subtable_open(encode(single,data[i]),1,16,&subtables[i].basic)==SfntStatus::ok);lookups[i].type=1;lookups[i].bounds={9,9,1};}
    plan.substitutions=&program;policy={};policy.mode=4;stages.count=6;
    CHECK(sfnt_shape_run(plan,{input,1},1,{boundaries,1},false,1000,storage(),&result,{}, {steps,2},{},{},nullptr,&stages)==SfntStatus::ok);
    CHECK(result.glyph_count==1&&glyphs[0].glyph.glyph==10&&result.substitution_edits==1);
    const u16 root[]{0};plan.substitution_roots={root,1};policy.calls=0;
    CHECK(sfnt_shape_run(plan,{input,1},1,{boundaries,1},false,1000,storage(),&result,{}, {},{},{},nullptr,&stages)==SfntStatus::invalid_input&&!policy.calls);
    print("PASS staged substitution ordering, provenance, empty stages, bounds invalidation and work limits\n");return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_shape_stage_entry() noexcept {ExitProcess(maicord::run()?0u:1u);}
