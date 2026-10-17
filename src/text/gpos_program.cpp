#include "maicord/text/gpos_program.hpp"
#include "maicord/text/components.hpp"
#include "gpos_validated.hpp"
#include "context_lookups.hpp"
#include "lookup_selection.hpp"
namespace maicord {namespace {
template<class T> bool valid(Slice<T> s) noexcept {return !s.count || s.data;}
bool spend(u64* work,u64 n=1) noexcept {if(n>*work)return false;*work-=n;return true;}
SfntStatus require_children(const SfntGposExecutableLookup& lookup,u16 count,ConstSlice<u64> selected,u32& work) noexcept {
    if(!selected.count||lookup.type<7)return SfntStatus::ok;
    for(u32 i=0;i<lookup.tables.context.count;++i){u32 used=0;const auto status=sfnt_context_visit_lookups(lookup.tables.context[i].table,
        lookup.type==7?SfntContextKind::sequence:SfntContextKind::chained,count,require_lookup,&selected,work,&used);
        work-=used;if(status!=SfntStatus::ok)return status;}
    return SfntStatus::ok;
}
SfntStatus inspect(const SfntLookup& lookup,ConstSlice<u8> bytes,SfntContextRequirements* req) noexcept {
    *req={};if(lookup.type>=1 && lookup.type<=6)return SfntStatus::ok;
    if(lookup.type==7 || lookup.type==8)return sfnt_context_requirements(bytes,lookup.type==7?SfntContextKind::sequence:SfntContextKind::chained,req);
    return SfntStatus::unsupported;
}
}
SfntStatus sfnt_gpos_program_requirements(const SfntLayout& layout,SfntGposProgramRequirements* output,u32 budget,ConstSlice<u64> selected,const SfntGposProgram* inherited) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};if(layout.kind!=SfntLayoutKind::positioning)return SfntStatus::invalid_input;
    if(inherited&&(inherited->lookups.count!=layout.lookup_count||(layout.lookup_count&&!inherited->lookups.data&&!inherited->entries)))return SfntStatus::invalid_input;
    const auto valid_selection=selection_valid(layout,selected);if(valid_selection!=SfntStatus::ok)return valid_selection;
    SfntGposProgramRequirements req{layout.lookup_count};u64 work=budget;
    for(u32 i=0;i<layout.lookup_count;++i){if(!spend(&work))return SfntStatus::resource_limit;if(!selected_lookup(selected,i))continue;const auto* prior=inherited?sfnt_gpos_lookup(*inherited,static_cast<u16>(i)):nullptr;if(prior&&prior->type)continue;SfntLookup lookup{};auto s=sfnt_layout_lookup(layout,static_cast<u16>(i),&lookup);if(s!=SfntStatus::ok)return s;
        if(!spend(&work,lookup.subtable_count))return SfntStatus::resource_limit;
        ++req.records;u32& total=lookup.type<=2?req.basic:lookup.type<=6?req.attachments:req.contexts;
        if(static_cast<u64>(total)+lookup.subtable_count>0xffffffffu)return SfntStatus::resource_limit;total+=lookup.subtable_count;
        for(u16 j=0;j<lookup.subtable_count;++j){ConstSlice<u8> bytes{};s=sfnt_lookup_subtable(lookup,j,&bytes);if(s!=SfntStatus::ok)return s;SfntContextRequirements n{};s=inspect(lookup,bytes,&n);if(s!=SfntStatus::ok)return s;
            if(static_cast<u64>(req.coverages)+n.coverages>0xffffffffu || static_cast<u64>(req.classes)+n.classes>0xffffffffu)return SfntStatus::resource_limit;req.coverages+=n.coverages;req.classes+=n.classes;}}
    *output=req;return SfntStatus::ok;
}
SfntStatus sfnt_gpos_program_prepare(const SfntLayout& layout,const SfntGdef& gdef,u32 limit,SfntGposProgramStorage storage,SfntGposProgram* output,u32 subwork,u32 budget,ConstSlice<u64> selected,const SfntGposProgram* inherited) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(layout.kind!=SfntLayoutKind::positioning)return SfntStatus::invalid_input;
    if(inherited&&(inherited->glyph_limit!=limit||inherited->gdef.table.data!=gdef.table.data||inherited->gdef.table.count!=gdef.table.count||
        inherited->lookups.count!=layout.lookup_count||(layout.lookup_count&&!inherited->lookups.data&&!inherited->entries)))return SfntStatus::invalid_input;
    if(!limit || limit>65536 || (gdef.table.data && gdef.glyph_limit!=limit) || !valid(storage.lookups) || !valid(storage.basic) || !valid(storage.attachments) || !valid(storage.contexts) || !valid(storage.coverages) || !valid(storage.classes)||!valid(storage.entries))return SfntStatus::invalid_input;
    auto s=selection_valid(layout,selected);if(s!=SfntStatus::ok)return s;
    const bool compact=storage.entries.data||storage.entries.count;
    if(layout.lookup_count>(compact?storage.entries.count:storage.lookups.count))return SfntStatus::output_too_small;
    u64 directory_work=budget;
    u32 record=0,basic=0,attachment=0,context=0,coverage=0,classes=0,shared_work=minimum(subwork,65536u),closure_work=budget;
    for(u32 i=0;i<layout.lookup_count;++i){if(!spend(&directory_work))return SfntStatus::resource_limit;
        if(compact)storage.entries[i]=nullptr;else storage.lookups[i]={};if(!selected_lookup(selected,i))continue;
        const auto* prior=inherited?sfnt_gpos_lookup(*inherited,static_cast<u16>(i)):nullptr;
        if(prior&&prior->type){s=require_children(*prior,layout.lookup_count,selected,closure_work);if(s!=SfntStatus::ok)return s;
            if(compact)storage.entries[i]=prior;else storage.lookups[i]=*prior;continue;}
        if(compact&&record==storage.lookups.count)return SfntStatus::output_too_small;
        auto& dest=storage.lookups[compact?record++:i];dest={};
        SfntLookup lookup{};s=sfnt_layout_lookup(layout,static_cast<u16>(i),&lookup);if(s!=SfntStatus::ok)return s;
        if(!spend(&directory_work,lookup.subtable_count))return SfntStatus::resource_limit;dest.type=lookup.type;
        if(lookup.type<=2){if(lookup.subtable_count>storage.basic.count-basic)return SfntStatus::output_too_small;
            dest.tables.basic={storage.basic.data?storage.basic.data+basic:nullptr,lookup.subtable_count};}
        else if(lookup.type<=6){if(lookup.subtable_count>storage.attachments.count-attachment)return SfntStatus::output_too_small;
            dest.tables.attachment={storage.attachments.data?storage.attachments.data+attachment:nullptr,lookup.subtable_count};}
        else {if(lookup.type>8)return SfntStatus::unsupported;if(lookup.subtable_count>storage.contexts.count-context)return SfntStatus::output_too_small;
            dest.tables.context={storage.contexts.data?storage.contexts.data+context:nullptr,lookup.subtable_count};}
        s=sfnt_lookup_filter_prepare(gdef,lookup.flags,lookup.mark_set,&dest.filter);if(s!=SfntStatus::ok)return s;
        if((lookup.flags>>8) && !(lookup.flags&0x18) && !gdef.mark_classes.format)return SfntStatus::missing_table;
        for(u16 j=0;j<lookup.subtable_count;++j){ConstSlice<u8> bytes{};s=sfnt_lookup_subtable(lookup,j,&bytes);if(s!=SfntStatus::ok)return s;u32 charged=0;
            if(lookup.type<=2)s=sfnt_gpos_subtable_open(bytes,lookup.type,limit,&storage.basic.data[basic++],subwork,&charged);
            else if(lookup.type<=6)s=sfnt_gpos_attachment_open(bytes,lookup.type,limit,&storage.attachments.data[attachment++],subwork,&charged);
            else {SfntContextRequirements n{};s=inspect(lookup,bytes,&n);if(s!=SfntStatus::ok)return s;
                if(n.coverages>storage.coverages.count-coverage||n.classes>storage.classes.count-classes)return SfntStatus::output_too_small;
                s=sfnt_context_open(bytes,lookup.type==7?SfntContextKind::sequence:SfntContextKind::chained,limit,layout.lookup_count,{storage.coverages.data?storage.coverages.data+coverage:nullptr,n.coverages},{storage.classes.data?storage.classes.data+classes:nullptr,n.classes},&storage.contexts.data[context++],subwork,&charged);coverage+=n.coverages;classes+=n.classes;}
            subwork-=charged;if(s!=SfntStatus::ok)return s;
            sfnt_lookup_bounds_add(dest.bounds,lookup.type<=2?storage.basic[basic-1].coverage:lookup.type<=6?storage.attachments[attachment-1].coverage[0]:sfnt_context_first_coverage(storage.contexts[context-1]));
        }
        s=require_children(dest,layout.lookup_count,selected,closure_work);if(s!=SfntStatus::ok)return s;
        dest.bounds.ready=1;
        // Some fonts repeat an identical first-input coverage across dozens
        // of contextual subtables. Certify it once while preparing immutable
        // views; the executor can reject the entire root with one search.
        if(lookup.type>=7&&dest.tables.context.count>1){
            const auto& first=sfnt_context_first_coverage(dest.tables.context[0]);bool same=true;
            for(u32 j=1;j<dest.tables.context.count&&same;++j){const auto& next=sfnt_context_first_coverage(dest.tables.context[j]);
                same=sfnt_lookup_coverage_equal(first,next,shared_work);}
            if(same)dest.bounds.ready|=2;
        }
        // Disjoint singleton nearest-backtrack sets in glyph order identify
        // at most one candidate. Keep original rule/action order; no index table.
        if(lookup.type==8&&dest.tables.context.count>=4){bool ordered=true;u16 previous=0;
            for(u32 j=0;j<dest.tables.context.count;++j){
                if(!shared_work){ordered=false;break;}--shared_work;
                const auto& c=dest.tables.context[j];
                if(c.format!=3||!byte_load_be16(c.table.data+2)||c.coverages[0].covered_count!=1){ordered=false;break;}
                const u16 glyph=byte_load_be16(c.coverages[0].records);
                if(j&&glyph<=previous){ordered=false;break;}previous=glyph;
            }
            if(ordered)dest.bounds.ready|=4;
        }
        if(compact)storage.entries[i]=&dest;
    }
    *output={{compact?nullptr:storage.lookups.data,layout.lookup_count},gdef,limit,compact?storage.entries.data:nullptr};return SfntStatus::ok;
}
namespace {
struct Executor {
    const SfntGposProgram& program;SfntGlyphView glyphs;ConstSlice<double> nominal;ConstSlice<u8> flags;ConstSlice<SfntMarkAssociation> associations;ConstSlice<u32> targets;Slice<SfntPosition> positions;
    bool rtl;const SfntPositionInstance& instance;SfntNullAnchorPolicy null_policy;SfntComponentAnchorPolicy component_policy;SfntGposExecutionScratch scratch;SfntGposExecutionResult& result;
    u64 work;u32 match_work;SfntAnchorPointResolver resolver;void* context;u32 depth=0,used=0;bool chains_ready=false;SfntCursiveDeferred deferred{};
    SfntStatus flush() noexcept {SfntCursiveExecution r{};const auto s=sfnt_position_cursive_flush(positions,scratch.cursive_chains,&deferred,&r,work);work-=r.operations;return s;}
    SfntStatus attempt(const SfntGposExecutableLookup* entry,u32 at,u32 end,bool selected,u32* resume) noexcept {
        *resume=at+1;if(!spend(&work))return SfntStatus::resource_limit;if(!entry||!entry->type)return SfntStatus::invalid_input;const auto& lookup=*entry;
        const auto mode=selected?SfntPositionApplication::selected:SfntPositionApplication::scan;SfntStatus s;
        if(lookup.type<=6 && !(selected && lookup.type==3 && (lookup.filter.flags&1))){s=flush();if(s!=SfntStatus::ok)return s;}
        if(lookup.type<=2){
            SfntPositionExecution r{};s=sfnt_position_execute_lookup({lookup.tables.basic,lookup.filter},program.gdef,glyphs,positions,at,end,rtl,instance,&r,work,match_work,mode);work-=r.operations;result.matches+=r.matches;*resume=end;return s;}
        if(lookup.type<=6){const SfntGposAttachmentLookup attachment{lookup.tables.attachment,lookup.filter};
            if(lookup.type==3){SfntCursiveExecution r{};
                if(selected){if((lookup.filter.flags&1) && !chains_ready){const u32 n=sfnt_glyph_count(glyphs);if(scratch.cursive_chains.count<n)return SfntStatus::output_too_small;if(!spend(&work,n))return SfntStatus::resource_limit;
                        for(u32 i=0;i<n;++i)scratch.cursive_chains.data[i]={0xffffffffu,0xffffffffu};chains_ready=true;}
                    s=sfnt_position_execute_cursive_selected(attachment,program.gdef,glyphs,nominal,positions,at,end,rtl,instance,scratch.cursive_chains,&r,work,resolver,context,sfnt_glyph_count(glyphs)>=32?&deferred:nullptr);}
                else s=sfnt_position_execute_cursive_lookup(attachment,program.gdef,glyphs,nominal,positions,at,end,rtl,instance,scratch.cursive_bits,&r,work,resolver,context);
                work-=r.operations;result.matches+=r.matches;
            }else if(lookup.type==5){SfntLigatureExecution r{};s=sfnt_position_execute_ligature_lookup(attachment,program.gdef,glyphs,nominal,associations,targets,positions,at,end,rtl,instance,null_policy,component_policy,scratch.ligature_prefix,&r,work,resolver,context,mode);work-=r.operations;result.matches+=r.matches;}
            else {
                SfntMarkExecution r{};s=sfnt_position_execute_mark_lookup(attachment,program.gdef,glyphs,nominal,flags,positions,at,end,rtl,instance,null_policy,&r,work,resolver,context,associations,mode,associations.count&&glyphs.record_size==sizeof(SfntComponentGlyph),targets);work-=r.operations;result.matches+=r.matches;}
            *resume=end;return s;
        }
        if(lookup.type!=7 && lookup.type!=8)return SfntStatus::unsupported;const auto& glyph=sfnt_glyph_at(glyphs,at);
        if(sfnt_lookup_bounds_disjoint(lookup.bounds,glyph.glyph,glyph.glyph))return SfntStatus::ok;
        if(!selected && sfnt_lookup_ignores(program.gdef,lookup.filter,glyph.glyph,glyph.glyph_class))return SfntStatus::ok;
        u32 shared_index=sfnt_not_covered;
        if(lookup.bounds.ready&2){shared_index=sfnt_coverage_index(sfnt_context_first_coverage(lookup.tables.context[0]),glyph.glyph);
            if(shared_index==sfnt_not_covered)return SfntStatus::ok;}
        u32 first=0,last=lookup.tables.context.count;
        if(lookup.bounds.ready&4){u32 previous=at;
            for(;;){if(!previous)return SfntStatus::ok;--previous;if(!spend(&work))return SfntStatus::resource_limit;
                const auto& back=sfnt_glyph_at(glyphs,previous);
                if(!sfnt_lookup_ignores(program.gdef,lookup.filter,back.glyph,back.glyph_class))break;}
            const u16 key=sfnt_glyph_at(glyphs,previous).glyph;u32 high=last;
            while(first<high){if(!spend(&work))return SfntStatus::resource_limit;const u32 middle=first+(high-first)/2;
                if(byte_load_be16(lookup.tables.context[middle].coverages[0].records)<key)first=middle+1;else high=middle;}
            if(first==last||byte_load_be16(lookup.tables.context[first].coverages[0].records)!=key)return SfntStatus::ok;
            last=first+1;
        }
        for(u32 i=first;i<last;++i){if(!spend(&work))return SfntStatus::resource_limit;SfntContextMatch match{};u32 charged=0;
            u32 first_coverage_index=shared_index;
            if(lookup.bounds.ready&&first_coverage_index==sfnt_not_covered){first_coverage_index=sfnt_coverage_index(sfnt_context_first_coverage(lookup.tables.context[i]),glyph.glyph);if(first_coverage_index==sfnt_not_covered)continue;}
            s=sfnt_context_match_view(lookup.tables.context.data[i],program.gdef,lookup.filter,glyphs,at,end,{scratch.positions.data?scratch.positions.data+used:nullptr,scratch.positions.count-used},&match,static_cast<u32>(minimum<u64>(work,match_work)),&charged,0,first_coverage_index);work-=charged;
            if(s!=SfntStatus::ok)return s;if(!match.matched)continue;++result.matches;
            u32 nested_end=match.last_input+1;
            while(nested_end<end){if(!spend(&work))return SfntStatus::resource_limit;const auto& g=sfnt_glyph_at(glyphs,nested_end);if(!sfnt_lookup_ignores(program.gdef,lookup.filter,g.glyph,g.glyph_class))break;++nested_end;}
            *resume=nested_end;if(!match.actions.count)return SfntStatus::ok;if(depth==scratch.frames.count)return SfntStatus::resource_limit;
            scratch.frames.data[depth++]={match.actions,used,match.input_count,0,nested_end};used+=match.input_count;result.max_depth=maximum(result.max_depth,depth);return SfntStatus::ok;
        }
        return SfntStatus::ok;
    }
    SfntStatus run(u16 index,u32 start,u32 end) noexcept {
        const auto* root=sfnt_gpos_lookup(program,index);
        for(u32 at=start;at<end;){u32 resume=at+1;auto s=attempt(root,at,end,false,&resume);if(s!=SfntStatus::ok)return s;
            while(depth){auto& frame=scratch.frames.data[depth-1];if(frame.next_action==frame.actions.count/4){used=frame.positions;--depth;continue;}
                if(!spend(&work))return SfntStatus::resource_limit;SfntSequenceAction action{};s=sfnt_context_action({frame.actions,0,0,true},frame.next_action++,&action);if(s!=SfntStatus::ok)return s;
                if(action.sequence_index>=frame.count)return SfntStatus::invalid_input;const u32 position=scratch.positions.data[frame.positions+action.sequence_index];u32 ignored=0;++result.actions;
                s=attempt(action.lookup_index<program.lookups.count?sfnt_gpos_lookup(program,action.lookup_index):nullptr,position,frame.input_end,true,&ignored);if(s!=SfntStatus::ok)return s;
            }
            at=resume;
        }return flush();
    }
};
}
SfntStatus sfnt_gpos_execute_program_lookup(const SfntGposProgram& program,u16 lookup,SfntGlyphView glyphs,ConstSlice<double> nominal,ConstSlice<u8> flags,ConstSlice<SfntMarkAssociation> associations,ConstSlice<u32> targets,Slice<SfntPosition> positions,u32 start,u32 end,bool rtl,const SfntPositionInstance& instance,SfntNullAnchorPolicy null_policy,SfntComponentAnchorPolicy component_policy,SfntGposExecutionScratch scratch,SfntGposExecutionLimits limits,SfntGposExecutionResult* output,SfntAnchorPointResolver resolver,void* context) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};const u32 n=sfnt_glyph_count(glyphs);
    const auto* root=lookup<program.lookups.count?sfnt_gpos_lookup(program,lookup):nullptr;
    if(!sfnt_glyph_view_valid(glyphs) || !root || !root->type || !program.glyph_limit || program.glyph_limit>65536 || start>end || end>n || nominal.count<n || flags.count<n || positions.count<n || (n && (!nominal.data || !flags.data || !positions.data)) || (associations.count && (associations.count<n || !associations.data)) || (targets.count && !targets.data) || static_cast<u8>(null_policy)>1 || static_cast<u8>(component_policy)>1 || static_cast<u8>(instance.arithmetic)>1 || !valid(scratch.frames) || !valid(scratch.positions) || !valid(scratch.cursive_bits) || !valid(scratch.ligature_prefix) || !valid(scratch.cursive_chains) || scratch.positions.count>0x3fffffffu)return SfntStatus::invalid_input;
    u64 work=limits.operations;
    for(u32 i=0;i<targets.count;++i){if(!spend(&work)){output->operations=limits.operations-work;return SfntStatus::resource_limit;}if(targets.data[i]>=n || (i && targets.data[i]<=targets.data[i-1]))return SfntStatus::invalid_input;}
    u16 first_glyph=65535,last_glyph=0;
    for(u32 i=0;i<n;++i){if(!spend(&work)){output->operations=limits.operations-work;return SfntStatus::resource_limit;}const auto& g=sfnt_glyph_at(glyphs,i);const auto& p=positions.data[i];
        first_glyph=minimum(first_glyph,g.glyph);last_glyph=maximum(last_glyph,g.glyph);
        if(g.glyph>=program.glyph_limit || static_cast<u8>(g.glyph_class)>4 || (flags.data[i]&~3u) || (!i && !(flags.data[i]&sfnt_position_cluster_start)) || !is_finite(nominal.data[i]) || !is_finite(p.advance) || !is_finite(p.advance_offset) || !is_finite(p.ascender_offset))return SfntStatus::invalid_input;
        if(associations.count && associations.data[i].target!=0xffffffffu && (associations.data[i].target>=targets.count || targets.data[associations.data[i].target]>=i))return SfntStatus::invalid_input;
    }
    if(sfnt_lookup_bounds_disjoint(root->bounds,first_glyph,last_glyph)){output->operations=limits.operations-work;return SfntStatus::ok;}
    const auto status=sfnt_gpos_execute_validated_lookup(program,lookup,glyphs,nominal,flags,associations,targets,positions,start,end,rtl,instance,null_policy,component_policy,scratch,{work,limits.match_work},output,resolver,context);
    output->operations+=limits.operations-work;return status;
}
SfntStatus sfnt_gpos_execute_validated_lookup(const SfntGposProgram& program,u16 lookup,SfntGlyphView glyphs,ConstSlice<double> nominal,ConstSlice<u8> flags,ConstSlice<SfntMarkAssociation> associations,ConstSlice<u32> targets,Slice<SfntPosition> positions,u32 start,u32 end,bool rtl,const SfntPositionInstance& instance,SfntNullAnchorPolicy null_policy,SfntComponentAnchorPolicy component_policy,SfntGposExecutionScratch scratch,SfntGposExecutionLimits limits,SfntGposExecutionResult* output,SfntAnchorPointResolver resolver,void* context) noexcept {
    *output={};Executor e{program,glyphs,nominal,flags,associations,targets,positions,rtl,instance,null_policy,component_policy,scratch,*output,limits.operations,limits.match_work,resolver,context};
    const auto status=e.run(lookup,start,end);output->operations=limits.operations-e.work;return status;
}
}
