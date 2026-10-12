#pragma once
#include "maicord/text/unicode.hpp"
namespace maicord {
// Original-scalar categories for the Sinhala USE frontend. Composite classes
// retain their leading and final positional rank until input is expanded.
enum class SinhalaCategory : u8 {
    other,base,halant,pre,above,below,post,modifier_above,modifier_post,
    split_above,split_post,placeholder,joiner,nonjoiner,cgj,selector,defective,symbol,reserved,number
};
SinhalaCategory sinhala_category(u32 scalar) noexcept;
}
