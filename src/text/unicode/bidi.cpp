#include "maicord/text/unicode/detail/bidi_internal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using B=UnicodeBidiClass;
constexpr u32 none=~0u;
inline bool isolate(B type) noexcept {return type==B::lri || type==B::rli || type==B::fsi;}
inline bool embedding(B type) noexcept {return type==B::lre || type==B::rle || type==B::lro || type==B::rlo;}
inline bool removed(B type) noexcept {return embedding(type) || type==B::pdf || type==B::bn;}
inline bool neutral(B type) noexcept {return type==B::b || type==B::s || type==B::ws || type==B::on || isolate(type) || type==B::pdi;}
inline B direction(u32 level) noexcept {return level&1?B::r:B::l;}
inline B strong(B type) noexcept {return type==B::en || type==B::an?B::r:type;}
inline bool space(B type) noexcept {return type==B::ws || isolate(type) || type==B::pdi;}
inline B analysis_type(B type,bool tailored) noexcept {return tailored && (isolate(type) || type==B::pdi)?B::bn:type;}

// Resets each cell's working fields and links isolate initiators to their terminators.
inline void match_isolates(Slice<UnicodeBidiCell> cells,bool tailored) noexcept {
    u32 top=none;
    for(u32 i=0;i<cells.count;++i) {
        auto& cell=cells[i];
        const B type=analysis_type(cell.original,tailored);
        cell.link=none;cell.next=none;cell.resolved=type;cell.explicit_level=0;cell.level=unicode_bidi_removed;
        if(isolate(type)){cell.link=top;top=i;}
        else if(type==B::pdi && top!=none) {
            const u32 previous=cells[top].link;cells[top].link=i;cell.link=top;top=previous;
        }
    }
    while(top!=none){const u32 previous=cells[top].link;cells[top].link=none;top=previous;}
}
inline u8 first_direction(ConstSlice<UnicodeBidiCell> cells,u32 first,u32 end,u8 fallback,bool tailored) noexcept {
    for(u32 i=first;i<end;++i) {
        const auto& cell=cells[i];const B type=analysis_type(cell.original,tailored);
        if(type==B::l)return 0;
        if(type==B::r || type==B::al)return 1;
        if(isolate(type)) {if(cell.link==none)return fallback;i=cell.link;}
    }
    return fallback;
}
void explicit_levels(Slice<UnicodeBidiCell> cells,u8 base,bool tailored) noexcept {
    struct Status {u8 level;B override_type;bool isolate;};
    Status stack[126];u32 depth=1,overflow_isolates=0,overflow_embeddings=0,valid_isolates=0;
    stack[0]={base,B::on,false};
    for(u32 i=0;i<cells.count;++i) {
        auto& cell=cells[i];const B type=analysis_type(cell.original,tailored);auto current=stack[depth-1];
        cell.explicit_level=current.level;
        if(embedding(type)) {
            const bool rtl=type==B::rle || type==B::rlo;
            const u32 next=rtl?((current.level+1u)|1u):((current.level+2u)&~1u);
            if(next<=125 && !overflow_isolates && !overflow_embeddings) {
                stack[depth++]={static_cast<u8>(next),type==B::rlo?B::r:type==B::lro?B::l:B::on,false};
            } else if(!overflow_isolates)++overflow_embeddings;
        } else if(isolate(type)) {
            if(current.override_type!=B::on)cell.resolved=current.override_type;
            const bool rtl=type==B::rli || (type==B::fsi && first_direction({cells.data,cells.count},i+1,cell.link==none?cells.count:cell.link,0,tailored));
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
void tailored_weak_tail(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order,B sos) noexcept {
    // Numeric context survives intervening weak/neutral characters until
    // another number or strong character replaces it. ET runs and W7 are resolved
    // together so no extra per-cell flags, buffer or backward repair scan is needed.
    B strong=sos,previous=sos;bool number_an=false,pending_cs=false;
    for(u32 i=0;i<order.count;){auto& cell=cells[order[i]];const B type=cell.resolved;
        if(type==B::et){const u32 first=i;while(i<order.count&&cells[order[i]].resolved==B::et)++i;
            const bool special=(strong==B::l&&number_an)||pending_cs;if(pending_cs)strong=B::l;
            const bool convert=previous==B::en||(i<order.count&&cells[order[i]].resolved==B::en);
            const B value=convert?(strong==B::l?B::l:B::en):B::on;
            for(u32 j=first;j<i;++j)cells[order[j]].resolved=value;
            // The first terminator stays neutral for these numeric
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
void weak_types(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order,B sos,bool tailored) noexcept {
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
    if(!tailored)for(u32 at=0;at<order.count;++at)if(cells[order[at]].resolved==B::al)cells[order[at]].resolved=B::r;
    for(u32 i=1;i+1<order.count;++i) {
        auto& type=cells[order[i]].resolved;const B left=cells[order[i-1]].resolved,right=cells[order[i+1]].resolved;
        if(type==B::es && left==B::en && right==B::en)type=B::en;
        else if(type==B::cs && left==right && (left==B::en || left==B::an))type=left;
    }
    if(tailored){tailored_weak_tail(cells,order,sos);return;}
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
inline void neutral_types(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order,B sos,B eos,B embedding_direction) noexcept {
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

u32 unicode_bidi_paragraph_end(ConstSlice<UnicodeBidiCell> cells,u32 first) noexcept {
    for(u32 i=first;i<cells.count;++i)if(cells[i].original==B::b)return i+1;
    return cells.count;
}
u8 bidi_resolve_with_brackets(Slice<UnicodeBidiCell> cells,UnicodeParagraphDirection policy,u32* scratch,BidiBracketResolver resolve_brackets,bool tailored) noexcept {
    match_isolates(cells,tailored);
    const u8 base=policy==UnicodeParagraphDirection::ltr?0:policy==UnicodeParagraphDirection::rtl?1:
        first_direction({cells.data,cells.count},0,cells.count,policy==UnicodeParagraphDirection::auto_rtl?1:0,tailored);
    explicit_levels(cells,base,tailored);
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
        const B embedding_direction=direction(cells[first].explicit_level);const ConstSlice<u32> order{scratch,count};
        weak_types(cells,order,sos,tailored);resolve_brackets(cells,order,sos,eos,embedding_direction);neutral_types(cells,order,sos,eos,embedding_direction);
        for(u32 i=0;i<count;++i) {
            auto& cell=cells[order[i]];u8 level=cell.explicit_level;
            if(level&1){if(cell.resolved==B::l || cell.resolved==B::en || cell.resolved==B::an)++level;}
            else if(cell.resolved==B::r)++level;else if(cell.resolved==B::en || cell.resolved==B::an)level+=2;
            cell.level=level;
        }
    }
    return base;
}
void unicode_bidi_line_levels(ConstSlice<UnicodeBidiCell> cells,u8 base,u8* levels) noexcept {
    // L1: separators, and the whitespace before them and at the line end, take the paragraph level.
    const auto reset_before=[&](u32 end) noexcept {
        for(u32 j=end;j>0;) {--j;if(levels[j]==unicode_bidi_removed)continue;if(!space(cells[j].original))break;levels[j]=base;}};
    for(u32 i=0;i<cells.count;++i) {
        if(cells[i].original!=B::s && cells[i].original!=B::b){levels[i]=cells[i].level;continue;}
        reset_before(i);levels[i]=base;
    }
    reset_before(cells.count);
}
} // namespace maicord
