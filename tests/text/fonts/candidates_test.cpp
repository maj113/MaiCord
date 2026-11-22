#include "maicord/text/fonts/candidates.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "../../support/test_output.hpp"
#if defined(MAICORD_FONT_FAMILY)
#include "maicord/text/fonts/family.hpp"
#endif
namespace maicord {namespace {
#if defined(MAICORD_FONT_FAMILY)
#include "../../generated/font_family_fixture.inc"
#endif
u32 checks=0;char buffer[65536];u32 used=0;bool print_ok=true;
void flush() noexcept {DWORD n=0;if(used&&(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),buffer,used,&n,nullptr)||n!=used))print_ok=false;used=0;}
void ch(char c) noexcept {if(used==sizeof(buffer))flush();buffer[used++]=c;}
void print(const char* s) noexcept {while(*s)ch(*s++);}
void number(u32 v) noexcept {char s[16];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n)ch(s[--n]);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
void w16(u8* p,u16 v) noexcept {p[0]=static_cast<u8>(v>>8);p[1]=static_cast<u8>(v);}
constexpr u32 os2_at=76,stat_at=140,fvar_at=176,name_at=268,font_size=288;
void font(u8* p) noexcept {
    clear_bytes(p,font_size);byte_store_be32(p,0x10000);w16(p+4,4);
    const u32 tags[]{sfnt_tag('O','S','/','2'),sfnt_tag('S','T','A','T'),sfnt_tag('f','v','a','r'),sfnt_tag('n','a','m','e')};
    const u32 offsets[]{os2_at,stat_at,fvar_at,name_at},sizes[]{64,36,92,20};
    for(u32 i=0;i<4;++i){byte_store_be32(p+12+i*16,tags[i]);byte_store_be32(p+20+i*16,offsets[i]);byte_store_be32(p+24+i*16,sizes[i]);}
    w16(p+os2_at,4);w16(p+os2_at+4,400);w16(p+os2_at+6,5);
    w16(p+stat_at,1);w16(p+stat_at+2,1);w16(p+stat_at+4,8);w16(p+stat_at+6,2);byte_store_be32(p+stat_at+8,20);
    w16(p+fvar_at,1);w16(p+fvar_at+4,16);w16(p+fvar_at+6,2);w16(p+fvar_at+8,2);w16(p+fvar_at+10,20);w16(p+fvar_at+12,3);w16(p+fvar_at+14,12);
    const u32 axis_tags[]{sfnt_tag('w','g','h','t'),sfnt_tag('w','d','t','h')};const u32 lows[]{300,75},normals[]{400,100},highs[]{700,100};
    for(u32 i=0;i<2;++i){auto* a=p+fvar_at+16+i*20;byte_store_be32(a,axis_tags[i]);byte_store_be32(a+4,lows[i]*65536);byte_store_be32(a+8,normals[i]*65536);byte_store_be32(a+12,highs[i]*65536);w16(a+18,static_cast<u16>(256+i));byte_store_be32(p+stat_at+20+i*8,axis_tags[i]);w16(p+stat_at+24+i*8,static_cast<u16>(256+i));}
    const u32 weights[]{400,600,300},widths[]{100,100,75};for(u32 i=0;i<3;++i){auto* a=p+fvar_at+56+i*12;w16(a,2);byte_store_be32(a+4,weights[i]*65536);byte_store_be32(a+8,widths[i]*65536);}
    w16(p+name_at+2,1);w16(p+name_at+4,18);w16(p+name_at+6,3);w16(p+name_at+8,1);w16(p+name_at+10,0x409);w16(p+name_at+12,2);w16(p+name_at+14,2);w16(p+name_at+18,'R');
}
bool basic() noexcept {
    u8 bytes[font_size];font(bytes);SfntFace face{};CHECK(sfnt_open({bytes,font_size},0,&face)==SfntStatus::ok);NativeFontCandidates source{};
    CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok&&source.count==3&&source.axes.axis_count==2);
    // STAT names more axes than fvar varies (Noto Sans and Ubuntu Sans name ital): a copy of STAT at the end with a third
    // record. Its order is free, and every fvar axis still needs its STAT record.
    {u8 wide[font_size+44];copy_bytes(wide,bytes,font_size);copy_bytes(wide+font_size,bytes+stat_at,36);clear_bytes(wide+font_size+36,8);
        byte_store_be32(wide+36,font_size);byte_store_be32(wide+40,44);w16(wide+font_size+6,3);
        byte_store_be32(wide+font_size+36,sfnt_tag('i','t','a','l'));w16(wide+font_size+40,258);SfntFace extra{};NativeFontCandidates more{};
        CHECK(sfnt_open({wide,sizeof(wide)},0,&extra)==SfntStatus::ok);
        CHECK(native_font_candidates_open(extra,&more)==SfntStatus::ok&&more.count==3&&more.axes.axis_count==2);
        u8 record[8];copy_bytes(record,wide+font_size+20,8);copy_bytes(wide+font_size+20,wide+font_size+36,8);copy_bytes(wide+font_size+36,record,8);
        CHECK(native_font_candidates_open(extra,&more)==SfntStatus::ok&&more.count==3);
        byte_store_be32(wide+font_size+36,sfnt_tag('s','l','n','t'));CHECK(native_font_candidates_open(extra,&more)==SfntStatus::unsupported&&!more.count);
        w16(wide+font_size+6,1);CHECK(native_font_candidates_open(extra,&more)==SfntStatus::unsupported&&!more.count);}
    NativeFontCandidate value{};FontAxisSetting settings[2]{};u32 count=99;
    CHECK(native_font_candidate(source,1,&value)==SfntStatus::ok&&value.properties.weight==600&&value.properties.width==5);
    CHECK(native_font_candidate_settings(source,1,{settings,2},&count)==SfntStatus::ok&&count==2&&settings[0].value==600&&settings[1].value==100);
    settings[0].value=123;CHECK(native_font_candidate_settings(source,1,{settings,1},&count)==SfntStatus::output_too_small&&settings[0].value==123);
    CHECK(native_font_candidates_open(face,&source,0)==SfntStatus::resource_limit&&!source.count);
    CHECK(native_font_candidates_open(face,&source,10)==SfntStatus::resource_limit&&!source.count);
    byte_store_be32(bytes+fvar_at+60,1000*65536);CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok&&source.count==4);
    CHECK(native_font_candidate(source,0,&value)==SfntStatus::ok&&value.properties.weight==700);
    CHECK(native_font_candidate_settings(source,0,{settings,2},&count)==SfntStatus::ok&&settings[0].value==700);
    CHECK(native_font_candidate(source,3,&value)==SfntStatus::ok&&value.properties.weight==400);
    CHECK(native_font_candidate_settings(source,3,{settings,2},&count)==SfntStatus::ok&&count==2&&settings[0].value==400&&settings[1].value==100);
    w16(bytes+fvar_at+12,0);CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok&&source.count==1&&source.axes.instance_count==0);
    CHECK(native_font_candidate(source,0,&value)==SfntStatus::ok&&value.properties.weight==400);
    font(bytes);byte_store_be32(bytes+fvar_at+72,600*65536+32768);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
    font(bytes);byte_store_be32(bytes+fvar_at+76,80*65536);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
#if defined(_WIN32)
    // Windows refuses an axis other than wght and wdth, and a STAT record with another name ID than fvar.
    font(bytes);byte_store_be32(bytes+fvar_at+16,sfnt_tag('o','p','s','z'));byte_store_be32(bytes+stat_at+20,sfnt_tag('o','p','s','z'));CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
    font(bytes);w16(bytes+stat_at+24,999);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
#else
    // An axis other than wght and wdth leaves the properties to OS/2; the candidate keeps the instance's coordinate.
    font(bytes);byte_store_be32(bytes+fvar_at+16,sfnt_tag('o','p','s','z'));byte_store_be32(bytes+stat_at+20,sfnt_tag('o','p','s','z'));
    CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok&&source.count==3);
    CHECK(native_font_candidate(source,1,&value)==SfntStatus::ok&&value.properties.weight==400&&value.properties.width==5);
    CHECK(native_font_candidate_settings(source,1,{settings,2},&count)==SfntStatus::ok&&count==2&&settings[0].value==600);
    // As in Adwaita Sans: wght and opsz axes, and STAT names opsz with its own name ID. A STAT record matches by tag.
    font(bytes);byte_store_be32(bytes+fvar_at+36,sfnt_tag('o','p','s','z'));byte_store_be32(bytes+stat_at+28,sfnt_tag('o','p','s','z'));w16(bytes+stat_at+32,298);
    CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok&&source.count==3);
    CHECK(native_font_candidate(source,1,&value)==SfntStatus::ok&&value.properties.weight==600&&value.properties.width==5);
    CHECK(native_font_candidate(source,2,&value)==SfntStatus::ok&&value.properties.weight==300&&value.properties.width==5);
    CHECK(native_font_candidate_settings(source,2,{settings,2},&count)==SfntStatus::ok&&count==2&&settings[0].value==300&&settings[1].value==75);
    font(bytes);w16(bytes+stat_at+24,999);CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok&&source.count==3);
#endif
    font(bytes);byte_store_be32(bytes+28,sfnt_tag('S','U','A','T'));CHECK(sfnt_open({bytes,font_size},0,&face)==SfntStatus::ok);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
    font(bytes);CHECK(sfnt_open({bytes,font_size},0,&face)==SfntStatus::ok);w16(bytes+fvar_at+56,999);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
    font(bytes);w16(bytes+os2_at+4,0);CHECK(native_font_candidates_open(face,&source)==SfntStatus::malformed&&!source.count);
    for(u32 length=0;length<=36;++length){font(bytes);byte_store_be32(bytes+40,length);CHECK(sfnt_open({bytes,font_size},0,&face)==SfntStatus::ok);
        CHECK(native_font_candidates_open(face,&source)==(length<36?SfntStatus::truncated:SfntStatus::ok));if(length<36)CHECK(!source.count);}
    font(bytes);CHECK(sfnt_open({bytes,font_size},0,&face)==SfntStatus::ok);w16(bytes+stat_at+2,3);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
    font(bytes);byte_store_be32(bytes+stat_at+8,0);CHECK(native_font_candidates_open(face,&source)==SfntStatus::malformed&&!source.count);
    font(bytes);w16(bytes+stat_at+4,7);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
    font(bytes);w16(bytes+4,1);w16(bytes+os2_at+62,512);CHECK(sfnt_open({bytes,font_size},0,&face)==SfntStatus::ok);
    CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok&&source.count==1&&!source.axes.axis_count);
    CHECK(native_font_candidate(source,0,&value)==SfntStatus::ok&&value.properties.style==1);
    CHECK(native_font_candidate_settings(source,0,{},&count)==SfntStatus::ok&&!count);
    return true;
}
bool guards() noexcept {
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(pages_allocate(page*4));CHECK(memory);
    struct Cleanup{u8* p;~Cleanup() noexcept{pages_release(p);}} cleanup{memory};CHECK(pages_protect(memory+page, page, PageAccess::none)&&pages_protect(memory+page*3, page, PageAccess::none));
    u8 bytes[font_size];font(bytes);for(u32 size=0;size<=font_size;++size){auto* end=memory+page-size;copy_bytes(end,bytes,size);SfntFace face{};
        const auto status=sfnt_open({end,size},0,&face);CHECK(status==(size<font_size?SfntStatus::truncated:SfntStatus::ok));}
    auto* end=memory+page-font_size;copy_bytes(end,bytes,font_size);CHECK(pages_protect(memory, page, PageAccess::read));
    SfntFace face{};CHECK(sfnt_open({end,font_size},0,&face)==SfntStatus::ok);NativeFontCandidates source{};CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok);
    auto* settings=reinterpret_cast<FontAxisSetting*>(memory+page*3-2*sizeof(FontAxisSetting));u32 count=0;
    CHECK(native_font_candidate_settings(source,2,{settings,2},&count)==SfntStatus::ok&&count==2&&settings[0].value==300&&settings[1].value==75);
    return true;
}
#if defined(MAICORD_FONT_FAMILY)
struct Reader {const u8* data;u32 size,at;
    bool word(u32* value) noexcept {if(at>size||size-at<4)return false;*value=byte_load_le32(data+at);at+=4;return true;}
    bool take(void* output,u32 bytes) noexcept {if(at>size||bytes>size-at)return false;copy_bytes(output,data+at,bytes);at+=bytes;return true;}
};
bool family_word_boundaries() noexcept {
    struct Case {const char* suffix;NativeFontProperties properties;const char* retained;};
    const Case cases[]{{" Extra Bold",{800,5,0},""},{"_semi-condensed",{400,4,0},""},{" italic",{400,5,2},""},
        {" Bold face",{700,5,0},""},{" Bolder",{400,5,0}," Bolder"},{" Bold Extra Light",{200,5,0}," Bold"},{" Bold Bold",{700,5,0}," Bold"}};
    const u32 lengths[]{63,64,65,80};
    for(u32 length:lengths)for(const auto& item:cases){u32 suffix=0,retained=0;while(item.suffix[suffix])++suffix;while(item.retained[retained])++retained;
        const u32 prefix=length-suffix;u8 data[512]{};constexpr u32 os2=44,name=108;const u32 name_bytes=30+2*(length+7),bytes=name+name_bytes;
        byte_store_be32(data,0x10000);w16(data+4,2);byte_store_be32(data+12,sfnt_tag('O','S','/','2'));byte_store_be32(data+20,os2);byte_store_be32(data+24,64);
        byte_store_be32(data+28,sfnt_tag('n','a','m','e'));byte_store_be32(data+36,name);byte_store_be32(data+40,name_bytes);
        w16(data+os2,4);w16(data+os2+4,400);w16(data+os2+6,5);w16(data+name+2,2);w16(data+name+4,30);
        for(u32 i=0;i<2;++i){auto* record=data+name+6+i*12;w16(record,3);w16(record+2,1);w16(record+4,0x409);w16(record+6,static_cast<u16>(i+1));w16(record+8,static_cast<u16>((i?7:length)*2));w16(record+10,static_cast<u16>(i?length*2:0));}
        for(u32 i=0;i<length;++i)w16(data+name+30+i*2,static_cast<u16>(i<prefix?'X':item.suffix[i-prefix]));
        constexpr char regular[]="Regular";for(u32 i=0;i<7;++i)w16(data+name+30+(length+i)*2,regular[i]);
        SfntFace face{};CHECK(sfnt_open({data,bytes},0,&face)==SfntStatus::ok);u16 output[96]{};NativeFontFamily family{};
        CHECK(native_font_family(face,{output,96},&family)==SfntStatus::ok);
        CHECK(family.units==prefix+retained&&bytes_equal(&family.properties,&item.properties,4));
        for(u32 i=0;i<family.units;++i)CHECK(output[i]==static_cast<u16>(i<prefix?'X':item.retained[i-prefix]));
    }return true;
}
bool family_fixture() noexcept {
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(pages_allocate(page*2));CHECK(memory);
    struct Cleanup{u8* p;~Cleanup() noexcept{pages_release(p);}} cleanup{memory};CHECK(pages_protect(memory+page, page, PageAccess::none));
    auto* guarded=reinterpret_cast<u16*>(memory+page-512);
    Reader reader{font_family_fixture,sizeof(font_family_fixture),8};u32 count=0;CHECK(bytes_equal(reader.data,"MCFFAM1\0",8)&&reader.word(&count));
    for(u32 i=0;i<count;++i){u32 bytes=0,units=0;NativeFontProperties expected{};CHECK(reader.word(&bytes)&&reader.word(&units)&&reader.take(&expected,4)&&bytes<=reader.size-reader.at);
        SfntFace face{};CHECK(sfnt_open({reader.data+reader.at,bytes},0,&face)==SfntStatus::ok);reader.at+=bytes;u16 name[2048],expected_name[2048];CHECK(units<=2048&&reader.take(expected_name,units*2));
        NativeFontFamily family{};NativeFontCandidates candidates{};CHECK(native_font_family_candidates(face,{name,2048},&family,&candidates)==SfntStatus::ok);
        if(family.units!=units||!bytes_equal(name,expected_name,units*2)||!bytes_equal(&family.properties,&expected,4)){print("family_fixture=");number(i);print("\n");}
        CHECK(family.units==units&&bytes_equal(name,expected_name,units*2)&&bytes_equal(&family.properties,&expected,4));
        NativeFontCandidate candidate{};CHECK(native_font_candidate(candidates,0,&candidate)==SfntStatus::ok&&bytes_equal(&candidate.properties,&expected,4));
        CHECK(native_font_family(face,{},&family)==SfntStatus::output_too_small&&!family.units);
        CHECK(native_font_family(face,{name,units?units-1:0},&family)==SfntStatus::output_too_small&&!family.units);
        CHECK(native_font_family_candidates(face,{guarded,256},&family,&candidates)==SfntStatus::ok&&family.units==units&&bytes_equal(guarded,expected_name,units*2));
    }CHECK(reader.at==reader.size);return true;
}
#endif
bool run() noexcept {
#if defined(MAICORD_FONT_FAMILY)
    CHECK(family_word_boundaries()&&family_fixture());
#endif
    return basic()&&guards();}
}}
MAICORD_TEST_ENTRY(maicord_font_candidates_entry) {
    using namespace maicord;const bool success=run();print("checks=");number(checks);print(success?"\nPASS native font candidates\n":"\nFAIL native font candidates\n");flush();::maicord::test_process_exit(success&&print_ok?0:1);
}
