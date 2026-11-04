#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/window/dropped_files.hpp"

// Dropped and copied files arrive as a text/uri-list (or GNOME's x-special/gnome-copied-files): one file: URI a line,
// percent-encoded UTF-8. This turns the text into the paths the app takes (Linux; no connection involved, so tests feed
// it strings).

namespace maicord {

inline constexpr u32 kMaxDroppedFiles = 256;

// The files of one drop or one clipboard, as UTF-16 paths in one block.
struct DroppedFiles {
    u32 count;
    u32 start[kMaxDroppedFiles];   // of path i in `characters`
    u32 length[kMaxDroppedFiles];  // in code units, without a terminator
    wchar_t* characters;
};

} // namespace maicord

namespace maicord::wayland {

// Decodes %XX escapes of `length` bytes into `output` (`capacity` bytes); false for a malformed escape, an encoded NUL
// or a result that does not fit. *written is the decoded length.
bool uri_percent_decode(const char* text, u32 length, char* output, u32 capacity, u32* written) noexcept;

// The local path of a file: URI ("file:///a%20b" and "file://localhost/a%20b" are "/a b"); false for another scheme or
// another host, or when it does not fit.
bool uri_file_path(const char* uri, u32 length, char* output, u32 capacity, u32* written) noexcept;

// The files of a uri-list body (comment lines and a leading "copy" or "cut" line are skipped), at most
// kMaxDroppedFiles of them, in a heap block the caller releases with dropped_files_release; null when it names none.
DroppedFiles* uri_list_files(const u8* text, u32 bytes) noexcept;

} // namespace maicord::wayland
