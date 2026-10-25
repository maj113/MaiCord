#include "maicord/protocol/https_upload.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/protocol/client_user_agent.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
HttpsUploadUrl https_upload_parse_url(Utf8View url) noexcept {
    if (!url.data || url.bytes < 12 || url.bytes > kHttpsUploadMaximumUrlBytes) return {};
    static constexpr char prefix[] = "https://";
    for (u32 i = 0; i < 8; ++i) if (url.data[i] != prefix[i]) return {};
    u32 end = 8;
    while (end < url.bytes && url.data[end] != '/') ++end;
    if (end == url.bytes || end - 8 > 253) return {};
    u32 label = 0, dots = 0;
    bool last_letter = false;
    for (u32 i = 8; i < end; ++i) {
        const char c = url.data[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (c == '.') {
            if (!label || url.data[i - 1] == '-') return {};
            label = 0; ++dots;
        } else {
            if ((!letter && (c < '0' || c > '9') && c != '-') ||
                (!label && c == '-') || ++label > 63) return {};
        }
        last_letter = letter;
    }
    if (!dots || !label || !last_letter) return {};
    for (u32 i = end; i < url.bytes; ++i) {
        const u8 c = static_cast<u8>(url.data[i]);
        if (c <= 0x20 || c > 0x7e || c == '#' || c == '\\') return {};
        if (c == '%') {
            if (url.bytes - i < 3) return {};
            for (u32 j = 1; j <= 2; ++j) {
                const char h = url.data[i + j];
                if (!ascii_is_hex_digit(h)) return {};
            }
            i += 2;
        }
    }
    return {{url.data + 8, end - 8}, {url.data + end, url.bytes - end}, true};
}
u32 https_upload_write_head(Utf8View url, u64 file_bytes, Slice<u8> output,
    HttpsUploadContentType content_type) noexcept {
    const auto parsed = https_upload_parse_url(url);
    if (!parsed.valid || !file_bytes) return 0;
    ProtocolWriter writer{output, 0};
    return protocol_writer_append_literal(&writer, "PUT ") &&
        protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(parsed.target.data), parsed.target.bytes) &&
        protocol_writer_append_literal(&writer, " HTTP/1.1\r\nHost: ") &&
        protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(parsed.host.data), parsed.host.bytes) &&
        protocol_writer_append_literal(&writer, "\r\nUser-Agent: ") &&
        protocol_writer_append_literal(&writer, kClientUserAgent) &&
        protocol_writer_append_literal(&writer, "\r\nContent-Type: ") &&
        (content_type == HttpsUploadContentType::audio_ogg
             ? protocol_writer_append_literal(&writer, "audio/ogg")
             : protocol_writer_append_literal(&writer, "application/octet-stream")) &&
        protocol_writer_append_literal(&writer, "\r\nContent-Length: ") &&
        protocol_writer_append_u64(&writer, file_bytes) &&
        protocol_writer_append_literal(&writer, "\r\nConnection: close\r\n\r\n") ? writer.used : 0;
}
}
