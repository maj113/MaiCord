#include "maicord/text/lookup_selection.hpp"
#include "maicord/core/memory.hpp"
#include "context_lookups.hpp"
namespace maicord {namespace {
template<class T> bool valid(Slice<T> s) noexcept {return !s.count||s.data;}
bool spend(u64* work,u64 n=1) noexcept {if(n>*work)return false;*work-=n;return true;}
struct Selection {Slice<u64> bits;Slice<u16> queue;u32 count;u16 limit;};
SfntStatus select_lookup(void* context,u16 index) noexcept {
    auto& s=*static_cast<Selection*>(context);if(index>=s.limit)return SfntStatus::invalid_input;
    auto& word=s.bits[index/64];const u64 bit=1ull<<(index%64);if(word&bit)return SfntStatus::ok;
    if(s.count==s.queue.count)return SfntStatus::output_too_small;word|=bit;s.queue[s.count++]=index;return SfntStatus::ok;
}
}
SfntStatus sfnt_lookup_selection(const SfntLayout& layout,ConstSlice<u16> roots,Slice<u64> bits,Slice<u16> queue,SfntLookupSelectionResult* output,u32 budget) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!layout.table.data||static_cast<u8>(layout.kind)>1||(roots.count&&!roots.data)||!valid(bits)||!valid(queue))return SfntStatus::invalid_input;
    const u32 words=(layout.lookup_count+63u)/64;if(bits.count<words||queue.count<layout.lookup_count)return SfntStatus::output_too_small;
    u64 work=budget;Selection selection{bits,queue,0,layout.lookup_count};
    struct Report {SfntLookupSelectionResult& out;const Selection& selected;u32 initial;const u64& work;~Report() noexcept {out={selected.count,initial-static_cast<u32>(work)};}} report{*output,selection,budget,work};
    if(!spend(&work,words))return SfntStatus::resource_limit;for(u32 i=0;i<words;++i)bits[i]=0;
    for(u32 i=0;i<roots.count;++i){if(!spend(&work))return SfntStatus::resource_limit;const auto status=select_lookup(&selection,roots[i]);if(status!=SfntStatus::ok)return status;}
    for(u32 i=0;i<selection.count;++i){if(!spend(&work))return SfntStatus::resource_limit;SfntLookup lookup{};u32 used=0;
        auto status=sfnt_layout_lookup(layout,selection.queue[i],&lookup,static_cast<u32>(work),&used);work-=used;if(status!=SfntStatus::ok)return status;
        const u16 context_type=layout.kind==SfntLayoutKind::substitution?5:7;if(lookup.type!=context_type&&lookup.type!=context_type+1)continue;
        for(u16 j=0;j<lookup.subtable_count;++j){if(!spend(&work))return SfntStatus::resource_limit;ConstSlice<u8> bytes{};
            status=sfnt_lookup_subtable(lookup,j,&bytes);if(status!=SfntStatus::ok)return status;
            status=sfnt_context_visit_lookups(bytes,lookup.type==context_type?SfntContextKind::sequence:SfntContextKind::chained,layout.lookup_count,select_lookup,&selection,static_cast<u32>(work),&used);
            work-=used;if(status!=SfntStatus::ok)return status;}
    }return SfntStatus::ok;
}
}
