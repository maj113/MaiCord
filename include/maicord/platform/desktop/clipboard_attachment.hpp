#pragma once
#include "maicord/platform/desktop/upload_source.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "maicord/core/win32_types.hpp"

struct IStream;
namespace maicord {
// sequence is the Win32 clipboard sequence at capture (zero for providers that
// are not the live clipboard); image formats fall back to the Win32 clipboard
// only while it still holds the captured contents.
struct ClipboardCapture { IStream* object; DWORD sequence; };
// Retains/marshals the data object, without copying attachment payloads.
bool clipboard_attachment_capture(ClipboardCapture* output) noexcept;
void clipboard_attachment_release(ClipboardCapture* input) noexcept;
void clipboard_attachment_shutdown() noexcept;
// The attachment that exceeded `maximum_bytes`, as far as the clipboard states it: its byte count and name (a
// pasted image is "image.png"). Zero bytes when nothing was refused for size.
struct ClipboardAttachmentOversize { u64 bytes; u32 name_units; wchar_t name[260]; };
// Called on an initialized MTA. Successful visit transfers one source reference. A named file or image refused
// for size returns ERROR_FILE_TOO_LARGE and, when `oversize` is set, is recorded there (a path the visitor
// refuses is the visitor's to record).
using ClipboardAttachmentVisitor = bool (*)(void*, ConstSlice<wchar_t> path,
    ConstSlice<wchar_t> filename, UploadSource source) noexcept;
DWORD clipboard_attachment_visit(ClipboardCapture* input, u32 maximum_files, u64 maximum_bytes,
    ClipboardAttachmentVisitor visitor, void* context, u32* count,
    ClipboardAttachmentOversize* oversize = nullptr) noexcept;
}
