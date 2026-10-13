#include "maicord/text/font_names.hpp"
#include "maicord/core/byte_order.hpp"
#include "font_names_reference.hpp"
#include <Windows.h>
namespace maicord {namespace {
u32 checks=0;bool print_ok=true;char print_buffer[65536];u32 output_count=0;u16 decoded[32768];
void flush() noexcept {DWORD written=0;if(output_count&&(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),print_buffer,output_count,&written,nullptr)||written!=output_count))print_ok=false;output_count=0;}
void ch(char c) noexcept {if(output_count==sizeof(print_buffer))flush();print_buffer[output_count++]=c;}
void byte_store_be16(u8* p,u16 value) noexcept {p[0]=static_cast<u8>(value>>8);p[1]=static_cast<u8>(value);}
void zero_bytes(u8* p,u32 n) noexcept {for(u32 i=0;i<n;++i)p[i]=0;}
void print(const char* s) noexcept {while(*s)ch(*s++);}
void number(u32 value) noexcept {char digits[16];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(n)ch(digits[--n]);}
void string(ConstSlice<u16> units) noexcept {const char hex[]="0123456789abcdef";ch('"');for(u32 i=0;i<units.count;++i){print("\\u");for(int shift=12;shift>=0;shift-=4)ch(hex[(units[i]>>shift)&15]);}ch('"');}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
void record(u8* p,u16 platform,u16 encoding,u16 language,u16 id,u16 length,u16 offset) noexcept {
    const u16 values[]{platform,encoding,language,id,length,offset};for(u32 i=0;i<6;++i)byte_store_be16(p+i*2,values[i]);
}
bool basic() noexcept {
    u8 table[64]{};byte_store_be16(table+2,3);byte_store_be16(table+4,42);
    record(table+6,3,1,0x411,1,2,0);record(table+18,0,4,0,1,2,2);record(table+30,3,10,0x409,1,6,4);
    const u16 data[]{'J','U','A',0xd83d,0xde00};for(u32 i=0;i<5;++i)byte_store_be16(table+42+i*2,data[i]);
    SfntNames names{};CHECK(sfnt_names_open({table,52},&names)==SfntStatus::ok&&names.records==3&&sizeof(names)==24);
    SfntName name{};bool found=true;CHECK(sfnt_name_english(names,1,&name,&found)==SfntStatus::ok&&found&&name.platform==3&&name.encoding==10&&name.language==0x409);
    u32 units=99;CHECK(sfnt_name_decode(name,{decoded,32768},&units)==SfntStatus::ok&&units==3&&bytes_equal(decoded,data+2,6));
    decoded[0]=0xbeef;CHECK(sfnt_name_decode(name,{decoded,2},&units)==SfntStatus::output_too_small&&!units&&decoded[0]==0xbeef);
    CHECK(sfnt_name_english(names,21,&name,&found)==SfntStatus::ok&&!found&&!name.bytes.data);
    CHECK(sfnt_name_record(names,3,&name)==SfntStatus::invalid_input&&!name.bytes.data);
    byte_store_be16(table+34,0x809);CHECK(sfnt_names_open({table,52},&names)==SfntStatus::ok);CHECK(sfnt_name_english(names,1,&name,&found)==SfntStatus::ok&&found&&name.language==0x809);
    byte_store_be16(table+34,0x411);CHECK(sfnt_names_open({table,52},&names)==SfntStatus::ok);CHECK(sfnt_name_english(names,1,&name,&found)==SfntStatus::ok&&found&&name.platform==0);
    byte_store_be16(table+24,2);CHECK(sfnt_names_open({table,52},&names)==SfntStatus::ok);CHECK(sfnt_name_english(names,1,&name,&found)==SfntStatus::ok&&found&&name.language==0x411&&name.encoding==1);
    CHECK(sfnt_names_open({nullptr,0},&names)==SfntStatus::invalid_input&&!names.table.data);
    CHECK(sfnt_names_open({table,52},nullptr)==SfntStatus::invalid_input);
    CHECK(sfnt_name_english({},1,&name,&found)==SfntStatus::invalid_input&&!found&&!name.bytes.data);
    CHECK(sfnt_name_english({},1,nullptr,&found)==SfntStatus::invalid_input);
    CHECK(sfnt_name_decode({}, {},nullptr)==SfntStatus::invalid_input);
    SfntName empty{};CHECK(sfnt_name_decode(empty,{},&units)==SfntStatus::ok&&!units);
    empty.platform=1;CHECK(sfnt_name_decode(empty,{},&units)==SfntStatus::unsupported&&!units);
    return true;
}
bool tags() noexcept {
    u8 table[96]{};byte_store_be16(table,1);byte_store_be16(table+2,3);byte_store_be16(table+4,52);
    record(table+6,3,1,0x8000,16,2,0);record(table+18,3,1,0x8001,16,2,2);record(table+30,3,1,0x8002,16,2,4);
    byte_store_be16(table+42,2);byte_store_be16(table+44,4);byte_store_be16(table+46,6);byte_store_be16(table+48,10);byte_store_be16(table+50,10);
    const u16 strings[]{'A','B','C','e','n','E','N','-','u','S'};for(u32 i=0;i<10;++i)byte_store_be16(table+52+i*2,strings[i]);
    SfntNames names{};SfntName name{};bool found=false;
    CHECK(sfnt_names_open({table,72},&names)==SfntStatus::ok&&names.language_tags==2);
    CHECK(sfnt_name_english(names,16,&name,&found)==SfntStatus::ok&&found&&name.language==0x8001);
    CHECK(sfnt_name_record(names,2,&name)==SfntStatus::ok&&!name.language_tag.data);
    byte_store_be16(table+22,0x8002);CHECK(sfnt_names_open({table,72},&names)==SfntStatus::ok);CHECK(sfnt_name_english(names,16,&name,&found)==SfntStatus::ok&&found&&name.language==0x8000);
    byte_store_be16(table+10,0x8003);CHECK(sfnt_names_open({table,72},&names)==SfntStatus::ok);CHECK(sfnt_name_english(names,16,&name,&found)==SfntStatus::ok&&!found);
    byte_store_be16(table+44,3);CHECK(sfnt_names_open({table,72},&names)==SfntStatus::malformed&&!names.table.data);
    return true;
}
bool selection_permutations() noexcept {
    // Rotate and permute ordered/unsorted records with language/platform ties,
    // unsupported encodings, absent IDs and dangling version-1 language tags.
    u8 table[512]{};byte_store_be16(table,1);byte_store_be16(table+2,9);byte_store_be16(table+4,128);
    byte_store_be16(table+114,2);byte_store_be16(table+116,4);byte_store_be16(table+118,18);byte_store_be16(table+120,10);byte_store_be16(table+122,22);
    const u16 strings[]{'A','B','C','D','E','F','G','H','I','e','n','e','n','-','U','S'};
    for(u32 i=0;i<16;++i)byte_store_be16(table+128+i*2,strings[i]);
    const u16 variants[][4]{{3,1,0x409,1},{3,10,0x409,1},{0,4,0,1},{3,1,0x809,1},{1,0,0,1},{3,1,0x411,1},{3,1,0x8000,1},{3,1,0x8001,2},{3,1,0x8002,1}};
    u32 state=0x705a381u;
    for(u32 sample=0;sample<512;++sample){u32 order[9];for(u32 i=0;i<9;++i)order[i]=i;
        for(u32 i=9;i>1;--i){state=state*1664525u+1013904223u;const u32 at=state%i,old=order[at];order[at]=order[i-1];order[i-1]=old;}
        for(u32 i=0;i<9;++i){const auto* v=variants[order[i]];const u16 id=sample&1?v[3]:static_cast<u16>(v[3]+20);
            record(table+6+i*12,v[0],v[1],v[2],id,2,static_cast<u16>(order[i]*2));}
        SfntNames names{};CHECK(sfnt_names_open({table,160},&names)==SfntStatus::ok);
        const u16 ids[]{1,2,16,17,21,22};for(u16 id:ids){SfntName actual{},expected{};bool a=false,b=false;
            CHECK(sfnt_name_english(names,id,&actual,&a)==font_names_reference::english(names,id,&expected,&b)&&a==b&&font_names_reference::equal(actual,expected));}
    }return true;
}
bool guards() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup{u8* p;~Cleanup() noexcept{VirtualFree(p,0,MEM_RELEASE);}} cleanup{memory};
    DWORD old=0;CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&old));CHECK(VirtualProtect(memory+page*3,page,PAGE_NOACCESS,&old));
    u8 table[40]{};byte_store_be16(table,1);byte_store_be16(table+2,1);byte_store_be16(table+4,24);
    record(table+6,3,1,0x8000,1,4,0);byte_store_be16(table+18,1);byte_store_be16(table+20,10);byte_store_be16(table+22,4);
    const u16 data[]{'A','B','e','n','-','U','S'};for(u32 i=0;i<7;++i)byte_store_be16(table+24+i*2,data[i]);
    for(u32 size=0;size<=38;++size){auto* end=memory+page-size;copy_bytes(end,table,size);SfntNames names{};
        CHECK(sfnt_names_open({end,size},&names)==(size<38?SfntStatus::truncated:SfntStatus::ok));if(size<38)CHECK(!names.table.data);}
    auto* source=memory+page-38;copy_bytes(source,table,38);CHECK(VirtualProtect(memory,page,PAGE_READONLY,&old));
    SfntNames names{};SfntName name{};bool found=false;CHECK(sfnt_names_open({source,38},&names)==SfntStatus::ok);
    CHECK(sfnt_name_english(names,1,&name,&found)==SfntStatus::ok&&found);
    auto* target=reinterpret_cast<u16*>(memory+page*3-4);u32 units=0;CHECK(sfnt_name_decode(name,{target,2},&units)==SfntStatus::ok&&units==2&&target[0]=='A'&&target[1]=='B');
    CHECK(VirtualProtect(memory,page,PAGE_READWRITE,&old));
    byte_store_be16(memory+page-4,0xd83d);byte_store_be16(memory+page-2,0xde00);name={{memory+page-4,4},{},3,10,0x409,1};
    CHECK(VirtualProtect(memory,page,PAGE_READONLY,&old));CHECK(sfnt_name_decode(name,{target,2},&units)==SfntStatus::ok&&units==2&&target[0]==0xd83d&&target[1]==0xde00);
    CHECK(VirtualProtect(memory,page,PAGE_READWRITE,&old));
    const u16 invalid[][3]{{0xdc00,0,0},{0xd800,0,0},{0xd800,'A',0},{'A',0xdfff,0},{0xd800,0xdbff,0}};
    const u32 sizes[]{1,1,2,2,2};
    for(u32 i=0;i<5;++i){const u32 size=sizes[i]*2;auto* end=memory+page-size;for(u32 j=0;j<sizes[i];++j)byte_store_be16(end+j*2,invalid[i][j]);
        name={{end,size},{},3,1,0x409,1};target[0]=0xbeef;CHECK(sfnt_name_decode(name,{target,2},&units)==SfntStatus::malformed&&!units&&target[0]==0xbeef);}
    name={{memory+page-1,1},{},3,1,0x409,1};CHECK(sfnt_name_decode(name,{target,2},&units)==SfntStatus::malformed);
    for(u32 which=0;which<4;++which){copy_bytes(source,table,38);byte_store_be16(source+(which==0?0:which==1?4:which==2?16:22),0xffff);
        CHECK(sfnt_names_open({source,38},&names)==(which==0?SfntStatus::unsupported:SfntStatus::truncated)&&!names.table.data);}
    copy_bytes(source,table,38);byte_store_be16(source+4,23);CHECK(sfnt_names_open({source,38},&names)==SfntStatus::malformed);
    u8 directory[16]{};byte_store_be32(directory,sfnt_tag('O','S','/','2'));SfntFace face{{memory,page},{directory,16},0,1,0x10000};SfntFontAttributes attr{};
    for(u32 size=0;size<=64;++size){const u32 at=page-size;zero_bytes(memory+at,size);if(size>=8){byte_store_be16(memory+at+4,600);byte_store_be16(memory+at+6,5);}byte_store_be32(directory+8,at);byte_store_be32(directory+12,size);
        CHECK(sfnt_font_attributes(face,&attr)==(size<64?SfntStatus::truncated:SfntStatus::ok));if(size<64)CHECK(!attr.weight);else CHECK(attr.weight==600&&attr.width==5);}
    byte_store_be16(memory+page-64+4,1001);CHECK(sfnt_font_attributes(face,&attr)==SfntStatus::malformed&&!attr.weight);
    byte_store_be16(memory+page-64+4,400);byte_store_be16(memory+page-64+6,0);CHECK(sfnt_font_attributes(face,&attr)==SfntStatus::malformed);
    return true;
}
bool argument(const wchar_t** command,wchar_t* output,u32 available) noexcept {
    const wchar_t* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 count=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){if(count+1>=available)return false;output[count++]=*p++;}
    if(quoted){if(*p!='"')return false;++p;}output[count]=0;*command=p;return count!=0;
}
bool audit(const wchar_t* path,u32 index) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    HANDLE mapping=nullptr;const u8* data=nullptr;struct Cleanup{HANDLE& f;HANDLE& m;const u8*& p;~Cleanup() noexcept{if(p)UnmapViewOfFile(p);if(m)CloseHandle(m);CloseHandle(f);}} cleanup{file,mapping,data};
    LARGE_INTEGER size{};CHECK(GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<=0xffffffffll);
    mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);CHECK(mapping);data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CHECK(data);
    SfntFace face{};CHECK(sfnt_open({data,static_cast<u32>(size.QuadPart)},index,&face)==SfntStatus::ok);
    ConstSlice<u8> table{};CHECK(sfnt_table(face,sfnt_tag('n','a','m','e'),&table)==SfntStatus::ok);SfntNames names{};CHECK(sfnt_names_open(table,&names)==SfntStatus::ok);
    SfntFontAttributes attributes{};CHECK(sfnt_font_attributes(face,&attributes)==SfntStatus::ok);
    print("{\"face_index\":");number(index);print(",\"face_count\":");number(face.face_count);print(",\"attributes\":[");
    const u16 attrs[]{attributes.weight,attributes.width,attributes.selection,attributes.version};for(u32 i=0;i<4;++i){if(i)ch(',');number(attrs[i]);}print("],\"records\":[");
    u32 count=0;
    for(u32 i=0;i<names.records;++i){SfntName name{};CHECK(sfnt_name_record(names,i,&name)==SfntStatus::ok);u32 units=0;const auto status=sfnt_name_decode(name,{decoded,32768},&units);if(status==SfntStatus::unsupported)continue;CHECK(status==SfntStatus::ok);
        if(count++)ch(',');ch('[');number(name.platform);ch(',');number(name.encoding);ch(',');number(name.language);ch(',');number(name.id);ch(',');string({decoded,units});ch(']');}
    print("],\"english\":[");const u16 ids[]{1,2,4,6,16,17,21,22};
    for(u32 i=0;i<8;++i){SfntName name{};bool found=false;CHECK(sfnt_name_english(names,ids[i],&name,&found)==SfntStatus::ok);if(i)ch(',');ch('[');number(ids[i]);ch(',');
        if(!found)print("null");else{u32 units=0;CHECK(sfnt_name_decode(name,{decoded,32768},&units)==SfntStatus::ok);string({decoded,units});}ch(']');}
    print("]}\n");return true;
}
bool run() noexcept {
    const wchar_t* command=GetCommandLineW();wchar_t path[32768];CHECK(argument(&command,path,32768));
    while(*command==' '||*command=='\t')++command;if(!*command)return basic()&&tags()&&guards()&&selection_permutations();
    CHECK(argument(&command,path,32768));while(*command==' '||*command=='\t')++command;u32 index=0;while(*command>='0'&&*command<='9'){CHECK(index<65536);index=index*10+*command++-'0';}
    CHECK(!*command);return audit(path,index);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_names_entry() noexcept {
    using namespace maicord;const bool ok=run();print("checks=");number(checks);print(ok?"\nPASS native font names\n":"\nFAIL native font names\n");flush();ExitProcess(ok&&print_ok?0:1);
}
