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
UnicodeBidiCell cells[capacity];u32 scratch[capacity];u8 levels[capacity];
bool unit_cases() noexcept {
    const u32 text[]{0x202b,0x5d0,0x20,0x5d1,0x202c};
    unicode_bidi_classify({text,5},{cells,5});
    if(unicode_bidi_resolve({cells,5},UnicodeParagraphDirection::ltr,scratch) || cells[2].level!=1 || cells[0].level!=255 || cells[4].level!=255)return false;
    // L1 on a line ending at the space resets it; the paragraph's levels stay.
    unicode_bidi_line_levels({cells,3},0,levels);
    if(levels[0]!=255 || levels[1]!=1 || levels[2]!=0 || cells[2].level!=1)return false;
    unicode_bidi_line_levels({cells,5},0,levels);
    if(levels[1]!=1 || levels[2]!=1 || levels[3]!=1 || levels[4]!=255)return false;
    if(unicode_bidi_resolve({},UnicodeParagraphDirection::auto_rtl,scratch)!=1)return false;
    const u32 paragraphs[]{0x5d0,0xa,0x41,0x2029,0x42};
    unicode_bidi_classify({paragraphs,5},{cells,5});
    return unicode_bidi_paragraph_end({cells,5},0)==2 && unicode_bidi_resolve({cells,2},UnicodeParagraphDirection::auto_ltr,scratch)==1 &&
           unicode_bidi_paragraph_end({cells,5},2)==4 && unicode_bidi_resolve({cells+2,2},UnicodeParagraphDirection::auto_ltr,scratch)==0 &&
           unicode_bidi_paragraph_end({cells,5},4)==5;
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
    for(const auto& test:cases) {
        unicode_bidi_classify({test.text,test.count},{cells,test.count});
        bidi_resolve_with_brackets({cells,test.count},static_cast<UnicodeParagraphDirection>(test.base),scratch,bidi_tailored_brackets);
        unicode_bidi_line_levels({cells,test.count},test.base,levels);
        for(u32 i=0;i<test.count;++i)if(levels[i]!=test.expected[i])return false;
    }
    return true;
}
bool tailored_cases() noexcept {
    struct Case {u32 count;u32 text[32];u8 windows[32],standard[32];u8 base;};
    static const Case cases[]{
#include "bidi_tailored_weak_cases.inc"
#include "bidi_tailored_bracket_cases.inc"
#include "bidi_tailored_control_cases.inc"
    };
    for(const auto& test:cases)for(u32 profile=0;profile<2;++profile) {
        unicode_bidi_classify({test.text,test.count},{cells,test.count});
        const auto policy=static_cast<UnicodeParagraphDirection>(test.base);
        const u8 level=profile?bidi_resolve_tailored({cells,test.count},policy,scratch):unicode_bidi_resolve({cells,test.count},policy,scratch);
        if(level!=test.base)return false;
        unicode_bidi_line_levels({cells,test.count},level,levels);
        for(u32 i=0;i<test.count;++i) {
            const u8 expected=profile?test.windows[i]:test.standard[i];
            if(levels[i]!=expected || cells[i].scalar!=test.text[i] || cells[i].original!=unicode_properties(test.text[i])->bidi_class())return false;
        }
        if(profile) {
            // A subsequent standards resolution uses the same untouched inputs.
            unicode_bidi_line_levels({cells,test.count},unicode_bidi_resolve({cells,test.count},policy,scratch),levels);
            for(u32 i=0;i<test.count;++i)if(levels[i]!=test.standard[i])return false;
        }
    }
    return bidi_resolve_tailored({},UnicodeParagraphDirection::auto_rtl,scratch)==1;
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
        u8 saved[128];
        const u8 level=unicode_bidi_resolve({cells,count},static_cast<UnicodeParagraphDirection>(random%4),scratch);
        for(u32 i=0;i<count;++i) {
            const auto& cell=cells[i];saved[i]=cell.level;
            if(cell.level==255)continue;
            if(cell.level>126 || cell.explicit_level>125 || (cell.resolved!=B::l && cell.resolved!=B::r && cell.resolved!=B::en && cell.resolved!=B::an))return false;
            if((cell.resolved==B::r)!=(bool)(cell.level&1))return false;
        }
        unicode_bidi_line_levels({cells,count},level,levels);
        for(u32 i=0;i<count;++i)if((levels[i]==255)!=(saved[i]==255) || cells[i].level!=saved[i])return false;
    }
    return true;
}
bool long_cases() noexcept {
    constexpr u32 depth=10000,count=depth*2+1;
    auto* memory=static_cast<u8*>(pages_allocate(count*20u));if(!memory)return false;
    auto* items=reinterpret_cast<UnicodeBidiCell*>(memory);auto* order=reinterpret_cast<u32*>(memory+count*16u);
    bool ok=true;
    for(u32 mode=0;mode<2 && ok;++mode) {
        for(u32 i=0;i<depth;++i){items[i].scalar=mode?0x2068:'(';items[i].original=mode?B::fsi:B::on;}
        items[depth].scalar=0x5d0;items[depth].original=B::r;
        for(u32 i=depth+1;i<count;++i){items[i].scalar=mode?0x2069:')';items[i].original=mode?B::pdi:B::on;}
        ok=unicode_bidi_resolve({items,count},UnicodeParagraphDirection::ltr,order)==0 && items[count-1].level==0;
        if(ok && !mode)for(u32 i=0;i<count;++i)if(items[i].level!=(i==depth?1:0)){ok=false;break;}
        if(ok && mode)ok=items[depth].level==125;
    }
    if(ok) {
        // Long disjoint empty pairs exercise the candidate's successor map.
        // The final L supplies following context to every pair in one run.
        for(u32 i=0;i<count;++i){items[i].scalar=i+1==count?'a':i&1?')':'(';items[i].original=i+1==count?B::l:B::on;}
        bidi_resolve_with_brackets({items,count},UnicodeParagraphDirection::ltr,order,bidi_tailored_brackets);
        for(u32 i=0;i<count;++i)if(items[i].level!=0){ok=false;break;}
    }
    if(ok) {
        // One long ET run preserves only its first element as neutral in the
        // observed AN/CS context. This also exercises constant scratch usage.
        for(u32 i=0;i<count;++i) {
            const u32 scalar=i==0?0x661:i==1?',':i+1==count?'1':'$';
            items[i].scalar=scalar;items[i].original=unicode_properties(scalar)->bidi_class();
        }
        bidi_resolve_tailored({items,count},UnicodeParagraphDirection::rtl,order);
        for(u32 i=0;i<count;++i)if(items[i].level!=(i==1 || i==2?1:2)){ok=false;break;}
    }
    if(ok) {
        // Bracket context crosses a long separator span without suffix scans.
        for(u32 i=0;i<count;++i) {
            const u32 scalar=i==0?'a':i==1?'(':i==count-5 || i==count-4?'$':i==count-3?'1':i==count-2?0x5d0:i==count-1?')':' ';
            items[i].scalar=scalar;items[i].original=unicode_properties(scalar)->bidi_class();
        }
        bidi_resolve_tailored({items,count},UnicodeParagraphDirection::rtl,order);
        for(u32 i=0;i<count;++i)if(items[i].level!=(i==0 || i==count-4 || i==count-3?2:1)){ok=false;break;}
    }
    if(ok) {
        // Isolate controls retain source identity but do not create levels or
        // isolate stacks in the tailored policy.
        for(u32 i=0;i<count;++i) {
            const u32 scalar=i<depth?0x2068:i==depth?0x5d0:0x2069;
            items[i].scalar=scalar;items[i].original=unicode_properties(scalar)->bidi_class();
        }
        bidi_resolve_tailored({items,count},UnicodeParagraphDirection::ltr,order);
        for(u32 i=0;i<count;++i)if(items[i].level!=(i==depth?1:255) || items[i].scalar!=(i<depth?0x2068u:i==depth?0x5d0u:0x2069u) ||
            items[i].original!=(i<depth?B::fsi:i==depth?B::r:B::pdi)){ok=false;break;}
        if(ok){unicode_bidi_resolve({items,count},UnicodeParagraphDirection::ltr,order);ok=items[depth].level==125;}
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
