#include "maicord/text/font_catalog.hpp"
#include <Windows.h>
#include <stddef.h>
namespace maicord {
static_assert(sizeof(Win32FileTime)==sizeof(FILETIME)&&alignof(Win32FileTime)==alignof(FILETIME)&&
              offsetof(Win32FileTime,dwHighDateTime)==offsetof(FILETIME,dwHighDateTime));
static_assert(sizeof(Win32FileId128)==sizeof(FILE_ID_128)&&sizeof(Win32FileIdInfo)==sizeof(FILE_ID_INFO)&&
              alignof(Win32FileIdInfo)==alignof(FILE_ID_INFO)&&offsetof(Win32FileIdInfo,FileId)==offsetof(FILE_ID_INFO,FileId));
namespace {
template<class T> bool valid(Slice<T> s,u32 used) noexcept {return used<=s.count&&(!s.count||s.data);}
bool valid(const NativeFontCatalog& c) noexcept {return valid(c.files,c.count.files)&&valid(c.faces,c.count.faces)&&valid(c.candidates,c.count.candidates)&&valid(c.settings,c.count.settings)&&valid(c.strings,c.count.strings);}
NativeFontResult result(NativeFontError e,SfntStatus s=SfntStatus::ok,u32 native=0) noexcept {return {e,s,native};}
bool room(u32 used,u32 capacity,u32 amount) noexcept {return used<=capacity&&amount<=capacity-used;}
ConstSlice<u16> string(const NativeFontCatalog& c,u32 offset,u32 units) noexcept {return units&&room(offset,c.count.strings,units)?ConstSlice<u16>{c.strings.data+offset,units}:ConstSlice<u16>{};}
}
ConstSlice<u16> native_font_catalog_path(const NativeFontCatalog& c,u32 file) noexcept {if(!valid(c)||file>=c.count.files)return {};const auto& f=c.files[file];return string(c,f.path_offset,f.path_units);}
ConstSlice<u16> native_font_catalog_name(const NativeFontCatalog& c,u32 face) noexcept {if(!valid(c)||face>=c.count.faces)return {};const auto& f=c.faces[face];return string(c,f.name_offset,f.name_units);}
ConstSlice<FontAxisSetting> native_font_catalog_settings(const NativeFontCatalog& c,u32 candidate) noexcept {
    if(!valid(c)||candidate>=c.count.candidates)return {};const auto& v=c.candidates[candidate];if(!v.setting_count||!room(v.first_setting,c.count.settings,v.setting_count))return {};
    return {c.settings.data+v.first_setting,v.setting_count};
}
NativeFontResult native_font_catalog_entry(const NativeFontCatalog& c,u32 candidate,NativeFontCatalogEntry* output) noexcept {
    if(output)*output={};if(!output||!valid(c)||candidate>=c.count.candidates)return result(NativeFontError::invalid_input);
    const u32 face_index=c.candidates[candidate].face;if(face_index>=c.count.faces)return result(NativeFontError::invalid_input);
    const auto& face=c.faces[face_index];if(face.file>=c.count.files)return result(NativeFontError::invalid_input);const auto& file=c.files[face.file];
    const auto path=native_font_catalog_path(c,face.file);if(!path.count||!room(file.path_offset,c.count.strings,path.count+1)||c.strings[file.path_offset+path.count])return result(NativeFontError::invalid_input);
    const auto& value=c.candidates[candidate];if(!room(value.first_setting,c.count.settings,value.setting_count))return result(NativeFontError::invalid_input);
    const auto settings=native_font_catalog_settings(c,candidate);*output={path,settings,&file,face.index};return result(NativeFontError::none);
}
NativeFontResult native_font_catalog_validate(const NativeFontCatalogEntry& entry,const NativeFontStore& store,NativeFontHandle handle) noexcept {
    if(!entry.file)return result(NativeFontError::invalid_input);const auto* selected=native_font_get(store,handle);if(!selected)return result(NativeFontError::invalid_input);
    const auto& file=*entry.file;const auto& opened=store.files[store.faces[handle.slot-1].file_slot];FILE_BASIC_INFO basic{};
    if(selected->geometry.face.face_index!=entry.face_index)return result(NativeFontError::io,SfntStatus::ok,ERROR_RETRY);
    if(!GetFileInformationByHandleEx(opened.file,FileBasicInfo,&basic,sizeof(basic)))return result(NativeFontError::io,SfntStatus::ok,GetLastError());
    if(!bytes_equal(&file.identity,&opened.identity,sizeof(opened.identity))||file.bytes!=opened.bytes.count||(!bytes_equal(&basic.LastWriteTime,&file.last_write,8)||!bytes_equal(&basic.ChangeTime,&file.change_time,8)))return result(NativeFontError::io,SfntStatus::ok,ERROR_RETRY);
    return result(NativeFontError::none);
}
}
