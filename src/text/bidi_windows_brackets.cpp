#include "bidi_internal.hpp"

namespace maicord {
namespace {
using B=UnicodeBidiClass;
constexpr u32 none=~0u;
struct Content {bool l=false,r=false;};
Content content(ConstSlice<UnicodeBidiCell> cells,ConstSlice<u32> order,u32 first,u32 end) noexcept {
    Content result;
    for(u32 i=first;i<end;++i) {
        const B type=cells[order[i]].resolved;
        result.l|=type==B::l;result.r|=type==B::r || type==B::an;
    }
    return result;
}
B context(B type) noexcept {
    return type==B::an?B::r:type;
}
bool paired_open(const UnicodeBidiCell& cell) noexcept {
    return cell.link!=none && unicode_bidi_bracket(cell.scalar).type==UnicodeBracketType::open;
}
bool context_strong(B type) noexcept {return type==B::l || type==B::r || type==B::an;}
void assign(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order,u32 at,B value) noexcept {
    auto& cell=cells[order[at]];const bool existing=context_strong(cell.resolved);cell.resolved=value;
    if(existing)return;
    // Insert a newly strong cell into the successor map. Stop after updating
    // the nearest preceding strong cell; all earlier successors stay valid.
    for(u32 j=at;j>0;) {
        auto& preceding=cells[order[--j]];preceding.next=at;
        if(context_strong(preceding.resolved))break;
    }
}
void terminator_context(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order) noexcept {
    // A resolved R bracket supplies the numeric context through weak
    // separators and marks, until another pair, strong character or number.
    // Both matched ends have links; the successor map is no longer needed.
    bool context=false;
    for(u32 i=0;i<order.count;) {
        auto& cell=cells[order[i]];const B original=cell.original;
        if(original==B::on && cell.link!=none)context=cell.resolved==B::r;
        else if(original==B::l || original==B::r || original==B::al || original==B::an || original==B::en)context=false;
        else if(original==B::et) {
            const u32 first=i++;
            while(i<order.count && (cells[order[i]].original==B::et || cells[order[i]].original==B::nsm))++i;
            if(context && cell.resolved==B::l && i-first>1)cell.resolved=B::on;
            continue;
        }
        ++i;
    }
}
}
UnicodeBidiStatus bidi_windows_resolve_candidate(Slice<UnicodeBidiCell> cells,UnicodeParagraphDirection direction,
    Slice<u32> scratch,UnicodeBidiParagraph* paragraph) noexcept {
    return bidi_resolve_with_brackets(cells,direction,scratch,paragraph,bidi_windows_brackets_candidate,true);
}
void bidi_windows_brackets_candidate(Slice<UnicodeBidiCell> cells,ConstSlice<u32> order,
                                    B sos,B eos,B embedding_direction) noexcept {
    // Windows-compatible, not UAX #9 N0: at most 61 tracked openings, matched by
    // scalar. Not a substitute for the public standards resolver.
    struct Open {u32 paired,position;};Open stack[61];u32 depth=0;
    for(u32 i=0;i<order.count;++i) {
        auto& cell=cells[order[i]];if(cell.resolved!=B::on)continue;
        cell.link=none;const auto bracket=unicode_bidi_bracket(cell.scalar);
        if(bracket.type==UnicodeBracketType::open) {
            if(depth<61)stack[depth++]={bracket.paired,i};
        } else if(bracket.type==UnicodeBracketType::close) {
            for(u32 j=depth;j>0;--j)if(stack[j-1].paired==cell.scalar) {
                cells[order[stack[j-1].position]].link=i;cell.link=stack[j-1].position;depth=j-1;break;
            }
        }
    }
    // Resolve pairs containing the embedding direction before consulting the
    // context of opposite-only pairs. A later mixed pair can therefore supply
    // context to an earlier pair without a speculative backward repair pass.
    for(u32 i=0;i<order.count;++i) {
        auto& cell=cells[order[i]];if(!paired_open(cell))continue;
        const auto inside=content({cells.data,cells.count},order,i+1,cell.link);
        if(embedding_direction==B::l?inside.l:inside.r)
            cell.resolved=cells[order[cell.link]].resolved=embedding_direction;
    }
    // The old next links are no longer needed; reuse them for the nearest following
    // strong position, avoiding repeated suffix scans over long runs of empty pairs.
    u32 next_strong=none;
    for(u32 i=order.count;i>0;) {
        auto& cell=cells[order[--i]];cell.next=next_strong;
        if(context_strong(cell.resolved))next_strong=i;
    }
    bool has_r_pair=false;
    for(u32 i=0;i<order.count;++i) {
        auto& cell=cells[order[i]];if(!paired_open(cell))continue;
        const u32 close=cell.link;const auto inside=content({cells.data,cells.count},order,i+1,close);
        const bool same=embedding_direction==B::l?inside.l:inside.r;
        const bool opposite=embedding_direction==B::l?inside.r:inside.l;
        B value=B::on;
        if(same)value=embedding_direction;
        else {
            B before=sos,after=eos;
            for(u32 j=i;j>0;) {
                const B raw=cells[order[--j]].resolved;
                const B type=raw==B::an?B::r:raw;
                if(type==B::l || type==B::r){before=type;break;}
            }
            const u32 following=cells[order[close]].next;
            if(following!=none)after=context(cells[order[following]].resolved);
            value=opposite?(after!=embedding_direction?after:before):
                  before==after?before:embedding_direction;
        }
        if(value==B::on)continue;
        has_r_pair|=value==B::r;
        assign(cells,order,i,value);assign(cells,order,close,value);
        for(u32 j=i+1;j<order.count && cells[order[j]].original==B::nsm;++j)assign(cells,order,j,value);
        for(u32 j=close+1;j<order.count && cells[order[j]].original==B::nsm;++j)assign(cells,order,j,value);
    }
    // In an even embedding the first neutral resolves back to L because the
    // rest of this converted run is L. Only odd embeddings need the repair.
    if(embedding_direction==B::r && has_r_pair)terminator_context(cells,order);
}
}
