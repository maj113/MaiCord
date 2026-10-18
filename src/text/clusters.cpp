#include "maicord/text/clusters.hpp"
#include "maicord/text/base.hpp"
#include "maicord/sys/intrin.hpp"

namespace maicord {namespace {
constexpr u32 absent=0xffffffffu;
u32 word_count(u32 bits) noexcept {return (bits>>6)+((bits&63)?1u:0u);}
u64 below(u32 bit) noexcept {return bit?((u64{1}<<bit)-1):0;}
u32 first_bit(u64 bits) noexcept {unsigned long at=0;_BitScanForward64(&at,bits);return static_cast<u32>(at);}
u32 last_bit(u64 bits) noexcept {unsigned long at=0;_BitScanReverse64(&at,bits);return static_cast<u32>(at);}
u32 find(const SfntClusterMap& map,u32 index,bool forward) noexcept {
    u32 offsets[6],offset=0,count=word_count(map.text_count);
    for(u32 level=0;level<map.levels;++level){offsets[level]=offset;const u32 word=index>>6,bit=index&63;
        if(word>=count)return absent;
        const u64 value=map.storage.data[offset+word] & (forward?~below(bit):(bit==63?~u64{0}:below(bit+1)));
        if(value){u32 found=word*64+(forward?first_bit(value):last_bit(value));
            while(level){--level;const u64 child=map.storage.data[offsets[level]+found];found=found*64+(forward?first_bit(child):last_bit(child));}return found;}
        if(!forward && !word)return absent;index=forward?word+1:word-1;offset+=count;count=word_count(count);
    }return absent;
}
void clear_word(SfntClusterMap& map,u32 word,u64 mask) noexcept {
    u64& value=map.storage.data[word];value&=~mask;if(value)return;
    u32 offset=word_count(map.text_count),count=word_count(offset);
    for(u32 level=1;level<map.levels;++level){const u32 parent=word>>6;u64& summary=map.storage.data[offset+parent];summary&=~(u64{1}<<(word&63));if(summary)return;word=parent;offset+=count;count=word_count(count);}
}
u64 erase_mask(u32 word,u32 first,u32 last) noexcept {
    const u32 lo=word==(first>>6)?first&63:0,hi=word==(last>>6)?last&63:63;
    return ~below(lo)&(hi==63?~u64{0}:below(hi+1));
}
}
u32 sfnt_cluster_map_words(u32 count) noexcept {
    u32 result=0;while(count){count=word_count(count);result+=count;if(count==1)break;}return result;
}
SfntStatus sfnt_cluster_map_open(u32 text_count,ConstSlice<u32> starts,Slice<u64> storage,SfntClusterMap* output,u64 work) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};const u32 words=sfnt_cluster_map_words(text_count);
    if((starts.count && !starts.data) || (storage.count && (!storage.data || (reinterpret_cast<usize>(storage.data)&7))))return SfntStatus::invalid_input;
    if(words>storage.count)return SfntStatus::output_too_small;
    if(static_cast<u64>(words)*3+starts.count>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<starts.count;++i)if(starts.data[i]>=text_count || (i?starts.data[i]<=starts.data[i-1]:starts.data[i]!=0))return SfntStatus::invalid_input;
    SfntClusterMap result{};result.storage={storage.data,words};result.text_count=text_count;u32 count=text_count;
    while(count){count=word_count(count);++result.levels;if(count==1)break;}
    clear_bytes(storage.data,words*sizeof(u64));
    if(text_count){if(starts.count){for(u32 i=0;i<starts.count;++i)storage.data[starts.data[i]>>6]|=u64{1}<<(starts.data[i]&63);}
        else {const u32 leaves=word_count(text_count);for(u32 i=0;i<leaves;++i)storage.data[i]=~u64{0};if(text_count&63)storage.data[leaves-1]=below(text_count&63);}
        u32 previous=0,previous_count=word_count(text_count),offset=previous_count;
        for(u32 level=1;level<result.levels;++level){for(u32 i=0;i<previous_count;++i)if(storage.data[previous+i])storage.data[offset+(i>>6)]|=u64{1}<<(i&63);
            previous=offset;previous_count=word_count(previous_count);offset+=previous_count;}
    }
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_cluster_map_find(const SfntClusterMap& map,u32 source,u32* first,u32* end) noexcept {
    if(!first || !end)return SfntStatus::invalid_input;*first=*end=0;if(source>=map.text_count)return SfntStatus::invalid_input;
    *first=find(map,source,false);if(*first==absent)return SfntStatus::invalid_input;
    const u32 next=source+1==map.text_count?absent:find(map,source+1,true);*end=next==absent?map.text_count:next;return SfntStatus::ok;
}
SfntStatus sfnt_cluster_map_plan(const SfntClusterMap& map,u32 first,u32 last,SfntClusterMerge* output,u64 work,u64* used) noexcept {
    if(used)*used=0;if(!output)return SfntStatus::invalid_input;*output={};if(first>last || last>=map.text_count)return SfntStatus::invalid_input;
    SfntClusterMerge plan{first,last,0};u64 charged=0;const u32 search_cost=map.levels*2+2;
    if(first!=last){u32 at=first+1;for(;;){if(search_cost>work-charged){if(used)*used=charged;return SfntStatus::resource_limit;}charged+=search_cost;
            const u32 found=find(map,at,true);plan.commit_work+=search_cost;if(found==absent || found>last)break;
            plan.commit_work+=map.levels+1;if((found>>6)==(last>>6))break;at=((found>>6)+1)*64;
        }}
    if(used)*used=charged;*output=plan;return SfntStatus::ok;
}
void sfnt_cluster_map_commit(SfntClusterMap* map,const SfntClusterMerge& plan) noexcept {
    if(plan.first==plan.last)return;u32 at=plan.first+1;
    for(;;){const u32 found=find(*map,at,true);if(found==absent || found>plan.last)return;const u32 word=found>>6;
        clear_word(*map,word,erase_mask(word,plan.first+1,plan.last));if(word==(plan.last>>6))return;at=(word+1)*64;}
}
SfntStatus sfnt_cluster_map_split(SfntClusterMap* map,u32 source,u64 work) noexcept {
    if(!map || source>=map->text_count)return SfntStatus::invalid_input;
    if(map->levels>work)return SfntStatus::resource_limit;
    u32 index=source,offset=0,count=word_count(map->text_count);
    for(u32 level=0;level<map->levels;++level){const u32 word=index>>6;u64& value=map->storage.data[offset+word];const u64 previous=value;
        value|=u64{1}<<(index&63);if(previous)break;index=word;offset+=count;count=word_count(count);}
    return SfntStatus::ok;
}
SfntStatus sfnt_cluster_map_export(const SfntClusterMap& map,SfntGlyphView glyphs,Slice<u32> characters,Slice<u8> starts,u64 work) noexcept {
    if(!sfnt_glyph_view_valid(glyphs) || glyphs.record_size<sizeof(SfntClusterGlyph) || (characters.count && !characters.data) || (starts.count && !starts.data))return SfntStatus::invalid_input;
    const u32 n=sfnt_glyph_count(glyphs);if(characters.count<map.text_count || (starts.count && starts.count<n))return SfntStatus::output_too_small;
    const u64 required=static_cast<u64>(map.text_count)*2+static_cast<u64>(n)*(map.levels*2+3);if(required>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<map.text_count;++i)characters.data[i]=absent;
    for(u32 i=0;i<n;++i){const auto* record=reinterpret_cast<const u8*>(&sfnt_glyph_at(glyphs,i));const u32 source=byte_load_le32(record+4);if(source>=map.text_count)return SfntStatus::invalid_input;
        const u32 root=find(map,source,false);if(root==absent)return SfntStatus::invalid_input;const bool first=characters.data[root]==absent;
        if(first)characters.data[root]=i;if(starts.count)starts.data[i]=first?1u:0u;}
    u32 glyph=absent;
    for(u32 i=0;i<map.text_count;++i){if(map.storage.data[i>>6]&(u64{1}<<(i&63)))glyph=characters.data[i];if(glyph==absent)return SfntStatus::invalid_input;characters.data[i]=glyph;}
    return SfntStatus::ok;
}
}
