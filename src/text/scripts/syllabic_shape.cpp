#include "maicord/text/scripts/syllabic_shape.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/unicode/unicode.hpp"
#include "script_work.hpp"
// The Khmer, Myanmar and Universal Shaping Engine shapers of HarfBuzz (hb-ot-shaper-khmer, -myanmar, -use) over
// one run: HarfBuzz's text preparation, syllables from the shaper's grammar (tools/syllable_machines.py), dotted
// circles, the shaper's reordering and masks, GSUB with the per-syllable steps inside each syllable, GPOS.
namespace maicord {
namespace {
#include "../unicode/generated/syllabic_categories.inc"
#include "khmer_machine.inc"
#include "myanmar_machine.inc"
#include "use_machine.inc"
using script_work::Work;
constexpr u32 nil=0xffffffffu;
// Categories, in the generated tables' order.
enum class K : u8 {x,c,v,h,zwnj,zwj,placeholder,dotted_circle,ra,vabv,vblw,vpre,vpst,robatic,xgroup,ygroup};
enum class M : u8 {x,c,iv,db,h,zwnj,zwj,sm,a,gb,dotted_circle,ra,cs,smpst,vabv,vblw,vpre,vpst,as,mh,mr,mw,my,pt,vs,ml};
enum class U : u8 {o,b,n,gb,cgj,sub,h,hn,zwnj,wj,r,cs,is,sk,g,j,sb,se,hvm,hm,hr,rk,fabv,fblw,fpst,mabv,mblw,mpst,mpre,
    cmabv,cmblw,vabv,vblw,vpst,vpre,vmabv,vmblw,vmpst,vmpre,smabv,smblw,fmabv,fmblw,fmpst};
static_assert(sizeof(khmer_column)==static_cast<u32>(K::ygroup)+1&&sizeof(myanmar_column)==static_cast<u32>(M::ml)+1&&
    sizeof(use_column)==static_cast<u32>(U::fmpst)+1);
// HarfBuzz's syllable types (the machines' outcomes).
constexpr u8 khmer_consonant=1,khmer_broken=2,myanmar_consonant=1,myanmar_broken=2;
constexpr u8 use_virama=1,use_sakot=2,use_standard=3,use_symbol=6,use_hieroglyph=7,use_broken=8,use_other=9;
// Glyph metadata (the first component's): the category, whether the character is a default ignorable HarfBuzz
// hides, and whether its syllable begins inside a grapheme (the cluster merges with the one before).
constexpr u8 category_bits=63,ignorable_bit=64,merge_bit=128;
// HarfBuzz is_default_ignorable (hb-unicode.hh).
constexpr bool default_ignorable(u32 u) noexcept {
    switch(u>>8){
    case 0x00:return u==0xad;case 0x03:return u==0x34f;case 0x06:return u==0x61c;case 0x17:return u-0x17b4u<2u;
    case 0x18:return u-0x180bu<5u;case 0x20:return u-0x200bu<5u||u-0x202au<5u||u-0x2060u<16u;
    case 0xfe:return u-0xfe00u<16u||u==0xfeff;case 0xff:return u-0xfff0u<9u;
    case 0x1d1:return u-0x1d173u<8u;default:return u-0xe0000u<0x1000u;}
}
bool unicode_mark(const UnicodeProperties& p) noexcept {
    const auto category=p.general_category();
    return category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me;
}
// One prepared character: its scalar, UTF-16 source, nominal glyph (the variation glyph of a sequence the font
// maps), category and whether it continues the grapheme before it (hb_set_unicode_props).
struct Piece {u32 scalar,source;u16 glyph;u8 category,continuation;};
static_assert(sizeof(Piece)==12);
struct Shaper {
    u8 (*category)(u32) noexcept;const u8* column;const u8* next;const u8* outcome;u8 columns;
    u8 broken,dotted,repha,stages; // repha: the category that stays before an inserted dotted circle (none: 255)
    bool split_matras,keep_split,zero_marks,use;
};
u8 khmer_of(u32 u) noexcept {return khmer_category(u);}
u8 myanmar_of(u32 u) noexcept {return myanmar_category(u);}
u8 use_of(u32 u) noexcept {return use_category(u);}
// Khmer: split matras decompose (decompose_khmer) and do not recompose, marks keep their width. Myanmar: the
// default composition. USE: split matras do not recompose; marks are zeroed by GDEF before positioning.
constexpr Shaper khmer{khmer_of,khmer_column,&khmer_next[0][0],khmer_outcome,static_cast<u8>(sizeof(khmer_next[0])),
    khmer_broken,static_cast<u8>(K::dotted_circle),255,3,true,true,false,false};
constexpr Shaper myanmar{myanmar_of,myanmar_column,&myanmar_next[0][0],myanmar_outcome,static_cast<u8>(sizeof(myanmar_next[0])),
    myanmar_broken,static_cast<u8>(M::dotted_circle),255,7,false,false,true,false};
constexpr Shaper universal{use_of,use_column,&use_next[0][0],use_outcome,static_cast<u8>(sizeof(use_next[0])),
    use_broken,static_cast<u8>(U::b),static_cast<u8>(U::r),7,false,true,true,true};
const Shaper& shaper_of(SfntSyllabicScript script) noexcept {
    return script==SfntSyllabicScript::khmer?khmer:script==SfntSyllabicScript::myanmar?myanmar:universal;
}
// HarfBuzz's Sinhala vowel constraints (hb-ot-shaper-vowel-constraints.cc): an independent vowel and a sign that
// would look like another vowel get a dotted circle between them.
bool sinhala_constrained(u32 previous,u32 u) noexcept {
    switch(previous){
    case 0xd85:return u-0xdcfu<3u;
    case 0xd8b:case 0xd8f:case 0xd94:return u==0xddf;
    case 0xd8d:return u==0xdd8;
    case 0xd91:return u==0xdca||u==0xdd9||u==0xdda||u==0xddc||u==0xddd||u==0xdde;
    default:return false;}
}
// HarfBuzz's text preparation for one run: vowel constraints, then normalization (hb-ot-shape-normalize.cc, mode
// composed-diacritics-no-short-circuit): every character decomposes fully where the font maps the parts, except in
// a base-and-marks cluster with a variation selector, where a selector joins the character before it when the font
// maps the sequence; runs of up to 32 marks are sorted by modified combining class; marks recompose with their
// starter where the font maps the composite (not onto a mark for Khmer and USE). Without a cmap every part and
// composite counts as mapped and no sequence is.
struct Text {
    const Shaper& shaper;bool sinhala;const SfntCmap* cmap;Slice<Piece> pieces;u32 count=0,units=0;bool simple=true;
    SfntStatus glyph(u32 u,u16* g) const noexcept {if(!cmap){*g=1;return SfntStatus::ok;}return sfnt_cmap_glyph(*cmap,u,0,g);}
    // Until the categories are assigned, a piece's category byte holds its modified combining class and bit 1 of
    // continuation marks a Unicode mark (which always continues its grapheme).
    SfntStatus push(u32 u,u32 source,u16 g,u8 continuation,u8 ccc=255) noexcept {
        if(count==pieces.count)return SfntStatus::output_too_small;
        if(ccc==255){const bool mark=unicode_mark(*unicode_properties(u));ccc=mark?unicode_modified_combining_class(u):0;continuation=static_cast<u8>(continuation|(mark?2:0));}
        pieces[count++]={u,source,g,ccc,continuation};return SfntStatus::ok;
    }
    // HarfBuzz decompose(): the parts of u where the font maps the second part and the first part decomposes
    // further or is mapped. *n is zero when u stays whole.
    SfntStatus decompose(u32 u,u32 source,u8 continuation,u32* n) noexcept {
        *n=0;u32 a=0,b=0;
        if(shaper.split_matras&&(u-0x17beu<3u||u-0x17c4u<2u)){a=0x17c1;b=u;}else a=unicode_canonical_decompose(u,&b);
        if(!a)return SfntStatus::ok;
        u16 gb=0,ga=0;
        if(b){const auto status=glyph(b,&gb);if(status!=SfntStatus::ok||!gb)return status;}
        auto status=glyph(a,&ga);if(status!=SfntStatus::ok)return status;
        status=decompose(a,source,continuation,n);if(status!=SfntStatus::ok)return status;
        if(!*n){if(!ga)return SfntStatus::ok;status=push(a,source,ga,continuation);if(status!=SfntStatus::ok)return status;*n=1;}
        if(b){status=push(b,source,gb,continuation);if(status!=SfntStatus::ok)return status;++*n;}
        return SfntStatus::ok;
    }
    SfntStatus read(Utf8View text) noexcept {
        u32 previous=0;bool selectors=false,joinable=false,zwj=false;
        // Variation selectors (U+180B-180F, U+FE00-FE0F, U+E0100-E01EF) start with E1 A0, EF B8 or F3 A0 in UTF-8:
        // text without those bytes skips the look-ahead for them.
        bool any_selector=false;
        for(u32 at=0;at+1<text.bytes&&!any_selector;++at){const u8 a=static_cast<u8>(text.data[at]),b=static_cast<u8>(text.data[at+1]);
            any_selector=(a==0xe1&&b==0xa0)||(a==0xef&&b==0xb8)||(a==0xf3&&b==0xa0);}
        for(u32 at=0,index=0;at<text.bytes;++index){
            const auto decoded=utf8_decode_one(text.data+at,text.bytes-at);if(!decoded.valid)return SfntStatus::invalid_input;
            const u32 u=decoded.codepoint;const auto& properties=*unicode_properties(u);const bool mark=unicode_mark(properties);
            if(index&&mark)simple=false;
            // A normalization cluster is a character and the marks after it; one with a selector is not decomposed.
            if(any_selector&&(!index||!mark)){selectors=false;joinable=false;
                for(u32 next=at+decoded.width;next<text.bytes;){const auto later=utf8_decode_one(text.data+next,text.bytes-next);
                    if(!later.valid||!unicode_mark(*unicode_properties(later.codepoint)))break;
                    selectors|=unicode_layout_variation_selector(later.codepoint);next+=later.width;}}
            // Marks, ZWJ, an extended pictographic after ZWJ, emoji modifiers, halfwidth sound marks and tags
            // continue the grapheme before them.
            const u8 continuation=static_cast<u8>((mark?3:0)|(u==0x200d||(zwj&&properties.has(UnicodeFlag::extended_pictographic))||u-0x1f3fbu<5u||
                u-0xff9eu<2u||u-0xe0020u<0x60u));
            const u8 ccc=mark?unicode_modified_combining_class(u):0;
            zwj=u==0x200d;
            if(sinhala&&previous&&sinhala_constrained(previous,u)){u16 circle=0;auto status=glyph(0x25cc,&circle);if(status!=SfntStatus::ok)return status;
                status=push(0x25cc,units,circle,static_cast<u8>(continuation&1),0);if(status!=SfntStatus::ok)return status;previous=0;}
            else previous=u;
            const bool selector=unicode_layout_variation_selector(u);
            if(selectors){
                if(selector&&joinable&&cmap){u16 g=0;const auto status=sfnt_cmap_glyph(*cmap,pieces[count-1].scalar,u,&g);
                    if(status!=SfntStatus::ok&&status!=SfntStatus::unsupported_sequence)return status;
                    if(status==SfntStatus::ok&&g){pieces[count-1].glyph=g;joinable=false;units+=u>0xffff?2u:1u;at+=decoded.width;continue;}}
                u16 g=0;auto status=glyph(u,&g);if(status!=SfntStatus::ok)return status;
                // A character other than a selector can take the selector after it; later selectors stay separate.
                status=push(u,units,g,continuation,ccc);if(status!=SfntStatus::ok)return status;joinable=!selector;
            }else{u32 n=0;auto status=decompose(u,units,static_cast<u8>(continuation&1),&n);if(status!=SfntStatus::ok)return status;
                if(!n){u16 g=0;status=glyph(u,&g);if(status!=SfntStatus::ok)return status;status=push(u,units,g,continuation,ccc);if(status!=SfntStatus::ok)return status;}}
            units+=u>0xffff?2u:1u;at+=decoded.width;
        }
        return simple?SfntStatus::ok:reorder_and_compose();
    }
    // Sorting and composition merge the clusters of the characters they move across (hb_buffer_t::sort,
    // merge_out_clusters): those characters take the lowest source among them.
    void merge(u32 first,u32 last,u32 source) noexcept {for(u32 i=first;i<=last;++i)source=minimum(source,pieces[i].source);for(u32 i=first;i<=last;++i)pieces[i].source=source;}
    SfntStatus reorder_and_compose() noexcept {
        const auto ccc=[&](u32 i) noexcept {return pieces[i].category;};
        for(u32 i=0;i<count;){if(!ccc(i)){++i;continue;}
            u32 end=i+1;while(end<count&&ccc(end))++end;
            if(end-i<=32)for(u32 k=i+1;k<end;++k){const auto piece=pieces[k];const u8 key=piece.category;u32 j=k;
                for(;j>i&&ccc(j-1)>key;--j)pieces[j]=pieces[j-1];pieces[j]=piece;if(j<k)merge(j,k,piece.source);}
            i=end;}
        u32 out=1,starter=0;
        for(u32 i=1;i<count;++i){const auto piece=pieces[i];
            if((piece.continuation&2)&&(starter==out-1||ccc(out-1)<piece.category)){
                const auto& first=pieces[starter];
                if(!(shaper.keep_split&&(first.continuation&2)))
                    if(const u32 composed=unicode_canonical_compose(first.scalar,piece.scalar)){u16 g=0;const auto status=glyph(composed,&g);
                        if(status!=SfntStatus::ok)return status;
                        if(g){pieces[starter].scalar=composed;pieces[starter].glyph=g;merge(starter,out-1,piece.source);continue;}}}
            pieces[out++]=piece;if(!piece.category)starter=out-1;}
        count=out;return SfntStatus::ok;
    }
};
// The scanner's view of the prepared characters. USE's machine skips CGJ (and ZWJ and selectors, which share its
// category) and a ZWNJ before a mark (find_syllables_use); they stay in the syllable around them.
struct Scanner {
    const Shaper& shaper;Slice<Piece> pieces;u32 count;
    bool skipped(u32 i) const noexcept {
        if(!shaper.use)return false;const auto c=static_cast<U>(pieces[i].category);if(c==U::cgj)return true;if(c!=U::zwnj)return false;
        for(u32 j=i+1;j<count;++j)if(static_cast<U>(pieces[j].category)!=U::cgj)return unicode_mark(*unicode_properties(pieces[j].scalar));
        return false;
    }
    // The longest syllable at p: its end and outcome. Characters the USE machine skips before the first syllable
    // form one of their own (HarfBuzz leaves them syllable zero, a virama-terminated cluster).
    void scan(u32 p,u32* end,u8* outcome) const noexcept {
        if(skipped(p)){u32 q=p;while(q<count&&skipped(q))++q;if(!p){*end=q;*outcome=use_virama;return;}}
        u32 state=0,last=p;u8 found=0;
        for(u32 i=p;i<count;++i){if(skipped(i))continue;
            const u8 next=shaper.next[state*shaper.columns+shaper.column[pieces[i].category]];if(!next)break;state=next;
            if(const u8 result=shaper.outcome[state]){last=i+1;found=result;}}
        if(last==p){last=p+1;found=shaper.use?use_other:3;}
        while(last<count&&skipped(last))++last;
        *end=last;*outcome=found;
    }
};
// Glyph records of the run being written, with their metadata and nominal glyphs moving together.
struct Output {
    SfntClusterGlyph* mapped;u8* meta;u16* original;
    u8 category(u32 i) const noexcept {return meta[i]&category_bits;}
    void move(u32 from,u32 to) noexcept {
        const auto record=mapped[from];const u8 m=meta[from];const u16 o=original[from];
        if(from>to)for(u32 i=from;i>to;--i){mapped[i]=mapped[i-1];meta[i]=meta[i-1];original[i]=original[i-1];}
        else for(u32 i=from;i<to;++i){mapped[i]=mapped[i+1];meta[i]=meta[i+1];original[i]=original[i+1];}
        mapped[to]=record;meta[to]=m;original[to]=o;
    }
    void reverse(u32 first,u32 end) noexcept {
        while(first+1<end){--end;const auto r=mapped[first];mapped[first]=mapped[end];mapped[end]=r;
            const u8 m=meta[first];meta[first]=meta[end];meta[end]=m;const u16 o=original[first];original[first]=original[end];original[end]=o;++first;}
    }
};
// reorder_consonant_syllable (hb-ot-shaper-khmer): masks, then Coeng,Ro and the left matra move to the start.
void khmer_reorder(Output& o,u32 start,u32 end) noexcept {
    for(u32 i=start+1;i<end;++i)o.mapped[i].glyph.reserved|=sfnt_khmer_post;
    u32 coengs=0;
    for(u32 i=start+1;i<end;++i){const auto c=static_cast<K>(o.category(i));
        if(c==K::h&&coengs<=2&&i+1<end){++coengs;
            if(static_cast<K>(o.category(i+1))==K::ra){o.mapped[i].glyph.reserved|=sfnt_khmer_pref;o.mapped[i+1].glyph.reserved|=sfnt_khmer_pref;
                o.move(i,start);o.move(i+1,start+1);
                for(u32 j=i+2;j<end;++j)o.mapped[j].glyph.reserved|=sfnt_khmer_cfar;
                coengs=2;}
        }else if(c==K::vpre)o.move(i,start);
    }
}
// initial_reordering_consonant_syllable (hb-ot-shaper-myanmar): positions, a stable sort, left matras in order.
void myanmar_reorder(Output& o,u8* position,u32 start,u32 end) noexcept {
    enum : u8 {pre_m=2,pre_c=3,base_c=4,after_main=5,before_sub=7,below_c=8,after_sub=9};
    const auto c=[&](u32 i) noexcept {return static_cast<M>(o.category(i));};
    const auto consonant=[&](u32 i) noexcept {const auto k=c(i);return k==M::c||k==M::cs||k==M::ra||k==M::iv||k==M::gb||k==M::dotted_circle;};
    u32 base=end,limit=start;const bool reph=start+3<=end&&c(start)==M::ra&&c(start+1)==M::as&&c(start+2)==M::h;
    if(reph){limit+=3;base=start;}else base=limit;
    for(u32 i=limit;i<end;++i)if(consonant(i)){base=i;break;}
    u32 i=start;
    for(;i<start+(reph?3u:0u);++i)position[i]=after_main;
    for(;i<base;++i)position[i]=pre_c;
    if(i<end)position[i++]=base_c;
    u8 p=after_main;
    for(;i<end;++i){const auto k=c(i);
        if(k==M::mr)position[i]=pre_c;
        else if(k==M::vpre)position[i]=pre_m;
        else if(k==M::vs)position[i]=position[i-1];
        else if(p==after_main&&k==M::vblw)position[i]=p=below_c;
        else if(p==below_c&&k==M::a)position[i]=before_sub;
        else if(p==below_c&&k==M::vblw)position[i]=p;
        else if(p==below_c)position[i]=p=after_sub;
        else position[i]=p;
    }
    for(u32 k=start+1;k<end;++k){const u8 key=position[k];u32 j=k;while(j>start&&position[j-1]>key)--j;
        if(j==k)continue;for(u32 m=k;m>j;--m)position[m]=position[m-1];position[j]=key;o.move(k,j);}
    u32 first=end,last=end;
    for(u32 k=start;k<end;++k)if(position[k]==pre_m){if(first==end)first=k;last=k;}
    if(first<last){o.reverse(first,last+1);u32 at=first;
        for(u32 k=first;k<=last;++k)if(c(k)==M::vpre){o.reverse(at,k+1);at=k+1;}}
}
// HarfBuzz's topographical forms of USE clusters (setup_topographical_masks).
constexpr u8 topographical=sfnt_use_isol|sfnt_use_init|sfnt_use_medi|sfnt_use_fina;
// The run's stage and glyph policies. Meta: the initial glyphs' metadata (read once by stage 0); original: their
// nominal glyphs; types: USE syllable types by initial cluster; starts: initial cluster starts; snapshot: glyph IDs
// by component node at the last substitution-flag clear (USE).
struct Policy {
    const SfntGdef* gdef;const Shaper* shaper;const u8* meta;const u16* original;const u8* types;const u32* starts;
    u16* snapshot;u32 snapshot_nodes,count,clusters,units;SfntComponentForest* forest;bool rphf,pref;u32 snapshot_used=0;
};
// The first component of a glyph carries its metadata (HarfBuzz keeps the first component's properties on a
// ligature); an expansion output left without components takes the metadata of the output before it.
struct Glyphs {
    SfntComponentForest& forest;Slice<SfntComponentGlyph> g;Work& work;SfntStatus status=SfntStatus::ok;
    u32 owner(u32 i) const noexcept {u32 j=i;while(j&&g[j].component==nil)--j;
        if(g[j].component==nil)while(j+1<g.count&&g[j].component==nil)++j;return g[j].component;}
    SfntComponentNode* first(u32 i) noexcept {u32 node=owner(i);if(node==nil)return nullptr;
        while(node<forest.nodes.count){if(!work.spend(1)){status=SfntStatus::resource_limit;return nullptr;}
            auto& n=forest.nodes[node];if((n.flags&1)||n.left==nil)return &n;node=n.left;}
        status=SfntStatus::invalid_input;return nullptr;}
    u8 meta(u32 i) noexcept {const auto* n=first(i);return n?static_cast<u8>(n->reserved>>8):0;}
    U use(u32 i) noexcept {return static_cast<U>(meta(i)&category_bits);}
    void set(u32 i,U c) noexcept {if(auto* n=first(i))n->reserved=static_cast<u16>((n->reserved&~(static_cast<u16>(category_bits)<<8))|static_cast<u16>(c)<<8);}
    bool ligated(u32 i) const noexcept {const u32 node=owner(i);return node<forest.nodes.count&&forest.nodes[node].count>=2;}
    void move(u32 from,u32 before) noexcept {if(status==SfntStatus::ok)status=script_work::move_before(g,from,before,work);}
};
// Visits each syllable's glyphs: the glyphs whose sources lie in one initial cluster.
template<class Visit> SfntStatus syllables(const Policy& p,Slice<SfntComponentGlyph> glyphs,Work& work,const Visit& visit) noexcept {
    u32 syllable=0;
    for(u32 at=0;at<glyphs.count;){const u32 first=glyphs[at].source;
        while(syllable+1<p.clusters&&p.starts[syllable+1]<=first)++syllable;
        const u32 lower=p.clusters?p.starts[syllable]:0,upper=syllable+1<p.clusters?p.starts[syllable+1]:p.units;u32 end=at+1;
        while(end<glyphs.count&&glyphs[end].source>=lower&&glyphs[end].source<upper)++end;
        if(!work.spend(end-at))return SfntStatus::resource_limit;
        const auto status=visit(at,end,syllable);if(status!=SfntStatus::ok)return status;at=end;}
    return SfntStatus::ok;
}
bool substituted(const Policy& p,const SfntComponentGlyph& g) noexcept {
    return g.component>=p.snapshot_used||p.snapshot[g.component]!=g.glyph.glyph;
}
// Records each live node's glyph; nodes made later are substitution results.
void snapshot(Policy& p,Slice<SfntComponentGlyph> glyphs) noexcept {
    p.snapshot_used=minimum(p.snapshot_nodes,static_cast<u32>(p.forest->nodes.count));
    for(u32 i=0;i<p.snapshot_used;++i)p.snapshot[i]=0xffff;
    for(u32 i=0;i<glyphs.count;++i)if(glyphs[i].component<p.snapshot_used)p.snapshot[glyphs[i].component]=glyphs[i].glyph.glyph;
}
constexpr u64 bit(U c) noexcept {return 1ull<<static_cast<u8>(c);}
constexpr u64 post_base=bit(U::fabv)|bit(U::fblw)|bit(U::fpst)|bit(U::fmabv)|bit(U::fmblw)|bit(U::fmpst)|bit(U::mabv)|bit(U::mblw)|
    bit(U::mpst)|bit(U::mpre)|bit(U::vabv)|bit(U::vblw)|bit(U::vpst)|bit(U::vpre)|bit(U::vmabv)|bit(U::vmblw)|bit(U::vmpst)|bit(U::vmpre);
// reorder_syllable_use: a repha moves before the first post-base glyph (or to the end), pre-base vowels and vowel
// modifiers move after the last halant before them (or to the start).
void use_reorder(Glyphs& s,u32 start,u32 end) noexcept {
    const auto halant=[&](u32 i) noexcept {const auto c=s.use(i);return (c==U::h||c==U::hvm||c==U::is)&&!s.ligated(i);};
    if(s.use(start)==U::r&&end-start>1)
        for(u32 i=start+1;i<end;++i){const bool post=(post_base>>static_cast<u8>(s.use(i))&1u)||halant(i);
            if(post||i==end-1){s.move(start,post?i:i+1);break;}}
    u32 j=start;
    for(u32 i=start;i<end&&s.status==SfntStatus::ok;++i){
        if(halant(i))j=i+1;
        else{const auto c=s.use(i);if((c==U::vpre||c==U::vmpre)&&s.g[i].component!=nil&&j<i)s.move(i,j);}
    }
}
SfntStatus prepare(void* opaque,u16 stage,SfntGlyphBuffer* buffer,SfntClusterMap*,SfntComponentForest* forest,const SfntGsubExecutionScratch&,u64 available,u64* used) noexcept {
    auto& p=*static_cast<Policy*>(opaque);script_work::Metered work{available,used};
    const u32 count=sfnt_glyph_count(sfnt_glyph_buffer_view(*buffer));if(!work.spend(static_cast<u64>(count)*2))return SfntStatus::resource_limit;
    auto status=sfnt_glyph_buffer_seek(buffer,count,static_cast<u64>(count)*2);if(status!=SfntStatus::ok)return status;
    Slice<SfntComponentGlyph> glyphs{reinterpret_cast<SfntComponentGlyph*>(buffer->storage.data),count};
    if(!stage){
        // Glyphs are still the mapped records, one each.
        p.forest=forest;if(count!=p.count)return SfntStatus::invalid_input;
        for(u32 i=0;i<count;++i){const u32 node=glyphs[i].component;if(node>=forest->nodes.count)return SfntStatus::invalid_input;
            forest->nodes[node].reserved=static_cast<u16>((forest->nodes[node].reserved&255u)|static_cast<u16>(p.meta[i])<<8);}
        return SfntStatus::ok;
    }
    // USE: substitution flags clear before rphf (stage 2) and pref (3); rphf and pref results are recorded after
    // them; the syllables reorder after the orthographic unit features (5).
    if(!p.shaper->use||stage<2||stage>5)return SfntStatus::ok;
    if(!work.spend(minimum(p.snapshot_nodes,static_cast<u32>(forest->nodes.count))+count))return SfntStatus::resource_limit;
    Glyphs s{*forest,glyphs,work};
    if(stage==3&&p.rphf)status=syllables(p,glyphs,work,[&](u32 start,u32 end,u32) noexcept {
        // A substituted glyph among the rphf-masked ones at the start becomes a repha.
        for(u32 i=start;i<end&&(glyphs[i].glyph.reserved&sfnt_use_rphf);++i)if(substituted(p,glyphs[i])){s.set(i,U::r);break;}
        return s.status;});
    if(stage==4&&p.pref)status=syllables(p,glyphs,work,[&](u32 start,u32 end,u32) noexcept {
        // A substituted pref glyph behaves like a pre-base vowel.
        for(u32 i=start;i<end;++i)if(substituted(p,glyphs[i])){s.set(i,U::vpre);break;}
        return s.status;});
    if(status!=SfntStatus::ok)return status;
    if((stage==2&&p.rphf)||(stage==3&&p.pref))snapshot(p,glyphs);
    if(stage==5)status=syllables(p,glyphs,work,[&](u32 start,u32 end,u32 syllable) noexcept {
        const u8 type=p.types[syllable];
        if(end-start>1&&(type==use_virama||type==use_sakot||type==use_standard||type==use_symbol||type==use_broken))use_reorder(s,start,end);
        return s.status;});
    return status;
}
// Final classes, mark zeroing and the default ignorables HarfBuzz hides: those substitution left alone keep their
// glyph through positioning (GPOS can pass over joiners) and sfnt_syllabic_run replaces them afterwards.
SfntStatus finish(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 available) noexcept {
    auto& p=*static_cast<Policy*>(opaque);if(!p.forest)return SfntStatus::invalid_input;if(zero.count<glyphs.count)return SfntStatus::output_too_small;
    Work work{available};Glyphs s{*p.forest,glyphs,work};SfntGlyphClassMemo memo;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];const u32 node=s.owner(i);
        const bool untouched=node<p.count&&p.forest->nodes[node].count==1&&g.glyph.glyph==p.original[node];
        const bool hide=untouched&&(s.meta(i)&ignorable_bit);
        if(p.gdef->glyph_classes.format)g.glyph.glyph_class=sfnt_gdef_glyph_class(*p.gdef,g.glyph.glyph,memo);
        g.glyph.reserved=static_cast<u8>(hide);zero[i]=static_cast<u8>(hide||(p.shaper->zero_marks&&g.glyph.glyph_class==SfntGlyphClass::mark));}
    return s.status;
}
// A syllable that begins inside a grapheme joins the cluster before it (HarfBuzz merges graphemes).
SfntStatus merge(void* opaque,SfntClusterMap* clusters,const SfntComponentForest& forest,Slice<SfntComponentGlyph>,u64 available) noexcept {
    const auto& p=*static_cast<const Policy*>(opaque);if(p.count>available)return SfntStatus::resource_limit;u64 left=available-p.count;
    for(u32 i=0;i<p.count;++i){if(!(forest.nodes[i].reserved>>8&merge_bit))continue;
        const u32 start=sfnt_cluster_map_start(*clusters,forest.nodes[i].source);if(!start||start==nil)continue;
        SfntClusterMerge plan{};u64 used=0;const auto status=sfnt_cluster_map_plan(*clusters,start-1,start,&plan,left,&used);if(status!=SfntStatus::ok)return status;
        left-=used;if(plan.commit_work>left)return SfntStatus::resource_limit;left-=plan.commit_work;sfnt_cluster_map_commit(clusters,plan);}
    return SfntStatus::ok;
}
}
SfntStatus sfnt_syllabic_run(SfntSyllabicScript script,const SfntShapePlan& plan,const SfntCmap& cmap,Utf8View text,float em,
    const SfntShapeStorage& storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,ConstSlice<u64> per_syllable,
    const SfntShapeLimits& limits,bool rtl) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(static_cast<u8>(script)>3||(text.bytes&&!text.data)||(steps.count&&!steps.data)||(per_syllable.count&&!per_syllable.data)||
       !plan.advance||!plan.units_per_em||!(em>0)||em>65536)return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    if((storage.advances.count&&!storage.advances.data)||(storage.targets.count&&!storage.targets.data)||(storage.flags.count&&!storage.flags.data)||
       (storage.positions.count&&!storage.positions.data)||(storage.placements.count&&!storage.placements.data)||
       (storage.character_glyphs.count&&!storage.character_glyphs.data)||(storage.glyphs.count&&!storage.glyphs.data))return SfntStatus::invalid_input;
    if(!storage.substitutions.replacements.data||reinterpret_cast<usize>(storage.substitutions.replacements.data)%alignof(SfntClusterGlyph))return SfntStatus::invalid_input;
    const auto& shaper=shaper_of(script);
    // Until substitution starts: the prepared characters borrow the positions, the mapped glyphs the replacement
    // scratch, their metadata the flags, their nominal glyphs the advances, the syllable starts the targets, USE's
    // syllable types the character map and Myanmar's reordering positions the placements.
    const Slice<Piece> pieces{reinterpret_cast<Piece*>(storage.positions.data),
        static_cast<u32>(minimum<u64>(static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(Piece),0xffffffffu))};
    Text prepared{shaper,script==SfntSyllabicScript::sinhala,&cmap,pieces};
    auto status=prepared.read(text);if(status!=SfntStatus::ok)return status;
    const u32 n=prepared.count,total=prepared.units;if(!n)return SfntStatus::ok;
    const bool classes=plan.gdef.glyph_classes.format!=0;
    for(u32 i=0;i<n;++i)pieces[i].category=shaper.category(pieces[i].scalar);
    auto* mapped=reinterpret_cast<SfntClusterGlyph*>(storage.substitutions.replacements.data);
    const u64 capacity=minimum<u64>(minimum<u64>(minimum<u64>(storage.glyphs.count,storage.flags.count),storage.substitutions.replacements.count/sizeof(SfntClusterGlyph)),
        static_cast<u64>(storage.advances.count)*sizeof(double)/sizeof(u16));
    Output out{mapped,storage.flags.data,reinterpret_cast<u16*>(storage.advances.data)};
    u8* types=reinterpret_cast<u8*>(storage.character_glyphs.data);const u64 type_capacity=static_cast<u64>(storage.character_glyphs.count)*sizeof(u32);
    u8* positions=reinterpret_cast<u8*>(storage.placements.data);const u64 position_capacity=static_cast<u64>(storage.placements.count)*sizeof(SfntPlacement);
    u16 circle=0;bool circle_known=false;SfntGlyphClassMemo memo;
    const auto class_of=[&](const Piece& piece) noexcept {
        // Without GDEF glyph classes a nonspacing mark is a mark (hb_synthesize_glyph_classes).
        if(classes)return sfnt_gdef_glyph_class(plan.gdef,piece.glyph,memo);
        return unicode_properties(piece.scalar)->general_category()==UnicodeGeneralCategory::mn&&!default_ignorable(piece.scalar)?SfntGlyphClass::mark:SfntGlyphClass::base;
    };
    const Scanner scanner{shaper,pieces,n};
    u32 count=0,boundaries=0,last_start=0;u8 last_form=0; // last_form: 0 none, else a topographical mask
    for(u32 p=0;p<n;){u32 q=0;u8 type=0;scanner.scan(p,&q,&type);
        // Broken clusters get a dotted circle after any leading repha (hb_syllabic_insert_dotted_circles).
        bool dotted=false;u32 at=p;
        if(type==shaper.broken){
            if(!circle_known){status=sfnt_cmap_glyph(cmap,0x25cc,0,&circle);if(status!=SfntStatus::ok)return status;circle_known=true;}
            dotted=circle!=0;if(shaper.repha!=255)while(at<q&&pieces[at].category==shaper.repha)++at;}
        const u32 length=q-p+dotted;if(count+length>capacity||count+length>position_capacity)return SfntStatus::output_too_small;
        // The syllable's cluster starts at its lowest source (HarfBuzz clusters; sorting and composition merged the
        // sources they moved across). A syllable that starts at or before the previous one joins its cluster.
        u32 first=pieces[p].source;for(u32 i=p+1;i<q;++i)first=minimum(first,pieces[i].source);if(!boundaries)first=0;
        if(!boundaries||first>storage.targets[boundaries-1]){
            if(boundaries==storage.targets.count||(shaper.use&&boundaries==type_capacity))return SfntStatus::output_too_small;
            if(shaper.use)types[boundaries]=type;storage.targets[boundaries++]=first;}
        const u32 floor=storage.targets[boundaries-1];
        // USE's rphf mask: the first three characters, or the repha alone (setup_rphf_mask); a dotted circle takes
        // the mask of the character after it.
        const u32 rphf=shaper.use?(static_cast<U>(pieces[p].category)==U::r?1u:minimum(3u,q-p)):0;
        const u32 start=count;
        for(u32 i=p;i<=q;++i){
            if(dotted&&i==at){const Piece piece{0x25cc,i<q?pieces[i].source:pieces[q-1].source,circle,shaper.dotted,0};
                out.mapped[count]={{circle,class_of(piece),static_cast<u8>(i<p+rphf?sfnt_use_rphf:0)},maximum(piece.source,floor)};
                out.meta[count]=shaper.dotted;out.original[count++]=circle;}
            if(i==q)break;const auto& piece=pieces[i];
            out.mapped[count]={{piece.glyph,class_of(piece),static_cast<u8>(i<p+rphf?sfnt_use_rphf:0)},maximum(piece.source,floor)};
            out.meta[count]=static_cast<u8>(piece.category|(default_ignorable(piece.scalar)?ignorable_bit:0));out.original[count++]=piece.glyph;
        }
        if(pieces[p].continuation)out.meta[start]|=merge_bit;
        if(&shaper==&khmer){if(type==khmer_consonant||type==khmer_broken)khmer_reorder(out,start,count);}
        else if(&shaper==&myanmar){if(type==myanmar_consonant||type==myanmar_broken)myanmar_reorder(out,positions,start,count);}
        else if(type==use_hieroglyph||type==use_other)last_form=0;
        else{
            // A cluster after an isolated or final one joins it: that one becomes initial or medial, this one final.
            const bool join=last_form==sfnt_use_fina||last_form==sfnt_use_isol;
            if(join){last_form=last_form==sfnt_use_fina?sfnt_use_medi:sfnt_use_init;
                for(u32 i=last_start;i<start;++i)out.mapped[i].glyph.reserved=static_cast<u8>((out.mapped[i].glyph.reserved&~topographical)|last_form);}
            last_form=join?sfnt_use_fina:sfnt_use_isol;
            for(u32 i=start;i<count;++i)out.mapped[i].glyph.reserved=static_cast<u8>(out.mapped[i].glyph.reserved|last_form);
        }
        last_start=start;p=q;
    }
    bool rphf=false,pref=false;for(u32 i=0;i<steps.count;++i){rphf|=steps[i].stage==2;pref|=steps[i].stage==3;}
    // USE's substitution snapshots borrow the positions (the prepared characters are dead) by component node.
    const u32 nodes=static_cast<u32>(minimum<u64>(storage.components.count,static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(u16)));
    if(shaper.use&&(rphf||pref)&&nodes<storage.components.count)return SfntStatus::output_too_small;
    Policy policy{&plan.gdef,&shaper,storage.flags.data,out.original,types,storage.targets.data,reinterpret_cast<u16*>(storage.positions.data),
        nodes,count,boundaries,total,nullptr,shaper.use&&rphf,shaper.use&&pref};
    const SfntShapeStagePolicy stages{&policy,prepare,shaper.stages,true,per_syllable};
    // Marks are zeroed before GPOS (HB_OT_SHAPE_ZERO_WIDTH_MARKS_BY_GDEF_EARLY), so advances GPOS gives them stay.
    auto positioned=plan;positioned.retain_zero_positioning=true;
    status=sfnt_shape_run(positioned,{mapped,count},total,{storage.targets.data,boundaries},rtl,em,storage,result,limits,steps,{},{&policy,finish,merge},nullptr,&stages);
    if(status!=SfntStatus::ok)return status;
    // Hidden default ignorables become the space glyph with nothing to place (hb_ot_hide_default_ignorables).
    u16 space=0;bool space_known=false;
    for(u32 i=0;i<result->glyph_count;++i)if(storage.glyphs[i].glyph.reserved&1){
        if(!space_known){status=sfnt_cmap_glyph(cmap,0x20,0,&space);if(status!=SfntStatus::ok){reset_value(result);return status;}space_known=true;}
        // HarfBuzz zeroes the advance and the horizontal offset (hb_ot_zero_width_default_ignorables).
        storage.glyphs[i].glyph.glyph=space;storage.placements[i].advance=0;storage.placements[i].advance_offset=0;}
    return SfntStatus::ok;
}
SfntStatus sfnt_syllabic_boundaries(SfntSyllabicScript script,ConstSlice<u32> scalars,Slice<u8> output,u64 work) noexcept {
    if(static_cast<u8>(script)>3||(scalars.count&&!scalars.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    if(output.count<scalars.count)return SfntStatus::output_too_small;
    if(static_cast<u64>(scalars.count)*16>work)return SfntStatus::resource_limit;
    if(byte_ranges_overlap_or_wrap(scalars.data,static_cast<u64>(scalars.count)*4,output.data,scalars.count))return SfntStatus::invalid_input;
    // Up to 256 scalars: at most four UTF-8 bytes each, and three pieces plus a circle.
    if(scalars.count>256)return SfntStatus::resource_limit;
    char text[1024];u32 bytes=0;
    for(u32 i=0;i<scalars.count;++i){if(!unicode_is_scalar(scalars[i]))return SfntStatus::invalid_input;bytes+=utf8_encode(scalars[i],text+bytes);}
    // Map UTF-16 sources back to scalar indices.
    u32 index[512];for(u32 i=0,units=0;i<scalars.count;++i){index[units++]=i;if(scalars[i]>0xffff)index[units++]=i;}
    const auto& shaper=shaper_of(script);Piece pieces[1024];u8 starts[256]{};
    // Without a cmap every decomposition and composition applies, as with a font that maps every character.
    Text prepared{shaper,script==SfntSyllabicScript::sinhala,nullptr,{pieces,1024}};
    const auto status=prepared.read({text,bytes});if(status!=SfntStatus::ok)return status;
    for(u32 i=0;i<prepared.count;++i)pieces[i].category=shaper.category(pieces[i].scalar);
    const Scanner scanner{shaper,{pieces,prepared.count},prepared.count};
    // A syllable starts a cluster unless it begins inside a grapheme or before the previous cluster's start.
    for(u32 p=0,last=0;p<prepared.count;){u32 q=0;u8 type=0;scanner.scan(p,&q,&type);
        u32 first=pieces[p].source;for(u32 i=p+1;i<q;++i)first=minimum(first,pieces[i].source);
        if(!p)starts[0]=1;else if(first>last){last=first;starts[index[first]]=!pieces[p].continuation;}p=q;}
    copy_bytes(output.data,starts,scalars.count);return SfntStatus::ok;
}
}
