#pragma once
#include "maicord/text/fonts/names.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord::font_names_reference {
inline u16 word(ConstSlice<u8> bytes,u32 at) noexcept {return byte_load_be16(bytes.data+at);}
bool unicode(u16 platform,u16 encoding) noexcept {return (platform==0&&encoding<=6)||(platform==3&&(encoding==0||encoding==1||encoding==10));}
bool tag_is(ConstSlice<u8> tag,const char* text,u32 length) noexcept {
    if(tag.count!=length*2)return false;
    for(u32 i=0;i<length;++i){const u16 c=word(tag,i*2);if((c>='A'&&c<='Z'?c+32:c)!=static_cast<u8>(text[i]))return false;}
    return true;
}
bool english_tag(ConstSlice<u8> tag) noexcept {
    return tag.count>=4&&(word(tag,0)|32)=='e'&&(word(tag,2)|32)=='n'&&(tag.count==4||(tag.count>=6&&word(tag,4)=='-'));
}
// Record `index` (below names.records) read from the table as the format lays it out; a version-1 tag reference past
// the tag list leaves the record without a language tag.
inline SfntName record(const SfntNames& names,u32 index) noexcept {
    const u32 at=6+index*12;const u16 language=word(names.table,at+4);
    SfntName name{{names.table.data+names.storage+word(names.table,at+10),word(names.table,at+8)},{},
        word(names.table,at),word(names.table,at+2),language,word(names.table,at+6)};
    if(names.version&&language>=0x8000&&static_cast<u32>(language-0x8000)<names.language_tags){
        const u32 tag=6+static_cast<u32>(names.records)*12+2+static_cast<u32>(language-0x8000)*4;
        name.language_tag={names.table.data+names.storage+word(names.table,tag+2),word(names.table,tag)};
    }
    return name;
}
SfntStatus english(const SfntNames& names,u16 id,SfntName* output,bool* found) noexcept {
    if(output)*output={};if(found)*found=false;if(!output||!found||!names.table.data)return SfntStatus::invalid_input;
    u32 best=0xffffffff;SfntName selected{};
    for(u32 i=0;i<names.records;++i){const SfntName name=record(names,i);
        if(name.id!=id||!unicode(name.platform,name.encoding))continue;
        u32 language=3;
        if(name.language>=0x8000){if(!names.version||!name.language_tag.data)continue;
            if(tag_is(name.language_tag,"en-us",5))language=0;else if(english_tag(name.language_tag))language=1;
        }else if(name.platform==3){if(name.language==0x409)language=0;else if((name.language&0x3ff)==9)language=1;}
        else if(!name.language)language=2;
        const u32 rank=language*2+(name.platform==3?0:1);
        if(rank<best){best=rank;selected=name;}
    }
    if(best!=0xffffffff){*output=selected;*found=true;}return SfntStatus::ok;
}
inline bool equal(const SfntName& a,const SfntName& b) noexcept {
    return a.bytes.data==b.bytes.data&&a.bytes.count==b.bytes.count&&a.language_tag.data==b.language_tag.data&&a.language_tag.count==b.language_tag.count&&
        a.platform==b.platform&&a.encoding==b.encoding&&a.language==b.language&&a.id==b.id;
}
}
