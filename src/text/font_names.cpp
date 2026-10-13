#include "maicord/text/font_names.hpp"
#include "maicord/text/base.hpp"
namespace maicord {namespace {
u16 word(ConstSlice<u8> bytes,u32 at) noexcept {return byte_load_be16(bytes.data+at);}
bool unicode(u16 platform,u16 encoding) noexcept {return (platform==0&&encoding<=6)||(platform==3&&(encoding==0||encoding==1||encoding==10));}
bool span(ConstSlice<u8> table,u32 storage,u32 offset,u32 length) noexcept {
    return storage<=table.count&&offset<=table.count-storage&&length<=table.count-storage-offset;
}
bool tag_is(ConstSlice<u8> tag,const char* text,u32 length) noexcept {
    if(tag.count!=length*2)return false;
    for(u32 i=0;i<length;++i){const u16 c=word(tag,i*2);if((c>='A'&&c<='Z'?c+32:c)!=static_cast<u8>(text[i]))return false;}
    return true;
}
bool english_tag(ConstSlice<u8> tag) noexcept {
    return tag.count>=4&&(word(tag,0)|32)=='e'&&(word(tag,2)|32)=='n'&&(tag.count==4||(tag.count>=6&&word(tag,4)=='-'));
}
}
SfntStatus sfnt_names_open(ConstSlice<u8> table,SfntNames* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};if(!table.data)return SfntStatus::invalid_input;
    if(table.count<6)return SfntStatus::truncated;
    const u16 version=word(table,0),count=word(table,2),storage=word(table,4);
    if(version>1)return SfntStatus::unsupported;
    u32 end=6+static_cast<u32>(count)*12;u16 tags=0;
    if(end>table.count)return SfntStatus::truncated;
    if(version){if(table.count-end<2)return SfntStatus::truncated;tags=word(table,end);end+=2;
        if(static_cast<u32>(tags)*4>table.count-end)return SfntStatus::truncated;end+=static_cast<u32>(tags)*4;}
    if(storage<end)return SfntStatus::malformed;if(storage>table.count)return SfntStatus::truncated;
    for(u32 i=0;i<count;++i){const u32 at=6+i*12;if(!span(table,storage,word(table,at+10),word(table,at+8)))return SfntStatus::truncated;}
    for(u32 i=0;i<tags;++i){const u32 at=6+static_cast<u32>(count)*12+2+i*4;
        if(!span(table,storage,word(table,at+2),word(table,at)))return SfntStatus::truncated;
        if(word(table,at)&1)return SfntStatus::malformed;}
    *output={table,count,storage,tags,version};return SfntStatus::ok;
}
SfntStatus sfnt_name_record(const SfntNames& names,u32 index,SfntName* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!names.table.data||index>=names.records)return SfntStatus::invalid_input;
    const u32 at=6+index*12;const u16 language=word(names.table,at+4);
    SfntName name{{names.table.data+names.storage+word(names.table,at+10),word(names.table,at+8)}, {},
        word(names.table,at),word(names.table,at+2),language,word(names.table,at+6)};
    if(names.version&&language>=0x8000&&static_cast<u32>(language-0x8000)<names.language_tags){
        const u32 tag=6+static_cast<u32>(names.records)*12+2+static_cast<u32>(language-0x8000)*4;
        name.language_tag={names.table.data+names.storage+word(names.table,tag+2),word(names.table,tag)};
    }
    *output=name;return SfntStatus::ok;
}
SfntStatus sfnt_name_english(const SfntNames& names,u16 id,SfntName* output,bool* found) noexcept {
    if(output)*output={};if(found)*found=false;if(!output||!found||!names.table.data)return SfntStatus::invalid_input;
    u32 best=0xffffffff;SfntName selected{};
    for(u32 i=0;i<names.records;++i){
        // Opening already validated every record span. Unrelated IDs need no
        // descriptor or language-tag view, including absent requested names.
        if(word(names.table,6+i*12+6)!=id)continue;
        SfntName name{};const auto status=sfnt_name_record(names,i,&name);if(status!=SfntStatus::ok)return status;
        if(name.id!=id||!unicode(name.platform,name.encoding))continue;
        u32 language=3;
        if(name.language>=0x8000){if(!names.version||!name.language_tag.data)continue;
            if(tag_is(name.language_tag,"en-us",5))language=0;else if(english_tag(name.language_tag))language=1;
        }else if(name.platform==3){if(name.language==0x409)language=0;else if((name.language&0x3ff)==9)language=1;}
        else if(!name.language)language=2;
        const u32 rank=language*2+(name.platform==3?0:1);
        if(rank<best){best=rank;selected=name;if(!rank)break;} // First rank-zero record cannot be improved or displaced by a tie.
    }
    if(best!=0xffffffff){*output=selected;*found=true;}return SfntStatus::ok;
}
SfntStatus sfnt_name_decode(const SfntName& name,Slice<u16> output,u32* units) noexcept {
    if(!units)return SfntStatus::invalid_input;*units=0;
    if((name.bytes.count&&!name.bytes.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    if(!unicode(name.platform,name.encoding))return SfntStatus::unsupported;
    if(name.bytes.count&1)return SfntStatus::malformed;const u32 count=name.bytes.count/2;
    for(u32 i=0;i<count;++i){const u16 c=word(name.bytes,i*2);
        if(c>=0xd800&&c<=0xdbff){if(i+1==count)return SfntStatus::malformed;const u16 next=word(name.bytes,(++i)*2);if(next<0xdc00||next>0xdfff)return SfntStatus::malformed;}
        else if(c>=0xdc00&&c<=0xdfff)return SfntStatus::malformed;
    }
    if(count>output.count)return SfntStatus::output_too_small;
    for(u32 i=0;i<count;++i)output[i]=word(name.bytes,i*2);*units=count;return SfntStatus::ok;
}
SfntStatus sfnt_font_attributes(const SfntFace& face,SfntFontAttributes* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};ConstSlice<u8> table{};
    const auto status=sfnt_table(face,sfnt_tag('O','S','/','2'),&table);if(status!=SfntStatus::ok)return status;
    if(table.count<64)return SfntStatus::truncated;
    const u16 weight=word(table,4),width=word(table,6);
    if(!weight||weight>1000||!width||width>9)return SfntStatus::malformed;
    *output={weight,width,word(table,62),word(table,0)};return SfntStatus::ok;
}
}
