#include "gsub_program_fixture.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
#include "generated/gsub_execute_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
struct Reader {ConstSlice<u8> data;u32 at=0;
    bool word(u32* v) noexcept {if(at>data.count || data.count-at<4)return false;*v=byte_load_le32(data.data+at);at+=4;return true;}
    bool take(u32 n,ConstSlice<u8>* v) noexcept {if(at>data.count || n>data.count-at)return false;*v={data.data+at,n};at+=n;return true;}
};
SfntGsubExecutableLookup lookups[128];SfntGsubExecutableSubtable subtables[1024];SfntCoverage coverages[1024];SfntClassDef classes[128];
SfntGsubExecutionFrame frames[32];u32 positions[1024],rebased[1024],consumed[256];SfntGsubGlyph replacements[256],glyphs[1024];
u64 queries=0,checked=0,edits=0,windows_differences=0,budget_checks=0,shared_certificates=0;u32 failures=0;
SfntGsubExecutionScratch scratch() noexcept {return {{frames,32},{positions,1024},{rebased,1024},{consumed,256},{reinterpret_cast<u8*>(replacements),sizeof(replacements)}};}
GsubProgramStorage storage() noexcept {return {{lookups,128},{subtables,1024},{coverages,1024},{classes,128}};}
struct Fixture {ConstSlice<u8> gsub,gdef,input;u32 limit,count;};
bool fixture(u32 index,Fixture* output) noexcept {
    Reader r{{gsub_execution_fixture,sizeof(gsub_execution_fixture)},8};u32 cases=0;if(!r.word(&cases) || index>=cases)return false;
    for(u32 i=0;i<=index;++i){u32 fields[11]{};for(u32& field:fields)if(!r.word(&field))return false;ConstSlice<u8> ignored{};
        if(!r.take(fields[1],&output->gsub)||!r.take(fields[2],&output->gdef)||!r.take(fields[3]*4,&output->input)||!r.take((fields[8]+fields[10])*2,&ignored))return false;
        output->limit=fields[0];output->count=fields[3];}
    return true;
}
bool prepare(const Fixture& f,SfntGsubProgram* program) noexcept {
    SfntLayout layout{};SfntGdef gdef{};return sfnt_layout_open(f.gsub,SfntLayoutKind::substitution,&layout)==SfntStatus::ok &&
        sfnt_gdef_open(f.gdef,f.limit,&gdef)==SfntStatus::ok && prepare_gsub_program(layout,gdef,f.limit,storage(),program)==SfntStatus::ok;
}
#include "gsub_unreachable_units.inc"
#include "gsub_direct_units.inc"
bool units() noexcept {
    const u8 first_bytes[]{0,3,0,9};u8 next_bytes[]{0,3,0,9};
    const SfntCoverage first{first_bytes,2,1,2};SfntCoverage next{next_bytes,2,1,2};u32 proof_work=5;
    if(!sfnt_lookup_coverage_equal(first,next,proof_work)||proof_work)return false;
    proof_work=4;if(sfnt_lookup_coverage_equal(first,next,proof_work)||proof_work!=3)return false;
    proof_work=0;if(sfnt_lookup_coverage_equal(first,first,proof_work)||proof_work)return false;
    proof_work=1;if(!sfnt_lookup_coverage_equal(first,first,proof_work)||proof_work)return false;
    next_bytes[3]=10;proof_work=5;if(sfnt_lookup_coverage_equal(first,next,proof_work)||proof_work)return false;
    next=first;next.format=2;proof_work=5;if(sfnt_lookup_coverage_equal(first,next,proof_work)||proof_work!=4)return false;
    next=first;next.covered_count=3;proof_work=5;if(sfnt_lookup_coverage_equal(first,next,proof_work)||proof_work!=4)return false;
    next=first;next.record_count=1;proof_work=5;if(sfnt_lookup_coverage_equal(first,next,proof_work)||proof_work!=4)return false;
    if(!unreachable_units())return false;
    Fixture f{};SfntGsubProgram program{};if(!fixture(1,&f) || !prepare(f,&program))return false;
    for(u32 mode=0;mode<4;++mode){copy_bytes(glyphs,f.input.data,f.input.count);SfntGlyphBuffer buffer{};
        if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),(f.count+(mode?8u:0u))*sizeof(SfntGsubGlyph)},sizeof(SfntGsubGlyph),f.count,&buffer)!=SfntStatus::ok)return false;
        auto temp=scratch();if(mode==1)temp.replacements.count=sizeof(SfntGsubGlyph);SfntGsubExecutionResult result{};
        const auto status=sfnt_gsub_execute_lookup(program,0,&buffer,0,f.count,0,temp,{mode==2?f.count:1000000u,mode==3?0u:100000u},&result);
        if(status!=(mode<2?SfntStatus::output_too_small:SfntStatus::resource_limit) || result.edits || !bytes_equal(glyphs,f.input.data,f.input.count) || buffer.gap_begin!=f.count)return false;
    }
    // A failed later edit retains the earlier committed edit, as documented.
    SfntGsubGlyph original{};copy_bytes(&original,f.input.data,sizeof(original));glyphs[0]=glyphs[1]=original;
    SfntGlyphBuffer partial{};if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),3*sizeof(SfntGsubGlyph)},sizeof(SfntGsubGlyph),2,&partial)!=SfntStatus::ok)return false;
    SfntGsubExecutionResult partial_result{};if(sfnt_gsub_execute_lookup(program,0,&partial,0,2,0,scratch(),{1000000,100000},&partial_result)!=SfntStatus::output_too_small || partial_result.edits!=1 || partial_result.input_end!=3 || sfnt_glyph_count(sfnt_glyph_buffer_view(partial))!=3 || sfnt_glyph_at(sfnt_glyph_buffer_view(partial),2).glyph!=original.glyph)return false;
    if(!fixture(4,&f) || !prepare(f,&program))return false;
    for(u32 mode=0;mode<3;++mode){copy_bytes(glyphs,f.input.data,f.input.count);SfntGlyphBuffer buffer{};if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),16*sizeof(SfntGsubGlyph)},sizeof(SfntGsubGlyph),f.count,&buffer)!=SfntStatus::ok)return false;
        auto temp=scratch();if(mode==0)temp.positions.count=f.count;else if(mode==1)temp.rebased.count=f.count;else temp.frames.count=0;
        SfntGsubExecutionResult result{};const auto status=sfnt_gsub_execute_lookup(program,0,&buffer,0,f.count,0,temp,{1000000,100000},&result);
        if(status!=(mode==2?SfntStatus::resource_limit:SfntStatus::output_too_small) || result.edits || !bytes_equal(glyphs,f.input.data,f.input.count))return false;
    }
    u64 full_work=0;
    for(u64 budget=1000000;;){copy_bytes(glyphs,f.input.data,f.input.count);SfntGlyphBuffer buffer{};if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),16*sizeof(SfntGsubGlyph)},sizeof(SfntGsubGlyph),f.count,&buffer)!=SfntStatus::ok)return false;
        SfntGsubExecutionResult result{};const auto status=sfnt_gsub_execute_lookup(program,0,&buffer,0,f.count,0,scratch(),{budget,100000},&result);
        if(result.operations>budget)return false;
        if(budget==1000000){if(status!=SfntStatus::ok || !result.operations || result.operations>10000)return false;full_work=result.operations;budget=0;continue;}
        ++budget_checks;if(status!=(budget<full_work?SfntStatus::resource_limit:SfntStatus::ok))return false;
        if(budget==full_work)break;++budget;
    }
    SfntGdef gdef{};if(sfnt_gdef_open(f.gdef,f.limit,&gdef)!=SfntStatus::ok)return false;
    // A valid lookup index can create a recursive cycle. It must terminate at
    // the caller's frame limit without relying on C++ recursion or mutating text.
    u8 changed[2048];if(f.gsub.count>sizeof(changed))return false;copy_bytes(changed,f.gsub.data,f.gsub.count);
    const auto& sub=program.lookups.data[0].subtables.data[0].context;SfntContextMatch match{};copy_bytes(glyphs,f.input.data,f.input.count);
    if(sfnt_context_match_view(sub,gdef,program.lookups.data[0].filter,sfnt_glyph_view({glyphs,f.count}),0,f.count,{consumed,256},&match)!=SfntStatus::ok || !match.matched)return false;
    const u32 offset=static_cast<u32>(match.actions.data-f.gsub.data);changed[offset+2]=changed[offset+3]=0;
    f.gsub={changed,f.gsub.count};if(!prepare(f,&program))return false;SfntGlyphBuffer cycle{};if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),16*sizeof(SfntGsubGlyph)},sizeof(SfntGsubGlyph),f.count,&cycle)!=SfntStatus::ok)return false;
    auto temp=scratch();temp.frames.count=4;SfntGsubExecutionResult result{};
    if(sfnt_gsub_execute_lookup(program,0,&cycle,0,f.count,0,temp,{1000000,100000},&result)!=SfntStatus::resource_limit || result.max_depth!=4 || result.edits || !bytes_equal(glyphs,f.input.data,f.input.count))return false;
    return true;
}
volatile u32 guard_observations=0;
bool guards() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;
    DWORD old=0;if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    u32 random=0x19db342a;bool ok=true;
    const u32 fixture_count=byte_load_le32(gsub_execution_fixture+8);
    for(u32 i=0;i<10000 && ok;++i){Fixture f{};if(!fixture(i%fixture_count,&f) || f.gsub.count>page){ok=false;break;}random^=random<<13;random^=random>>17;random^=random<<5;
        const u32 size=i<fixture_count?f.gsub.count:random%(f.gsub.count+1);u8* data=memory+page-size;copy_bytes(data,f.gsub.data,size);if(i>=fixture_count && size)data[(random>>8)%size]^=static_cast<u8>(random>>16);
        SfntLayout layout{};SfntGdef gdef{};SfntGsubProgram program{};auto s=sfnt_layout_open({data,size},SfntLayoutKind::substitution,&layout);guard_observations=guard_observations+static_cast<u32>(s)+1;if(s!=SfntStatus::ok)continue;
        if(sfnt_gdef_open(f.gdef,f.limit,&gdef)!=SfntStatus::ok){ok=false;break;}s=prepare_gsub_program(layout,gdef,f.limit,storage(),&program,false,false,1000);guard_observations=guard_observations+static_cast<u32>(s)+1;if(s!=SfntStatus::ok)continue;
        copy_bytes(glyphs,f.input.data,f.input.count);SfntGlyphBuffer buffer{};if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),sizeof(glyphs)},sizeof(SfntGsubGlyph),f.count,&buffer)!=SfntStatus::ok){ok=false;break;}
        SfntGsubExecutionResult result{};s=sfnt_gsub_execute_lookup(program,0,&buffer,0,f.count,0,scratch(),{10000,1000},&result);guard_observations=guard_observations+static_cast<u32>(s)+result.input_end+1;
        if(result.operations>10000 || sfnt_glyph_count(sfnt_glyph_buffer_view(buffer))>1024)ok=false;
    }
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
bool corpus(ConstSlice<u8> bytes) noexcept {
    if(bytes.count<12 || !bytes_equal(bytes.data,"MCGEX02\0",8))return false;Reader r{bytes,8};u32 cases=0;if(!r.word(&cases) || !cases || cases>100000)return false;
    for(u32 c=0;c<cases;++c){u32 limit=0,gs=0,gd=0,n=0,start=0,end=0,index=0,alt=0,expected_count=0,kind=0,windows_count=0;ConstSlice<u8> gsub{},gdef{},input{},expected{},windows{};
        if(!r.word(&limit)||!r.word(&gs)||!r.word(&gd)||!r.word(&n)||!r.word(&start)||!r.word(&end)||!r.word(&index)||!r.word(&alt)||!r.word(&expected_count)||!r.word(&kind)||!r.word(&windows_count)||!limit||limit>65536||n>512||expected_count>512||windows_count>512||kind>3||index>65535||!r.take(gs,&gsub)||!r.take(gd,&gdef)||!r.take(n*4,&input)||!r.take(expected_count*2,&expected)||!r.take(windows_count*2,&windows))return false;
        SfntLayout layout{};SfntGdef definition{};SfntGsubProgram program{};
        const bool ignore_empty=(kind&2)!=0;
        // Bit one selects the separately observed empty-sequence compatibility
        // policy. Bit zero retains the existing explicit-difference marker.
        if(sfnt_layout_open(gsub,SfntLayoutKind::substitution,&layout)!=SfntStatus::ok || sfnt_gdef_open(gdef,limit,&definition)!=SfntStatus::ok ||
           prepare_gsub_program(layout,definition,limit,storage(),&program,ignore_empty)!=SfntStatus::ok)return false;kind&=1;
        if(!direct_preparation(layout,definition,limit,ignore_empty))return false;
        const SfntGsubExecutableLookup* sparse_entries[128];SfntGsubLookupSource sparse{nullptr,sparse_entries};
        u16 prepared_bounds[128];for(u32 i=0;i<program.lookups.count;++i){sparse_entries[i]=direct_lookups+i;prepared_bounds[i]=lookups[i].bounds.ready;if(prepared_bounds[i]&2)++shared_certificates;}
        // Every gap location checks no certificate, interval-only bounds and
        // all certificates actually established by immutable preparation.
        for(u32 mode=0;mode<3;++mode){auto* directory=mode?direct_lookups:lookups;program.lookups.data=mode==2?nullptr:directory;program.source=mode==2?&sparse:nullptr;
        for(u32 gate=0;gate<3;++gate){for(u32 i=0;i<program.lookups.count;++i)directory[i].bounds.ready=gate==2?prepared_bounds[i]:static_cast<u16>(gate);
        for(u32 split=0;split<=n;++split){copy_bytes(glyphs,input.data,input.count);SfntGlyphBuffer buffer{};const u32 capacity=maximum(n,expected_count)+8;
            if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),capacity*sizeof(SfntGsubGlyph)},sizeof(SfntGsubGlyph),n,&buffer)!=SfntStatus::ok || sfnt_glyph_buffer_seek(&buffer,split)!=SfntStatus::ok)return false;
            SfntGsubExecutionResult result{};const auto status=sfnt_gsub_execute_lookup(program,static_cast<u16>(index),&buffer,start,end,alt,scratch(),{1000000,100000},&result);
            const auto view=sfnt_glyph_buffer_view(buffer);bool pass=status==SfntStatus::ok && sfnt_glyph_count(view)==expected_count && result.input_end==expected_count;
            for(u32 i=0;i<minimum(expected_count,sfnt_glyph_count(view));++i){const u16 want=static_cast<u16>(expected.data[i*2]|(static_cast<u16>(expected.data[i*2+1])<<8));if(sfnt_glyph_at(view,i).glyph!=want)pass=false;++checked;}
            bool windows_match=sfnt_glyph_count(view)==windows_count;
            for(u32 i=0;i<minimum(windows_count,sfnt_glyph_count(view));++i){const u16 want=static_cast<u16>(windows.data[i*2]|(static_cast<u16>(windows.data[i*2+1])<<8));if(sfnt_glyph_at(view,i).glyph!=want)windows_match=false;}
            if(windows_match!=(kind==0))pass=false;if(!windows_match)++windows_differences;
            ++queries;edits+=result.edits;
            if(!pass){++failures;print(utf8_literal("case="));number(c);print(utf8_literal(" split="));number(split);print(utf8_literal(" status="));number(static_cast<u32>(status));print(utf8_literal(" actual="));for(u32 i=0;i<sfnt_glyph_count(view);++i){number(sfnt_glyph_at(view,i).glyph);print(utf8_literal(" "));}print(utf8_literal("\n"));}
            if(pass&&gate&&split==0&&directory[index].subtables.count>=16){const u64 budgets[]{result.operations-1,result.operations,result.operations+1};
                for(u64 budget:budgets){copy_bytes(glyphs,input.data,input.count);SfntGlyphBuffer limited{};
                    if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),capacity*sizeof(SfntGsubGlyph)},sizeof(SfntGsubGlyph),n,&limited)!=SfntStatus::ok||sfnt_glyph_buffer_seek(&limited,0)!=SfntStatus::ok)return false;
                    SfntGsubExecutionResult check{};const auto limited_status=sfnt_gsub_execute_lookup(program,static_cast<u16>(index),&limited,start,end,alt,scratch(),{budget,100000},&check);++budget_checks;
                    if(check.operations>budget||limited_status!=(budget<result.operations?SfntStatus::resource_limit:SfntStatus::ok))return false;
                }
            }
        }
        }
        }
    }return r.at==r.data.count;
}
bool file_corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size) || size.QuadPart<12 || size.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE map=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!map){CloseHandle(file);return false;}
    const auto* data=static_cast<const u8*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0));const bool ok=data && corpus({data,static_cast<u32>(size.QuadPart)});
    if(data)UnmapViewOfFile(data);CloseHandle(map);CloseHandle(file);return ok;
}
int run() noexcept {
    if(!units()){print(utf8_literal("FAIL execution units\n"));return 3;}
    if(!guards()){print(utf8_literal("FAIL execution guards\n"));return 4;}
    const wchar_t* command=GetCommandLineW();bool quote=false;while(*command){if(*command=='"')quote=!quote;else if(!quote && (*command==' ' || *command=='\t'))break;++command;}while(*command==' ' || *command=='\t')++command;
    bool ok=false;if(!*command)ok=corpus({gsub_execution_fixture,sizeof(gsub_execution_fixture)});
    else {wchar_t path[32768];u32 n=0;quote=*command=='"';if(quote)++command;while(*command && !(quote?*command=='"':(*command==' ' || *command=='\t'))){if(n+1>=32768)return 2;path[n++]=*command++;}if(quote){if(*command!='"')return 2;++command;}while(*command==' ' || *command=='\t')++command;if(*command || !n)return 2;path[n]=0;ok=file_corpus(path);}
    print(utf8_literal("queries="));number(queries);print(utf8_literal("\nglyph_checks="));number(checked);print(utf8_literal("\nedits="));number(edits);print(utf8_literal("\nexplicit_windows_differences="));number(windows_differences);print(utf8_literal("\nbudget_checks="));number(budget_checks);print(utf8_literal("\nguard_observations="));number(guard_observations);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\n"));
    print(utf8_literal("direct_entries="));number(direct_entries);print(utf8_literal("\ndirect_limits="));number(direct_limits);print(utf8_literal("\n"));
    print(utf8_literal("shared_certificates="));number(shared_certificates);print(utf8_literal("\n"));
    print(ok && !failures?utf8_literal("PASS native GSUB execution\n"):utf8_literal("FAIL native GSUB execution\n"));return ok && !failures?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_gsub_execute_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
