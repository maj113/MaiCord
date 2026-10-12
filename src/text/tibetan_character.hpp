#pragma once
#include "maicord/text/unicode.hpp"
namespace maicord {
enum class TibetanCategory:u8 {
    other,base,placeholder,symbol,reserved,subjoined,cons_above,cons_below,
    vowel_pre,vowel_above,vowel_below,compound,vowel_post,modifier,final,split,
    defective,joiner,nonjoiner,cgj,selector,supplementary_selector
};
TibetanCategory tibetan_category(u32) noexcept;
inline bool tibetan_split_pair(u32 scalar,u32& first,u32& second) noexcept {
    if(scalar!=0xf73&&scalar!=0xf76&&scalar!=0xf78&&scalar!=0xf81)return false;
    first=scalar==0xf76?0xfb2u:scalar==0xf78?0xfb3u:0xf71u;
    second=scalar==0xf73?0xf72u:0xf80u;return true;
}
}
