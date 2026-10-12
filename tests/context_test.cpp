#include "maicord/text/context.hpp"
#include "maicord/core/byte_order.hpp"

#include "glyph_view_check.hpp"

namespace maicord {namespace {
#include "generated/context_fixture.inc"
volatile u32 observations=0;
void observe(SfntStatus s) noexcept {observations=observations+static_cast<u32>(s)+1;}
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
bool units() noexcept {
    SfntGdef gdef{};if(sfnt_gdef_open({context_gdef_fixture,sizeof(context_gdef_fixture)},10,&gdef)!=SfntStatus::ok)return false;SfntLookupFilter filter{};if(sfnt_lookup_filter_prepare(gdef,8,0,&filter)!=SfntStatus::ok)return false;
    SfntCoverage coverage[16];SfntClassDef classes[3];u32 positions[4];SfntGsubGlyph input[11],reverse_input[9];const u16 ids[]{2,9,1,9,3,9,4,9,5,9,6},rev_ids[]{2,9,1,9,3,9,5,9,6};
    for(u32 i=0;i<11;++i)input[i]={ids[i],sfnt_gdef_glyph_class(gdef,ids[i]),0};for(u32 i=0;i<9;++i)reverse_input[i]={rev_ids[i],sfnt_gdef_glyph_class(gdef,rev_ids[i]),0};
    for(const auto& f:context_fixtures){const ConstSlice<u8> bytes{f.data,f.size};
        if(f.mode==2){u32 count=0;SfntReverseSubstitution r{};SfntGsubMatch result{};
            if(sfnt_gsub_reverse_requirements(bytes,&count)!=SfntStatus::ok || count!=5 || sfnt_gsub_reverse_open(bytes,10,{coverage,4},&r)!=SfntStatus::output_too_small || r.table.data)return false;
            if(sfnt_gsub_reverse_open(bytes,10,{coverage,16},&r)!=SfntStatus::ok || sfnt_gsub_reverse_match(r,gdef,filter,{reverse_input,9},4,&result)!=SfntStatus::ok || !result.matched || result.single!=8 || result.last_input!=4 || result.input_count!=1)return false;
            if(sfnt_gsub_reverse_match(r,gdef,filter,{reverse_input,8},4,&result)!=SfntStatus::ok || result.matched)return false;
            if(sfnt_gsub_reverse_match(r,gdef,filter,{reverse_input,9},4,&result,0)!=SfntStatus::resource_limit || result.matched)return false;
            continue;
        }
        const auto kind=static_cast<SfntContextKind>(f.mode);SfntContextRequirements req{};SfntContext c{};SfntContextMatch result{};
        if(sfnt_context_requirements(bytes,kind,&req)!=SfntStatus::ok || req.coverages!=(f.format==3?(f.mode?6u:2u):1u) || req.classes!=(f.format==2?(f.mode?3u:1u):0u))return false;
        if(sfnt_context_open(bytes,kind,10,3,{}, {},&c)!=SfntStatus::output_too_small || c.table.data)return false;
        if(sfnt_context_open(bytes,kind,10,3,{coverage,16},{classes,3},&c)!=SfntStatus::ok || c.max_inputs!=2 || sfnt_context_match(c,gdef,filter,{input,11},4,7,{positions,4},&result)!=SfntStatus::ok || !result.matched || result.input_count!=2 || result.last_input!=6 || positions[0]!=4 || positions[1]!=6 || result.actions.count!=12)return false;
        const auto& first=c.coverages[c.format==3&&c.kind==SfntContextKind::chained?byte_load_be16(c.table.data+2):0];
        const u32 first_index=sfnt_coverage_index(first,input[4].glyph);
        if(first_index==sfnt_not_covered||sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,7,{positions,4},&result,1000000,nullptr,0,first_index)!=SfntStatus::ok||!result.matched||result.input_count!=2||result.last_input!=6||positions[0]!=4||positions[1]!=6||result.actions.count!=12)return false;
        SfntContextMatch rejected{};
        if(sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,7,{positions,4},&rejected,1000000,nullptr,0,first.covered_count)!=SfntStatus::invalid_input||rejected.matched||rejected.input_count)return false;
        const u16 seq[]{1,0,1},lookup[]{2,1,0};for(u32 i=0;i<3;++i){SfntSequenceAction action{};if(sfnt_context_action(result,i,&action)!=SfntStatus::ok || action.sequence_index!=seq[i] || action.lookup_index!=lookup[i])return false;}
        const u32 actions_at=static_cast<u32>(result.actions.data-f.data);
        if(sfnt_context_match(c,gdef,filter,{input,11},4,7,{positions,1},&result)!=SfntStatus::output_too_small || result.input_count!=2 || result.matched || result.actions.data)return false;
        if(sfnt_context_match(c,gdef,filter,{input,11},4,5,{positions,4},&result)!=SfntStatus::ok || result.matched!=(f.format!=3) || result.actions.count || result.input_count!=(f.format==3?0u:1u))return false;
        if(sfnt_context_match(c,gdef,filter,{input,11},4,7,{positions,4},&result,0)!=SfntStatus::resource_limit || result.matched)return false;
        u8 changed[512];if(f.size>sizeof(changed))return false;for(u32 i=0;i<f.size;++i)changed[i]=f.data[i];put(changed,actions_at,2);
        if(sfnt_context_open({changed,f.size},kind,10,3,{coverage,16},{classes,3},&c)!=SfntStatus::malformed || c.table.data)return false;
        put(changed,actions_at,1);put(changed,actions_at+2,3);if(sfnt_context_open({changed,f.size},kind,10,3,{coverage,16},{classes,3},&c)!=SfntStatus::malformed)return false;
    }
    // Ignore surplus format-1 sets, including an unused out-of-range offset.
    u8 extra[512];const auto& f=context_fixtures[0];for(u32 i=0;i<6;++i)extra[i]=f.data[i];put(extra,2,static_cast<u16>(byte_load_be16(f.data+2)+2));put(extra,4,2);put(extra,6,10);put(extra,8,65535);for(u32 i=8;i<f.size;++i)extra[i+2]=f.data[i];
    SfntContext c{};SfntContextMatch result{};if(sfnt_context_open({extra,f.size+2},SfntContextKind::sequence,10,3,{coverage,16},{classes,3},&c)!=SfntStatus::ok || sfnt_context_match(c,gdef,filter,{input,11},4,7,{positions,4},&result)!=SfntStatus::ok || !result.matched)return false;
    // Ignore surplus coverage entries when no corresponding rule set exists.
    for(u32 i=0;i<f.size;++i)extra[i]=f.data[i];const u32 cov=byte_load_be16(extra+2);put(extra,cov+2,2);put(extra,cov+6,7);input[4].glyph=7;input[4].glyph_class=SfntGlyphClass::base;
    return sfnt_context_open({extra,f.size+2},SfntContextKind::sequence,10,3,{coverage,16},{classes,3},&c)==SfntStatus::ok && sfnt_context_match(c,gdef,filter,{input,11},4,7,{positions,4},&result)==SfntStatus::ok && !result.matched;
}
bool guards() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* mem=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!mem)return false;DWORD old=0;if(!VirtualProtect(mem+page,page,PAGE_NOACCESS,&old)){VirtualFree(mem,0,MEM_RELEASE);return false;}
    SfntCoverage coverages[128];SfntClassDef classes[3];u32 positions[16];SfntGsubGlyph glyphs[11],reverse_glyphs[9];const u16 ids[]{2,9,1,9,3,9,4,9,5,9,6},reverse_ids[]{2,9,1,9,3,9,5,9,6};for(u32 i=0;i<11;++i)glyphs[i]={ids[i],ids[i]==9 || ids[i]==3?SfntGlyphClass::mark:SfntGlyphClass::base,0};for(u32 i=0;i<9;++i)reverse_glyphs[i]={reverse_ids[i],reverse_ids[i]==9 || reverse_ids[i]==3?SfntGlyphClass::mark:SfntGlyphClass::base,0};SfntLookupFilter filter{{},8};u32 random=0x825ceb77;
    for(u32 i=0;i<20000;++i){const auto& f=context_fixtures[i%7];random^=random<<13;random^=random>>17;random^=random<<5;const u32 size=i<1024?minimum(i/7,f.size):random%(f.size+1);u8* b=mem+page-size;for(u32 j=0;j<size;++j)b[j]=f.data[j];if(i>=1024 && size)b[(random>>8)%size]^=static_cast<u8>(random>>16);
        if(f.mode==2){u32 count=0;const auto need=sfnt_gsub_reverse_requirements({b,size},&count);observe(need);if(need!=SfntStatus::ok || count>128)continue;SfntReverseSubstitution r{};const auto s=sfnt_gsub_reverse_open({b,size},10,{coverages,128},&r,1000);observe(s);if(s==SfntStatus::ok){SfntGsubMatch m{};observe(sfnt_gsub_reverse_match(r,{},filter,{reverse_glyphs,9},4,&m,100));observations=observations+m.single;}continue;}
        const auto kind=static_cast<SfntContextKind>(f.mode);SfntContextRequirements req{};const auto need=sfnt_context_requirements({b,size},kind,&req);observe(need);if(need!=SfntStatus::ok || req.coverages>128 || req.classes>3)continue;SfntContext c{};const auto s=sfnt_context_open({b,size},kind,10,3,{coverages,128},{classes,3},&c,1000);observe(s);if(s==SfntStatus::ok){SfntContextMatch m{};observe(sfnt_context_match(c,{},filter,{glyphs,11},4,7,{positions,16},&m,100));if(m.matched)for(u32 j=0;j<m.actions.count/4;++j){SfntSequenceAction action{};observe(sfnt_context_action(m,j,&action));observations=observations+action.sequence_index+action.lookup_index;}}
    }
    VirtualFree(mem,0,MEM_RELEASE);return true;
}
struct Reader {const u8* data;u32 size,at;
    bool word(u32* v) noexcept {if(size-at<4)return false;*v=byte_load_le32(data+at);at+=4;return true;}
    bool take(u32 n,const u8** v) noexcept {if(n>size-at)return false;*v=data+at;at+=n;return true;}
};
SfntCoverage coverages[196605];SfntClassDef classes[3];SfntGsubGlyph glyphs[65536];u32 positions[65536];
bool corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER len{};if(!GetFileSizeEx(file,&len) || len.QuadPart<12 || len.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE map=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!map){CloseHandle(file);return false;}auto* bytes=static_cast<const u8*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0));Reader r{bytes,static_cast<u32>(len.QuadPart),8};u32 tables=0,failures=0,counts[3][4]{};u64 probes=0,positives=0,actions=0,reverse_outputs=0,position_checks=0;
    bool ok=bytes && bytes_equal(bytes,"MCCTX1\0",8) && r.word(&tables) && tables>0;
    for(u32 table=0;table<tables && ok;++table){u32 limit=0,size=0,gdsize=0,count=0,kind=0,lookups=0;const u8 *raw=nullptr,*gd=nullptr;ok=r.word(&limit)&&r.word(&size)&&r.word(&gdsize)&&r.word(&count)&&r.word(&kind)&&r.word(&lookups)&&limit>0&&limit<=65536&&kind<=2&&lookups<=65535&&count>0&&count<=1000000&&r.take(size,&raw)&&r.take(gdsize,&gd);if(!ok)break;
        SfntLayout layout{};SfntGdef gdef{};bool match=(kind==2 || (sfnt_layout_open({raw,size},static_cast<SfntLayoutKind>(kind),&layout)==SfntStatus::ok && layout.lookup_count==lookups)) && (!gdsize || sfnt_gdef_open({gd,gdsize},limit,&gdef)==SfntStatus::ok);
        for(u32 sub=0;sub<count && ok;++sub){u32 mode=0,offset=0,flags=0,markset=0,li=0,si=0,cov_count=0,class_count=0,max_inputs=0,cases=0;ok=r.word(&mode)&&r.word(&offset)&&r.word(&flags)&&r.word(&markset)&&r.word(&li)&&r.word(&si)&&r.word(&cov_count)&&r.word(&class_count)&&r.word(&max_inputs)&&r.word(&cases)&&mode<=2&&offset<size&&flags<=65535&&markset<=65535&&si<=65535&&cov_count<=196605&&class_count<=3&&max_inputs<=65535&&cases<=1000000;if(!ok)break;
            if(kind<2){SfntLookup lookup{};ConstSlice<u8> payload{};const u32 typ=kind==0?(mode==2?8u:mode==1?6u:5u):(mode==1?8u:7u);
                if(li>65535 || sfnt_layout_lookup(layout,static_cast<u16>(li),&lookup)!=SfntStatus::ok || sfnt_lookup_subtable(lookup,static_cast<u16>(si),&payload)!=SfntStatus::ok || payload.data!=raw+offset || lookup.type!=typ || lookup.flags!=flags || lookup.mark_set!=markset)match=false;}
            const ConstSlice<u8> data{raw+offset,size-offset};SfntContext c{};SfntReverseSubstitution reverse{};SfntStatus status=SfntStatus::ok;
            if(mode==2){u32 needed=0;status=sfnt_gsub_reverse_requirements(data,&needed);if(needed!=cov_count || class_count || max_inputs!=1)match=false;if(status==SfntStatus::ok)status=sfnt_gsub_reverse_open(data,limit,{coverages,196605},&reverse);}
            else {SfntContextRequirements req{};const auto k=static_cast<SfntContextKind>(mode);status=sfnt_context_requirements(data,k,&req);if(req.coverages!=cov_count || req.classes!=class_count)match=false;if(status==SfntStatus::ok)status=sfnt_context_open(data,k,limit,static_cast<u16>(lookups),{coverages,196605},{classes,3},&c);if(c.max_inputs!=max_inputs)match=false;}
            if(status!=SfntStatus::ok)match=false;SfntLookupFilter filter{};if(sfnt_lookup_filter_prepare(gdef,static_cast<u16>(flags),static_cast<u16>(markset),&filter)!=SfntStatus::ok)match=false;
            for(u32 q=0;q<cases && ok;++q){u32 start=0,end=0,n=0,consumed=0,ac=0;ok=r.word(&start)&&r.word(&end)&&r.word(&n)&&r.word(&consumed)&&r.word(&ac)&&n>0&&n<=65536&&start<end&&end<=n&&consumed<=65535&&consumed<=n&&ac<=65535;if(!ok)break;
                for(u32 i=0;i<n && ok;++i){u32 gid=0,cl=0;ok=r.word(&gid)&&r.word(&cl)&&gid<limit&&cl<=4;glyphs[i]={static_cast<u16>(gid),static_cast<SfntGlyphClass>(cl),0};}
                const u8 *expected_positions=nullptr,*expected_actions=nullptr;ok=ok&&r.take(consumed*4,&expected_positions)&&r.take(ac*8,&expected_actions);if(!ok)break;
                if(mode==2){SfntGsubMatch m{};if(status!=SfntStatus::ok || qa_reverse_match(reverse,gdef,filter,{glyphs,n},start,&m)!=SfntStatus::ok || m.matched!=(consumed!=0) || m.input_count!=consumed || m.output_count!=ac)match=false;
                    if(consumed && (consumed!=1 || ac!=1 || m.last_input!=start || byte_load_le32(expected_positions)!=start || byte_load_le32(expected_actions)!=0 || m.single!=byte_load_le32(expected_actions+4)))match=false;if(consumed)++reverse_outputs;
                } else {SfntContextMatch m{};if(status!=SfntStatus::ok || qa_context_match(c,gdef,filter,{glyphs,n},start,end,{positions,65536},&m)!=SfntStatus::ok || m.matched!=(consumed!=0) || m.input_count!=consumed || m.actions.count/4!=ac)match=false;
                    if(consumed && m.last_input!=byte_load_le32(expected_positions+(consumed-1)*4))match=false;for(u32 i=0;i<consumed;++i){if(positions[i]!=byte_load_le32(expected_positions+i*4))match=false;++position_checks;}
                    for(u32 i=0;i<ac;++i){SfntSequenceAction a{};if(sfnt_context_action(m,i,&a)!=SfntStatus::ok || a.sequence_index!=byte_load_le32(expected_actions+i*8) || a.lookup_index!=byte_load_le32(expected_actions+i*8+4))match=false;++actions;}}
                ++probes;if(consumed)++positives;
            }
            const u16 fmt=data.count>=2?byte_load_be16(data.data):0;if(fmt<4)++counts[mode][fmt];if(!match){++failures;print(utf8_literal("mismatch_table="));number(table);print(utf8_literal(" subtable="));number(sub);print(utf8_literal(" status="));number(static_cast<u32>(status));print(utf8_literal("\n"));match=true;}
        }
    }
    ok=ok&&r.at==r.size;print(utf8_literal("tables="));number(tables);for(u32 m=0;m<3;++m)for(u32 f=1;f<4;++f){print(utf8_literal("\nmode_"));number(m);print(utf8_literal("_format_"));number(f);print(utf8_literal("="));number(counts[m][f]);}print(utf8_literal("\nprobes="));number(probes);print(utf8_literal("\npositive="));number(positives);print(utf8_literal("\nposition_checks="));number(position_checks);print(utf8_literal("\naction_checks="));number(actions);print(utf8_literal("\nreverse_outputs="));number(reverse_outputs);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\ncomplete="));number(ok);print(utf8_literal("\n"));
    print(utf8_literal("gap_view_queries="));number(qa_gap_queries);print(utf8_literal("\n"));
    if(bytes)UnmapViewOfFile(bytes);CloseHandle(map);CloseHandle(file);return ok&&!failures;
}
int run() noexcept {
    if(!units()){print(utf8_literal("FAIL context units\n"));return 1;}if(!guards()){print(utf8_literal("FAIL context guards\n"));return 2;}
    const wchar_t* cmd=GetCommandLineW();bool quote=false;while(*cmd){if(*cmd=='"')quote=!quote;else if(!quote && (*cmd==' ' || *cmd=='\t'))break;++cmd;}while(*cmd==' ' || *cmd=='\t')++cmd;
    if(*cmd){wchar_t path[32768];u32 n=0;quote=*cmd=='"';if(quote)++cmd;while(*cmd && !(quote?*cmd=='"':(*cmd==' ' || *cmd=='\t'))){if(n+1>=32768)return 3;path[n++]=*cmd++;}if(quote){if(*cmd!='"')return 3;++cmd;}while(*cmd==' ' || *cmd=='\t')++cmd;if(*cmd || !n)return 3;path[n]=0;if(!corpus(path))return 4;}
    print(utf8_literal("PASS native contextual matching\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_context_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
