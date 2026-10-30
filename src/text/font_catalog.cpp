#include "maicord/text/font_catalog.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include <stddef.h>
#include "maicord/core/win32_file_info.hpp"
namespace maicord {
static_assert(sizeof(Win32FileTime)==sizeof(FILETIME)&&alignof(Win32FileTime)==alignof(FILETIME)&&
              offsetof(Win32FileTime,dwHighDateTime)==offsetof(FILETIME,dwHighDateTime));
static_assert(sizeof(Win32FileId128)==sizeof(FILE_ID_128)&&sizeof(Win32FileIdInfo)==sizeof(FILE_ID_INFO)&&
              alignof(Win32FileIdInfo)==alignof(FILE_ID_INFO)&&offsetof(Win32FileIdInfo,FileId)==offsetof(FILE_ID_INFO,FileId));
NativeFontResult native_font_catalog_validate(const NativeFontCatalog& c,const NativeFontStore& store,NativeFontHandle handle) noexcept {
    const auto* selected=native_font_get(store,handle);if(!selected)return {NativeFontError::invalid_input,SfntStatus::ok,0};
    const auto& opened=store.files[store.faces[handle.slot-1].file_slot];FILE_BASIC_INFO basic{};
    if(selected->geometry.face.face_index!=c.face_index)return {NativeFontError::io,SfntStatus::ok,ERROR_RETRY};
    if(!GetFileInformationByHandleEx(opened.file,FileBasicInfo,&basic,sizeof(basic)))return {NativeFontError::io,SfntStatus::ok,GetLastError()};
    if(!bytes_equal(&c.stamp.identity,&opened.identity,sizeof(opened.identity))||c.stamp.bytes!=opened.bytes.count||
       !bytes_equal(&basic.LastWriteTime,&c.stamp.write,8)||!bytes_equal(&basic.ChangeTime,&c.stamp.change,8))return {NativeFontError::io,SfntStatus::ok,ERROR_RETRY};
    return {};
}
}
