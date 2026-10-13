#include "maicord/text/layout_directory.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>

namespace maicord {namespace {
#include "generated/layout_directory_fixture.inc"
volatile u32 guard_observations=0;
void observe(SfntStatus status) noexcept {guard_observations=guard_observations+static_cast<u32>(status)+1;}
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
bool selection_units() noexcept {
    u8 bytes[sizeof(layout_directory_fixture)];copy_bytes(bytes,layout_directory_fixture,sizeof(bytes));SfntLayout layout{};
    if(sfnt_layout_open({bytes,sizeof(bytes)},SfntLayoutKind::substitution,&layout)!=SfntStatus::ok)return false;
    constexpr u32 latin=sfnt_tag('l','a','t','n'),fallback=sfnt_tag('D','F','L','T'),turkish=sfnt_tag('T','R','K',' '),english=sfnt_tag('E','N','G',' ');
    const u32 candidates[]{sfnt_tag('a','r','a','b'),latin,fallback};SfntLanguageSelection result{};u64 work=0;
    const u32 languages[]{turkish,english,0};
    for(u32 requested:languages){
        if(sfnt_layout_select_language(layout,{candidates,3},requested,&result,1000000,&work)!=SfntStatus::ok||result.script_tag!=latin||result.language_tag!=(requested==turkish?turkish:0)||
           result.language.required_feature!=0xffff||result.language.feature_indices.count!=2||byte_load_be16(result.language.feature_indices.data)!=(requested==turkish?1:0))return false;
        const auto expected=result;const u64 full=work;
        for(u64 budget=0;budget<=full;++budget){const auto status=sfnt_layout_select_language(layout,{candidates,3},requested,&result,budget,&work);
            if(work>budget||status!=(budget<full?SfntStatus::resource_limit:SfntStatus::ok))return false;
            if(status==SfntStatus::ok&&(result.script_tag!=expected.script_tag||result.language_tag!=expected.language_tag||result.language.feature_indices.data!=expected.language.feature_indices.data))return false;}
    }
    const u32 default_first[]{fallback,latin};
    if(sfnt_layout_select_language(layout,{default_first,2},english,&result)!=SfntStatus::ok||result.script_tag!=fallback||result.language_tag||result.language.required_feature!=1||result.language.feature_indices.count!=4)return false;
    u16 script_index=0;u32 tag=0;SfntScript script{};
    if(sfnt_layout_find_script(layout,latin,&script_index)!=SfntStatus::ok||sfnt_layout_script(layout,script_index,&tag,&script)!=SfntStatus::ok)return false;
    const u32 at=static_cast<u32>(script.table.data-bytes);put(bytes,at,0);
    // An existing script with no applicable language stops the script search.
    if(sfnt_layout_select_language(layout,{candidates,3},english,&result)!=SfntStatus::missing_table||result.script_tag!=latin||result.language_tag||result.language.feature_indices.data)return false;
    if(sfnt_layout_select_language(layout,{candidates,3},turkish,&result)!=SfntStatus::ok||result.script_tag!=latin||result.language_tag!=turkish)return false;
    if(sfnt_layout_find_script(layout,fallback,&script_index)!=SfntStatus::ok||sfnt_layout_script(layout,script_index,&tag,&script)!=SfntStatus::ok)return false;
    const u32 fallback_at=static_cast<u32>(script.table.data-bytes);put(bytes,fallback_at,0);
    if(sfnt_layout_select_language(layout,{default_first,2},english,&result)!=SfntStatus::missing_table||result.script_tag!=fallback||result.language.feature_indices.data)return false;
    // The compatibility case does not relax nonzero offset validation.
    put(bytes,fallback_at,1);
    if(sfnt_layout_select_language(layout,{default_first,2},english,&result)!=SfntStatus::malformed)return false;
    put(bytes,fallback_at,0);
    if(sfnt_layout_select_language(layout,{},0,&result,0,&work)!=SfntStatus::missing_table||result.script_tag||work)return false;
    if(sfnt_layout_select_language({}, {},0,&result)!=SfntStatus::invalid_input||sfnt_layout_select_language(layout,{nullptr,1},0,&result)!=SfntStatus::invalid_input||
       sfnt_layout_select_language(layout,{},0,nullptr)!=SfntStatus::invalid_input)return false;
    return true;
}
bool units() noexcept {
    if(!selection_units())return false;
    constexpr u32 size=sizeof(layout_directory_fixture);u8 b[size];for(u32 i=0;i<size;++i)b[i]=layout_directory_fixture[i];
    SfntLayout l{};if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::ok || l.script_count!=2 || l.feature_count!=3 || l.lookup_count!=2)return false;
    u16 index=0;u32 tag=0;SfntScript script{};SfntLanguage lang{};
    if(sfnt_layout_find_script(l,sfnt_tag('l','a','t','n'),&index)!=SfntStatus::ok || index!=1 || sfnt_layout_find_script(l,sfnt_tag('a','r','a','b'),&index)!=SfntStatus::missing_table || index!=65535)return false;
    if(sfnt_layout_script(l,0,&tag,&script)!=SfntStatus::ok || sfnt_script_language(script,l.feature_count,sfnt_default_language,&tag,&lang)!=SfntStatus::ok || lang.required_feature!=1 || lang.feature_indices.count!=4 || byte_load_be16(lang.feature_indices.data)!=1 || byte_load_be16(lang.feature_indices.data+2)!=0)return false;
    SfntLookup lookup{};if(sfnt_layout_lookup(l,1,&lookup)!=SfntStatus::ok || lookup.source_type!=7 || lookup.type!=1 || lookup.flags!=0x210 || lookup.mark_set!=1 || lookup.subtable_count!=2)return false;
    ConstSlice<u8> sub{};if(sfnt_lookup_subtable(lookup,2,&sub)!=SfntStatus::invalid_input || sub.data)return false;
    // Exhaust all normalized F2DOT14 x values across three y regions.
    const i16 ys[]{-8193,0,8193};for(i16 y:ys)for(i32 x=-16384;x<=16384;++x){i16 coords[]{static_cast<i16>(x),y};SfntFeatureSubstitutions selected{};u32 record=0;
        const u32 expected=(x>=0 && x<=8192 && y==0)?2u:(x>=-8192 && x<=8192)?3u:4u;
        if(sfnt_layout_variation_select(l,{coords,2},&selected,&record)!=SfntStatus::ok || record!=expected)return false;
        for(u16 f=0;f<3;++f){SfntFeature feature{};if(sfnt_layout_feature(l,f,selected,&tag,&feature)!=SfntStatus::ok)return false;
            const u32 count=f==0?1u:f==1?(expected==3?0u:2u):(expected==3?1u:0u);
            if(feature.lookup_indices.count!=count*2)return false;
            if(count && byte_load_be16(feature.lookup_indices.data)!=(f==0?(expected==2?1u:0u):f==1?1u:0u))return false;
        }}
    i16 coords[]{0,0};SfntFeatureSubstitutions selected{};u32 record=0;
    if(sfnt_layout_variation_select(l,{coords,2},&selected,&record,2)!=SfntStatus::resource_limit || selected.table.data || record!=sfnt_no_variation_record)return false;
    coords[0]=16385;if(sfnt_layout_variation_select(l,{coords,2},&selected,&record)!=SfntStatus::invalid_input)return false;coords[0]=0;
    const u32 variations=l.variations,condition=variations+byte_load_be32(b+variations+8+2*8),format_at=condition+byte_load_be32(b+condition+2);
    put(b,format_at,99);if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::ok || sfnt_layout_variation_select(l,{coords,2},&selected,&record)!=SfntStatus::ok || record!=3)return false;
    put(b,format_at,1);byte_store_be32(b+variations+4,4);coords[0]=16384;
    if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::ok || sfnt_layout_variation_select(l,{coords,2},&selected,&record)!=SfntStatus::ok || record!=sfnt_no_variation_record || selected.table.data)return false;
    // A selected null substitution stops processing even with later universal records.
    for(u32 i=0;i<size;++i)b[i]=layout_directory_fixture[i];
    if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::ok || sfnt_layout_variation_select(l,{coords,2},&selected,&record)!=SfntStatus::ok || record!=4 || selected.table.data)return false;
    // Bad root offsets, script ordering, extension recursion and mixed types.
    put(b,4,0);if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::malformed || l.table.data)return false;
    for(u32 i=0;i<size;++i)b[i]=layout_directory_fixture[i];const u32 scripts=byte_load_be16(b+4);byte_store_be32(b+scripts+8,sfnt_tag('D','F','L','T'));
    if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::malformed)return false;
    for(u32 i=0;i<size;++i)b[i]=layout_directory_fixture[i];if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::ok)return false;
    const u32 lookup_at=static_cast<u32>(l.lookups.data-b)+byte_load_be16(l.lookups.data+4),extension_at=lookup_at+byte_load_be16(b+lookup_at+6),second_at=lookup_at+byte_load_be16(b+lookup_at+8);
    put(b,extension_at+2,7);if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::ok || sfnt_layout_lookup(l,1,&lookup)!=SfntStatus::malformed || lookup.table.data)return false;
    put(b,extension_at+2,1);put(b,second_at+2,2);if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::ok || sfnt_layout_lookup(l,1,&lookup)!=SfntStatus::malformed)return false;
    auto* wide=static_cast<u8*>(VirtualAlloc(nullptr,70000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!wide)return false;
    for(u32 i=0;i<size;++i)wide[i]=layout_directory_fixture[i];byte_store_be32(wide+extension_at+4,66000);put(wide,extension_at+66000,1);
    bool ok=sfnt_layout_open({wide,70000},SfntLayoutKind::substitution,&l)==SfntStatus::ok && sfnt_layout_lookup(l,1,&lookup)==SfntStatus::ok && sfnt_lookup_subtable(lookup,0,&sub)==SfntStatus::ok && sub.data==wide+extension_at+66000;
    VirtualFree(wide,0,MEM_RELEASE);
    const u8 empty[]{0,1,0,0,0,10,0,10,0,10,0,0};
    ok=ok && sfnt_layout_open({empty,sizeof(empty)},SfntLayoutKind::positioning,&l)==SfntStatus::ok && !l.script_count && !l.feature_count && !l.lookup_count &&
       sfnt_layout_find_script(l,sfnt_tag('D','F','L','T'),&index)==SfntStatus::missing_table && sfnt_layout_variation_select(l,{},&selected,&record,0)==SfntStatus::ok && record==sfnt_no_variation_record;
    for(u32 i=0;i<size;++i)b[i]=layout_directory_fixture[i];
    if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::ok || sfnt_layout_script(l,1,&tag,&script)!=SfntStatus::ok)return false;
    const u32 script_at=static_cast<u32>(script.table.data-b);put(b,script_at,0);
    ok=ok && sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)==SfntStatus::ok && sfnt_layout_script(l,1,&tag,&script)==SfntStatus::ok && sfnt_script_language(script,l.feature_count,sfnt_default_language,&tag,&lang)==SfntStatus::missing_table && !lang.feature_indices.data && lang.required_feature==65535;
    for(u32 i=0;i<size;++i)b[i]=layout_directory_fixture[i];
    const u32 first_condition=variations+byte_load_be32(b+variations+8);put(b,first_condition,0);coords[0]=0;
    ok=ok && sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)==SfntStatus::ok && sfnt_layout_variation_select(l,{coords,2},&selected,&record)==SfntStatus::ok && record==0;
    for(u32 i=0;i<size;++i)b[i]=layout_directory_fixture[i];
    const u32 sub3=variations+byte_load_be32(b+variations+12+3*8);put(b,sub3+12,1);coords[0]=-1;
    ok=ok && sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)==SfntStatus::ok && sfnt_layout_variation_select(l,{coords,2},&selected,&record)==SfntStatus::malformed && !selected.table.data && record==sfnt_no_variation_record;
    return ok;
}
bool guards() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;constexpr u32 seed_size=sizeof(layout_directory_fixture);if(seed_size>page)return false;
    auto* mem=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!mem)return false;DWORD old=0;if(!VirtualProtect(mem+page,page,PAGE_NOACCESS,&old)){VirtualFree(mem,0,MEM_RELEASE);return false;}
    u32 random=0x52a2b783;for(u32 i=0;i<20000;++i){random^=random<<13;random^=random>>17;random^=random<<5;const u32 size=i<=seed_size?i:random%(seed_size+1);u8* b=mem+page-size;
        for(u32 j=0;j<size;++j)b[j]=layout_directory_fixture[j];if(i>seed_size && size)b[(random>>8)%size]^=static_cast<u8>(random>>16);
        SfntLayout l{};if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&l)!=SfntStatus::ok)continue;
        const u32 candidates[]{sfnt_tag('a','r','a','b'),sfnt_tag('l','a','t','n'),sfnt_tag('D','F','L','T')};SfntLanguageSelection choice{};
        observe(sfnt_layout_select_language(l,{candidates,3},sfnt_tag('T','R','K',' '),&choice,1000));
        for(u16 j=0;j<l.script_count;++j){u32 tag=0;SfntScript s{};if(sfnt_layout_script(l,j,&tag,&s)!=SfntStatus::ok)continue;SfntLanguage lang{};observe(sfnt_script_language(s,l.feature_count,sfnt_default_language,&tag,&lang));for(u16 k=0;k<s.language_count;++k)observe(sfnt_script_language(s,l.feature_count,k,&tag,&lang));}
        for(u16 j=0;j<l.feature_count;++j){u32 tag=0;SfntFeature f{};observe(sfnt_layout_feature(l,j,{},&tag,&f));}
        for(u16 j=0;j<l.lookup_count;++j){SfntLookup lkp{};if(sfnt_layout_lookup(l,j,&lkp)!=SfntStatus::ok)continue;for(u16 k=0;k<lkp.subtable_count;++k){ConstSlice<u8> payload{};observe(sfnt_lookup_subtable(lkp,k,&payload));}}
        i16 coords[]{0,0};SfntFeatureSubstitutions substitutions{};u32 record=0;const auto vs=sfnt_layout_variation_select(l,{coords,2},&substitutions,&record,1000);observe(vs);if(vs==SfntStatus::ok)for(u16 j=0;j<l.feature_count;++j){u32 tag=0;SfntFeature f{};observe(sfnt_layout_feature(l,j,substitutions,&tag,&f));}
    }
    VirtualFree(mem,0,MEM_RELEASE);return true;
}
struct Reader {const u8* data;u32 size,at;
    bool word(u32* v) noexcept {if(size-at<4)return false;*v=byte_load_le32(data+at);at+=4;return true;}
    bool take(u32 n,const u8** v) noexcept {if(n>size-at)return false;*v=data+at;at+=n;return true;}
};
bool indices(Reader& r,u32 count,ConstSlice<u8> actual,bool* match,u64* checks) noexcept {
    if(actual.count!=count*2)*match=false;
    for(u32 i=0;i<count;++i){u32 value=0;if(!r.word(&value))return false;if(i<actual.count/2 && byte_load_be16(actual.data+i*2)!=value)*match=false;++*checks;}return true;
}
i16 coordinates[65535];
bool corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER len{};if(!GetFileSizeEx(file,&len) || len.QuadPart<12 || len.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping){CloseHandle(file);return false;}auto* bytes=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));Reader r{bytes,static_cast<u32>(len.QuadPart),8};u32 total=0,failures=0;u64 script_checks=0,language_checks=0,feature_checks=0,lookup_checks=0,subtable_checks=0,variation_checks=0,index_checks=0;
    bool ok=bytes && bytes_equal(bytes,"MCLDIR1",8) && r.word(&total) && total;
    for(u32 record=0;record<total && ok;++record){u32 kind=0,size=0,axes=0,count=0;const u8* raw=nullptr;ok=r.word(&kind)&&r.word(&size)&&r.word(&axes)&&kind<2&&axes<=65535&&r.take(size,&raw)&&r.word(&count)&&count<=65535;if(!ok)break;
        SfntLayout l{};bool match=sfnt_layout_open({raw,size},static_cast<SfntLayoutKind>(kind),&l)==SfntStatus::ok && l.script_count==count;
        for(u32 i=0;i<count && ok;++i){u32 expected_tag=0,offset=0,named=0,langs=0,tag=0;SfntScript script{};
            ok=r.word(&expected_tag)&&r.word(&offset)&&r.word(&named)&&r.word(&langs)&&named<=65535&&langs<=named+1;if(!ok)break;u16 found=0;
            if(sfnt_layout_script(l,static_cast<u16>(i),&tag,&script)!=SfntStatus::ok || tag!=expected_tag || offset>size || script.table.data!=raw+offset || script.language_count!=named || sfnt_layout_find_script(l,expected_tag,&found)!=SfntStatus::ok || found!=i)match=false;++script_checks;
            for(u32 j=0;j<langs && ok;++j){u32 index=0,ltag=0,required=0,n=0;ok=r.word(&index)&&r.word(&ltag)&&r.word(&required)&&r.word(&n)&&n<=65535;if(!ok)break;SfntLanguage lang{};
                if(sfnt_script_language(script,l.feature_count,index,&tag,&lang)!=SfntStatus::ok || tag!=ltag || lang.required_feature!=required)match=false;
                if(index!=sfnt_default_language && (sfnt_script_find_language(script,ltag,&found)!=SfntStatus::ok || found!=index))match=false;ok=indices(r,n,lang.feature_indices,&match,&index_checks);++language_checks;}}
        u32 features=0;ok=ok&&r.word(&features)&&features<=65535;if(l.feature_count!=features)match=false;
        for(u32 i=0;i<features && ok;++i){u32 expected_tag=0,offset=0,params=0,n=0,tag=0;ok=r.word(&expected_tag)&&r.word(&offset)&&r.word(&params)&&r.word(&n)&&n<=65535;if(!ok)break;SfntFeature feature{};
            if(sfnt_layout_feature(l,static_cast<u16>(i),{},&tag,&feature)!=SfntStatus::ok || tag!=expected_tag || offset>size || feature.table.data!=raw+offset)match=false;
            if(params==0xffffffff){if(feature.params_offset)match=false;}else if(params>size || !feature.table.data || feature.table.data+feature.params_offset!=raw+params)match=false;
            ok=indices(r,n,feature.lookup_indices,&match,&index_checks);++feature_checks;}
        u32 lookups=0;ok=ok&&r.word(&lookups)&&lookups<=65535;if(l.lookup_count!=lookups)match=false;
        for(u32 i=0;i<lookups && ok;++i){u32 type=0,flags=0,set=0,n=0;ok=r.word(&type)&&r.word(&flags)&&r.word(&set)&&r.word(&n)&&n<=65535;if(!ok)break;SfntLookup lookup{};
            if(sfnt_layout_lookup(l,static_cast<u16>(i),&lookup)!=SfntStatus::ok || lookup.source_type!=type || lookup.flags!=flags || lookup.mark_set!=set || lookup.subtable_count!=n)match=false;++lookup_checks;
            for(u32 j=0;j<n && ok;++j){u32 resolved=0,offset=0;ok=r.word(&resolved)&&r.word(&offset);if(!ok)break;ConstSlice<u8> data{};
                if(sfnt_lookup_subtable(lookup,static_cast<u16>(j),&data)!=SfntStatus::ok || lookup.type!=resolved || offset>size || data.data!=raw+offset)match=false;++subtable_checks;}}
        u32 probes=0;ok=ok&&r.word(&probes)&&probes<=1000000;
        for(u32 i=0;i<probes && ok;++i){u32 n=0,expected=0,substitutions=0;ok=r.word(&n)&&n==axes;if(!ok)break;
            for(u32 a=0;a<n && ok;++a){u32 value=0;ok=r.word(&value)&&static_cast<i32>(value)>=-16384&&static_cast<i32>(value)<=16384;coordinates[a]=static_cast<i16>(value);}
            ok=ok&&r.word(&expected)&&r.word(&substitutions);SfntFeatureSubstitutions selected{};u32 actual=0;
            if(sfnt_layout_variation_select(l,{coordinates,n},&selected,&actual)!=SfntStatus::ok || actual!=expected || selected.count!=substitutions)match=false;++variation_checks;
            for(u32 f=0;f<features && ok;++f){u32 count_indices=0,tag=0;ok=r.word(&count_indices)&&count_indices<=65535;if(!ok)break;SfntFeature feature{};if(sfnt_layout_feature(l,static_cast<u16>(f),selected,&tag,&feature)!=SfntStatus::ok)match=false;ok=indices(r,count_indices,feature.lookup_indices,&match,&index_checks);}}
        if(!match){++failures;print(utf8_literal("mismatch_record="));number(record);print(utf8_literal("\n"));}
    }
    ok=ok && r.at==r.size;print(utf8_literal("records="));number(total);print(utf8_literal("\nscripts="));number(script_checks);print(utf8_literal("\nlanguages="));number(language_checks);print(utf8_literal("\nfeatures="));number(feature_checks);print(utf8_literal("\nlookups="));number(lookup_checks);print(utf8_literal("\nsubtables="));number(subtable_checks);print(utf8_literal("\nvariation_probes="));number(variation_checks);print(utf8_literal("\nindex_checks="));number(index_checks);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\ncomplete="));number(ok);print(utf8_literal("\n"));
    if(bytes)UnmapViewOfFile(bytes);CloseHandle(mapping);CloseHandle(file);return ok&&!failures;
}
int run() noexcept {
    if(!units()){print(utf8_literal("FAIL layout directory units\n"));return 1;}if(!guards()){print(utf8_literal("FAIL layout directory guards\n"));return 2;}
    const wchar_t* cmd=GetCommandLineW();bool quote=false;while(*cmd){if(*cmd=='"')quote=!quote;else if(!quote && (*cmd==' ' || *cmd=='\t'))break;++cmd;}while(*cmd==' ' || *cmd=='\t')++cmd;
    if(*cmd){wchar_t path[32768];u32 n=0;quote=*cmd=='"';if(quote)++cmd;while(*cmd && !(quote?*cmd=='"':(*cmd==' ' || *cmd=='\t'))){if(n+1>=32768)return 3;path[n++]=*cmd++;}if(quote){if(*cmd!='"')return 3;++cmd;}while(*cmd==' ' || *cmd=='\t')++cmd;if(*cmd || !n)return 3;path[n]=0;if(!corpus(path))return 4;}
    print(utf8_literal("PASS native layout directory\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_layout_directory_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
