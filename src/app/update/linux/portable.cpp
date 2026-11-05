#include "maicord/app/update/portable.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/text.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord {
namespace {

constexpr u32 kPathCapacity = 4096;

bool marker_first_line(const wchar_t* marker) noexcept {
    const FileHandle file = file_open(marker, {FileAccess::read, FileCreate::existing, kFileShareRead, kFileNoFollow});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    FileStat stat{};
    char text[64];
    u32 read = 0;
    const bool ok = file_stat(file, &stat) && stat.kind == FileKind::regular && file_read(file, text, sizeof(text), &read);
    file_close(file);
    if (!ok)
        return false;
    // The first line is exactly the marker's line: the file may go on with fields, after a line break.
    const u32 expected = sizeof(kUpdatePortableMarkerLine) - 1;
    if (read < expected)
        return false;
    for (u32 i = 0; i < expected; ++i)
        if (text[i] != kUpdatePortableMarkerLine[i])
            return false;
    return read == expected || text[expected] == '\n' || (text[expected] == '\r' && read > expected + 1 &&
                                                         text[expected + 1] == '\n');
}

} // namespace

u32 update_portable_marker_path(ConstSlice<wchar_t> executable, wchar_t* output, u32 capacity) noexcept {
    if (!executable.data || !output)
        return 0;
    u32 directory = executable.count;
    while (directory && executable[directory - 1] != L'/')
        --directory;
    // "/MaiCord" has the directory "/"; a name without any slash has none.
    if (directory == 0)
        return 0;
    const u32 name = sizeof(kUpdatePortableMarkerName) - 1;
    const u32 count = directory + name;
    if (count >= capacity)
        return 0;
    for (u32 i = 0; i < directory; ++i)
        output[i] = executable[i];
    for (u32 i = 0; i < name; ++i)
        output[directory + i] = static_cast<wchar_t>(kUpdatePortableMarkerName[i]);
    output[count] = 0;
    return count;
}

// Whether this process may create files in the directory of `file` (a path of `count` characters): the update is staged
// beside the file it replaces and swapped in by rename.
bool directory_writable(const wchar_t* file, u32 count) noexcept {
    u32 directory = count;
    while (directory && file[directory - 1] != L'/')
        --directory;
    if (directory > 1)
        --directory;
    char path[kPathCapacity * 3];
    const u32 bytes = directory ? utf16_to_utf8(file, directory, path, sizeof(path) - 1, true) : 0;
    if (!bytes)
        return false;
    path[bytes] = 0;
    return !syscall_failed(sys_access(path, kAccessWrite));
}

bool update_portable_install(ConstSlice<wchar_t> executable) noexcept {
    wchar_t marker[kPathCapacity];
    const u32 count = update_portable_marker_path(executable, marker, kPathCapacity);
    return count && marker_first_line(marker) && directory_writable(marker, count);
}

bool update_appimage_install(ConstSlice<wchar_t> image) noexcept {
    FileStat stat{};
    return image.count && file_path_stat(image.data, &stat) && stat.kind == FileKind::regular &&
           directory_writable(image.data, image.count);
}

void update_portable_gate(UpdateServiceConfig* config, bool published, ConstSlice<wchar_t> executable,
                          ConstSlice<wchar_t> image) noexcept {
    const bool appimage = image.count != 0;
    if (config->configured && (!published || !(appimage ? update_appimage_install(image) : update_portable_install(executable))))
        config->configured = false;
    g_update_flavor = appimage ? UpdateFlavor::appimage : UpdateFlavor::tarball;
}

} // namespace maicord
