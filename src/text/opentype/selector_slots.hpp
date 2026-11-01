#pragma once
#include "maicord/text/base.hpp"
namespace maicord {
// Original-order selector slots can survive beyond the current cluster's
// overwritten prefix. All 256 selectors plus zero fit in nine bits per slot.
struct SfntSelectorSlots {
    u8 low[32]{};u32 high=0;
    void put(u32 at,u32 selector) noexcept {
        const u32 code=!selector?0:selector>=0xe0100?selector-0xe0100+17:selector-0xfe00+1;
        low[at]=static_cast<u8>(code);high=(high&~(1u<<at))|((code>>8)<<at);
    }
    u32 get(u32 at) const noexcept {
        const u32 code=low[at]+(((high>>at)&1u)<<8);
        return !code?0:code<=16?0xfe00+code-1:0xe0100+code-17;
    }
};
static_assert(sizeof(SfntSelectorSlots)==36);
}
