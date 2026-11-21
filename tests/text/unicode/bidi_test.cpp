#include "maicord/text/unicode/bidi.hpp"
#include "maicord/text/unicode/detail/bidi_internal.hpp"
#include "../../support/text_seams.hpp"
#include "../../support/bidi_conformance.hpp"
#include "maicord/text/unicode/bidi_ltr.hpp"
#include "maicord/core/byte_order.hpp"
#include "../../support/text_test_support.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {
using B=UnicodeBidiClass;
constexpr u32 capacity=512;
UnicodeBidiCell cells[capacity];u32 scratch[capacity],visual[capacity];u8 levels[capacity];
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
    auto* memory=static_cast<u8*>(pages_allocate(count*20u));if(!memory)return false;
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
    pages_release(memory);return ok;
}
#include "bidi_ltr_elision.inc"
int run() noexcept {
    if(!ltr_elision_cases()){print(utf8_literal("FAIL LTR elision\n"));return 8;}
    if(!unit_cases()){print(utf8_literal("FAIL bidi unit tests\n"));return 1;}
    if(!candidate_cases()){print(utf8_literal("FAIL tailored bracket cases\n"));return 6;}
    if(!tailored_cases()){print(utf8_literal("FAIL tailored weak cases\n"));return 7;}
    if(!long_cases()){print(utf8_literal("FAIL bidi long cases\n"));return 2;}
    if(!random_cases()){print(utf8_literal("FAIL bidi invariants\n"));return 5;}
    print(utf8_literal("PASS bidi tests\n"));return 0;
}
}
}
MAICORD_TEST_ENTRY(maicord_bidi_tests_entry) {::maicord::test_process_exit(static_cast<UINT>(maicord::run()));}
