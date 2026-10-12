#pragma once
#include "maicord/protocol/attachment_upload.hpp"
namespace maicord {
// Compact process-local representation: headers and UTF-8 strings are copied
// together. No persisted/native pointers and no alignment requirement.
u32 message_attachment_storage_bytes(ConstSlice<UploadedAttachment> attachments, ConstSlice<u64> sizes = {}) noexcept;
bool message_attachment_storage_write(ConstSlice<UploadedAttachment> attachments, Slice<u8> output, ConstSlice<u64> sizes = {}) noexcept;
bool message_attachment_storage_read(ConstSlice<u8> storage, Slice<UploadedAttachment> output, u32* count, Slice<u64> sizes = {}) noexcept;
}
