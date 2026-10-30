#include "maicord/text/glyph_buffer.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {namespace {
inline bool aligned(const void* p) noexcept {return (reinterpret_cast<usize>(p)&(alignof(SfntGsubGlyph)-1))==0;}
bool valid(const SfntGlyphBuffer& b) noexcept {return b.record_size>=sizeof(SfntGsubGlyph) && b.record_size%alignof(SfntGsubGlyph)==0 && (!b.storage.count || (b.storage.data && aligned(b.storage.data))) && b.capacity==b.storage.count/b.record_size && b.gap_begin<=b.gap_end && b.gap_end<=b.capacity;}
inline u32 count(const SfntGlyphBuffer& b) noexcept {return b.capacity-(b.gap_end-b.gap_begin);}
inline u32 distance(const SfntGlyphBuffer& b,u32 at) noexcept {return b.gap_begin==b.gap_end?0u:at>b.gap_begin?at-b.gap_begin:b.gap_begin-at;}
// Null storage has zero capacity, so every index reaching it is zero.
inline u8* slot(const SfntGlyphBuffer& b,u32 index) noexcept {return b.storage.data+static_cast<usize>(index)*b.record_size;}
void seek(SfntGlyphBuffer& b,u32 at) noexcept {
    if(b.gap_begin==b.gap_end){b.gap_begin=b.gap_end=at;return;}
    if(at<b.gap_begin){const u32 n=b.gap_begin-at;copy_bytes(slot(b,b.gap_end-n),slot(b,at),n*b.record_size);b.gap_end-=n;b.gap_begin=at;}
    else if(at>b.gap_begin){const u32 n=at-b.gap_begin;copy_bytes(slot(b,b.gap_begin),slot(b,b.gap_end),n*b.record_size);b.gap_begin+=n;b.gap_end+=n;}
}
void advance(SfntGlyphBuffer& b,u32 n) noexcept {if(b.gap_begin!=b.gap_end)copy_bytes(slot(b,b.gap_begin),slot(b,b.gap_end),n*b.record_size);b.gap_begin+=n;b.gap_end+=n;}
u32 rank(ConstSlice<u32> positions,u32 index) noexcept {u32 lo=0,hi=positions.count;while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(positions.data[mid]<index)lo=mid+1;else hi=mid;}return lo;}
}
bool sfnt_glyph_view_valid(const SfntGlyphView& v) noexcept {
    return v.record_size>=sizeof(SfntGsubGlyph) && v.record_size%alignof(SfntGsubGlyph)==0 && (!v.prefix_count || (v.prefix && aligned(v.prefix))) && (!v.suffix_count || (v.suffix && aligned(v.suffix))) && static_cast<u64>(v.prefix_count)+v.suffix_count<=0xffffffffu;
}
SfntStatus sfnt_glyph_buffer_open(Slice<u8> storage,u32 record_size,u32 initial_count,SfntGlyphBuffer* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(record_size<sizeof(SfntGsubGlyph) || record_size%alignof(SfntGsubGlyph) || (storage.count && (!storage.data || !aligned(storage.data))))return SfntStatus::invalid_input;
    const u32 capacity=storage.count/record_size;if(initial_count>capacity)return SfntStatus::output_too_small;*output={storage,record_size,capacity,initial_count,capacity};return SfntStatus::ok;
}
SfntStatus sfnt_glyph_buffer_seek(SfntGlyphBuffer* b,u32 at,u64 work) noexcept {
    if(!b || !valid(*b) || at>count(*b))return SfntStatus::invalid_input;if(distance(*b,at)>work)return SfntStatus::resource_limit;seek(*b,at);return SfntStatus::ok;
}
SfntStatus sfnt_glyph_buffer_replace(SfntGlyphBuffer* b,u32 start,ConstSlice<u32> consumed,ConstSlice<u8> replacements,SfntGlyphEdit* output,u64 work) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!b || !valid(*b) || (consumed.count && !consumed.data) || (replacements.count && !replacements.data) || replacements.count%b->record_size)return SfntStatus::invalid_input;
    const u32 before=count(*b),inserted=replacements.count/b->record_size;if(start>before || consumed.count>before || (consumed.count && (start==before || consumed.data[0]!=start)))return SfntStatus::invalid_input;
    if(byte_ranges_overlap(consumed.data,static_cast<u64>(consumed.count)*sizeof(u32),b->storage.data,b->storage.count) || byte_ranges_overlap(replacements.data,replacements.count,b->storage.data,b->storage.count))return SfntStatus::invalid_input;
    if(consumed.count>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<consumed.count;++i)if(consumed.data[i]>=before || (i && consumed.data[i]<=consumed.data[i-1]))return SfntStatus::invalid_input;
    const u64 after=static_cast<u64>(before)-consumed.count+inserted;if(after>b->capacity)return SfntStatus::output_too_small;
    const u32 surviving=consumed.count?consumed.data[consumed.count-1]-start+1-consumed.count:0;
    const bool early=consumed.count && inserted<=b->gap_end-b->gap_begin+1;
    const u32 stationary=early && inserted==b->gap_end-b->gap_begin+1 && consumed.count>1?consumed.data[1]-start-1:0;
    const u64 moved=(!consumed.count && !inserted)?0:static_cast<u64>(distance(*b,start))+inserted+surviving-stationary+(consumed.count && !early?surviving:0);
    if(static_cast<u64>(consumed.count)+moved>work)return SfntStatus::resource_limit;
    SfntGlyphEdit result{consumed,start,before,static_cast<u32>(after),inserted,moved};if(!consumed.count && !inserted){*output=result;return SfntStatus::ok;}
    seek(*b,start);
    if(consumed.count){++b->gap_end;if(early){copy_bytes(slot(*b,b->gap_begin),replacements.data,replacements.count);b->gap_begin+=inserted;}
        for(u32 i=1;i<consumed.count;++i){advance(*b,consumed.data[i]-consumed.data[i-1]-1);++b->gap_end;}
        if(!early)seek(*b,start);}
    if(!early){copy_bytes(slot(*b,b->gap_begin),replacements.data,replacements.count);b->gap_begin+=inserted;}
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_glyph_edit_boundary(const SfntGlyphEdit& edit,u32 old,bool after,u32* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output=0;if(old>edit.old_count)return SfntStatus::invalid_input;*output=old-rank(edit.consumed,old)+((old>edit.start || (old==edit.start && after))?edit.replacement_count:0);return SfntStatus::ok;
}
}
