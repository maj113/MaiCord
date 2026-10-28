#include "maicord/core/memory.hpp"
#include "maicord/text/sinhala_input.hpp"
#include "use_character.hpp"
#include "use_shape_internal.hpp"
namespace maicord {namespace {
constexpr u32 nil=0xffffffffu;
using namespace use_shape;
struct Snapshot {u32 component;u16 glyph;};
static_assert(sizeof(Snapshot)==8);
struct Policy {
    const SfntGdef* gdef;Slice<Snapshot> snapshots;
    SfntComponentForest* forest=nullptr;u32 snapshot_count=0,pref=nil;
    // The mapped pre-span is bounded by the frontend's 31-item cluster cap
    // (including selector/expansion allowance).
    u8 pre_count=0;u16 halant=0,space=0,controls[4]{};
    bool has_reph:1=false,has_pref:1=false,reph_formed:1=false,control_ids:1=false;
};
static_assert(sizeof(Policy)==56);
SfntStatus role(const SfntComponentGlyph& glyph,u8* out,Work& w) noexcept {
    if(!w.spend(1))return SfntStatus::resource_limit;*out=glyph.glyph.reserved;return SfntStatus::ok;
}
SfntStatus snapshot(Policy& p,Slice<SfntComponentGlyph> glyphs,Work& w) noexcept {
    const u32 count=glyphs.count;if(count>p.snapshots.count)return SfntStatus::output_too_small;
    if(!w.spend(count))return SfntStatus::resource_limit;
    for(u32 i=0;i<count;++i)p.snapshots[i]={glyphs[i].component,glyphs[i].glyph.glyph};
    p.snapshot_count=count;return SfntStatus::ok;
}
SfntStatus changed(Policy& p,Slice<SfntComponentGlyph> glyphs,u32& node,Work& w,bool reph=false) noexcept {
    node=nil;u32 cursor=0,last=nil;
    for(u32 i=0;i<glyphs.count;++i){if(!w.spend(1))return SfntStatus::resource_limit;bool same=false;
        // Unedited runs compare once per glyph. After an edit, stable component
        // identity finds the surviving suffix without marking shifted glyphs.
        for(u32 j=cursor;j<p.snapshot_count;++j){if(!w.spend(1))return SfntStatus::resource_limit;
            if(glyphs[i].component==p.snapshots[j].component&&glyphs[i].glyph.glyph==p.snapshots[j].glyph){same=true;cursor=j+1;break;}}
        if(!same){last=i;node=glyphs[i].component;if(!reph)break;}
    }if(reph&&last!=nil){glyphs[last].glyph.reserved|=reph_role;p.reph_formed=true;}return SfntStatus::ok;
}
SfntStatus reorder(Policy& p,Slice<SfntComponentGlyph> glyphs,Work& w) noexcept {
    u32 pref=nil;SfntStatus status=SfntStatus::ok;
    for(u32 at=0;at<glyphs.count;){
        if(!(glyphs[at].glyph.reserved&reph_role)){if(!w.spend(1))return SfntStatus::resource_limit;++at;continue;}
        glyphs[at].glyph.reserved&=static_cast<u8>(~reph_role);bool moved=false;
        for(u32 i=at+1;i<glyphs.count;++i){u8 r=0;status=role(glyphs[i],&r,w);if(status!=SfntStatus::ok)return status;
            if((r&halant_role)&&glyphs[i].glyph.glyph==p.halant)break;
            if((r&base_role)&&!(r&reph_role)&&sfnt_gdef_glyph_class(*p.gdef,glyphs[i].glyph.glyph)!=SfntGlyphClass::mark){
                glyphs[at].attachment=glyphs[i].component;status=move_before(glyphs,at,i+1,w);if(status!=SfntStatus::ok)return status;moved=true;break;
            }
        }
        if(!moved)++at;
    }
    status=locate(*p.forest,glyphs,p.pref,&pref,w);if(status!=SfntStatus::ok)return status;
    u32 start=0;
    for(u32 i=0;i<glyphs.count;++i){u8 r=0;status=role(glyphs[i],&r,w);if(status!=SfntStatus::ok)return status;
        if((r&halant_role)&&glyphs[i].glyph.glyph==p.halant)start=i+1;
    }
    if(pref!=nil&&pref>start){status=move_before(glyphs,pref,start,w);if(status!=SfntStatus::ok)return status;}
    // USE moves one original pre-vowel span. Joiners are ignored when
    // counting that span, while CGJ inherits its preceding positional class.
    // Font expansion does not increase this original span width.
    for(u32 i=start;i<glyphs.count&&p.pre_count;++i){if(!w.spend(1))return SfntStatus::resource_limit;
        if(!(glyphs[i].glyph.reserved&pre_role)||glyphs[i].component==nil)continue;
        const u32 amount=minimum<u32>(p.pre_count,glyphs.count-i);
        if(!w.spend(static_cast<u64>(i+amount-start)*3))return SfntStatus::resource_limit;
        const auto reverse=[&](u32 first,u32 last) noexcept {while(first<last&&first<--last){const auto saved=glyphs[first];glyphs[first++]=glyphs[last];glyphs[last]=saved;}};
        reverse(start,i);reverse(i,i+amount);reverse(start,i+amount);break;
    }
    return SfntStatus::ok;
}
SfntStatus finish(void* context,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    auto& p=*static_cast<Policy*>(context);if(!p.forest)return SfntStatus::invalid_input;
    if(zero.count<glyphs.count)return SfntStatus::output_too_small;Work w{work};
    for(u32 i=0;i<glyphs.count;++i){u8 r=0;auto status=role(glyphs[i],&r,w);if(status!=SfntStatus::ok)return status;
        auto& g=glyphs[i].glyph;
        const bool control=p.control_ids?(g.glyph==p.controls[0]||g.glyph==p.controls[1]||g.glyph==p.controls[2]||g.glyph==p.controls[3]):
            (r&control_role)&&glyphs[i].component!=nil&&p.forest->nodes[glyphs[i].component].count==1;
        const bool invisible=control&&(p.control_ids||!(r&cgj_role)||!g.glyph);
        if(invisible)g.glyph=p.space;g.glyph_class=sfnt_gdef_glyph_class(*p.gdef,g.glyph);g.reserved=static_cast<u8>(control);
        zero[i]=static_cast<u8>(invisible||g.glyph_class==SfntGlyphClass::mark);
    }return SfntStatus::ok;
}
SfntStatus map_cluster(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    const SfntSinhalaInputScalar* expanded,const SfntSinhalaInputResult& sizes,bool rtl,SfntClusterGlyph* mapped,u32& count) noexcept {
    SfntStatus status=SfntStatus::ok;
    count=0;
    auto map=[&](u32 scalar,u32 source,u8 role,u32 selector=0) noexcept {
        const u32 mapped_scalar=rtl?unicode_bidi_mirror(scalar):scalar;
        u16 glyph=0;auto s=sfnt_cmap_glyph(cmap,mapped_scalar,selector,&glyph);
        if(selector&&s==SfntStatus::unsupported_sequence)s=sfnt_cmap_glyph(cmap,mapped_scalar,0,&glyph);
        if(s!=SfntStatus::ok)return s;
        if(!glyph&&unicode_properties(scalar)->general_category()==UnicodeGeneralCategory::cn)role|=control_role;
        mapped[count++]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),role},source};return SfntStatus::ok;
    };
    if(input.count){const auto* p=unicode_properties(input[0]);const auto gc=p->general_category();
        if((gc==UnicodeGeneralCategory::mn||gc==UnicodeGeneralCategory::mc)&&!p->has(UnicodeFlag::default_ignorable_code_point)){
            status=map(0x25cc,0,base_role);if(status!=SfntStatus::ok)return status;}}
    for(u32 i=0;i<sizes.scalar_count;++i){const u32 scalar=expanded[i].scalar;const auto c=sinhala_category(scalar);
        if(c==SinhalaCategory::selector&&i)continue;
        const bool piece=i&&expanded[i].source==expanded[i-1].source;
        const auto* p=unicode_properties(scalar);
        const u8 role=p->has(UnicodeFlag::default_ignorable_code_point)?static_cast<u8>(control_role|(c==SinhalaCategory::joiner?join_role:c==SinhalaCategory::cgj?cgj_role:0)):piece?0:
            c==SinhalaCategory::base||c==SinhalaCategory::number||c==SinhalaCategory::placeholder?base_role:c==SinhalaCategory::halant?halant_role:c==SinhalaCategory::pre?pre_role:0;
        const u32 selector=i+1<sizes.scalar_count&&sinhala_category(expanded[i+1].scalar)==SinhalaCategory::selector?expanded[i+1].scalar:0;
        status=map(scalar,expanded[i].source,role,selector);if(status!=SfntStatus::ok)return status;
    }
    return SfntStatus::ok;
}

struct RunPolicy {
    Policy cluster;const SfntShapePlan* plan;ConstSlice<SfntLookupStep> early;
    Slice<u32> groups,boundaries;SfntShapeLimits limits;u64 edits=0;u32 boundary_count=0;
};
static_assert(sizeof(RunPolicy)==144);
SfntStatus run_prepare(void* context,u16 stage,SfntGlyphBuffer* buffer,SfntClusterMap* clusters,
    SfntComponentForest* forest,const SfntGsubExecutionScratch& scratch,u64 available,u64* used) noexcept {
    *used=0;if(stage)return SfntStatus::ok;
    auto& run=*static_cast<RunPolicy*>(context);auto& p=run.cluster;p.forest=forest;forest->preserve_forms=false;
    Work w{available};struct Report {Work& w;u64* out;~Report() noexcept {*out=w.used;}} report{w,used};
    SfntStatus status=SfntStatus::ok;
    for(u32 first=0;first<sfnt_glyph_count(sfnt_glyph_buffer_view(*buffer));){
        const auto view=sfnt_glyph_buffer_view(*buffer);const u32 total=sfnt_glyph_count(view);
        const auto& initial=*reinterpret_cast<const SfntComponentGlyph*>(&sfnt_glyph_at(view,first));
        const u32 group=run.groups[initial.source];u32 end=first;p.pre_count=0;p.pref=nil;p.reph_formed=false;
        bool pre=false;
        while(end<total){const auto& value=*reinterpret_cast<const SfntComponentGlyph*>(&sfnt_glyph_at(view,end));
            if(!w.spend(1))return SfntStatus::resource_limit;if(run.groups[value.source]!=group)break;++end;
            const u8 r=value.glyph.reserved;if(r&join_role)continue;
            if(!(r&cgj_role))pre=(r&pre_role)!=0;if(pre)++p.pre_count;}
        SfntGlyphBuffer local{};
        status=open_group(w,*buffer,first,end,local);if(status!=SfntStatus::ok)return status;
        u32 step=0;
        for(u16 phase=0;phase<=4;++phase){
            Slice<SfntComponentGlyph> current{reinterpret_cast<SfntComponentGlyph*>(local.storage.data),sfnt_glyph_count(sfnt_glyph_buffer_view(local))};
            if(phase==1&&p.has_reph)status=snapshot(p,current,w);
            else if(phase==2){if(p.has_reph){u32 ignored=nil;status=changed(p,current,ignored,w,true);}
                if(status==SfntStatus::ok&&p.has_pref)status=snapshot(p,current,w);}
            else if(phase==3&&p.has_pref)status=changed(p,current,p.pref,w);
            else if(phase==4&&(p.reph_formed||p.pref!=nil||p.pre_count))status=reorder(p,current,w);
            if(status!=SfntStatus::ok)return status;
            status=run_early(w,*run.plan,run.early,step,phase,local,scratch,run.limits.match_work,clusters,forest,run.edits);if(status!=SfntStatus::ok)return status;
        }
        buffer->gap_begin=first+local.gap_begin;buffer->gap_end=first+local.gap_end;
        first=buffer->gap_begin;
    }
    return SfntStatus::ok;
}
SfntStatus run_finish(void* context,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    return finish(&static_cast<RunPolicy*>(context)->cluster,glyphs,zero,work);
}
SfntStatus run_restore(void* context,SfntClusterMap* clusters,const SfntComponentForest& forest,Slice<SfntComponentGlyph> glyphs,u64 work) noexcept {
    auto& p=*static_cast<RunPolicy*>(context);
    return use_shape::restore_groups(p.groups,p.boundaries,p.boundary_count,clusters,forest,glyphs,work);
}

}
SfntStatus use_shape::seek(Work& w,SfntGlyphBuffer& value,u32 at) noexcept {
    const u64 cost=(at>value.gap_begin?at-value.gap_begin:value.gap_begin-at)+1ull;
    if(!w.spend(cost))return SfntStatus::resource_limit;return sfnt_glyph_buffer_seek(&value,at,cost);
}
SfntStatus use_shape::open_group(Work& w,SfntGlyphBuffer& buffer,u32 first,u32 end,SfntGlyphBuffer& local) noexcept {
    const auto status=seek(w,buffer,end);if(status!=SfntStatus::ok)return status;
    return sfnt_glyph_buffer_open({buffer.storage.data+first*sizeof(SfntComponentGlyph),
        (buffer.gap_end-first)*static_cast<u32>(sizeof(SfntComponentGlyph))},sizeof(SfntComponentGlyph),end-first,&local);
}
SfntStatus use_shape::run_early(Work& w,const SfntShapePlan& plan,ConstSlice<SfntLookupStep> early,u32& step,u32 phase,SfntGlyphBuffer& local,
    const SfntGsubExecutionScratch& scratch,u32 match_work,SfntClusterMap* clusters,SfntComponentForest* forest,u64& edits) noexcept {
    while(step<early.count&&(phase==0xffffu||early[step].stage==phase)){
        const u32 count=sfnt_glyph_count(sfnt_glyph_buffer_view(local));
        if(plan.refresh_substitution_classes){if(!w.spend(count))return SfntStatus::resource_limit;
            auto* values=reinterpret_cast<SfntComponentGlyph*>(local.storage.data);
            for(u32 i=0;i<count;++i)values[i].glyph.glyph_class=sfnt_gdef_glyph_class(plan.gdef,values[i].glyph.glyph);}
        SfntGsubExecutionResult execution{};
        auto status=sfnt_gsub_execute_lookup(*plan.substitutions,early[step++].lookup,&local,0,count,0,scratch,{w.left,match_work},&execution,clusters,forest);
        if(!w.spend(execution.operations))return SfntStatus::invalid_input;edits+=execution.edits;
        if(status!=SfntStatus::ok)return status;
        status=seek(w,local,sfnt_glyph_count(sfnt_glyph_buffer_view(local)));if(status!=SfntStatus::ok)return status;
    }
    return SfntStatus::ok;
}
SfntStatus use_shape::locate(const SfntComponentForest& forest,const Slice<SfntComponentGlyph>& glyphs,u32 node,u32* index,Work& w) noexcept {
    *index=0xffffffffu;if(node==0xffffffffu)return SfntStatus::ok;u32 root=0,ordinal=0;u64 used=0;
    const auto status=sfnt_component_location(forest,node,&root,&ordinal,w.left,&used);w.spend(used);
    if(status!=SfntStatus::ok||root==0xffffffffu)return status;
    if(!w.spend(glyphs.count))return SfntStatus::resource_limit;
    for(u32 i=0;i<glyphs.count;++i)if(glyphs[i].component==root){*index=i;break;}return SfntStatus::ok;
}
SfntStatus use_shape::move_before(const Slice<SfntComponentGlyph>& view,u32 from,u32 before,Work& w) noexcept {
    const Slice<SfntComponentGlyph> glyphs=view; // the record stores below must not force reloads
    if(from==0xffffffffu||before==0xffffffffu)return SfntStatus::ok;
    const u32 to=before>from?before-1:before;if(from>=glyphs.count||to>=glyphs.count)return SfntStatus::invalid_input;
    if(!w.spend((from>to?from-to:to-from)+1))return SfntStatus::resource_limit;
    const auto saved=glyphs[from];if(from<to)for(u32 i=from;i<to;++i)glyphs[i]=glyphs[i+1];
    else for(u32 i=from;i>to;--i)glyphs[i]=glyphs[i-1];glyphs[to]=saved;return SfntStatus::ok;
}
SfntStatus use_shape::admit_run(const SfntShapePlan& plan,ConstSlice<u32> input,ConstSlice<SfntLookupStep> steps,const SfntShapeStorage& storage,
    const SfntShapeLimits& limits,u32* early) noexcept {
    if((input.count&&!input.data)||(steps.count&&!steps.data)||plan.substitution_roots.count)return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*8+steps.count>limits.stage_work)return SfntStatus::resource_limit;
    if(steps.count&&!plan.substitutions)return SfntStatus::invalid_input;
    *early=0;
    for(u32 i=0;i<steps.count;++i){if(steps[i].feature_mask||steps[i].stage>=7||(i&&steps[i].stage<steps[i-1].stage)||
        steps[i].lookup>=plan.substitutions->lookups.count)return SfntStatus::invalid_input;if(steps[i].stage<5)++*early;}
    u64 total_units=0;for(u32 i=0;i<input.count;++i){if(!unicode_is_scalar(input[i]))return SfntStatus::invalid_input;total_units+=input[i]>0xffff?2u:1u;}
    if(total_units>0xffffffffull)return SfntStatus::resource_limit;
    if(total_units>storage.character_glyphs.count)return SfntStatus::output_too_small;
    if((storage.character_glyphs.count&&!storage.character_glyphs.data)||(storage.positions.count&&!storage.positions.data)||
        (storage.targets.count&&!storage.targets.data))return SfntStatus::invalid_input;
    return SfntStatus::ok;
}
SfntStatus sfnt_sinhala_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,
    float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,const SfntShapeLimits& limits,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    u32 early=0;
    if(const auto admitted=use_shape::admit_run(plan,input,steps,storage,limits,&early);admitted!=SfntStatus::ok)return admitted;
    auto* mapped=reinterpret_cast<SfntClusterGlyph*>(storage.positions.data);
    const u64 capacity=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntClusterGlyph);
    u32 count=0,units=0,starts=0;
    for(u32 first=0;first<input.count;){const u32 window=minimum<u32>(32,input.count-first);u8 boundaries[32];
        auto status=sfnt_sinhala_cluster_boundaries({input.data+first,window},{boundaries,32},limits.stage_work);if(status!=SfntStatus::ok)return status;
        const u32 origin=first;
        for(u32 at=0;at<window;){u32 length=1;while(at+length<window&&!boundaries[at+length])++length;
            if(at+length==window&&origin+window<input.count)break;
            const ConstSlice<u32> cluster{input.data+origin+at,length};SfntSinhalaInputScalar expanded[62];SfntSinhalaInputResult sizes{};
            status=sfnt_sinhala_expand_cluster(cluster,{expanded,62},&sizes,limits.stage_work);if(status!=SfntStatus::ok)return status;
            SfntClusterGlyph local[63];u32 amount=0;status=map_cluster(plan,cmap,cluster,expanded,sizes,rtl,local,amount);if(status!=SfntStatus::ok)return status;
            if(static_cast<u64>(count)+amount>capacity||starts>=storage.targets.count)return SfntStatus::output_too_small;
            storage.targets[starts++]=units;
            for(u32 i=0;i<amount;++i){local[i].source+=units;mapped[count++]=local[i];}
            for(u32 i=0;i<sizes.source_units;++i)storage.character_glyphs[units+i]=units;
            units+=sizes.source_units;at+=length;first=origin+at;
        }
    }
    return use_shape::run(plan,cmap,{mapped,count},units,starts,early,0xdca,em,storage,result,steps,limits,rtl);
}
SfntStatus use_shape::finish_group_run(void* context,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    const auto& p=*static_cast<GroupRun*>(context);
    if(zero.count<glyphs.count)return SfntStatus::output_too_small;
    if(glyphs.count>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i].glyph;
        // Only the nominal IDs count, even when another scalar maps to one; a
        // substitution to a different ID stays visible.
        const bool invisible=g.glyph==p.invisible[0]||g.glyph==p.invisible[1]||g.glyph==p.invisible[2]||g.glyph==p.invisible[3];
        if(invisible)g.glyph=p.space;g.glyph_class=sfnt_gdef_glyph_class(*p.gdef,g.glyph);g.reserved=static_cast<u8>(invisible);
        zero[i]=static_cast<u8>(invisible||g.glyph_class==SfntGlyphClass::mark);
    }return SfntStatus::ok;
}
SfntStatus use_shape::restore_group_run(void* context,SfntClusterMap* clusters,const SfntComponentForest& forest,Slice<SfntComponentGlyph> glyphs,u64 work) noexcept {
    auto& p=*static_cast<GroupRun*>(context);
    return restore_groups(p.groups,p.boundaries,p.boundary_count,clusters,forest,glyphs,work);
}
SfntStatus use_shape::restore_groups(Slice<u32> groups,Slice<u32> boundaries,u32 boundary_count,SfntClusterMap* clusters,
    const SfntComponentForest& forest,Slice<SfntComponentGlyph> glyphs,u64 work) noexcept {
    Work w{work};
    // Late ligatures retain the last consumed source group. Reordered prefixes
    // may still own earlier groups, and decomposition can restore those groups.
    for(u32 i=0;i<glyphs.count;++i){u32 lo=nil,hi=0;u64 cost=0;
        auto status=sfnt_component_source_bounds(forest,glyphs[i].component,&lo,&hi,w.left,&cost);w.spend(cost);
        if(status!=SfntStatus::ok)return status;if(!w.spend(1))return SfntStatus::resource_limit;
        const u32 source=lo==nil?glyphs[i].source:hi;if(source>=groups.count)return SfntStatus::invalid_input;
        glyphs[i].source=groups[source];
    }
    if(!w.spend(static_cast<u64>(boundary_count)*3+glyphs.count))return SfntStatus::resource_limit;
    for(u32 i=0;i<boundary_count;++i)groups[boundaries[i]]=nil;
    for(u32 i=0;i<glyphs.count;++i){auto& first=groups[glyphs[i].source];first=minimum(first,i);}
    // Earliest glyph ownership determines monotone source groups. A later mark
    // from an earlier group does not erase a boundary restored by decomposition.
    u32 next=nil;
    for(u32 i=boundary_count;i;){auto& first=groups[boundaries[--i]];next=minimum(next,first);first=next;}
    u32 count=0,previous=nil;
    for(u32 i=0;i<boundary_count;++i){const u32 source=boundaries[i];const u32 value=groups[source];
        if(!i||(value!=nil&&value!=previous)){boundaries[count++]=source;previous=value;}}
    const u64 cost=static_cast<u64>(sfnt_cluster_map_words(groups.count))*3+count;
    if(!w.spend(cost))return SfntStatus::resource_limit;
    return sfnt_cluster_map_open(groups.count,{boundaries.data,count},clusters->storage,clusters,cost);
}
SfntStatus use_shape::run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntClusterGlyph> mapped,u32 units,u32 starts,
    u32 early,u32 halant_scalar,float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,
    const SfntShapeLimits& limits,bool rtl,const u16* control_glyphs) noexcept {
    RunPolicy policy{{&plan.gdef,{reinterpret_cast<Snapshot*>(storage.positions.data),storage.positions.count}},&plan,
        {steps.data,early},{storage.character_glyphs.data,units},storage.targets,limits};policy.boundary_count=starts;
    for(u32 i=0;i<early;++i){policy.cluster.has_reph|=steps[i].stage==1;policy.cluster.has_pref|=steps[i].stage==2;}
    auto status=SfntStatus::ok;
    if(halant_scalar){status=sfnt_cmap_glyph(cmap,halant_scalar,0,&policy.cluster.halant);if(status!=SfntStatus::ok)return status;}
    if(control_glyphs){copy_bytes(policy.cluster.controls,control_glyphs,sizeof(policy.cluster.controls));policy.cluster.control_ids=true;}
    status=sfnt_cmap_glyph(cmap,0x20,0,&policy.cluster.space);if(status!=SfntStatus::ok)return status;
    const SfntShapeStagePolicy stages{&policy,run_prepare,7};
    const SfntShapeGlyphPolicy glyph_policy{&policy,run_finish,run_restore};
    auto resolved=plan;resolved.retain_zero_positioning=true;
    status=sfnt_shape_run(resolved,mapped,units,{storage.targets.data,starts},rtl,em,storage,result,limits,
        {steps.data?steps.data+early:nullptr,steps.count-early},{},glyph_policy,nullptr,&stages);
    result->substitution_edits+=policy.edits;return status;
}

}
