#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Files dropped on the window; one owner, released with dropped_files_release (replaces DragFinish).
struct DroppedFiles;

u32 dropped_files_count(const DroppedFiles* files) noexcept;
// The length in UTF-16 code units of the path of file `index`; zero when the index is out of range.
u32 dropped_files_path_length(const DroppedFiles* files, u32 index) noexcept;
// Copies the path of file `index` into `output` and returns its length in code units, or zero when the index is
// out of range or the path does not fit. A backend may write a terminator after the path when there is room (Windows
// does, and needs the room): size the buffer for dropped_files_path_length + 1 and write your own terminator.
u32 dropped_files_path(const DroppedFiles* files, u32 index, const Slice<wchar_t>& output) noexcept;
void dropped_files_release(DroppedFiles* files) noexcept;

} // namespace maicord
