#include "maicord/text/scripts/latin_run.hpp"
#include "maicord/core/memory.hpp"
#include "../opentype/nominal_placements.hpp"
#include "maicord/text/shaping/alphabetic_run.hpp"
#include "maicord/text/opentype/lookup_plan.hpp"
#include "maicord/text/scripts/syllabic_shape.hpp"
#include "maicord/text/scripts/emoji_run.hpp"
#include "maicord/text/opentype/nominal_sources.hpp"
#include "maicord/text/unicode/grapheme.hpp"
#include "maicord/sys/simd.hpp"
namespace maicord {namespace {
bool unicode_mark(u32 cp) noexcept {const auto category=unicode_properties(cp)->general_category();
    return category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me;}
// Hebrew presentation forms HarfBuzz composes for a font without GPOS mark positioning (hb-ot-shaper-hebrew.cc).
u32 hebrew_presentation(u32 first,u32 second) noexcept {
    static constexpr u16 dagesh[]{0xfb30,0xfb31,0xfb32,0xfb33,0xfb34,0xfb35,0xfb36,0,0xfb38,0xfb39,0xfb3a,0xfb3b,0xfb3c,0,0xfb3e,0,0xfb40,
        0xfb41,0,0xfb43,0xfb44,0,0xfb46,0xfb47,0xfb48,0xfb49,0xfb4a};
    switch(second){
        case 0x5b4:return first==0x5d9?0xfb1d:0;
        case 0x5b7:return first==0x5f2?0xfb1f:first==0x5d0?0xfb2e:0;
        case 0x5b8:return first==0x5d0?0xfb2f:0;
        case 0x5b9:return first==0x5d5?0xfb4b:0;
        case 0x5bc:return first-0x5d0u<27?dagesh[first-0x5d0]:first==0xfb2a?0xfb2c:first==0xfb2b?0xfb2d:0;
        case 0x5bf:return first==0x5d1?0xfb4c:first==0x5db?0xfb4d:first==0x5e4?0xfb4e:0;
        case 0x5c1:return first==0x5e9?0xfb2a:first==0xfb49?0xfb2c:0;
        case 0x5c2:return first==0x5e9?0xfb2b:first==0xfb49?0xfb2d:0;
        default:return 0;
    }
}
// HarfBuzz's decompose(): the parts of cp where the font maps the second part and the first part decomposes
// further or is mapped (shortest: a mapped first part stops). *n is 0 when cp stays whole.
template<class Present> SfntStatus decompose_scalar(u32 cp,bool shortest,u32* parts,u32* n,const Present& present) noexcept {
    *n=0;u32 second=0;const u32 first=unicode_canonical_decompose(cp,&second);if(!first)return SfntStatus::ok;bool mapped=false;
    if(second){const auto status=present(second,&mapped);if(status!=SfntStatus::ok||!mapped)return status;}
    bool has_first=false;auto status=present(first,&has_first);if(status!=SfntStatus::ok)return status;
    if(!(shortest&&has_first)){status=decompose_scalar(first,shortest,parts,n,present);if(status!=SfntStatus::ok)return status;}
    if(!*n){if(!has_first)return SfntStatus::ok;parts[(*n)++]=first;}
    if(second)parts[(*n)++]=second;return SfntStatus::ok;
}
// HarfBuzz normalization (hb-ot-shape-normalize.cc, composed-diacritics mode) of a run's scalars in place. A character
// before no mark keeps its mapped glyph or takes its shortest mapped decomposition; a base with its marks decomposes
// fully where the font maps the parts, unless a variation selector is among them. Runs of up to 32 marks are then
// sorted by modified combining class (with the Hebrew shaper's swap), and marks recompose with their starter where
// the font maps the composite. Entries keep their sources in order, so moved and composed marks stay in their
// base's cluster. present(scalar,&mapped) answers whether the font maps a scalar.
template<class Present> SfntStatus normalize(u32* scalars,SfntClusterGlyph* entries,u32& count,u32 capacity,bool hebrew,bool hebrew_forms,const Present& present) noexcept {
    // The glyph class field holds each character's decomposition mode until mapping: 0 none, 1 shortest, 2 full.
    bool simple=true;u32 parts[8];
    for(u32 i=0;i<count;){u32 end=i+1;while(end<count&&!unicode_mark(scalars[end]))++end;if(end<count)--end;
        for(;i<end;++i)entries[i].glyph.glyph_class=static_cast<SfntGlyphClass>(1);
        if(i==count)break;simple=false;end=i+1;while(end<count&&unicode_mark(scalars[end]))++end;
        bool selector=false;for(u32 j=i;j<end;++j)selector|=entries[j].glyph.glyph!=0||unicode_layout_variation_selector(scalars[j]);
        for(;i<end;++i)entries[i].glyph.glyph_class=static_cast<SfntGlyphClass>(selector?0:2);}
    const auto expand=[&](u32 i,u32* n) noexcept {*n=0;const u8 mode=static_cast<u8>(entries[i].glyph.glyph_class);if(!mode)return SfntStatus::ok;
        if(mode==1){bool mapped=false;const auto status=present(scalars[i],&mapped);if(status!=SfntStatus::ok||mapped)return status;}
        return decompose_scalar(scalars[i],mode==1,parts,n,present);};
    u32 total=0;for(u32 i=0;i<count;++i){u32 n=0;const auto status=expand(i,&n);if(status!=SfntStatus::ok)return status;total+=n?n:1;}
    if(total>capacity)return SfntStatus::output_too_small;
    // Decomposition only lengthens, so writing from the end never overtakes an unread character.
    if(total!=count){for(u32 i=count,write=total;i--;){u32 n=0;const auto status=expand(i,&n);if(status!=SfntStatus::ok)return status;
            const auto entry=entries[i];if(!n)parts[n++]=scalars[i];write-=n;
            for(u32 k=0;k<n;++k){scalars[write+k]=parts[k];entries[write+k]=entry;if(k)entries[write+k].glyph.glyph=0;}}
        count=total;}
    if(simple)return SfntStatus::ok;
    for(u32 i=0;i<count;){if(!unicode_modified_combining_class(scalars[i])){++i;continue;}
        u32 end=i+1;while(end<count&&unicode_modified_combining_class(scalars[end]))++end;
        if(end-i<=32){const u32 n=end-i;u8 keys[32];u32 sources[32];
            for(u32 k=0;k<n;++k){keys[k]=unicode_modified_combining_class(scalars[i+k]);sources[k]=entries[i+k].source;}
            for(u32 k=1;k<n;++k){const u8 key=keys[k];const u32 scalar=scalars[i+k];const auto entry=entries[i+k];u32 j=k;
                for(;j&&keys[j-1]>key;--j){keys[j]=keys[j-1];scalars[i+j]=scalars[i+j-1];entries[i+j]=entries[i+j-1];}
                keys[j]=key;scalars[i+j]=scalar;entries[i+j]=entry;}
            // Patah or qamats, sheva or hiriq, then meteg or a below mark: the last two swap (reorder_marks_hebrew).
            if(hebrew)for(u32 k=2;k<n;++k)if((keys[k-2]==20||keys[k-2]==21)&&(keys[k-1]==22||keys[k-1]==23)&&(keys[k]==25||keys[k]==220)){
                const u32 scalar=scalars[i+k];const auto entry=entries[i+k];scalars[i+k]=scalars[i+k-1];entries[i+k]=entries[i+k-1];
                scalars[i+k-1]=scalar;entries[i+k-1]=entry;break;}
            for(u32 k=0;k<n;++k)entries[i+k].source=sources[k];}
        i=end;}
    u32 starter=0,out=1;
    for(u32 i=1;i<count;++i){const u32 scalar=scalars[i];
        if(unicode_mark(scalar)&&(starter==out-1||unicode_modified_combining_class(scalars[out-1])<unicode_modified_combining_class(scalar))){
            u32 composed=unicode_canonical_compose(scalars[starter],scalar);if(!composed&&hebrew_forms)composed=hebrew_presentation(scalars[starter],scalar);
            bool mapped=false;if(composed){const auto status=present(composed,&mapped);if(status!=SfntStatus::ok)return status;}
            if(mapped){scalars[starter]=composed;entries[starter].glyph.glyph=0;continue;}}
        scalars[out]=scalar;entries[out]=entries[i];if(!unicode_modified_combining_class(scalar))starter=out;++out;}
    count=out;return SfntStatus::ok;
}
// A selector joins its base only when the font maps the sequence (cmap format 14, default or not); otherwise it stays
// a separate default-ignorable glyph, hidden like any other (hb-ot-shape-normalize.cc, handle_variation_selector_cluster).
bool selects(const SfntCmap& cmap,u32 scalar,u32 selector) noexcept {u16 glyph=0;return sfnt_cmap_glyph(cmap,scalar,selector,&glyph)==SfntStatus::ok&&glyph;}
// Per source unit: kind << 16 | nominal glyph; kind 1 is a default ignorable. The table shares the shaping
// advances, so it is gone once positioning starts.
// hidden: finish_controls marked a hidden control (reserved bit 0) for alphabetic_run to replace after positioning.
// keep_marks: the shaper zeroes no mark advances (Hangul, HB_OT_SHAPE_ZERO_WIDTH_MARKS_NONE).
struct Controls {const SfntGdef* gdef;const u32* sources;u32 units;u16 space;bool expansion_attachments;mutable bool hidden=false;bool keep_marks=false;};
// Preserve the origin of inserted dotted circles in existing component padding.
// A missing synthetic base is permitted; a missing user character is not.
SfntStatus remember_synthetic_bases(void*,SfntGlyphBuffer* buffer,SfntClusterMap*,SfntComponentForest* forest,const SfntGsubExecutionScratch&,u64 work) noexcept {
    const auto view=sfnt_glyph_buffer_view(*buffer);const u32 count=sfnt_glyph_count(view);
    if(count>work)return SfntStatus::resource_limit;if(count!=forest->nodes.count)return SfntStatus::invalid_input;
    for(u32 i=0;i<count;++i){auto& glyph=const_cast<SfntGsubGlyph&>(sfnt_glyph_at(view,i));
        forest->nodes[i].reserved=static_cast<u16>((glyph.reserved&1)?0x100:0);glyph.reserved=static_cast<u8>(glyph.reserved&~1u);}
    return SfntStatus::ok;
}
// HarfBuzz hides the default ignorables that substitution left alone (hb_ot_hide_default_ignorables;
// _hb_glyph_info_is_default_ignorable excludes substituted glyphs), after positioning: a hidden glyph keeps its
// own glyph through GPOS (which can pass over joiners), marked by reserved bit 0, and becomes the space glyph once
// the run is shaped (alphabetic_run).
SfntStatus finish_controls(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    const auto& p=*static_cast<const Controls*>(opaque);if(glyphs.count>work)return SfntStatus::resource_limit;if(zero.count<glyphs.count)return SfntStatus::output_too_small;
    u32 previous=0xffffffffu;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];if(g.source>=p.units)return SfntStatus::invalid_input;
        const u32 kind=p.sources?p.sources[g.source]:0;const bool control=kind>>16==1&&g.glyph.glyph==static_cast<u16>(kind);
        g.glyph.reserved=static_cast<u8>(control);if(control)p.hidden=true;
        if(control||p.gdef->glyph_classes.format)g.glyph.glyph_class=sfnt_gdef_glyph_class(*p.gdef,g.glyph.glyph);
        const bool mark=!control&&g.glyph.glyph_class==SfntGlyphClass::mark;
        if(p.expansion_attachments&&mark&&g.component==0xffffffffu)g.attachment=previous;
        if(!mark)previous=g.component;zero[i]=static_cast<u8>(control||(mark&&!p.keep_marks));}
    return SfntStatus::ok;
}
struct EgyptianControls {Controls common;u16 glyphs[4];};
// USE suppresses matching control glyph IDs, including absent mappings to zero.
SfntStatus finish_egyptian_controls(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    auto& p=*static_cast<EgyptianControls*>(opaque);if(glyphs.count>work/2)return SfntStatus::resource_limit;
    const auto status=finish_controls(&p.common,glyphs,zero,work-glyphs.count);if(status!=SfntStatus::ok)return status;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];const auto id=g.glyph.glyph;
        if(id==p.glyphs[0]||id==p.glyphs[1]||id==p.glyphs[2]||id==p.glyphs[3]){
            g.glyph.glyph=p.common.space;g.glyph.glyph_class=sfnt_gdef_glyph_class(*p.common.gdef,p.common.space);zero[i]=1;}}
    return SfntStatus::ok;
}
// HarfBuzz's Hangul text preprocessing (hb-ot-shaper-hangul.cc, preprocess_text_hangul) over the decoded characters:
// conjoining jamo compose into the syllable the font maps; a syllable the font lacks, or one without a trailing
// consonant before a trailing jamo, decomposes into jamo the font maps; a tone mark after a syllable moves before it
// unless the font draws it with zero width, and one without a syllable gets a dotted circle after it (before it when
// zero width). The jamo of an uncomposed syllable carry their ljmo, vjmo or tjmo mask and the syllable is one
// cluster. Entries keep the selector code in the glyph field and metadata (bit 0: synthetic base) in reserved; the
// cluster starts are rebuilt in place. out holds the rewritten characters until they replace the input; its start
// is 1 at a cluster start, plus 2 on a tone mark's dotted circle.
struct HangulEntry {u32 scalar,source;u16 selector;u8 reserved,start;};
static_assert(sizeof(HangulEntry)==12);
constexpr bool hangul_l(u32 u) noexcept {return u-0x1100u<0x60u||u-0xa960u<0x1du;}
constexpr bool hangul_v(u32 u) noexcept {return u-0x1160u<0x48u||u-0xd7b0u<0x17u;}
constexpr bool hangul_t(u32 u) noexcept {return u-0x11a8u<0x58u||u-0xd7cbu<0x31u;}
// L, V and T together: U+1100-11FF, U+A960-A97C, U+D7B0-D7C6 and U+D7CB-D7FB.
__forceinline bool hangul_jamo(u32 u) noexcept {return u-0x1100u<0x100u||u-0xa960u<0x1du||u-0xd7b0u<0x17u||u-0xd7cbu<0x31u;}
constexpr bool hangul_filler(u32 u) noexcept {return u==0x115f||u==0x1160||u==0x3164||u==0xffa0;}
template<class Has,class Zero> SfntStatus hangul_preprocess(u32* scalars,SfntClusterGlyph* entries,u32& count,u32 capacity,Slice<u32> starts,u32& start_count,
    Slice<HangulEntry> out,const Has& has,const Zero& zero_width,u16 circle,bool* synthetic) noexcept {
    u32 written=0,next=0,first=0,end=0;
    const auto starts_at=[&](u32 source) noexcept {while(next<start_count&&starts[next]<source)++next;return static_cast<u8>(next<start_count&&starts[next]==source);};
    const auto push=[&](u32 scalar,u32 source,u16 selector,u8 reserved,u8 start) noexcept {if(written==out.count)return false;out[written++]={scalar,source,selector,reserved,start};return true;};
    for(u32 i=0;i<count;){
        const u32 u=scalars[i];const auto& e=entries[i];const u8 start=starts_at(e.source);
        if(u==0x302e||u==0x302f){bool zero=false;const auto status=zero_width(u,&zero);if(status!=SfntStatus::ok)return status;
            if(first<end&&end==written){if(!push(u,e.source,e.glyph.glyph,e.glyph.reserved,0))return SfntStatus::output_too_small;
                // The tone mark moves to the syllable's front and takes its cluster start.
                if(!zero){const auto tone=out[written-1];for(u32 k=written-1;k>first;--k)out[k]=out[k-1];out[first]=tone;out[first].start=out[first+1].start;out[first+1].start=0;}}
            else if(circle){*synthetic=true;
                // The circle has the tone mark's properties (start bit 1), as HarfBuzz copies them.
                if(!(zero?push(0x25cc,e.source,0,1,static_cast<u8>(start|2))&&push(u,e.source,e.glyph.glyph,e.glyph.reserved,0):
                    push(u,e.source,e.glyph.glyph,e.glyph.reserved,start)&&push(0x25cc,e.source,0,1,2)))return SfntStatus::output_too_small;}
            else if(!push(u,e.source,e.glyph.glyph,e.glyph.reserved,start))return SfntStatus::output_too_small;
            first=end=written;++i;continue;}
        first=written;
        if(hangul_l(u)&&i+1<count&&hangul_v(scalars[i+1])){
            const u32 v=scalars[i+1],t=i+2<count&&hangul_t(scalars[i+2])?scalars[i+2]:0,l_index=u-0x1100,v_index=v-0x1161,t_index=t?t-0x11a7:0;
            if(l_index<19&&v_index<21&&(!t||t_index-1<27)){bool mapped=false;const u32 s=0xac00+(l_index*21+v_index)*28+t_index;const auto status=has(s,&mapped);
                if(status!=SfntStatus::ok)return status;
                if(mapped){if(!push(s,e.source,0,0,start))return SfntStatus::output_too_small;i+=t?3:2;end=first+1;continue;}}
            if(!push(u,e.source,e.glyph.glyph,sfnt_hangul_ljmo,start)||!push(v,entries[i+1].source,entries[i+1].glyph.glyph,sfnt_hangul_vjmo,0)||
               (t&&!push(t,entries[i+2].source,entries[i+2].glyph.glyph,sfnt_hangul_tjmo,0)))return SfntStatus::output_too_small;
            i+=t?3:2;end=written;continue;}
        if(u-0xac00u<11172u){bool mapped=false;auto status=has(u,&mapped);if(status!=SfntStatus::ok)return status;
            const u32 index=u-0xac00,t_index=index%28,following=i+1<count?scalars[i+1]:0;
            if(!t_index&&following-0x11a8u<27u){bool composed=false;status=has(u+following-0x11a7,&composed);if(status!=SfntStatus::ok)return status;
                if(composed){if(!push(u+following-0x11a7,e.source,0,0,start))return SfntStatus::output_too_small;i+=2;end=first+1;continue;}}
            if(!mapped||(!t_index&&hangul_t(following))){
                const u32 l=0x1100+index/588,v=0x1161+index%588/28,t=t_index?0x11a7+t_index:0;bool ml=false,mv=false,mt=true;
                status=has(l,&ml);if(status==SfntStatus::ok)status=has(v,&mv);if(status==SfntStatus::ok&&t)status=has(t,&mt);if(status!=SfntStatus::ok)return status;
                if(ml&&mv&&mt){
                    if(!push(l,e.source,0,sfnt_hangul_ljmo,start)||!push(v,e.source,0,sfnt_hangul_vjmo,0)||(t&&!push(t,e.source,0,sfnt_hangul_tjmo,0)))return SfntStatus::output_too_small;
                    // A mapped syllable decomposed for the trailing jamo after it takes that jamo along.
                    if(mapped&&!t_index){if(!push(following,entries[i+1].source,entries[i+1].glyph.glyph,sfnt_hangul_tjmo,0))return SfntStatus::output_too_small;++i;}
                    ++i;end=written;continue;}}
            if(mapped)end=first+1;}
        if(!push(u,e.source,e.glyph.glyph,e.glyph.reserved,hangul_jamo(u)?1:start))return SfntStatus::output_too_small;++i;}
    if(written>capacity)return SfntStatus::output_too_small;
    // Normalization then reorders marks by combining class even in mode NONE: each run of non-zero classes sorts
    // stably (runs over 32 stay) and the characters it moves across join the cluster of the first of them
    // (merge_clusters), which also takes the rest of the moved character's cluster.
    const auto ccc=[&](u32 k) noexcept -> u8 {return out[k].start&2?unicode_modified_combining_class(0x302e):unicode_modified_combining_class(out[k].scalar);};
    for(u32 i=0;i<written;){if(!ccc(i)){++i;continue;}u32 end=i+1;while(end<written&&ccc(end))++end;
        if(end-i<=32)for(u32 k=i+1;k<end;++k){const u8 key=ccc(k);u32 j=k;while(j>i&&ccc(j-1)>key)--j;if(j==k)continue;
            const auto moved=out[k];const u8 start=out[j].start&1;
            for(u32 m=k;m>j;--m){out[m]=out[m-1];out[m].start&=2;}
            out[j]=moved;out[j].start=static_cast<u8>((moved.start&2)|start);}
        i=end;}
    // A cluster starts at the lowest source of each run of entries before the next start, unless an earlier one covers it.
    start_count=0;
    for(u32 i=0;i<written;){u32 j=i+1,source=out[i].source;while(j<written&&!(out[j].start&1)){source=minimum(source,out[j].source);++j;}
        if(!start_count||source>starts[start_count-1]){if(start_count==starts.count)return SfntStatus::output_too_small;starts[start_count++]=source;}
        for(u32 k=i;k<j;++k){scalars[k]=out[k].scalar;entries[k]={{out[k].selector,SfntGlyphClass{},out[k].reserved},out[k].source};}i=j;}
    count=written;return SfntStatus::ok;
}
SfntStatus alphabetic_run(const NativeRunPlanStore& store,NativeRunPlanHandle handle,Utf8View text,bool independent,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,SfntShapeLimits limits,bool require_source_coverage,const NativeAlphabeticPlan* profile,bool force_shaping,bool rtl=false,SfntShapeRunResult* raw_output=nullptr,bool paragraph_start=true,bool hangul_pass=false) noexcept {
    if(!out)return SfntStatus::invalid_input;reset_value(out);u32 em_bits=0;copy_bytes(&em_bits,&em,4);
    const auto* plan=native_run_plan_get(store,handle,independent);
    if(!plan||(text.bytes&&!text.data)||!em_bits||em_bits>=0x7f800000u||em>65536||
       (s.mapped.count&&!s.mapped.data)||(s.shaping.components.count&&!s.shaping.components.data)||(s.shaping.targets.count&&!s.shaping.targets.data)||
       (s.glyphs.count&&!s.glyphs.data)||(s.placements.count&&!s.placements.data)||(s.character_glyphs.count&&!s.character_glyphs.data))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    const auto* instance=native_font_instance_get(*store.instances,store.slots[handle.slot-1].instance);if(!instance)return SfntStatus::invalid_input;
    const auto* font=native_font_get(*store.instances->fonts,instance->face);if(!font)return SfntStatus::invalid_input;
    auto* scalars=reinterpret_cast<u32*>(s.shaping.components.data);const u64 capacity=static_cast<u64>(s.shaping.components.count)*sizeof(SfntComponentNode)/sizeof(u32);
    const bool korean=profile&&profile->script_tag==sfnt_tag('h','a','n','g');
    const bool egyptian=profile&&profile->script_tag==sfnt_tag('e','g','y','p');
    const bool thai=profile&&(profile->script_tag==sfnt_tag('t','h','a','i')||profile->script_tag==sfnt_tag('l','a','o',' '));
    bool sara_am=false,marks=false;
    // Temporary input metadata lives in the mapped glyph padding. It is cleared before GSUB except for the Hangul
    // feature masks; only synthetic-base provenance survives in component nodes.
    u32 count=0,units=0,boundary_count=0;bool controls=false,hangul=hangul_pass,synthetic_bases=false;UnicodeGraphemeState graphemes{};
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return SfntStatus::invalid_input;
        if(count>=capacity||count>=s.mapped.count)return SfntStatus::output_too_small;
        bool boundary=unicode_grapheme_boundary(graphemes,cp.codepoint);
        // Thai and Lao clusters follow HarfBuzz: marks, ZWJ and Sara Am (decomposed into Nikhahit and Sara Aa) continue
        // the previous cluster, and no sequence is broken (hb-ot-shaper-thai.cc, preprocess_text_thai).
        if(thai){const auto category=unicode_properties(cp.codepoint)->general_category();
            boundary=!count||!(category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me||
                cp.codepoint==0xe33||cp.codepoint==0xeb3||cp.codepoint==0x200d);}
        // ZWNJ starts its own shape cluster: HarfBuzz marks only Unicode marks, ZWJ, emoji modifiers, halfwidth kana
        // sound marks and tags as cluster continuations (hb-ot-shape.cc, hb_set_unicode_props), so it merges the
        // halfwidth sound marks U+FF9E/FF9F and conjoining Jamo (Hangul syllable clusters, UAX #29 GB6-GB8) with
        // their grapheme like any other extender. Shaping policy only; grapheme/editor boundaries are separate.
        if(cp.codepoint==0x200c)boundary=true;else if(cp.codepoint==0x200d)boundary|=egyptian;
        if(boundary){if(boundary_count==s.shaping.targets.count)return SfntStatus::output_too_small;s.shaping.targets[boundary_count++]=units;}
        // Unicode 17 selectors begin with E1, EF or F3 in UTF-8. Ordinary
        // text avoids a second decode/property lookup; the property still
        // validates the complete candidate before consuming a selector.
        const u8 lead=at+cp.width<text.bytes?static_cast<u8>(text.data[at+cp.width]):0;
        const auto next=(lead==0xe1||lead==0xef||lead==0xf3)?utf8_decode_one(text.data+at+cp.width,text.bytes-at-cp.width):Utf8DecodeResult{};
        const bool variation=next.valid&&unicode_layout_variation_selector(next.codepoint)&&!unicode_properties(cp.codepoint)->has(UnicodeFlag::variation_selector)&&
            selects(font->cmap,rtl?unicode_bidi_mirror(cp.codepoint):cp.codepoint,next.codepoint);
        // The not-yet-mapped glyph field temporarily holds the selector. Its
        // compact code covers both BMP and supplementary selectors without
        // adding storage. UTF-16 sources include both consumed scalars.
        // A mark that begins its paragraph follows a dotted circle when the font maps one (HarfBuzz HB_BUFFER_FLAG_BOT,
        // hb_insert_dotted_circle). A default-ignorable mark (a selector, CGJ) gets none: the client stays invisible
        // there like the browser it matches.
        if(!count&&paragraph_start){const auto* properties=unicode_properties(cp.codepoint);const auto category=properties->general_category();
            if((category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me)&&!properties->has(UnicodeFlag::default_ignorable_code_point)){
                u16 circle=0;const auto status=sfnt_cmap_glyph(font->cmap,0x25cc,0,&circle);if(status!=SfntStatus::ok)return status;
                if(circle){if(count+1>=capacity||count+1>=s.mapped.count)return SfntStatus::output_too_small;
                    scalars[count]=0x25cc;s.mapped[count].source=units;s.mapped[count].glyph.reserved=1;s.mapped[count++].glyph.glyph=0;synthetic_bases=true;}}}
        const bool am=thai&&(cp.codepoint==0x0e33||cp.codepoint==0x0eb3);
        if(am){if(count+1>=capacity||count+1>=s.mapped.count)return SfntStatus::output_too_small;
            if(require_source_coverage){u16 original=0;const auto status=sfnt_cmap_glyph(font->cmap,cp.codepoint,0,&original);if(status!=SfntStatus::ok)return status;if(!original)return SfntStatus::unsupported_sequence;}
            scalars[count]=cp.codepoint==0x0e33?0x0e4d:0x0ecd;s.mapped[count++]={{0,SfntGlyphClass{},2},units};sara_am=true;}
        scalars[count]=am?cp.codepoint-1:cp.codepoint;s.mapped[count].source=units;if(korean||thai)s.mapped[count].glyph.reserved=0;
        if(thai){const u32 cpmark=cp.codepoint&~0x80u;if(cpmark==0xe31||(cpmark>=0xe34&&cpmark<=0xe37)||(cpmark>=0xe47&&cpmark<=0xe4e)||cp.codepoint==0xebb)s.mapped[count].glyph.reserved=4;}
        s.mapped[count].glyph.glyph=variation?static_cast<u16>(next.codepoint>65535?next.codepoint-0xe0000:next.codepoint):0;++count;
        units+=(cp.codepoint>65535?2u:1u)+(variation?(next.codepoint>65535?2u:1u):0u);at+=cp.width+(variation?next.width:0u);
        if(variation)unicode_grapheme_boundary(graphemes,next.codepoint);
        // HarfBuzz renders the Hangul fillers (is_default_ignorable leaves them out).
        if(cp.codepoint>=0xad&&!(korean&&hangul_filler(cp.codepoint)))controls|=unicode_properties(cp.codepoint)->has(UnicodeFlag::default_ignorable_code_point);
        hangul|=korean&&hangul_jamo(cp.codepoint);marks|=cp.codepoint>=0x300&&unicode_mark(cp.codepoint);
    }
    // Marks (the tone marks among them) take the preprocessing too: it also reorders them.
    hangul|=korean&&marks;
    if(units>s.character_glyphs.count)return SfntStatus::output_too_small;if(!count)return SfntStatus::ok;
    auto* control_sources=reinterpret_cast<u32*>(s.shaping.advances.data);
    if(controls){if(!control_sources||static_cast<u64>(s.shaping.advances.count)*sizeof(double)<static_cast<u64>(units)*sizeof(u32))return SfntStatus::output_too_small;for(u32 i=0;i<units;++i)control_sources[i]=0;}
    const bool hebrew=profile&&profile->script_tag==sfnt_tag('h','e','b','r');
    const auto nominal=native_run_plan_nominal(store,handle);if(!nominal.glyph_limit)return SfntStatus::invalid_input;bool simple=!independent&&!force_shaping&&!marks;SfntGlyphClassMemo classes;
    // Reuse the plan's validated affine ASCII cmap prefix inside Latin text.
    // Accented scalars and fonts without that prefix keep ordinary cmap lookup.
    const auto map=[&](u32 scalar,u16* glyph,u32 selector=0) noexcept {
        if(rtl)scalar=unicode_bidi_mirror(scalar);
        if(selector&&selector<0x200)selector+=0xe0000;
        const u32 offset=scalar-32u;if(!selector&&offset<nominal.ascii_count){*glyph=static_cast<u16>(nominal.ascii_first+offset);return SfntStatus::ok;}
        const auto status=sfnt_cmap_glyph(font->cmap,scalar,selector,glyph);
        return status==SfntStatus::unsupported_sequence&&selector?sfnt_cmap_glyph(font->cmap,scalar,0,glyph):status;
    };
    // Jamo, tone marks and syllables the font lacks take hb-ot-shaper-hangul's preprocessing; the moved tone marks
    // and merged syllables leave the nominal path.
    if(hangul){simple=false;
        const auto has=[&](u32 scalar,bool* mapped) noexcept {u16 glyph=0;const auto status=map(scalar,&glyph);*mapped=glyph!=0;return status;};
        const auto zero_width=[&](u32 scalar,bool* zero) noexcept {u16 glyph=0;auto status=map(scalar,&glyph);*zero=false;if(status!=SfntStatus::ok||!glyph)return status;
            double advance=0;status=plan->advance(plan->metric_context,glyph,&advance);*zero=advance==0;return status;};
        u16 circle=0;auto status=sfnt_cmap_glyph(font->cmap,0x25cc,0,&circle);if(status!=SfntStatus::ok)return status;
        // The mapped input may borrow the positions; the shaping glyphs stay free until substitution starts.
        if(s.shaping.glyphs.count&&!s.shaping.glyphs.data)return SfntStatus::invalid_input;
        const Slice<HangulEntry> entries{reinterpret_cast<HangulEntry*>(s.shaping.glyphs.data),
            static_cast<u32>(minimum<u64>(static_cast<u64>(s.shaping.glyphs.count)*sizeof(SfntComponentGlyph)/sizeof(HangulEntry),0xffffffffu))};
        status=hangul_preprocess(scalars,s.mapped.data,count,static_cast<u32>(minimum<u64>(capacity,s.mapped.count)),s.shaping.targets,boundary_count,
            entries,has,zero_width,circle,&synthetic_bases);
        if(status!=SfntStatus::ok)return status;}
    // HarfBuzz also decomposes a precomposed character the font does not map (hb-ot-shape-normalize.cc,
    // decompose_current_character), so such a character needs normalization even without marks.
    if(!marks&&!korean)for(u32 j=0;j<count&&!marks;++j){const u32 cp=scalars[j];u32 second=0;if(cp<0xc0||!unicode_canonical_decompose(cp,&second))continue;
        u16 glyph=0;const auto status=map(cp,&glyph,s.mapped[j].glyph.glyph);if(status!=SfntStatus::ok)return status;marks=!glyph;simple=simple&&glyph;}
    // The Hangul shaper does not normalize (hb-ot-shaper-hangul.cc composes its own syllables).
    if(marks&&!korean){const auto present=[&](u32 scalar,bool* mapped) noexcept {u16 glyph=0;const auto status=map(scalar,&glyph);*mapped=glyph!=0;return status;};
        const auto status=normalize(scalars,s.mapped.data,count,static_cast<u32>(minimum<u64>(capacity,s.mapped.count)),hebrew,hebrew&&!profile->mark_positioning,present);
        if(status!=SfntStatus::ok)return status;}
    for(u32 j=0;j<count;++j){const u32 cp=scalars[j];const u32 source=s.mapped[j].source;
        // Jamo keep their own masks; every other character takes calt (hb-ot-shaper-hangul, setup_masks_hangul).
        const u8 metadata=static_cast<u8>(korean?s.mapped[j].glyph.reserved|(hangul_jamo(cp)?0:sfnt_hangul_calt):thai?s.mapped[j].glyph.reserved:0);
        u16 glyph=0;const auto status=map(cp,&glyph,s.mapped[j].glyph.glyph);if(status!=SfntStatus::ok)return status;
        // A syllable the font lacks may decompose into jamo it maps: shape again with the preprocessing.
        if(korean&&!hangul&&!glyph&&cp-0xac00u<11172u)
            return alphabetic_run(store,handle,text,independent,em,s,out,limits,require_source_coverage,profile,force_shaping,rtl,raw_output,paragraph_start,true);
        const u32 flags=cp>=0xad?unicode_properties(cp)->flags():0;
        constexpr u32 selector=static_cast<u32>(UnicodeFlag::variation_selector),ignorable=static_cast<u32>(UnicodeFlag::default_ignorable_code_point);
        if(require_source_coverage&&!glyph&&!(metadata&1)&&!(flags&selector))return SfntStatus::unsupported_sequence;
        const bool control=(flags&ignorable)&&!(korean&&hangul_filler(cp));if(controls&&control)control_sources[source]=1u<<16|glyph;
        simple=simple&&!control&&glyph&&nominal_scalar_is_simple(cp)&&(!nominal.participation.count||!(nominal.participation[glyph/64]&(1ull<<(glyph%64))));
        // Without GDEF glyph classes HarfBuzz takes a nonspacing mark for a mark (hb_synthesize_glyph_classes).
        const bool synthesized=marks&&!plan->gdef.glyph_classes.format&&cp>=0x300&&unicode_properties(cp)->general_category()==UnicodeGeneralCategory::mn;
        s.mapped[j]={{glyph,synthesized?SfntGlyphClass::mark:sfnt_gdef_glyph_class(plan->gdef,glyph,classes),metadata},source};
    }
    const u32 written=count;
    if(simple){if(written>s.glyphs.count||written>s.placements.count)return SfntStatus::output_too_small;
        const auto scale=_mm_div_ss(_mm_set_ss(em),_mm_set_ss(static_cast<float>(plan->units_per_em)));
        if(plan->cache_advances){const auto status=nominal_placements(*plan,{s.mapped.data,written},s.glyphs,s.placements,s.shaping.targets,scale);if(status!=SfntStatus::ok)return status;
            for(u32 i=0;i<written;++i){const auto& g=s.mapped[i];
                const u32 end=i+1<written?s.mapped[i+1].source:units;for(u32 source=g.source;source<end;++source)s.character_glyphs[source]=i;}
        }else for(u32 i=0;i<written;++i){const auto& g=s.mapped[i];double advance=0;const auto status=nominal_advance(nominal,*plan,g.glyph.glyph,&advance);if(status!=SfntStatus::ok)return status;
            s.glyphs[i]=g.glyph.glyph;s.placements[i]={_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(static_cast<float>(advance)),scale)),0,0};
            const u32 end=i+1<written?s.mapped[i+1].source:units;for(u32 source=g.source;source<end;++source)s.character_glyphs[source]=i;}
        *out={written,units,1,0,0};return SfntStatus::ok;
    }
    const bool extended=profile&&profile->script_tag!=sfnt_tag('l','a','t','n');
    ConstSlice<u32> input_order{};
    // Sara Am contributes a nikhahit before preceding above-base marks (hb-ot-shaper-thai.cc); original source
    // indices stay intact. Reuse the existing input-permutation scratch.
    if(sara_am){if(s.shaping.character_glyphs.count<written)return SfntStatus::output_too_small;auto* order=s.shaping.character_glyphs.data;u64 work=limits.stage_work;
        if(written>work)return SfntStatus::resource_limit;work-=written;
#pragma clang loop vectorize(disable) interleave(disable)
        for(u32 i=0;i<written;++i)order[i]=i;
        for(u32 i=0;i<written;++i)if(s.mapped[i].glyph.reserved&2){u32 position=i;
            while(position&&(s.mapped[order[position-1]].glyph.reserved&4)){if(!work)return SfntStatus::resource_limit;--work;order[position]=order[position-1];--position;}order[position]=i;}
        input_order={order,written};}
    Controls policy{&plan->gdef,controls?control_sources:nullptr,units,0,extended};if(controls||egyptian){const auto status=sfnt_cmap_glyph(font->cmap,32,0,&policy.space);if(status!=SfntStatus::ok)return status;}
    policy.keep_marks=korean;
    EgyptianControls hieroglyphs;
    if(egyptian){hieroglyphs.common=policy;for(u32 i=0;i<4;++i){const auto status=sfnt_cmap_glyph(font->cmap,0x200c+i,0,hieroglyphs.glyphs+i);if(status!=SfntStatus::ok)return status;}}
    auto latin=*plan;latin.refresh_substitution_classes=true;latin.position_instance.round_variable_anchors=true;
    if(profile){if(profile->positioning_program)latin.positioning=profile->positioning_program;latin.substitution_roots=profile->substitution_roots;latin.positioning_roots=profile->positioning_roots;}
    latin.retain_zero_positioning=false;
    const SfntShapeInputPolicy initial{nullptr,remember_synthetic_bases};
    // Only the preprocessing gives jamo their ljmo, vjmo or tjmo mask: without it a plan of only those features
    // (Malgun Gothic's hang GSUB) has nothing to act on and runs no GSUB.
    ConstSlice<SfntLookupStep> steps=korean?profile->substitution_steps:ConstSlice<SfntLookupStep>{};
    if(korean&&!hangul){bool jamo_only=true;constexpr u8 jamo=sfnt_hangul_ljmo|sfnt_hangul_vjmo|sfnt_hangul_tjmo;
        for(u32 i=0;i<steps.count&&jamo_only;++i)jamo_only=steps[i].feature_mask&&!(steps[i].feature_mask&~jamo);
        if(jamo_only)steps={};}
    SfntShapeRunResult result{};const auto status=sfnt_shape_run(latin,{s.mapped.data,written},units,{s.shaping.targets.data,boundary_count},rtl,em,s.shaping,&result,limits,
        steps,input_order,
        egyptian?SfntShapeGlyphPolicy{&hieroglyphs,finish_egyptian_controls}:controls||extended?SfntShapeGlyphPolicy{&policy,finish_controls}:SfntShapeGlyphPolicy{},synthetic_bases||thai?&initial:nullptr);
    out->lookup_operations=result.lookup_operations;if(status!=SfntStatus::ok)return status;
    // Hidden controls become the space glyph without horizontal offset after positioning (finish_controls).
    if(const auto& controlled=egyptian?hieroglyphs.common:policy;controlled.hidden)
        for(u32 i=0;i<result.glyph_count;++i)if(s.shaping.glyphs[i].glyph.reserved&1){s.shaping.glyphs[i].glyph.glyph=controlled.space;s.shaping.placements[i].advance_offset=0;}
    if(require_source_coverage){u32 byte=0,source=0;u64 work=limits.stage_work;
        if(result.glyph_count>work)return SfntStatus::resource_limit;work-=result.glyph_count;
        for(u32 i=0;i<result.glyph_count;++i)if(!s.shaping.glyphs[i].glyph.glyph){
            const u32 component=s.shaping.glyphs[i].component;
            if(synthetic_bases&&component<written&&s.shaping.components[component].count==1&&s.shaping.components[component].reserved==0x100)continue;
            const u32 origin=s.shaping.glyphs[i].source;if(origin<source){byte=source=0;}
            while(source<origin&&byte<text.bytes){if(!work)return SfntStatus::resource_limit;--work;const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);source+=cp.codepoint>65535?2u:1u;byte+=cp.width;}
            const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);
            if(source!=origin||!cp.valid||!unicode_properties(cp.codepoint)->has(UnicodeFlag::variation_selector))return SfntStatus::unsupported_sequence;
        }
    }
    if(raw_output){*raw_output=result;return SfntStatus::ok;}
    if(result.glyph_count>s.glyphs.count||result.glyph_count>s.placements.count)return SfntStatus::output_too_small;
    if(s.character_glyphs.data!=s.shaping.character_glyphs.data)copy_bytes(s.character_glyphs.data,s.shaping.character_glyphs.data,static_cast<usize>(units)*sizeof(u32));
    if(s.placements.data!=s.shaping.placements.data)copy_bytes(s.placements.data,s.shaping.placements.data,static_cast<usize>(result.glyph_count)*sizeof(SfntPlacement));
    for(u32 i=0;i<result.glyph_count;++i)s.glyphs[i]=s.shaping.glyphs[i].glyph.glyph;
    *out={result.glyph_count,units,0,1,result.lookup_operations};return SfntStatus::ok;
}
}
SfntStatus native_latin_shape_run(const NativeRunPlanStore& store,NativeRunPlanHandle handle,Utf8View text,bool independent,bool rtl,float em,const SfntShapeStorage& storage,SfntShapeRunResult* out,SfntShapeLimits limits) noexcept {
    if(!out)return SfntStatus::invalid_input;reset_value(out);
    const u64 capacity=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntClusterGlyph);
    NativeAsciiStorage s{{reinterpret_cast<SfntClusterGlyph*>(storage.positions.data),static_cast<u32>(minimum(capacity,u64{0xffffffffu}))},storage,{},storage.placements,storage.character_glyphs};
    // Arabic's generic fallback is not told its paragraph position; it inserts no paragraph-initial circle.
    NativeAsciiResult compact{};return alphabetic_run(store,handle,text,independent,em,s,&compact,limits,false,nullptr,true,rtl,out,false);
}
SfntStatus native_latin_run(const NativeRunPlanStore& store,NativeRunPlanHandle handle,Utf8View text,bool independent,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,SfntShapeLimits limits,bool coverage,bool force_shaping,bool paragraph_start) noexcept {
    return alphabetic_run(store,handle,text,independent,em,s,out,limits,coverage,nullptr,force_shaping,false,nullptr,paragraph_start);
}
SfntStatus native_alphabetic_run(const NativeRunPlanStore& store,const NativeAlphabeticPlan& profile,Utf8View text,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,SfntShapeLimits limits,bool coverage,bool force_shaping,bool rtl,bool paragraph_start) noexcept {
    if(!profile.script_tag){
        if(out)reset_value(out);const auto* plan=native_run_plan_get(store,profile.run,profile.independent);if(!plan)return SfntStatus::invalid_input;
        const auto* instance=native_font_instance_get(*store.instances,store.slots[profile.run.slot-1].instance);
        const auto* font=instance?native_font_get(*store.instances->fonts,instance->face):nullptr;if(!font)return SfntStatus::invalid_input;
        NativeEmojiPlan common{profile.run,&font->cmap,profile.substitution_roots,profile.positioning_roots,0,profile.positioning_program};
        const auto status=sfnt_cmap_glyph(font->cmap,32,0,&common.space_glyph);if(status!=SfntStatus::ok)return status;
        const auto result=native_emoji_run(store,common,text,em,s,out,limits,coverage,rtl,false,paragraph_start);
        // Proved-inert glyphs can bypass lookup execution while source/script
        // policy still requires shaped (sequential) paragraph accumulation.
        if(result==SfntStatus::ok&&out&&force_shaping&&out->nominal_ranges){out->nominal_ranges=0;out->shaped_ranges=1;}
        return result;
    }
    if((profile.script_tag!=sfnt_tag('l','a','t','n')&&!sfnt_alphabetic_script_supported(profile.script_tag))||
        (profile.substitution_roots.count&&!profile.substitution_roots.data)||(profile.positioning_roots.count&&!profile.positioning_roots.data)){if(out)reset_value(out);return SfntStatus::invalid_input;}
    return alphabetic_run(store,profile.run,text,profile.independent,em,s,out,limits,coverage,&profile,force_shaping,rtl,nullptr,paragraph_start);
}
}
