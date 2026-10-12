#include "maicord/protocol/base64.hpp"
#include "base64_codec.hpp"

namespace maicord {

Base64Result base64_decode(Utf8View input, Slice<u8> output) noexcept {
    return base64_codec_decode(input, output, Base64CodecAlphabet::standard, Base64CodecPadding::required);
}

Base64Result base64_encode(ConstSlice<u8> input, Slice<char> output) noexcept {
    return base64_codec_encode(input, output, Base64CodecAlphabet::standard, Base64CodecPadding::required);
}

} // namespace maicord
