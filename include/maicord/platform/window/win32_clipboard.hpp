#pragma once

// The Windows clipboard's own vocabulary, for the code that must name its formats: the transaction behind
// clipboard_write_utf8 and clipboard_write_image (platform/window/clipboard.hpp), and the CF_HTML builder.
#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/window/clipboard.hpp"

namespace maicord {

// Every write is one replace-all transaction: all formats are published together or the clipboard is left alone. These
// three are the Win32 clipboard, alone in clipboard_system.cpp; tests link a capturing replacement so they never touch
// the user's clipboard. open also empties it; when publish returns true the clipboard owns the HGLOBAL.
bool clipboard_system_open() noexcept;
bool clipboard_system_publish(UINT format, HGLOBAL data) noexcept;
void clipboard_system_close() noexcept;

// Registered "HTML Format" (CF_HTML) holding one <img> fragment for the image's source URL, so rich editors paste a
// linked image. Returns the payload bytes (no terminator), or zero when the URL or capacity is invalid.
inline constexpr u32 kClipboardHtmlMaximumUrlBytes = 4096;
inline constexpr u32 kClipboardHtmlImageCapacity = 256 + kClipboardHtmlMaximumUrlBytes * 6;
u32 clipboard_html_image_fragment(const Utf8View& source_url, const Slice<char>& output) noexcept;

} // namespace maicord
