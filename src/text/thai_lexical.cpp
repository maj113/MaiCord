#include "maicord/text/detail/thai_lexical.hpp"
#include "maicord/text/detail/thai_syllable.hpp"
#include "maicord/sys/intrin.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {namespace {
// Word graph generated from ICU and PyThaiNLP word lists; see the file header
// and tools/unicode/thai_break_data.py for the node encoding.
#include "generated/thai_lexical_data.inc"

constexpr u32 no_node=0xffffffffu;
struct UnaryPatterns { u16 values[128]; };
static_assert(sizeof(thai_word_special)/sizeof(thai_word_special[0])<=48);
// Ordinary labels have zero distance; compact headers carry their target
// distance. Resolve both with one comparison before reading any reference.
constexpr UnaryPatterns unary_patterns() noexcept {
    UnaryPatterns result{};
    for(u32 i=0;i<80;++i)result.values[i]=static_cast<u16>(i);
    for(u32 i=0;i<sizeof(thai_word_special)/sizeof(thai_word_special[0]);++i)
        result.values[80+i]=thai_word_special[i];
    return result;
}
constexpr auto unary=unary_patterns();
u32 graph_rank(u64 value) noexcept {
    return static_cast<u32>(__popcnt64(value));
}
u32 graph_reference(u32 node,u32 at) noexcept {
    u32 value=0,shift=0;
    for(;;){const u32 byte=thai_word_graph[at++];value|=(byte&127)<<shift;if(!(byte&128))break;shift+=7;}
    return value<thai_word_reference_count?thai_word_references[value]:node-(value-thai_word_reference_count);
}
u32 graph_child(u32 node,u32 label) noexcept {
    const u32 header=thai_word_graph[node];u32 at=node+1;
    if(header<128){
        const u32 pattern=unary.values[header];
        if((pattern&255)!=label)return no_node;
        return pattern>>8?node-(pattern>>8):graph_reference(node,at);
    }
    if(header>=248){
        const u8* mask=thai_word_graph+at;
        const u64 low=byte_load_le64(mask),high=mask[8]|(u64{mask[9]}<<8),word=label<64?low:high,bit=1ull<<(label&63);
        if(!(word&bit))return no_node;
        const u32 index=graph_rank(word&(bit-1))+(label<64?0u:graph_rank(low)),width=header&2?3u:2u;
        const u8* distance=mask+10+index*width;
        return node-(distance[0]|(distance[1]<<8)|(width==3?static_cast<u32>(distance[2])<<16:0u));
    }
    for(u32 edges=(header-128)>>1;edges;--edges){
        const u32 packed=thai_word_graph[at++],edge=packed&127;
        if(edge==label)return packed&128?0:graph_reference(node,at);
        if(edge>label)break;
        // Earlier edges only need their encoded length, not a resolved child.
        if(!(packed&128))while(thai_word_graph[at++]&128){}
    }
    return no_node;
}
// A word may end before a scalar that begins a cluster, and not before a
// consonant silenced by THANTHAKHAT one or two scalars later (as in ฟิลด์).
bool lexical_end(ConstSlice<u32> text,u32 end) noexcept {
    if(end==text.count)return true;
    if(!native_thai_boundary_class(text[end]))return false;
    if(text[end]-0xe01u>=46)return true;
    return (end+1>=text.count||text[end+1]!=0xe4c)&&(end+2>=text.count||text[end+2]!=0xe4c);
}
}
NativeThaiWordCandidates native_thai_lexical_candidates(ConstSlice<u32> text,u32 first) noexcept {
    NativeThaiWordCandidates output{};
    if(!text.data||first>=text.count)return output;
    const u32 cp=text[first];
    if(cp==0x20||cp==0x200b||cp==0xe2f||cp==0xe46){output.lengths[0]=1;return output;}
    u32 node=cp-0xe00u<sizeof(thai_word_initial)/sizeof(thai_word_initial[0])?thai_word_initial[cp-0xe00u]:no_node;
    // Lengths grow, so each word goes first and the eight longest are kept.
    for(u32 length=2;node!=no_node&&length<=thai_word_longest&&length<=text.count-first;++length){
        const u32 label=text[first+length-1]-0xe00u;
        node=label<128?graph_child(node,label):no_node;
        if(node==no_node||thai_word_graph[node]<128||!(thai_word_graph[node]&1)||!lexical_end(text,first+length))continue;
        for(u32 i=7;i;--i)output.lengths[i]=output.lengths[i-1];
        output.lengths[0]=static_cast<i32>(length);
    }
    return output;
}
}
