#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
u32 checks=0;bool print_ok=true;
void print(const char* text) noexcept {u32 n=0;while(text[n])++n;DWORD written=0;print_ok=print_ok&&WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,nullptr)&&written==n;}
void number(u32 v) noexcept {char digits[12];u32 n=0;do{digits[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n){char text[]{digits[--n],0};print(text);}}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
bool run() noexcept {
    Win32SystemInfo system{};GetSystemInfo(system.native());const u32 page=system.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*3,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Release {u8* p;~Release() noexcept {VirtualFree(p,0,MEM_RELEASE);}} release{memory};DWORD old=0;
    CHECK(VirtualProtect(memory,page,PAGE_NOACCESS,&old)&&VirtualProtect(memory+page*2,page,PAGE_NOACCESS,&old));
    const u32 widths[]{2,3,4,8};
    for(u32 width:widths)for(u32 location=0;location<18;++location){
        u8* input=location==0?memory+page:location==1?memory+page*2-width:memory+page+64+location-2;
        // All bit positions expose signedness, endian and truncation mistakes;
        // the two inaccessible borders detect reads wider than the API width.
        for(u32 bit=0;bit<width*8;++bit){CHECK(VirtualProtect(memory+page,page,PAGE_READWRITE,&old));
            for(u32 i=0;i<width;++i)input[i]=0;input[bit/8]=static_cast<u8>(1u<<(bit%8));
            CHECK(VirtualProtect(memory+page,page,PAGE_READONLY,&old));
            const u64 little=1ull<<bit,big=1ull<<((width-1-bit/8)*8+bit%8);
            if(width==2){CHECK(byte_load_le16(input)==little);CHECK(byte_load_be16(input)==big);}
            else if(width==3)CHECK(byte_load_le24(input)==little);
            else if(width==4){CHECK(byte_load_le32(input)==little);CHECK(byte_load_be32(input)==big);}
            else {CHECK(byte_load_le64(input)==little);CHECK(byte_load_be64(input)==big);}
            if(width!=3){
                CHECK(VirtualProtect(memory+page,page,PAGE_READWRITE,&old));
                if(input>memory+page)input[-1]=0xa5;
                if(input+width<memory+page*2)input[width]=0x5a;
                for(u32 i=0;i<width;++i)input[i]=0xcc;
                if(width==2)byte_store_le16(input,static_cast<u16>(little));
                else if(width==4)byte_store_le32(input,static_cast<u32>(little));
                else byte_store_le64(input,little);
                for(u32 i=0;i<width;++i)CHECK(input[i]==static_cast<u8>(little>>(i*8)));
                if(width==4){
                    byte_store_be32(input,static_cast<u32>(little));
                    for(u32 i=0;i<width;++i)CHECK(input[i]==static_cast<u8>(little>>((width-1-i)*8)));
                }
                CHECK(input==memory+page||input[-1]==0xa5);
                CHECK(input+width==memory+page*2||input[width]==0x5a);
            }
        }
    }
    // Multibit fixed vectors prevent an implementation accidentally combining
    // fields with an operation that happens to work for single-bit inputs.
    CHECK(VirtualProtect(memory+page,page,PAGE_READWRITE,&old));u8* p=memory+page*2-8;
    const u8 bytes[]{0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01};for(u32 i=0;i<8;++i)p[i]=bytes[i];
    CHECK(VirtualProtect(memory+page,page,PAGE_READONLY,&old));
    CHECK(byte_load_le16(p)==0xcdef&&byte_load_be16(p)==0xefcd&&byte_load_le24(p)==0xabcdef);
    CHECK(byte_load_le32(p)==0x89abcdefu&&byte_load_be32(p)==0xefcdab89u&&byte_load_le64(p)==0x0123456789abcdefull&&byte_load_be64(p)==0xefcdab8967452301ull);
    return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_byte_order_entry() noexcept {using namespace maicord;const bool ok=run();print("checks=");number(checks);print(ok?" PASS exact-width byte reads and writes\n":" FAIL exact-width byte reads and writes\n");ExitProcess(ok&&print_ok?0:1);}
