#include "maicord/app/message_attachment_storage.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {
namespace {
struct Header { u32 filename; u32 uploaded; u32 description; u32 id; u32 spoiler; u32 waveform; u32 duration_ms; };
}
u32 message_attachment_storage_bytes(ConstSlice<UploadedAttachment> attachments, ConstSlice<u64> sizes) noexcept {
    if (!attachments.data || !attachments.count || attachments.count > kAttachmentUploadMaximumCount) return 0;
    if (sizes.count && (!sizes.data || sizes.count != attachments.count)) return 0;
    u32 bytes = 0;
    for (u32 i = 0; i < attachments.count; ++i) {
        const auto& a = attachments[i];
        if (!attachment_upload_valid_reference(a)) return 0;
        for (u32 j = 0; j < i; ++j) if (attachments[j].id == a.id) return 0;
        bytes += static_cast<u32>(sizeof(Header)) + (sizes.count ? 8u : 0u) + a.filename.bytes + a.uploaded_filename.bytes +
            a.description.bytes + a.waveform.bytes;
    }
    return bytes;
}
bool message_attachment_storage_write(ConstSlice<UploadedAttachment> attachments, Slice<u8> output, ConstSlice<u64> sizes) noexcept {
    const u32 bytes = message_attachment_storage_bytes(attachments, sizes);
    if (!bytes || !output.data || output.count < bytes) return false;
    u32 used = 0;
    for (u32 i = 0; i < attachments.count; ++i) {
        const auto& a = attachments[i];
        const Header header{a.filename.bytes, a.uploaded_filename.bytes, a.description.bytes, a.id,
            (a.spoiler ? 1u : 0u) | (sizes.count ? 2u : 0u), a.waveform.bytes, a.duration_ms};
        copy_bytes(output.data + used, &header, sizeof(header)); used += sizeof(header);
        if (sizes.count) { copy_bytes(output.data + used, &sizes[i], 8); used += 8; }
        const Utf8View strings[]{a.filename, a.uploaded_filename, a.description, a.waveform};
        for (const Utf8View text : strings) {
            copy_bytes(output.data + used, text.data, text.bytes); used += text.bytes;
        }
    }
    return true;
}
bool message_attachment_storage_read(ConstSlice<u8> storage, Slice<UploadedAttachment> output, u32* count, Slice<u64> sizes) noexcept {
    if (count) *count = 0;
    if (!count || !storage.data || !storage.count || !output.data || (sizes.count && !sizes.data)) return false;
    u32 used = 0, items = 0;
    while (used < storage.count) {
        if (items >= output.count || items >= kAttachmentUploadMaximumCount || sizeof(Header) > storage.count - used) return false;
        Header h{}; copy_bytes(&h, storage.data + used, sizeof(h)); used += sizeof(h);
        if (h.spoiler > 3 || (sizes.count && items >= sizes.count)) return false;
        u64 size = 0;
        if (h.spoiler & 2u) {
            if (storage.count - used < 8) return false;
            copy_bytes(&size, storage.data + used, 8); used += 8;
        }
        if (sizes.count) sizes[items] = size;
        const u64 bytes = static_cast<u64>(h.filename) + h.uploaded + h.description + h.waveform;
        if (bytes > storage.count - used) return false;
        const char* strings = reinterpret_cast<const char*>(storage.data + used);
        output[items] = {{strings, h.filename}, {strings + h.filename, h.uploaded},
            {strings + h.filename + h.uploaded, h.description}, h.id, (h.spoiler & 1u) != 0,
            {strings + h.filename + h.uploaded + h.description, h.waveform}, h.duration_ms};
        if (!attachment_upload_valid_reference(output[items])) return false;
        for (u32 j = 0; j < items; ++j) if (output[j].id == h.id) return false;
        used += static_cast<u32>(bytes); ++items;
    }
    *count = items;
    return true;
}
}
