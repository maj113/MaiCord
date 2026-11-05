#include "maicord/app/update/portable.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/os/file.hpp"

// The Linux self-update marker (include/maicord/app/update/portable.hpp): which installs may replace their own files.

namespace maicord {
namespace {

u32 length(const wchar_t* text) noexcept {
    u32 n = 0;
    while (text[n])
        ++n;
    return n;
}

void join(wchar_t* output, const wchar_t* parent, const wchar_t* name) noexcept {
    u32 n = length(parent);
    copy_bytes(output, parent, n * sizeof(wchar_t));
    output[n++] = L'/';
    u32 j = 0;
    do {
        output[n + j] = name[j];
    } while (name[j++]);
}

bool write(const wchar_t* path, const char* text) noexcept {
    const FileHandle file = file_open(path, {FileAccess::write, FileCreate::create_always, 0, 0});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    u32 count = 0;
    while (text[count])
        ++count;
    u32 written = 0;
    const bool ok = file_write(file, text, count, &written) && written == count;
    return file_close(file) && ok;
}

bool same(const wchar_t* a, const wchar_t* b) noexcept {
    u32 n = 0;
    while (a[n] && a[n] == b[n])
        ++n;
    return a[n] == b[n];
}

ConstSlice<wchar_t> view(const wchar_t* text) noexcept {
    return {text, length(text)};
}

} // namespace

void test_update_portable(void (*check)(bool, const char*) noexcept) noexcept {
    // The marker's path: beside the executable, whatever the executable is called.
    wchar_t path[256];
    check(update_portable_marker_path(view(L"/opt/MaiCord/MaiCord"), path, 256) == 30 &&
              same(path, L"/opt/MaiCord/.maicord-portable"),
          "portable: the marker is the file of that name in the executable's directory");
    check(update_portable_marker_path(view(L"/MaiCord"), path, 256) == 18 && same(path, L"/.maicord-portable"),
          "portable: an executable in the root has the root's marker");
    check(update_portable_marker_path(view(L"MaiCord"), path, 256) == 0 &&
              update_portable_marker_path({}, path, 256) == 0 &&
              update_portable_marker_path(view(L"/opt/MaiCord/MaiCord"), path, 20) == 0,
          "portable: no directory, nothing, or no room gives no path");

    wchar_t root[1024];
    const DataPathResult made = cache_path(root, 1024, L"portable-install", true);
    if (!made.ok()) {
        check(false, "portable: a scratch directory");
        return;
    }
    dir_create(root);
    wchar_t executable[1100], marker[1100];
    join(executable, root, L"MaiCord");
    join(marker, root, L".maicord-portable");
    file_remove(marker);
    check(write(executable, "image") && !update_portable_install(view(executable)),
          "portable: an install without the marker does not update itself");
    check(write(marker, "maicord-portable 1\n") && update_portable_install(view(executable)),
          "portable: the marker's line makes the tarball install updatable");
    check(write(marker, "maicord-portable 1") && update_portable_install(view(executable)),
          "portable: the marker may end without a line break");
    check(write(marker, "maicord-portable 1\r\nchannel=preview\nfuture=1\n") && update_portable_install(view(executable)),
          "portable: later lines (a channel, unknown fields) are ignored");
    check(write(marker, "maicord-portable 2\n") && !update_portable_install(view(executable)),
          "portable: another marker version is not this one");
    check(write(marker, "maicord-portable 12\n") && !update_portable_install(view(executable)) &&
              write(marker, "") && !update_portable_install(view(executable)) &&
              write(marker, "Maicord-portable 1\n") && !update_portable_install(view(executable)),
          "portable: a longer, empty or differently spelled first line is not the marker");
    file_remove(marker);
    dir_create(marker);
    check(!update_portable_install(view(executable)), "portable: a directory of that name is not a marker");
    dir_remove(marker);
    file_remove(executable);
    dir_remove(root);
}

} // namespace maicord
