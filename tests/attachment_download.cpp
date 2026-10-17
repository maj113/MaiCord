#include "maicord/protocol/attachment_download.hpp"
#include "maicord/protocol/http_response_stream.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include <initializer_list>

namespace maicord { namespace {
ConstSlice<u8> bytes(Utf8View value) noexcept {
    return {reinterpret_cast<const u8*>(value.data), value.bytes};
}
bool same(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
} // namespace

void test_attachment_download(void (*check)(bool, const char*) noexcept) noexcept {
    u8 request[2304]{};
    const auto url = utf8_literal("https://cdn.discordapp.com/attachments/77/42/report%20final.zip?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    const auto result = attachment_download_write_request(url, 77, 42, 2, {request, sizeof(request)});
    const auto expected = utf8_literal("GET /attachments/77/42/report%20final.zip?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef HTTP/1.1\r\nHost: cdn.discordapp.com\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
    check(result.ok() && result.host == CdnHost::cdn && result.request_bytes == expected.bytes &&
        bytes_equal(request, expected.data, expected.bytes),
        "attachment GET preserves signed target byte-for-byte and contains no credentials or range/preview transform");
    const auto ephemeral = attachment_download_write_request(utf8_literal("https://media.discordapp.net/ephemeral-attachments/77/42/photo.png?is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef&ex=ffffffff"),
        77, 42, 2, {request, sizeof(request)});
    check(ephemeral.ok() && ephemeral.host == CdnHost::media,
        "attachment GET admits signed ephemeral origin using original snapshot channel identity");
    check(attachment_download_write_request(url, 78, 42, 2, {request, sizeof(request)}).error == AttachmentDownloadRequestError::origin_mismatch &&
        !attachment_download_write_request(url, 77, 43, 2, {request, sizeof(request)}).ok() &&
        attachment_download_write_request(url, 77, 42, 0xffffffffull, {request, sizeof(request)}).error == AttachmentDownloadRequestError::expired &&
        !attachment_download_write_request(url, 0, 42, 2, {request, sizeof(request)}).ok(),
        "attachment GET rejects wrong original channel/id, expired authority and missing origin identity");
    bool unsafe_url = true;
    for (const auto invalid : {
        utf8_literal("http://cdn.discordapp.com/attachments/77/42/f?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://user@cdn.discordapp.com/attachments/77/42/f?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://cdn.discordapp.com:443/attachments/77/42/f?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://cdn.discordapp.com.evil.test/attachments/77/42/f?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://cdn.discordapp.com/attachments/77/42/../f?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://cdn.discordapp.com/attachments/77/42/f?ex=ffffffff&ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
        utf8_literal("https://cdn.discordapp.com/attachments/77/42/f?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef#fragment"),
        utf8_literal("https://cdn.discordapp.com/attachments/77/42/f?ex=ffffffff&is=1&hm=nope"),
        utf8_literal("https://cdn.discordapp.com/attachments/77/42/f")}) {
        unsafe_url = unsafe_url && !attachment_download_write_request(invalid, 77, 42, 2, {request, sizeof(request)}).ok();
    }
    check(unsafe_url, "attachment GET rejects credentials, ports, foreign hosts, traversal, duplicate/missing/bad signatures and fragments");
    request[0] = 0xa5;
    const auto capacity = attachment_download_write_request(url, 77, 42, 2, {request, 4});
    check(capacity.error == AttachmentDownloadRequestError::output_capacity && request[0] == 0xa5,
        "attachment GET does not publish a partial request on insufficient capacity");
    const auto media = attachment_download_write_media_request(
        utf8_literal("https://media.discordapp.net/external/AbCdEf/https/media.tenor.com/abc/tenor.mp4"),
        {request, sizeof(request)});
    const auto media_get = utf8_literal(
        "GET /external/AbCdEf/https/media.tenor.com/abc/tenor.mp4 HTTP/1.1\r\nHost: "
        "media.discordapp.net\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
    check(media.ok() && media.host == CdnHost::media && media.request_bytes == media_get.bytes &&
              bytes_equal(request, media_get.data, media_get.bytes) &&
              attachment_download_write_media_request(
                  utf8_literal("https://images-ext-1.discordapp.net/external/AbC/https/example.com/a.gif"),
                  {request, sizeof(request)})
                      .host == CdnHost::images_ext_1 &&
              !attachment_download_write_media_request(
                   utf8_literal("https://media.tenor.com/abc/tenor.gif"), {request, sizeof(request)})
                   .ok(),
          "embed media GET uses Discord's media proxy target as is and refuses a direct third-party host");
    AttachmentDownloadResponsePolicy unsized{kHttpResponseLengthFromHead, 100,
                                             AttachmentDownloadResponseError::none};
    check(
        attachment_download_parse_response_head(
            &unsized, bytes(utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n")), 1024, 32)
                .ok() &&
            !attachment_download_parse_response_head(
                 &unsized, bytes(utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 101\r\n\r\n")), 1024, 32)
                 .ok() &&
            unsized.error == AttachmentDownloadResponseError::size_limit &&
            !attachment_download_parse_response_head(
                 &unsized, bytes(utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")),
                 1024, 32)
                 .ok() &&
            unsized.error == AttachmentDownloadResponseError::size_mismatch,
        "unsized response requires a Content-Length within the maximum");

    AttachmentDownloadResponsePolicy policy{5, 100, AttachmentDownloadResponseError::none};
    bool admitted = true;
    for (const auto wire : {utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Encoding: IDENTITY\r\n\r\n"),
            utf8_literal("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n")}) {
        admitted = admitted && attachment_download_parse_response_head(&policy, bytes(wire), 1024, 32).ok();
    }
    check(admitted, "attachment response admits200 opaque file bytes with fixed/chunked/close framing and identity encoding");
    bool rejected = true;
    for (const auto wire : {utf8_literal("HTTP/1.1 302 Found\r\nLocation: https://evil.test/file\r\n\r\n"),
            utf8_literal("HTTP/1.1 206 Partial Content\r\nContent-Length: 5\r\n\r\n"),
            utf8_literal("HTTP/1.1 403 Forbidden\r\nContent-Length: 5\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nContent-Encoding: identity, identity\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Encoding: identity\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nContent-Encoding:\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n"),
            utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 101\r\n\r\n")}) {
        const auto parsed = attachment_download_parse_response_head(&policy, bytes(wire), 1024, 32);
        rejected = rejected && parsed.state == HttpParseState::error && policy.error != AttachmentDownloadResponseError::none;
    }
    check(rejected, "attachment response rejects redirects/partial/error statuses, encoded bytes and inconsistent declared size");
    const auto partial = attachment_download_parse_response_head(&policy, bytes(utf8_literal("HTTP/1.1 200 OK\r\nContent-Length:")), 1024, 32);
    check(partial.state == HttpParseState::need_more && policy.error == AttachmentDownloadResponseError::none,
        "attachment admission accepts fragmented head without leaving a stale error");
    u8 stream_storage[2048]{};
    HttpResponseStream stream{};
    http_response_stream_initialize(&stream, {stream_storage, sizeof(stream_storage)}, 5, 100, 1024, 32,
        attachment_download_parse_response_head, &policy);
    const auto denied = http_response_stream_feed(&stream, bytes(utf8_literal("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\nhello")), true);
    check(denied.state == HttpResponseStreamState::failed && !denied.pending.count && !denied.body_bytes &&
        policy.error == AttachmentDownloadResponseError::content_encoding,
        "attachment admission rejects encoded payload before any borrowed output is written");
    http_response_stream_initialize(&stream, {stream_storage, sizeof(stream_storage)}, 5, 100, 1024, 32,
        attachment_download_parse_response_head, &policy);
    const auto forbidden = http_response_stream_feed(&stream, bytes(utf8_literal("HTTP/1.1 403 Forbidden\r\nContent-Length: 5\r\n\r\nerror")), true);
    check(!forbidden.ok() && !forbidden.pending.count && !forbidden.body_bytes && stream.parsed_head.status_code == 403,
        "attachment rejection retains server status for bounded diagnostics while withholding body output");
    policy = {0, 0, AttachmentDownloadResponseError::none};
    check(attachment_download_parse_response_head(&policy, bytes(utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")), 1024, 32).ok(),
        "attachment response accepts exact zero-size metadata");
    policy = {4294967301ull, 4294967301ull, AttachmentDownloadResponseError::none};
    check(attachment_download_parse_response_head(&policy, bytes(utf8_literal("HTTP/1.1 200 OK\r\nContent-Length: 4294967301\r\n\r\n")), 1024, 32).ok(),
        "attachment response compares source size with full u64 precision");

    char filename[514]{};
    const auto unchanged = attachment_download_safe_filename(utf8_literal("holiday photo.png"), {filename, sizeof(filename)});
    check(unchanged.ok() && !unchanged.changed && !unchanged.risky && same({filename, unchanged.bytes}, utf8_literal("holiday photo.png")),
        "attachment filename preserves ordinary metadata spelling");
    const auto unsafe = attachment_download_safe_filename(utf8_literal("..\\dir/report.exe:stream?* \r\n"), {filename, sizeof(filename)});
    check(unsafe.ok() && unsafe.changed && same({filename, unsafe.bytes}, utf8_literal(".._dir_report.exe_stream__ __")) &&
        filename[unsafe.bytes] == 0, "attachment filename flattens traversal/separators, ADS and control punctuation to one component");
    bool devices = true;
    for (const auto device : {utf8_literal("CON.txt"), utf8_literal("prn"), utf8_literal("aux.log"), utf8_literal("NUL.tar.gz"),
            utf8_literal("COM1.exe"), utf8_literal("lpt9"), utf8_literal("CON .txt"), utf8_literal("COM\xc2\xb9.log"), utf8_literal("LPT\xc2\xb2.txt")}) {
        const auto renamed = attachment_download_safe_filename(device, {filename, sizeof(filename)});
        devices = devices && renamed.ok() && renamed.changed && filename[0] == '_';
    }
    check(devices, "attachment filename avoids case-insensitive Windows devices including extensions and superscript COM/LPT digits");
    const auto trailing = attachment_download_safe_filename(utf8_literal("run.EXE.  "), {filename, sizeof(filename)});
    check(trailing.ok() && trailing.risky && same({filename, trailing.bytes}, utf8_literal("run.EXE")),
        "attachment filename strips trailing dots/spaces before risk classification");
    const auto empty = attachment_download_safe_filename(utf8_literal(".. "), {filename, sizeof(filename)});
    check(empty.ok() && same({filename, empty.bytes}, utf8_literal("attachment")), "empty or dots-only attachment name receives a safe fallback");
    const auto bidi = attachment_download_safe_filename(utf8_literal("photo\xe2\x80\xae.exe"), {filename, sizeof(filename)});
    check(bidi.ok() && bidi.risky && same({filename, bidi.bytes}, utf8_literal("photo_.exe")),
        "attachment filename removes bidi override that could disguise risky extension");
    const auto encoded = attachment_download_safe_filename(utf8_literal("file%2Fname%20final.txt"), {filename, sizeof(filename)});
    check(encoded.ok() && !encoded.changed, "attachment metadata filename is not URL-decoded a second time");
    char long_name[504]{};
    for (u32 i = 0; i < 500; ++i) long_name[i] = 'a';
    copy_bytes(long_name + 500, ".EXE", 4);
    const auto bounded = attachment_download_safe_filename({long_name, sizeof(long_name)}, {filename, sizeof(filename)});
    check(bounded.ok() && bounded.bytes == 240 && bounded.risky && bytes_equal(filename + bounded.bytes - 4, ".EXE", 4),
        "filename truncation retains risky final extension rather than concealing it");
    for (u32 i = 0; i < 125; ++i) copy_bytes(long_name + i * 4, "\xf0\x9f\x98\x80", 4);
    const auto unicode = attachment_download_safe_filename({long_name, sizeof(long_name)}, {filename, sizeof(filename)});
    check(unicode.ok() && unicode.bytes == 476 && unicode.risky && utf8_validate({filename, unicode.bytes}).valid,
        "filename truncation preserves UTF8 scalars and counts surrogate pairs as two UTF16 units");
    char cjk_name[724]{};
    for (u32 i = 0; i < 240; ++i) copy_bytes(cjk_name + i * 3, "\xe6\x96\x87", 3);
    copy_bytes(cjk_name + 720, ".exe", 4);
    const auto cjk = attachment_download_safe_filename({cjk_name, sizeof(cjk_name)}, {filename, sizeof(filename)});
    check(cjk.ok() && cjk.bytes == 511 && cjk.risky && utf8_validate({filename, cjk.bytes}).valid &&
        bytes_equal(filename + cjk.bytes - 4, ".exe", 4),
        "filename policy admits long Chinese metadata and bounds suggested UTF8 bytes without hiding suffix");
    filename[0] = 'z';
    const char invalid_utf8[]{static_cast<char>(0xc0), static_cast<char>(0xaf)};
    check(attachment_download_safe_filename({invalid_utf8, sizeof(invalid_utf8)}, {filename, sizeof(filename)}).error == AttachmentDownloadFilenameError::invalid_input &&
        attachment_download_safe_filename(utf8_literal("file.txt"), {filename, 4}).error == AttachmentDownloadFilenameError::output_capacity && filename[0] == 'z',
        "filename rejection and insufficient output do not publish partial names");
    char aliased[32]{};
    copy_bytes(aliased, "name:file.txt", 13);
    const auto alias = attachment_download_safe_filename({aliased, 13}, {aliased, sizeof(aliased)});
    check(alias.ok() && alias.changed && same({aliased, alias.bytes}, utf8_literal("name_file.txt")),
        "filename sanitizer safely supports shared input/output and retains change diagnostic");
    bool extensions = true;
    for (const auto name : {utf8_literal("file.ExE"), utf8_literal("file.zip"), utf8_literal("file.ps1"), utf8_literal("file.docm"),
            utf8_literal("file.printerexport"), utf8_literal(".lnk"), utf8_literal("file.msixbundle")})
        extensions = extensions && attachment_download_risk_extension(name);
    for (const auto name : {utf8_literal("file.png"), utf8_literal("file.txt"), utf8_literal("file.exe.png"), utf8_literal("no-extension")})
        extensions = extensions && !attachment_download_risk_extension(name);
    check(extensions, "risk policy matches case-insensitive last suffix and the official "
                      "archive/script/macro/package list");
}
} // namespace maicord
