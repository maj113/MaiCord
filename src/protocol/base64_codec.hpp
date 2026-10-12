#pragma once
#include "maicord/protocol/base64.hpp"

namespace maicord {

enum class Base64CodecAlphabet : u8 {
    standard,
    url
};
enum class Base64CodecPadding : u8 {
    required,
    optional,
    omitted
};

Base64Result base64_codec_decode(Utf8View input, Slice<u8> output, Base64CodecAlphabet alphabet,
                                 Base64CodecPadding padding) noexcept;
Base64Result base64_codec_encode(ConstSlice<u8> input, Slice<char> output, Base64CodecAlphabet alphabet,
                                 Base64CodecPadding padding) noexcept;

} // namespace maicord
