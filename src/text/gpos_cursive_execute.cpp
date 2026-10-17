#include "maicord/text/gpos_cursive_execute.hpp"
#include "position_anchor.hpp"
namespace maicord {namespace {
bool valid(const SfntPosition& value) noexcept {return is_finite(value.advance)&&is_finite(value.advance_offset)&&is_finite(value.ascender_offset);}
bool spend(SfntCursiveExecution* out,u64* work,u64 cost=1) noexcept {if(cost>*work)return false;*work-=cost;out->operations+=cost;return true;}
double magnitude(double x) noexcept {return x<0?-x:x;}
bool bounded_integer(double value) noexcept {
    u64 bits=0;copy_bytes(&bits,&value,8);bits&=0x7fffffffffffffffull;if(!bits)return true;
    const u32 exponent=static_cast<u32>(bits>>52);if(exponent<1023 || bits>0x4270000000000000ull)return false;
    return !(bits&((1ull<<(1075-exponent))-1)); // |value| <= 2^40
}
SfntStatus flush(Slice<SfntPosition> positions,Slice<SfntCursiveChainNode> chains,SfntCursiveDeferred* pending,SfntCursiveExecution* out,u64* work) noexcept {
    if(!pending || pending->head==0xffffffffu)return SfntStatus::ok;
    if(!positions.data || !chains.data || pending->next>=positions.count || pending->next>=chains.count || pending->head>=pending->next || !is_finite(pending->translation))return SfntStatus::invalid_input;
    u32 previous=pending->next,at=pending->head;
    while(at!=0xffffffffu){if(!spend(out,work))return SfntStatus::resource_limit;if(at>=previous)return SfntStatus::invalid_input;
        const double y=positions.data[at].ascender_offset+pending->translation;if(!is_finite(y))return SfntStatus::resource_limit;positions.data[at].ascender_offset=y;previous=at;at=chains.data[at].child;
    }
    *pending={};return SfntStatus::ok;
}
}
SfntStatus sfnt_position_cursive_flush(Slice<SfntPosition> positions,Slice<SfntCursiveChainNode> chains,SfntCursiveDeferred* pending,SfntCursiveExecution* out,u64 work) noexcept {
    if(!out || !pending)return SfntStatus::invalid_input;*out={};return flush(positions,chains,pending,out,&work);
}
SfntStatus sfnt_position_execute_cursive_lookup(const SfntGposAttachmentLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,ConstSlice<double> nominal,Slice<SfntPosition> positions,u32 start,u32 end,bool rtl,const SfntPositionInstance& instance,Slice<u64> bits,SfntCursiveExecution* out,u64 work,SfntAnchorPointResolver resolver,void* context) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};const u32 count=sfnt_glyph_count(glyphs);
    if(!sfnt_glyph_view_valid(glyphs) || start>end || end>count || positions.count<count || nominal.count<count || (count && (!positions.data || !nominal.data)) || static_cast<u8>(instance.arithmetic)>1)return SfntStatus::invalid_input;
    if(!lookup.subtables.count || start==end)return SfntStatus::ok;if(lookup.subtables.data[0].type!=3)return SfntStatus::unsupported;
    const bool backward=(lookup.filter.flags&1)!=0;const u32 length=end-start,words=backward?length/64+(length%64?1u:0u):0;
    if(bits.count<words)return SfntStatus::output_too_small;if(words && !bits.data)return SfntStatus::invalid_input;if(!spend(out,&work,words))return SfntStatus::resource_limit;
    for(u32 i=0;i<words;++i)bits.data[i]=0;out->chain_words=words;u32 previous=0xffffffffu;double gap_advance=0;
    for(u32 at=start;at<end;++at){if(!spend(out,&work))return SfntStatus::resource_limit;++out->scan_steps;const auto& glyph=sfnt_glyph_at(glyphs,at);
        if(glyph.glyph>=lookup.subtables.data[0].glyph_limit)return SfntStatus::glyph_out_of_range;if(static_cast<u8>(glyph.glyph_class)>4)return SfntStatus::invalid_input;
        if(sfnt_lookup_ignores(gdef,lookup.filter,glyph.glyph,glyph.glyph_class)){gap_advance+=positions.data[at].advance;if(!is_finite(gap_advance))return SfntStatus::resource_limit;continue;}
        if(previous!=0xffffffffu){const auto& first=sfnt_glyph_at(glyphs,previous);
            for(u32 i=0;i<lookup.subtables.count;++i){if(!spend(out,&work))return SfntStatus::resource_limit;SfntGposCursiveAnchors a{},b{};
                auto s=sfnt_gpos_cursive_anchors(lookup.subtables.data[i],first.glyph,&a);if(s!=SfntStatus::ok)return s;if(!a.covered || !a.exit.table.data)continue;
                s=sfnt_gpos_cursive_anchors(lookup.subtables.data[i],glyph.glyph,&b);if(s!=SfntStatus::ok)return s;if(!b.covered || !b.entry.table.data)continue;
                double ax=0,ay=0,bx=0,by=0;s=position_detail::anchor_point(a.exit,first.glyph,instance,&ax,&ay,&out->operations,&work,resolver,context);if(s!=SfntStatus::ok)return s;
                s=position_detail::anchor_point(b.entry,glyph.glyph,instance,&bx,&by,&out->operations,&work,resolver,context);if(s!=SfntStatus::ok)return s;
                auto p=positions.data[previous],q=positions.data[at];
                if(rtl){const double placement=backward?q.advance-nominal.data[at]-q.advance_offset:p.advance-nominal.data[previous]-p.advance_offset;const double advance=bx-ax+(backward?placement:-placement)-gap_advance;q.advance_offset+=advance-q.advance;q.advance=advance;}
                else p.advance=ax-bx+(backward?-q.advance_offset:p.advance_offset)-gap_advance;
                if(backward)p.ascender_offset=by-ay;else q.ascender_offset=p.ascender_offset+ay-by;
                if(!valid(p)||!valid(q))return SfntStatus::resource_limit;positions.data[previous]=p;positions.data[at]=q;
                if(backward){const u32 slot=previous-start;bits.data[slot/64]|=1ull<<(slot%64);}++out->matches;break;
            }
        }
        previous=at;gap_advance=0;
    }
    if(backward){u32 next=0xffffffffu;for(u32 at=end;at>start;){--at;if(!spend(out,&work))return SfntStatus::resource_limit;++out->scan_steps;const auto& glyph=sfnt_glyph_at(glyphs,at);
        if(sfnt_lookup_ignores(gdef,lookup.filter,glyph.glyph,glyph.glyph_class))continue;const u32 slot=at-start;
        if(bits.data[slot/64]&(1ull<<(slot%64))){if(next==0xffffffffu)return SfntStatus::malformed;const double y=positions.data[at].ascender_offset+positions.data[next].ascender_offset;if(!is_finite(y))return SfntStatus::resource_limit;positions.data[at].ascender_offset=y;}
        next=at;}}
    return SfntStatus::ok;
}
SfntStatus sfnt_position_execute_cursive_selected(const SfntGposAttachmentLookup& lookup,const SfntGdef& gdef,SfntGlyphView glyphs,ConstSlice<double> nominal,Slice<SfntPosition> positions,u32 at,u32 end,bool rtl,const SfntPositionInstance& instance,Slice<SfntCursiveChainNode> chains,SfntCursiveExecution* out,u64 work,SfntAnchorPointResolver resolver,void* context,SfntCursiveDeferred* pending) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};const u32 count=sfnt_glyph_count(glyphs);
    if(!sfnt_glyph_view_valid(glyphs) || at>=end || end>count || positions.count<count || nominal.count<count || !positions.data || !nominal.data || static_cast<u8>(instance.arithmetic)>1)return SfntStatus::invalid_input;
    if(!lookup.subtables.count)return SfntStatus::ok;if(lookup.subtables.data[0].type!=3)return SfntStatus::unsupported;
    const bool backward=(lookup.filter.flags&1)!=0;
    if(backward && (chains.count<count || !chains.data))return SfntStatus::output_too_small;
    // Small runs cannot amortize proof/flush overhead. Keep their eager path.
    if(pending && pending->head==0xffffffffu && count<32)pending=nullptr;
    if(pending && pending->head!=0xffffffffu && (!backward || at!=pending->next || resolver)){const auto s=flush(positions,chains,pending,out,&work);if(s!=SfntStatus::ok)return s;}
    const auto& selected=sfnt_glyph_at(glyphs,at);const u32 limit=lookup.subtables.data[0].glyph_limit;
    if(selected.glyph>=limit)return SfntStatus::glyph_out_of_range;if(static_cast<u8>(selected.glyph_class)>4)return SfntStatus::invalid_input;
    u32 other=at;double gap=0;bool found=false;
    while(backward?other+1<end:other>0){other=backward?other+1:other-1;if(!spend(out,&work))return SfntStatus::resource_limit;++out->scan_steps;const auto& g=sfnt_glyph_at(glyphs,other);
        if(g.glyph>=limit)return SfntStatus::glyph_out_of_range;if(static_cast<u8>(g.glyph_class)>4)return SfntStatus::invalid_input;
        if(!sfnt_lookup_ignores(gdef,lookup.filter,g.glyph,g.glyph_class)){found=true;break;}
        gap+=positions.data[other].advance;if(!is_finite(gap))return SfntStatus::resource_limit;
    }
    if(!found)return SfntStatus::ok;const u32 first=backward?at:other,second=backward?other:at;
    const u16 ag=sfnt_glyph_at(glyphs,first).glyph,bg=sfnt_glyph_at(glyphs,second).glyph;
    for(u32 i=0;i<lookup.subtables.count;++i){if(!spend(out,&work))return SfntStatus::resource_limit;SfntGposCursiveAnchors a{},b{};
        auto s=sfnt_gpos_cursive_anchors(lookup.subtables.data[i],ag,&a);if(s!=SfntStatus::ok)return s;if(!a.covered || !a.exit.table.data)continue;
        s=sfnt_gpos_cursive_anchors(lookup.subtables.data[i],bg,&b);if(s!=SfntStatus::ok)return s;if(!b.covered || !b.entry.table.data)continue;
        double ax=0,ay=0,bx=0,by=0;s=position_detail::anchor_point(a.exit,ag,instance,&ax,&ay,&out->operations,&work,resolver,context);if(s!=SfntStatus::ok)return s;
        s=position_detail::anchor_point(b.entry,bg,instance,&bx,&by,&out->operations,&work,resolver,context);if(s!=SfntStatus::ok)return s;
        auto p=positions.data[first],q=positions.data[second];
        if(rtl){const double placement=backward?q.advance-nominal.data[second]-q.advance_offset:p.advance-nominal.data[first]-p.advance_offset;const double advance=bx-ax+(backward?placement:-placement)-gap;q.advance_offset+=advance-q.advance;q.advance=advance;}
        else p.advance=ax-bx+(backward?-q.advance_offset:p.advance_offset)-gap;
        if(backward)p.ascender_offset=q.ascender_offset+by-ay;else q.ascender_offset=p.ascender_offset+ay-by;
        if(!valid(p)||!valid(q))return SfntStatus::resource_limit;
        if(backward){const double delta=p.ascender_offset-positions.data[first].ascender_offset;if(!is_finite(delta))return SfntStatus::resource_limit;
            auto& node=chains.data[first];
            // Initial values and total absolute translation are <= 2^40.
            // Stored (new_y - translation) and materialized sums stay below
            // 2^42, so regrouping these integer additions is exact in binary64.
            bool defer=pending && !resolver && bounded_integer(p.ascender_offset) && bounded_integer(positions.data[first].ascender_offset) && bounded_integer(delta);
            if(defer && pending->head!=0xffffffffu)defer=pending->head==node.child && pending->magnitude+magnitude(delta)<=1099511627776.0;
            if(defer && pending->head==0xffffffffu){u32 previous=first;
                for(u32 child=node.child;child!=0xffffffffu;child=chains.data[child].child){if(!spend(out,&work))return SfntStatus::resource_limit;if(child>=previous)return SfntStatus::invalid_input;previous=child;
                    if(!bounded_integer(positions.data[child].ascender_offset)){defer=false;break;}}}
            if(!defer && pending){s=flush(positions,chains,pending,out,&work);if(s!=SfntStatus::ok)return s;}
            if(node.parent!=0xffffffffu){if(node.parent>=count || node.parent<=first || chains.data[node.parent].child!=first)return SfntStatus::invalid_input;chains.data[node.parent].child=0xffffffffu;}
            const u32 replaced=chains.data[second].child;
            if(replaced!=0xffffffffu){if(replaced>=second || chains.data[replaced].parent!=second)return SfntStatus::invalid_input;chains.data[replaced].parent=0xffffffffu;}
            node.parent=second;chains.data[second].child=first;
            // A parent has one live predecessor; a later attachment replaces it.
            u32 child=defer?0xffffffffu:node.child;
            while(child!=0xffffffffu){if(!spend(out,&work))return SfntStatus::resource_limit;if(child>=first)return SfntStatus::invalid_input;
                const double y=positions.data[child].ascender_offset+delta;if(!is_finite(y))return SfntStatus::resource_limit;positions.data[child].ascender_offset=y;
                child=chains.data[child].child;
            }
            if(defer){pending->translation+=delta;pending->magnitude+=magnitude(delta);pending->head=first;pending->next=second;p.ascender_offset-=pending->translation;}
        }
        positions.data[first]=p;positions.data[second]=q;++out->matches;break;
    }
    return SfntStatus::ok;
}
}
