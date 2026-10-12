#pragma once
#include "maicord/text/unicode.hpp"
#include "maicord/text/indic_plan.hpp"
namespace maicord {
// Internal shared classification; scalar validity is checked by callers.
enum class DevanagariCategory : u8 {other,consonant,vowel,placeholder,nukta,halant,joiner,nonjoiner,pre,above,below,post,bindu,visarga,vedic,medial};
DevanagariCategory indic_category(SfntIndicProfile,u32 scalar) noexcept;
}
