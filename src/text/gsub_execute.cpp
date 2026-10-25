#include "maicord/text/gsub_execute.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/base.hpp"
#include "gsub_validated.hpp"
#include "maicord/sys/intrin.hpp"

namespace maicord {namespace {
template<class T> bool valid(Slice<T> s) noexcept {return !s.count || s.data;}
inline bool spend(u64* work,u64 amount=1) noexcept {if(amount>*work)return false;*work-=amount;return true;}
SfntStatus inspect(const SfntLookup& lookup,ConstSlice<u8> bytes,SfntContextRequirements* req) noexcept {
    reset_value(req);if(lookup.type<=4)return SfntStatus::ok;
    if(lookup.type==5 || lookup.type==6)return sfnt_context_requirements(bytes,lookup.type==5?SfntContextKind::sequence:SfntContextKind::chained,req);
    if(lookup.type==8)return sfnt_gsub_reverse_requirements(bytes,&req->coverages);
    return SfntStatus::unsupported;
}
SfntStatus lookup_requirements(const SfntLookup& lookup,SfntGsubProgramRequirements& result,u64& work) noexcept {
    SfntStatus s;
    if(!spend(&work,lookup.subtable_count))return SfntStatus::resource_limit;
    if(static_cast<u64>(result.subtables)+lookup.subtable_count>0xffffffffu)return SfntStatus::resource_limit;result.subtables+=lookup.subtable_count;
    for(u16 j=0;j<lookup.subtable_count;++j){ConstSlice<u8> bytes{};s=sfnt_lookup_subtable(lookup,j,&bytes);if(s!=SfntStatus::ok)return s;SfntContextRequirements req{};s=inspect(lookup,bytes,&req);if(s!=SfntStatus::ok)return s;
        if(static_cast<u64>(result.coverages)+req.coverages>0xffffffffu || static_cast<u64>(result.classes)+req.classes>0xffffffffu)return SfntStatus::resource_limit;result.coverages+=req.coverages;result.classes+=req.classes;}
    return SfntStatus::ok;
}
struct LookupBuilder {
    SfntGsubLookupStorage storage;u32 subwork,shared_work,subat=0,covat=0,classat=0;
    SfntStatus prepare(const SfntLookup& lookup,const SfntGdef& gdef,u32 limit,u16 lookup_count,
        SfntGsubExecutableLookup& dest,bool ignore_empty,bool ignore_unreachable) noexcept {
        SfntStatus s;
        if(lookup.subtable_count>storage.subtables.count-subat)return SfntStatus::output_too_small;
        reset_value(&dest);dest.type=lookup.type;dest.subtables={storage.subtables.data?storage.subtables.data+subat:nullptr,lookup.subtable_count};
        s=sfnt_lookup_filter_prepare(gdef,lookup.flags,lookup.mark_set,&dest.filter);if(s!=SfntStatus::ok)return s;
        if((lookup.flags>>8) && !(lookup.flags&0x18) && !gdef.mark_classes.format)return SfntStatus::missing_table;
        for(u16 j=0;j<lookup.subtable_count;++j){ConstSlice<u8> bytes{};s=sfnt_lookup_subtable(lookup,j,&bytes);if(s!=SfntStatus::ok)return s;SfntContextRequirements need{};s=inspect(lookup,bytes,&need);if(s!=SfntStatus::ok)return s;
            if(need.coverages>storage.coverages.count-covat||need.classes>storage.classes.count-classat)return SfntStatus::output_too_small;
            auto& sub=storage.subtables.data[subat++];Slice<SfntCoverage> cv{storage.coverages.data?storage.coverages.data+covat:nullptr,need.coverages};Slice<SfntClassDef> cl{storage.classes.data?storage.classes.data+classat:nullptr,need.classes};
            u32 charged=0;
            if(lookup.type<=4)s=sfnt_gsub_subtable_open(bytes,lookup.type,limit,&sub.basic,subwork,&charged,ignore_empty);
            else if(lookup.type==8)s=sfnt_gsub_reverse_open(bytes,limit,cv,&sub.reverse,subwork,&charged);
            else s=sfnt_context_open(bytes,lookup.type==5?SfntContextKind::sequence:SfntContextKind::chained,limit,lookup_count,cv,cl,&sub.context,subwork,&charged);
            subwork-=charged;
            if(s==SfntStatus::malformed&&ignore_unreachable&&lookup.type==1&&limit<65536){
                SfntGsubSubtable candidate{};charged=0;
                const auto retry=sfnt_gsub_subtable_open(bytes,1,65536,&candidate,subwork,&charged);subwork-=charged;
                if(retry==SfntStatus::resource_limit)return retry;
                // Coverage validation proves sorted input IDs. The first ID
                // therefore proves whether any real glyph can reach this rule.
                u16 first=0;if(retry==SfntStatus::ok&&candidate.coverage.covered_count&&
                    sfnt_coverage_glyph(candidate.coverage,0,&first)==SfntStatus::ok&&first>=limit){
                    reset_value(&candidate.coverage);candidate.glyph_limit=limit;sub.basic=candidate;s=SfntStatus::ok;}
            }
            if(s!=SfntStatus::ok)return s;covat+=need.coverages;classat+=need.classes;
            sfnt_lookup_bounds_add(dest.bounds,lookup.type<=4?sub.basic.coverage:lookup.type==8?sub.reverse.coverages[0]:sfnt_context_first_coverage(sub.context));
        }
        dest.bounds.ready=1;
        if((lookup.type==5||lookup.type==6)&&dest.subtables.count>1){
            const auto& first=sfnt_context_first_coverage(dest.subtables[0].context);bool same=true;
            for(u32 j=1;j<dest.subtables.count&&same;++j)
                same=sfnt_lookup_coverage_equal(first,sfnt_context_first_coverage(dest.subtables[j].context),shared_work);
            if(same)dest.bounds.ready|=2;
        }
        return SfntStatus::ok;
    }
};
inline SfntGlyphView prefix(SfntGlyphView v,u32 end) noexcept {if(end<v.prefix_count){v.prefix_count=end;v.suffix_count=0;}else v.suffix_count=end-v.prefix_count;return v;}
struct Executor {
    struct CoverageCache {
        u64 words[8][4];u16 glyphs[8],checked[8];u8 count=0,next=0;
        CoverageCache() noexcept {}
    };
    const SfntGsubProgram& program;SfntGlyphBuffer& buffer;SfntGsubExecutionScratch scratch;
    SfntGsubExecutionResult& result;u64 work;u32 match_work,alternate;SfntClusterMap* clusters;SfntComponentForest* components;u8 mask;u32 depth=0,used=0;
    CoverageCache coverage_cache;
    SfntStatus rebase(const SfntGlyphEdit& edit) noexcept {
        u32 total=0;
        for(u32 f=0;f<depth;++f){auto& frame=scratch.frames.data[f];u32 removed=0,out_count=0;bool member=false,emitted=false;
            // Charge both scans, bounded output writes and the final copy.
            if(!spend(&work,static_cast<u64>(frame.count)*3+static_cast<u64>(edit.consumed.count)*2+static_cast<u64>(edit.replacement_count)*2))return SfntStatus::resource_limit;
            for(u32 i=0;i<frame.count && !member;++i){const u32 old=scratch.positions.data[frame.positions+i];
                while(removed<edit.consumed.count && edit.consumed.data[removed]<old)++removed;
                member=removed<edit.consumed.count && edit.consumed.data[removed]==old;}
            removed=0;
            for(u32 i=0;i<frame.count;++i){const u32 old=scratch.positions.data[frame.positions+i];
                while(removed<edit.consumed.count && edit.consumed.data[removed]<old)++removed;
                if(member && !emitted && old>=edit.start){
                    if(edit.replacement_count>scratch.rebased.count-total)return SfntStatus::output_too_small;
                    for(u32 k=0;k<edit.replacement_count;++k)scratch.rebased.data[total++]=edit.start+k;
                    out_count+=edit.replacement_count;emitted=true;
                }
                if(removed<edit.consumed.count && edit.consumed.data[removed]==old)continue;
                if(total==scratch.rebased.count)return SfntStatus::output_too_small;
                scratch.rebased.data[total++]=old-removed+(old>=edit.start?edit.replacement_count:0);++out_count;
            }
            if(total>scratch.positions.count)return SfntStatus::output_too_small;
            frame.positions=total-out_count;frame.count=out_count;u32 end=0;
            const auto s=sfnt_glyph_edit_boundary(edit,frame.input_end,true,&end);if(s!=SfntStatus::ok)return s;frame.input_end=end;
        }
        used=total;
        return SfntStatus::ok;
    }
    SfntStatus apply(const SfntGsubMatch& match,u16 type,u32* resume) noexcept {
        const u32 first=scratch.consumed.data[0],n=sfnt_glyph_count(sfnt_glyph_buffer_view(buffer));
        const u32 stride=buffer.record_size,replacement_bytes=static_cast<u32>(match.output_count)*stride;
        if(replacement_bytes>scratch.replacements.count || static_cast<u64>(n)-match.input_count+match.output_count>buffer.capacity)return SfntStatus::output_too_small;
        if(!spend(&work,match.output_count))return SfntStatus::resource_limit;
        const SfntGsubGlyph source=sfnt_glyph_at(sfnt_glyph_buffer_view(buffer),first);
        u32 source_index=0;SfntClusterMerge merge{};
        if(clusters){const auto view=sfnt_glyph_buffer_view(buffer);source_index=byte_load_le32(reinterpret_cast<const u8*>(&sfnt_glyph_at(view,first))+4);
            u32 lo=source_index,hi=source_index;
            if(match.input_count>1){if(!spend(&work,match.last_input-first+1))return SfntStatus::resource_limit;
                for(u32 i=first;i<=match.last_input;++i){const u32 at=byte_load_le32(reinterpret_cast<const u8*>(&sfnt_glyph_at(view,i))+4);lo=minimum(lo,at);hi=maximum(hi,at);}}
            u64 charged=0;const auto status=sfnt_cluster_map_plan(*clusters,lo,hi,&merge,work,&charged);work-=charged;if(status!=SfntStatus::ok)return status;
            if(!spend(&work,merge.commit_work))return SfntStatus::resource_limit;
        }
        for(u16 i=0;i<match.output_count;++i){u16 glyph=0;const auto s=sfnt_gsub_replacement(match,i,&glyph);if(s!=SfntStatus::ok)return s;
            if(glyph>=program.glyph_limit)return SfntStatus::invalid_input;
            const SfntGsubGlyph replacement{glyph,source.glyph_class,source.reserved};u8* record=scratch.replacements.data+static_cast<u32>(i)*stride;
            if(components)copy_bytes(record,&sfnt_glyph_at(sfnt_glyph_buffer_view(buffer),first),stride);
            copy_bytes(record,&replacement,sizeof(replacement));if(clusters)copy_bytes(record+4,&source_index,sizeof(source_index));}
        SfntGlyphEdit edit{{scratch.consumed.data,match.input_count},first,n,n-match.input_count+match.output_count,match.output_count,0};
        // Rebasing is prepared before the edit: an error invalidates scratch but
        // leaves the glyph edit atomic, and a successful commit needs only the packed copy.
        auto s=rebase(edit);if(s!=SfntStatus::ok)return s;
        const bool component_change=components && (type==4 || (type==2 && match.output_count>1));SfntComponentEdit component_edit{};
        if(component_change){
            if(type==2 && !spend(&work,static_cast<u64>(match.output_count)*clusters->levels))return SfntStatus::resource_limit;
            s=sfnt_components_edit(components,sfnt_glyph_buffer_view(buffer),edit.consumed,type==4,{scratch.replacements.data,replacement_bytes},scratch.component_journal,&component_edit,work);work-=component_edit.operations;if(s!=SfntStatus::ok)return s;
            for(u32 i=0;i<match.output_count;++i)if(byte_load_le32(scratch.replacements.data+i*stride+4)>=clusters->text_count){sfnt_components_rollback(components,{scratch.component_journal.data,component_edit.undo_count});return SfntStatus::invalid_input;}
        }
        s=sfnt_glyph_buffer_replace(&buffer,first,edit.consumed,{scratch.replacements.data,replacement_bytes},&edit,work);
        if(s!=SfntStatus::ok){if(component_change)sfnt_components_rollback(components,{scratch.component_journal.data,component_edit.undo_count});return s;}
        work-=edit.moved_records+edit.consumed.count;
        if(clusters)sfnt_cluster_map_commit(clusters,merge);
        if(component_change && type==2)for(u32 i=1;i<match.output_count;++i){const u32 boundary_source=byte_load_le32(scratch.replacements.data+i*stride+4);
            if(boundary_source!=byte_load_le32(scratch.replacements.data+(i-1)*stride+4))sfnt_cluster_map_split(clusters,boundary_source,clusters->levels);}
        copy_bytes(scratch.positions.data,scratch.rebased.data,used*sizeof(u32));
        u32 end=0;s=sfnt_glyph_edit_boundary(edit,result.input_end,true,&end);if(s!=SfntStatus::ok)return s;result.input_end=end;
        ++result.edits;result.moved_records+=edit.moved_records;
        // Resume after the entire consumed span, retaining skipped survivors.
        return sfnt_glyph_edit_boundary(edit,match.last_input+1,true,resume);
    }
    // view is the current buffer; it is unchanged until a matching edit
    // returns, so it serves all rejected subtables of this lookup.
    SfntStatus attempt(const SfntGsubExecutableLookup& lookup,const SfntGlyphView& view,u32 at,u32 end,bool* matched,u32* resume) noexcept {
        *matched=false;*resume=at+1;if(!spend(&work))return SfntStatus::resource_limit;
        const u16 first=sfnt_glyph_at(view,at).glyph;
        if(sfnt_lookup_bounds_exclude(lookup.bounds,first))return SfntStatus::ok;
        // Preparation sets this certificate only on contextual lookups.
        const bool shared=(lookup.bounds.ready&2)!=0;
        u32 shared_index=sfnt_not_covered;
        if(shared){shared_index=sfnt_coverage_index(sfnt_context_first_coverage(lookup.subtables[0].context),first);
            if(shared_index==sfnt_not_covered)return SfntStatus::ok;}
        // Large contextual lookups often repeat a small glyph alphabet. Only
        // first-coverage membership is cached, never a context match or action;
        // nested actions leave the outer lookup's cache intact. Enumerating set
        // bits avoids revisiting hundreds of impossible subtables.
        u32 cache_slot=0xffffffffu,known=0,block=0;u64 remaining=0;bool extending=false;
        if(!shared&&!depth&&lookup.bounds.ready&&(lookup.type==5||lookup.type==6)&&lookup.subtables.count>=16&&lookup.subtables.count<=256){
            u32 slot=0;for(;slot<coverage_cache.count;++slot){if(!spend(&work))return SfntStatus::resource_limit;if(coverage_cache.glyphs[slot]==first)break;}
            if(slot==coverage_cache.count){
                if(!spend(&work,4))return SfntStatus::resource_limit;
                if(coverage_cache.count<8)++coverage_cache.count;else{slot=coverage_cache.next;coverage_cache.next=static_cast<u8>((coverage_cache.next+1)&7);}
                for(u32 word=0;word<4;++word)coverage_cache.words[slot][word]=0;
                coverage_cache.glyphs[slot]=first;coverage_cache.checked[slot]=0;
            }cache_slot=slot;known=coverage_cache.checked[slot];
        }
        const SfntCoverage* previous_coverage=nullptr;u32 previous_index=shared_index;
        for(u32 i=0;i<lookup.subtables.count;++i){
            if(cache_slot!=0xffffffffu&&!extending){while(!remaining&&block<(known+63)/64){if(!spend(&work))return SfntStatus::resource_limit;remaining=coverage_cache.words[cache_slot][block++];}
                if(remaining){unsigned long bit=0;_BitScanForward64(&bit,remaining);i=(block-1)*64+bit;remaining&=remaining-1;}
                else{i=known;extending=true;if(i==lookup.subtables.count)break;}}
            if(!spend(&work))return SfntStatus::resource_limit;
            const auto& sub=lookup.subtables.data[i];SfntStatus s;SfntGsubMatch m{};
            if(cache_slot!=0xffffffffu&&extending){const auto& coverage=sfnt_context_first_coverage(sub.context);
                previous_index=sfnt_coverage_index(coverage,first);previous_coverage=&coverage;coverage_cache.checked[cache_slot]=static_cast<u16>(i+1);
                if(previous_index==sfnt_not_covered)continue;coverage_cache.words[cache_slot][i/64]|=1ull<<(i%64);}
            const u32 allowance=static_cast<u32>(minimum<u64>(match_work,work));u32 charged=0;
            if(lookup.type<=4){u32 left=allowance;s=gsub_subtable_match(sub.basic,program.gdef,lookup.filter,prefix(view,end),at,alternate,scratch.consumed,&m,&left,mask);work-=allowance-left;}
            else if(lookup.type==8){s=sfnt_gsub_reverse_match_view(sub.reverse,program.gdef,lookup.filter,view,at,&m,allowance,&charged,mask);work-=charged;
                if(s==SfntStatus::ok && m.matched){if(!scratch.consumed.count)return SfntStatus::output_too_small;scratch.consumed.data[0]=at;}}
            else {
                if(lookup.bounds.ready&&!shared){const auto& coverage=sfnt_context_first_coverage(sub.context);
                    if(!previous_coverage||previous_coverage->records!=coverage.records||previous_coverage->record_count!=coverage.record_count||previous_coverage->format!=coverage.format)
                        previous_index=sfnt_coverage_index(coverage,first);
                    previous_coverage=&coverage;if(previous_index==sfnt_not_covered)continue;
                }
                SfntContextMatch context{};s=sfnt_context_match_view(sub.context,program.gdef,lookup.filter,view,at,end,scratch.consumed,&context,allowance,&charged,mask,previous_index);work-=charged;
                if(s!=SfntStatus::ok)return s;if(!context.matched)continue;
                // A parent's ignored trailing glyphs remain inside the nested
                // input range, up to the next nonignored glyph or inherited end.
                u32 nested_end=context.last_input+1;
                while(nested_end<end){if(!spend(&work))return SfntStatus::resource_limit;const auto& g=sfnt_glyph_at(view,nested_end);
                    if(!sfnt_glyph_feature_enabled(g,mask) || !sfnt_lookup_ignores(program.gdef,lookup.filter,g.glyph,g.glyph_class))break;++nested_end;}
                *matched=true;*resume=nested_end;
                if(!context.actions.count)return SfntStatus::ok;
                if(depth==scratch.frames.count)return SfntStatus::resource_limit;
                if(context.input_count>scratch.positions.count-used)return SfntStatus::output_too_small;
                if(!spend(&work,context.input_count))return SfntStatus::resource_limit;
                copy_bytes(scratch.positions.data+used,scratch.consumed.data,context.input_count*sizeof(u32));
                scratch.frames.data[depth++]={context.actions,used,context.input_count,0,nested_end};used+=context.input_count;result.max_depth=maximum(result.max_depth,depth);return SfntStatus::ok;
            }
            if(s!=SfntStatus::ok)return s;if(!m.matched)continue;*matched=true;return apply(m,lookup.type,resume);
        }return SfntStatus::ok;
    }
    SfntStatus run(const SfntGsubExecutableLookup& root,u32 start) noexcept {
        const bool reverse=root.type==8;u32 at=reverse?result.input_end:start;
        // Only a match (with its nested actions) can edit the buffer.
        auto view=sfnt_glyph_buffer_view(buffer);
        while(reverse?at>start:at<result.input_end){const u32 current=reverse?--at:at;bool matched=false;u32 resume=current+1;
            // An excluded or ignored glyph costs the same single unit as a rejected attempt.
            const auto& glyph=sfnt_glyph_at(view,current);
            if(sfnt_lookup_bounds_exclude(root.bounds,glyph.glyph)||(root.filter.flags&&sfnt_lookup_ignores(program.gdef,root.filter,glyph.glyph,glyph.glyph_class))){
                if(!spend(&work))return SfntStatus::resource_limit;if(!reverse)at=resume;continue;}
            auto s=attempt(root,view,current,result.input_end,&matched,&resume);if(s!=SfntStatus::ok)return s;
            while(depth){auto& frame=scratch.frames.data[depth-1];
                if(frame.next_action==frame.actions.count/4){resume=frame.input_end;used=frame.positions;--depth;continue;}
                if(!spend(&work))return SfntStatus::resource_limit;SfntSequenceAction action{};s=sfnt_context_action({frame.actions,0,0,true},frame.next_action++,&action);if(s!=SfntStatus::ok)return s;
                if(action.sequence_index>=frame.count)continue;const u32 position=scratch.positions.data[frame.positions+action.sequence_index];
                // Source membership was preserved during rebasing, including
                // newly emitted glyphs. Do not refilter it by destination class.
                if(position>=frame.input_end)continue;
                const SfntGsubExecutableLookup* child=nullptr;s=sfnt_gsub_lookup_prepare(program,action.lookup_index,&child);if(s!=SfntStatus::ok)return s;
                u32 ignored=0;bool nested=false;s=attempt(*child,sfnt_glyph_buffer_view(buffer),position,frame.input_end,&nested,&ignored);if(s!=SfntStatus::ok)return s;
            }
            if(matched)view=sfnt_glyph_buffer_view(buffer);
            if(!reverse)at=resume;
        }return SfntStatus::ok;
    }
};
}
SfntStatus sfnt_gsub_lookup_requirements(const SfntLayout& layout,u16 index,SfntGsubProgramRequirements* output,u32 budget) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(layout.kind!=SfntLayoutKind::substitution)return SfntStatus::invalid_input;
    if(!budget)return SfntStatus::resource_limit;
    SfntLookup lookup{};auto s=sfnt_layout_lookup(layout,index,&lookup);if(s!=SfntStatus::ok)return s;
    SfntGsubProgramRequirements result{1,0,0,0};u64 work=budget-1;
    s=lookup_requirements(lookup,result,work);if(s==SfntStatus::ok)*output=result;return s;
}
SfntStatus sfnt_gsub_prepare_lookup(const SfntLayout& layout,u16 index,const SfntGdef& gdef,u32 limit,
    SfntGsubLookupStorage storage,SfntGsubExecutableLookup* output,u32 subwork,u32 budget,bool ignore_empty,bool ignore_unreachable) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(layout.kind!=SfntLayoutKind::substitution||!limit||limit>65536||(gdef.table.data&&gdef.glyph_limit!=limit)||
       !valid(storage.subtables)||!valid(storage.coverages)||!valid(storage.classes))return SfntStatus::invalid_input;
    if(!budget)return SfntStatus::resource_limit;
    SfntLookup lookup{};auto s=sfnt_layout_lookup(layout,index,&lookup);if(s!=SfntStatus::ok)return s;
    if(lookup.subtable_count>budget-1)return SfntStatus::resource_limit;
    LookupBuilder builder{storage,subwork,minimum(subwork,65536u)};SfntGsubExecutableLookup result{};
    s=builder.prepare(lookup,gdef,limit,layout.lookup_count,result,ignore_empty,ignore_unreachable);
    if(s==SfntStatus::ok)*output=result;return s;
}
SfntStatus sfnt_gsub_execute_lookup(const SfntGsubProgram& program,u16 lookup,SfntGlyphBuffer* buffer,u32 start,u32 end,u32 alternate,SfntGsubExecutionScratch scratch,SfntGsubExecutionLimits limits,SfntGsubExecutionResult* output,SfntClusterMap* clusters,SfntComponentForest* components,u8 mask) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!buffer || lookup>=program.lookups.count || !sfnt_gsub_directory_valid(program) || (components && (!clusters || components->sealed)) || buffer->record_size!=(components?sizeof(SfntComponentGlyph):clusters?sizeof(SfntClusterGlyph):sizeof(SfntGsubGlyph)) || !valid(scratch.frames) || !valid(scratch.positions) || !valid(scratch.rebased) || !valid(scratch.consumed) || !valid(scratch.replacements) || !valid(scratch.component_journal) || scratch.positions.count>0x3fffffffu || scratch.rebased.count>0x3fffffffu || scratch.consumed.count>0x3fffffffu)return SfntStatus::invalid_input;
    const SfntGsubExecutableLookup* root=nullptr;const auto prepared=sfnt_gsub_lookup_prepare(program,lookup,&root);if(prepared!=SfntStatus::ok)return prepared;
    const auto s=sfnt_glyph_buffer_seek(buffer,buffer->gap_begin,0);if(s!=SfntStatus::ok)return s;
    const auto view=sfnt_glyph_buffer_view(*buffer);if(start>end || end>sfnt_glyph_count(view))return SfntStatus::invalid_input;
    // Validate the complete run once, including lookaround outside the range.
    if(sfnt_glyph_count(view)>limits.operations)return SfntStatus::resource_limit;
    SfntGlyphSummary run{};
    for(u32 i=0;i<sfnt_glyph_count(view);++i){const auto& g=sfnt_glyph_at(view,i);if(g.glyph>=program.glyph_limit || static_cast<u8>(g.glyph_class)>4 || (clusters && byte_load_le32(reinterpret_cast<const u8*>(&g)+4)>=clusters->text_count))return SfntStatus::invalid_input;
        sfnt_glyph_summary_add(run,g.glyph);
        if(components){const auto& c=*reinterpret_cast<const SfntComponentGlyph*>(&g);if((c.component!=0xffffffffu && c.component>=components->nodes.count) || (c.attachment!=0xffffffffu && c.attachment>=components->nodes.count))return SfntStatus::invalid_input;}}
    output->input_end=end;
    if(sfnt_lookup_bounds_miss(root->bounds,run)){output->operations=sfnt_glyph_count(view);return SfntStatus::ok;}
    const u32 count=sfnt_glyph_count(view);const auto status=sfnt_gsub_execute_validated_lookup(program,*root,buffer,start,end,alternate,scratch,{limits.operations-count,limits.match_work},output,clusters,components,mask);
    output->operations+=count;return status;
}
SfntStatus sfnt_gsub_execute_validated_lookup(const SfntGsubProgram& program,const SfntGsubExecutableLookup& lookup,SfntGlyphBuffer* buffer,u32 start,u32 end,u32 alternate,SfntGsubExecutionScratch scratch,SfntGsubExecutionLimits limits,SfntGsubExecutionResult* output,SfntClusterMap* clusters,SfntComponentForest* components,u8 mask) noexcept {
    reset_value(output);output->input_end=end;
    Executor e{program,*buffer,scratch,*output,limits.operations,limits.match_work,alternate,clusters,components,mask};const auto status=e.run(lookup,start);output->operations=limits.operations-e.work;return status;
}
}
