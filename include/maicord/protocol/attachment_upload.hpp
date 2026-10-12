#pragma once
#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/https_upload.hpp"

namespace maicord {
inline constexpr u32 kAttachmentUploadMaximumFilenameBytes = 1024;
inline constexpr u32 kAttachmentUploadMaximumUploadedFilenameBytes = 4096;
inline constexpr u32 kAttachmentUploadMaximumCount = 10;
struct AttachmentUploadWriteResult { u32 target_bytes; u32 body_bytes; bool ok; };
struct AttachmentUploadResponse { u32 url_bytes; u32 uploaded_filename_bytes; bool ok; };
struct UploadedAttachment {
    Utf8View filename;
    Utf8View uploaded_filename;
    Utf8View description;
    u32 id;
    bool spoiler;
    // Voice messages only: base64 of the u8 amplitude samples and the audible
    // duration, sent as waveform and duration_secs. Empty on ordinary files.
    Utf8View waveform{};
    u32 duration_ms{};
};
constexpr bool attachment_upload_is_voice(const UploadedAttachment& attachment) noexcept {
    return attachment.waveform.bytes != 0;
}
bool attachment_upload_valid_filename(Utf8View filename) noexcept;
AttachmentUploadWriteResult attachment_upload_write(u64 channel_id, u32 id, Utf8View filename,
    u64 file_bytes, Slice<u8> target, Slice<u8> body) noexcept;
// duration_secs: seconds with millisecond precision and no trailing zeros
// ("4.48"). Zero bytes on capacity failure.
u32 attachment_upload_write_duration(u32 duration_ms, Slice<char> output) noexcept;
RestRouteKey attachment_upload_route(u64 channel_id) noexcept;
// Exactly one matching response item. Output is usable only when ok is true.
AttachmentUploadResponse attachment_upload_parse_response(Utf8View body, u32 expected_id,
    Slice<char> url, Slice<char> uploaded_filename) noexcept;
bool attachment_upload_valid_reference(const UploadedAttachment& attachment) noexcept;
// Writes the complete JSON array; zero indicates invalid input/capacity.
u32 attachment_upload_write_references(ConstSlice<UploadedAttachment> attachments,
    Slice<u8> output) noexcept;
}
