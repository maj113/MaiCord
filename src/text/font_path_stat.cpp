#include "font_path_stat.hpp"

#include <Windows.h>

extern "C" __declspec(dllimport) LONG WINAPI NtCreateFile(HANDLE*,ULONG,
    maicord::font_path_stat::Attributes*,maicord::font_path_stat::Io*,LARGE_INTEGER*,ULONG,ULONG,ULONG,ULONG,void*,ULONG);
namespace maicord::font_path_stat {namespace {
wchar_t lower(wchar_t c) noexcept {return c>=L'A'&&c<=L'Z'?static_cast<wchar_t>(c+32):c;}
bool ordinary(ConstSlice<wchar_t> path) noexcept {
    if(!path.data||path.count<4||path.count>=260||lower(path[0])<L'a'||lower(path[0])>L'z'||path[1]!=L':'||path[2]!=L'\\')return false;
    u32 first=3,base_end=0;
    for(u32 i=3;i<=path.count;++i){const wchar_t c=i<path.count?path[i]:L'\\';
        if(c==L'\\'){
            if(i==first||path[i-1]==L'.'||path[i-1]==L' ')return false;
            u32 end=base_end?base_end:i;while(end>first&&path[end-1]==L' ')--end;
            const u32 n=end-first;
            if(n==3||n==4){const auto a=lower(path[first]),b=lower(path[first+1]),d=lower(path[first+2]);
                if(n==3&&((a==L'c'&&b==L'o'&&d==L'n')||(a==L'p'&&b==L'r'&&d==L'n')||
                    (a==L'a'&&b==L'u'&&d==L'x')||(a==L'n'&&b==L'u'&&d==L'l')))return false;
                if(n==4&&path[first+3]>=L'0'&&path[first+3]<=L'9'&&
                    ((a==L'c'&&b==L'o'&&d==L'm')||(a==L'l'&&b==L'p'&&d==L't')))return false;
            }
            first=i+1;base_end=0;continue;
        }
        if(c<32||c>126||c==L'/'||c==L':'||c==L'"'||c==L'<'||c==L'>'||c==L'|'||c==L'?'||c==L'*'||c==L'$')return false;
        if(c==L'.'&&!base_end)base_end=i;
    }return true;
}
}
bool prepare(ConstSlice<wchar_t> path,Slice<wchar_t> scratch,Name* output) noexcept {
    if(!output)return false;*output={};
    if(!scratch.data||path.count>scratch.count||scratch.count-path.count<4||!ordinary(path))return false;
    const usize a=reinterpret_cast<usize>(path.data),b=reinterpret_cast<usize>(scratch.data);
    // Read the terminator only through the unchanged fallback path. Include it
    // in the alias exclusion so preparing cannot corrupt a caller's input.
    if(a<=b?b-a<(static_cast<usize>(path.count)+1)*2:a-b<(static_cast<usize>(path.count)+4)*2)return false;
    copy_bytes(scratch.data,L"\\??\\",8);copy_bytes(scratch.data+4,path.data,path.count*2);
    const auto bytes=static_cast<u16>((path.count+4)*2);*output={bytes,bytes,scratch.data};return true;
}
HANDLE open_read(ConstSlice<wchar_t> path,Slice<wchar_t> scratch,bool* no_reparse) noexcept {
    if(no_reparse)*no_reparse=false;
    Name name{};
    if(prepare(path,scratch,&name)){
        SECURITY_QUALITY_OF_SERVICE quality{sizeof(quality),SecurityImpersonation,TRUE,TRUE};
        Attributes attributes{sizeof(Attributes),nullptr,&name,no_reparse?0x1040u:0x40u,nullptr,&quality};Io io{};HANDLE file=nullptr;
        // Same read attributes and synchronous random-access options as CreateFileW;
        // sharing still excludes writes/deletion.
        if(NtCreateFile(&file,GENERIC_READ|SYNCHRONIZE|FILE_READ_ATTRIBUTES,&attributes,&io,nullptr,
            FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,1,0x860,nullptr,0)>=0){if(no_reparse)*no_reparse=true;return file;}
    }
    return CreateFileW(path.data,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_RANDOM_ACCESS,nullptr);
}
}
