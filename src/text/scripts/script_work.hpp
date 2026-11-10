#pragma once
#include "maicord/text/shaping/shape_run.hpp"
// Helpers of the script policies that run inside sfnt_shape_run (the Indic and syllabic shapers).
namespace maicord::script_work {
// Setup-work meter; used counts what was spent.
struct Work {
    u64 left,used=0;
    bool spend(u64 n) noexcept {if(n>left)return false;left-=n;used+=n;return true;}
};
// Work meter that reports what it spent to *out when the policy callback returns.
struct Metered : Work {
    u64* out;
    Metered(u64 available,u64* report) noexcept : Work{available},out(report) {}
    ~Metered() noexcept {*out=used;}
};
// Moves one glyph record in front of position before, keeping the others in order.
inline SfntStatus move_before(const Slice<SfntComponentGlyph>& view,u32 from,u32 before,Work& w) noexcept {
    const Slice<SfntComponentGlyph> glyphs=view; // the record stores below must not force reloads
    if(from==0xffffffffu||before==0xffffffffu)return SfntStatus::ok;
    const u32 to=before>from?before-1:before;if(from>=glyphs.count||to>=glyphs.count)return SfntStatus::invalid_input;
    if(!w.spend((from>to?from-to:to-from)+1))return SfntStatus::resource_limit;
    const auto saved=glyphs[from];if(from<to)for(u32 i=from;i<to;++i)glyphs[i]=glyphs[i+1];
    else for(u32 i=from;i>to;--i)glyphs[i]=glyphs[i-1];glyphs[to]=saved;return SfntStatus::ok;
}
// A glyph run read through its component forest: the first component of a glyph carries its metadata (HarfBuzz keeps
// the first component's properties on a ligature); an expansion output left without components takes the metadata of
// the output before it. The two walks stay out of line: the reordering code calls them from dozens of sites.
struct Glyphs {
    SfntComponentForest& forest;Slice<SfntComponentGlyph> g;Work& work;SfntStatus status=SfntStatus::ok;
    static constexpr u32 nil=0xffffffffu;
    u32 owner(u32 i) const noexcept {u32 j=i;while(j&&g[j].component==nil)--j;
        if(g[j].component==nil)while(j+1<g.count&&g[j].component==nil)++j;return g[j].component;}
    __attribute__((noinline)) SfntComponentNode* first(u32 i) noexcept {u32 node=owner(i);if(node==nil)return nullptr;
        while(node<forest.nodes.count){if(!work.spend(1)){status=SfntStatus::resource_limit;return nullptr;}
            auto& n=forest.nodes[node];if((n.flags&1)||n.left==nil)return &n;node=n.left;}
        status=SfntStatus::invalid_input;return nullptr;}
    u8 meta(u32 i) noexcept {const auto* n=first(i);return n?static_cast<u8>(n->reserved>>8):0;}
    __attribute__((noinline)) bool ligated(u32 i) const noexcept {const u32 node=owner(i);return node<forest.nodes.count&&forest.nodes[node].count>=2;}
    void move(u32 from,u32 before) noexcept {if(status==SfntStatus::ok)status=move_before(g,from,before,work);}
};
}
