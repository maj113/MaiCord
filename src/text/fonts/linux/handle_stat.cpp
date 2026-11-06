#include "../path_stat.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"

// src/text/fonts/path_stat.hpp on Linux: one statx of the open descriptor gives the identity (device and inode),
// size and write and change times, so read_handle is the whole read and read_fallback only names why it declined.
namespace maicord::font_path_stat {
bool read_handle(FileHandle file,NativeFontFileStamp* output) noexcept {
    FileStat stat{};
    if(!file_stat(file,&stat)||stat.kind!=FileKind::regular)return false;
    *output={stat.id,stat.bytes,{},{}};
    copy_bytes(&output->write,&stat.last_write,8);copy_bytes(&output->change,&stat.change,8);
    return true;
}
bool read_fallback(FileHandle file,NativeFontFileStamp*,u32* error) noexcept {
    // read_handle declines a handle that cannot be queried or is not a regular file; there is no second query.
    FileStat stat{};
    *error=file_stat(file,&stat)?ERROR_FILE_INVALID:thread_last_error();
    return false;
}
}
