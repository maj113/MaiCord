#include "maicord/core/win32_process.hpp"
#include "maicord/platform/file_identity.hpp"

#include "maicord/core/win32_kernel.hpp"
namespace maicord {
bool file_snapshot_read(HANDLE file, FileSnapshot* output, u32* native_error) noexcept {
    if (native_error) *native_error = 0;
    if (!output || !file || file == INVALID_HANDLE_VALUE) {
        if (native_error) *native_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file) != FILE_TYPE_DISK) {
        if (native_error) *native_error = ERROR_FILE_INVALID;
        return false;
    }
    if (!GetFileInformationByHandle(file, &info)) {
        if (native_error) *native_error = thread_last_error();
        return false;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (native_error) *native_error = ERROR_DIRECTORY;
        return false;
    }
    *output = {{(static_cast<u64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (static_cast<u64>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
        info.dwVolumeSerialNumber, 0}, (static_cast<u64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow};
    return true;
}
bool file_identity_equal(const FileIdentity& a, const FileIdentity& b) noexcept {
    return a.index == b.index && a.volume == b.volume && a.last_write == b.last_write;
}
}
