#pragma once
#include "maicord/core/utf.hpp"

namespace maicord {
inline constexpr u32 kHttpsUploadMaximumUrlBytes = 8192;
// Views borrow the supplied URL. Only HTTPS on a DNS host at port 443 is
// accepted. Signed query bytes are preserved verbatim; never log this URL.
struct HttpsUploadUrl { Utf8View host; Utf8View target; bool valid; };
HttpsUploadUrl https_upload_parse_url(Utf8View url) noexcept;
// The PUT's Content-Type. Voice messages upload as audio/ogg; everything else
// is an opaque octet stream.
enum class HttpsUploadContentType : u8 { octet_stream, audio_ogg };
// Credential-free PUT, with an explicit body size and no redirects/cookies.
u32 https_upload_write_head(Utf8View url, u64 file_bytes, Slice<u8> output,
    HttpsUploadContentType content_type = HttpsUploadContentType::octet_stream) noexcept;
}
