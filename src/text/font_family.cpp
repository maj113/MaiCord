#include "maicord/text/font_family.hpp"
#include "maicord/text/font_names.hpp"
#include <intrin.h>
// Font-family discovery is outside warm drawing; keep its parser compact.
#pragma optimize("s",on)
namespace maicord {namespace {
bool separator(u16 c) noexcept {return c==32||c==9||c==10||c==13||c=='.'||c=='-'||c=='_';}
u16 lower(u16 c) noexcept {return c>='A'&&c<='Z'?static_cast<u16>(c+32):c;}
bool spend(u32& work,u32 n=1) noexcept {if(n>work)return false;work-=n;return true;}
// Compile pointer-free pattern records; matching priority and spelling stay
// in source order. Source pointers exist only during constant evaluation.
struct Pattern {u16 offset,value;};
static_assert(sizeof(Pattern)==4);
struct PatternSource {const char* text;u16 value;};
constexpr PatternSource pattern_source[]{
    // regular
    {"book",1},{"normal",1},{"regular",1},{"roman",1},{"upright",1},
    // styles
    {"ita",2},{"ital",2},{"italic",2},{"cursive",2},{"kursiv",2},{"inclined",1},{"oblique",1},{"backslanted",1},{"backslant",1},{"slanted",1},
    // widths
    {"extra compressed",1},{"ext compressed",1},{"ultra compressed",1},{"ultra condensed",1},{"ultra cond",1},{"compressed",2},{"extra condensed",2},{"ext condensed",2},{"extra cond",2},{"ext cond",2},{"narrow",4},{"compact",4},{"semi condensed",4},{"semi cond",4},{"wide",6},{"semi expanded",6},{"semi extended",6},{"extra expanded",8},{"ext expanded",8},{"extra extended",8},{"ext extended",8},{"ultra expanded",9},{"ultra extended",9},{"condensed",3},{"cond",3},{"expanded",7},{"extended",7},
    // weights
    {"extra thin",100},{"ext thin",100},{"ultra thin",100},{"extra light",200},{"ext light",200},{"ultra light",200},{"semi bold",600},{"demi bold",600},{"extra bold",800},{"ext bold",800},{"ultra bold",800},{"extra black",950},{"ext black",950},{"ultra black",950},{"bold",700},{"thin",100},{"light",300},{"medium",500},{"black",900},{"heavy",900},{"nord",900},{"demi",600},{"ultra",800},
};
constexpr u32 pattern_count=sizeof(pattern_source)/sizeof(pattern_source[0]);
constexpr u32 pattern_bytes() noexcept {u32 bytes=0;for(const auto& pattern:pattern_source){u32 n=0;while(pattern.text[n])++n;bytes+=n+1;}return bytes;}
static_assert(pattern_bytes()<65536);
struct PatternTable {Pattern records[pattern_count];char text[pattern_bytes()];};
constexpr PatternTable pack_patterns() noexcept {
    PatternTable result{};u32 offset=0;for(u32 i=0;i<pattern_count;++i){result.records[i]={static_cast<u16>(offset),pattern_source[i].value};
        const char* p=pattern_source[i].text;do{result.text[offset++]=*p;}while(*p++);}return result;
}
constexpr auto pattern_table=pack_patterns();
constexpr ConstSlice<Pattern> regular{pattern_table.records+0,5};
constexpr ConstSlice<Pattern> styles{pattern_table.records+5,10};
constexpr ConstSlice<Pattern> widths{pattern_table.records+15,27};
constexpr ConstSlice<Pattern> weights{pattern_table.records+42,23};
bool match(ConstSlice<u16> text,u32 at,const char* pattern,u32* end,u32& work) noexcept {
    for(u32 i=0;pattern[i];++i){if(!spend(work))return false;
        if(pattern[i]==' '){while(at<text.count&&separator(text[at])){if(!spend(work))return false;++at;}}
        else{if(at==text.count||lower(text[at])!=static_cast<u8>(pattern[i]))return false;++at;}}
    *end=at;return true;
}
void erase(Slice<u16> text,u32& count,u32 start,u32 end) noexcept {
    while(start&&separator(text[start-1]))--start;while(end<count&&separator(text[end]))++end;
    const bool space=start&&end<count;const u32 tail=count-end;
    copy_bytes(text.data+start+(space?1:0),text.data+end,tail*2);if(space)text[start]=32;count=start+(space?1:0)+tail;
}
SfntStatus extract(Slice<u16> text,u32& count,ConstSlice<Pattern> patterns,bool first,bool face_suffix,u16* value,u32& work) noexcept {
    *value=0;
    // Short family names reuse their word starts across every priority pattern.
    // Charge skipped positions the same way the reverse scan does, so
    // both the chosen match and the caller's resource accounting stay stable.
    const bool indexed=count<=64;u64 starts=first&&count?1:0;
    if(indexed)for(u32 i=1;i<count;++i)if(separator(text[i-1]))starts|=u64{1}<<i;
    u32 initials=0,miss_work=count;
    for(u64 bits=starts;bits;){unsigned long at=0;_BitScanReverse64(&at,bits);bits^=u64{1}<<at;++miss_work;
        const u32 letter=static_cast<u32>(lower(text[at]))-'a';if(letter<26)initials|=1u<<letter;}
    for(u32 p=0;p<patterns.count;++p){
        // A missing initial proves every word-start match fails on its first
        // character. Charge those comparisons and all skipped positions once.
        const char* pattern=pattern_table.text+patterns[p].offset;
        if(indexed&&!(initials&(1u<<(pattern[0]-'a')))){
            if(!spend(work,miss_work)){work=0;return SfntStatus::resource_limit;}
            if(first&&count&&!work)return SfntStatus::resource_limit;
            continue;
        }
        u64 remaining=starts;for(u32 at=count;at;){
        if(indexed){if(!remaining){if(!spend(work,at)){work=0;return SfntStatus::resource_limit;}break;}
            unsigned long next=0;_BitScanReverse64(&next,remaining);remaining^=u64{1}<<next;
            if(!spend(work,at-next)){work=0;return SfntStatus::resource_limit;}at=next;
        }else{--at;if(!spend(work))return SfntStatus::resource_limit;
            if((!at&&!first)||(at&&!separator(text[at-1])))continue;}
        u32 end=0;
        if(!match({text.data,count},at,pattern,&end,work)){if(!work)return SfntStatus::resource_limit;continue;}
        if(face_suffix){u32 next=end;while(next<count&&separator(text[next])){if(!spend(work))return SfntStatus::resource_limit;++next;}u32 suffix=0;
            if(match({text.data,count},next,"face",&suffix,work)&&(suffix==count||separator(text[suffix])))end=suffix;if(!work)return SfntStatus::resource_limit;}
        if(end<count&&!separator(text[end]))continue;erase(text,count,at,end);*value=patterns[p].value;return SfntStatus::ok;
    }}return SfntStatus::ok;
}
void trim(Slice<u16> text,u32& count) noexcept {u32 start=0;while(start<count&&text[start]==32)++start;while(count>start&&text[count-1]==32)--count;copy_bytes(text.data,text.data+start,(count-start)*2);count-=start;}
bool contained(ConstSlice<u16> family,ConstSlice<u16> face,u32& work) noexcept {
    if(face.count>family.count)return false;
    for(u32 i=0;i<=family.count-face.count;++i){if(!spend(work))return false;if((i&&!separator(family[i-1]))||(i+face.count<family.count&&!separator(family[i+face.count])))continue;
        bool equal=true;for(u32 j=0;j<face.count;++j){if(!spend(work))return false;if(lower(family[i+j])!=lower(face[j])){equal=false;break;}}if(equal)return true;}
    return false;
}
u16 resolved_weight(u16 raw,u16 extracted) noexcept {
    if(!extracted||(raw<400&&extracted<400)||(raw>500&&extracted>500&&raw!=700)||
       ((raw==400||raw==500)&&(extracted==400||extracted==500)))return raw;
    const i32 difference=static_cast<i32>(raw)-extracted;
    if(difference>=-150&&difference<=150&&raw!=400&&raw!=500&&raw!=700)return raw;return extracted;
}
}
SfntStatus native_font_family(const SfntFace& face,Slice<u16> output,NativeFontFamily* result,u32 work,u32* used) noexcept {
    const u32 initial=work;struct Report{u32 initial;u32& left;u32* used;~Report() noexcept{if(used)*used=initial-left;}} report{initial,work,used};
    if(!result)return SfntStatus::invalid_input;*result={};if(output.count&&!output.data)return SfntStatus::invalid_input;
    SfntFontAttributes attributes{};auto status=sfnt_font_attributes(face,&attributes);if(status!=SfntStatus::ok)return status;
    ConstSlice<u8> table{};status=sfnt_table(face,sfnt_tag('n','a','m','e'),&table);if(status!=SfntStatus::ok)return status;
    SfntNames names{};status=sfnt_names_open(table,&names);if(status!=SfntStatus::ok)return status;
    if(!spend(work,names.records*6u+names.language_tags+1))return SfntStatus::resource_limit;
    SfntName family{},subfamily{};bool found=false;u16 chosen=0;
    const u16 ids[]{21,16,1};for(u16 id:ids){status=sfnt_name_english(names,id,&family,&found);if(status!=SfntStatus::ok)return status;if(found){chosen=id;break;}}
    if(!chosen||!family.bytes.count)return SfntStatus::unsupported;
    const u16 sub_ids[]{22,17,2};for(u16 id:sub_ids){if(id==22&&chosen!=21)continue;status=sfnt_name_english(names,id,&subfamily,&found);if(status!=SfntStatus::ok)return status;if(found)break;}
    const bool english=family.language==0x409||(family.platform==3&&(family.language&0x3ff)==9);
    const bool repair=chosen!=21&&!(attributes.selection&256)&&english;
    const u32 family_units=family.bytes.count/2,sub_units=repair&&found?subfamily.bytes.count/2:0;
    if(!spend(work,family_units+sub_units))return SfntStatus::resource_limit;
    if(family_units+sub_units+(sub_units?1u:0u)>output.count)return SfntStatus::output_too_small;
    u32 count=0;status=sfnt_name_decode(family,output,&count);if(status!=SfntStatus::ok)return status;
    NativeFontProperties properties{attributes.weight,static_cast<u8>(attributes.width),static_cast<u8>(attributes.selection&512?1:attributes.selection&1?2:0)};
    if(repair){trim(output,count);const u32 family_count=count;u32 sub_count=0;
        if(sub_units){const Slice<u16> sub{output.data+family_count+1,output.count-family_count-1};status=sfnt_name_decode(subfamily,sub,&sub_count);if(status!=SfntStatus::ok)return status;trim(sub,sub_count);u16 removed=0;
            status=extract(sub,sub_count,regular,true,false,&removed,work);if(status!=SfntStatus::ok)return status;
            const bool exists=contained({output.data,family_count},{sub.data,sub_count},work);if(!work)return SfntStatus::resource_limit;
            if(sub_count&&!exists){output[family_count]=32;count=family_count+1+sub_count;}}
        u16 style=0,width=0,weight=0;
        status=extract(output,count,styles,false,false,&style,work);if(status!=SfntStatus::ok)return status;
        status=extract(output,count,widths,false,false,&width,work);if(status!=SfntStatus::ok)return status;
        status=extract(output,count,weights,false,true,&weight,work);if(status!=SfntStatus::ok)return status;
        properties.weight=resolved_weight(properties.weight,weight);if(style)properties.style=static_cast<u8>(style);
        if(width&&!((properties.width<5&&width<5)||(properties.width>5&&width>5)))properties.width=static_cast<u8>(width);
    }
    if(!count)return SfntStatus::unsupported;*result={properties,count};return SfntStatus::ok;
}
SfntStatus native_font_family_candidates(const SfntFace& face,Slice<u16> name,NativeFontFamily* family,NativeFontCandidates* candidates,u32 work) noexcept {
    if(family)*family={};if(candidates)*candidates={};if(!family||!candidates)return SfntStatus::invalid_input;
    NativeFontFamily resolved{};u32 used=0;auto status=native_font_family(face,name,&resolved,work,&used);if(status!=SfntStatus::ok)return status;
    NativeFontCandidates source{};status=native_font_candidates_open(face,&source,work-used);if(status!=SfntStatus::ok)return status;
    source.base=resolved.properties;*family=resolved;*candidates=source;return SfntStatus::ok;
}
}
