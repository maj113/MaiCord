// Arabic joining plans. No argument runs the built-in corpora with the test's own prepared
// programs; --run first shapes them through the owned plan store and native_arabic_run.
#include "gsub_program_fixture.hpp"
#include "maicord/text/shape_run.hpp"
#include "maicord/text/arabic_shape_run.hpp"
#include "maicord/text/arabic_joining.hpp"
#include "maicord/text/grapheme.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/text/arabic_run_plan.hpp"
#include "maicord/text/arabic_run.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/byte_order.hpp"
#include "text_test_support.hpp"

namespace maicord {namespace {
#include "generated/arabic_plan_fixture.inc"
#include "generated/arabic_control_fixture.inc"
#include "generated/arabic_dotted_fixture.inc"
#include "generated/arabic_cgj_fixture.inc"
#include "generated/arabic_ltr_fixture.inc"
#include "generated/arabic_ltr_cgj_fixture.inc"
#include "generated/arabic_ltr_cgj_position_fixture.inc"
#include "generated/arabic_generic_fixture.inc"
struct Reader {ConstSlice<u8> bytes;u32 at=0;
    bool word(u32* out) noexcept {if(at>bytes.count || bytes.count-at<4)return false;*out=byte_load_le32(bytes.data+at);at+=4;return true;}
    bool take(u32 count,ConstSlice<u8>* out) noexcept {if(at>bytes.count || count>bytes.count-at)return false;*out={bytes.data+at,count};at+=count;return true;}
};
bool fail(u32 c,u32 code,u32 value=0) noexcept {print(utf8_literal("FAIL case="));number(c);print(utf8_literal(" code="));number(code);print(utf8_literal(" value="));number(value);print(utf8_literal("\n"));return false;}
SfntGsubExecutableLookup gs_lookups[256];SfntGsubExecutableSubtable gs_subs[512];SfntCoverage gs_cov[4096];SfntClassDef gs_classes[512];
SfntGposExecutableLookup gp_lookups[256];SfntGposSubtable gp_basic[512];SfntGposAttachment gp_attach[512];SfntContext gp_context[512];SfntCoverage gp_cov[4096];SfntClassDef gp_classes[512];
SfntGsubExecutionFrame gs_frames[16];u32 gs_positions[512],gs_rebased[512],gs_consumed[128];SfntComponentGlyph glyphs[256],replacements[128];SfntComponentNode nodes[128];SfntComponentUndo journal[4096];
SfntGposExecutionFrame gp_frames[16];u32 gp_positions[512];SfntCursiveChainNode chains[256];
ConstSlice<u8> fonts[512];
u64 runs=0,glyph_checks=0,cluster_checks=0,float_checks=0,budget_checks=0;
u64 utf8_calls=0;
bool run_mode=false;
#include "arabic_owned_units.inc"
#include "arabic_run_units.inc"
bool dotted_storage_checks(const SfntShapePlan& plan,const SfntCmap& cmap,SfntShapeStorage storage) noexcept {
    const u32 scalars[]{0x64e,0x651,0x628},sources[]{0,4,8},boundaries[]{0,8};SfntClusterGlyph input[3];
    for(u32 i=0;i<3;++i){u16 glyph=0;if(sfnt_cmap_glyph(cmap,scalars[i],0,&glyph)!=SfntStatus::ok)return false;input[i]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),0},sources[i]};}
    auto shape=[&](SfntShapeStorage s,ConstSlice<u32> starts,SfntShapeRunResult* result) noexcept {return sfnt_arabic_shape_run(plan,cmap,{input,3},{scalars,3},10,starts,true,1000,s,result,{});};
    u16 circle=0;if(sfnt_cmap_glyph(cmap,0x25cc,0,&circle)!=SfntStatus::ok)return false;SfntShapeRunResult result{};
    for(u32 repeat=0;repeat<2;++repeat){copy_bytes(storage.targets.data,boundaries,sizeof(boundaries));if(shape(storage,{storage.targets.data,2},&result)!=SfntStatus::ok || result.glyph_count!=5)return false;
        if(storage.glyphs[0].glyph.glyph!=circle || storage.glyphs[1].glyph.glyph!=input[1].glyph.glyph || storage.glyphs[2].glyph.glyph!=circle || storage.glyphs[3].glyph.glyph!=input[0].glyph.glyph)return false;
        for(u32 i=0;i<10;++i)if(storage.character_glyphs[i]!=(i<4?0u:i<8?2u:4u))return false;
    }
    for(u32 kind=0;kind<4;++kind){auto small=storage;SfntStatus expected=SfntStatus::output_too_small;
        if(kind==0)small.substitutions.replacements.count=5*sizeof(SfntClusterGlyph)-1;
        else if(kind==1)small.glyphs.count=4;else if(kind==2)small.components.count=4;
        else{++small.substitutions.replacements.data;--small.substitutions.replacements.count;expected=SfntStatus::invalid_input;}
        result.glyph_count=99;if(shape(small,{boundaries,2},&result)!=expected || result.glyph_count || result.source_units)return false;++budget_checks;
    }
    // The input remains available for a complete retry after any failed attempt.
    for(u32 i=0;i<3;++i)if(input[i].source!=sources[i])return false;
    return shape(storage,{boundaries,2},&result)==SfntStatus::ok;
}
bool control_storage_checks(const SfntShapePlan& plan,const SfntCmap& cmap,SfntShapeStorage storage) noexcept {
    const u32 scalars[]{0x628,0x200c,0x628},sources[]{0,4,8},boundaries[]{0,8};SfntClusterGlyph input[3];
    for(u32 i=0;i<3;++i){u16 glyph=0;if(sfnt_cmap_glyph(cmap,scalars[i],0,&glyph)!=SfntStatus::ok)return false;input[i]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),0},sources[i]};}
    auto shape=[&](SfntShapeStorage s,ConstSlice<u32> starts,SfntShapeRunResult* result,SfntShapeLimits limits=SfntShapeLimits{}) noexcept {
        return sfnt_arabic_shape_run(plan,cmap,{input,3},{scalars,3},10,starts,true,1000,s,result,{},limits);
    };
    u16 space=0;if(sfnt_cmap_glyph(cmap,32,0,&space)!=SfntStatus::ok)return false;SfntShapeRunResult result{};
    if(shape(storage,{boundaries,2},&result)!=SfntStatus::ok || result.glyph_count!=3 || result.source_units!=10 || storage.glyphs[1].glyph.glyph!=space || storage.placements[1].advance)return false;
    for(u32 i=0;i<10;++i)if(storage.character_glyphs[i]!=(i<4?0u:i<8?1u:2u))return false;
    // Sparse UTF-16 source positions, borrowed target prefix, and repeat use.
    for(u32 repeat=0;repeat<2;++repeat){copy_bytes(storage.targets.data,boundaries,sizeof(boundaries));if(shape(storage,{storage.targets.data,2},&result)!=SfntStatus::ok)return false;for(u32 i=0;i<10;++i)if(storage.character_glyphs[i]!=(i<4?0u:i<8?1u:2u))return false;}
    for(u32 kind=0;kind<4;++kind){auto small=storage;SfntShapeLimits limits{};SfntStatus expected=SfntStatus::output_too_small;
        if(kind==0)small.advances.count=1;else if(kind==1)small.targets.count=2;else if(kind==2)small.character_glyphs.count=2;else{limits.stage_work=47;expected=SfntStatus::resource_limit;}
        result.glyph_count=99;result.source_units=99;if(shape(small,{boundaries,2},&result,limits)!=expected || result.glyph_count || result.source_units)return false;++budget_checks;
    }
    input[2].source=10;if(shape(storage,{boundaries,2},&result)!=SfntStatus::invalid_input || result.glyph_count)return false;
    input[2].source=2;if(shape(storage,{boundaries,2},&result)!=SfntStatus::invalid_input || result.glyph_count)return false;input[2].source=8;
    const u32 repeated[]{0,0};if(shape(storage,{repeated,2},&result)!=SfntStatus::invalid_input || result.glyph_count)return false;
    return shape(storage,{boundaries,2},&result)==SfntStatus::ok;
}
// Fixed component transactions run once in the base plan variant.
bool component_transactions() noexcept {
    SfntComponentGlyph initial[3]{{{2,SfntGlyphClass::base,8},0,0,0},{{2,SfntGlyphClass::base,4},1,0,0},{{2,SfntGlyphClass::base,2},2,0,0}};
    SfntComponentNode saved[3],joined[3];SfntComponentForest forest{};u32 consumed_indices[]{0,1,2};
    if(sfnt_components_open({initial,3},{saved,3},&forest,100,true)!=SfntStatus::ok)return false;
    for(u32 i=0;i<3;++i)saved[i].reserved|=static_cast<u16>((i+1)<<8);
    const auto original=forest;SfntComponentGlyph combined=initial[0];SfntComponentEdit complete{};
    if(sfnt_components_edit(&forest,sfnt_glyph_view({}),{},true,{}, {},&complete)!=SfntStatus::invalid_input)return false;
    const SfntGlyphView view{&initial[0].glyph,nullptr,3,0,sizeof(SfntComponentGlyph)};
    copy_bytes(nodes,saved,sizeof(saved));forest.nodes={nodes,3};
    if(sfnt_components_edit(&forest,view,{consumed_indices,3},true,{reinterpret_cast<u8*>(&combined),sizeof(combined)},{journal,1024},&complete)!=SfntStatus::ok || combined.glyph.reserved!=2)return false;
    copy_bytes(joined,nodes,sizeof(joined));const u64 join_work=complete.operations;
    u8 metadata=0;u64 metadata_work=0;
    if(sfnt_component_first_metadata(forest,combined.component,&metadata,100,&metadata_work)!=SfntStatus::ok||metadata!=1||!metadata_work)return false;
    for(u64 work=0;work<=metadata_work;++work){u64 used=99;metadata=99;++budget_checks;
        const auto status=sfnt_component_first_metadata(forest,combined.component,&metadata,work,&used);
        if(status!=(work<metadata_work?SfntStatus::resource_limit:SfntStatus::ok)||used!=work||metadata!=(work<metadata_work?0u:1u))return false;}
    for(u64 work=0;work<=join_work;++work) {
        copy_bytes(nodes,saved,sizeof(saved));forest=original;forest.nodes={nodes,3};SfntComponentGlyph output=initial[0];SfntComponentEdit result{};
        const auto s=sfnt_components_edit(&forest,view,{consumed_indices,3},true,{reinterpret_cast<u8*>(&output),sizeof(output)},{journal,1024},&result,work);++budget_checks;
        if(s!=(work<join_work?SfntStatus::resource_limit:SfntStatus::ok) || result.operations>work)return false;
        if(s==SfntStatus::ok){if(output.glyph.reserved!=2 || !bytes_equal(nodes,joined,sizeof(joined)))return false;sfnt_components_rollback(&forest,{journal,result.undo_count});}
        if(!bytes_equal(nodes,saved,sizeof(saved)))return false;
    }
    copy_bytes(nodes,joined,sizeof(joined));forest.nodes={nodes,3};const SfntGlyphView joined_view{&combined.glyph,nullptr,1,0,sizeof(SfntComponentGlyph)};
    SfntComponentGlyph output[4];for(auto& g:output)g=combined;SfntComponentEdit split{};
    if(sfnt_components_edit(&forest,joined_view,{consumed_indices,1},false,{reinterpret_cast<u8*>(output),sizeof(output)},{journal,1024},&split)!=SfntStatus::ok)return false;
    const u8 forms[]{8,4,2,2};const u32 sources[]{0,1,2,2};
    for(u32 i=0;i<4;++i){if(output[i].glyph.reserved!=forms[i] || output[i].source!=sources[i])return false;
        if(sfnt_component_first_metadata(forest,output[i].component,&metadata)!=SfntStatus::ok||metadata!=(i<3?i+1:0u))return false;}
    for(u64 work=0;work<=split.operations;++work) {
        copy_bytes(nodes,joined,sizeof(joined));for(auto& g:output)g=combined;SfntComponentEdit result{};
        const auto s=sfnt_components_edit(&forest,joined_view,{consumed_indices,1},false,{reinterpret_cast<u8*>(output),sizeof(output)},{journal,1024},&result,work);++budget_checks;
        if(s!=(work<split.operations?SfntStatus::resource_limit:SfntStatus::ok) || result.operations>work)return false;
        if(s==SfntStatus::ok){for(u32 i=0;i<4;++i)if(output[i].glyph.reserved!=forms[i] || output[i].source!=sources[i])return false;sfnt_components_rollback(&forest,{journal,result.undo_count});}
        if(!bytes_equal(nodes,joined,sizeof(joined)))return false;
    }
    return true;
}
#include "arabic_cgj_units.inc"
bool plan_checks(const SfntLayout& layout,bool independent,u8* scratch,const SfntLookupPlanResult& full,ConstSlice<SfntLookupStep> expected,bool budgets) noexcept {
    SfntLookupStep output[768];SfntLookupPlanResult result{};
    if(full.operations>10000 || expected.count>768)return false;
    if(budgets)for(u64 work=0;work<=full.operations;++work) {
        for(auto& step:output)step={65535,255,255};
        const auto s=sfnt_arabic_lookup_plan(layout,{},independent,{scratch,256},{output,768},&result,work);++budget_checks;
        if(s!=(work<full.operations?SfntStatus::resource_limit:SfntStatus::ok) || result.operations>work)return false;
        if(s==SfntStatus::ok){if(result.lookup_count!=expected.count || !bytes_equal(output,expected.data,expected.count*4))return false;}
        else for(const auto& step:output)if(step.lookup!=65535 || step.feature_mask!=255 || step.stage!=255)return false;
    }
    if(expected.count) {
        for(auto& step:output)step={65535,255,255};
        if(sfnt_arabic_lookup_plan(layout,{},independent,{scratch,256},{output,expected.count-1},&result)!=SfntStatus::output_too_small || result.lookup_count!=expected.count)return false;
        for(const auto& step:output)if(step.lookup!=65535 || step.feature_mask!=255 || step.stage!=255)return false;
    }
    return true;
}
bool corpus(ConstSlice<u8> corpus_bytes,bool force_arabic=false) noexcept {
    if(run_mode && !owned_close())return false;
    if(corpus_bytes.count<16 || !bytes_equal(corpus_bytes.data,"MCARPL",6) || corpus_bytes.data[6]<'1' || corpus_bytes.data[6]>'3' || corpus_bytes.data[7])return false;
    const bool sized=corpus_bytes.data[6]>='2',directed=corpus_bytes.data[6]>='3';Reader r{corpus_bytes,8};u32 font_count=0,cases=0;
    if(!r.word(&font_count)||font_count>512||!r.word(&cases))return false;
    for(u32 i=0;i<font_count;++i){u32 n=0;if(!r.word(&n)||!r.take(n,&fonts[i]))return false;}
    for(u32 c=0;c<cases;++c) {
        u32 f=0,independent=0,n=0,expected_count=0,em_bits=1148846080,rtl=1;ConstSlice<u8> scalar_bytes{},expected{},clusters{},floats{};
        if((sized && !r.word(&em_bits))||(directed && !r.word(&rtl))||rtl>1||!r.word(&f)||f>=font_count||!r.word(&independent)||independent>1||!r.word(&n)||n>128||!r.word(&expected_count)||expected_count>256||
           !r.take(n*4,&scalar_bytes)||!r.take(expected_count*2,&expected)||!r.take(n*4,&clusters)||!r.take(expected_count*12,&floats))return false;
        SfntFace face{};SfntCmap cmap{};SfntHorizontalMetrics metrics{};SfntGdef gdef{};ConstSlice<u8> gdef_bytes{};
        if(sfnt_open(fonts[f],0,&face)!=SfntStatus::ok || sfnt_cmap_open(face,&cmap)!=SfntStatus::ok || sfnt_horizontal_metrics_open(face,&metrics)!=SfntStatus::ok ||
           sfnt_table(face,sfnt_tag('G','D','E','F'),&gdef_bytes)!=SfntStatus::ok || sfnt_gdef_open(gdef_bytes,metrics.glyph_count,&gdef)!=SfntStatus::ok)return fail(c,1);
        SfntGsubProgram gs{};SfntGposProgram gp{};SfntLookupStep steps[768],gp_steps[768];u16 gp_roots[768];u8 scratch[256];u32 gs_count=0,gp_count=0;bool generic=false;
        for(u32 kind=0;kind<2;++kind) {
            ConstSlice<u8> bytes{};const auto table=sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&bytes);
            if(table==SfntStatus::missing_table)continue;if(table!=SfntStatus::ok)return fail(c,2);
            SfntLayout layout{};SfntLookupPlanResult result{};auto* out=kind?gp_steps:steps;
            if(sfnt_layout_open(bytes,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout)!=SfntStatus::ok ||
               sfnt_arabic_lookup_plan(layout,{},independent!=0,{scratch,256},{out,768},&result)!=SfntStatus::ok)return fail(c,3,kind);
            if(!kind)generic=result.script_tag==sfnt_tag('l','a','t','n');
            if(!plan_checks(layout,independent!=0,scratch,result,{out,result.lookup_count},c%17==0))return fail(c,4,kind);
            if(kind) {
                gp_count=result.lookup_count;for(u32 i=0;i<gp_count;++i){if(out[i].feature_mask || out[i].stage)return fail(c,5);gp_roots[i]=out[i].lookup;}
                if(sfnt_gpos_program_prepare(layout,gdef,metrics.glyph_count,{{gp_lookups,256},{gp_basic,512},{gp_attach,512},{gp_context,512},{gp_cov,4096},{gp_classes,512}},&gp)!=SfntStatus::ok)return fail(c,6);
            } else {
                gs_count=result.lookup_count;if(prepare_gsub_program(layout,gdef,metrics.glyph_count,{{gs_lookups,256},{gs_subs,512},{gs_cov,4096},{gs_classes,512}},&gs)!=SfntStatus::ok)return fail(c,7);
            }
        }
        (void)generic;
        u32 scalars[128],starts[128],start_count=0;SfntClusterGlyph input[128];UnicodeGraphemeState graphemes{};bool arabic=force_arabic;
        for(u32 i=0;i<n;++i){scalars[i]=byte_load_le32(scalar_bytes.data+i*4);u16 glyph=0;if(sfnt_cmap_glyph(cmap,scalars[i],0,&glyph)!=SfntStatus::ok)return fail(c,8);input[i]={{glyph,sfnt_gdef_glyph_class(gdef,glyph),0},i};if(unicode_grapheme_boundary(graphemes,scalars[i]))starts[start_count++]=i;}
        // This corpus consists of homogeneous Arabic runs or isolated Common/
        // Inherited marks. Resolve that distinction from native Unicode data,
        // never from a reference script ID or expected output.
        for(u32 i=0;i<n;++i)if(unicode_properties(scalars[i])->script()==UnicodeScript::arab)arabic=true;
        char utf8[512];u32 utf8_count=0;
        if(run_mode)for(u32 i=0;i<n;++i){const u8 encoded=utf8_encode(scalars[i],utf8+utf8_count);if(!encoded || utf8_count+encoded>sizeof(utf8))return fail(c,18);utf8_count+=encoded;}
        if(arabic_joining_forms({scalars,n},{&input[0].glyph.reserved,sizeof(input)-3},sizeof(input[0]))!=ArabicJoiningStatus::ok)return fail(c,9);
        SfntShapePlan plan{};plan.substitutions=&gs;plan.positioning=&gp;plan.positioning_roots={gp_roots,gp_count};plan.gdef=gdef;plan.units_per_em=metrics.units_per_em;
        plan.metric_context=&metrics;plan.advance=+[](void* context,u16 glyph,double* value) noexcept {SfntGlyphMetrics m{};const auto s=sfnt_horizontal_metrics(*static_cast<SfntHorizontalMetrics*>(context),glyph,&m);*value=m.advance;return s;};
        ConstSlice<SfntLookupStep> scheduled{steps,gs_count};
        if(run_mode){NativeArabicPlanView owned{};if(!owned_select(fonts[f],f,independent!=0,&owned))return fail(c,16);
            if(owned.generic!=generic)return fail(c,17);
            if(generic){if(owned.steps.count)return fail(c,17);}
            else if(owned.steps.count!=gs_count || !bytes_equal(owned.steps.data,steps,gs_count*sizeof(SfntLookupStep)) || owned.shape->positioning_roots.count!=gp_count || !bytes_equal(owned.shape->positioning_roots.data,gp_roots,gp_count*2))return fail(c,17);
            plan=*owned.shape;scheduled=owned.steps;cmap=*owned.cmap;}
        u64 bitmap[8],chain_bits[4];u32 character_map[256],targets[256];SfntMarkAssociation associations[256];u8 flags[256];double advances[256],prefix[256];SfntPosition positions[256];SfntPlacement placements[256];float em=0;copy_bytes(&em,&em_bits,4);
        SfntShapeStorage storage{{glyphs,256},{nodes,128},{bitmap,8},{character_map,256},{targets,256},{associations,256},{advances,256},{flags,256},{positions,256},{placements,256},
            {{gs_frames,16},{gs_positions,512},{gs_rebased,512},{gs_consumed,128},{reinterpret_cast<u8*>(replacements),sizeof(replacements)},{journal,4096}},
            {{gp_frames,16},{gp_positions,512},{chain_bits,4},{prefix,256},{chains,256}}};
        for(u32 gate=run_mode?1u:0u;gate<2;++gate) {
            for(u32 i=0;i<gs.lookups.count;++i)gs_lookups[i].bounds.ready=static_cast<u16>(gate);
            for(u32 i=0;i<gp.lookups.count;++i)gp_lookups[i].bounds.ready=static_cast<u16>(gate);
            SfntShapeRunResult result{};
            if(run_mode&&arabic)++utf8_calls;
            const auto s=!arabic?
                sfnt_shape_run(plan,{input,n},n,{starts,start_count},rtl!=0,em,storage,&result,{},scheduled):
                run_mode?native_arabic_run(owned_store,owned_handle,{utf8,utf8_count},independent!=0,rtl!=0,em,storage,&result):
                sfnt_arabic_shape_run(plan,cmap,{input,n},{scalars,n},n,{starts,start_count},rtl!=0,em,storage,&result,scheduled);
            if(s!=SfntStatus::ok || result.glyph_count!=expected_count)return fail(c,10,static_cast<u32>(s));
            for(u32 i=0;i<expected_count;++i){++glyph_checks;if(glyphs[i].glyph.glyph!=byte_load_le16(expected.data+i*2))return fail(c,11,i);}
            for(u32 i=0;i<n;++i){++cluster_checks;if(character_map[i]!=byte_load_le32(clusters.data+i*4))return fail(c,12,i);}
            for(u32 i=0;i<expected_count*3;++i){++float_checks;if(byte_load_le32(reinterpret_cast<const u8*>(placements)+i*4)!=byte_load_le32(floats.data+i*4))return fail(c,13,i);}
            ++runs;
            if(gs_count && c%17==0 && result.lookup_operations){const u64 budget=result.lookup_operations-1;
                const auto limited=!arabic?
                    sfnt_shape_run(plan,{input,n},n,{starts,start_count},rtl!=0,em,storage,&result,{budget,100000,1000000},scheduled):
                    run_mode?native_arabic_run(owned_store,owned_handle,{utf8,utf8_count},independent!=0,rtl!=0,em,storage,&result,{budget,100000,1000000}):
                    sfnt_arabic_shape_run(plan,cmap,{input,n},{scalars,n},n,{starts,start_count},rtl!=0,em,storage,&result,scheduled,{budget,100000,1000000});
                if(limited!=SfntStatus::resource_limit || result.glyph_count || result.lookup_operations>budget)return fail(c,14);}
        }
        if(directed && !c && (!control_storage_checks(plan,cmap,storage) || !dotted_storage_checks(plan,cmap,storage) || !cgj_storage_checks(plan,cmap,storage)))return fail(c,15);
        if(run_mode && !c && !utf8_run_units(storage))return fail(c,19);
    }
    print(utf8_literal("cases="));number(cases);print(utf8_literal(" runs="));number(runs);print(utf8_literal(" glyph_checks="));number(glyph_checks);print(utf8_literal(" cluster_checks="));number(cluster_checks);print(utf8_literal(" float_checks="));number(float_checks);print(utf8_literal(" budget_checks="));number(budget_checks);print(utf8_literal("\n"));
    if(run_mode && !owned_close())return false;
    return r.at==r.bytes.count;
}
int run() noexcept {
    const wchar_t* command=test_arguments();
    {constexpr wchar_t flag[]=L"--run";u32 i=0;while(flag[i]&&command[i]==flag[i])++i;
        if(!flag[i]&&(!command[i]||command[i]==' '||command[i]=='\t')){run_mode=true;command+=i;while(*command==' ' || *command=='\t')++command;}}
    if(!run_mode && !component_transactions())return 2;
    if(run_mode && !owned_start())return 3;
    bool ok=
        (run_mode || corpus({arabic_plan_fixture,sizeof(arabic_plan_fixture)})) &&
        corpus({arabic_control_fixture,sizeof(arabic_control_fixture)}) && corpus({arabic_dotted_fixture,sizeof(arabic_dotted_fixture)}) && corpus({arabic_cgj_fixture,sizeof(arabic_cgj_fixture)}) && corpus({arabic_ltr_fixture,sizeof(arabic_ltr_fixture)},true) && corpus({arabic_ltr_cgj_fixture,sizeof(arabic_ltr_cgj_fixture)},true) && corpus({arabic_ltr_cgj_position_fixture,sizeof(arabic_ltr_cgj_position_fixture)},true)
        && (!run_mode || corpus({arabic_generic_fixture,sizeof(arabic_generic_fixture)},true));
    if(run_mode){
        if(!owned_gate_checks(fonts[0]))ok=false;
        if(!owned_close())ok=false;arena_release(&owned_scratch);if(!DeleteFileW(owned_path))ok=false;
        print(utf8_literal("owned_preparations="));number(owned_preparations);print(utf8_literal(" owned_hits="));number(owned_hits);print(utf8_literal(" committed_sum="));number(owned_committed);print(utf8_literal(" used_sum="));number(owned_used);print(utf8_literal("\n"));
        print(utf8_literal("utf8_calls="));number(utf8_calls);print(utf8_literal("\n"));
    }
    if(!ok)return 1;print(utf8_literal("PASS native Arabic schedule and coordinated shaping\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_arabic_plan_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
