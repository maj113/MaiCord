#include "maicord/text/font_candidates.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
#if defined(MAICORD_FONT_FAMILY)
#include "maicord/text/font_family.hpp"
#endif
#if defined(MAICORD_FONT_FAMILY)
#include "font_family_reference.inc"
#endif
namespace maicord {namespace {
#if defined(MAICORD_FONT_FAMILY)
#include "generated/font_family_fixture.inc"
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
    NativeFontCandidate value{};FontAxisSetting settings[2]{};u32 count=99;
    CHECK(native_font_candidate(source,1,&value)==SfntStatus::ok&&value.named_index==1&&value.properties.weight==600&&value.properties.width==5);
    CHECK(native_font_candidate_settings(source,1,{settings,2},&count)==SfntStatus::ok&&count==2&&settings[0].value==600&&settings[1].value==100);
    settings[0].value=123;CHECK(native_font_candidate_settings(source,1,{settings,1},&count)==SfntStatus::output_too_small&&!count&&settings[0].value==123);
    CHECK(native_font_candidate(source,3,&value)==SfntStatus::invalid_input&&!value.properties.weight);
    CHECK(native_font_candidate_settings(source,3,{settings,2},&count)==SfntStatus::invalid_input&&!count);
    CHECK(native_font_candidates_open(face,&source,0)==SfntStatus::resource_limit&&!source.count);
    CHECK(native_font_candidates_open(face,&source,10)==SfntStatus::resource_limit&&!source.count);
    byte_store_be32(bytes+fvar_at+60,1000*65536);CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok&&source.count==4);
    CHECK(native_font_candidate(source,0,&value)==SfntStatus::ok&&value.properties.weight==700);
    CHECK(native_font_candidate_settings(source,0,{settings,2},&count)==SfntStatus::ok&&settings[0].value==700);
    CHECK(native_font_candidate(source,3,&value)==SfntStatus::ok&&value.named_index==0xffffffff&&value.properties.weight==400);
    CHECK(native_font_candidate_settings(source,3,{settings,2},&count)==SfntStatus::ok&&count==2&&settings[0].value==400&&settings[1].value==100);
    w16(bytes+fvar_at+12,0);CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok&&source.count==1&&source.axes.instance_count==0);
    CHECK(native_font_candidate(source,0,&value)==SfntStatus::ok&&value.properties.weight==400&&value.named_index==0xffffffff);
    font(bytes);byte_store_be32(bytes+fvar_at+72,600*65536+32768);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
    font(bytes);byte_store_be32(bytes+fvar_at+76,80*65536);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
    font(bytes);byte_store_be32(bytes+fvar_at+16,sfnt_tag('o','p','s','z'));byte_store_be32(bytes+stat_at+20,sfnt_tag('o','p','s','z'));CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
    font(bytes);w16(bytes+stat_at+24,999);CHECK(native_font_candidates_open(face,&source)==SfntStatus::unsupported&&!source.count);
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
    CHECK(native_font_candidate(source,0,&value)==SfntStatus::ok&&value.named_index==0xffffffff&&value.properties.style==1);
    CHECK(native_font_candidate_settings(source,0,{},&count)==SfntStatus::ok&&!count);
    CHECK(native_font_candidate({},0,&value)==SfntStatus::invalid_input&&native_font_candidates_open(face,nullptr)==SfntStatus::invalid_input);
    return true;
}
bool guards() noexcept {
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup{u8* p;~Cleanup() noexcept{VirtualFree(p,0,MEM_RELEASE);}} cleanup{memory};DWORD old=0;
    CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)&&VirtualProtect(memory+page*3,page,PAGE_NOACCESS,&old));
    u8 bytes[font_size];font(bytes);for(u32 size=0;size<=font_size;++size){auto* end=memory+page-size;copy_bytes(end,bytes,size);SfntFace face{};
        const auto status=sfnt_open({end,size},0,&face);CHECK(status==(size<font_size?SfntStatus::truncated:SfntStatus::ok));}
    auto* end=memory+page-font_size;copy_bytes(end,bytes,font_size);CHECK(VirtualProtect(memory,page,PAGE_READONLY,&old));
    SfntFace face{};CHECK(sfnt_open({end,font_size},0,&face)==SfntStatus::ok);NativeFontCandidates source{};CHECK(native_font_candidates_open(face,&source)==SfntStatus::ok);
    auto* settings=reinterpret_cast<FontAxisSetting*>(memory+page*3-2*sizeof(FontAxisSetting));u32 count=0;
    CHECK(native_font_candidate_settings(source,2,{settings,2},&count)==SfntStatus::ok&&count==2&&settings[0].value==300&&settings[1].value==75);
    CHECK(native_font_candidate_settings(source,0,{nullptr,2},&count)==SfntStatus::invalid_input&&!count);
    return true;
}
bool argument(const wchar_t** command,wchar_t* out) noexcept {const wchar_t* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){if(n==32767)return false;out[n++]=*p++;}if(quoted){if(*p!='"')return false;++p;}out[n]=0;*command=p;return n!=0;}
bool audit(const wchar_t* path,u32 index) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);HANDLE mapping=nullptr;const u8* data=nullptr;
    struct Cleanup{HANDLE& f;HANDLE& m;const u8*& p;~Cleanup() noexcept{if(p)UnmapViewOfFile(p);if(m)CloseHandle(m);CloseHandle(f);}} cleanup{file,mapping,data};Win32LargeInteger size{};
    CHECK(GetFileSizeEx(file,size.native())&&size.QuadPart>0&&size.QuadPart<=0xffffffffll);mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);CHECK(mapping);data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CHECK(data);
    SfntFace face{};CHECK(sfnt_open({data,static_cast<u32>(size.QuadPart)},index,&face)==SfntStatus::ok);NativeFontCandidates source{};
#if defined(MAICORD_FONT_FAMILY)
    u16 name[2048];NativeFontFamily family{};const auto status=native_font_family_candidates(face,{name,2048},&family,&source);
    print("{\"status\":");number(static_cast<u32>(status));print(",\"family\":\"");const char hex[]="0123456789abcdef";
    for(u32 i=0;i<family.units;++i){print("\\u");for(int shift=12;shift>=0;shift-=4)ch(hex[(name[i]>>shift)&15]);}print("\",\"candidates\":[");
#else
    const auto status=native_font_candidates_open(face,&source);print("{\"status\":");number(static_cast<u32>(status));print(",\"candidates\":[");
#endif
    if(status==SfntStatus::ok)for(u32 i=0;i<source.count;++i){NativeFontCandidate candidate{};FontAxisSetting values[2];u32 count=0;CHECK(native_font_candidate(source,i,&candidate)==SfntStatus::ok&&native_font_candidate_settings(source,i,{values,2},&count)==SfntStatus::ok);
        if(i)ch(',');print("{\"named_index\":");number(candidate.named_index);print(",\"properties\":[");number(candidate.properties.weight);ch(',');number(candidate.properties.width);ch(',');number(candidate.properties.style);print("],\"settings\":[");
        for(u32 j=0;j<count;++j){if(j)ch(',');ch('[');number(values[j].tag);ch(',');const float f=static_cast<float>(values[j].value);u32 bits=0;copy_bytes(&bits,&f,4);number(bits);ch(']');}print("]} ");}
    print("]}\n");return true;
}
struct MappedFile {
    HANDLE file=nullptr,mapping=nullptr;const u8* data=nullptr;u32 size=0;
    ~MappedFile() noexcept {if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);if(file&&file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    bool open(const wchar_t* path) noexcept {
        file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
        Win32LargeInteger length{};CHECK(GetFileSizeEx(file,length.native())&&length.QuadPart>0&&length.QuadPart<=0xffffffffll);size=static_cast<u32>(length.QuadPart);
        mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);CHECK(mapping);data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CHECK(data);return true;
    }
};
struct Reader {const u8* data;u32 size,at;
    bool word(u32* value) noexcept {if(at>size||size-at<4)return false;*value=byte_load_le32(data+at);at+=4;return true;}
    bool take(void* output,u32 bytes) noexcept {if(at>size||bytes>size-at)return false;copy_bytes(output,data+at,bytes);at+=bytes;return true;}
};
#if defined(MAICORD_FONT_FAMILY)
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
        SfntFace face{};CHECK(sfnt_open({data,bytes},0,&face)==SfntStatus::ok);u16 output[96]{};NativeFontFamily family{};u32 spent=0;
        CHECK(native_font_family(face,{output,96},&family,1000000,&spent)==SfntStatus::ok&&spent>0);
        CHECK(family.units==prefix+retained&&bytes_equal(&family.properties,&item.properties,4));
        for(u32 i=0;i<family.units;++i)CHECK(output[i]==static_cast<u16>(i<prefix?'X':item.retained[i-prefix]));
        const u32 expected_work=spent;
        for(u32 budget=0;budget<=expected_work+1;++budget){
            u16 actual[96]{},expected[96]{};NativeFontFamily a{},b{};u32 aw=0,bw=0;
            const auto as=native_font_family(face,{actual,96},&a,budget,&aw);
            const auto bs=family_reference::reference_font_family(face,{expected,96},&b,budget,&bw);
            CHECK(as==bs&&aw==bw&&a.units==b.units&&bytes_equal(&a.properties,&b.properties,4)&&bytes_equal(actual,expected,sizeof(actual)));
        }
        CHECK(native_font_family(face,{output,96},&family,expected_work-1,&spent)==SfntStatus::resource_limit&&!family.units&&spent<=expected_work-1);
        CHECK(native_font_family(face,{output,96},&family,expected_work+1,&spent)==SfntStatus::ok&&family.units==prefix+retained&&spent==expected_work&&bytes_equal(&family.properties,&item.properties,4));
    }return true;
}
bool family_fixture() noexcept {
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup{u8* p;~Cleanup() noexcept{VirtualFree(p,0,MEM_RELEASE);}} cleanup{memory};DWORD old=0;CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&old));
    auto* guarded=reinterpret_cast<u16*>(memory+page-512);
    Reader reader{font_family_fixture,sizeof(font_family_fixture),8};u32 count=0;CHECK(bytes_equal(reader.data,"MCFFAM1\0",8)&&reader.word(&count));
    for(u32 i=0;i<count;++i){u32 bytes=0,units=0;NativeFontProperties expected{};CHECK(reader.word(&bytes)&&reader.word(&units)&&reader.take(&expected,4)&&bytes<=reader.size-reader.at);
        SfntFace face{};CHECK(sfnt_open({reader.data+reader.at,bytes},0,&face)==SfntStatus::ok);reader.at+=bytes;u16 name[2048],expected_name[2048];CHECK(units<=2048&&reader.take(expected_name,units*2));
        NativeFontFamily family{};NativeFontCandidates candidates{};CHECK(native_font_family_candidates(face,{name,2048},&family,&candidates)==SfntStatus::ok);
        if(family.units!=units||!bytes_equal(name,expected_name,units*2)||!bytes_equal(&family.properties,&expected,4)){print("family_fixture=");number(i);print("\n");}
        CHECK(family.units==units&&bytes_equal(name,expected_name,units*2)&&bytes_equal(&family.properties,&expected,4));
        NativeFontCandidate candidate{};CHECK(native_font_candidate(candidates,0,&candidate)==SfntStatus::ok&&bytes_equal(&candidate.properties,&expected,4));
        CHECK(native_font_family(face,{},&family)==SfntStatus::output_too_small&&!family.units);
        CHECK(native_font_family(face,{name,2048},&family,0)==SfntStatus::resource_limit&&!family.units);
        CHECK(native_font_family_candidates(face,{guarded,256},&family,&candidates)==SfntStatus::ok&&family.units==units&&bytes_equal(guarded,expected_name,units*2));
    }CHECK(reader.at==reader.size);return true;
}
#endif
bool family_audit(const wchar_t* path) noexcept {
    MappedFile input;CHECK(input.open(path));CHECK(input.size>=16&&bytes_equal(input.data,"MCFCAND1",8));Reader reader{input.data,input.size,8};u32 file_count=0,queries=0;
    CHECK(reader.word(&file_count)&&reader.word(&queries)&&file_count&&file_count<=64);
    MappedFile files[64];NativeFontCandidates sources[64];NativeFontProperties candidates[4096];u32 candidate_files[4096],candidate_indices[4096],count=0;
    for(u32 f=0;f<file_count;++f){u32 index=0,units=0;wchar_t file_path[32768];CHECK(reader.word(&index)&&reader.word(&units)&&units&&units<32768&&reader.take(file_path,units*2));file_path[units]=0;
        CHECK(files[f].open(file_path));SfntFace face{};CHECK(sfnt_open({files[f].data,files[f].size},index,&face)==SfntStatus::ok);
#if defined(MAICORD_FONT_FAMILY)
        u16 name[2048];NativeFontFamily family{};CHECK(native_font_family_candidates(face,{name,2048},&family,sources+f)==SfntStatus::ok);
#else
        CHECK(native_font_candidates_open(face,sources+f)==SfntStatus::ok);
#endif
        CHECK(sources[f].count<=4096-count);for(u32 i=0;i<sources[f].count;++i){NativeFontCandidate value{};CHECK(native_font_candidate(sources[f],i,&value)==SfntStatus::ok);candidates[count]=value.properties;candidate_files[count]=f;candidate_indices[count]=i;++count;}
    }
    for(u32 q=0;q<queries;++q){NativeFontProperties request{},expected_properties{};u32 expected_file=0,simulations=0,axes=0;
        CHECK(reader.take(&request,4)&&reader.word(&expected_file)&&reader.take(&expected_properties,4)&&reader.word(&simulations)&&reader.word(&axes));
        NativeFontMatch match{};CHECK(native_font_match({candidates,count},request,&match)==SfntStatus::ok);
        if(candidate_files[match.index]!=expected_file||match.simulations!=simulations||!bytes_equal(&match.properties,&expected_properties,4)){print("query=");number(q);print(" actual_file=");number(candidate_files[match.index]);print(" expected_file=");number(expected_file);print("\n");}
        CHECK(candidate_files[match.index]==expected_file&&match.simulations==simulations&&bytes_equal(&match.properties,&expected_properties,4));
        FontAxisSetting settings[2];u32 settings_count=0;CHECK(native_font_candidate_settings(sources[candidate_files[match.index]],candidate_indices[match.index],{settings,2},&settings_count)==SfntStatus::ok&&settings_count==axes);
        for(u32 i=0;i<axes;++i){u32 tag=0,expected_bits=0;CHECK(reader.word(&tag)&&reader.word(&expected_bits));const float value=static_cast<float>(settings[i].value);u32 actual_bits=0;copy_bytes(&actual_bits,&value,4);CHECK(settings[i].tag==tag&&actual_bits==expected_bits);}
    }
    CHECK(reader.at==reader.size);print("{\"files\":");number(file_count);print(",\"candidates\":");number(count);print(",\"queries\":");number(queries);print(",\"exact\":");number(queries);print("}\n");return true;
}
bool run() noexcept {const wchar_t* command=GetCommandLineW();wchar_t path[32768];CHECK(argument(&command,path));while(*command==' '||*command=='\t')++command;if(!*command){
#if defined(MAICORD_FONT_FAMILY)
    CHECK(family_word_boundaries()&&family_fixture());
#endif
    return basic()&&guards();}
    CHECK(argument(&command,path));if(bytes_equal(path,L"--family",18)){CHECK(argument(&command,path));return family_audit(path);}
    while(*command==' '||*command=='\t')++command;u32 index=0;while(*command>='0'&&*command<='9'){CHECK(index<65536);index=index*10+*command++-'0';}CHECK(!*command);return audit(path,index);}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_candidates_entry() noexcept {
    using namespace maicord;const bool success=run();print("checks=");number(checks);print(success?"\nPASS native font candidates\n":"\nFAIL native font candidates\n");flush();ExitProcess(success&&print_ok?0:1);
}
