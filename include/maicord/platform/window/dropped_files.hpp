#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Files dropped on the window; one owner, released with dropped_files_release (replaces DragFinish).
struct DroppedFiles;

u32 dropped_files_count(const DroppedFiles* files) noexcept;
// Copies the path of file `index` (UTF-16) into `output` without a terminator and returns its length in code
// units, or zero when the index is out of range or the path does not fit.
u32 dropped_files_path(const DroppedFiles* files, u32 index, const Slice<wchar_t>& output) noexcept;
void dropped_files_release(DroppedFiles* files) noexcept;

} // namespace maicord
