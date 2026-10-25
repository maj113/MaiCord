#include "maicord/media/deflate.hpp"
#include "maicord/media/regional_flag_svg.hpp"

namespace maicord {
namespace {
struct FlagGroup { u32 packed_offset, packed_bytes, raw_bytes; };
struct FlagEntry { u16 sequence, offset, bytes; u8 group; };
// Generated from assets/regional_flags by tools/generate_regional_flag_bundle.py.
#include "regional_flag_bundle.inc"
static_assert(kFlagCount==262);
static_assert(kFlagScratchBytes<=kRegionalFlagSvgMaximumBytes);

// Writes a record as the SVG text the generator's decode() produces.
struct FlagText {
    u8* at; u8* end; const u8* in; const u8* stop; bool ok=true;
    void put(char c) noexcept {if(at<end)*at++=static_cast<u8>(c);else ok=false;}
    void put(const char* text) noexcept {while(*text)put(*text++);}
    u8 next() noexcept {if(in<stop)return *in++;ok=false;return 0;}
    u32 unsigned_value() noexcept {
        u32 value=0;
        for(u32 shift=0;shift<32;shift+=7){const u8 byte=next();value|=u32{byte&127u}<<shift;if(byte<128)return value;}
        ok=false;return 0;
    }
    // Hundredths in their shortest form; a sign or a second point already ends
    // the previous number. Returns whether this one has a point.
    bool number(bool separate,bool previous_point) noexcept {
        const u32 zigzag=unsigned_value();
        const u32 magnitude=(zigzag>>1)+(zigzag&1),whole=magnitude/100,fraction=magnitude%100;
        const bool negative=zigzag&1,digits=magnitude>=100||!fraction;
        if(separate&&!negative&&(digits||!previous_point))put(' ');
        if(negative)put('-');
        if(digits){char text[10];u32 count=0;u32 value=whole;
            do{text[count++]=static_cast<char>('0'+value%10);value/=10;}while(value);
            while(count)put(text[--count]);}
        if(fraction){put('.');put(static_cast<char>('0'+fraction/10));if(fraction%10)put(static_cast<char>('0'+fraction%10));}
        return fraction!=0;
    }
};

ConstSlice<u8> flag_text(ConstSlice<u8> record,Slice<u8> output) noexcept {
    FlagText t{output.data,output.data+output.count,record.data,record.data+record.count};
    t.put("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\">");
    constexpr const char* tags[]{"path","circle","ellipse","g"};
    constexpr const char* shapes[]{" cx=\""," cy=\""," r=\""," cx=\""," cy=\""," rx=\""," ry=\""};
    for(u8 tag=t.next();t.ok&&tag!=5;tag=t.next()){
        if(tag==4){t.put("</g>");continue;}
        if(tag>3)return {};
        t.put('<');t.put(tags[tag]);
        if(const u8 fill=t.next()){t.put(" fill=\"");for(u8 i=0;i<fill;++i)t.put(static_cast<char>(t.next()));t.put('"');}
        const u8 flags=t.next();
        if(flags&1)t.put(" fill-rule=\"evenodd\"");
        if(flags&2)t.put(" transform=\"rotate(-76.714 13.001 18.001)\"");
        if(tag==0){
            t.put(" d=\"");
            for(u8 letter=t.next();t.ok&&letter;letter=t.next()){
                t.put(static_cast<char>(letter));bool point=false;
                for(u32 i=0,count=t.unsigned_value();t.ok&&i<count;++i)point=t.number(i!=0,point);
            }
            t.put("\"/>");
        } else if(tag==3)t.put('>');
        else {
            for(u32 i=tag==1?0:3,end=tag==1?3:7;i<end;++i){t.put(shapes[i]);t.number(false,false);t.put('"');}
            t.put("/>");
        }
    }
    t.put("</svg>");
    if(!t.ok)return {};
    return {output.data,static_cast<u32>(t.at-output.data)};
}
}
ConstSlice<u8> regional_flag_bundled_svg(u16 sequence, Slice<u8> scratch) noexcept {
    u32 first=0,last=kFlagCount;
    while(first<last){const u32 middle=first+(last-first)/2;
        if(kFlagEntries[middle].sequence<sequence)first=middle+1;else last=middle;}
    if(first==kFlagCount || kFlagEntries[first].sequence!=sequence)return {};
    const FlagEntry& entry=kFlagEntries[first];
    const FlagGroup& group=kFlagGroups[entry.group];
    if(scratch.count<group.raw_bytes)return {};
    const DeflateInputSpan input{kFlagPacked+group.packed_offset,group.packed_bytes};
    DeflateWorkspace workspace;
    if(!zlib_inflate({&input,1},{scratch.data,group.raw_bytes},group.raw_bytes,&workspace).ok())return {};
    return flag_text({scratch.data+entry.offset,entry.bytes},
                     {scratch.data+group.raw_bytes,scratch.count-group.raw_bytes});
}
u64 regional_flag_bundled_text_checksum() noexcept {return kFlagTextChecksum;}
} // namespace maicord
