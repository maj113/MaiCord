// Manual: --backtrack-cost measures the backtracking context cost; the registered run is the check.
#include "maicord/text/gsub_execute.hpp"
#include "maicord/text/gpos_ligature_execute.hpp"
#include "maicord/text/gpos_cursive_execute.hpp"
#include "maicord/text/gpos_program.hpp"
#include "maicord/text/shape_run.hpp"
#include "maicord/text/run_plan.hpp"
#include "maicord/text/grapheme.hpp"
#include "maicord/core/byte_order.hpp"
#include "../src/text/substitution_selection.hpp"
#include "../src/text/run_profile.hpp"
#include "maicord/text/nominal_participation.hpp"
namespace maicord {namespace {
#include "generated/component_shaping_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
bool error(u32 font,u32 split,Utf8View what,u32 value=0) noexcept {print(what);print(utf8_literal(" font="));number(font);print(utf8_literal(" split="));number(split);print(utf8_literal(" value="));number(value);print(utf8_literal("\n"));return false;}
struct Reader {ConstSlice<u8> data;u32 at=0;
    bool word(u32* v) noexcept {if(at>data.count || data.count-at<4)return false;*v=byte_load_le32(data.data+at);at+=4;return true;}
    bool take(u32 n,ConstSlice<u8>* v) noexcept {if(at>data.count || n>data.count-at)return false;*v={data.data+at,n};at+=n;return true;}
};
SfntGsubExecutableLookup gsub_lookups[32];SfntGsubExecutableSubtable gsub_subtables[64];SfntCoverage coverages[256];SfntClassDef classes[64];
SfntGsubExecutionFrame frames[32];u32 positions[256],rebased[256],consumed[64];SfntComponentGlyph glyphs[128],replacements[64];SfntComponentNode nodes[64];SfntComponentUndo journal[4096];
SfntGposExecutableLookup gpos_lookups[32];SfntGposSubtable basic_storage[64];SfntGposAttachment attachment_storage[64];SfntContext context_storage[64];SfntCoverage gpos_coverages[256];SfntClassDef gpos_classes[64];
SfntGposExecutableLookup shared_gpos_lookups[32];
SfntGposExecutionFrame gpos_frames[32];u32 gpos_positions[256];SfntCursiveChainNode cursive_chains[64];
u64 gpos_operations=0,gpos_actions=0;u32 gpos_depth=0;
u64 shape_runs=0,glyph_checks=0,cluster_checks=0,flag_checks=0,placement_runs=0,float_checks=0,gsub_operations=0,component_export_work=0;u32 failures=0;
u64 commit_budget_checks=0,commit_journal_checks=0;bool commit_tested=false;
SfntComponentGlyph saved_glyphs[128],committed_glyphs[128];SfntComponentNode saved_nodes[64],committed_nodes[64];
SfntGsubExecutionScratch scratch() noexcept {return {{frames,32},{positions,256},{rebased,256},{consumed,64},{reinterpret_cast<u8*>(replacements),sizeof(replacements)},{journal,4096}};}
bool commit_units(const SfntGsubProgram& program,u16 lookup,SfntGlyphBuffer* buffer,SfntClusterMap* clusters,SfntComponentForest* forest) noexcept {
    const SfntGlyphBuffer initial=*buffer;const u32 n=sfnt_glyph_count(sfnt_glyph_buffer_view(*buffer));u64 saved_map[8],committed_map[8];const u32 map_bytes=clusters->storage.count*sizeof(u64);
    copy_bytes(saved_glyphs,glyphs,sizeof(glyphs));copy_bytes(saved_nodes,nodes,sizeof(nodes));copy_bytes(saved_map,clusters->storage.data,map_bytes);SfntGsubExecutionResult result{};
    if(sfnt_gsub_execute_lookup(program,lookup,buffer,0,n,0,scratch(),{1000000,100000},&result,clusters,forest)!=SfntStatus::ok || result.edits!=1)return false;
    const u64 total=result.operations;const SfntGlyphBuffer committed=*buffer;copy_bytes(committed_glyphs,glyphs,sizeof(glyphs));copy_bytes(committed_nodes,nodes,sizeof(nodes));copy_bytes(committed_map,clusters->storage.data,map_bytes);
    for(u64 budget=0;budget<=total;++budget){*buffer=initial;copy_bytes(glyphs,saved_glyphs,sizeof(glyphs));copy_bytes(nodes,saved_nodes,sizeof(nodes));copy_bytes(clusters->storage.data,saved_map,map_bytes);
        const auto status=sfnt_gsub_execute_lookup(program,lookup,buffer,0,n,0,scratch(),{budget,100000},&result,clusters,forest);++commit_budget_checks;
        if(status!=(budget<total?SfntStatus::resource_limit:SfntStatus::ok) || result.operations>budget || result.edits>1)return false;
        const bool edited=result.edits!=0;
        if(!bytes_equal(glyphs,edited?committed_glyphs:saved_glyphs,sizeof(glyphs)) || !bytes_equal(nodes,edited?committed_nodes:saved_nodes,sizeof(nodes)) || !bytes_equal(clusters->storage.data,edited?committed_map:saved_map,map_bytes) || !bytes_equal(buffer,edited?&committed:&initial,sizeof(initial)))return false;
    }
    bool reached=false;
    for(u32 capacity=0;capacity<64;++capacity){*buffer=initial;copy_bytes(glyphs,saved_glyphs,sizeof(glyphs));copy_bytes(nodes,saved_nodes,sizeof(nodes));copy_bytes(clusters->storage.data,saved_map,map_bytes);auto storage=scratch();storage.component_journal.count=capacity;
        const auto status=sfnt_gsub_execute_lookup(program,lookup,buffer,0,n,0,storage,{1000000,100000},&result,clusters,forest);++commit_journal_checks;
        if(status==SfntStatus::ok){reached=true;break;}
        if(status!=SfntStatus::output_too_small || result.edits || !bytes_equal(glyphs,saved_glyphs,sizeof(glyphs)) || !bytes_equal(nodes,saved_nodes,sizeof(nodes)) || !bytes_equal(clusters->storage.data,saved_map,map_bytes) || !bytes_equal(buffer,&initial,sizeof(initial)))return false;
    }
    *buffer=initial;copy_bytes(glyphs,saved_glyphs,sizeof(glyphs));copy_bytes(nodes,saved_nodes,sizeof(nodes));copy_bytes(clusters->storage.data,saved_map,map_bytes);return reached;
}
bool placements(ConstSlice<u8> expected,ConstSlice<SfntPlacement> values,u32 font,u32 split,u32 config) noexcept {
    for(u32 i=0;i<values.count;++i){u32 bits[3];copy_bytes(bits,&values.data[i],12);for(u32 j=0;j<3;++j){++float_checks;if(bits[j]!=byte_load_le32(expected.data+i*12+j*4)){++failures;if(failures<10)error(font,split,utf8_literal("placement mismatch"),config*100+i*3+j);}}}return true;
}
#include "gpos_program_units.inc"
#include "gpos_backtrack_dispatch_units.inc"
#include "gpos_backtrack_dispatch_cost.inc"
u64 coordinated_runs=0,coordinated_values=0,coordinated_failures=0;
bool coordinated(const SfntCmap& cmap,const SfntHorizontalMetrics& metrics,const SfntGdef& gdef,
    const SfntGsubProgram& sub,const SfntGposProgram& pos,const SfntFeature& gs,const SfntFeature& gp,
    ConstSlice<u8> scalars,u32 units,u32 count,ConstSlice<u8> characters,ConstSlice<u8> expected_glyphs,
    ConstSlice<u8> configs,u32 config_count) noexcept {
    SfntClusterGlyph input[64];u32 starts[64],start_count=0,source=0;UnicodeGraphemeState graphemes{};
    const u32 input_count=scalars.count/4;u16 gs_roots[32],gp_roots[32];
    if(gs.lookup_indices.count>64||gp.lookup_indices.count>64)return false;
    for(u32 i=0;i<gs.lookup_indices.count/2;++i)gs_roots[i]=byte_load_be16(gs.lookup_indices.data+i*2);
    for(u32 i=0;i<gp.lookup_indices.count/2;++i)gp_roots[i]=byte_load_be16(gp.lookup_indices.data+i*2);
    for(u32 i=0;i<input_count;++i){const u32 scalar=byte_load_le32(scalars.data+i*4);u16 glyph=0;
        if(sfnt_cmap_glyph(cmap,scalar,0,&glyph)!=SfntStatus::ok)return false;
        input[i]={{glyph,sfnt_gdef_glyph_class(gdef,glyph),0},source};bool boundary=false;
        if(!unicode_grapheme_next(&graphemes,scalar,&boundary))return false;if(boundary)starts[start_count++]=source;
        source+=scalar>65535?2:1;
    }
    if(source!=units)return false;
    SfntShapePlan plan{};plan.substitutions=&sub;plan.positioning=&pos;plan.gdef=gdef;
    plan.substitution_roots={gs_roots,gs.lookup_indices.count/2};plan.units_per_em=metrics.units_per_em;
    plan.metric_context=const_cast<SfntHorizontalMetrics*>(&metrics);
    plan.advance=+[](void* context,u16 glyph,double* value) noexcept {SfntGlyphMetrics m{};
        const auto status=sfnt_horizontal_metrics(*static_cast<const SfntHorizontalMetrics*>(context),glyph,&m);*value=m.advance;return status;};
    u64 bitmap[8],chain[2];u32 character_map[128],targets[128];SfntMarkAssociation associations[128];
    u8 flags[128];double advances[128],prefix[128];SfntPosition state[128];SfntPlacement output[128];
    SfntShapeStorage storage{{glyphs,128},{nodes,64},{bitmap,8},{character_map,128},{targets,128},
        {associations,128},{advances,128},{flags,128},{state,128},{output,128},scratch(),
        {{gpos_frames,32},{gpos_positions,256},{chain,2},{prefix,128},{cursive_chains,64}}};
    for(u32 gate=0;gate<2;++gate){
    for(u32 i=0;i<sub.lookups.count;++i)gsub_lookups[i].bounds.ready=static_cast<u16>(gate);
    for(u32 i=0;i<pos.lookups.count;++i){const auto* lookup=sfnt_gpos_lookup(pos,static_cast<u16>(i));if(lookup)const_cast<SfntGposExecutableLookup*>(lookup)->bounds.ready=static_cast<u16>(gate);}
    Reader reader{configs};
    for(u32 c=0;c<config_count;++c){u32 em_bits=0,rtl=0;ConstSlice<u8> expected[2];
        if(!reader.word(&em_bits)||!reader.word(&rtl)||!reader.take(count*12,&expected[0])||!reader.take(count*12,&expected[1]))return false;
        float em=0;copy_bytes(&em,&em_bits,4);
        for(u32 positioned=0;positioned<2;++positioned){plan.positioning_roots={gp_roots,positioned?gp.lookup_indices.count/2:0};
            SfntShapeRunResult result{};
            const auto status=sfnt_shape_run(plan,{input,input_count},units,{starts,start_count},rtl!=0,em,storage,&result);
            if(status!=SfntStatus::ok||result.glyph_count!=count||result.source_units!=units)return false;
            for(u32 i=0;i<count;++i)if(glyphs[i].glyph.glyph!=byte_load_le16(expected_glyphs.data+i*4)||flags[i]!=expected_glyphs.data[i*4+2])return false;
            for(u32 i=0;i<units;++i)if(character_map[i]!=byte_load_le32(characters.data+i*4))return false;
            if(!bytes_equal(output,expected[positioned].data,count*12))return false;
            ++coordinated_runs;coordinated_values+=count*5+units;
            const u64 full_work=result.lookup_operations;
            auto short_storage=storage;short_storage.placements.count=count-1;
            if(sfnt_shape_run(plan,{input,input_count},units,{starts,start_count},rtl!=0,em,short_storage,&result)!=SfntStatus::output_too_small||result.glyph_count||result.source_units)return false;
            ++coordinated_failures;
            if(!c && positioned){
                u32 callback_calls=0;SfntShapeGlyphPolicy policy{};policy.context=&callback_calls;
                policy.clusters=+[](void* context,SfntClusterMap*,const SfntComponentForest&,Slice<SfntComponentGlyph>,u64) noexcept {
                    ++*static_cast<u32*>(context);return SfntStatus::unsupported_sequence;};
                if(sfnt_shape_run(plan,{input,input_count},units,{starts,start_count},rtl!=0,em,storage,&result,{}, {}, {},policy)!=SfntStatus::unsupported_sequence||callback_calls!=1||result.glyph_count||result.source_units)return false;
                ++coordinated_failures;
                if(full_work && (sfnt_shape_run(plan,{input,input_count},units,{starts,start_count},rtl!=0,em,storage,&result,{full_work-1,100000,1000000})!=SfntStatus::resource_limit||result.glyph_count||result.source_units||result.lookup_operations>full_work-1))return false;
                if(sfnt_shape_run(plan,{input,input_count},units,{starts,start_count},rtl!=0,em,storage,&result,{1000000,100000,1})!=SfntStatus::resource_limit||result.glyph_count||result.source_units)return false;
                auto failed_plan=plan;failed_plan.advance=+[](void*,u16,double*) noexcept{return SfntStatus::malformed;};
                if(sfnt_shape_run(failed_plan,{input,input_count},units,{starts,start_count},rtl!=0,em,storage,&result)!=SfntStatus::malformed||result.glyph_count||result.source_units)return false;
                short_storage=storage;short_storage.character_glyphs.count=units-1;
                if(sfnt_shape_run(plan,{input,input_count},units,{starts,start_count},rtl!=0,em,short_storage,&result)!=SfntStatus::output_too_small||result.glyph_count||result.source_units)return false;
                if(sfnt_shape_run(plan,{},0,{},rtl!=0,em,storage,&result)!=SfntStatus::ok||result.glyph_count||result.source_units)return false;
                coordinated_failures+=5;
            }
        }
    }
    }
    return true;
}
bool corpus(ConstSlice<u8> bytes) noexcept {
    if(bytes.count<12 || !bytes_equal(bytes.data,"MCCMP01\0",8))return false;Reader r{bytes,8};u32 fonts=0;if(!r.word(&fonts) || fonts>10000)return false;
    for(u32 f=0;f<fonts;++f){u32 size=0,input_count=0,units=0,n=0,cc=0;ConstSlice<u8> raw{},scalars{},characters{},expected_glyphs{},configs{};
        if(!r.word(&size)||!r.word(&input_count)||!r.word(&units)||!r.word(&n)||!r.word(&cc)||!input_count||input_count>64||!units||units>128||!n||n>64||cc>10000||!r.take(size,&raw)||!r.take(input_count*4,&scalars)||!r.take(units*4,&characters)||!r.take(n*4,&expected_glyphs)||!r.take(cc*(8+24*n),&configs))return false;
        SfntFace face{};SfntCmap cmap{};SfntHorizontalMetrics metrics{};SfntGdef gdef{};SfntLayout gs{},gp{};ConstSlice<u8> table{};SfntGsubProgram program{};
        if(sfnt_open(raw,0,&face)!=SfntStatus::ok || sfnt_cmap_open(face,&cmap)!=SfntStatus::ok || sfnt_horizontal_metrics_open(face,&metrics)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('G','D','E','F'),&table)!=SfntStatus::ok || sfnt_gdef_open(table,metrics.glyph_count,&gdef)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('G','S','U','B'),&table)!=SfntStatus::ok || sfnt_layout_open(table,SfntLayoutKind::substitution,&gs)!=SfntStatus::ok || sfnt_gsub_program_prepare(gs,gdef,metrics.glyph_count,{{gsub_lookups,32},{gsub_subtables,64},{coverages,256},{classes,64}},&program)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('G','P','O','S'),&table)!=SfntStatus::ok || sfnt_layout_open(table,SfntLayoutKind::positioning,&gp)!=SfntStatus::ok)return error(f,0,utf8_literal("font prepare"));
        SfntFeature gs_feature{},gp_feature{};u32 tag=0;
        if(sfnt_layout_feature(gs,0,{},&tag,&gs_feature)!=SfntStatus::ok || tag!=sfnt_tag('r','l','i','g') || sfnt_layout_feature(gp,0,{},&tag,&gp_feature)!=SfntStatus::ok || tag!=sfnt_tag('t','e','s','t') || gp_feature.lookup_indices.count>16)return false;
        const u32 lc=gp_feature.lookup_indices.count/2;SfntGposProgram position_program{};
        if(sfnt_gpos_program_prepare(gp,gdef,metrics.glyph_count,{{gpos_lookups,32},{basic_storage,64},{attachment_storage,64},{context_storage,64},{gpos_coverages,256},{gpos_classes,64}},&position_program)!=SfntStatus::ok)return error(f,0,utf8_literal("GPOS prepare"));
        bool contextual=false;for(u32 i=0;i<position_program.lookups.count;++i)if(position_program.lookups.data[i].type>=7)contextual=true;
        if(contextual && !gpos_prepare_units(gp,gdef,metrics.glyph_count))return error(f,0,utf8_literal("GPOS prepare units"));
        if(!coordinated(cmap,metrics,gdef,program,position_program,gs_feature,gp_feature,scalars,units,n,characters,expected_glyphs,configs,cc))return error(f,0,utf8_literal("coordinated run"));
        u16 roots[32],queue[32];u64 selected=0;SfntLookupSelectionResult selection{};
        const u32 substitutions=gs_feature.lookup_indices.count/2;if(substitutions>32)return false;
        for(u32 i=0;i<substitutions;++i)roots[i]=byte_load_be16(gs_feature.lookup_indices.data+i*2);
        if(sfnt_lookup_selection(gs,{roots,substitutions},{&selected,1},{queue,32},&selection)!=SfntStatus::ok||
           sfnt_gsub_program_prepare(gs,gdef,metrics.glyph_count,{{gsub_lookups,32},{gsub_subtables,64},{coverages,256},{classes,64}},&program,1000000,1000000,false,false,{&selected,1})!=SfntStatus::ok||
           !coordinated(cmap,metrics,gdef,program,position_program,gs_feature,gp_feature,scalars,units,n,characters,expected_glyphs,configs,cc))return error(f,0,utf8_literal("selected GSUB run"));
        for(u32 i=0;i<lc;++i)roots[i]=byte_load_be16(gp_feature.lookup_indices.data+i*2);
        if(sfnt_lookup_selection(gp,{roots,lc},{&selected,1},{queue,32},&selection)!=SfntStatus::ok||
           sfnt_gpos_program_prepare(gp,gdef,metrics.glyph_count,{{gpos_lookups,32},{basic_storage,64},{attachment_storage,64},{context_storage,64},{gpos_coverages,256},{gpos_classes,64}},&position_program,1000000,1000000,{&selected,1})!=SfntStatus::ok)return error(f,0,utf8_literal("selected GPOS prepare"));
        gpos_selected_lookups+=selection.lookups;gpos_all_lookups+=gp.lookup_count;
        if(!coordinated(cmap,metrics,gdef,program,position_program,gs_feature,gp_feature,scalars,units,n,characters,expected_glyphs,configs,cc))return error(f,0,utf8_literal("selected coordinated run"));
        SfntGposProgram shared_program{};
        if(sfnt_gpos_program_prepare(gp,gdef,metrics.glyph_count,{{shared_gpos_lookups,32},{},{},{},{},{}},&shared_program,1000000,1000000,{&selected,1},&position_program)!=SfntStatus::ok||
           !coordinated(cmap,metrics,gdef,program,shared_program,gs_feature,gp_feature,scalars,units,n,characters,expected_glyphs,configs,cc))return error(f,0,utf8_literal("shared coordinated run"));
        const SfntGposExecutableLookup* inherited_entries[32];SfntGposProgram compact{};
        if(sfnt_gpos_program_prepare(gp,gdef,metrics.glyph_count,{{},{},{},{},{},{},{inherited_entries,32}},&compact,0,1000000,{&selected,1},&position_program)!=SfntStatus::ok||
           !coordinated(cmap,metrics,gdef,program,compact,gs_feature,gp_feature,scalars,units,n,characters,expected_glyphs,configs,cc))return error(f,0,utf8_literal("compact shared run"));
        const SfntGposExecutableLookup* fresh_entries[32];
        if(sfnt_gpos_program_prepare(gp,gdef,metrics.glyph_count,{{shared_gpos_lookups,32},{basic_storage,64},{attachment_storage,64},{context_storage,64},{gpos_coverages,256},{gpos_classes,64},{fresh_entries,32}},&compact,1000000,1000000,{&selected,1})!=SfntStatus::ok||
           !coordinated(cmap,metrics,gdef,program,compact,gs_feature,gp_feature,scalars,units,n,characters,expected_glyphs,configs,cc))return error(f,0,utf8_literal("compact fresh run"));
        position_program=compact;
        for(u32 split=0;split<=input_count;++split){u32 starts[64],start_count=0,source=0;UnicodeGraphemeState graphemes{};u64 bitmap[8];u32 character_map[128],targets[64];SfntMarkAssociation associations[64];u8 flags[64];double nominal[64],prefix[64];SfntPosition state[64];SfntPlacement output[64];
            for(u32 i=0;i<input_count;++i){const u32 scalar=byte_load_le32(scalars.data+i*4);u16 id=0;if(sfnt_cmap_glyph(cmap,scalar,0,&id)!=SfntStatus::ok)return false;const auto cls=sfnt_gdef_glyph_class(gdef,id);glyphs[i]={{id,cls,0},source,0,0};
                bool boundary=false;if(!unicode_grapheme_next(&graphemes,scalar,&boundary))return false;if(boundary)starts[start_count++]=source;source+=scalar>65535?2:1;}
            if(source!=units)return false;SfntClusterMap clusters{};SfntComponentForest forest{};SfntGlyphBuffer buffer{};
            if(sfnt_cluster_map_open(units,{starts,start_count},{bitmap,8},&clusters)!=SfntStatus::ok || sfnt_components_open({glyphs,input_count},{nodes,64},&forest)!=SfntStatus::ok || sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),sizeof(glyphs)},sizeof(SfntComponentGlyph),input_count,&buffer)!=SfntStatus::ok || sfnt_glyph_buffer_seek(&buffer,split)!=SfntStatus::ok)return false;
            if(!commit_tested && gs_feature.lookup_indices.count){const u16 first=byte_load_be16(gs_feature.lookup_indices.data);if(program.lookups.data[first].type==4){if(!commit_units(program,first,&buffer,&clusters,&forest))return error(f,split,utf8_literal("commit units"));commit_tested=true;}}
            for(u32 i=0;i<gs_feature.lookup_indices.count/2;++i){SfntGsubExecutionResult result{};const auto status=sfnt_gsub_execute_lookup(program,byte_load_be16(gs_feature.lookup_indices.data+i*2),&buffer,0,sfnt_glyph_count(sfnt_glyph_buffer_view(buffer)),0,scratch(),{1000000,100000},&result,&clusters,&forest);gsub_operations+=result.operations;
                if(status!=SfntStatus::ok)return error(f,split,utf8_literal("GSUB status"),static_cast<u32>(status));}
            auto view=sfnt_glyph_buffer_view(buffer);if(sfnt_glyph_count(view)!=n)return error(f,split,utf8_literal("glyph count"),sfnt_glyph_count(view));
            for(u32 i=0;i<n;++i){++glyph_checks;const u16 expected=static_cast<u16>(expected_glyphs.data[i*4]|(static_cast<u16>(expected_glyphs.data[i*4+1])<<8));if(sfnt_glyph_at(view,i).glyph!=expected)return error(f,split,utf8_literal("glyph mismatch"),i);}
            if(sfnt_cluster_map_export(clusters,view,{character_map,128})!=SfntStatus::ok)return error(f,split,utf8_literal("cluster export"));
            for(u32 i=0;i<units;++i){++cluster_checks;if(character_map[i]!=byte_load_le32(characters.data+i*4))return error(f,split,utf8_literal("character mismatch"),i);}
            for(u32 i=0;i<n;++i){auto& glyph=const_cast<SfntGsubGlyph&>(sfnt_glyph_at(view,i));glyph.glyph_class=sfnt_gdef_glyph_class(gdef,glyph.glyph);SfntGlyphMetrics m{};if(sfnt_horizontal_metrics(metrics,glyph.glyph,&m)!=SfntStatus::ok)return false;nominal[i]=m.advance;}
            if(sfnt_position_flags_from_clusters(clusters,view,{flags,n})!=SfntStatus::ok)return error(f,split,utf8_literal("position flag export"));
            for(u32 i=0;i<n;++i){++flag_checks;if(flags[i]!=expected_glyphs.data[i*4+2])return error(f,split,utf8_literal("position flag mismatch"),i);}
            u32 target_count=0;u64 used=0;auto status=sfnt_components_export(&forest,view,{targets,64},{associations,64},&target_count,1000000,&used);component_export_work+=used;
            if(status!=SfntStatus::ok)return error(f,split,utf8_literal("association export"),static_cast<u32>(status));++shape_runs;
            Reader config{configs};for(u32 c=0;c<cc;++c){u32 em_bits=0,rtl=0;ConstSlice<u8> expected[2];if(!config.word(&em_bits)||!config.word(&rtl)||rtl>1||!config.take(n*12,&expected[0])||!config.take(n*12,&expected[1]))return false;float em=0;copy_bytes(&em,&em_bits,4);
                if(sfnt_position_initialize({nominal,n},{flags,n},rtl!=0,{state,n})!=SfntStatus::ok || sfnt_position_finalize({state,n},{flags,n},em,metrics.units_per_em,{output,n})!=SfntStatus::ok)return false;placements(expected[0],{output,n},f,split,c);
                for(u32 i=0;i<lc;++i){SfntGposExecutionResult result{};u64 chain=0;
                    const u16 lookup_index=byte_load_be16(gp_feature.lookup_indices.data+i*2);
                    if(!split && !c && sfnt_gpos_lookup(position_program,lookup_index)->type>=7 && !gpos_program_units(position_program,lookup_index,view,{nominal,n},{flags,n},{associations,n},{targets,target_count},{state,n}))return error(f,split,utf8_literal("GPOS units"));
                    status=sfnt_gpos_execute_program_lookup(position_program,byte_load_be16(gp_feature.lookup_indices.data+i*2),view,{nominal,n},{flags,n},{associations,n},{targets,target_count},{state,n},0,n,rtl!=0,{},SfntNullAnchorPolicy::windows_parent_coordinates,SfntComponentAnchorPolicy::skip_missing,{{gpos_frames,32},{gpos_positions,256},{&chain,1},{prefix,target_count},{cursive_chains,64}},{1000000,100000},&result);
                    gpos_operations+=result.operations;gpos_actions+=result.actions;gpos_depth=maximum(gpos_depth,result.max_depth);
                    if(status!=SfntStatus::ok)return error(f,split,utf8_literal("GPOS status"),static_cast<u32>(status));}
                if(sfnt_position_finalize({state,n},{flags,n},em,metrics.units_per_em,{output,n})!=SfntStatus::ok)return false;placements(expected[1],{output,n},f,split,c);++placement_runs;
            }
        }
    }return r.at==r.data.count && commit_tested;
}
bool file_corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size) || size.QuadPart<12 || size.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping){CloseHandle(file);return false;}const auto* data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));const bool ok=data && corpus({data,static_cast<u32>(size.QuadPart)});if(data)UnmapViewOfFile(data);CloseHandle(mapping);CloseHandle(file);return ok;
}
#include "component_sequence_units.inc"
#include "component_bounds_units.inc"
#include "position_flags_units.inc"
#include "shaping_bounds_units.inc"
#include "substitution_selection_units.inc"
int run() noexcept {
    if(!substitution_selection_units()||!substitution_cycle_selection_units()){print(utf8_literal("FAIL GSUB selection units\n"));return 11;}
    if(!gpos_shared_coverage_units()){print(utf8_literal("FAIL shared GPOS coverage units\n"));return 8;}
    if(!gpos_backtrack_dispatch_units()){print(utf8_literal("FAIL GPOS backtrack dispatch units\n"));return 9;}
    if(!component_bounds_units()){print(utf8_literal("FAIL component bounds units\n"));return 7;}
    if(!shaping_bounds_units()){print(utf8_literal("FAIL shaping bounds units\n"));return 6;}
    if(!gpos_cycle_units()){print(utf8_literal("FAIL GPOS cycle units\n"));return 5;}
    if(!sequence_units() || !affinity_units()){print(utf8_literal("FAIL component sequence units\n"));return 3;}
    if(!position_flag_units()){print(utf8_literal("FAIL positioning flag units\n"));return 4;}
    const wchar_t* command=GetCommandLineW();bool quote=false;while(*command){if(*command=='"')quote=!quote;else if(!quote && (*command==' ' || *command=='\t'))break;++command;}while(*command==' ' || *command=='\t')++command;
    constexpr wchar_t cost_option[]=L"--backtrack-cost";u32 cost_units=0;
    while(command[cost_units]&&command[cost_units]==cost_option[cost_units])++cost_units;
    if(cost_units==sizeof(cost_option)/sizeof(wchar_t)-1&&!command[cost_units])return gpos_backtrack_dispatch_cost()?0:10;
    bool ok=false;if(!*command)ok=corpus({component_fixture,sizeof(component_fixture)});
    else {wchar_t path[32768];u32 n=0;quote=*command=='"';if(quote)++command;while(*command && !(quote?*command=='"':(*command==' ' || *command=='\t'))){if(n+1>=32768)return 2;path[n++]=*command++;}if(quote){if(*command!='"')return 2;++command;}while(*command==' ' || *command=='\t')++command;if(*command || !n)return 2;path[n]=0;ok=file_corpus(path);}
    print(utf8_literal("shape_runs="));number(shape_runs);print(utf8_literal("\nglyph_checks="));number(glyph_checks);print(utf8_literal("\ncluster_checks="));number(cluster_checks);print(utf8_literal("\nplacement_runs="));number(placement_runs);print(utf8_literal("\nfloat_checks="));number(float_checks);print(utf8_literal("\ngsub_operations="));number(gsub_operations);print(utf8_literal("\ncomponent_export_work="));number(component_export_work);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\n"));
    print(utf8_literal("sequence_queries="));number(sequence_queries);print(utf8_literal("\nsequence_edits="));number(sequence_edits);print(utf8_literal("\nsequence_operations="));number(sequence_operations);print(utf8_literal("\nsequence_rollbacks="));number(sequence_rollbacks);print(utf8_literal("\nsequence_max_height="));number(sequence_max_height);print(utf8_literal("\nsequence_max_journal="));number(sequence_max_journal);print(utf8_literal("\n"));
    print(utf8_literal("affinity_first_work="));number(affinity_first_work);print(utf8_literal("\naffinity_repeat_work="));number(affinity_repeat_work);print(utf8_literal("\n"));
    print(utf8_literal("commit_budget_checks="));number(commit_budget_checks);print(utf8_literal("\ncommit_journal_checks="));number(commit_journal_checks);print(utf8_literal("\n"));
    print(utf8_literal("flag_checks="));number(flag_checks);print(utf8_literal("\n"));
    print(utf8_literal("gpos_operations="));number(gpos_operations);print(utf8_literal("\ngpos_actions="));number(gpos_actions);print(utf8_literal("\ngpos_depth="));number(gpos_depth);print(utf8_literal("\n"));
    print(utf8_literal("gpos_budget_checks="));number(gpos_budget_checks);print(utf8_literal("\ngpos_capacity_checks="));number(gpos_capacity_checks);print(utf8_literal("\ngpos_guard_checks="));number(gpos_guard_checks);print(utf8_literal("\ngpos_cycle_depth="));number(gpos_cycle_depth);print(utf8_literal("\n"));
    print(utf8_literal("gpos_prepare_budgets="));number(gpos_prepare_budgets);print(utf8_literal("\ngpos_prepare_guards="));number(gpos_prepare_guards);print(utf8_literal("\n"));
    print(utf8_literal("gpos_selected_lookups="));number(gpos_selected_lookups);print(utf8_literal("\ngpos_all_lookups="));number(gpos_all_lookups);print(utf8_literal("\n"));
    print(utf8_literal("coordinated_runs="));number(coordinated_runs);print(utf8_literal("\ncoordinated_values="));number(coordinated_values);print(utf8_literal("\n"));
    print(utf8_literal("coordinated_failure_checks="));number(coordinated_failures);print(utf8_literal("\n"));
    print(utf8_literal("shaping_bounds_checks="));number(shaping_bounds_checks);print(utf8_literal("\n"));
    print(ok&&!failures?utf8_literal("PASS native component shaping\n"):utf8_literal("FAIL native component shaping\n"));return ok&&!failures?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_component_shaping_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
