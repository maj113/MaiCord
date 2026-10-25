#include "maicord/core/memory.hpp"
#include "maicord/text/devanagari_order.hpp"
#include "devanagari_character.hpp"
#include "indic_cluster.hpp"
#include "indic_forms.hpp"
namespace maicord {namespace {
constexpr u32 nil=0xffffffffu;
constexpr u8 halant_role=1,control_role=2,sign_role=4,post_role=8,spacing_role=16,pre_attachment_role=32,join_role=64,half_nukta_role=128;
struct Policy {
    const SfntGdef* gdef;ConstSlice<u32> scalars,order;ConstSlice<u8> forms;SfntDevanagariOrderResult initial;
    SfntComponentForest* forest=nullptr;u32 base=nil,pre=nil,reph=nil,units=0;u16 halant=0,space=0;
    SfntIndicProfile profile=SfntIndicProfile::devanagari;bool word_initial=false;u16 pref_glyph=0;
};
static_assert(sizeof(Policy)==104);
struct Work {
    u64 left,used=0;
    bool spend(u64 n) noexcept {if(n>left)return false;left-=n;used+=n;return true;}
};
SfntStatus locate(const SfntComponentForest& forest,Slice<SfntComponentGlyph> glyphs,u32 node,u32* index,Work& work) noexcept {
    *index=nil;if(node==nil)return SfntStatus::ok;u32 root=0,ordinal=0;u64 used=0;
    auto status=sfnt_component_location(forest,node,&root,&ordinal,work.left,&used);work.spend(used);if(status!=SfntStatus::ok)return status;
    if(root==nil)return SfntStatus::ok;
    if(!work.spend(glyphs.count))return SfntStatus::resource_limit;
    for(u32 i=0;i<glyphs.count;++i)if(glyphs[i].component==root){*index=i;break;}return SfntStatus::ok;
}
SfntStatus role(const SfntComponentForest& forest,const SfntComponentGlyph& glyph,u8* value,Work& work) noexcept {
    *value=0;if(glyph.component==nil)return SfntStatus::ok;u64 used=0;
    const auto status=sfnt_component_first_metadata(forest,glyph.component,value,work.left,&used);work.spend(used);return status;
}
SfntStatus move_before(Slice<SfntComponentGlyph> glyphs,u32 from,u32 before,Work& work) noexcept {
    if(from==nil)return SfntStatus::ok;const u32 target=before>from?before-1:before;
    if(from>=glyphs.count||target>=glyphs.count)return SfntStatus::invalid_input;
    const u32 distance=from>target?from-target:target-from;if(!work.spend(distance+1))return SfntStatus::resource_limit;
    const auto saved=glyphs[from];if(from<target)for(u32 i=from;i<target;++i)glyphs[i]=glyphs[i+1];
    else for(u32 i=from;i>target;--i)glyphs[i]=glyphs[i-1];glyphs[target]=saved;return SfntStatus::ok;
}
SfntStatus move_group_before(Slice<SfntComponentGlyph> glyphs,u32 from,u32 amount,u32 before,Slice<u8> scratch,Work& work) noexcept {
    if(from>glyphs.count||amount>glyphs.count-from||before>glyphs.count)return SfntStatus::invalid_input;
    if(before>=from&&before<=from+amount)return SfntStatus::ok;
    const u32 target=before>from?before-amount:before;
    const u32 bytes=amount*static_cast<u32>(sizeof(SfntComponentGlyph));if(scratch.count<bytes)return SfntStatus::output_too_small;
    if(!scratch.data)return SfntStatus::invalid_input;
    const u32 distance=target>from?target-from:from-target;if(!work.spend(static_cast<u64>(amount)*2+distance))return SfntStatus::resource_limit;
    copy_bytes(scratch.data,glyphs.data+from,bytes);
    if(target>from)for(u32 i=from;i<target;++i)glyphs[i]=glyphs[i+amount];else for(u32 i=from;i-->target;)glyphs[i+amount]=glyphs[i];
    copy_bytes(glyphs.data+target,scratch.data,bytes);return SfntStatus::ok;
}
SfntStatus prepare(void* opaque,u16 stage,SfntGlyphBuffer* buffer,SfntClusterMap*,SfntComponentForest* forest,SfntGsubExecutionScratch scratch,u64 available,u64* used) noexcept {
    auto& p=*static_cast<Policy*>(opaque);Work work{available};struct Report {Work& w;u64* out;~Report() noexcept {*out=w.used;}} report{work,used};
    const bool initial=stage==12&&p.profile==SfntIndicProfile::bengali;
    const bool pref_boundary=(p.profile==SfntIndicProfile::telugu||p.profile==SfntIndicProfile::kannada||p.profile==SfntIndicProfile::malayalam)&&(stage==static_cast<u16>(SfntDevanagariStage::pre_base)||stage==static_cast<u16>(SfntDevanagariStage::below_base));
    if(stage!=0&&stage!=static_cast<u16>(SfntDevanagariStage::final_reorder)&&!initial&&!pref_boundary)return SfntStatus::ok;
    const u32 count=sfnt_glyph_count(sfnt_glyph_buffer_view(*buffer));if(!work.spend(static_cast<u64>(count)*2))return SfntStatus::resource_limit;
    auto status=sfnt_glyph_buffer_seek(buffer,count,static_cast<u64>(count)*2);if(status!=SfntStatus::ok)return status;
    Slice<SfntComponentGlyph> glyphs{reinterpret_cast<SfntComponentGlyph*>(buffer->storage.data),count};
    // The original matra slot is dead here (Malayalam keeps its node in
    // initial.base), so it is borrowed across pref; the glyph identity fits
    // existing padding. This keeps an unsubstituted glyph from looking eligible
    // for pre-base reordering at later stages.
    if(pref_boundary){u32 at=nil;status=locate(*forest,glyphs,p.pre,&at,work);if(status!=SfntStatus::ok)return status;
        if(stage==static_cast<u16>(SfntDevanagariStage::pre_base)){
            p.initial.pre_matra=at==nil?nil:glyphs[at].component;p.pref_glyph=at==nil?0:glyphs[at].glyph.glyph;
        }else{if(at==nil||(glyphs[at].component==p.initial.pre_matra&&glyphs[at].glyph.glyph==p.pref_glyph))p.pre=nil;p.initial.pre_matra=nil;}
        return SfntStatus::ok;}
    if(initial){if(!work.spend(count))return SfntStatus::resource_limit;
        for(u32 i=0;i<count;++i)glyphs[i].glyph.reserved&=static_cast<u8>(~sfnt_bengali_initial);
        u32 pre=nil;if(p.word_initial){status=locate(*forest,glyphs,p.pre,&pre,work);if(status!=SfntStatus::ok)return status;
            if(pre==0)glyphs[pre].glyph.reserved|=sfnt_bengali_initial;}return SfntStatus::ok;}
    if(!stage){u32 malayalam_matra=nil;p.forest=forest;if(count!=p.scalars.count)return SfntStatus::invalid_input;
        u32 pre_end=p.initial.pre_matra;
        if(pre_end!=nil){if(!work.spend(count))return SfntStatus::resource_limit;++pre_end;while(pre_end<count){const auto c=indic_category(p.profile,p.scalars[pre_end]);if(c!=DevanagariCategory::nukta&&c!=DevanagariCategory::halant)break;++pre_end;}}
        for(u32 i=0;i<count;++i){const u32 original=p.order[i],node=glyphs[i].component;if(original>=p.scalars.count||node>=forest->nodes.count)return SfntStatus::invalid_input;
            const auto c=indic_category(p.profile,p.scalars[original]);u8 metadata=0;
            if(c==DevanagariCategory::halant)metadata|=halant_role;
            // Malayalam does not use half-nukta metadata. Retain whether the
            // first full glyph belongs to an independent-vowel main group.
            if(p.profile==SfntIndicProfile::malayalam){
                if(c==DevanagariCategory::vowel)metadata|=half_nukta_role;
                // Retain source roles when cmap/GDEF assign a different class.
                if(p.scalars[original]==0xd4e)metadata|=half_nukta_role|sign_role;
                if(original>p.initial.base&&(p.forms[original]&sfnt_deva_post))metadata|=half_nukta_role;
            }
            // This script does not use the half-nukta role. Reuse that bit for
            // marks tied to the main base, even if a lower form later expands.
            if(p.profile==SfntIndicProfile::odia&&(c==DevanagariCategory::above||c==DevanagariCategory::bindu||(!original&&p.initial.reph_end)))metadata|=half_nukta_role;
            if((p.profile==SfntIndicProfile::gujarati||p.profile==SfntIndicProfile::gurmukhi)&&c==DevanagariCategory::nukta){u32 before=original;
                while(before){if(!work.spend(1))return SfntStatus::resource_limit;--before;const auto previous=indic_category(p.profile,p.scalars[before]);
                    if(previous==DevanagariCategory::nukta||previous==DevanagariCategory::halant)continue;
                    if(p.forms[before]&sfnt_deva_half)metadata|=half_nukta_role;break;}}
            if(c==DevanagariCategory::joiner||c==DevanagariCategory::nonjoiner)metadata|=control_role;
            if(c==DevanagariCategory::joiner||(p.profile!=SfntIndicProfile::devanagari&&p.scalars[original]==0xa0))metadata|=join_role;
            if(c==DevanagariCategory::bindu||c==DevanagariCategory::visarga||c==DevanagariCategory::vedic)metadata|=sign_role;
            if(c==DevanagariCategory::visarga)metadata|=spacing_role;
            // This combination is unused by Malayalam post vowels/consonants.
            // It keeps a fallback accent with its overt halant for reph order.
            if(p.profile==SfntIndicProfile::malayalam&&(p.forms[0]&indic_default_model)&&original&&p.scalars[original-1]==0xd4d&&
                (p.scalars[original]==0x953||p.scalars[original]==0x954))metadata|=post_role;

            if(p.profile!=SfntIndicProfile::gurmukhi&&p.initial.pre_matra!=nil&&original>p.initial.pre_matra&&original<pre_end&&c==DevanagariCategory::nukta)metadata|=pre_attachment_role;
            if((p.profile==SfntIndicProfile::bengali||p.profile==SfntIndicProfile::odia||p.profile==SfntIndicProfile::malayalam)&&c==DevanagariCategory::pre&&original!=p.initial.pre_matra)metadata|=pre_attachment_role;
            if(original>p.initial.base&&(p.forms[original]&sfnt_deva_post))metadata|=post_role;
            if((p.profile==SfntIndicProfile::bengali||p.profile==SfntIndicProfile::odia||p.profile==SfntIndicProfile::malayalam)&&c==DevanagariCategory::post)metadata|=post_role;
            if(p.profile==SfntIndicProfile::odia&&(c==DevanagariCategory::below||(original>p.initial.base&&(p.forms[original]&sfnt_deva_below))))metadata|=post_role;
            if((p.profile==SfntIndicProfile::bengali||p.profile==SfntIndicProfile::odia)&&c==DevanagariCategory::halant&&original>p.initial.base){u32 next=original+1;
                while(next<count){if(!work.spend(1))return SfntStatus::resource_limit;if(indic_category(p.profile,p.scalars[next])!=DevanagariCategory::nukta)break;++next;}
                if(next<count&&(p.forms[next]&(sfnt_deva_post|(p.profile==SfntIndicProfile::odia?sfnt_deva_below:0))))metadata|=post_role;}
            if(p.initial.tail_control==original+1)metadata=control_role;
            if(p.initial.tail_sign==original+1)metadata=p.profile==SfntIndicProfile::kannada?halant_role:static_cast<u8>(sign_role|(p.scalars[p.scalars.count-1]==sfnt_indic_script(p.profile)->block_first+1u?half_nukta_role:spacing_role));
            forest->nodes[node].reserved=static_cast<u16>((forest->nodes[node].reserved&255u)|(static_cast<u16>(metadata)<<8));
            if((p.profile==SfntIndicProfile::telugu||p.profile==SfntIndicProfile::kannada||p.profile==SfntIndicProfile::malayalam)&&p.pre==nil&&(glyphs[i].glyph.reserved&sfnt_deva_pre)&&(p.forms[original]&sfnt_deva_pre))p.pre=node;
            if(original==p.initial.base)p.base=node;if(original==p.initial.pre_matra){if(p.profile==SfntIndicProfile::malayalam)malayalam_matra=node;else p.pre=node;}if(!original&&p.initial.reph_end)p.reph=node;
        }
        // The source base is no longer needed once metadata is stored; Malayalam's
        // separate left-vowel node takes its place across the pref stages.
        if(p.profile==SfntIndicProfile::malayalam)p.initial.base=malayalam_matra;
        return SfntStatus::ok;
    }
    const bool direct_control=p.profile==SfntIndicProfile::malayalam&&p.scalars.count>1&&
        (p.scalars[1]==0x200d||p.scalars[1]==0x200c||p.scalars[1]==0xa0);
    u32 base=nil,pre=nil,reph=nil;
    status=locate(*forest,glyphs,p.base,&base,work);if(status!=SfntStatus::ok)return status;
    if(base==reph)return SfntStatus::ok;
    status=locate(*forest,glyphs,p.pre,&pre,work);if(status!=SfntStatus::ok)return status;
    if((p.profile==SfntIndicProfile::telugu||p.profile==SfntIndicProfile::kannada||p.profile==SfntIndicProfile::malayalam)&&base!=nil&&pre!=nil&&(pre>base||(p.profile==SfntIndicProfile::malayalam&&pre!=base))&&
        glyphs[pre].component!=nil&&forest->nodes[glyphs[pre].component].count>=2){
        for(u32 i=pre+1;i<count;++i)if(glyphs[i].glyph.glyph_class==SfntGlyphClass::mark){u32 owner=nil;
            status=locate(*forest,glyphs,glyphs[i].attachment,&owner,work);if(status!=SfntStatus::ok)return status;
            if(owner==pre)glyphs[i].attachment=p.base;}
        u32 target=0;
        if(p.profile==SfntIndicProfile::malayalam){u32 matra=nil;status=locate(*forest,glyphs,p.initial.base,&matra,work);if(status!=SfntStatus::ok)return status;
            if(matra!=nil){target=matra+1;while(target<base){u8 metadata=0;status=role(*forest,glyphs[target],&metadata,work);if(status!=SfntStatus::ok)return status;if(!(metadata&pre_attachment_role))break;++target;}}
            if(direct_control)++target;}for(u32 i=0;i<base;++i){u8 metadata=0;status=role(*forest,glyphs[i],&metadata,work);if(status!=SfntStatus::ok)return status;
            if((metadata&halant_role)&&glyphs[i].glyph.glyph==p.halant){target=i+1;
                if(target<base){status=role(*forest,glyphs[target],&metadata,work);if(status!=SfntStatus::ok)return status;if(metadata&control_role)++target;}}}
        // An already pre-base form must not move right across later halants.
        if(p.profile==SfntIndicProfile::malayalam&&target>pre)target=pre;
        status=move_before(glyphs,pre,target,work);if(status!=SfntStatus::ok)return status;}
    if(p.profile==SfntIndicProfile::malayalam){status=locate(*forest,glyphs,p.initial.base,&pre,work);if(status!=SfntStatus::ok)return status;
        status=locate(*forest,glyphs,p.base,&base,work);if(status!=SfntStatus::ok)return status;
    // Contextual blwf can turn the selected base into a subjoined mark even
    // when the font's context-free pair query found no below form.
    if(p.profile==SfntIndicProfile::malayalam&&base!=nil&&sfnt_gdef_glyph_class(*p.gdef,glyphs[base].glyph.glyph)==SfntGlyphClass::mark){
        for(u32 before=base;before--;){u8 metadata=0;status=role(*forest,glyphs[before],&metadata,work);if(status!=SfntStatus::ok)return status;
            if(sfnt_gdef_glyph_class(*p.gdef,glyphs[before].glyph.glyph)!=SfntGlyphClass::mark&&!(metadata&(control_role|join_role))){base=before;break;}}}
    }
    if(p.profile!=SfntIndicProfile::telugu&&p.profile!=SfntIndicProfile::kannada&&base!=nil&&pre!=nil&&pre<base){u32 target=pre,attached=1;
        while(pre+attached<base){u8 metadata=0;status=role(*forest,glyphs[pre+attached],&metadata,work);if(status!=SfntStatus::ok)return status;
            if(!(metadata&pre_attachment_role))break;++attached;}
        bool first_main=true;
        for(u32 i=pre+attached;i<base;++i){u8 metadata=0;status=role(*forest,glyphs[i],&metadata,work);if(status!=SfntStatus::ok)return status;
            // A bare NBSP prefix leaves its mark-class dot before the left
            // vowel. A nominal base-class glyph has no such mark affinity.
            if(p.profile==SfntIndicProfile::malayalam&&p.scalars.count>1&&p.scalars[0]==0xa0&&p.scalars[1]==0xd4e&&
                (metadata&(half_nukta_role|sign_role))==(half_nukta_role|sign_role)&&glyphs[i].glyph.glyph_class==SfntGlyphClass::mark)target=i+1;
            if(p.profile==SfntIndicProfile::malayalam&&first_main&&!(metadata&(control_role|join_role))&&
                (metadata&(half_nukta_role|sign_role))!=(half_nukta_role|sign_role)&&glyphs[i].glyph.glyph_class!=SfntGlyphClass::mark){
                if(!(metadata&half_nukta_role)||(metadata&post_role))target=i+1;first_main=false;}
            if((metadata&halant_role)&&glyphs[i].glyph.glyph==p.halant){
                if((p.profile==SfntIndicProfile::gujarati||p.profile==SfntIndicProfile::gurmukhi)&&i){status=role(*forest,glyphs[i-1],&metadata,work);if(status!=SfntStatus::ok)return status;if(metadata&half_nukta_role)continue;}
                if(p.profile!=SfntIndicProfile::devanagari&&p.profile!=SfntIndicProfile::malayalam&&i+1<base){status=role(*forest,glyphs[i+1],&metadata,work);if(status!=SfntStatus::ok)return status;if(metadata&join_role)continue;}
                target=i+1;
                if(target<base){status=role(*forest,glyphs[target],&metadata,work);if(status!=SfntStatus::ok)return status;if(metadata&control_role)++target;}}}
        if(p.profile==SfntIndicProfile::malayalam){u32 consonant=nil;status=locate(*forest,glyphs,p.pre,&consonant,work);if(status!=SfntStatus::ok)return status;
            if(consonant!=nil&&(consonant<target||direct_control))target=consonant;}
        if(target>pre){status=move_group_before(glyphs,pre,attached,target,scratch.replacements,work);if(status!=SfntStatus::ok)return status;}}
    status=locate(*forest,glyphs,p.reph,&reph,work);if(status!=SfntStatus::ok)return status;
    if(reph==nil||glyphs[reph].component==nil||(!p.initial.nominal_reph&&forest->nodes[glyphs[reph].component].count<2))return SfntStatus::ok;
    status=locate(*forest,glyphs,p.base,&base,work);if(status!=SfntStatus::ok)return status;
    u32 target=count;
    for(u32 i=reph+1;i<count;++i){u8 metadata=0;status=role(*forest,glyphs[i],&metadata,work);if(status!=SfntStatus::ok)return status;
        if(!p.initial.nominal_reph&&base!=nil&&i<base&&(metadata&halant_role)&&glyphs[i].glyph.glyph==p.halant){target=i+1;
            if(target<count){status=role(*forest,glyphs[target],&metadata,work);if(status!=SfntStatus::ok)return status;if(metadata&control_role)++target;}break;}
        if((metadata&(post_role|sign_role))&&i>base){target=i;break;}}
    if(p.profile==SfntIndicProfile::malayalam&&base!=nil){target=base+1;u32 after=target;
        while(after<count){u8 metadata=0;status=role(*forest,glyphs[after],&metadata,work);if(status!=SfntStatus::ok)return status;
            const bool halant_accent=(metadata&(sign_role|post_role|half_nukta_role))==(sign_role|post_role);
            if(!(metadata&(control_role|join_role))&&!halant_accent&&!((metadata&halant_role)&&glyphs[after].glyph.glyph==p.halant)){
                // A post-base group's leading controls belong with that group.
                // They must not pull dot reph past the main-group boundary.
                if((metadata&(post_role|half_nukta_role))!=(post_role|half_nukta_role))target=after;break;
            }
            ++after;if(after==count)target=after;
        }}
    if(p.profile==SfntIndicProfile::kannada&&p.initial.nominal_reph&&base!=nil){const u32 end=p.initial.reph_end;
        if((end>3&&p.scalars[end-1]==0xccd)||(end<p.scalars.count&&p.scalars[end]==0xccd))target=base;}
    if(p.profile==SfntIndicProfile::odia&&target&&target!=reph+1)glyphs[reph].attachment=glyphs[target-1].component;
    return move_before(glyphs,reph,target,work);
}
SfntStatus finish(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 available) noexcept {
    auto& p=*static_cast<Policy*>(opaque);if(!p.forest)return SfntStatus::invalid_input;if(zero.count<glyphs.count)return SfntStatus::output_too_small;
    Work work{available};if(!work.spend(glyphs.count))return SfntStatus::resource_limit;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];u8 metadata=0;const auto status=role(*p.forest,g,&metadata,work);if(status!=SfntStatus::ok)return status;
        const bool control=(metadata&control_role)&&g.component!=nil&&p.forest->nodes[g.component].count==1;
        if(control)g.glyph.glyph=p.space;g.glyph.glyph_class=sfnt_gdef_glyph_class(*p.gdef,g.glyph.glyph);
        // Eligibility is dead after GSUB; the same byte then carries control
        // identity for run tracking once component scratch is reused.
        if(p.profile!=SfntIndicProfile::devanagari)g.glyph.reserved=static_cast<u8>(control);
        if(p.profile==SfntIndicProfile::odia&&(metadata&half_nukta_role)&&g.glyph.glyph_class==SfntGlyphClass::mark){
            g.glyph.reserved|=sfnt_mark_source_target;}
        // Gurmukhi/Odia/Telugu/Kannada preserve the font's mark advances, including nonzero ones.
        // Control slots still have zero advance independently of GDEF class.
        zero[i]=static_cast<u8>(control||(p.profile!=SfntIndicProfile::gurmukhi&&p.profile!=SfntIndicProfile::odia&&p.profile!=SfntIndicProfile::telugu&&p.profile!=SfntIndicProfile::kannada&&p.profile!=SfntIndicProfile::malayalam&&g.glyph.glyph_class==SfntGlyphClass::mark&&!(metadata&spacing_role)));}
    return SfntStatus::ok;
}
SfntStatus restore_cluster(void* opaque,SfntClusterMap* clusters,const SfntComponentForest&,Slice<SfntComponentGlyph>,u64 work) noexcept {
    const auto& p=*static_cast<Policy*>(opaque);SfntClusterMerge merge{};u64 used=0;
    const auto status=sfnt_cluster_map_plan(*clusters,0,p.units-1,&merge,work,&used);if(status!=SfntStatus::ok)return status;
    if(merge.commit_work>work-used)return SfntStatus::resource_limit;sfnt_cluster_map_commit(clusters,merge);return SfntStatus::ok;
}
}
SfntStatus indic_shape_cluster(SfntIndicProfile profile,const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntClusterGlyph> input,
    ConstSlice<u32> scalars,ConstSlice<u8> forms,u32 units,float em,SfntShapeStorage storage,SfntShapeRunResult* result,
    ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,bool followed_by_text,float trailing_spacing,bool rtl,bool word_initial,bool defer_output) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);u32 n=input.count;
    if(!sfnt_trailing_spacing_valid(trailing_spacing))return SfntStatus::invalid_input;
    if(n!=scalars.count||(n&&!input.data)||(!n!=!units))return SfntStatus::invalid_input;
    // Bengali split pieces can outnumber original UTF-16 units. Target scratch
    // is idle until metrics/export, after the stage policy consumes this order.
    const auto permutation=profile==SfntIndicProfile::devanagari?storage.character_glyphs:storage.targets;
    Policy policy{&plan.gdef,scalars,{permutation.data,n},forms,{},nullptr,nil,nil,nil,units};
    policy.profile=profile;policy.word_initial=word_initial;
    auto status=indic_initial_order(profile,scalars,forms,permutation,storage.flags,&policy.initial,limits.stage_work);
    if(status!=SfntStatus::ok)return status;if(!n)return SfntStatus::ok;
    SfntClusterGlyph augmented[indic_cluster_capacity];u32 augmented_scalars[indic_cluster_capacity];u8 augmented_forms[indic_cluster_capacity];
    if(policy.initial.needs_dotted_circle&&!((profile==SfntIndicProfile::odia||profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam)&&n>=31)&&!(n==1&&scalars[0]==0x200d&&followed_by_text)){
        if(n>=(profile==SfntIndicProfile::devanagari?31u:indic_cluster_capacity))return SfntStatus::unsupported_sequence;u16 circle=0;status=sfnt_cmap_glyph(cmap,0x25cc,0,&circle);if(status!=SfntStatus::ok)return status;
        augmented[0]={{circle,sfnt_gdef_glyph_class(plan.gdef,circle),0},input[0].source};augmented_scalars[0]=0x25cc;augmented_forms[0]=static_cast<u8>(forms[0]&indic_default_model);
        for(u32 i=0;i<n;++i){augmented[i+1]=input[i];augmented_scalars[i+1]=scalars[i];augmented_forms[i+1]=forms[i];}++n;
        input={augmented,n};scalars={augmented_scalars,n};forms={augmented_forms,n};policy.scalars=scalars;policy.forms=forms;policy.order={permutation.data,n};
        status=indic_initial_order(profile,scalars,forms,permutation,storage.flags,&policy.initial,limits.stage_work,true);if(status!=SfntStatus::ok)return status;
    }
    if(static_cast<u64>(n)*sizeof(SfntClusterGlyph)>storage.substitutions.replacements.count)return SfntStatus::output_too_small;
    if(!storage.substitutions.replacements.data||reinterpret_cast<usize>(storage.substitutions.replacements.data)%alignof(SfntClusterGlyph))return SfntStatus::invalid_input;
    for(u32 i=0;i<n;++i)if(input[i].source>=units||(!i&&input[i].source)||(i&&input[i].source<input[i-1].source))return SfntStatus::invalid_input;
    status=sfnt_cmap_glyph(cmap,sfnt_indic_script(profile)->block_first+0x4d,0,&policy.halant);if(status!=SfntStatus::ok)return status;
    status=sfnt_cmap_glyph(cmap,0x20,0,&policy.space);if(status!=SfntStatus::ok)return status;
    auto* mapped=reinterpret_cast<SfntClusterGlyph*>(storage.substitutions.replacements.data);
    for(u32 i=0;i<n;++i){mapped[i]=input[i];mapped[i].glyph.reserved=storage.flags[i];
        if(policy.initial.tail_sign==i+1){mapped[i].glyph.glyph=profile==SfntIndicProfile::kannada?policy.halant:input[n-1].glyph.glyph;mapped[i].glyph.glyph_class=profile==SfntIndicProfile::kannada?sfnt_gdef_glyph_class(plan.gdef,mapped[i].glyph.glyph):input[n-1].glyph.glyph_class;}
        if(policy.initial.tail_control==i+1){mapped[i].glyph.glyph=policy.space;mapped[i].glyph.glyph_class=sfnt_gdef_glyph_class(plan.gdef,policy.space);}}
    const u32 boundary=0;const SfntShapeStagePolicy stages{&policy,prepare,static_cast<u16>(SfntDevanagariStage::count)};
    const SfntShapeGlyphPolicy glyph_policy{&policy,finish,restore_cluster};
    status=sfnt_shape_run(plan,{mapped,n},units,{&boundary,1},rtl,em,storage,result,limits,steps,policy.order,glyph_policy,nullptr,&stages,defer_output);
    if(defer_output)return status;
    if(status!=SfntStatus::ok||trailing_spacing==0)return status;
    // Indic tracking can advance a final matra/mark. Only surviving controls
    // suppress the trailing slot; glyph width or GDEF mark class cannot tell
    // those cases apart. Component roles remain live until cluster return.
    Work work{limits.stage_work};
    for(u32 at=0;at<result->glyph_count;++at){const u32 i=rtl?at:result->glyph_count-1-at;const auto& glyph=storage.glyphs[i];u8 metadata=0;
        status=role(*policy.forest,glyph,&metadata,work);if(status!=SfntStatus::ok)break;
        const bool control=(metadata&control_role)&&glyph.component!=nil&&policy.forest->nodes[glyph.component].count==1;
        if(control)continue;
        const u8 saved=storage.flags[i];storage.flags[i]&=static_cast<u8>(~sfnt_position_zero_advance);
        status=sfnt_trailing_spacing({storage.placements.data,result->glyph_count},{storage.flags.data,result->glyph_count},trailing_spacing,limits.stage_work,rtl);
        storage.flags[i]=saved;break;}
    if(status!=SfntStatus::ok){result->glyph_count=result->source_units=result->target_count=0;}return status;
}
SfntStatus sfnt_devanagari_shape_cluster(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntClusterGlyph> input,
    ConstSlice<u32> scalars,ConstSlice<u8> forms,u32 units,float em,SfntShapeStorage storage,SfntShapeRunResult* result,
    ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,bool followed_by_text,float trailing_spacing,bool rtl) noexcept {
    return indic_shape_cluster(SfntIndicProfile::devanagari,plan,cmap,input,scalars,forms,units,em,storage,result,steps,limits,followed_by_text,trailing_spacing,rtl,false);
}
}
