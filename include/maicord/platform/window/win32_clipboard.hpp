#pragma once

// The Windows clipboard's own vocabulary, for the code that must name its formats: the test sink behind
// clipboard_write_utf8 and clipboard_write_image (platform/window/clipboard.hpp), and the CF_HTML builder.
#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/window/clipboard.hpp"

namespace maicord {

// Every write is one replace-all transaction: all formats are published together or the clipboard is left alone. The
// default sink is the Win32 clipboard; tests install a capturing sink so they never touch the user's real clipboard.
// When publish returns true the sink owns the HGLOBAL.
struct ClipboardSink {
    void* context;
    bool (*open)(void* context) noexcept;  // opens and empties
    bool (*publish)(void* context, UINT format, HGLOBAL data) noexcept;
    void (*close)(void* context) noexcept;
};
// A sink without callbacks restores the system clipboard. Returns the prior sink.
ClipboardSink clipboard_sink_install(ClipboardSink sink) noexcept;
struct ClipboardPayload {
    UINT format;
    HGLOBAL data;  // GMEM_MOVEABLE; ownership passes to clipboard_commit
};
// Publishes every payload or none. Frees each handle the sink did not take.
bool clipboard_commit(const Slice<ClipboardPayload>& payloads) noexcept;

// Registered "HTML Format" (CF_HTML) holding one <img> fragment for the image's source URL, so rich editors paste a
// linked image. Returns the payload bytes (no terminator), or zero when the URL or capacity is invalid.
inline constexpr u32 kClipboardHtmlMaximumUrlBytes = 4096;
inline constexpr u32 kClipboardHtmlImageCapacity = 256 + kClipboardHtmlMaximumUrlBytes * 6;
u32 clipboard_html_image_fragment(const Utf8View& source_url, const Slice<char>& output) noexcept;

} // namespace maicord
