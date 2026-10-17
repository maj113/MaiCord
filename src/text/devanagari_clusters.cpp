#include "maicord/text/devanagari_clusters.hpp"
#include "maicord/text/indic_clusters.hpp"
#include "devanagari_character.hpp"
namespace maicord {
using Category=DevanagariCategory;
namespace {
bool kannada_composite(u32 scalar) noexcept {
    const u32 bit=scalar-0xcc0;
    return bit<16&&(sfnt_indic_script(SfntIndicProfile::kannada)->split_vowels&(1u<<bit));
}
}
DevanagariCategory indic_category(SfntIndicProfile profile,u32 scalar) noexcept {
    if(scalar==0x200d)return Category::joiner;if(scalar==0x200c)return Category::nonjoiner;
    if(scalar==0x25cc||scalar==0xa0)return Category::placeholder;
    if(profile==SfntIndicProfile::gurmukhi){
        if(scalar==0xa72||scalar==0xa73)return Category::vowel;
        if(scalar==0xa71)return Category::bindu;
        if(scalar==0xa51||scalar==0xa75)return Category::medial;
    }
    if(profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam){
        if(scalar==0x951||scalar==0x952)return Category::vedic;
        if(scalar>=0xa8e0&&scalar<=0xa8ff)return Category::other;
    }
    if(profile==SfntIndicProfile::telugu){
        if(scalar==0xc48)return Category::below;
        if(scalar==0xc01||scalar==0xc02)return Category::visarga;
        if(scalar==0xc5d)return Category::consonant;
    }
    if(profile==SfntIndicProfile::kannada){
        if(scalar==0xc80)return Category::other;
        if(scalar==0x1cfa)return Category::vowel;
        if(scalar==0xc82||scalar==0xcf3)return Category::visarga;
        if(scalar==0xcdd)return Category::consonant;
        if(kannada_composite(scalar))return Category::post;
    }
    if(profile==SfntIndicProfile::malayalam){
        if(scalar==0xd04)return Category::other;
        if(scalar==0x1cfa)return Category::vowel;
        if(scalar>=0xd41&&scalar<=0xd44)return Category::post;
        if((scalar>=0xd54&&scalar<=0xd56)||(scalar>=0xd7a&&scalar<=0xd7f))return Category::consonant;
        if(scalar==0xd3b||scalar==0xd3c)return Category::above;
        if(scalar==0xd02)return Category::visarga;
        if(scalar>=0xd4a&&scalar<=0xd4c)return Category::post;
    }
    const bool bengali=profile==SfntIndicProfile::bengali;
    if(profile==SfntIndicProfile::odia){
        if(scalar==0xb48)return Category::above;
        if(scalar==0xb4b||scalar==0xb4c||scalar==0xb55||scalar==0xb57)return Category::post;
        if(scalar==0xb02)return Category::visarga;
    }
    if(profile==SfntIndicProfile::gujarati&&scalar==0xac9)return Category::post;
    if(profile==SfntIndicProfile::gujarati&&scalar==0xafb)return Category::bindu;
    if(bengali&&scalar==0x980)return Category::placeholder;
    if(bengali&&scalar==0x982)return Category::visarga;
    if(bengali&&scalar==0x9fc)return Category::other;
    if(bengali&&scalar==0x9fe)return Category::bindu;
    if(bengali&&scalar==0x9ce)return Category::consonant;
    if(bengali&&(scalar==0x9cb||scalar==0x9cc))return Category::post;
    if(!bengali&&(scalar==0x953||scalar==0x954))return Category::bindu;
    if(scalar==0xa8fe)return Category::vowel;if(scalar==0xa8ff)return Category::above;
    if((scalar>=0x1ce9&&scalar<=0x1cec)||scalar==0x1cf2||scalar==0x1cf3||scalar==0x1cfa)return Category::placeholder;
    if((scalar>=0x1ce2&&scalar<=0x1ce8)||scalar==0x1ced)return Category::vedic;
    const auto& p=*unicode_properties(scalar);using I=UnicodeIndicSyllabicCategory;
    const auto* info=sfnt_indic_script(profile);if(!info)return Category::other;const u32 first=info->block_first;
    if(scalar>=first&&scalar<first+128)switch(p.indic_syllabic_category()){
        case I::consonant:return Category::consonant;case I::vowel_independent:return Category::vowel;
        case I::nukta:return Category::nukta;case I::virama:return Category::halant;
        case I::bindu:return Category::bindu;case I::visarga:return Category::visarga;
        case I::cantillation_mark:return Category::vedic;
        case I::vowel_dependent:switch(p.indic_positional_category()){
            case UnicodeIndicPositionalCategory::left:return Category::pre;
            case UnicodeIndicPositionalCategory::top:return Category::above;
            case UnicodeIndicPositionalCategory::bottom:return Category::below;
            case UnicodeIndicPositionalCategory::right:return Category::post;
            default:return Category::other;}
        default:return Category::other;
    }
    if((scalar>=0x1cd0&&scalar<=0x1cff)||(scalar>=0xa8e0&&scalar<=0xa8ff)){
        if(p.indic_syllabic_category()==I::cantillation_mark||p.indic_syllabic_category()==I::syllable_modifier)return Category::vedic;
    }
    return Category::other;
}
namespace {
// Bit 15 distinguishes a retained IRI/pre-vowel restriction after a direct
// YA/RA/VA/HA group from the original independent-vowel restriction. A following
// nukta or medial releases it. Bits 0..14 keep their U+xx3E..U+xx4C meaning.
constexpr u16 gurmukhi_subjoined_i=0x8002u;
enum class Phase : u8 {closed,dot_reph,space,initial_join,base,halant,strict_halant,join,joined_nukta,reph_nukta,reph_nukta_join,medial,matra,modifier,visarga,vedic};
struct State {
    Phase phase=Phase::closed;u8 nuktas=0,rank=0,vedics=0;u32 previous_vedic=0;
    bool allow_consonant=false,allow_vowel=false,joined=false,after_matra=false,vowel_base=false,halant_before_join=false,halant_bindu=false,matra_nukta=false,matra_halant=false;
    u16 excluded_matras=0;
    void begin(SfntIndicProfile profile,Category c,u32 scalar,u8 form) noexcept {
        *this={};if(c==Category::consonant||c==Category::vowel||c==Category::placeholder){phase=scalar==0xa0?Phase::space:Phase::base;allow_consonant=true;allow_vowel=profile==SfntIndicProfile::bengali?(scalar==0x9b0||scalar==0x9f0):profile!=SfntIndicProfile::gurmukhi&&profile!=SfntIndicProfile::telugu&&scalar==sfnt_indic_script(profile)->block_first+0x30u&&((profile!=SfntIndicProfile::kannada&&profile!=SfntIndicProfile::malayalam)||(form&sfnt_deva_reph));vowel_base=c==Category::vowel;}
        else if(c==Category::joiner){phase=Phase::initial_join;allow_consonant=true;}
        else if(profile==SfntIndicProfile::malayalam&&scalar==0xd4e)phase=Phase::dot_reph;
        // Independent-vowel matra restrictions per script block. Gurmukhi IRI/URA are
        // vowel carriers; their atomic-vowel alternatives stay separate groups of the
        // original input (no normalization).
        excluded_matras=scalar==0xd07||scalar==0xd09?0x8000u:scalar==0xd12?0x8001u:scalar==0xd0e?0x100u:scalar==0xc92?0x4000u:scalar==0xc0b||scalar==0xc0c?1u:scalar==0xc12?0x4000u:scalar==0xb05?1u:scalar==0xa05?0x4401u:scalar==0xa72?0x206u:scalar==0xa73?0x18u:scalar==0xa85?0x6e81u:scalar==0xa86?0x680u:scalar==0x985?1u:scalar==0x905?0x7901u:scalar==0x906?0x780u:scalar==0x90f?0x380u:0;
    }
    void next_base(SfntIndicProfile profile,Category c,u32 scalar,u8 form,bool clear_vowel=false,bool retain_vowel=false) noexcept {
        const u16 previous=excluded_matras;const bool direct_halant=phase==Phase::halant,original_vowel=vowel_base;begin(profile,c,scalar,form);allow_vowel=false;
        // Direct joins retain the initial Gujarati/Gurmukhi/Odia vowel restriction.
        // Gurmukhi IRI + direct YA/RA/VA/HA retains only the pre-vowel constraint;
        // a control following the halant or a joined nukta disables that form.
        if(profile==SfntIndicProfile::gurmukhi)excluded_matras=retain_vowel?previous:(direct_halant&&!clear_vowel&&(previous&2u)&&(scalar==0xa2f||scalar==0xa30||scalar==0xa35||scalar==0xa39)?gurmukhi_subjoined_i:0);
        else if(profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada){
            // Carries the vowel restriction over. Bit 15 (free in both profiles' vowel
            // masks) is set while it is inactive: retain_vowel or a below form after a
            // direct halant activates it, a pre-form there keeps the prior state
            // (non-vowel origin), any other base deactivates it. Kannada bit 13 records
            // a joined independent-vowel origin once its AU restriction is cleared; a
            // later direct vowel stays separate.
            vowel_base=vowel_base||(original_vowel&&retain_vowel);
            const bool active=retain_vowel||(direct_halant&&((form&sfnt_deva_below)||(!original_vowel&&(form&sfnt_deva_pre)&&!(previous&0x8000u))));
            excluded_matras=static_cast<u16>((clear_vowel?0:((previous&0x7fffu)|(active?0:0x8000u)))|
                (profile==SfntIndicProfile::kannada&&(previous||original_vowel)?0x2000u:0));
        }
        else if(profile==SfntIndicProfile::malayalam){vowel_base=vowel_base||(original_vowel&&retain_vowel);
            excluded_matras=static_cast<u16>((retain_vowel?previous:direct_halant&&!clear_vowel&&(form&(sfnt_deva_post|sfnt_deva_below))?previous&0x100u:0)|((original_vowel||(previous&0x2000u))?0x2000u:0));}
        else if(profile==SfntIndicProfile::gujarati||profile==SfntIndicProfile::odia)excluded_matras=retain_vowel?previous:0;
        else if(clear_vowel||profile==SfntIndicProfile::bengali)excluded_matras=0;
    }
    bool accept(SfntIndicProfile profile,Category c,u32 scalar,u32 length,u8 form) noexcept {
        if(phase==Phase::closed)return false;
        const bool bengali=profile==SfntIndicProfile::bengali,devanagari=profile==SfntIndicProfile::devanagari,odia=profile==SfntIndicProfile::odia;
        if(profile==SfntIndicProfile::malayalam&&((scalar>=0xd3e&&scalar<=0xd4c&&(excluded_matras&0xdfffu&(1u<<(scalar-0xd3e))))||(scalar==0xd57&&(excluded_matras&0x8000u))))return false;
        if(profile==SfntIndicProfile::gurmukhi&&scalar>=0xa3e&&scalar<=0xa4c&&(excluded_matras&(1u<<(scalar-0xa3e))))return false;
        if(profile==SfntIndicProfile::telugu&&!(excluded_matras&0x8000u)&&scalar>=0xc3e&&scalar<=0xc4c&&(excluded_matras&(1u<<(scalar-0xc3e))))return false;
        if(profile==SfntIndicProfile::telugu&&!(excluded_matras&0x8000u)&&scalar==0xc55&&(excluded_matras&0x4000u))return false;
        if(profile==SfntIndicProfile::kannada&&scalar==0xccc&&(excluded_matras&0xc000u)==0x4000u)return false;
        if(bengali&&scalar==0x9be&&(excluded_matras&1))return false;
        if(profile==SfntIndicProfile::odia&&scalar==0xb3e&&(excluded_matras&1))return false;
        if(profile==SfntIndicProfile::gujarati&&scalar>=0xabe&&scalar<=0xacc&&(excluded_matras&(1u<<(scalar-0xabe))))return false;
        if(scalar>=0x93e&&scalar<=0x94c&&(excluded_matras&(1u<<(scalar-0x93e))))return false;
        const bool consonant=c==Category::consonant||(c==Category::placeholder&&scalar!=0xa0);
        const bool joiner=c==Category::joiner||(!devanagari&&scalar==0xa0);
        const bool control=joiner||c==Category::nonjoiner;
        if(c==Category::medial){if(phase!=Phase::base||vowel_base||(excluded_matras&&!(excluded_matras&0x8000u)))return false;phase=Phase::medial;excluded_matras=0;return true;}
        if(profile==SfntIndicProfile::malayalam&&scalar==0xd4e&&(phase==Phase::space||phase==Phase::initial_join)){phase=Phase::dot_reph;return true;}
        if(phase==Phase::dot_reph){if(!consonant)return false;next_base(profile,c,scalar,form);return true;}
        if(phase==Phase::space){phase=joiner?Phase::initial_join:Phase::closed;return joiner||(c>=Category::nukta&&c!=Category::nonjoiner);}
        if(phase==Phase::initial_join){phase=c==Category::halant?Phase::strict_halant:Phase::closed;return c>=Category::nukta&&!control;}
        // Windows stops adding bases once ten input scalars are consumed; a
        // three-scalar consonant group can take the cluster to twelve before the
        // next base starts a new one.
        if(phase==Phase::strict_halant){if(!consonant||!allow_consonant||length>=10)return false;next_base(profile,c,scalar,form);return true;}
        if(phase==Phase::joined_nukta){if((consonant||c==Category::vowel)&&length<10){next_base(profile,c,scalar,form,true);return true;}
            if(c!=Category::bindu&&c!=Category::visarga)return false;}
        // A Kannada reph + joiner + nukta permits another control but no bare
        // consonant or halant. The second join permits a halant/matra, while a
        // second nukta starts a new group.
        if(phase==Phase::reph_nukta&&control){
            if(c!=Category::nonjoiner){phase=Phase::reph_nukta_join;joined=true;nuktas=1;}return true;}
        if(phase==Phase::reph_nukta_join){if(c==Category::nukta)return false;phase=Phase::join;}
        if(phase==Phase::base){
            if(c==Category::nukta&&nuktas<2){if((excluded_matras&0x8000u)||(profile==SfntIndicProfile::kannada&&(excluded_matras&0x2000u)))excluded_matras=static_cast<u16>(profile==SfntIndicProfile::kannada?excluded_matras&0x2000u:0);++nuktas;allow_vowel=false;return true;}
            if(c==Category::halant){phase=Phase::halant;nuktas=static_cast<u8>(profile==SfntIndicProfile::gujarati&&nuktas==2);return true;}
            if(control&&!joined){
                if(c==Category::nonjoiner&&!vowel_base){nuktas=maximum<u8>(nuktas,1);allow_vowel=false;return true;}
                phase=Phase::join;joined=true;nuktas=1;return true;}
        }else if(phase==Phase::halant){
            if(c==Category::nukta&&!nuktas&&(!after_matra||!matra_nukta)){++nuktas;allow_vowel=false;if(after_matra)matra_nukta=true;return true;}
            if(control&&!joined&&!after_matra){joined=true;halant_before_join=!allow_vowel;nuktas=static_cast<u8>(profile==SfntIndicProfile::kannada&&allow_vowel?2:1);phase=c==Category::nonjoiner?Phase::closed:Phase::join;return true;}
            if(!after_matra&&length<10&&((consonant&&allow_consonant)||(c==Category::vowel&&allow_vowel))){next_base(profile,c,scalar,form);return true;}
        }else if(phase==Phase::join){
            if(control)return false;
            // Precomposed Gurmukhi/Odia nukta consonants begin a new cluster after
            // a vowel's direct join; an intervening halant permits the base.
            if(vowel_base&&!halant_before_join&&
                ((profile==SfntIndicProfile::gurmukhi&&(scalar==0xa33||scalar==0xa36||scalar==0xa59||scalar==0xa5a||scalar==0xa5b||scalar==0xa5e))||
                 (odia&&(scalar==0xb5c||scalar==0xb5d))))return false;
            if(c==Category::nukta&&(!vowel_base||halant_before_join)){phase=profile==SfntIndicProfile::kannada&&nuktas==2&&!halant_before_join?Phase::reph_nukta:
                    !devanagari&&halant_before_join?Phase::joined_nukta:Phase::base;
                nuktas=2;joined=false;return true;}
            if(c==Category::halant&&!halant_before_join){if(profile==SfntIndicProfile::kannada&&nuktas==2)return false;phase=vowel_base?Phase::halant:Phase::strict_halant;if(vowel_base){nuktas=0;joined=false;}return true;}
            if(!after_matra&&allow_consonant&&length<10&&(consonant||(c==Category::vowel&&!vowel_base&&!((profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam)&&(excluded_matras&0x2000u))))){next_base(profile,c,scalar,form,false,!halant_before_join);return true;}
            if(vowel_base&&!halant_before_join&&c!=Category::pre&&c!=Category::above&&c!=Category::below&&c!=Category::post)return false;
        }
        // Gurmukhi udaat/yakash follow a consonant plus optional nukta.
        // Their phase accepts later vowels/signs but no second medial/control.
        if(phase==Phase::medial&&c==Category::halant){phase=Phase::halant;nuktas=static_cast<u8>(nuktas==2);allow_consonant=false;return true;}
        const u8 next=c==Category::pre?1:c==Category::above?2:c==Category::below?3:c==Category::post?4:0;
        const bool composite=profile==SfntIndicProfile::malayalam?(scalar>=0xd4a&&scalar<=0xd4c):bengali?(scalar==0x9cb||scalar==0x9cc):profile==SfntIndicProfile::kannada&&kannada_composite(scalar);
        // The spare high bit records a pre/above/below part. A composite may not add
        // another left part after both a lower-ranked and right part. Kannada uses
        // the same history for its two/three-part vowels. Packed into the rank byte
        // to keep State small.
        const u8 previous=rank&7;
        // Odia keeps combined pre/above/below ranks when entering right parts: O,
        // AU-length and AU contribute ranks 1/2/3, and ordinary right marks repeat
        // without erasing that history. Uses two spare rank bits.
        const u8 odia_right=odia?static_cast<u8>(scalar==0xb4b?1:scalar==0xb57?2:scalar==0xb4c?3:0):0;
        const bool odia_ai=odia&&scalar==0xb48,telugu_ai=profile==SfntIndicProfile::telugu&&scalar==0xc48;
        if(odia_right&&previous==4&&odia_right<=((rank>>4)&3))return false;
        if(next&&(phase==Phase::base||phase==Phase::reph_nukta||phase==Phase::medial||(phase==Phase::join&&!halant_before_join)||phase==Phase::matra||(phase==Phase::halant&&after_matra))&&
            !(composite&&previous==4&&(rank&8))&&(next>previous||(next==4&&previous==4)||(odia_ai&&previous==2&&!(rank&16))||(telugu_ai&&previous==3&&!(rank&16)))){
                phase=Phase::matra;rank=static_cast<u8>(next|(rank&0xf8)|(((bengali||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam)&&(next<4||composite))?8:0));
                // Telugu AI can follow an above or a below part separately,
                // but cannot add both after an existing above+below pair.
                if(profile==SfntIndicProfile::telugu&&(telugu_ai||next==2))rank|=16;
                if(odia)rank|=static_cast<u8>((odia_ai?3:next<4?next:odia_right)<<4);nuktas=0;after_matra=true;return true;}
        if(phase==Phase::matra){
            if(c==Category::nukta&&!matra_nukta){matra_nukta=true;return true;}
            if(c==Category::halant&&!matra_halant){matra_halant=true;phase=Phase::halant;nuktas=0;return true;}
        }
        if(c==Category::bindu&&phase!=Phase::modifier&&phase!=Phase::visarga&&phase!=Phase::vedic){
            if((profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam)&&halant_bindu)return false;
            if((devanagari||((profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam)&&(scalar==0x953||scalar==0x954)))&&phase==Phase::halant&&!after_matra&&!allow_vowel){if(halant_bindu)return false;halant_bindu=true;}
            else phase=Phase::modifier;return true;}
        if(c==Category::visarga&&phase!=Phase::vedic){phase=Phase::visarga;return true;}
        if(c==Category::vedic&&vedics<2&&(!vedics||previous_vedic!=scalar)){phase=Phase::vedic;previous_vedic=scalar;++vedics;return true;}
        if(c==Category::nonjoiner&&(phase==Phase::modifier||phase==Phase::visarga||(phase==Phase::vedic&&vedics<2))){phase=Phase::closed;return true;}
        return false;
    }
};
static_assert(sizeof(State)==20);
}
SfntStatus sfnt_indic_cluster_boundaries(SfntIndicProfile profile,ConstSlice<u32> scalars,Slice<u8> output,u64 work,ConstSlice<u8> forms) noexcept {
    if((profile!=SfntIndicProfile::devanagari&&profile!=SfntIndicProfile::bengali&&profile!=SfntIndicProfile::gujarati&&profile!=SfntIndicProfile::gurmukhi&&profile!=SfntIndicProfile::odia&&profile!=SfntIndicProfile::telugu&&profile!=SfntIndicProfile::kannada&&profile!=SfntIndicProfile::malayalam)||(scalars.count&&!scalars.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    if((profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam)?(forms.count!=128||!forms.data):forms.count!=0)return SfntStatus::invalid_input;
    if(output.count<scalars.count)return SfntStatus::output_too_small;
    if(static_cast<u64>(scalars.count)*4>work)return SfntStatus::resource_limit;
    const u64 n=scalars.count,bytes=n*4,a=reinterpret_cast<usize>(scalars.data),b=reinterpret_cast<usize>(output.data);
    if(a>~u64{0}-bytes||b>~u64{0}-n||(n&&a<b+n&&b<a+bytes))return SfntStatus::invalid_input;
    if(forms.count){const u64 f=reinterpret_cast<usize>(forms.data);if(f>~u64{0}-forms.count||(n&&f<b+n&&b<f+forms.count))return SfntStatus::invalid_input;}
    for(u32 i=0;i<scalars.count;++i)if(!unicode_is_scalar(scalars[i]))return SfntStatus::invalid_input;
    // Windows limits a cluster's mark tail to 31 input scalars.
    State state{};u32 first=0;const u32 block=sfnt_indic_script(profile)->block_first;
    for(u32 i=0;i<scalars.count;++i){const auto c=indic_category(profile,scalars[i]);const u32 offset=scalars[i]-block;
        const u8 form=forms.count&&offset<128?forms[offset]:0;const bool start=!i||i-first>=31||!state.accept(profile,c,scalars[i],i-first,form);
        output[i]=static_cast<u8>(start);if(start){first=i;state.begin(profile,c,scalars[i],form);}}
    return SfntStatus::ok;
}
SfntStatus sfnt_devanagari_cluster_boundaries(ConstSlice<u32> scalars,Slice<u8> output,u64 work) noexcept {
    return sfnt_indic_cluster_boundaries(SfntIndicProfile::devanagari,scalars,output,work);
}
}
