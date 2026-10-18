#include "maicord/protocol/attachment_upload.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {
namespace {
bool valid_text(Utf8View value, u32 maximum, bool slash) noexcept {
    if (!value.data || !value.bytes || value.bytes > maximum || !utf8_validate(value).valid) return false;
    for (u32 i = 0; i < value.bytes; ++i) {
        const u8 c = static_cast<u8>(value.data[i]);
        if (c < 0x20 || c == 0x7f || (!slash && (c == '/' || c == '\\'))) return false;
    }
    return true;
}
struct Response {
    JsonIndexedDocument document;
    JsonString url, filename;
    u64 id;
    u8 fields;
    bool list;
    bool item;
};
bool field(void* context, JsonString name, JsonValue value) noexcept {
    auto& r = *static_cast<Response*>(context);
    u8 bit = 0;
    if (json_string_equals_ascii(name, utf8_literal("id"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("upload_url"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("upload_filename"))) bit = 4;
    else return true;
    if (r.fields & bit) return false;
    r.fields |= bit;
    if (bit == 1) return json_value_to_u64_or_string(value, &r.id);
    return json_value_as_string(value, bit == 2 ? &r.url : &r.filename);
}
bool item(void* context, u32 index, JsonValue value) noexcept {
    auto& r = *static_cast<Response*>(context);
    if (index || r.item || value.kind != JsonValueKind::object) return false;
    r.item = true;
    return json_visit_indexed_object(r.document, value, 8, field, &r).ok();
}
bool root(void* context, JsonString name, JsonValue value) noexcept {
    auto& r = *static_cast<Response*>(context);
    if (!json_string_equals_ascii(name, utf8_literal("attachments"))) return true;
    if (r.list || value.kind != JsonValueKind::array) return false;
    r.list = true;
    return json_visit_indexed_array(r.document, value, 8, item, &r).ok();
}
}
bool attachment_upload_valid_filename(Utf8View filename) noexcept {
    return valid_text(filename, kAttachmentUploadMaximumFilenameBytes, false);
}
AttachmentUploadWriteResult attachment_upload_write(u64 channel_id, u32 id, Utf8View filename,
    u64 file_bytes, Slice<u8> target, Slice<u8> body) noexcept {
    if (!channel_id || !file_bytes || !attachment_upload_valid_filename(filename)) return {};
    ProtocolWriter path{target, 0}, json{body, 0};
    if (!protocol_writer_append_literal(&path, "/api/v9/channels/") ||
        !protocol_writer_append_u64(&path, channel_id) ||
        !protocol_writer_append_literal(&path, "/attachments") ||
        !protocol_writer_append_literal(&json, "{\"files\":[{\"filename\":") ||
        !protocol_writer_append_json_string(&json, filename) ||
        !protocol_writer_append_literal(&json, ",\"file_size\":") ||
        !protocol_writer_append_u64(&json, file_bytes) ||
        !protocol_writer_append_literal(&json, ",\"id\":\"") ||
        !protocol_writer_append_u64(&json, id) ||
        !protocol_writer_append_literal(&json, "\",\"is_clip\":false}]}")) return {};
    return {path.used, json.used, true};
}
RestRouteKey attachment_upload_route(u64 channel_id) noexcept {
    return {0x41545455504c4full, channel_id, RestMethod::post, RestMajorParameter::channel};
}
AttachmentUploadResponse attachment_upload_parse_response(Utf8View body, u32 expected_id,
    Slice<char> url, Slice<char> uploaded_filename) noexcept {
    if (body.bytes > 32 * 1024 || !url.data || !uploaded_filename.data) return {};
    u32 structurals[512]{};
    const auto scan = json_scan_structurals_avx1(body, {structurals, 512});
    Response r{};
    r.document = {body, {structurals, scan.count}};
    if (!scan.ok() || !json_visit_root_object(body, r.document.structurals, 8, root, &r).ok() ||
        r.fields != 7 || r.id != expected_id) return {};
    const auto decoded_url = json_string_decode_utf8(r.url, url);
    const auto decoded_name = json_string_decode_utf8(r.filename, uploaded_filename);
    if (!decoded_url.ok() || !decoded_name.ok() ||
        !https_upload_parse_url({url.data, decoded_url.bytes}).valid ||
        !valid_text({uploaded_filename.data, decoded_name.bytes}, kAttachmentUploadMaximumUploadedFilenameBytes, true)) return {};
    return {decoded_url.bytes, decoded_name.bytes, true};
}
u32 attachment_upload_write_duration(u32 duration_ms, Slice<char> output) noexcept {
    char text[16];
    ProtocolWriter writer{{reinterpret_cast<u8*>(text), sizeof(text)}, 0};
    protocol_writer_append_u64(&writer, duration_ms / 1000);
    u32 fraction = duration_ms % 1000, digits = 3;
    while (fraction && fraction % 10 == 0) fraction /= 10, --digits;
    if (fraction) {
        char decimals[3];
        for (u32 i = digits; i-- > 0; fraction /= 10) decimals[i] = static_cast<char>('0' + fraction % 10);
        protocol_writer_append_literal(&writer, ".");
        protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(decimals), digits);
    }
    if (!output.data || output.count < writer.used) return 0;
    copy_bytes(output.data, text, writer.used);
    return writer.used;
}
bool attachment_upload_valid_reference(const UploadedAttachment& attachment) noexcept {
    // A voice reference carries both the waveform and a positive duration.
    if ((attachment.waveform.bytes != 0) != (attachment.duration_ms != 0) ||
        (attachment.waveform.bytes && !gateway_message_waveform_valid(attachment.waveform)))
        return false;
    return attachment_upload_valid_filename(attachment.filename) &&
        valid_text(attachment.uploaded_filename, kAttachmentUploadMaximumUploadedFilenameBytes, true) &&
        (!attachment.description.bytes || (attachment.description.data &&
          attachment.description.bytes <= 4096 && utf8_validate(attachment.description).valid &&
          utf8_validate(attachment.description).codepoints <= 1024));
}
u32 attachment_upload_write_references(ConstSlice<UploadedAttachment> attachments,
    Slice<u8> output) noexcept {
    if (!attachments.data || !attachments.count || attachments.count > kAttachmentUploadMaximumCount) return 0;
    for (u32 i = 0; i < attachments.count; ++i) {
        if (!attachment_upload_valid_reference(attachments[i])) return 0;
        for (u32 j = 0; j < i; ++j) if (attachments[i].id == attachments[j].id) return 0;
    }
    ProtocolWriter json{output, 0};
    if (!protocol_writer_append_literal(&json, "[")) return 0;
    for (u32 i = 0; i < attachments.count; ++i) {
        const auto& a = attachments[i];
        if ((i && !protocol_writer_append_literal(&json, ",")) ||
            !protocol_writer_append_literal(&json, "{\"id\":\"") ||
            !protocol_writer_append_u64(&json, a.id) ||
            !protocol_writer_append_literal(&json, "\",\"filename\":") ||
            !protocol_writer_append_json_string(&json, a.filename) ||
            !protocol_writer_append_literal(&json, ",\"uploaded_filename\":") ||
            !protocol_writer_append_json_string(&json, a.uploaded_filename) ||
            (a.description.bytes && (!protocol_writer_append_literal(&json, ",\"description\":") ||
                !protocol_writer_append_json_string(&json, a.description))) ||
            (a.spoiler && !protocol_writer_append_literal(&json, ",\"is_spoiler\":true"))) return 0;
        if (attachment_upload_is_voice(a)) {
            char duration[16];
            const u32 duration_bytes = attachment_upload_write_duration(a.duration_ms, {duration, sizeof(duration)});
            if (!duration_bytes || !protocol_writer_append_literal(&json, ",\"duration_secs\":") ||
                !protocol_writer_append_bytes(&json, reinterpret_cast<const u8*>(duration), duration_bytes) ||
                !protocol_writer_append_literal(&json, ",\"waveform\":") ||
                !protocol_writer_append_json_string(&json, a.waveform)) return 0;
        }
        if (!protocol_writer_append_literal(&json, "}")) return 0;
    }
    return protocol_writer_append_literal(&json, "]") ? json.used : 0;
}
}
