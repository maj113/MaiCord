#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

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

ClipboardTextResult clipboard_read_utf8(HWND owner, Slice<char> output) noexcept;

// Every write is one replace-all transaction: all formats are published
// together or the clipboard is left alone. The default sink is the Win32
// clipboard; tests install a capturing sink so they never touch the user's
// real clipboard. When publish returns true the sink owns the HGLOBAL.
struct ClipboardSink {
    void* context;
    bool (*open)(void* context, HWND owner) noexcept; // opens and empties
    bool (*publish)(void* context, UINT format, HGLOBAL data) noexcept;
    void (*close)(void* context) noexcept;
};
// A sink without callbacks restores the system clipboard. Returns the prior sink.
ClipboardSink clipboard_sink_install(ClipboardSink sink) noexcept;
struct ClipboardPayload {
    UINT format;
    HGLOBAL data; // GMEM_MOVEABLE; ownership passes to clipboard_commit
};
// Publishes every payload or none. Frees each handle the sink did not take.
bool clipboard_commit(HWND owner, Slice<ClipboardPayload> payloads) noexcept;

bool clipboard_write_utf8(HWND owner, Utf8View text) noexcept;
// Registered "HTML Format" (CF_HTML) holding one <img> fragment for the
// image's source URL, so rich editors paste a linked image. Returns the
// payload bytes (no terminator), or zero when the URL or capacity is invalid.
inline constexpr u32 kClipboardHtmlMaximumUrlBytes = 4096;
inline constexpr u32 kClipboardHtmlImageCapacity = 256 + kClipboardHtmlMaximumUrlBytes * 6;
u32 clipboard_html_image_fragment(Utf8View source_url, Slice<char> output) noexcept;
// Copy Image: CF_DIBV5 (alpha-aware BGRA) and "PNG" from straight RGBA rows,
// plus "HTML Format" when a source URL is known.
bool clipboard_write_image(HWND owner, const u8* rgba, u32 width, u32 height, u32 stride,
                           Utf8View source_url = {}) noexcept;

} // namespace maicord
