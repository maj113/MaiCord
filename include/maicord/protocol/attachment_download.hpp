#pragma once

#include "maicord/protocol/cdn_url.hpp"
#include "maicord/protocol/http1.hpp"
#include "maicord/protocol/http_response_stream.hpp"

namespace maicord {

enum class AttachmentDownloadRequestError : u8 {
    none, invalid_input, invalid_url, origin_mismatch, expired, output_capacity,
};
struct AttachmentDownloadRequestResult {
    u32 request_bytes;
    CdnHost host;
    AttachmentDownloadRequestError error;
    constexpr bool ok() const noexcept { return error == AttachmentDownloadRequestError::none; }
};

// The source channel is the original channel for a forwarded snapshot. This
// writes the exact signed origin-form target, never a preview rewrite, and
// supplies neither account credentials nor a Cookie/Authorization header.
AttachmentDownloadRequestResult attachment_download_write_request(
    Utf8View signed_url, u64 source_channel_id, u64 attachment_id,
    u64 now_seconds, Slice<u8> output) noexcept;
// The same credential-free GET for embed media: a URL on Discord's CDN or
// media proxy (images-ext-N, media.discordapp.net), never another host.
AttachmentDownloadRequestResult attachment_download_write_media_request(Utf8View url,
                                                                        Slice<u8> output) noexcept;

enum class AttachmentDownloadResponseError : u8 {
    none, invalid_input, http_head, status, content_encoding, size_limit, size_mismatch,
};
struct AttachmentDownloadResponsePolicy {
    u64 expected_size;
    u64 maximum_size;
    AttachmentDownloadResponseError error;
};

// HttpResponseHeadParser callback. Only HTTP 200 with absent or a single
// identity Content-Encoding is accepted. Content-Length, if present, must
// match the source attachment size; with kHttpResponseLengthFromHead as the
// expected size it must be within the maximum, and absent only for a chunked
// body. The streaming decoder verifies the body.
HttpHeadResult attachment_download_parse_response_head(
    void* context, ConstSlice<u8> input, u32 maximum_header_bytes,
    u32 maximum_fields) noexcept;

inline constexpr u32 kAttachmentDownloadMaximumFilenameBytes = 2048;
inline constexpr u32 kAttachmentDownloadMaximumSuggestedFilenameBytes = 512;
enum class AttachmentDownloadFilenameError : u8 { none, invalid_input, output_capacity };
struct AttachmentDownloadFilenameResult {
    u32 bytes;
    AttachmentDownloadFilenameError error;
    bool risky;
    bool changed;
    constexpr bool ok() const noexcept { return error == AttachmentDownloadFilenameError::none; }
};

// Produces one NUL-terminated Windows filename component from the attachment
// metadata name (not an encoded URL). Replaces separators, ADS punctuation,
// controls and bidi overrides; avoids device names and trailing dots/spaces.
// Truncation keeps whole UTF-8 scalars and an extension of up to 64 UTF-16
// units. Input is at most 2048 bytes; output at most 512 bytes and 240 UTF-16
// units (local policy, not a server limit).
AttachmentDownloadFilenameResult attachment_download_safe_filename(
    Utf8View metadata_filename, Slice<char> output) noexcept;

// Executable, installer, script and archive extensions treated as risky; a
// warning signal, never a statement that the server scanned bytes or that an
// unlisted file is safe to execute.
bool attachment_download_risk_extension(Utf8View filename) noexcept;

} // namespace maicord
