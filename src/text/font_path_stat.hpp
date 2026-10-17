#pragma once
#include "maicord/text/font_catalog.hpp"

#include "maicord/text/base.hpp"
namespace maicord::font_path_stat {
struct Stat {
    i64 file_id,creation,read,write,change,allocation,bytes;
    u32 attributes,reparse,links,device_type,device_characteristics,reserved;
    u64 volume;Win32FileId128 identity;
};
struct Io {void* status;usize bytes;};
static_assert(sizeof(Stat)==104&&sizeof(Io)==16);
using HandleQuery=LONG (WINAPI*)(HANDLE,Io*,void*,u32,u32);
// Optional one-query metadata read from an already read-locked handle. False
// clears the output and the caller falls back to the public query path.
// No pathname lookup, handle ownership or cache.
bool read_handle(HandleQuery,HANDLE,NativeFontFileStamp*) noexcept;
bool read_handle(HANDLE,NativeFontFileStamp*) noexcept;
}
