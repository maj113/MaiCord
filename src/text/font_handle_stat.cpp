#include "font_path_stat.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

// This ntdll entry point exists throughout the supported Windows baseline.
// The newer information class remains optional and is checked at each call.
extern "C" __declspec(dllimport) LONG WINAPI NtQueryInformationFile(
    HANDLE,maicord::font_path_stat::Io*,void*,maicord::u32,maicord::u32);
namespace maicord::font_path_stat {
bool read_handle(HandleQuery query,HANDLE file,NativeFontFileStamp* output) noexcept {
    if(output)reset_value(output);if(!output||!query||!file||file==INVALID_HANDLE_VALUE)return false;
    Io io{};Stat stat{};
    if(query(file,&io,&stat,sizeof(stat),77)!=0||io.bytes!=sizeof(stat)||stat.device_type!=7||stat.bytes<0||
       (stat.attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
    *output={{stat.volume,stat.identity},static_cast<u64>(stat.bytes),{}, {}};
    copy_bytes(&output->write,&stat.write,8);copy_bytes(&output->change,&stat.change,8);return true;
}
bool read_handle(HANDLE file,NativeFontFileStamp* output) noexcept {
    return read_handle(NtQueryInformationFile,file,output);
}
}
