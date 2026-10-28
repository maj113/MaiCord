#pragma once
#include "maicord/text/font_names.hpp"
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
SfntStatus english(const SfntNames& names,u16 id,SfntName* output,bool* found) noexcept {
    if(output)*output={};if(found)*found=false;if(!output||!found||!names.table.data)return SfntStatus::invalid_input;
    u32 best=0xffffffff;SfntName selected{};
    for(u32 i=0;i<names.records;++i){SfntName name{};const auto status=sfnt_name_record(names,i,&name);if(status!=SfntStatus::ok)return status;
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
