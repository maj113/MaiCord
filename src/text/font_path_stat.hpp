#pragma once
#include "maicord/text/font_catalog.hpp"

#include "maicord/core/win32_types.hpp"
namespace maicord::font_path_stat {
struct Stat {
    i64 file_id,creation,read,write,change,allocation,bytes;
    u32 attributes,reparse,links,device_type,device_characteristics,reserved;
    u64 volume;Win32FileId128 identity;
};
struct Name {u16 bytes,maximum;const wchar_t* text;};
struct Attributes {u32 bytes;HANDLE root;Name* name;u32 flags;void* security;void* quality;};
struct Io {void* status;usize bytes;};
static_assert(sizeof(Stat)==104&&sizeof(Name)==16&&sizeof(Attributes)==48&&sizeof(Io)==16);
using HandleQuery=LONG (WINAPI*)(HANDLE,Io*,void*,u32,u32);
// Optional one-query metadata projection from the already read-locked handle.
// False clears output and requires the caller's existing public-query path.
// No pathname lookup, handle ownership, cache or minimum-OS change.
bool read_handle(HandleQuery,HANDLE,NativeFontFileStamp*) noexcept;
bool read_handle(HANDLE,NativeFontFileStamp*) noexcept;

// Only ordinary short ASCII drive paths bypass Win32 normalization. Everything
// else keeps the public path, including DOS devices and noncanonical segments.
bool prepare(ConstSlice<wchar_t>,Slice<wchar_t>,Name*) noexcept;
// Read-locked synchronous random-access open for a validated terminated path.
// Reuse otherwise idle caller scratch for the guarded native-name conversion.
// Other paths, short/aliased scratch and native failures use CreateFileW.
// Optional proof is true only when this very handle was opened with
// OBJ_DONT_REPARSE. All normalizing/error fallbacks leave it false.
HANDLE open_read(ConstSlice<wchar_t> terminated_path,Slice<wchar_t> scratch,bool* no_reparse=nullptr) noexcept;
}
