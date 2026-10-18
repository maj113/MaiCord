#pragma once
#include "maicord/platform/upload_source.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/ole.hpp"

struct IStream;
namespace maicord {
// sequence is the Win32 clipboard sequence at capture (zero for providers that
// are not the live clipboard); image formats fall back to the Win32 clipboard
// only while it still holds the captured contents.
struct ClipboardAttachmentData { IStream* object; DWORD sequence; };
bool clipboard_attachment_available() noexcept;
// Retains/marshals the data object, without copying attachment payloads.
bool clipboard_attachment_capture(ClipboardAttachmentData* output) noexcept;
void clipboard_attachment_release(ClipboardAttachmentData* input) noexcept;
void clipboard_attachment_shutdown() noexcept;
// Called on an initialized MTA. Successful visit transfers one source reference.
using ClipboardAttachmentVisitor = bool (*)(void*, ConstSlice<wchar_t> path,
    ConstSlice<wchar_t> filename, UploadSource source) noexcept;
DWORD clipboard_attachment_visit(ClipboardAttachmentData* input, u32 maximum_files, u64 maximum_bytes,
    ClipboardAttachmentVisitor visitor, void* context, u32* count) noexcept;
}
