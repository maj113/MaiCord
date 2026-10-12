#include "maicord/text/devanagari_order.hpp"
#include "maicord/text/devanagari_clusters.hpp"
#include "devanagari_character.hpp"
#include "indic_cluster.hpp"
#include "indic_forms.hpp"
namespace maicord {namespace {
using C=DevanagariCategory;
bool base_character(C category) noexcept {return category==C::consonant||category==C::vowel||category==C::placeholder;}
void move_order(Slice<u32> order,u32 from,u32 to) noexcept {
    const u32 saved=order[from];if(from<to)for(u32 i=from;i<to;++i)order[i]=order[i+1];
    else for(u32 i=from;i>to;--i)order[i]=order[i-1];order[to]=saved;
}
u32 consonant_prefix(ConstSlice<u32> order,const C* categories,u32 at) noexcept {
    u32 first=at;while(first){const auto c=categories[order[first-1]];
        if(c!=C::nukta&&c!=C::joiner&&c!=C::nonjoiner)break;--first;}
    return first&&categories[order[first-1]]==C::halant?first-1:at;
}
bool overlaps(const void* a,u64 an,const void* b,u64 bn) noexcept {
    const u64 x=reinterpret_cast<usize>(a),y=reinterpret_cast<usize>(b);
    return x>~u64{0}-an||y>~u64{0}-bn||(an&&bn&&x<y+bn&&y<x+an);
}
}
SfntStatus indic_initial_order(SfntIndicProfile profile,ConstSlice<u32> scalars,ConstSlice<u8> forms,
    Slice<u32> order,Slice<u8> masks,SfntDevanagariOrderResult* result,u64 work,bool inserted_circle) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};const u32 n=scalars.count,nil=0xffffffffu;
    if(forms.count!=n||(n&&(!scalars.data||!forms.data))||(order.count&&!order.data)||(masks.count&&!masks.data))return SfntStatus::invalid_input;
    const bool bengali=profile==SfntIndicProfile::bengali,gurmukhi=profile==SfntIndicProfile::gurmukhi,odia=profile==SfntIndicProfile::odia,kannada=profile==SfntIndicProfile::kannada,malayalam=profile==SfntIndicProfile::malayalam,southern=profile==SfntIndicProfile::telugu||kannada||malayalam;
    const auto* info=sfnt_indic_script(profile);const u32 block=info?info->block_first:0;
    if(!bengali&&!odia&&!southern&&profile!=SfntIndicProfile::devanagari&&profile!=SfntIndicProfile::gujarati&&profile!=SfntIndicProfile::gurmukhi)return SfntStatus::invalid_input;
    if(n>(profile==SfntIndicProfile::devanagari?31u:indic_cluster_capacity))return SfntStatus::unsupported_sequence;
    if(order.count<n||masks.count<n)return SfntStatus::output_too_small;
    if(static_cast<u64>(n)*64>work)return SfntStatus::resource_limit;
    if(overlaps(scalars.data,static_cast<u64>(n)*4,order.data,static_cast<u64>(n)*4)||overlaps(scalars.data,static_cast<u64>(n)*4,masks.data,n)||
       overlaps(forms.data,n,order.data,static_cast<u64>(n)*4)||overlaps(forms.data,n,masks.data,n)||overlaps(order.data,static_cast<u64>(n)*4,masks.data,n))return SfntStatus::invalid_input;
    u8 boundaries[indic_cluster_capacity]{};if(profile==SfntIndicProfile::devanagari){const auto status=sfnt_devanagari_cluster_boundaries(scalars,{boundaries,indic_cluster_capacity},work);if(status!=SfntStatus::ok)return status;}
    C categories[indic_cluster_capacity]{};u8 combining[indic_cluster_capacity]{};bool nukta=false,any_matra=false;
    for(u32 i=0;i<n;++i){if(!unicode_is_scalar(scalars[i]))return SfntStatus::invalid_input;
        if(i&&boundaries[i])return SfntStatus::unsupported_sequence;categories[i]=indic_category(profile,scalars[i]);combining[i]=unicode_properties(scalars[i])->combining_class();nukta|=categories[i]==C::nukta;
        if(odia&&scalars[i]==0xb55)categories[i]=C::vedic;
        if(southern&&(scalars[i]==block+0x55||scalars[i]==block+0x56||scalars[i]<block||scalars[i]>=block+128))combining[i]=0;
        // Gujarati nukta signs use shaping order even where Unicode CCC is zero.
        if(profile==SfntIndicProfile::gujarati&&categories[i]==C::nukta)combining[i]=7;
        if(gurmukhi&&i&&categories[i-1]==C::medial&&categories[i]==C::halant)combining[i]=0;
        any_matra|=categories[i]==C::pre||categories[i]==C::above||categories[i]==C::below||categories[i]==C::post||(odia&&scalars[i]==0xb55);}
    SfntDevanagariOrderResult value{nil,nil,0,false};
    const bool ra=n&&(bengali?(scalars[0]==0x9b0||scalars[0]==0x9f0):scalars[0]==block+0x30u);
    if(n>=3&&ra&&categories[1]==C::halant&&(forms[0]&sfnt_deva_reph)&&categories[2]!=C::joiner&&categories[2]!=C::nonjoiner&&categories[2]!=C::nukta&&(profile==SfntIndicProfile::devanagari||scalars[2]!=0xa0)){
        for(u32 i=2;i<n;++i)if(base_character(categories[i])){value.reph_end=2;break;}}
    if(!bengali&&n>=4&&ra&&(forms[0]&sfnt_deva_reph)&&categories[1]==C::nonjoiner){u32 end=2;
        while(end<n&&categories[end]==C::nonjoiner)++end;
        if(end<n&&(!kannada||end+1>=n||(categories[end+1]!=C::nukta&&categories[end+1]!=C::joiner&&scalars[end+1]!=0xa0))&&(categories[end]==C::halant||categories[end]==C::joiner||(kannada&&scalars[end]==0xa0)))for(u32 i=end+1;i<n;++i)if(base_character(categories[i])){value.reph_end=end+1;value.nominal_reph=true;break;}}
    if(malayalam&&n>=2&&scalars[0]==0xd4e){value.reph_end=1;value.nominal_reph=true;}
    u32 first_base=nil;
    for(u32 i=value.reph_end;i<n;++i)if(base_character(categories[i])){first_base=i;break;}
    u32 pre_consonant=nil;
    if(southern&&first_base!=nil)for(u32 i=first_base+1;i<n;++i)if((forms[i]&sfnt_deva_pre)&&(!malayalam||!(forms[i]&indic_default_position))){u32 before=i;while(before&&categories[before-1]==C::nukta)--before;
        if(before&&categories[before-1]==C::halant){pre_consonant=i;break;}}
    bool vowel_joiner_base=false,kannada_vowel_chain=false;
    if(first_base!=nil&&categories[first_base]==C::vowel){u32 at=first_base+1;while(at<n&&categories[at]==C::nukta)++at;
        if(at<n&&categories[at]==C::halant){kannada_vowel_chain=kannada;++at;while(at<n&&categories[at]==C::nukta)++at;vowel_joiner_base=at<n&&categories[at]==C::joiner;}}
    bool below_seen=false,explicit_half=false;
    if((vowel_joiner_base&&!southern)||(malayalam&&first_base!=nil&&categories[first_base]==C::vowel&&first_base+1<n&&(categories[first_base+1]==C::joiner||scalars[first_base+1]==0xa0)&&first_base+2<n&&categories[first_base+2]!=C::halant))value.base=first_base;
    else if(first_base!=nil)for(u32 i=n;i-->first_base;){if(!base_character(categories[i])||(profile!=SfntIndicProfile::devanagari&&scalars[i]==0xa0&&i!=first_base))continue;value.base=i;
        // Cmap swaps and disabled-lookup controls preserve this scalar rule:
        // after RRA/LLA/LLLA, a default-position consonant starts a main group.
        if(malayalam&&(forms[i]&indic_default_position)&&i>=2&&categories[i-1]==C::halant&&
            (scalars[i-2]==0xd31||scalars[i-2]==0xd33||scalars[i-2]==0xd34))break;
        if(kannada&&i+2<n&&categories[i+1]==C::halant&&(categories[i+2]==C::joiner||scalars[i+2]==0xa0)){explicit_half=true;break;}
        if(southern){u32 before=i;while(before&&categories[before-1]==C::nukta)--before;
            if(before&&((kannada&&(categories[before-1]==C::joiner||scalars[before-1]==0xa0))||
                (malayalam&&scalars[before-1]==0xa0))){u32 halant=before-1;
                while(halant&&categories[halant-1]==C::nukta)--halant;
                if(halant&&categories[halant-1]==C::halant)break;}
            if(before&&(categories[before-1]==C::nonjoiner||(scalars[before-1]==0xa0&&((categories[first_base]==C::vowel&&!kannada_vowel_chain)||(kannada&&before>=2&&categories[before-2]==C::halant)))||(categories[before-1]==C::joiner&&((categories[first_base]==C::vowel&&!kannada_vowel_chain)||(before>=2&&categories[before-2]==C::halant)))))break;}
        if((!southern||!(forms[i]&sfnt_deva_pre))&&profile!=SfntIndicProfile::devanagari&&i+1<n&&categories[i+1]==C::nukta)break;
        if((bengali||gurmukhi||odia||southern)&&below_seen&&(forms[i]&(sfnt_deva_post|(southern?sfnt_deva_pre:0)))&&!(forms[i]&sfnt_deva_below))break;
        below_seen|=(forms[i]&sfnt_deva_below)!=0;
        if(southern&&(forms[i]&sfnt_deva_pre)&&i!=pre_consonant&&!(forms[i]&(sfnt_deva_below|sfnt_deva_post)))break;
        if(i==first_base||(i&&(categories[i-1]==C::joiner||(southern&&categories[i-1]==C::nonjoiner))&&(!odia||i<2||categories[i-2]==C::halant)&&(!southern||(categories[first_base]==C::vowel&&!kannada_vowel_chain)||i<2||categories[i-2]==C::halant))||!(forms[i]&(sfnt_deva_below|sfnt_deva_post|sfnt_deva_pre)))break;}
    if(kannada&&!explicit_half&&ra&&n>=3&&(forms[0]&sfnt_deva_reph)&&categories[1]==C::halant&&(categories[2]==C::joiner||scalars[2]==0xa0)&&
        !(value.base!=nil&&value.base+1<n&&categories[value.base+1]==C::nukta))value.base=first_base;
    if(kannada&&ra&&(forms[0]&sfnt_deva_reph)&&n>4&&categories[1]==C::nonjoiner&&categories[2]==C::halant&&
        (categories[3]==C::joiner||scalars[3]==0xa0))value.base=first_base;
    if(n)value.needs_dotted_circle=(malayalam&&scalars[0]==0xd4e&&value.base==nil)||categories[0]==C::joiner||(value.base==nil&&categories[0]>=C::nukta&&categories[0]!=C::nonjoiner&&categories[0]!=C::medial);
    // A dot reph after only joiners/NBSP has no main consonant. The
    // observed broken cluster keeps that prefix, then supplies its base.
    // A real leading dotted circle is a different, already valid cluster.
    u32 broken_reph=inserted_circle?1u:0u;
    if(malayalam){while(broken_reph<n&&(categories[broken_reph]==C::joiner||scalars[broken_reph]==0xa0))++broken_reph;
        if(broken_reph>(inserted_circle?1u:0u)&&broken_reph<n&&scalars[broken_reph]==0xd4e){
            if(broken_reph+1==n)value.needs_dotted_circle=!inserted_circle;}
        else broken_reph=nil;}
    else broken_reph=nil;
    // Default below forms do not displace a full main group in this broken
    // prefix. Real post-base classification retains the placeholder as base.
    if(malayalam&&broken_reph!=nil&&broken_reph+1<n&&value.base==0&&
        (inserted_circle||scalars[0]==0xa0)&&(forms[broken_reph+1]&indic_default_position)&&
        (forms[broken_reph+1]&sfnt_deva_below)&&!(forms[broken_reph+1]&sfnt_deva_post)){
        bool left=false;for(u32 i=broken_reph+2;i<n;++i)left|=categories[i]==C::pre;
        if(!left)value.base=broken_reph+1;
    }
    const u8 post=sfnt_deva_below|sfnt_deva_pre|sfnt_deva_post|sfnt_deva_vatu;
    for(u32 i=0;i<n;++i){order[i]=i;u8 mask=value.base==nil?0:sfnt_deva_conjunct;
        if((bengali||gurmukhi||odia||southern)&&value.base!=nil){if(!kannada||!explicit_half||i>value.base)mask|=sfnt_deva_below;if(i>=value.base)mask|=sfnt_deva_vatu;}
        if(nukta)mask|=sfnt_deva_nukta;if(i<value.reph_end)mask|=sfnt_deva_reph;
        if(value.base!=nil){if(i>=value.reph_end&&i<value.base)mask|=sfnt_deva_half;if(i>value.base)mask|=post;}
        // Gurmukhi pstf also contains vowel-tail substitutions. Only the
        // classified post-base consonant group participates in this stage.
        if(gurmukhi&&(mask&sfnt_deva_post)){u32 at=i;
            if(categories[at]==C::halant){++at;while(at<n&&categories[at]==C::nukta)++at;}
            if(at>=n||!(forms[at]&sfnt_deva_post))mask&=static_cast<u8>(~sfnt_deva_post);}
        if(southern){mask&=static_cast<u8>(~sfnt_deva_pre);
            if(kannada&&value.nominal_reph&&value.reph_end<n&&categories[value.reph_end]==C::halant&&i<=value.base)mask&=static_cast<u8>(~sfnt_deva_below);
            if(pre_consonant!=nil&&pre_consonant!=value.base){u32 first=pre_consonant;while(first&&categories[first-1]==C::nukta)--first;if(first)--first;
                if(i>=first&&i<=pre_consonant)mask|=sfnt_deva_pre;}}
        masks[i]=mask;
        if(categories[i]==C::pre&&(!(bengali||odia||malayalam)||value.pre_matra==nil))value.pre_matra=i;
        // The fixed-size local permutation never changes the user's source
        // positions. Nuktas and Vedic marks retain their source/component identity.
        if(combining[i]){u32 at=i;while(at){const auto previous=combining[order[at-1]];
            if(!previous||previous<=combining[i])break;order[at]=order[at-1];--at;}order[at]=i;}
    }
    // The no-post-form Malayalam fallback uses consonant/halant order for
    // default post/below groups. Preserve original source identities while
    // moving only the permutation, before font substitutions can consume it.
    if(malayalam&&value.base!=nil)for(u32 at=1;at<n;++at){const u32 original=order[at];
        if(original>value.base&&(forms[original]&indic_default_position)&&categories[order[at-1]]==C::halant){u32 end=at;
            while(end+1<n&&(categories[order[end+1]]==C::joiner||categories[order[end+1]]==C::nonjoiner||scalars[order[end+1]]==0xa0))++end;
            move_order(order,at-1,end);at=end;}}
    // Kannada moves the base group's control before its halant. For a
    // direct consonant/control/consonant group, the observed preparation
    // replaces the preceding slot with the halant. Reuse the otherwise
    // unused Kannada tail-sign slot; original source ownership is retained.
    if(kannada&&value.base!=nil&&categories[value.base]!=C::vowel){u32 control=value.base+1;
        while(control+1<n&&(categories[control+1]==C::nukta||categories[control+1]==C::halant||
            categories[control+1]==C::nonjoiner||categories[control+1]==C::joiner||scalars[control+1]==0xa0)){
            if(categories[control]==C::halant){++control;break;}++control;}
        if(control+1<n&&(categories[control]==C::joiner||scalars[control]==0xa0)&&categories[control+1]==C::consonant&&(forms[control+1]&sfnt_deva_below)){
            const u32 halant=control-1;categories[control]=C::joiner;
            if(categories[halant]!=C::halant){value.tail_sign=static_cast<u8>(halant+1);categories[halant]=C::halant;masks[halant]|=sfnt_deva_below;}
            u32 from=0,to=0;for(u32 at=0;at<n;++at){if(order[at]==halant)from=at;if(order[at]==control)to=at;}
            move_order(order,from,to);}}
    // The Odia upper modifier also occupies a slot within a multi-mark tail.
    // Frozen Windows output advances that slot once per four tail scalars. Keep
    // the source identity of the replaced slot; only its shaping role changes.
    const auto bindu_slot=[&]() noexcept {u32 first=nil,start=0;
        if(!n||categories[n-1]!=C::bindu)return;
        for(u32 at=0;at<n;++at)if(categories[order[at]]==C::pre)start=at+1;
        for(u32 at=start;at<n;++at){const u32 original=order[at];const auto c=categories[original];
            if(first==nil&&(c==C::below||c==C::post||scalars[original]==0xb55))first=at;
            if(first!=nil&&c==C::bindu){const u32 slot=first+(at-first+3)/4,saved=order[slot];
                value.tail_sign=static_cast<u8>(saved+1);categories[saved]=C::bindu;
                u32 end=slot+1;while(end<at&&(categories[order[end]]==C::nukta||scalars[order[end]]==0xb55))++end;
                move_order(order,slot,end-1);move_order(order,at,first);break;}}};
    if(odia&&value.pre_matra==nil)bindu_slot();
    if(odia&&n&&categories[n-1]==C::visarga){bool earlier=false,seen=false;u32 first=nil;
        for(u32 at=0;at<n;++at){const auto c=categories[order[at]];
            if(at+1<n&&(c==C::bindu||c==C::visarga))earlier=true;
            // A later vowel ends the overline group attached to a left vowel.
            if(value.pre_matra!=nil&&(c==C::above||c==C::below||c==C::post))first=nil;
            if(c==C::pre||c==C::above||c==C::below||c==C::post)seen=true;
            if(first==nil&&scalars[order[at]]==0xb55&&seen)first=at;}
        if(!seen)for(u32 at=0;at<n;++at)if(scalars[order[at]]==0xb55){first=at;break;}
        if(!earlier&&first!=nil){const u32 slot=first+(n-1-first+3)/4,saved=order[slot];
            value.tail_sign=static_cast<u8>(saved+1);categories[saved]=C::visarga;
            u32 end=slot+1;while(end+1<n&&(categories[order[end]]==C::nukta||scalars[order[end]]==0xb55))++end;
            move_order(order,slot,end-1);move_order(order,n-1,first);}}
    // A leading Odia overline follows the vowel tail. An overline already
    // following a vowel stays attached to it, including a left-hand vowel.
    if(odia){u32 leading=0,end=0;bool seen=false,attached=false;
        for(u32 at=0;at<n;++at){const u32 original=order[at];const auto c=categories[original];
            if(c==C::bindu){end=at+1;break;}
            if(c==C::pre||c==C::above||c==C::below||c==C::post){seen=true;end=at+1;attached=false;}
            else if(scalars[original]==0xb55){attached=!seen;if(attached)leading|=1u<<original;else if(end==at)end=at+1;}
            else if(c==C::nukta){if(attached)leading|=1u<<original;else if(end==at)end=at+1;}
            else if(c==C::halant&&!seen&&at+1<n&&scalars[order[at+1]]==0xb55){leading|=1u<<original;attached=false;}
            else{attached=false;if(c==C::visarga)break;}}
        u32 target=0;
        for(u32 at=0;at<end;++at)if(!(leading&(1u<<order[at]))){const u32 saved=order[at];
            for(u32 from=at;from>target;--from)order[from]=order[from-1];order[target++]=saved;}
        for(u32 i=0;i<n;++i)if(scalars[i]==0xb55&&value.tail_sign!=i+1)
            categories[i]=value.pre_matra!=nil&&(leading&(1u<<i))?C::vedic:C::nukta;
    }
    if(odia&&value.pre_matra!=nil)bindu_slot();
    // Observed modern Windows tail-slot behavior: without a Vedic tail, a
    // terminal non-joiner after a modifier also occupies the slot immediately
    // following the first canonically ordered matra (a quarter-tail offset
    // for Odia). Preserve that original
    // source identity while moving the control slot after the remaining matras.
    if(any_matra&&n&&categories[n-1]==C::nonjoiner){bool sign=false,vedic=false;u32 first=nil;
        for(u32 i=0;i<n;++i){const auto c=categories[order[i]];if(first==nil&&(c==C::pre||c==C::above||c==C::below||c==C::post||(odia&&scalars[order[i]]==0xb55)))first=i;
            if(first!=nil){sign|=c==C::bindu||c==C::visarga;vedic|=c==C::vedic;}}
        if(sign&&!vedic&&first+1<n){const u32 at=first+((odia||southern)?(n-1-first+3)/4:1),saved=order[at];value.tail_control=static_cast<u8>(saved+1);categories[saved]=C::nonjoiner;
            u32 end=at+1;while(end<n){const auto c=categories[order[end]];if(c!=C::pre&&c!=C::above&&c!=C::below&&c!=C::post&&c!=C::nukta)break;++end;}
            if((bengali||gurmukhi)&&end<n&&categories[order[end]]==C::bindu)++end;
            const bool overline=odia&&scalars[order[first]]==0xb55;
            for(u32 i=at;i+1<end;++i)order[i]=order[i+1];order[end-1]=saved;
            if(overline)move_order(order,n-1,first);}}
    // A halant inside the matra tail follows all of that tail's vowel signs.
    bool matra_tail=false,below_tail=false;
    for(u32 i=0;i<n;++i){const auto c=categories[order[i]];matra_tail|=c==C::above||c==C::below||c==C::post||((bengali||gurmukhi||odia||malayalam)&&c==C::pre);
        if(odia&&c==C::halant)for(u32 before=0;before<order[i];++before)matra_tail|=scalars[before]==0xb55;
        below_tail|=c==C::below;
        if(bengali&&matra_tail&&c==C::halant&&i+1<n&&categories[order[i+1]]==C::below){u32 target=i+1;
            for(u32 at=target+1;at<n;++at){const auto next=categories[order[at]];if(next!=C::pre&&next!=C::post&&next!=C::below)break;
                if(next==C::post){const u32 saved=order[at];for(u32 from=at;from>target;--from)order[from]=order[from-1];order[target++]=saved;}}continue;}
        if((southern&&!malayalam)||!matra_tail||c!=C::halant||((bengali||odia)&&below_tail))continue;u32 attached=1;
        if(odia&&i+1<n&&scalars[order[i+1]]==0xb55)
            while(i+attached<n&&(scalars[order[i+attached]]==0xb55||categories[order[i+attached]]==C::nukta))++attached;
        u32 end=i+attached;
        while(end<n){const auto next=categories[order[end]];
            if(odia&&next==C::below)break;
            if(odia&&scalars[order[end]]==0xb55){++end;continue;}
            if(bengali?(next!=C::pre&&next!=C::post&&next!=C::nukta&&next!=C::bindu):(next!=C::pre&&next!=C::above&&next!=C::below&&next!=C::post&&next!=C::nukta&&(!(gurmukhi||odia)||next!=C::bindu)))break;++end;}
        for(u32 part=0;part<attached;++part){const u32 saved=order[i];for(u32 j=i;j+1<end;++j)order[j]=order[j+1];order[end-1]=saved;}i=end-1;}
    // Halant reordering can expose further vowels after the synthetic
    // tail-control slot. Finish that slot's placement in the resulting tail.
    if(malayalam&&value.tail_control)for(u32 at=0;at<n;++at)if(order[at]+1==value.tail_control){u32 end=at+1;
        while(end<n){const auto c=categories[order[end]];if(c!=C::pre&&c!=C::above&&c!=C::below&&c!=C::post&&c!=C::nukta)break;++end;}
        move_order(order,at,end-1);break;}
    // Non-joiners preceding the end of a modifier/Vedic tail appear before
    // that tail in the glyph stream, while retaining their logical source.
    for(u32 i=1;i<n;++i)if(categories[order[i]]==C::nonjoiner){const u32 saved=order[i];u32 at=i;
        while(at){const auto c=categories[order[at-1]];if(c!=C::bindu&&c!=C::visarga&&c!=C::vedic)break;
            if((bengali||gurmukhi||odia)&&(!any_matra||value.tail_control)&&c==C::bindu)break;
            if((profile==SfntIndicProfile::devanagari||(southern&&(scalars[order[at-1]]==0x953||scalars[order[at-1]]==0x954)))&&!any_matra&&c==C::bindu&&at>=2&&categories[order[at-2]]==C::halant&&
                !(order[at-2]==1&&scalars[0]==block+0x30u&&(forms[0]&sfnt_deva_reph)))break;order[at]=order[at-1];--at;}order[at]=saved;}
    if(southern)for(u32 at=1;at<n;++at)if(scalars[order[at]]==0x952){u32 target=at;
        while(target&&categories[order[target-1]]==C::vedic&&scalars[order[target-1]]!=0x952)--target;
        move_order(order,at,target);}
    if(malayalam)for(u32 at=1;at<n;++at)if(categories[order[at]]==C::below){u32 target=at;
        while(target&&(categories[order[target-1]]==C::above||categories[order[target-1]]==C::post))--target;
        move_order(order,at,target);}
    const auto after_subscript=[&](u32 scalar) noexcept {const u32 part=scalar-block;
        return part==0x43||part==0x44||(kannada&&(part==0x55||part==0x56));};
    // Telugu/Kannada base-attached vowels precede subjoined consonants. Keep the
    // consonant's halant/control prefix intact so its form remains executable.
    if(southern&&!malayalam&&value.base!=nil){u32 target=nil,movable=0,pending=0,parts=0;
        const auto before_subscript=[&](u32 scalar) noexcept {const u32 part=scalar-block;
            return (part>=0x3e&&part<=0x42)||(part>=0x46&&part<=0x4c)||(!kannada&&(part==0x55||part==0x56))||part==0x62||part==0x63;};
        // Frozen Windows output leaves a contiguous vowel group in source
        // order once it contains more than six before-subscript pieces. A
        // halant ends the group; Kannada after-subscript pieces do too.
        // Record eligible source slots in one bitset.
        for(u32 at=0;at<=n;++at){const auto c=at<n?categories[order[at]]:C::other;
            if(at<n&&(c==C::above||c==C::below||c==C::post)&&before_subscript(scalars[order[at]])){pending|=1u<<order[at];++parts;}
            if((c!=C::above&&c!=C::below&&c!=C::post&&c!=C::nukta)||(kannada&&at<n&&after_subscript(scalars[order[at]]))){if(parts<=6)movable|=pending;pending=parts=0;}}
        for(u32 at=0;at<n;++at){const u32 original=order[at],scalar=scalars[original];
            if(original<=value.base)continue;
            if(target==nil&&after_subscript(scalar))target=at;
            if(target==nil&&base_character(categories[original])&&(forms[original]&(sfnt_deva_below|sfnt_deva_post))){
                target=consonant_prefix({order.data,n},categories,at);
                if(kannada){u32 before=target;while(before&&categories[order[before-1]]==C::nukta)--before;
                    if(before&&(scalars[order[before-1]]==0x953||scalars[order[before-1]]==0x954))target=before-1;}
                if(target<at&&!inserted_circle&&categories[value.base]!=C::vowel&&target&&(categories[order[target-1]]==C::joiner||(kannada?scalars[order[target-1]]==0xa0:categories[order[target-1]]==C::nonjoiner)))--target;}
            if(target!=nil&&scalar==0xa0&&target&&(categories[order[target-1]]==C::joiner||categories[order[target-1]]==C::nonjoiner))--target;
            const bool vowel=(movable&(1u<<original))!=0;
            if(target==nil||!vowel)continue;u32 attached=1;
            while(at+attached<n&&categories[order[at+attached]]==C::nukta)++attached;
            for(u32 j=0;j<attached;++j)move_order(order,at+j,target+j);
            target+=attached;at+=attached-1;}}
    if(southern){bool seen=false,after=false;
        for(u32 at=0;at<n;++at){const u32 scalar=scalars[order[at]];const auto c=categories[order[at]];
            if(c==C::above||c==C::below||c==C::post){seen=true;after=after_subscript(scalar);}
            if(seen&&base_character(c))after=true;
            if(!seen||c!=C::halant)continue;u32 end=at+1;
            while(end<n){const u32 next=scalars[order[end]];const auto category=categories[order[end]];
                if(category!=C::nukta&&after!=after_subscript(next))break;
                if(category!=C::above&&category!=C::below&&category!=C::post&&category!=C::nukta)break;++end;}
            move_order(order,at,end-1);at=end-1;}}
    if(odia&&value.base!=nil){u32 target=nil;
        for(u32 at=0;at<n;++at){const u32 original=order[at];const auto c=categories[original];
            if(original<=value.base)continue;
            if(target==nil){if(c==C::below||c==C::post)target=c==C::below?consonant_prefix({order.data,n},categories,at):at;
                else if(base_character(c)&&(forms[original]&(sfnt_deva_below|sfnt_deva_post))){
                    // After an independent vowel, only a halant-led consonant
                    // group moves behind upper marks; a direct join stays put.
                    u32 before=original;while(before&&categories[before-1]==C::nukta)--before;
                    if(categories[value.base]==C::vowel&&(!before||categories[before-1]!=C::halant))continue;
                    target=consonant_prefix({order.data,n},categories,at);}}
            // A trailing placeholder includes the group's preceding joiner.
            if(target!=nil&&scalars[original]==0xa0&&target&&categories[order[target-1]]==C::joiner)--target;
            if(target==nil||(c!=C::above&&c!=C::bindu))continue;
            u32 attached=1;while(at+attached<n&&categories[order[at+attached]]==C::nukta)++attached;
            for(u32 j=0;j<attached;++j){const u32 saved=order[at+j];for(u32 from=at+j;from>target+j;--from)order[from]=order[from-1];order[target+j]=saved;}
            target+=attached;at+=attached-1;}}
    // Odia lower vowels follow subscript consonants, but precede a right-side
    // consonant group. Keep that group's halant/nukta prefix intact for GSUB.
    if((odia||malayalam)&&value.base!=nil){u32 target=nil;
        for(u32 at=0;at<n;++at){const u32 original=order[at];
            if(original<=value.base)continue;
            const u8 form=forms[original];
            const bool right_group=(form&sfnt_deva_post)&&!(form&sfnt_deva_below);
            const bool default_below=malayalam&&(form&indic_default_position)&&(form&sfnt_deva_below)&&!(form&sfnt_deva_post);
            if(target==nil&&base_character(categories[original])&&(right_group||default_below)){
                if(malayalam){u32 before=original;
                    while(before&&(categories[before-1]==C::nukta||categories[before-1]==C::joiner||categories[before-1]==C::nonjoiner))--before;
                    if(categories[value.base]==C::vowel&&(!before||categories[before-1]!=C::halant))continue;
                }
                target=consonant_prefix({order.data,n},categories,at);}
            if(target!=nil&&categories[original]==C::below){const u32 saved=original;
                for(u32 from=at;from>target;--from)order[from]=order[from-1];order[target++]=saved;}}}
    u32 matra_start=value.reph_end;
    // In a prefixed dot-reph group, a full main consonant precedes the
    // placeholder base. Its surviving halant/controls stay in that group;
    // vowel signs and nuktas remain after the placeholder. Keep source IDs.
    if(malayalam&&broken_reph!=nil&&value.base!=nil&&value.base>broken_reph&&
        (inserted_circle||scalars[0]==0xa0)&&!(forms[value.base]&sfnt_deva_post)){
        u32 from=0,to=0;for(u32 at=0;at<n;++at){if(!order[at])from=at;if(order[at]==value.base)to=at;}
        while(to+1<n){const auto c=categories[order[to+1]];if(c!=C::halant&&c!=C::joiner&&c!=C::nonjoiner)break;++to;}
        if(from<to)move_order(order,from,to);
    }
    if(malayalam&&broken_reph!=nil&&broken_reph+1<n&&value.base==0&&
        (inserted_circle||scalars[0]==0xa0)&&(forms[broken_reph+1]&sfnt_deva_post)){
        const bool defaults=(forms[broken_reph+1]&indic_default_position)!=0;
        if(value.pre_matra!=nil)matra_start=broken_reph+static_cast<u32>(defaults);
        else move_order(order,0,broken_reph-static_cast<u32>(!defaults));
    }
    if(malayalam&&broken_reph!=nil&&broken_reph+1<n&&value.base==0&&value.pre_matra!=nil&&
        (inserted_circle||scalars[0]==0xa0)&&(forms[broken_reph+1]&indic_default_position)&&
        (forms[broken_reph+1]&sfnt_deva_below)&&!(forms[broken_reph+1]&sfnt_deva_post))matra_start=broken_reph+2;
    if(bengali||odia||malayalam){u32 target=matra_start;
        for(u32 at=target;at<n;++at)if(categories[order[at]]==C::pre){u32 attached=1;
            while(at+attached<n&&categories[order[at+attached]]==C::nukta)++attached;
            for(u32 j=0;j<attached;++j){const u32 saved=order[at+j];u32 from=at+j;while(from>target+j){order[from]=order[from-1];--from;}order[target+j]=saved;}
            target+=attached;at+=attached-1;}}
    else if(value.pre_matra!=nil){u32 at=0;while(at<n&&order[at]!=value.pre_matra)++at;
        u32 attached=1;while(at+attached<n&&categories[order[at+attached]]==C::nukta)++attached;
        const u32 target=value.reph_end;
        if(at>=target&&at<n)for(u32 j=0;j<attached;++j){const u32 saved=order[at+j];u32 from=at+j;
            while(from>target+j){order[from]=order[from-1];--from;}order[target+j]=saved;}}
    // Explicit halant + ZWJ requests a half form even when signs follow it or
    // when nukta ordering has moved the halant next to that joiner.
    for(u32 i=1;i<n;++i)if(categories[order[i]]==C::joiner){u32 halant=i-1;if(!any_matra&&halant&&categories[order[halant]]==C::bindu)--halant;
        if(categories[order[halant]]!=C::halant)continue;u32 first=halant;
        while(first&&!base_character(categories[order[first]]))--first;
        if(base_character(categories[order[first]])&&order[first]>=value.reph_end)for(u32 j=first;j<=i;++j)masks[order[j]]|=sfnt_deva_half;}
    // Fallback accents precede a group's terminal halant after a prior
    // dead-consonant prefix or multiple full consonants. Direct vowel/post
    // groups retain their order. Logical roles survive later ligatures.
    if(malayalam&&n&&(forms[0]&indic_default_model)){u32 full=0;bool linked=false;
        for(u32 i=0;i+1<n;++i){full+=categories[i]==C::consonant&&(i==first_base||!(forms[i]&(sfnt_deva_post|sfnt_deva_below)));
            linked|=scalars[i]==0xd4e;
            if(categories[i]!=C::halant)continue;
            if((linked||full>1)&&(scalars[i+1]==0x953||scalars[i+1]==0x954)){
                u32 from=0,to=0;for(u32 at=0;at<n;++at){if(order[at]==i)from=at;if(order[at]==i+1)to=at;}
                if(from<to)move_order(order,from,to);
            }
            linked=true;
        }}
    if(broken_reph!=nil&&broken_reph+1==n&&inserted_circle)move_order(order,0,n-1);
    *result=value;return SfntStatus::ok;
}
SfntStatus sfnt_devanagari_initial_order(ConstSlice<u32> scalars,ConstSlice<u8> forms,Slice<u32> order,Slice<u8> masks,SfntDevanagariOrderResult* result,u64 work) noexcept {
    return indic_initial_order(SfntIndicProfile::devanagari,scalars,forms,order,masks,result,work);
}
}
