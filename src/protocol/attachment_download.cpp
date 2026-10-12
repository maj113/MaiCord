#include "maicord/protocol/attachment_download.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"

namespace maicord { namespace {
HttpHeadResult rejected(AttachmentDownloadResponsePolicy* p,
                        AttachmentDownloadResponseError error, HttpResponseHead head = {}) noexcept {
    p->error = error;
    return {head, 0, HttpError::visitor_rejected, HttpParseState::error};
}
struct EncodingCheck { bool seen; bool valid; };
bool visit_encoding(void* context, HttpField field) noexcept {
    auto* check = static_cast<EncodingCheck*>(context);
    if (!http_ascii_equal_case_insensitive(field.name, "content-encoding")) return true;
    if (check->seen || !http_ascii_equal_case_insensitive(field.value, "identity")) {
        check->valid = false;
        return false;
    }
    check->seen = true;
    return true;
}
bool equal_ascii(Utf8View text, const char* literal, u32 count) noexcept {
    if (text.bytes != count) return false;
    for (u32 i = 0; i < count; ++i)
        if (ascii_lower(text.data[i]) != literal[i]) return false;
    return true;
}
bool reserved(Utf8View name) noexcept {
    u32 end = 0;
    while (end < name.bytes && name.data[end] != '.') ++end;
    while (end && name.data[end - 1] == ' ') --end;
    const Utf8View base{name.data, end};
    if (equal_ascii(base, "con", 3) || equal_ascii(base, "prn", 3) || equal_ascii(base, "aux", 3) ||
        equal_ascii(base, "nul", 3) || equal_ascii(base, "clock$", 6)) return true;
    if (end < 4 || (!equal_ascii({name.data, 3}, "com", 3) && !equal_ascii({name.data, 3}, "lpt", 3))) return false;
    if (end == 4) return name.data[3] >= '1' && name.data[3] <= '9';
    if (end == 5 && static_cast<u8>(name.data[3]) == 0xc2) {
        const u8 digit = static_cast<u8>(name.data[4]);
        return digit == 0xb9 || digit == 0xb2 || digit == 0xb3;
    }
    return false;
}
bool replaced(u32 cp) noexcept {
    return cp < 32 || cp == 127 || cp == '<' || cp == '>' || cp == ':' || cp == '"' ||
        cp == '/' || cp == '\\' || cp == '|' || cp == '?' || cp == '*' ||
        (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069);
}
u32 units(Utf8View text) noexcept {
    u32 total = 0;
    for (u32 i = 0; i < text.bytes;) {
        const auto scalar = utf8_decode_one(text.data + i, text.bytes - i);
        total += scalar.codepoint > 0xffff ? 2u : 1u;
        i += scalar.width;
    }
    return total;
}
u32 prefix_bytes(Utf8View text, u32 maximum_units, u32 maximum_bytes) noexcept {
    u32 used = 0, i = 0;
    while (i < text.bytes) {
        const auto scalar = utf8_decode_one(text.data + i, text.bytes - i);
        const u32 needed = scalar.codepoint > 0xffff ? 2u : 1u;
        if (needed > maximum_units - used || scalar.width > maximum_bytes - i) break;
        used += needed;
        i += scalar.width;
    }
    return i;
}
} // namespace

AttachmentDownloadRequestResult attachment_download_write_request(Utf8View url, u64 channel,
    u64 attachment, u64 now_seconds, Slice<u8> output) noexcept {
    if (!channel || !attachment || !url.data || !url.bytes || !output.data)
        return {0, CdnHost::none, AttachmentDownloadRequestError::invalid_input};
    u8 target[kCdnMaximumTargetBytes]{}, prefix[80]{}, request[kCdnMaximumTargetBytes + 192]{};
    const auto parsed = parse_signed_attachment_url(url, attachment, {target, sizeof(target)});
    if (!parsed.ok()) return {0, CdnHost::none, AttachmentDownloadRequestError::invalid_url};
    ProtocolWriter path{{prefix, sizeof(prefix)}, 0};
    const bool started = parsed.ephemeral
        ? protocol_writer_append_literal(&path, "/ephemeral-attachments/")
        : protocol_writer_append_literal(&path, "/attachments/");
    if (!started || !protocol_writer_append_u64(&path, channel) ||
        !protocol_writer_append_literal(&path, "/") || !protocol_writer_append_u64(&path, attachment) ||
        !protocol_writer_append_literal(&path, "/") || parsed.target_bytes <= path.used ||
        !bytes_equal(prefix, target, path.used))
        return {0, CdnHost::none, AttachmentDownloadRequestError::origin_mismatch};
    if (parsed.expires_at_seconds <= now_seconds)
        return {0, CdnHost::none, AttachmentDownloadRequestError::expired};
    ProtocolWriter writer{{request, sizeof(request)}, 0};
    const auto host = cdn_host_name(parsed.host);
    if (!protocol_writer_append_literal(&writer, "GET ") ||
        !protocol_writer_append_bytes(&writer, {target, parsed.target_bytes}) ||
        !protocol_writer_append_literal(&writer, " HTTP/1.1\r\nHost: ") ||
        !protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(host.data), host.bytes) ||
        !protocol_writer_append_literal(&writer, "\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n") ||
        writer.used > output.count)
        return {0, CdnHost::none, AttachmentDownloadRequestError::output_capacity};
    copy_bytes(output.data, request, writer.used);
    return {writer.used, parsed.host, AttachmentDownloadRequestError::none};
}

HttpHeadResult attachment_download_parse_response_head(void* context, ConstSlice<u8> input,
    u32 maximum_header, u32 maximum_fields) noexcept {
    auto* policy = static_cast<AttachmentDownloadResponsePolicy*>(context);
    if (!policy) return {{}, 0, HttpError::invalid_input, HttpParseState::error};
    policy->error = AttachmentDownloadResponseError::none;
    if (policy->expected_size > policy->maximum_size)
        return rejected(policy, AttachmentDownloadResponseError::size_limit);
    EncodingCheck encoding{false, true};
    const auto parsed = http_parse_response_head(input, maximum_header, maximum_fields, visit_encoding, &encoding);
    if (parsed.state == HttpParseState::error) {
        policy->error = encoding.valid ? AttachmentDownloadResponseError::http_head
                                     : AttachmentDownloadResponseError::content_encoding;
        return parsed;
    }
    if (!parsed.ok()) return parsed;
    if (parsed.head.status_code != 200) return rejected(policy, AttachmentDownloadResponseError::status, parsed.head);
    if (parsed.head.has_content_length && parsed.head.content_length > policy->maximum_size)
        return rejected(policy, AttachmentDownloadResponseError::size_limit, parsed.head);
    if (parsed.head.has_content_length && parsed.head.content_length != policy->expected_size)
        return rejected(policy, AttachmentDownloadResponseError::size_mismatch, parsed.head);
    return parsed;
}

bool attachment_download_risk_extension(Utf8View name) noexcept {
    if (!name.data || !name.bytes || name.bytes > kAttachmentDownloadMaximumFilenameBytes + 1) return false;
    u32 end = name.bytes;
    while (end && (name.data[end - 1] == '.' || name.data[end - 1] == ' ')) --end;
    u32 dot = end;
    while (dot && name.data[dot - 1] != '.') --dot;
    if (!dot || dot == end) return false;
    const Utf8View extension{name.data + dot, end - dot};
    // Executable, installer, script and archive extensions that count as risky downloads.
    static constexpr char set[] =
        "7z|ade|adp|arj|apk|appimage|application|appx|appxbundle|asx|bas|bat|cab|cer|chm|cmd|cnt|cpl|crt|csh|deb|der|diagcab|dll|dmg|docm|dotm|ex|ex_|exe|flatpak|flatpakref|fxp|gadget|grp|gz|hlp|hpj|hta|htc|inf|ins|ipa|iso|isp|its|jar|jnlp|jse|ksh|lib|lnk|mad|maf|mag|mam|maq|mar|mas|mat|mau|mav|maw|mcf|mda|mdb|mde|mdt|mdw|mdz|msc|msh|msh1|msh1xml|msh2|msh2xml|mshxml|msi|msix|msixbundle|msp|mst|msu|nsh|ops|osd|pcd|pif|pkg|pl|plg|potm|ppam|ppsm|pptm|prf|prg|printerexport|ps1|ps1xml|ps2|ps2xml|psc1|psc2|psd1|psdm1|pst|py|pyc|pyo|pyw|pyz|pyzw|rar|reg|rpm|scf|scr|sct|shb|shs|sldm|snap|sys|theme|tmp|url|vb|vbe|vbp|vbs|vhd|vhdx|vsmacros|vsw|vxd|webpnp|ws|wsc|wsf|wsh|xbap|xlam|xll|xlsm|xltm|xnk|z|zip";
    u32 start = 0;
    for (u32 i = 0; i < sizeof(set); ++i) {
        if (set[i] == '|' || !set[i]) {
            if (equal_ascii(extension, set + start, i - start)) return true;
            start = i + 1;
        }
    }
    return false;
}

AttachmentDownloadFilenameResult attachment_download_safe_filename(Utf8View name, Slice<char> output) noexcept {
    if ((!name.data && name.bytes) || name.bytes > kAttachmentDownloadMaximumFilenameBytes ||
        !utf8_validate(name).valid || !output.data)
        return {0, AttachmentDownloadFilenameError::invalid_input, false, false};
    char safe[kAttachmentDownloadMaximumFilenameBytes + 2]{};
    u32 used = 0;
    for (u32 i = 0; i < name.bytes;) {
        const auto scalar = utf8_decode_one(name.data + i, name.bytes - i);
        if (replaced(scalar.codepoint)) safe[used++] = '_';
        else { copy_bytes(safe + used, name.data + i, scalar.width); used += scalar.width; }
        i += scalar.width;
    }
    while (used && (safe[used - 1] == '.' || safe[used - 1] == ' ')) --used;
    if (!used) { copy_bytes(safe, "attachment", 10); used = 10; }
    if (reserved({safe, used})) {
        for (u32 i = used; i; --i) safe[i] = safe[i - 1];
        safe[0] = '_'; ++used;
    }
    if (units({safe, used}) > 240 || used > kAttachmentDownloadMaximumSuggestedFilenameBytes) {
        u32 suffix = used;
        while (suffix && safe[suffix - 1] != '.') --suffix;
        if (suffix) --suffix;
        const u32 suffix_units = units({safe + suffix, used - suffix});
        if (!suffix || suffix_units > 64) suffix = used;
        const u32 keep_units = suffix == used ? 240 : 240 - suffix_units;
        const u32 suffix_bytes = used - suffix;
        const u32 prefix = prefix_bytes({safe, suffix}, keep_units,
            kAttachmentDownloadMaximumSuggestedFilenameBytes - suffix_bytes);
        for (u32 i = 0; i < used - suffix; ++i) safe[prefix + i] = safe[suffix + i];
        used = prefix + used - suffix;
        while (used && (safe[used - 1] == '.' || safe[used - 1] == ' ')) --used;
    }
    if (used >= output.count)
        return {0, AttachmentDownloadFilenameError::output_capacity, false, false};
    const bool changed = used != name.bytes || !bytes_equal(safe, name.data, used);
    copy_bytes(output.data, safe, used);
    output.data[used] = 0;
    return {used, AttachmentDownloadFilenameError::none, attachment_download_risk_extension({safe, used}),
        changed};
}

} // namespace maicord
