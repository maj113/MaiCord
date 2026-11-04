#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

enum class ClipboardTextError : u8 {
    none,
    invalid_input,
    unavailable,
    open_failed,
    data_failed,
    invalid_text,
    capacity,
};

struct ClipboardTextResult {
    u32 bytes;
    ClipboardTextError error;

    constexpr bool ok() const noexcept {
        return error == ClipboardTextError::none;
    }
};

// The system clipboard of the main window; no window handle or native memory in the interface.
ClipboardTextResult clipboard_read_utf8(const Slice<char>& output) noexcept;
bool clipboard_has_text() noexcept;
// One replace-all transaction: every format is published together or the clipboard is left alone.
bool clipboard_write_utf8(const Utf8View& text) noexcept;
// Copy Image from straight RGBA rows; a source URL adds a linked-image HTML format where the platform has one.
bool clipboard_write_image(const u8* rgba, u32 width, u32 height, u32 stride, const Utf8View& source_url = {}) noexcept;

// The attachments (files, images) on the clipboard at one moment, retained without copying their payloads.
struct ClipboardCapture;
// Null when the clipboard holds no attachment.
ClipboardCapture* clipboard_capture_attachment() noexcept;
void clipboard_capture_release(ClipboardCapture* capture) noexcept;

} // namespace maicord
