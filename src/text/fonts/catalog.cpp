#include "maicord/text/fonts/catalog.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_process.hpp"
#include <stddef.h>
#include "maicord/os/file.hpp"
namespace maicord {
static_assert(sizeof(Win32FileTime)==sizeof(FILETIME)&&alignof(Win32FileTime)==alignof(FILETIME)&&
              offsetof(Win32FileTime,dwHighDateTime)==offsetof(FILETIME,dwHighDateTime));
static_assert(sizeof(Win32FileId128)==sizeof(FILE_ID_128)&&sizeof(Win32FileIdInfo)==sizeof(FILE_ID_INFO)&&
              alignof(Win32FileIdInfo)==alignof(FILE_ID_INFO)&&offsetof(Win32FileIdInfo,FileId)==offsetof(FILE_ID_INFO,FileId));
// native_font_catalog_validate compares both times in one call.
static_assert(offsetof(NativeFontFileStamp,change)==offsetof(NativeFontFileStamp,write)+8);
NativeFontResult native_font_catalog_validate(const NativeFontCatalog& c,const NativeFontStore& store,NativeFontHandle handle) noexcept {
    const auto* selected=native_font_get(store,handle);if(!selected)return {NativeFontError::invalid_input,SfntStatus::ok,0};
    const auto& opened=store.files[store.faces[handle.slot-1].file_slot];u64 times[2]{};
    if(selected->geometry.face.face_index!=c.face_index)return {NativeFontError::io,SfntStatus::ok,ERROR_RETRY};
    if(!file_times(opened.file,times,times+1))return {NativeFontError::io,SfntStatus::ok,thread_last_error()};
    if(!bytes_equal(&c.stamp.identity,&opened.identity,sizeof(opened.identity))||c.stamp.bytes!=opened.bytes.count||
       !bytes_equal(times,&c.stamp.write,16))return {NativeFontError::io,SfntStatus::ok,ERROR_RETRY};
    return {};
}
}
