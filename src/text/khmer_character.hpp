#pragma once
#include "maicord/text/unicode.hpp"
namespace maicord {
enum class KhmerCategory : u8 { other,base,coeng,above,post,vowel,shift,robat,sign,final,joiner,selector };
KhmerCategory khmer_category(u32 scalar) noexcept;
}
