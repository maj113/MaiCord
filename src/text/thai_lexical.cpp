#include "thai_lexical.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {namespace {
#include "generated/thai_lexical_data.inc"

u32 lexical_rank(u64 value) noexcept {
    value-=(value>>1)&0x5555555555555555ull;
    value=(value&0x3333333333333333ull)+((value>>2)&0x3333333333333333ull);
    value=(value+(value>>4))&0x0f0f0f0f0f0f0f0full;
    return static_cast<u32>((value*0x0101010101010101ull)>>56);
}
u32 lexical_variable(const u8*& data) noexcept {
    u32 value=0,shift=0;
    for(;;){const u32 byte=*data++;value|=(byte&127)<<shift;if(!(byte&128))return value;shift+=7;}
}
u32 lexical_class(u32 cp) noexcept {
    const u32 index=cp-0xe00u;
    return index<96?(thai_lexical_flags[index>>2]>>((index&3)*2))&3u:1u;
}
bool lexical_end(ConstSlice<u32> text,u32 end) noexcept {
    if(!(lexical_class(text[end-1])&2))return false;
    if(end==text.count)return true;
    if(!(lexical_class(text[end])&1))return false;
    return (end+1>=text.count||text[end+1]!=0xe4c)&&
        (end+2>=text.count||text[end+2]!=0xe4c);
}
void lexical_insert(NativeThaiWordCandidates& output,i32 value) noexcept {
    const auto magnitude=[](i32 v) noexcept {return v<0?0u-static_cast<u32>(v):static_cast<u32>(v);};
    u32 at=0;while(at<8&&magnitude(output.lengths[at])>magnitude(value))++at;
    if(at==8)return;
    if(magnitude(output.lengths[at])==magnitude(value)){output.lengths[at]=value;return;}
    for(u32 i=7;i>at;--i)output.lengths[i]=output.lengths[i-1];
    output.lengths[at]=value;
}
bool lexical_contains(const u8* values,u32 count,u16 key) noexcept {
    u32 first=0,last=count;
    while(first<last){const u32 middle=first+(last-first)/2;const u16 value=byte_load_le16(values+middle*2);
        if(value<key)first=middle+1;else last=middle;}
    return first<count&&byte_load_le16(values+first*2)==key;
}
void lexical_words(ConstSlice<u32> text,u32 first,NativeThaiWordCandidates& output) noexcept {
    if(text.count-first<2)return;
    const u32 a=text[first]-0xe00u,b=text[first+1]-0xe00u;
    if(a>=128||b>=128)return;
    const u32 row=thai_lexical_first[a];if(row==255)return;
    const u64 low=thai_lexical_masks[row*2],selected=thai_lexical_masks[row*2+(b>>6)],bit=1ull<<(b&63);
    if(!(selected&bit))return;
    const u32 bucket=thai_lexical_bases[row]+lexical_rank(selected&(bit-1))+(b>=64?lexical_rank(low):0);
    const u8* data=thai_lexical_payload+thai_lexical_offsets[bucket];
    const u32 lengths=byte_load_le16(data);data+=2;
    u16 hash=0;
    for(u32 length=2;length<=14&&length<=text.count-first;++length){
        if(length>2){const u32 cp=text[first+length-1];if(cp-0xe01u>=91)return;
            hash=static_cast<u16>((static_cast<u32>(hash)+1)*((cp-0xe00u+0xa0u)&127));}
        if(!(lengths&(1u<<(length-2))))continue;
        const u32 count=length==2?0:lexical_variable(data);
        if(lexical_end(text,first+length)&&(length==2||lexical_contains(data,count,hash)))
            lexical_insert(output,static_cast<i32>(length));
        data+=count*2;
    }
}
void lexical_special(ConstSlice<u32> text,u32 first,NativeThaiWordCandidates& output) noexcept {
    const u32 initial=text[first]-0xe00u;if(initial>=128)return;
    u32 at=thai_special_starts[initial>=0x2a];
    u32 cursor=first,forced=0,emitted=0;
    while(at<sizeof(thai_special_rules)&&emitted<4){
        const u32 length=thai_special_rules[at],action=thai_special_rules[at+1];
        const u8* prefix=thai_special_rules+at+2;at+=2+length;
        if(cursor==first&&prefix[0]>initial)break;
        bool match=length<=text.count-cursor;
        for(u32 i=0;match&&i<length;++i)match=text[cursor+i]-0xe00u==prefix[i];
        if(!match){cursor=first;forced=0;continue;}
        cursor+=length;
        if(action==254)forced=cursor-first;
        else if(action==255){lexical_insert(output,-static_cast<i32>(cursor-first));++emitted;}
        else if(cursor<text.count&&text[cursor]-0xe00u==action){
            if(forced){output={};output.lengths[0]=static_cast<i32>(forced);return;}
            lexical_insert(output,static_cast<i32>(cursor-first));++emitted;
        }
    }
}
}
NativeThaiWordCandidates native_thai_lexical_candidates(ConstSlice<u32> text,u32 first) noexcept {
    NativeThaiWordCandidates output{};
    if(!text.data||first>=text.count)return output;
    const u32 cp=text[first];
    if(cp==0x20||cp==0x200b||cp==0xe2f||cp==0xe46){output.lengths[0]=1;return output;}
    lexical_words(text,first,output);lexical_special(text,first,output);return output;
}
}
