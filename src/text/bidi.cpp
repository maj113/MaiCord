#include "bidi_internal.hpp"

namespace maicord {
namespace {
using B=UnicodeBidiClass;
constexpr u32 none=~0u;
bool isolate(B type) noexcept {return type==B::lri || type==B::rli || type==B::fsi;}
bool embedding(B type) noexcept {return type==B::lre || type==B::rle || type==B::lro || type==B::rlo;}
bool removed(B type) noexcept {return embedding(type) || type==B::pdf || type==B::bn;}
bool neutral(B type) noexcept {return type==B::b || type==B::s || type==B::ws || type==B::on || isolate(type) || type==B::pdi;}
B direction(u32 level) noexcept {return level&1?B::r:B::l;}
B strong(B type) noexcept {return type==B::en || type==B::an?B::r:type;}
bool space(B type) noexcept {return type==B::ws || isolate(type) || type==B::pdi;}
B analysis_type(B type,bool windows) noexcept {return windows && (isolate(type) || type==B::pdi)?B::bn:type;}

void match_isolates(Slice<UnicodeBidiCell> cells,bool windows) noexcept {
    u32 top=none;
    for(u32 i=0;i<cells.count;++i) {
        auto& cell=cells[i];const B type=analysis_type(cell.original,windows);
        cell.link=none;cell.next=none;cell.resolved=type;cell.explicit_level=0;cell.level=unicode_bidi_removed;
        if(isolate(type)){cell.link=top;top=i;}
        else if(type==B::pdi && top!=none) {
            const u32 previous=cells[top].link;cells[top].link=i;cell.link=top;top=previous;
        }
    }
    while(top!=none){const u32 previous=cells[top].link;cells[top].link=none;top=previous;}
}
u8 first_direction(ConstSlice<UnicodeBidiCell> cells,u32 first,u32 end,u8 fallback,bool windows) noexcept {
    for(u32 i=first;i<end;++i) {
        const auto& cell=cells[i];const B type=analysis_type(cell.original,windows);
        if(type==B::l)return 0;
        if(type==B::r || type==B::al)return 1;
        if(isolate(type)) {if(cell.link==none)return fallback;i=cell.link;}
    }
    return fallback;
}
void explicit_levels(Slice<UnicodeBidiCell> cells,u8 base,bool windows) noexcept {
    struct Status {u8 level;B override_type;bool isolate;};
    Status stack[126];u32 depth=1,overflow_isolates=0,overflow_embeddings=0,valid_isolates=0;
    stack[0]={base,B::on,false};
    for(u32 i=0;i<cells.count;++i) {
        auto& cell=cells[i];const B type=analysis_type(cell.original,windows);auto current=stack[depth-1];
        cell.explicit_level=current.level;
        if(embedding(type)) {
            const bool rtl=type==B::rle || type==B::rlo;
            const u32 next=rtl?((current.level+1u)|1u):((current.level+2u)&~1u);
            if(next<=125 && !overflow_isolates && !overflow_embeddings) {
                stack[depth++]={static_cast<u8>(next),type==B::rlo?B::r:type==B::lro?B::l:B::on,false};
            } else if(!overflow_isolates)++overflow_embeddings;
        } else if(isolate(type)) {
            if(current.override_type!=B::on)cell.resolved=current.override_type;
            const bool rtl=type==B::rli || (type==B::fsi && first_direction({cells.data,cells.count},i+1,cell.link==none?cells.count:cell.link,0,windows));
            const u32 next=rtl?((current.level+1u)|1u):((current.level+2u)&~1u);
            if(next<=125 && !overflow_isolates && !overflow_embeddings) {
                ++valid_isolates;stack[depth++]={static_cast<u8>(next),B::on,true};
            } else ++overflow_isolates;
        } else if(type==B::pdi) {
            if(overflow_isolates)--overflow_isolates;
            else if(valid_isolates) {
                overflow_embeddings=0;
                while(!stack[depth-1].isolate)--depth;
                --depth;--valid_isolates;
            }
            current=stack[depth-1];cell.explicit_level=current.level;
            if(current.override_type!=B::on)cell.resolved=current.override_type;
        } else if(type==B::pdf) {
            if(!overflow_isolates) {
                if(overflow_embeddings)--overflow_embeddings;
                else if(depth>1 && !current.isolate)--depth;
            }
        } else if(type==B::b)cell.explicit_level=base;
        else if(type!=B::bn && current.override_type!=B::on)cell.resolved=current.override_type;
        if(!removed(type))cell.level=cell.explicit_level;
    }
}
void windows_weak_tail(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order,B sos) noexcept {
    // Windows numeric context survives intervening weak/neutral characters until
    // another number or strong character replaces it. ET runs and W7 are resolved
    // together so no extra per-cell flags, buffer or backward repair scan is needed.
    B strong=sos,previous=sos;bool number_an=false,pending_cs=false;
    for(u32 i=0;i<order.count;){auto& cell=cells[order[i]];const B type=cell.resolved;
        if(type==B::et){const u32 first=i;while(i<order.count&&cells[order[i]].resolved==B::et)++i;
            const bool special=(strong==B::l&&number_an)||pending_cs;if(pending_cs)strong=B::l;
            const bool convert=previous==B::en||(i<order.count&&cells[order[i]].resolved==B::en);
            const B value=convert?(strong==B::l?B::l:B::en):B::on;
            for(u32 j=first;j<i;++j)cells[order[j]].resolved=value;
            // Windows leaves the first terminator neutral for these numeric
            // contexts only when the converted run has multiple elements.
            if(convert&&special&&i-first>1)cells[order[first]].resolved=B::on;
            previous=B::et;pending_cs=false;continue;
        }
        pending_cs=type==B::cs&&cell.original!=B::nsm&&previous==B::an&&strong!=B::al;
        if(type==B::l||type==B::r||type==B::al){strong=type;number_an=false;if(type==B::al)cell.resolved=B::r;}
        else if(type==B::an||type==B::en){number_an=type==B::an;if(type==B::en&&strong==B::l)cell.resolved=B::l;}
        else if(type==B::es||type==B::cs)cell.resolved=B::on;
        previous=type;++i;
    }
}
void weak_types(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order,B sos,bool windows) noexcept {
    B previous=sos;
    for(u32 at=0;at<order.count;++at) {
        const u32 index=order[at];
        auto& type=cells[index].resolved;
        if(type==B::nsm)type=isolate(previous) || previous==B::pdi?B::on:previous;
        previous=type;
    }
    previous=sos;
    for(u32 at=0;at<order.count;++at) {
        const u32 index=order[at];
        auto& type=cells[index].resolved;
        if(type==B::en && previous==B::al)type=B::an;
        if(type==B::l || type==B::r || type==B::al)previous=type;
    }
    if(!windows)for(u32 at=0;at<order.count;++at)if(cells[order[at]].resolved==B::al)cells[order[at]].resolved=B::r;
    for(u32 i=1;i+1<order.count;++i) {
        auto& type=cells[order[i]].resolved;const B left=cells[order[i-1]].resolved,right=cells[order[i+1]].resolved;
        if(type==B::es && left==B::en && right==B::en)type=B::en;
        else if(type==B::cs && left==right && (left==B::en || left==B::an))type=left;
    }
    if(windows){windows_weak_tail(cells,order,sos);return;}
    for(u32 i=0;i<order.count;) {
        if(cells[order[i]].resolved!=B::et){++i;continue;}
        const u32 first=i;while(i<order.count && cells[order[i]].resolved==B::et)++i;
        if((first && cells[order[first-1]].resolved==B::en) || (i<order.count && cells[order[i]].resolved==B::en))
            for(u32 j=first;j<i;++j)cells[order[j]].resolved=B::en;
    }
    for(u32 at=0;at<order.count;++at) {
        const u32 index=order[at];
        auto& type=cells[index].resolved;if(type==B::es || type==B::et || type==B::cs)type=B::on;
    }
    previous=sos;
    for(u32 at=0;at<order.count;++at) {
        const u32 index=order[at];
        auto& type=cells[index].resolved;
        if(type==B::en && previous==B::l)type=B::l;
        if(type==B::l || type==B::r)previous=type;
    }
}
u32 canonical_bracket(u32 scalar) noexcept {return scalar==0x232a?0x3009:scalar;}
void brackets(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order,B sos,B,B embedding_direction) noexcept {
    struct Open {u32 paired,position;};Open stack[63];u32 depth=0;
    for(u32 i=0;i<order.count;++i) {
        auto& cell=cells[order[i]];if(cell.resolved!=B::on)continue;
        cell.link=none;const auto bracket=unicode_bidi_bracket(cell.scalar);
        if(bracket.type==UnicodeBracketType::open) {
            // BD16 returns an empty list for the entire sequence on overflow.
            if(depth==63)return;
            stack[depth++]={canonical_bracket(bracket.paired),i};
        } else if(bracket.type==UnicodeBracketType::close) {
            const u32 scalar=canonical_bracket(cell.scalar);
            for(u32 j=depth;j>0;--j)if(stack[j-1].paired==scalar) {
                cells[order[stack[j-1].position]].link=i;depth=j-1;break;
            }
        }
    }
    B previous=sos;
    for(u32 i=0;i<order.count;++i) {
        auto& cell=cells[order[i]];
        if(cell.resolved==B::on && cell.link!=none) {
            const u32 close=cell.link;bool same=false,opposite=false;
            for(u32 j=i+1;j<close;++j) {
                const B type=strong(cells[order[j]].resolved);
                if(type==embedding_direction){same=true;break;}
                if(type==B::l || type==B::r)opposite=true;
            }
            if(same || opposite) {
                const B value=same?embedding_direction:previous;
                cell.resolved=value;cells[order[close]].resolved=value;
                for(u32 j=i+1;j<order.count && cells[order[j]].original==B::nsm;++j)cells[order[j]].resolved=value;
                for(u32 j=close+1;j<order.count && cells[order[j]].original==B::nsm;++j)cells[order[j]].resolved=value;
            }
        }
        const B type=strong(cell.resolved);if(type==B::l || type==B::r)previous=type;
    }
}
void neutral_types(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order,B sos,B eos,B embedding_direction) noexcept {
    B previous=sos;
    for(u32 i=0;i<order.count;) {
        if(!neutral(cells[order[i]].resolved)){previous=strong(cells[order[i++]].resolved);continue;}
        const u32 first=i;while(i<order.count && neutral(cells[order[i]].resolved))++i;
        const B after=i<order.count?strong(cells[order[i]].resolved):eos;
        const B value=previous==after?previous:embedding_direction;
        for(u32 j=first;j<i;++j)cells[order[j]].resolved=value;
        previous=value;
    }
}
}

UnicodeBidiStatus unicode_bidi_classify(ConstSlice<u32> scalars,Slice<UnicodeBidiCell> cells) noexcept {
    if((scalars.count && !scalars.data) || (cells.count && !cells.data))return UnicodeBidiStatus::invalid_input;
    if(cells.count<scalars.count)return UnicodeBidiStatus::output_too_small;
    for(u32 i=0;i<scalars.count;++i)if(!unicode_is_scalar(scalars[i]))return UnicodeBidiStatus::invalid_input;
    for(u32 i=0;i<scalars.count;++i) {
        const B type=unicode_properties(scalars[i])->bidi_class();cells[i]={scalars[i],none,none,type,type,0,0};
    }
    return UnicodeBidiStatus::ok;
}
UnicodeBidiStatus unicode_bidi_paragraph_end(ConstSlice<UnicodeBidiCell> cells,u32 first,u32* end) noexcept {
    if(!end)return UnicodeBidiStatus::invalid_input;*end=first;
    if(first>cells.count || (cells.count && !cells.data))return UnicodeBidiStatus::invalid_input;
    for(u32 i=first;i<cells.count;++i)if(cells[i].original==B::b){*end=i+1;return UnicodeBidiStatus::ok;}
    *end=cells.count;return UnicodeBidiStatus::ok;
}
UnicodeBidiStatus bidi_resolve_with_brackets(Slice<UnicodeBidiCell> cells,UnicodeParagraphDirection policy,Slice<u32> scratch,UnicodeBidiParagraph* paragraph,BidiBracketResolver resolve_brackets,bool windows_profile) noexcept {
    if(!paragraph)return UnicodeBidiStatus::invalid_input;*paragraph={};
    if(!resolve_brackets || static_cast<u8>(policy)>3 || (cells.count && !cells.data) || (scratch.count && !scratch.data))return UnicodeBidiStatus::invalid_input;
    if(scratch.count<cells.count)return UnicodeBidiStatus::output_too_small;
    for(u32 i=0;i<cells.count;++i) {
        if(!unicode_is_scalar(cells[i].scalar) || static_cast<u8>(cells[i].original)>static_cast<u8>(B::ws))return UnicodeBidiStatus::invalid_input;
        if(cells[i].original==B::b && i+1!=cells.count)return UnicodeBidiStatus::malformed_paragraph;
    }
    match_isolates(cells,windows_profile);
    const u8 base=policy==UnicodeParagraphDirection::ltr?0:policy==UnicodeParagraphDirection::rtl?1:
        first_direction({cells.data,cells.count},0,cells.count,policy==UnicodeParagraphDirection::auto_rtl?1:0,windows_profile);
    explicit_levels(cells,base,windows_profile);
    u32 previous=none;
    for(u32 i=0;i<cells.count;++i)if(cells[i].level!=unicode_bidi_removed) {
        if(previous!=none)cells[previous].next=cells[previous].explicit_level==cells[i].explicit_level?i:
            isolate(cells[previous].original)?cells[previous].link:none;
        previous=i;
    }
    previous=none;
    for(u32 first=0;first<cells.count;++first)if(cells[first].level!=unicode_bidi_removed) {
        const bool start=previous==none || cells[previous].explicit_level!=cells[first].explicit_level;
        const u8 preceding=previous==none?base:cells[previous].explicit_level;previous=first;
        if(!start || (cells[first].original==B::pdi && cells[first].link!=none))continue;
        u32 count=0,last=first;
        for(u32 i=first;i!=none;i=cells[i].next){scratch[count++]=i;last=i;}
        u8 following=base;
        if(!isolate(cells[last].original))for(u32 i=last+1;i<cells.count;++i)if(cells[i].level!=unicode_bidi_removed){following=cells[i].explicit_level;break;}
        const B sos=direction(maximum(preceding,cells[first].explicit_level)),eos=direction(maximum(following,cells[last].explicit_level));
        const B embedding_direction=direction(cells[first].explicit_level);const ConstSlice<u32> order{scratch.data,count};
        weak_types(cells,order,sos,windows_profile);resolve_brackets(cells,order,sos,eos,embedding_direction);neutral_types(cells,order,sos,eos,embedding_direction);
        for(u32 i=0;i<count;++i) {
            auto& cell=cells[order[i]];u8 level=cell.explicit_level;
            if(level&1){if(cell.resolved==B::l || cell.resolved==B::en || cell.resolved==B::an)++level;}
            else if(cell.resolved==B::r)++level;else if(cell.resolved==B::en || cell.resolved==B::an)level+=2;
            cell.level=level;
        }
    }
    *paragraph={cells.count,base};return UnicodeBidiStatus::ok;
}
UnicodeBidiStatus unicode_bidi_resolve(Slice<UnicodeBidiCell> cells,UnicodeParagraphDirection policy,Slice<u32> scratch,UnicodeBidiParagraph* paragraph) noexcept {
    return bidi_resolve_with_brackets(cells,policy,scratch,paragraph,brackets);
}
UnicodeBidiStatus unicode_bidi_line(ConstSlice<UnicodeBidiCell> cells,u8 base,u32 first,u32 end,Slice<u8> levels,Slice<u32> visual,u32* visual_count) noexcept {
    if(!visual_count)return UnicodeBidiStatus::invalid_input;*visual_count=0;
    if(base>1 || first>end || end>cells.count || (cells.count && !cells.data) || (levels.count && !levels.data) || (visual.count && !visual.data))return UnicodeBidiStatus::invalid_input;
    const u32 count=end-first;if(levels.count<count || visual.count<count)return UnicodeBidiStatus::output_too_small;
    for(u32 i=first;i<end;++i) {
        if(cells[i].level>126 && cells[i].level!=unicode_bidi_removed)return UnicodeBidiStatus::invalid_input;
        levels[i-first]=cells[i].level;
    }
    for(u32 i=first;i<end;++i)if(cells[i].original==B::s || cells[i].original==B::b) {
        levels[i-first]=base;
        for(u32 j=i;j>first;) {--j;if(levels[j-first]==unicode_bidi_removed)continue;if(!space(cells[j].original))break;levels[j-first]=base;}
    }
    for(u32 j=end;j>first;) {--j;if(levels[j-first]==unicode_bidi_removed)continue;if(!space(cells[j].original))break;levels[j-first]=base;}
    u32 total=0,highest=0,lowest_odd=255;
    for(u32 i=first;i<end;++i)if(levels[i-first]!=unicode_bidi_removed) {
        visual[total++]=i;const u32 level=levels[i-first];highest=maximum(highest,level);if(level&1)lowest_odd=minimum(lowest_odd,level);
    }
    for(u32 level=highest;level>=lowest_odd;--level)for(u32 i=0;i<total;) {
        if(levels[visual[i]-first]<level){++i;continue;}
        const u32 start=i;while(i<total && levels[visual[i]-first]>=level)++i;
        for(u32 a=start,b=i-1;a<b;++a,--b){const u32 item=visual[a];visual[a]=visual[b];visual[b]=item;}
    }
    *visual_count=total;return UnicodeBidiStatus::ok;
}
} // namespace maicord
