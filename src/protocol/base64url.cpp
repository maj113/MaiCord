#include "maicord/protocol/base64url.hpp"
#include "base64_codec.hpp"

namespace maicord {

static_assert(static_cast<u8>(Base64UrlError::output_too_small) ==
              static_cast<u8>(Base64Error::output_too_small));

Base64UrlResult base64url_decode(const Utf8View& input, const Slice<u8>& output) noexcept {
    const Base64Result result = base64_codec_decode(input, output, Base64CodecAlphabet::url, false);
    return Base64UrlResult{result.bytes, result.error_offset, static_cast<Base64UrlError>(result.error)};
}

Base64UrlResult base64url_encode(const ConstSlice<u8>& input, const Slice<char>& output) noexcept {
    const Base64Result result = base64_codec_encode(input, output, Base64CodecAlphabet::url, false);
    return Base64UrlResult{result.bytes, result.error_offset, static_cast<Base64UrlError>(result.error)};
}

} // namespace maicord
