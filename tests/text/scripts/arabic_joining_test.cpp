#include "maicord/text/scripts/arabic_joining.hpp"
#include "maicord/text/opentype/glyph_buffer.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "../../support/test_output.hpp"

namespace maicord {namespace {
#include "../../generated/arabic_joining_fixture.inc"
using S=ArabicJoiningStatus;
bool vectors() noexcept {
    u32 scalars[128];u8 forms[128];SfntGsubGlyph glyphs[128];u32 at=0,cases=0,total=0;
    constexpr u32 size=sizeof(arabic_joining_vectors)/sizeof(u32);
    while(at<size) {
        const u32 count=arabic_joining_vectors[at++];
        if(!count || count>128 || count>size-at)return false;
        for(u32 i=0;i<128;++i){forms[i]=0xa5;glyphs[i]={static_cast<u16>(i+1),SfntGlyphClass::base,0xa5};}
        for(u32 i=0;i<count;++i)scalars[i]=arabic_joining_vectors[at+i]&0x1fffff;
        if(arabic_joining_forms({scalars,count},forms,1,count*2)!=S::ok ||
           arabic_joining_forms({scalars,count},&glyphs[0].reserved,sizeof(glyphs[0]),count*2)!=S::ok)return false;
        for(u32 i=0;i<128;++i) {
            const u8 expected=i<count?static_cast<u8>(arabic_joining_vectors[at+i]>>21):0xa5;
            if(forms[i]!=expected || glyphs[i].reserved!=expected || glyphs[i].glyph!=i+1 || glyphs[i].glyph_class!=SfntGlyphClass::base)return false;
        }
        at+=count;++cases;total+=count;
    }
    return cases==arabic_joining_case_count && total==arabic_joining_scalar_count;
}
bool contracts() noexcept {
    u8 output[16];for(auto& b:output)b=0xa5;
    const u32 valid[]{0x628,0x628};
    if(arabic_joining_forms({valid,2},output,1,3)!=S::work_limit || arabic_joining_forms({},output,1,0)!=S::ok)return false;
    for(const auto b:output)if(b!=0xa5)return false;
    // Input is read only; exact strided span and untouched prefix/suffix.
    if(arabic_joining_forms({valid,2},output+2,4,4)!=S::ok)return false;
    for(u32 i=0;i<16;++i)if(output[i]!=(i==2?8:i==6?2:0xa5))return false;
    return true;
}
bool long_marks() noexcept {
    constexpr u32 count=8194;u32* scalars=static_cast<u32*>(pages_allocate(count*sizeof(u32)));
    if(!scalars)return false;
    u8* forms=static_cast<u8*>(pages_allocate(count));
    if(!forms){pages_release(scalars);return false;}
    for(u32 i=0;i<count;++i)scalars[i]=(i==0 || i==count-1)?0x628:0x64e;
    bool ok=pages_protect(scalars, count*sizeof(u32), PageAccess::read)!=0;
    ok=ok && arabic_joining_forms({scalars,count},forms,1,count*2)==S::ok;
    for(u32 i=0;i<count && ok;++i)ok=forms[i]==(i==0?8:i==count-1?2:0);
    pages_release(forms);pages_release(scalars);return ok;
}
int run() noexcept {
    if(!vectors())return 1;
    if(!contracts())return 2;
    if(!long_marks())return 3;
    constexpr auto message=utf8_literal("PASS Arabic joining: Windows forms, strided metadata, failure atomicity, linear transparent marks\n");
    DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message.data,message.bytes,&done,nullptr);return 0;
}
}}
MAICORD_TEST_ENTRY(maicord_arabic_joining_tests_entry) {
    ::maicord::test_process_exit(static_cast<UINT>(maicord::run()));
}
