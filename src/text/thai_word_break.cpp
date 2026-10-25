#include "maicord/text/detail/thai_lexical.hpp"
#include "maicord/text/detail/thai_syllable.hpp"
#include "maicord/text/unicode.hpp"

namespace maicord {namespace {
// Maximal matching: a Thai run is divided into lexicon words and, where no
// word fits, syllables of the authored grammar. The division with the fewest
// unknown syllables, then the fewest segments, wins; on a tie the longer first
// segment does. Runs longer than one window are searched one window at a time
// and committed up to its middle, far beyond the longest word or syllable.
constexpr u32 window=256;

bool thai_context(u32 cp) noexcept {
    return cp-0xe01u<91&&unicode_properties(cp)->line_break()==UnicodeLineBreak::sa;
}
bool consonant(u32 cp) noexcept {return cp-0xe01u<46;}
// PAIYANNOI and MAIYAMOK belong to the preceding word.
bool attaching(u32 cp) noexcept {return cp==0xe2f||cp==0xe46;}
u32 attached_end(ConstSlice<u32> text,u32 at) noexcept {
    while(at<text.count&&!native_thai_boundary_class(text[at]))++at;
    return at;
}

// Unknown text divides into syllables. An open syllable may or may not take
// the next consonant as its final, so both lengths are offered. Where the
// grammar recognizes no syllable, one cluster is: a leading vowel with its
// consonant, then any attached marks, optionally with a final consonant.
struct UnknownUnits {u32 lengths[2];u32 count;};
UnknownUnits unknown_units(ConstSlice<u32> text,u32 first) noexcept {
    UnknownUnits units{};
    const auto add=[&](u32 length) noexcept {
        length=minimum(length,text.count-first);
        for(u32 i=1;i<length;++i)if(attaching(text[first+i])){length=i;break;}
        if(length&&(!units.count||units.lengths[0]!=length))units.lengths[units.count++]=length;
    };
    const i32 syllable=native_thai_syllable_result(text,first);
    if(syllable){
        if(const u32 recovery=native_thai_unknown_recovery(text,first,syllable))add(recovery-1);
        if(syllable<0)add(attached_end(text,first+static_cast<u32>(-syllable)-1)-first);
        if(units.count)return units;
    }
    u32 end=first+1;
    if(text[first]-0xe40u<5&&end<text.count&&consonant(text[end]))++end;
    end=attached_end(text,end);
    add(end-first);
    if(end<text.count&&consonant(text[end])&&(end+1==text.count||native_thai_boundary_class(text[end+1])))add(end+1-first);
    return units;
}

enum class Segment : u8 {word,unknown,orphan};
struct Choice {u32 cost;u8 length;Segment kind;};
// Segments starting at one position: words longest first, then unknown units.
struct Edges {u8 lengths[10];u8 count,words;Segment unknown;bool reached,attached;};
// Costs count unknown syllables above segments; an attaching mark costs nothing.
constexpr u32 word_cost=1,unknown_cost=0x10001;

Edges edges_at(ConstSlice<u32> text,u32 first) noexcept {
    Edges edges{};edges.reached=true;
    if(attaching(text[first])){edges.lengths[0]=1;edges.count=edges.words=1;edges.attached=true;return edges;}
    for(i32 length:native_thai_lexical_candidates(text,first).lengths)
        if(length>0)edges.lengths[edges.count++]=static_cast<u8>(length);
    edges.words=edges.count;
    edges.unknown=native_thai_boundary_class(text[first])?Segment::unknown:Segment::orphan;
    const auto units=unknown_units(text,first);
    for(u32 u=0;u<units.count;++u){
        bool word=false;for(u32 w=0;w<edges.words;++w)word=word||edges.lengths[w]==units.lengths[u];
        if(!word)edges.lengths[edges.count++]=static_cast<u8>(units.lengths[u]);
    }
    return edges;
}

// Fills choice[i] with the best division of text[i..count) for every position
// a division from 0 reaches; the others are never read.
void search(ConstSlice<u32> text,Edges* edges,Choice* choice) noexcept {
    for(u32 i=0;i<text.count;++i)edges[i].reached=false;
    edges[0].reached=true;
    for(u32 i=0;i<text.count;++i)if(edges[i].reached){
        edges[i]=edges_at(text,i);
        for(u32 e=0;e<edges[i].count;++e)if(i+edges[i].lengths[e]<text.count)edges[i+edges[i].lengths[e]].reached=true;
    }
    choice[text.count]={0,0,Segment::word};
    for(u32 i=text.count;i--;)if(edges[i].reached){
        const Edges& from=edges[i];Choice best{0xffffffffu,0,Segment::word};
        for(u32 e=0;e<from.count;++e){
            const bool word=e<from.words;
            const u32 cost=choice[i+from.lengths[e]].cost+(from.attached?0:word?word_cost:unknown_cost);
            if(cost<best.cost)best={cost,from.lengths[e],word?Segment::word:from.unknown};
        }
        choice[i]=best;
    }
}

// A break follows every segment except an orphan mark, which joins the next
// segment, and never precedes an attaching mark.
void divide(ConstSlice<u32> run,u8* breaks) noexcept {
    Edges edges[window];Choice choice[window+1];
    for(u32 first=0;first<run.count;){
        const u32 count=minimum(run.count-first,window);
        search({run.data+first,count},edges,choice);
        const u32 commit=first+count==run.count?count:window/2;
        u32 at=0;
        do{
            const Choice step=choice[at];at+=step.length;
            if(at<count&&step.kind!=Segment::orphan&&!attaching(run[first+at]))breaks[first+at]=1;
        }while(at<commit);
        first+=at;
    }
}
}
bool native_thai_word_breaks(ConstSlice<u32> text,Slice<u8> breaks) noexcept {
    if((text.count&&!text.data)||!breaks.data||breaks.count<=text.count)return false;
    for(u32 at=0;at<text.count;){
        if(!thai_context(text[at])){++at;continue;}
        const u32 first=at;
        while(at<text.count&&thai_context(text[at]))++at;
        divide({text.data+first,at-first},breaks.data+first);
    }
    return true;
}
}
