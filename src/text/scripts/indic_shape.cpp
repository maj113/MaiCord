#include "../shaping/run_profile.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/unicode/unicode.hpp"
#include "maicord/text/scripts/detail/indic_form.hpp"
#include "maicord/text/shaping/clusters.hpp"
#include "maicord/text/shaping/shape_run.hpp"
#include "../shaping/compact_run.hpp"
#include "../shaping/script_paragraph.hpp"
#include "indic_syllables.hpp"
#include "script_work.hpp"
// Indic runs of the nine modern Indic scripts, shaped as HarfBuzz's Indic shaper (hb-ot-shaper-indic)
// implements the Microsoft Indic shaping specs: syllables from the Indic grammar, dotted circles for
// broken clusters, initial reordering and feature masks before the basic forms, final reordering
// before the presentation forms.
namespace maicord {
namespace {
using script_work::Work;
using C=IndicCategory;using P=IndicPosition;
constexpr u32 nil=0xffffffffu;
// Per script (HarfBuzz indic_configs): where a reph moves, whether reph needs Ra,H,ZWJ (Telugu) or is
// an encoded repha (Malayalam), and whether below-base forms also apply before the base.
struct Config {P reph;bool explicit_reph,repha,below_before_base;};
// SfntIndicProfile order: devanagari, tamil, bengali, gujarati, gurmukhi, odia, telugu, kannada, malayalam.
constexpr Config configs[]{
    {P::before_post,false,false,true},{P::after_post,false,false,true},{P::after_sub,false,false,true},
    {P::before_post,false,false,true},{P::before_sub,false,false,true},{P::after_main,false,false,true},
    {P::after_post,true,false,false},{P::after_post,false,false,false},{P::after_main,false,true,true}};
// Feature eligibility (SfntGsubGlyph::reserved): rphf, half, blwf, pref, abvf/pstf and init. The other
// Indic features apply to every glyph of the syllable. The top bit marks the glyphs of a non-Indic
// cluster, which keep their order; no step mask uses it.
constexpr u8 init_mask=sfnt_deva_init,reph_mask=sfnt_deva_reph,below_mask=sfnt_deva_below,half_mask=sfnt_deva_half,
    pref_mask=sfnt_deva_pre,post_mask=sfnt_deva_post,plain_mask=128;
// Component metadata: a class in the high nibble and the reordering position in the low one. The
// break classes are characters whose general category ends a word for init (HarfBuzz: not Cf..Mn).
enum Class : u8 {other,word_break,consonant,consonant_break,halant,zwj,zwnj,nukta,matra,post_matra,repha,hidden};
constexpr bool consonant_category(C c) noexcept {return (indic_consonants>>static_cast<u32>(c))&1u;}
bool breaks_word(const UnicodeProperties& properties) noexcept {
    const auto category=properties.general_category();
    return category<UnicodeGeneralCategory::cf||category>UnicodeGeneralCategory::mn;
}
u8 piece_metadata(const IndicPiece& piece,P position) noexcept {
    u8 c;
    switch(piece.category()){
    case C::h:c=halant;break;case C::zwj:c=zwj;break;case C::zwnj:c=zwnj;break;case C::n:c=nukta;break;
    case C::m:c=matra;break;case C::mpst:c=post_matra;break;case C::repha:c=repha;break;
    default:{const auto& properties=*unicode_properties(piece.scalar());
        if(consonant_category(piece.category()))c=breaks_word(properties)?consonant_break:consonant;
        else if(properties.has(UnicodeFlag::default_ignorable_code_point))c=hidden;
        else c=breaks_word(properties)?word_break:other;}
    }
    return static_cast<u8>(c<<4|static_cast<u8>(position));
}
void set_position(IndicPiece& piece,P position) noexcept {piece.value=(piece.value&~(15u<<26))|static_cast<u32>(position)<<26;}
struct Font {
    SfntIndicProfile profile;ConstSlice<u8> forms;u16 virama;u32 block;bool pref;
    // Context-free font forms of a mapped consonant (sfnt_deva_* probe bits); zero outside the block.
    u8 form(u32 scalar) const noexcept {const u32 offset=scalar-block;return offset<128?forms[offset]:0;}
};
// Initial reordering of one Indic syllable (HarfBuzz initial_reordering_consonant_syllable): consonant
// positions from the font, the base, reph, the position sort with the left-matra order kept, then the
// feature masks. pieces keep their logical order and receive the final positions; order lists them in
// glyph order and masks (logical index) the eligibility.
void initial_reordering(const Font& font,Slice<IndicPiece> pieces,Slice<u32> order,Slice<u8> masks) noexcept {
    const u32 n=pieces.count;const auto& config=configs[static_cast<u8>(font.profile)];
    // A lone consonant or vowel is its own base, with no masks.
    if(n==1&&consonant_category(pieces[0].category())){set_position(pieces[0],P::base_c);order[0]=0;masks[0]=0;return;}
    const auto category=[&](u32 i) noexcept {return pieces[i].category();};
    const auto position=[&](u32 i) noexcept {return pieces[i].position();};
    const auto consonant=[&](u32 i) noexcept {return consonant_category(category(i));};
    const auto joiner=[&](u32 i) noexcept {return category(i)==C::zwj||category(i)==C::zwnj;};
    // Below-base (blwf/vatu) and post-base (pstf/pref) consonants, as the font forms them with a virama
    // (update_consonant_positions_indic); only when the font maps the virama.
    if(font.virama)for(u32 i=0;i<n;++i)if(position(i)==P::base_c){const u8 form=font.form(pieces[i].scalar());
        if(form&(sfnt_deva_below|sfnt_deva_vatu))set_position(pieces[i],P::below_c);
        else if(form&(sfnt_deva_post|sfnt_deva_pre))set_position(pieces[i],P::post_c);}
    // Kannada Ra,H,ZWJ behaves like Ra,ZWJ,H (legacy usage).
    if(font.profile==SfntIndicProfile::kannada&&n>=3&&category(0)==C::ra&&category(1)==C::h&&category(2)==C::zwj){
        const auto saved=pieces[1];pieces[1]=pieces[2];pieces[2]=saved;}
    // Base: from the end, the last consonant without a below-base or post-base form (post-base forms
    // follow below-base ones), not counting an initial Ra,H that forms a reph.
    u32 base=n,limit=0;bool reph=false;
    const u32 virama_scalar=font.block+0x4d;
    if(config.repha?category(0)==C::repha:n>=3&&(config.explicit_reph?category(2)==C::zwj:!joiner(2))&&
        pieces[1].scalar()==virama_scalar&&(font.form(pieces[0].scalar())&sfnt_deva_reph)){
        limit=config.repha?1:2;while(limit<n&&joiner(limit))++limit;base=0;reph=true;}
    {u32 i=n;bool below=false;
        do{--i;
            if(consonant(i)){
                if(position(i)!=P::below_c&&(position(i)!=P::post_c||below)){base=i;break;}
                if(position(i)==P::below_c)below=true;
                base=i;
            // A ZWJ after a halant ends the search and asks for a half form.
            }else if(i&&category(i)==C::zwj&&category(i-1)==C::h)break;
        }while(i>limit);}
    if(reph&&base==0&&limit<=2)reph=false;
    for(u32 i=0;i<base&&i<n;++i)if(position(i)>P::pre_c)set_position(pieces[i],P::pre_c);
    if(base<n)set_position(pieces[base],P::base_c);
    if(reph)set_position(pieces[0],P::ra_to_become_reph);
    // Joiners, nukta, register shifters, medials and halants move with the preceding character; a
    // halant after a left matra stays with the consonant before it.
    {P last=P::start;
        for(u32 i=0;i<n;++i){const auto c=category(i);
            if(c==C::zwj||c==C::zwnj||c==C::n||c==C::rs||c==C::cm||c==C::h){set_position(pieces[i],last);
                if(c==C::h&&last==P::pre_m)for(u32 j=i;j;--j)if(position(j-1)!=P::pre_m){set_position(pieces[i],position(j-1));break;}}
            else if(position(i)!=P::smvd){
                if(c==C::mpst&&i&&category(i-1)==C::sm)set_position(pieces[i-1],position(i));
                last=position(i);}}}
    // A post-base consonant owns what precedes it since the last consonant or matra.
    {u32 last=base;
        for(u32 i=base+1;i<n;++i){
            if(consonant(i)){for(u32 j=last+1;j<i;++j)if(position(j)<P::smvd)set_position(pieces[j],position(i));last=i;}
            else if(category(i)==C::m||category(i)==C::mpst)last=i;}}
    // Stable sort by position.
    {u32 starts[16]{};for(u32 i=0;i<n;++i)++starts[static_cast<u8>(position(i))+1];
        for(u32 p=1;p<16;++p)starts[p]+=starts[p-1];
        for(u32 i=0;i<n;++i)order[starts[static_cast<u8>(position(i))]++]=i;}
    // Base again; consecutive left matras keep their logical order, each with its nuktas after it.
    base=n;u32 first_left=n,last_left=n;
    for(u32 i=0;i<n;++i){const auto p=position(order[i]);if(p==P::base_c){base=i;break;}
        if(p==P::pre_m){if(first_left==n)first_left=i;last_left=i;}}
    const auto reverse=[&](u32 first,u32 end) noexcept {while(first+1<end){const u32 saved=order[first];order[first++]=order[--end];order[end]=saved;}};
    if(first_left<last_left){reverse(first_left,last_left+1);u32 i=first_left;
        for(u32 j=i;j<=last_left;++j)if(category(order[j])==C::m||category(order[j])==C::mpst){reverse(i,j+1);i=j+1;}}
    // Masks: rphf on the reph, half (and blwf in pre-and-post scripts) before the base, blwf, abvf and
    // pstf after it.
    for(u32 i=0;i<n;++i)masks[i]=0;
    for(u32 i=0;i<n&&position(order[i])==P::ra_to_become_reph;++i)masks[order[i]]|=reph_mask;
    const u8 before=static_cast<u8>(half_mask|(config.below_before_base?below_mask:0));
    for(u32 i=0;i<base&&i<n;++i)masks[order[i]]|=before;
    for(u32 i=base+1;i<n;++i)masks[order[i]]|=below_mask|post_mask;
    // pref on the first halant,consonant pair after the base that the font forms.
    if(font.pref&&base+2<n)for(u32 i=base+1;i+1<n;++i)
        if(pieces[order[i]].scalar()==virama_scalar&&(font.form(pieces[order[i+1]].scalar())&sfnt_deva_pre)){
            masks[order[i]]|=pref_mask;masks[order[i+1]]|=pref_mask;break;}
    // A ZWNJ disables half forms back to the preceding consonant.
    for(u32 i=1;i<n;++i)if(category(order[i])==C::zwnj){u32 j=i;
        do{--j;masks[order[j]]&=static_cast<u8>(~half_mask);}while(j&&!consonant(order[j]));}
}
// metadata: the reordered pieces' piece_metadata per initial glyph, read once by the first stage;
// starts: the first unit of each syllable (the initial clusters), units the run's length; init: some
// step uses the init mask; space: the space glyph hidden default ignorables become, looked up at the
// first one.
struct Policy {
    const SfntGdef* gdef;const Font* font;const SfntCmap* cmap;const u8* metadata;u32 count;const u32* starts;u32 syllables,units;
    SfntComponentForest* forest;bool init;u16 space;bool space_known;
};
// Indic's view of the shared accessors: the class in the high nibble of a glyph's metadata and the reordering
// position in the low one (HarfBuzz copies the expanded glyph's properties to every output).
struct Glyphs : script_work::Glyphs {
    Glyphs(SfntComponentForest& forest,Slice<SfntComponentGlyph> glyphs,Work& work) noexcept : script_work::Glyphs{forest,glyphs,work} {}
    u8 cls(u32 i) noexcept {return meta(i)>>4;}
    P pos(u32 i) noexcept {return static_cast<P>(meta(i)&15);}
    void set_pos(u32 i,P p) noexcept {if(auto* n=first(i))n->reserved=static_cast<u16>((n->reserved&0xf0ffu)|static_cast<u16>(p)<<8);}
    // HarfBuzz is_one_of: a ligature is none of the classes.
    bool is(u32 i,u32 classes) noexcept {return !ligated(i)&&((classes>>cls(i))&1u);}
    bool halant(u32 i) noexcept {return is(i,1u<<Class::halant);}
    bool joiner(u32 i) noexcept {return is(i,1u<<Class::zwj|1u<<Class::zwnj);}
    bool consonant(u32 i) noexcept {return is(i,1u<<Class::consonant|1u<<Class::consonant_break);}
};
// Final reordering of one Indic syllable after the basic forms (HarfBuzz final_reordering_syllable_indic).
void final_reordering(Policy& p,Glyphs& s,u32 count) noexcept {
    const auto& config=configs[static_cast<u8>(p.font->profile)];
    const bool malayalam=p.font->profile==SfntIndicProfile::malayalam,tamil=p.font->profile==SfntIndicProfile::tamil;
    constexpr u32 matra_or_halant=1u<<Class::matra|1u<<Class::post_matra|1u<<Class::halant;
    bool try_pref=p.font->pref;u32 base=0;
    for(;base<count;++base)if(s.pos(base)>=P::base_c){
        if(try_pref&&base+1<count){
            for(u32 i=base+1;i<count;++i)if(s.g[i].glyph.reserved&pref_mask){
                // A pref candidate that formed nothing: the base is around here.
                if(!s.ligated(i)){base=i;while(base<count&&s.halant(base))++base;if(base<count)s.set_pos(base,P::base_c);try_pref=false;}
                break;}
            if(base==count)break;
        }
        // Malayalam skips over unformed below-base (not post-base) forms.
        if(malayalam)for(u32 i=base+1;i<count;++i){
            while(i<count&&s.joiner(i))++i;
            if(i==count||!s.halant(i))break;
            ++i;while(i<count&&s.joiner(i))++i;
            if(i<count&&s.consonant(i)&&s.pos(i)==P::below_c){base=i;s.set_pos(base,P::base_c);}}
        if(base&&s.pos(base)>P::base_c)--base;
        break;
    }
    if(base==count&&base&&s.is(base-1,1u<<Class::zwj))--base;
    if(base<count)while(base&&s.is(base,1u<<Class::nukta|1u<<Class::halant))--base;
    // Pre-base matras move after the last standalone halant before the main consonant, but not past a
    // halant that a ZWJ follows. Malayalam and Tamil have no half forms: the matra stays before the base.
    if(count>1&&base){
        u32 target=base==count?base-2:base-1;
        if(!malayalam&&!tamil){
            for(;;){
                while(target&&!s.is(target,matra_or_halant))--target;
                if(s.halant(target)&&s.pos(target)!=P::pre_m){
                    if(target+1<count&&s.cls(target+1)==Class::zwj&&target){--target;continue;}
                }else target=0;
                break;
            }
        }
        if(target&&s.pos(target)!=P::pre_m)
            for(u32 i=target;i;--i)if(s.pos(i-1)==P::pre_m){const u32 old=i-1;if(old<base&&base<=target)--base;s.move(old,target+1);--target;}
    }
    // Reph: an initial Ra,H that ligated (or an encoded repha that did not) moves to its script position.
    if(count>1&&s.pos(0)==P::ra_to_become_reph&&((s.cls(0)==Class::repha)!=s.ligated(0))){
        u32 target=nil;
        // After the first explicit halant before the main consonant (and a joiner after it).
        const auto explicit_halant=[&]() noexcept {u32 at=1;while(at<base&&!s.halant(at))++at;
            if(at<base&&s.halant(at)){if(at+1<base&&s.joiner(at+1))++at;target=at;}};
        if(config.reph!=P::after_post)explicit_halant();
        if(target==nil&&config.reph==P::after_main){u32 at=base;while(at+1<count&&s.pos(at+1)<=P::after_main)++at;if(at<count)target=at;}
        if(target==nil&&config.reph==P::after_sub){u32 at=base;
            while(at+1<count){const auto next=s.pos(at+1);if(next==P::post_c||next==P::after_post||next==P::smvd)break;++at;}
            if(at<count)target=at;}
        if(target==nil)explicit_halant();
        if(target==nil){
            // The end of the syllable, before syllable modifiers; before a final halant that follows a matra.
            target=count-1;while(target&&s.pos(target)==P::smvd)--target;
            if(s.halant(target))for(u32 i=base+1;i<target;++i){const u8 c=s.cls(i);if(c==Class::matra||c==Class::post_matra)--target;}
        }
        s.move(0,target+1);if(base&&base<=target)--base;
    }
    // A formed pre-base-reordering consonant moves like a pre-base matra, or right before the base.
    if(try_pref&&base+1<count)for(u32 i=base+1;i<count;++i)if(s.g[i].glyph.reserved&pref_mask){
        if(s.ligated(i)){u32 target=base;
            if(!malayalam&&!tamil)while(target&&!s.is(target-1,matra_or_halant))--target;
            if(target&&s.halant(target-1)&&target<count&&s.joiner(target))++target;
            s.move(i,target);if(target<=base&&base<i)++base;}
        break;}
}
SfntStatus prepare(void* opaque,u16 stage,SfntGlyphBuffer* buffer,SfntClusterMap*,SfntComponentForest* forest,const SfntGsubExecutionScratch&,u64 available,u64* used) noexcept {
    auto& p=*static_cast<Policy*>(opaque);script_work::Metered work{available,used};
    // Stage 0 starts the basic forms, stage 1 the presentation forms (after the final reordering).
    const u32 count=sfnt_glyph_count(sfnt_glyph_buffer_view(*buffer));if(!work.spend(static_cast<u64>(count)*2))return SfntStatus::resource_limit;
    auto status=sfnt_glyph_buffer_seek(buffer,count,static_cast<u64>(count)*2);if(status!=SfntStatus::ok)return status;
    Slice<SfntComponentGlyph> glyphs{reinterpret_cast<SfntComponentGlyph*>(buffer->storage.data),count};
    if(!stage){
        // Glyphs are still the reordered pieces, one each.
        p.forest=forest;if(count!=p.count)return SfntStatus::invalid_input;
        for(u32 i=0;i<count;++i){const u32 node=glyphs[i].component;if(node>=forest->nodes.count)return SfntStatus::invalid_input;
            forest->nodes[node].reserved=static_cast<u16>((forest->nodes[node].reserved&255u)|static_cast<u16>(p.metadata[i])<<8);}
        return SfntStatus::ok;
    }
    // Each syllable is one cluster; its glyphs stay together. init marks a left matra that starts the
    // syllable after the final reordering when the glyph before ends a word (HarfBuzz: its general
    // category is not Cf..Mn). A lone glyph has nothing to reorder.
    Glyphs all{*forest,glyphs,work};u32 syllable=0;
    for(u32 at=0;at<count;){const u32 first=glyphs[at].source;
        while(syllable+1<p.syllables&&p.starts[syllable+1]<=first)++syllable;
        const u32 lower=p.syllables?p.starts[syllable]:0,upper=syllable+1<p.syllables?p.starts[syllable+1]:p.units;u32 end=at+1;
        while(end<count&&glyphs[end].source>=lower&&glyphs[end].source<upper)++end;
        if(!work.spend(end-at))return SfntStatus::resource_limit;
        if(!(glyphs[at].glyph.reserved&plain_mask)){
            if(end-at>1){Glyphs s{*forest,{glyphs.data+at,end-at},work};final_reordering(p,s,end-at);if(s.status!=SfntStatus::ok)return s.status;}
            if(p.init&&all.pos(at)==P::pre_m){const u8 before=at?all.cls(at-1):Class::word_break;
                if(before==Class::word_break||before==Class::consonant_break)glyphs[at].glyph.reserved|=init_mask;}}
        if(all.status!=SfntStatus::ok)return all.status;at=end;}
    return SfntStatus::ok;
}
SfntStatus finish(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 available) noexcept {
    auto& p=*static_cast<Policy*>(opaque);if(!p.forest)return SfntStatus::invalid_input;if(zero.count<glyphs.count)return SfntStatus::output_too_small;
    Work work{available};Glyphs s{*p.forest,glyphs,work};
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];const u8 c=s.cls(i);
        // Default ignorables that did not take part in a ligature become the space glyph with zero advance
        // (HarfBuzz hide default ignorables); marks keep the font's advance. As in HarfBuzz, they keep their own
        // glyph through positioning, where GPOS can pass over joiners; indic_utf8_run replaces them afterwards.
        const bool hide=(c==Class::zwj||c==Class::zwnj||c==Class::hidden)&&!s.ligated(i);
        if(hide&&!p.space_known){const auto status=sfnt_cmap_glyph(*p.cmap,0x20,0,&p.space);if(status!=SfntStatus::ok)return status;p.space_known=true;}
        g.glyph.glyph_class=sfnt_gdef_glyph_class(*p.gdef,g.glyph.glyph);
        // Eligibility is dead after GSUB; the byte now marks hidden glyphs for run tracking.
        g.glyph.reserved=static_cast<u8>(hide);zero[i]=static_cast<u8>(hide);}
    return s.status;
}
}
// GSUB runs once over the run: the per-syllable steps run cluster by cluster (each syllable is one
// cluster) and the global ones (rvrn, rlig, calt, clig, rclt) across syllables, as in HarfBuzz.
SfntStatus indic_utf8_run(SfntIndicProfile profile,const SfntShapePlan& plan,const SfntCmap& cmap,Utf8View text,ConstSlice<u8> forms,
    float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,ConstSlice<u64> per_syllable,
    const SfntShapeLimits& limits,bool rtl,float trailing_spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    const auto* info=sfnt_indic_script(profile);
    if(!info||(text.bytes&&!text.data)||forms.count!=128||!forms.data||(steps.count&&!steps.data)||!plan.advance||!plan.units_per_em||!(em>0)||em>65536)return SfntStatus::invalid_input;
    if(!sfnt_trailing_spacing_valid(trailing_spacing))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    if((storage.advances.count&&!storage.advances.data)||(storage.targets.count&&!storage.targets.data)||(storage.flags.count&&!storage.flags.data)||
       (storage.positions.count&&!storage.positions.data)||(storage.placements.count&&!storage.placements.data))return SfntStatus::invalid_input;
    if(!storage.substitutions.replacements.data||reinterpret_cast<usize>(storage.substitutions.replacements.data)%alignof(SfntClusterGlyph))return SfntStatus::invalid_input;
    // UTF-16 units: one per scalar and one more per four-byte sequence (the reader rejects invalid text).
    u32 total=0;
    for(u32 i=0;i<text.bytes;++i){const u8 byte=static_cast<u8>(text.data[i]);total+=((byte&0xc0)!=0x80)+(byte>=0xf0);}
    Font font{profile,forms,0,info->block_first,false};
    auto status=sfnt_cmap_glyph(cmap,info->block_first+0x4d,0,&font.virama);if(status!=SfntStatus::ok)return status;
    u8 circle=2; // whether the font maps U+25CC, looked up at the first broken cluster
    {u64 any=0;for(u32 i=0;i<128;i+=8)any|=byte_load_le64(forms.data+i);font.pref=(any&0x0101010101010101ull*sfnt_deva_pre)!=0;}
    // Until substitution starts: each syllable's pieces borrow the advances, its glyph order the position
    // buffer and its masks the placements; the mapped glyphs take the replacement scratch, their
    // metadata the flags and the syllable starts (cluster boundaries) the targets.
    const Slice<IndicPiece> staging{reinterpret_cast<IndicPiece*>(storage.advances.data),storage.advances.count};
    const Slice<u32> order{reinterpret_cast<u32*>(storage.positions.data),static_cast<u32>(minimum<u64>(static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/4,0xffffffffu))};
    const Slice<u8> masks{reinterpret_cast<u8*>(storage.placements.data),static_cast<u32>(minimum<u64>(static_cast<u64>(storage.placements.count)*sizeof(SfntPlacement),0xffffffffu))};
    auto* mapped=reinterpret_cast<SfntClusterGlyph*>(storage.substitutions.replacements.data);
    const u64 capacity=minimum<u64>(minimum<u64>(storage.glyphs.count,storage.flags.count),storage.substitutions.replacements.count/sizeof(SfntClusterGlyph));
    // Classes are refreshed before the first lookup when the plan refreshes them.
    const bool classes=!plan.refresh_substitution_classes||!steps.count;
    u32 count=0,units=0,boundaries=0;
    IndicReader reader{profile,text,&cmap};
    for(;;){
        u32 length=0;IndicSyllable outcome{};
        status=indic_read_syllable(reader,staging,&length,&outcome);if(status!=SfntStatus::ok)return status;
        if(!length)break;
        // Broken clusters get a dotted circle after any leading repha (hb_syllabic_insert_dotted_circles).
        if(outcome==IndicSyllable::broken&&circle==2){u16 glyph=0;status=sfnt_cmap_glyph(cmap,0x25cc,0,&glyph);if(status!=SfntStatus::ok)return status;circle=glyph!=0;}
        if(outcome==IndicSyllable::broken&&circle){if(length==staging.count)return SfntStatus::output_too_small;
            u32 at=0;while(at<length&&staging[at].category()==C::repha)++at;
            for(u32 i=length;i>at;--i)staging[i]=staging[i-1];
            auto dotted=indic_piece(0x25cc,staging[0].source);set_position(dotted,P::end);staging[at]=dotted;++length;}
        if(count+length>capacity||length>order.count||length>masks.count)return SfntStatus::output_too_small;
        // The syllable's units run from the first unassigned unit through its last source; sources that
        // combining-class order moved across a syllable boundary map into that range. A syllable left with
        // no unit of its own joins the previous cluster.
        u32 last=units+1;for(u32 i=0;i<length;++i)last=maximum(last,staging[i].source+(staging[i].scalar()>0xffff?2u:1u));
        const bool shared=units==total;const u32 first=shared?units-1:units;last=shared?units:minimum(last,total);
        if(!shared){if(boundaries==storage.targets.count)return SfntStatus::output_too_small;storage.targets[boundaries++]=first;}
        const bool reorder=outcome==IndicSyllable::indic||outcome==IndicSyllable::broken;
        if(reorder)initial_reordering(font,{staging.data,length},{order.data,length},{masks.data,length});
        else for(u32 i=0;i<length;++i){order[i]=i;masks[i]=plain_mask;}
        for(u32 i=0;i<length;++i){const auto& piece=staging[order[i]];const u32 scalar=piece.scalar();u16 glyph=0;
            status=sfnt_cmap_glyph(cmap,rtl?unicode_bidi_mirror(scalar):scalar,0,&glyph);if(status!=SfntStatus::ok)return status;
            mapped[count+i]={{glyph,classes?sfnt_gdef_glyph_class(plan.gdef,glyph):SfntGlyphClass{},masks[order[i]]},minimum(maximum(piece.source,first),last-1)};
            storage.flags[count+i]=piece_metadata(piece,piece.position());}
        count+=length;units=last;
    }
    if(!count)return SfntStatus::ok;
    bool init=false;for(u32 i=0;i<steps.count;++i)init|=(steps[i].feature_mask&init_mask)!=0;
    Policy policy{&plan.gdef,&font,&cmap,storage.flags.data,count,storage.targets.data,boundaries,total,nullptr,init,0,false};
    const SfntShapeStagePolicy stages{&policy,prepare,2,true,per_syllable};
    status=sfnt_shape_run(plan,{mapped,count},total,{storage.targets.data,boundaries},rtl,em,storage,result,limits,steps,{},{&policy,finish},nullptr,&stages);
    if(status!=SfntStatus::ok)return status;
    // Hidden glyphs become the space glyph without horizontal offset after positioning (finish).
    if(policy.space_known)for(u32 i=0;i<result->glyph_count;++i)if(storage.glyphs[i].glyph.reserved&1){storage.glyphs[i].glyph.glyph=policy.space;storage.placements[i].advance_offset=0;}
    if(!trailing_spacing)return status;
    // Tracking advances the last glyph of each cluster in reading order, skipping hidden glyphs.
    Work budget{limits.stage_work};const u32 glyphs=result->glyph_count;
    for(u32 first=0;first<glyphs;){
        u32 end=first+1;while(end<glyphs&&!(storage.flags[end]&sfnt_position_cluster_start))++end;
        const u32 length=end-first;if(!budget.spend(static_cast<u64>(length)*2)){reset_value(result);return SfntStatus::resource_limit;}
        for(u32 offset=0;offset<length;++offset){const u32 index=rtl?first+offset:end-1-offset;
            if(storage.glyphs[index].glyph.reserved&1)continue;
            const u8 saved=storage.flags[index];storage.flags[index]&=static_cast<u8>(~sfnt_position_zero_advance);
            status=sfnt_trailing_spacing({storage.placements.data+first,length},{storage.flags.data+first,length},trailing_spacing,length,rtl);
            storage.flags[index]=saved;if(status!=SfntStatus::ok){reset_value(result);return status;}break;}
        first=end;}
    return SfntStatus::ok;
}
SfntStatus indic_owned_run(SfntIndicProfile profile,const NativeRunPlanStore& store,const NativeIndicFormPlan& prepared,Utf8View text,float em,
    const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,bool rtl,float trailing_spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(!sfnt_indic_script(profile)||prepared.indic.profile!=profile)return SfntStatus::invalid_input;
    const auto* parent=native_run_plan_get(store,prepared.indic.run);if(!parent||!prepared.indic.cmap)return SfntStatus::invalid_input;
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.indic.positioning_program,{},prepared.indic.positioning());
    plan.substitution_manual=prepared.indic.substitution_manual&&parent->substitutions?
        ConstSlice<u64>{prepared.indic.substitution_manual,(2u*parent->substitutions->lookups.count+63u)/64u}:ConstSlice<u64>{};
    return indic_utf8_run(profile,plan,*prepared.indic.cmap,text,{prepared.forms,128},em,storage,result,prepared.indic.substitutions(),
        prepared.indic.per_syllable_steps?prepared.indic.per_syllable():ConstSlice<u64>{},limits,rtl,trailing_spacing);
}
SfntStatus indic_owned_run(SfntIndicProfile profile,const NativeRunPlanStore& store,const NativeIndicFormPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,const SfntShapeLimits& limits,bool rtl,float trailing_spacing) noexcept {
    return compact_run(storage,result,[&](const SfntShapeStorage& shaping,SfntShapeRunResult* shaped) noexcept {return indic_owned_run(profile,store,prepared,text,em,shaping,shaped,limits,rtl,trailing_spacing);});
}
SfntStatus indic_form_paragraph(SfntIndicProfile profile,const NativeRunPlanStore& store,const NativeIndicFormPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!sfnt_indic_script(profile)||plan.indic.profile!=profile)return SfntStatus::invalid_input;
    return script_paragraph_shape(store,plan,plan.indic.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeIndicFormPlan& plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return indic_owned_run(plan.indic.profile,store,plan,input,size,storage,result,{},rtl,spacing);});
}
}
