#include "maicord/text/tamil_shape_run.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/indic_input.hpp"
#include "indic_run_support.hpp"
#include "maicord/text/indic_plan.hpp"
#include "maicord/text/unicode.hpp"
namespace maicord {namespace {
constexpr u32 nil=0xffffffffu;
constexpr u8 halant_role=1,control_role=2,base_role=4,pre_role=8,matra_role=16,modifier_role=32,above_role=64;
using I=UnicodeIndicSyllabicCategory;
I category(u32 c) noexcept {return c>=0xb80&&c<0xc00?unicode_properties(c)->indic_syllabic_category():I::other;}
bool base(u32 c) noexcept {const auto type=category(c);return c==0x25cc||c==0xa0||type==I::consonant||type==I::vowel_independent;}
bool control(u32 c) noexcept {return c==0x200c||c==0x200d;}
u8 rank(u32 c) noexcept {
    if(category(c)!=I::vowel_dependent)return 0;
    if(c==0xbc1||c==0xbc2)return 3;
    switch(unicode_properties(c)->indic_positional_category()){
        case UnicodeIndicPositionalCategory::left:return 1;
        case UnicodeIndicPositionalCategory::top:return 2;
        case UnicodeIndicPositionalCategory::bottom:return 3;
        case UnicodeIndicPositionalCategory::right:case UnicodeIndicPositionalCategory::left_and_right:return 4;
        default:return 0;}
}
bool sign(u32 c) noexcept {return rank(c)||c==0xbcd||c==0xb82;}
u32 cluster_end(ConstSlice<u32> text,u32 first) noexcept {
    const u32 initial=text[first],limit=minimum(text.count,first+minimum<u32>(31,text.count-first));u32 at=first+1;
    if(initial==0xa0||initial==0x200d){
        if(initial==0xa0&&at<limit&&(text[at]==0x200d||text[at]==0xa0))++at;
        if(at<limit&&sign(text[at]))++at;return at;
    }
    if(!base(initial))return at;
    enum class Phase : u8 {base,halant,join,matra,tail_halant,modifier};
    Phase phase=Phase::base;u8 last_rank=0,matras=0;bool vowel=category(initial)==I::vowel_independent,joined=false,tail_halant=false;
    while(at<limit){const u32 c=text[at];const auto type=category(c);bool accept=false;
        if(phase==Phase::base){
            if(c==0xbcd){phase=Phase::halant;accept=true;}
            else if(control(c)&&!joined){if(c==0x200d||vowel){phase=Phase::join;joined=true;}accept=true;}
            else if(c==0xa0&&!joined){phase=Phase::join;joined=true;accept=true;}
        }else if(phase==Phase::halant){
            // Tamil's orthographic conjunct is KA + VIRAMA + SSA. Other
            // explicit-virama consonants start a new source group; treating a
            // whole chain like Devanagari can split this akhand at a chunk edge.
            if(!vowel&&c==0xbb7&&at>=first+2&&text[at-2]==0xb95){phase=Phase::base;joined=false;accept=true;}
        }else if(phase==Phase::join){
            if(base(c)&&c!=0xa0&&(type!=I::vowel_independent||!vowel)&&(text[at-1]==0x200d||text[at-1]==0xa0||vowel)&&at-first<10){phase=Phase::base;vowel=type==I::vowel_independent;joined=false;accept=true;}
            else if(c==0xbcd){phase=Phase::halant;accept=true;}
        }
        const u8 next=rank(c);const bool composite=c>=0xbca&&c<=0xbcc;
        if(!accept&&next&&(phase==Phase::base||phase==Phase::join||phase==Phase::matra||phase==Phase::tail_halant)&&
            (!composite||!(matras&8)||!(matras&7))&&(next>last_rank||(next==4&&last_rank==4))){
            last_rank=next;matras|=static_cast<u8>((1u<<(next-1))|(composite?1u:0u));phase=Phase::matra;accept=true;}
        if(!accept&&phase==Phase::matra&&c==0xbcd&&!tail_halant){tail_halant=true;phase=Phase::tail_halant;accept=true;}
        if(!accept&&c==0xb82&&phase!=Phase::modifier&&phase!=Phase::halant&&!(phase==Phase::join&&vowel)){phase=Phase::modifier;accept=true;}
        if(!accept&&c==0x200c&&phase==Phase::modifier){++at;break;}
        if(!accept)break;++at;
    }return at;
}
struct Policy {const SfntGdef* gdef;u32* groups;u32 units;u16 halant,space;SfntComponentForest* forest=nullptr;};
SfntStatus prepare(void* opaque,u16 stage,SfntGlyphBuffer* buffer,SfntClusterMap*,SfntComponentForest* forest,
    const SfntGsubExecutionScratch&,u64 available,u64* used) noexcept {
    auto& p=*static_cast<Policy*>(opaque);use_shape::Metered work{available,used};
    if(stage!=0&&stage!=static_cast<u16>(SfntIndicStage::final_reorder))return SfntStatus::ok;
    const u32 count=sfnt_glyph_count(sfnt_glyph_buffer_view(*buffer));
    if(!work.spend(static_cast<u64>(count)*2))return SfntStatus::resource_limit;
    auto status=sfnt_glyph_buffer_seek(buffer,count,static_cast<u64>(count)*2);if(status!=SfntStatus::ok)return status;
    Slice<SfntComponentGlyph> glyphs{reinterpret_cast<SfntComponentGlyph*>(buffer->storage.data),count};
    if(!stage){p.forest=forest;if(!work.spend(count))return SfntStatus::resource_limit;
        for(u32 i=0;i<count;++i){const u32 node=glyphs[i].component;if(node>=forest->nodes.count)return SfntStatus::invalid_input;
            forest->nodes[node].reserved=static_cast<u16>(glyphs[i].glyph.reserved<<8);glyphs[i].glyph.reserved=0;}
        return SfntStatus::ok;
    }
    for(u32 first=0;first<count;){if(glyphs[first].source>=p.units)return SfntStatus::invalid_input;
        const u32 group=p.groups[glyphs[first].source];u32 end=first,base_at=nil,pre_at=nil,pre_count=0;
        while(end<count){if(!work.spend(1))return SfntStatus::resource_limit;
            if(glyphs[end].source>=p.units)return SfntStatus::invalid_input;if(p.groups[glyphs[end].source]!=group)break;
            u8 value=0;status=use_shape::first_role(*forest,glyphs[end],&value,work);if(status!=SfntStatus::ok)return status;
            if(value&base_role)base_at=end;if(value&pre_role){if(pre_at==nil)pre_at=end;if(end==pre_at+pre_count)++pre_count;}++end;}
        if(base_at!=nil&&pre_at!=nil&&pre_at<base_at){u32 before=pre_at;
            for(u32 i=pre_at+pre_count;i<base_at;++i){u8 value=0;status=use_shape::first_role(*forest,glyphs[i],&value,work);if(status!=SfntStatus::ok)return status;
                if((value&halant_role)&&glyphs[i].glyph.glyph==p.halant){before=i+1;
                    if(before<base_at){status=use_shape::first_role(*forest,glyphs[before],&value,work);if(status!=SfntStatus::ok)return status;if(value&control_role)++before;}}}
            if(before>pre_at+pre_count){if(!work.spend(static_cast<u64>(before-pre_at)*3))return SfntStatus::resource_limit;
                use_shape::rotate(glyphs,pre_at,pre_at+pre_count,before);}}
        first=end;
    }return SfntStatus::ok;
}
SfntStatus finish(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 available) noexcept {
    auto& p=*static_cast<Policy*>(opaque);if(!p.forest)return SfntStatus::invalid_input;
    if(zero.count<glyphs.count)return SfntStatus::output_too_small;
    use_shape::Work work{available};if(!work.spend(glyphs.count))return SfntStatus::resource_limit;
    for(u32 i=0;i<glyphs.count;++i){auto& glyph=glyphs[i];bool invisible=false;
        const auto status=use_shape::control_glyph(*p.forest,glyph,control_role,&invisible,work);if(status!=SfntStatus::ok)return status;
        if(invisible)glyph.glyph.glyph=p.space;glyph.glyph.glyph_class=sfnt_gdef_glyph_class(*p.gdef,glyph.glyph.glyph);
        zero[i]=static_cast<u8>(invisible||glyph.glyph.glyph_class==SfntGlyphClass::mark);
    }return SfntStatus::ok;
}
}
SfntStatus sfnt_tamil_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> scalars,float em,
    const SfntShapeStorage& storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,const SfntShapeLimits& limits,float trailing_spacing,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(!sfnt_trailing_spacing_valid(trailing_spacing))return SfntStatus::invalid_input;
    if((scalars.count&&!scalars.data)||(steps.count&&!steps.data))return SfntStatus::invalid_input;
    if(static_cast<u64>(scalars.count)*32+steps.count>limits.stage_work)return SfntStatus::resource_limit;
    for(u32 i=0;i<steps.count;++i)if(steps[i].feature_mask)return SfntStatus::unsupported_sequence;
    u64 units=0;for(u32 i=0;i<scalars.count;++i){if(!unicode_is_scalar(scalars[i]))return SfntStatus::invalid_input;units+=scalars[i]>0xffff?2u:1u;}
    if(units>0xffffffffu)return SfntStatus::resource_limit;
    if(units>storage.character_glyphs.count||units>static_cast<u64>(storage.advances.count)*2)return SfntStatus::output_too_small;
    if(scalars.count&&(!storage.positions.data||!storage.advances.data||!storage.targets.data))return SfntStatus::invalid_input;
    u16 halant=0,space=0;auto status=sfnt_cmap_glyph(cmap,0xbcd,0,&halant);if(status!=SfntStatus::ok)return status;
    status=sfnt_cmap_glyph(cmap,0x20,0,&space);if(status!=SfntStatus::ok)return status;
    auto* mapped=reinterpret_cast<SfntClusterGlyph*>(storage.positions.data);auto* groups=reinterpret_cast<u32*>(storage.advances.data);
    const u64 capacity=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntClusterGlyph);
    u32 count=0,starts=0,source=0;
    const auto boundary=[&](u32 value) noexcept {if(starts&&storage.targets[starts-1]==value)return true;
        if(starts>=storage.targets.count)return false;storage.targets[starts++]=value;return true;};
    const auto append=[&](u32 scalar,u32 at) noexcept {if(count>=capacity)return SfntStatus::output_too_small;u16 glyph=0;
        const auto status=sfnt_cmap_glyph(cmap,rtl?unicode_bidi_mirror(scalar):scalar,0,&glyph);if(status!=SfntStatus::ok)return status;
        const u8 value=scalar==0xbcd?halant_role:control(scalar)?control_role:base(scalar)?base_role:scalar>=0xbc6&&scalar<=0xbc8?pre_role|matra_role:rank(scalar)==2?matra_role|above_role:rank(scalar)?matra_role:scalar==0xb82?modifier_role:0;
        mapped[count++]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),value},at};return SfntStatus::ok;};
    for(u32 first=0;first<scalars.count;){const u32 end=cluster_end(scalars,first),begin=count,origin=source;const u32 initial=scalars[first];
        if(!boundary(source))return SfntStatus::output_too_small;
        const bool circle=sign(initial)||(initial==0x200d&&(end>first+1||end==scalars.count));
        if(circle){status=append(0x25cc,source);if(status!=SfntStatus::ok)return status;}
        SfntIndicInputScalar expanded[63];SfntIndicInputResult sizes{};
        status=sfnt_indic_expand_cluster(SfntIndicProfile::tamil,{scalars.data+first,end-first},{expanded,62},&sizes,limits.stage_work);if(status!=SfntStatus::ok)return status;
        for(u32 i=0;i<sizes.scalar_count;++i){const auto entry=expanded[i];const u32 at=source+entry.source;
            // A consonant following an explicit virama retains its own source
            // group unless actual font substitution consumes it into a ligature.
            if(base(entry.scalar)&&i&&expanded[i-1].scalar==0xbcd&&!boundary(at))return SfntStatus::output_too_small;
            status=append(entry.scalar,at);if(status!=SfntStatus::ok)return status;}
        bool matra=false,above=false;
        for(u32 i=begin;i<count;++i){const u8 value=mapped[i].glyph.reserved;matra|=(value&matra_role)!=0;above|=(value&above_role)!=0;
            if(matra&&!above&&(value&halant_role)){u32 end_at=i+1;while(end_at<count&&(mapped[end_at].glyph.reserved&matra_role)&&!(mapped[end_at].glyph.reserved&above_role))++end_at;
                const auto saved=mapped[i];for(u32 at=i;at+1<end_at;++at)mapped[at]=mapped[at+1];mapped[end_at-1]=saved;i=end_at-1;}}
        // Indic terminal-control slot: the glyph immediately after the first matra
        // becomes a control and follows the remaining matras.
        if(matra&&count>begin+1&&scalars[end-1]==0x200c&&(mapped[count-2].glyph.reserved&modifier_role)){
            u32 slot=begin;while(slot<count&&!(mapped[slot].glyph.reserved&matra_role))++slot;++slot;
            if(slot<count){mapped[slot].glyph={space,sfnt_gdef_glyph_class(plan.gdef,space),control_role};u32 after=slot+1;
                while(after<count&&(mapped[after].glyph.reserved&matra_role))++after;
                const auto saved=mapped[slot];for(u32 i=slot;i+1<after;++i)mapped[i]=mapped[i+1];mapped[after-1]=saved;}}
        for(u32 i=begin+1;i<count;++i)if((mapped[i].glyph.reserved&control_role)&&(mapped[i-1].glyph.reserved&modifier_role)){
            const auto saved=mapped[i];mapped[i]=mapped[i-1];mapped[i-1]=saved;}
        // Expansion storage is dead after mapping; its bytes hold a stable linear
        // pre-vowel partition.
        static_assert(sizeof(SfntIndicInputScalar)==sizeof(SfntClusterGlyph));u32 reordered=0;
        for(u32 pass=0;pass<2;++pass)for(u32 i=begin;i<count;++i)if(((mapped[i].glyph.reserved&pre_role)!=0)==(pass==0))
            copy_bytes(expanded+reordered++,&mapped[i],sizeof(SfntClusterGlyph));
        copy_bytes(mapped+begin,expanded,static_cast<usize>(count-begin)*sizeof(SfntClusterGlyph));
        for(u32 i=0;i<sizes.source_units;++i)groups[source+i]=origin;
        source+=sizes.source_units;first=end;
    }
    Policy policy{&plan.gdef,groups,static_cast<u32>(units),halant,space};
    const SfntShapeStagePolicy stages{&policy,prepare,static_cast<u16>(SfntIndicStage::count),true};
    const SfntShapeGlyphPolicy glyph_policy{&policy,finish,nullptr};
    status=sfnt_shape_run(plan,{mapped,count},static_cast<u32>(units),{storage.targets.data,starts},rtl,em,storage,result,limits,steps,{},glyph_policy,nullptr,&stages);
    if(status!=SfntStatus::ok||!trailing_spacing)return status;
    // A terminal matra may carry Indic tracking even if it has zero advance; only
    // a surviving control suppresses that slot. Decided from the live component
    // metadata and final cluster flags.
    use_shape::Work work{limits.stage_work};
    status=indic_trailing_spacing(storage,result->glyph_count,trailing_spacing,rtl,work,[&](u32 index,use_shape::Work& budget,bool* skipped) noexcept {
        return use_shape::control_glyph(*policy.forest,storage.glyphs[index],control_role,skipped,budget);});
    if(status!=SfntStatus::ok)result->glyph_count=result->source_units=result->target_count=0;return status;
}
}
