// Dropped files on Windows (include/maicord/platform/window/dropped_files.hpp): the HDROP of WM_DROPFILES is the
// DroppedFiles itself, so the conversions cost nothing and DragFinish is its release.
#include "maicord/platform/window/dropped_files.hpp"

#include "maicord/sys/shell.hpp"

namespace maicord {
namespace {

HDROP drop_of(const DroppedFiles* files) noexcept {
    return reinterpret_cast<HDROP>(const_cast<DroppedFiles*>(files));
}

} // namespace

u32 dropped_files_count(const DroppedFiles* files) noexcept {
    return files ? DragQueryFileW(drop_of(files), 0xFFFFFFFFu, nullptr, 0) : 0;
}

u32 dropped_files_path_length(const DroppedFiles* files, u32 index) noexcept {
    return files ? DragQueryFileW(drop_of(files), index, nullptr, 0) : 0;
}

// The system writes a terminator after the path, so the path must be shorter than `output`.
u32 dropped_files_path(const DroppedFiles* files, u32 index, const Slice<wchar_t>& output) noexcept {
    if (!files || !output.data || !output.count)
        return 0;
    const u32 units = DragQueryFileW(drop_of(files), index, nullptr, 0);
    if (!units || units >= output.count)
        return 0;
    return DragQueryFileW(drop_of(files), index, output.data, output.count) == units ? units : 0;
}

void dropped_files_release(DroppedFiles* files) noexcept {
    if (files)
        DragFinish(drop_of(files));
}

} // namespace maicord
