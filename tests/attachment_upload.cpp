#include "maicord/protocol/attachment_upload.hpp"
#include "maicord/protocol/message_create.hpp"
#include "maicord/core/json_reader.hpp"

namespace maicord {
namespace {
bool equal(const u8* bytes, u32 count, Utf8View text) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(bytes), count}, text);
}
bool contains(const u8* bytes, u32 count, Utf8View text) noexcept {
    for (u32 i = 0; i <= count && text.bytes <= count - i; ++i)
        if (equal(bytes + i, text.bytes, text)) return true;
    return false;
}
}
void test_attachment_upload(void (*check)(bool, const char*) noexcept) noexcept {
    u8 target[128]{}, body[8192]{};
    const auto request = attachment_upload_write(123, 0, utf8_literal("a\"b.txt"), 4294967297ull,
        {target, sizeof(target)}, {body, sizeof(body)});
    check(request.ok && equal(target, request.target_bytes, utf8_literal("/api/v9/channels/123/attachments")) &&
        equal(body, request.body_bytes, utf8_literal("{\"files\":[{\"filename\":\"a\\\"b.txt\",\"file_size\":4294967297,\"id\":\"0\",\"is_clip\":false}]}")),
        "attachment upload request preserves escaped filenames, zero IDs, and 64-bit file sizes");
    const auto route = attachment_upload_route(123);
    check(route.method == RestMethod::post, "attachment URL request uses POST rate route");
    check(!attachment_upload_write(0, 0, utf8_literal("a"), 1, {target, 128}, {body, 8192}).ok &&
        !attachment_upload_write(1, 0, utf8_literal("a"), 0, {target, 128}, {body, 8192}).ok &&
        !attachment_upload_valid_filename(utf8_literal("../secret")) &&
        !attachment_upload_valid_filename(utf8_literal("a\\secret")) &&
        !attachment_upload_valid_filename(utf8_literal("a\r\nb")) &&
        !attachment_upload_write(1, 0, utf8_literal("a"), 1, {target, 2}, {body, 8192}).ok,
        "attachment request rejects missing channel, empty files, paths, controls, and insufficient capacity");
    constexpr auto url = utf8_literal("https://uploads.example.com/a%20b?sig=a%2Fb&x=1");
    const auto parsed = https_upload_parse_url(url);
    check(parsed.valid && parsed.host.bytes == 19 && parsed.target.bytes == 20,
        "signed HTTPS URL splits host and preserves opaque query bytes");
    const Utf8View bad_urls[] = {utf8_literal("http://uploads.example.com/a"), utf8_literal("https://user@uploads.example.com/a"),
        utf8_literal("https://uploads.example.com:443/a"), utf8_literal("https://127.0.0.1/a"),
        utf8_literal("https://localhost/a"), utf8_literal("https://uploads.example.com/a#b"),
        utf8_literal("https://uploads.example.com/a\r\nHost:x"), utf8_literal("https://uploads.example.com/a\\b"),
        utf8_literal("https://uploads.example.com/a%2"), utf8_literal("https://-uploads.example.com/a"),
        utf8_literal("https://uploads..example.com/a"), utf8_literal("https://uploads.example.com")};
    for (const auto invalid : bad_urls) check(!https_upload_parse_url(invalid).valid, "upload URL rejects unsafe or unsupported authority/target");
    const u32 head = https_upload_write_head(url, 4294967297ull, {body, sizeof(body)});
    check(head && contains(body, head, utf8_literal("PUT /a%20b?sig=a%2Fb&x=1 HTTP/1.1\r\n")) &&
        contains(body, head, utf8_literal("Content-Length: 4294967297\r\n")) &&
        contains(body, head, utf8_literal("Content-Type: application/octet-stream\r\n")) &&
        !contains(body, head, utf8_literal("Authorization")) && !contains(body, head, utf8_literal("Cookie")),
        "upload PUT contains explicit byte count and no account credentials");
    char decoded_url[8192]{}, filename[4096]{};
    constexpr auto response = utf8_literal("{\"attachments\":[{\"id\":\"0\",\"upload_url\":\"https:\\/\\/uploads.example.com/a?sig=x\",\"upload_filename\":\"uploads/id/a.txt\",\"future\":true}]}");
    const auto decoded = attachment_upload_parse_response(response, 0, {decoded_url, sizeof(decoded_url)}, {filename, sizeof(filename)});
    check(decoded.ok && equal(reinterpret_cast<const u8*>(decoded_url), decoded.url_bytes, utf8_literal("https://uploads.example.com/a?sig=x")),
        "attachment response decodes escaped signed URL and matching string ID");
    check(attachment_upload_parse_response(
        utf8_literal("{\"attachments\":[{\"id\":0,\"upload_url\":\"https://uploads.example.com/a\",\"upload_filename\":\"uploads/id/a.txt\"}]}"),
        0, {decoded_url, 8192}, {filename, 4096}).ok,
        "attachment response also accepts matching numeric zero ID");
    check(!attachment_upload_parse_response(response, 1, {decoded_url, 8192}, {filename, 4096}).ok &&
        !attachment_upload_parse_response(response, 0, {decoded_url, 2}, {filename, 4096}).ok,
        "attachment response rejects mismatched request ID and truncated destination");
    const Utf8View bad_responses[] = {utf8_literal("{\"attachments\":[]}"),
        utf8_literal("{\"attachments\":[{\"id\":0,\"id\":0,\"upload_url\":\"https://a.example/x\",\"upload_filename\":\"x\"}]}"),
        utf8_literal("{\"attachments\":[{\"id\":0,\"upload_url\":\"http://a.example/x\",\"upload_filename\":\"x\"}]}"),
        utf8_literal("{\"attachments\":[{\"id\":0,\"upload_url\":\"https://a.example/x\",\"upload_filename\":\"\"}]}"),
        utf8_literal("{\"attachments\":[{\"id\":0,\"upload_url\":\"https://a.example/x\",\"upload_filename\":\"x\"},{}]}"),
        utf8_literal("{\"attachments\":[],\"attachments\":[]}")};
    for (const auto invalid : bad_responses)
        check(!attachment_upload_parse_response(invalid, 0, {decoded_url, 8192}, {filename, 4096}).ok,
            "upload response requires exactly one complete unambiguous HTTPS item");
    const UploadedAttachment attachments[] = {
        {utf8_literal("a.txt"), utf8_literal("uploads/id/a.txt"), utf8_literal("Alt \"text\""), 0, true},
        {utf8_literal("b.txt"), utf8_literal("uploads/id/b.txt"), {}, 1, false}};
    const auto message = write_message_create_body({}, 55, 123, 44, {attachments, 2}, {body, sizeof(body)});
    u32 structurals[512]{};
    const Utf8View json{reinterpret_cast<const char*>(body), message.bytes};
    const auto scan = json_scan_structurals_avx1(json, {structurals, 512});
    check(message.ok() && scan.ok() && json_visit_root_object(json, {structurals, scan.count}, 8, nullptr, nullptr).ok() &&
        contains(body, message.bytes, utf8_literal("\"content\":\"\"")) &&
        contains(body, message.bytes, utf8_literal("\"uploaded_filename\":\"uploads/id/a.txt\"")) &&
        contains(body, message.bytes, utf8_literal("\"is_spoiler\":true")) &&
        contains(body, message.bytes, utf8_literal("\"enforce_nonce\":true")) &&
        contains(body, message.bytes, utf8_literal("\"mobile_network_type\":\"unknown\"")) &&
        contains(body, message.bytes, utf8_literal("\"message_id\":\"44\"")),
        "attachment-only reply preserves uploaded references, spoiler, description, and reply metadata");
    UploadedAttachment duplicate[]{attachments[0], attachments[0]};
    check(!write_message_create_body({}, 55, 0, 0, {duplicate, 2}, {body, sizeof(body)}).ok() &&
        !write_message_create_body({}, 55, {body, sizeof(body)}).ok() &&
        !write_message_create_body({}, 55, 0, 0, {attachments, 2}, {body, 10}).ok(),
        "message attachments reject duplicate IDs, wholly empty messages, and insufficient capacity");
}
}
