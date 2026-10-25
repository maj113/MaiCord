#include "maicord/text/bidi.hpp"
#include "maicord/text/detail/bidi_internal.hpp"
#include "maicord/text/bidi_ltr.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using B=UnicodeBidiClass;
constexpr u32 capacity=512;
UnicodeBidiCell cells[capacity];u32 scratch[capacity],visual[capacity];u8 levels[capacity],expected_levels[capacity];
void print(Utf8View value) noexcept {DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),value.data,value.bytes,&written,nullptr);}
void number(u32 value) noexcept {char text[16];u32 n=0;do{text[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char c=text[i];text[i]=text[n-i-1];text[n-i-1]=c;}print({text,n});}
bool unit_cases() noexcept {
    const u32 text[]{0x202b,0x5d0,0x20,0x5d1,0x202c};UnicodeBidiParagraph paragraph{};
    if(unicode_bidi_classify({text,5},{cells,5})!=UnicodeBidiStatus::ok ||
       unicode_bidi_resolve({cells,5},UnicodeParagraphDirection::ltr,{scratch,5},&paragraph)!=UnicodeBidiStatus::ok ||
       paragraph.level || cells[2].level!=1 || cells[0].level!=255 || cells[4].level!=255)return false;
    u32 count=0;
    if(unicode_bidi_line({cells,5},0,0,3,{levels,3},{visual,3},&count)!=UnicodeBidiStatus::ok || count!=2 || levels[2]!=0 || visual[0]!=1 || visual[1]!=2 || cells[2].level!=1)return false;
    if(unicode_bidi_line({cells,5},0,0,5,{levels,5},{visual,5},&count)!=UnicodeBidiStatus::ok || count!=3 || visual[0]!=3 || visual[1]!=2 || visual[2]!=1)return false;
    u8 line_levels[5]{};
    if(unicode_bidi_line({cells,5},0,0,5,{line_levels,5},{},&count)!=UnicodeBidiStatus::ok || count!=3 || !bytes_equal(line_levels,levels,5) ||
       unicode_bidi_line({cells,5},0,0,5,{line_levels,5},{visual,4},&count)!=UnicodeBidiStatus::output_too_small)return false;
    if(unicode_bidi_resolve({cells,5},UnicodeParagraphDirection::ltr,{scratch,4},&paragraph)!=UnicodeBidiStatus::output_too_small || paragraph.count)return false;
    cells[0].original=B::b;
    if(unicode_bidi_resolve({cells,5},UnicodeParagraphDirection::ltr,{scratch,5},&paragraph)!=UnicodeBidiStatus::malformed_paragraph)return false;
    cells[0].original=static_cast<B>(255);
    if(unicode_bidi_resolve({cells,5},UnicodeParagraphDirection::ltr,{scratch,5},&paragraph)!=UnicodeBidiStatus::invalid_input)return false;
    const u32 invalid[]{0xd800};
    if(unicode_bidi_classify({invalid,1},{cells,1})!=UnicodeBidiStatus::invalid_input)return false;
    if(unicode_bidi_resolve({},UnicodeParagraphDirection::auto_rtl,{},&paragraph)!=UnicodeBidiStatus::ok || paragraph.level!=1)return false;
    if(unicode_bidi_line({},paragraph.level,0,0,{},{},&count)!=UnicodeBidiStatus::ok || count)return false;
    const u32 paragraphs[]{0x5d0,0xa,0x41,0x2029,0x42};u32 end=0;
    if(unicode_bidi_classify({paragraphs,5},{cells,5})!=UnicodeBidiStatus::ok)return false;
    if(unicode_bidi_paragraph_end({cells,5},0,&end)!=UnicodeBidiStatus::ok || end!=2 ||
       unicode_bidi_resolve({cells,2},UnicodeParagraphDirection::auto_ltr,{scratch,2},&paragraph)!=UnicodeBidiStatus::ok || paragraph.level!=1)return false;
    if(unicode_bidi_paragraph_end({cells,5},end,&end)!=UnicodeBidiStatus::ok || end!=4 ||
       unicode_bidi_resolve({cells+2,2},UnicodeParagraphDirection::auto_ltr,{scratch,2},&paragraph)!=UnicodeBidiStatus::ok || paragraph.level!=0)return false;
    return unicode_bidi_paragraph_end({cells,5},end,&end)==UnicodeBidiStatus::ok && end==5 &&
           unicode_bidi_paragraph_end({cells,5},end,&end)==UnicodeBidiStatus::ok && end==5 &&
           unicode_bidi_paragraph_end({cells,5},6,&end)==UnicodeBidiStatus::invalid_input;
}
bool candidate_cases() noexcept {
    // Captured Windows levels, explicitly separate from Unicode conformance.
    struct Case {u32 count;u32 text[8];u8 expected[8];u8 base=0;};
    static const Case cases[]{
        {4,{0x5d0,'(',0x31,')'},{1,0,2,0}},
        {4,{'(',0x5d0,')',0x5d1},{1,1,1,1}},
        {4,{0x5d0,'(',')',0x31},{1,0,0,2}},
        {5,{0x5d0,'(',0x31,')',0x5d0},{1,1,2,1,1}},
        {5,{0x5d0,0x31,'(',0x5d0,')'},{1,2,1,1,1}},
        {7,{0x3008,'1','[',']','a',0x3009,'1'},{2,2,2,2,2,2,2},1}
    };
    UnicodeBidiParagraph paragraph{};u32 count=0;
    for(const auto& test:cases) {
        if(unicode_bidi_classify({test.text,test.count},{cells,test.count})!=UnicodeBidiStatus::ok ||
           bidi_resolve_with_brackets({cells,test.count},static_cast<UnicodeParagraphDirection>(test.base),{scratch,test.count},&paragraph,bidi_tailored_brackets)!=UnicodeBidiStatus::ok ||
           unicode_bidi_line({cells,test.count},test.base,0,test.count,{levels,test.count},{visual,test.count},&count)!=UnicodeBidiStatus::ok || count!=test.count)return false;
        for(u32 i=0;i<count;++i)if(levels[i]!=test.expected[i])return false;
    }
    return bidi_resolve_with_brackets({},UnicodeParagraphDirection::ltr,{},&paragraph,nullptr)==UnicodeBidiStatus::invalid_input && !paragraph.count;
}
bool tailored_cases() noexcept {
    struct Case {u32 count;u32 text[32];u8 windows[32],standard[32];u8 base;};
    static const Case cases[]{
#include "bidi_tailored_weak_cases.inc"
#include "bidi_tailored_bracket_cases.inc"
#include "bidi_tailored_control_cases.inc"
    };
    for(const auto& test:cases)for(u32 profile=0;profile<2;++profile) {
        UnicodeBidiParagraph paragraph{};u32 count=0;
        if(unicode_bidi_classify({test.text,test.count},{cells,test.count})!=UnicodeBidiStatus::ok)return false;
        const auto policy=static_cast<UnicodeParagraphDirection>(test.base);
        const auto status=profile?bidi_resolve_tailored({cells,test.count},policy,{scratch,test.count},&paragraph):
            unicode_bidi_resolve({cells,test.count},policy,{scratch,test.count},&paragraph);
        if(status!=UnicodeBidiStatus::ok || paragraph.level!=test.base ||
           unicode_bidi_line({cells,test.count},paragraph.level,0,test.count,{levels,test.count},{visual,test.count},&count)!=UnicodeBidiStatus::ok)return false;
        u32 visible=0;
        for(u32 i=0;i<test.count;++i) {
            const u8 expected=profile?test.windows[i]:test.standard[i];if(expected!=255)++visible;
            if(levels[i]!=expected || cells[i].scalar!=test.text[i] || cells[i].original!=unicode_properties(test.text[i])->bidi_class())return false;
        }
        if(count!=visible)return false;
        if(profile) {
            // A subsequent standards resolution uses the same untouched inputs.
            if(unicode_bidi_resolve({cells,test.count},policy,{scratch,test.count},&paragraph)!=UnicodeBidiStatus::ok ||
               unicode_bidi_line({cells,test.count},paragraph.level,0,test.count,{levels,test.count},{visual,test.count},&count)!=UnicodeBidiStatus::ok)return false;
            for(u32 i=0;i<test.count;++i)if(levels[i]!=test.standard[i])return false;
        }
    }
    UnicodeBidiParagraph paragraph{7,1};
    if(bidi_resolve_tailored({cells,2},UnicodeParagraphDirection::ltr,{scratch,1},&paragraph)!=UnicodeBidiStatus::output_too_small || paragraph.count)return false;
    return bidi_resolve_tailored({},UnicodeParagraphDirection::auto_rtl,{},&paragraph)==UnicodeBidiStatus::ok && !paragraph.count && paragraph.level==1;
}
bool random_cases() noexcept {
    u32 random=0xb1d11701;
    for(u32 test=0;test<20000;++test) {
        random^=random<<13;random^=random>>17;random^=random<<5;const u32 count=1+random%128;
        for(u32 i=0;i<count;++i) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            B type=static_cast<B>(random%23);if(type==B::b && i+1<count)type=B::on;
            const u32 brackets[]{'(',')','[',']'};cells[i]={type==B::on?brackets[(random>>8)&3]:0,0,0,type,type,0,0};
        }
        UnicodeBidiParagraph paragraph{};u32 visible=0;bool seen[128]{};u8 saved[128];
        if(unicode_bidi_resolve({cells,count},static_cast<UnicodeParagraphDirection>(random%4),{scratch,count},&paragraph)!=UnicodeBidiStatus::ok)return false;
        for(u32 i=0;i<count;++i) {
            const auto& cell=cells[i];saved[i]=cell.level;
            if(cell.level==255)continue;
            if(cell.level>126 || cell.explicit_level>125 || (cell.resolved!=B::l && cell.resolved!=B::r && cell.resolved!=B::en && cell.resolved!=B::an))return false;
            if((cell.resolved==B::r)!=(bool)(cell.level&1))return false;
        }
        if(unicode_bidi_line({cells,count},paragraph.level,0,count,{levels,count},{visual,count},&visible)!=UnicodeBidiStatus::ok)return false;
        for(u32 i=0;i<visible;++i){if(visual[i]>=count || seen[visual[i]] || levels[visual[i]]==255)return false;seen[visual[i]]=true;}
        for(u32 i=0;i<count;++i)if(seen[i]!=(levels[i]!=255) || cells[i].level!=saved[i])return false;
    }
    return true;
}
bool long_cases() noexcept {
    constexpr u32 depth=10000,count=depth*2+1;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,count*20u,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;
    auto* items=reinterpret_cast<UnicodeBidiCell*>(memory);auto* order=reinterpret_cast<u32*>(memory+count*16u);
    bool ok=true;UnicodeBidiParagraph paragraph{};
    for(u32 mode=0;mode<2 && ok;++mode) {
        for(u32 i=0;i<depth;++i){items[i].scalar=mode?0x2068:'(';items[i].original=mode?B::fsi:B::on;}
        items[depth].scalar=0x5d0;items[depth].original=B::r;
        for(u32 i=depth+1;i<count;++i){items[i].scalar=mode?0x2069:')';items[i].original=mode?B::pdi:B::on;}
        ok=unicode_bidi_resolve({items,count},UnicodeParagraphDirection::ltr,{order,count},&paragraph)==UnicodeBidiStatus::ok && paragraph.level==0 && items[count-1].level==0;
        if(ok && !mode)for(u32 i=0;i<count;++i)if(items[i].level!=(i==depth?1:0)){ok=false;break;}
        if(ok && mode)ok=items[depth].level==125;
    }
    if(ok) {
        // Long disjoint empty pairs exercise the candidate's successor map.
        // The final L supplies following context to every pair in one run.
        for(u32 i=0;i<count;++i){items[i].scalar=i+1==count?'a':i&1?')':'(';items[i].original=i+1==count?B::l:B::on;}
        ok=bidi_resolve_with_brackets({items,count},UnicodeParagraphDirection::ltr,{order,count},&paragraph,bidi_tailored_brackets)==UnicodeBidiStatus::ok;
        if(ok)for(u32 i=0;i<count;++i)if(items[i].level!=0){ok=false;break;}
    }
    if(ok) {
        // One long ET run preserves only its first element as neutral in the
        // observed AN/CS context. This also exercises constant scratch usage.
        for(u32 i=0;i<count;++i) {
            const u32 scalar=i==0?0x661:i==1?',':i+1==count?'1':'$';
            items[i].scalar=scalar;items[i].original=unicode_properties(scalar)->bidi_class();
        }
        ok=bidi_resolve_tailored({items,count},UnicodeParagraphDirection::rtl,{order,count},&paragraph)==UnicodeBidiStatus::ok;
        if(ok)for(u32 i=0;i<count;++i)if(items[i].level!=(i==1 || i==2?1:2)){ok=false;break;}
    }
    if(ok) {
        // Bracket context crosses a long separator span without suffix scans.
        for(u32 i=0;i<count;++i) {
            const u32 scalar=i==0?'a':i==1?'(':i==count-5 || i==count-4?'$':i==count-3?'1':i==count-2?0x5d0:i==count-1?')':' ';
            items[i].scalar=scalar;items[i].original=unicode_properties(scalar)->bidi_class();
        }
        ok=bidi_resolve_tailored({items,count},UnicodeParagraphDirection::rtl,{order,count},&paragraph)==UnicodeBidiStatus::ok;
        if(ok)for(u32 i=0;i<count;++i)if(items[i].level!=(i==0 || i==count-4 || i==count-3?2:1)){ok=false;break;}
    }
    if(ok) {
        // Isolate controls retain source identity but do not create levels or
        // isolate stacks in the tailored policy.
        for(u32 i=0;i<count;++i) {
            const u32 scalar=i<depth?0x2068:i==depth?0x5d0:0x2069;
            items[i].scalar=scalar;items[i].original=unicode_properties(scalar)->bidi_class();
        }
        ok=bidi_resolve_tailored({items,count},UnicodeParagraphDirection::ltr,{order,count},&paragraph)==UnicodeBidiStatus::ok && paragraph.count==count;
        if(ok)for(u32 i=0;i<count;++i)if(items[i].level!=(i==depth?1:255) || items[i].scalar!=(i<depth?0x2068u:i==depth?0x5d0u:0x2069u) ||
            items[i].original!=(i<depth?B::fsi:i==depth?B::r:B::pdi)){ok=false;break;}
        if(ok)ok=unicode_bidi_resolve({items,count},UnicodeParagraphDirection::ltr,{order,count},&paragraph)==UnicodeBidiStatus::ok && items[depth].level==125;
    }
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
struct Reader {
    const u8* bytes;u32 size,at;
    bool word(u32* value) noexcept {if(size-at<4)return false;*value=byte_load_le32(bytes+at);at+=4;return true;}
    bool take(u32 count,const u8** value) noexcept {if(count>size-at)return false;*value=bytes+at;at+=count;return true;}
};
bool corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    Win32LargeInteger size{};if(!GetFileSizeEx(file,size.native()) || size.QuadPart<12 || size.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping){CloseHandle(file);return false;}
    const auto* bytes=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));
    bool ok=bytes && bytes_equal(bytes,"MCBIDI1",8);u32 total=0,failures=0,classes=0,characters=0,scalars=0,order_checks=0;
    Reader reader{bytes,static_cast<u32>(size.QuadPart),8};if(ok)ok=reader.word(&total) && total>0;
    for(u32 test=0;test<total && ok;++test) {
        u32 count=0,policy=0,base=0,expected_count=0,line=0,kind=0;
        ok=reader.word(&count) && reader.word(&policy) && reader.word(&base) && reader.word(&expected_count) && reader.word(&line) && reader.word(&kind) && count<=capacity && expected_count<=count && policy<=2 && (base<=1 || base==255) && kind<=1;
        if(!ok)break;
        for(u32 i=0;i<count && ok;++i) {
            u32 cp=0;const u8* values=nullptr;ok=reader.word(&cp) && reader.take(4,&values) && unicode_is_scalar(cp);
            if(ok){const auto type=values[0]==255?unicode_properties(cp)->bidi_class():static_cast<B>(values[0]);cells[i]={cp,0,0,type,type,0,0};expected_levels[i]=values[1];}
        }
        const u8* expected_visual=nullptr;if(ok)ok=reader.take(expected_count*4,&expected_visual);if(!ok)break;
        UnicodeBidiParagraph paragraph{};u32 actual_count=0,first_difference=~0u;
        const auto status=unicode_bidi_resolve({cells,count},static_cast<UnicodeParagraphDirection>(policy),{scratch,count},&paragraph);
        const auto line_status=status==UnicodeBidiStatus::ok?unicode_bidi_line({cells,count},paragraph.level,0,count,{levels,count},{visual,count},&actual_count):UnicodeBidiStatus::invalid_input;
        bool match=status==UnicodeBidiStatus::ok && line_status==UnicodeBidiStatus::ok && (base==255 || base==paragraph.level) && actual_count==expected_count;
        if(line_status==UnicodeBidiStatus::ok)for(u32 i=0;i<count;++i)if(levels[i]!=expected_levels[i]){if(first_difference==~0u)first_difference=i;match=false;}
        if(match)for(u32 i=0;i<expected_count;++i)if(visual[i]!=byte_load_le32(expected_visual+i*4)){match=false;break;}
        if(!match) {
            if(failures<12){print(utf8_literal("FAIL kind="));number(kind);print(utf8_literal(" line="));number(line);print(utf8_literal(" policy="));number(policy);print(utf8_literal(" status="));number(static_cast<u32>(status));
                print(utf8_literal(" first_level="));number(first_difference);
                if(first_difference<count){print(utf8_literal(" actual="));number(levels[first_difference]);print(utf8_literal(" expected="));number(expected_levels[first_difference]);}
                print(utf8_literal("\n"));}
            ++failures;
        }
        if(kind)++characters;else ++classes;scalars+=count;order_checks+=expected_count;
    }
    if(ok)ok=reader.at==reader.size;
    print(utf8_literal("class_cases="));number(classes);print(utf8_literal("\ncharacter_cases="));number(characters);print(utf8_literal("\nscalar_inputs="));number(scalars);
    print(utf8_literal("\norder_checks="));number(order_checks);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\ncomplete="));number(ok?1:0);print(utf8_literal("\n"));
    if(bytes)UnmapViewOfFile(bytes);CloseHandle(mapping);CloseHandle(file);return ok && !failures;
}
#include "bidi_ltr_elision.inc"
int run() noexcept {
    if(!ltr_elision_cases()){print(utf8_literal("FAIL LTR elision\n"));return 8;}
    if(!unit_cases()){print(utf8_literal("FAIL bidi unit tests\n"));return 1;}
    if(!candidate_cases()){print(utf8_literal("FAIL tailored bracket cases\n"));return 6;}
    if(!tailored_cases()){print(utf8_literal("FAIL tailored weak cases\n"));return 7;}
    if(!long_cases()){print(utf8_literal("FAIL bidi long cases\n"));return 2;}
    if(!random_cases()){print(utf8_literal("FAIL bidi invariants\n"));return 5;}
    const wchar_t* command=GetCommandLineW();bool quote=false;
    while(*command){if(*command=='"')quote=!quote;else if(!quote && (*command==' ' || *command=='\t'))break;++command;}
    while(*command==' ' || *command=='\t')++command;
    if(*command) {
        wchar_t path[32768];u32 count=0;quote=*command=='"';if(quote)++command;
        while(*command && !(quote?*command=='"':(*command==' ' || *command=='\t'))) {if(count+1>=32768)return 3;path[count++]=*command++;}
        if(quote){if(*command!='"')return 3;++command;}while(*command==' ' || *command=='\t')++command;
        if(*command || !count)return 3;path[count]=0;if(!corpus(path))return 4;
    }
    print(utf8_literal("PASS bidi tests\n"));return 0;
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_bidi_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
