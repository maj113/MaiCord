#pragma once
#include "maicord/protocol/base64.hpp"

namespace maicord {

enum class Base64CodecAlphabet : u8 {
    standard,
    url
};

// padded: decode requires '=' padding, or accepts it only in canonical form
// when false; encode emits it, or omits it when false.
Base64Result base64_codec_decode(Utf8View input, Slice<u8> output, Base64CodecAlphabet alphabet,
                                 bool padded) noexcept;
Base64Result base64_codec_encode(ConstSlice<u8> input, Slice<char> output, Base64CodecAlphabet alphabet,
                                 bool padded) noexcept;

} // namespace maicord
