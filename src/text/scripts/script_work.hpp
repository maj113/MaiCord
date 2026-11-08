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
}
