#include "maicord/text/gsub_execute.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
#include "generated/cluster_execute_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
u64 bitmap[9000],snapshot[9000];u32 expected_first[262145],expected_end[262145],boundaries[9000];
u64 model_queries=0,merge_plans=0,split_queries=0,split_operations=0,split_merges=0,execution_queries=0,glyph_checks=0,character_checks=0,start_checks=0,budget_checks=0;u32 failures=0;
bool model() noexcept {
    if(sfnt_cluster_map_words(0)!=0 || sfnt_cluster_map_words(64)!=1 || sfnt_cluster_map_words(65)!=3 || sfnt_cluster_map_words(0xffffffffu)!=68174085)return false;
    const u32 lengths[]{1,2,63,64,65,4095,4096,4097,262145};u32 random=0x869421f1;
    for(u32 count:lengths)for(u32 mode=0;mode<2;++mode){u32 starts=0;const u32 stride=mode?31u:1u;
        if(mode)for(u32 i=0;i<count;i+=stride)boundaries[starts++]=i;
        SfntClusterMap map{};if(sfnt_cluster_map_open(count,{boundaries,starts},{bitmap,9000},&map)!=SfntStatus::ok)return false;
        for(u32 i=0;i<count;++i){expected_first[i]=(i/stride)*stride;expected_end[i]=minimum(expected_first[i]+stride,count);}
        for(u32 pass=0;pass<129;++pass){
            const u32 probes=count<8192?count:1025;
            for(u32 q=0;q<probes;++q){const u32 at=count<8192?q:static_cast<u32>((static_cast<u64>(q)*(count-1))/(probes-1));u32 first=0,end=0;
                if(sfnt_cluster_map_find(map,at,&first,&end)!=SfntStatus::ok || first!=expected_first[at] || end!=expected_end[at]){print(utf8_literal("model_count="));number(count);print(utf8_literal(" pass="));number(pass);print(utf8_literal(" at="));number(at);print(utf8_literal("\n"));return false;}++model_queries;}
            if(pass==128)break;
            random^=random<<13;random^=random>>17;random^=random<<5;u32 first=random%count;
            random^=random<<13;random^=random>>17;random^=random<<5;u32 last=random%count;
            if(first>last){const u32 tmp=first;first=last;last=tmp;}
            if(pass<16){first=minimum(pass*63u,count-1);last=minimum(first+pass+1,count-1);}
            copy_bytes(snapshot,bitmap,map.storage.count*sizeof(u64));SfntClusterMerge plan{};u64 used=0;
            if(first!=last && (sfnt_cluster_map_plan(map,first,last,&plan,0,&used)!=SfntStatus::resource_limit || used || !bytes_equal(bitmap,snapshot,map.storage.count*sizeof(u64))))return false;
            if(sfnt_cluster_map_plan(map,first,last,&plan,1000000,&used)!=SfntStatus::ok || !bytes_equal(bitmap,snapshot,map.storage.count*sizeof(u64)))return false;
            sfnt_cluster_map_commit(&map,plan);++merge_plans;
            const u32 lo=expected_first[first],hi=expected_end[last];for(u32 i=lo;i<hi;++i){expected_first[i]=lo;expected_end[i]=hi;}
        }
        for(u32 i=0;i<count;++i){u32 first=0,end=0;if(sfnt_cluster_map_find(map,i,&first,&end)!=SfntStatus::ok || first!=expected_first[i] || end!=expected_end[i])return false;++model_queries;}
    }
    return true;
}
bool split_model() noexcept {
    const u32 lengths[]{1,2,63,64,65,4095,4096,4097,262145};u32 random=0x185371b9;
    for(u32 count:lengths)for(u32 mode=0;mode<2;++mode){const u32 zero=0;SfntClusterMap map{};
        if(sfnt_cluster_map_open(count,mode?ConstSlice<u32>{}:ConstSlice<u32>{&zero,1},{bitmap,9000},&map)!=SfntStatus::ok)return false;
        for(u32 i=0;i<count;++i){expected_first[i]=mode?i:0;expected_end[i]=mode?i+1:count;}
        for(u32 pass=0;pass<256;++pass){
            random^=random<<13;random^=random>>17;random^=random<<5;u32 at=random%count;
            if(pass<16)at=minimum(pass*63u,count-1);
            if(pass%4){copy_bytes(snapshot,bitmap,map.storage.count*sizeof(u64));
                if(sfnt_cluster_map_split(&map,at,map.levels-1)!=SfntStatus::resource_limit || !bytes_equal(bitmap,snapshot,map.storage.count*sizeof(u64)))return false;
                if(sfnt_cluster_map_split(&map,at,map.levels)!=SfntStatus::ok)return false;++split_operations;
                const u32 lo=expected_first[at],hi=expected_end[at];for(u32 i=lo;i<at;++i)expected_end[i]=at;for(u32 i=at;i<hi;++i)expected_first[i]=at;
            }else {random^=random<<13;random^=random>>17;random^=random<<5;u32 last=random%count;if(at>last){const u32 tmp=at;at=last;last=tmp;}
                SfntClusterMerge plan{};if(sfnt_cluster_map_plan(map,at,last,&plan)!=SfntStatus::ok)return false;sfnt_cluster_map_commit(&map,plan);++split_merges;
                const u32 lo=expected_first[at],hi=expected_end[last];for(u32 i=lo;i<hi;++i){expected_first[i]=lo;expected_end[i]=hi;}
            }
            const u32 probes=count<8192?count:1025;
            for(u32 q=0;q<probes+3;++q){const u32 source=q<probes?(count<8192?q:static_cast<u32>((static_cast<u64>(q)*(count-1))/(probes-1))):q==probes?at:q==probes+1?(at?at-1:0):minimum(at+1,count-1);u32 first=0,end=0;
                if(sfnt_cluster_map_find(map,source,&first,&end)!=SfntStatus::ok || first!=expected_first[source] || end!=expected_end[source])return false;++split_queries;}
        }
        for(u32 i=0;i<count;++i){u32 first=0,end=0;if(sfnt_cluster_map_find(map,i,&first,&end)!=SfntStatus::ok || first!=expected_first[i] || end!=expected_end[i])return false;++split_queries;}
    }
    return true;
}
bool units() noexcept {
    SfntClusterMap map{};u32 first=0,end=0;if(sfnt_cluster_map_open(0,{}, {},&map)!=SfntStatus::ok || map.levels || sfnt_cluster_map_find(map,0,&first,&end)!=SfntStatus::invalid_input)return false;
    const u32 starts[]{0,2};if(sfnt_cluster_map_open(4,{starts,2},{bitmap,9000},&map)!=SfntStatus::ok)return false;
    SfntClusterGlyph records[3]{{{1,SfntGlyphClass::base,0},2},{{2,SfntGlyphClass::base,0},0},{{3,SfntGlyphClass::base,0},1}};
    SfntGlyphView view{&records[0].glyph,nullptr,2,0,sizeof(SfntClusterGlyph)};u32 output[4]{};u8 flags[3]{};
    if(sfnt_cluster_map_export(map,view,{output,4},{flags,3})!=SfntStatus::ok || output[0]!=1 || output[1]!=1 || output[2]!=0 || output[3]!=0 || flags[0]!=1 || flags[1]!=1)return false;
    records[0].source=0;records[1].source=1;records[2].source=2;view.prefix_count=3;
    if(sfnt_cluster_map_export(map,view,{output,4},{flags,3})!=SfntStatus::ok || output[0]!=0 || output[1]!=0 || output[2]!=2 || output[3]!=2 || flags[0]!=1 || flags[1]!=0 || flags[2]!=1)return false;
    records[1].source=2;records[2].source=1;
    if(sfnt_cluster_map_export(map,view,{output,4},{flags,3})!=SfntStatus::ok || output[0]!=0 || output[1]!=0 || output[2]!=1 || output[3]!=1 || flags[0]!=1 || flags[1]!=1 || flags[2])return false;
    if(sfnt_cluster_map_export(map,view,{output,3})!=SfntStatus::output_too_small)return false;
    copy_bytes(snapshot,bitmap,map.storage.count*sizeof(u64));
    if(sfnt_cluster_map_split(nullptr,0)!=SfntStatus::invalid_input || sfnt_cluster_map_split(&map,4)!=SfntStatus::invalid_input || !bytes_equal(bitmap,snapshot,map.storage.count*sizeof(u64)))return false;
    // Measured AB ligature decomposition: X Y M N, character map [0,0,1,1].
    // First-occurrence bytes [1,1,0,0] deliberately do not claim to reproduce
    // Windows shaping flags [1,1,1,1] for this case.
    SfntClusterGlyph expanded[4]{{{1,SfntGlyphClass::base,0},0},{{2,SfntGlyphClass::base,0},2},{{3,SfntGlyphClass::mark,0},1},{{4,SfntGlyphClass::mark,0},3}},with_gap[7];u8 first_occurrence[4];
    SfntClusterMerge collapse{};if(sfnt_cluster_map_plan(map,0,3,&collapse)!=SfntStatus::ok)return false;sfnt_cluster_map_commit(&map,collapse);
    if(sfnt_cluster_map_find(map,3,&first,&end)!=SfntStatus::ok || first || end!=4 || sfnt_cluster_map_split(&map,2,map.levels)!=SfntStatus::ok)return false;
    for(u32 split=0;split<=4;++split){for(u32 i=0;i<7;++i)with_gap[i]={{0xffff,SfntGlyphClass::unassigned,0},0xffffffffu};
        for(u32 i=0;i<4;++i)with_gap[i<split?i:i+3]=expanded[i];
        SfntGlyphView separated{&with_gap[0].glyph,split<4?&with_gap[split+3].glyph:nullptr,split,4-split,sizeof(SfntClusterGlyph)};
        if(sfnt_cluster_map_export(map,separated,{output,4},{first_occurrence,4})!=SfntStatus::ok || output[0]!=0 || output[1]!=0 || output[2]!=1 || output[3]!=1 || first_occurrence[0]!=1 || first_occurrence[1]!=1 || first_occurrence[2] || first_occurrence[3])return false;
    }
    SfntGlyphView missing{&expanded[0].glyph,nullptr,1,0,sizeof(SfntClusterGlyph)};
    if(sfnt_cluster_map_export(map,missing,{output,4})!=SfntStatus::invalid_input)return false;
    expanded[0].source=4;if(sfnt_cluster_map_export(map,missing,{output,4})!=SfntStatus::invalid_input)return false;
    SfntClusterMerge plan{};if(sfnt_cluster_map_plan(map,3,2,&plan)!=SfntStatus::invalid_input || sfnt_cluster_map_plan(map,0,4,&plan)!=SfntStatus::invalid_input)return false;
    if(sfnt_cluster_map_open(65,{}, {bitmap,2},&map)!=SfntStatus::output_too_small || map.levels)return false;
    const u32 invalid[]{1,2};if(sfnt_cluster_map_open(4,{invalid,2},{bitmap,9000},&map)!=SfntStatus::invalid_input)return false;
    return sfnt_cluster_map_split(&map,0)==SfntStatus::invalid_input;
}
bool guards() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*17,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;
    DWORD old=0;if(!VirtualProtect(memory+page*16,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}bool ok=true;
    const u32 lengths[]{1,63,64,65,4096,4097,262145};
    for(u32 count:lengths){const u32 words=sfnt_cluster_map_words(count);if(static_cast<u64>(words)*8>page*16){ok=false;break;}
        auto* storage=reinterpret_cast<u64*>(memory+page*16-words*8);SfntClusterMap map{};if(sfnt_cluster_map_open(count,{}, {storage,words},&map)!=SfntStatus::ok){ok=false;break;}
        for(u32 pass=0;pass<12;++pass){SfntClusterMerge plan{};u64 used=0;if(sfnt_cluster_map_plan(map,0,count-1,&plan,1000000,&used)!=SfntStatus::ok){ok=false;break;}
            if(pass && count>1 && (used!=map.levels*2+2 || plan.commit_work!=used)){ok=false;break;}sfnt_cluster_map_commit(&map,plan);
            u32 first=0,end=0;if(sfnt_cluster_map_find(map,count-1,&first,&end)!=SfntStatus::ok || first || end!=count){ok=false;break;}}
        if(!ok)break;
        const u32 cuts[]{1,63,64,65,4095,4096,4097,count-1};
        for(u32 cut:cuts)if(cut<count){if(sfnt_cluster_map_split(&map,cut,map.levels)!=SfntStatus::ok){ok=false;break;}}
        for(u32 i=0;i<count && ok;++i){u32 lo=0,hi=count,first=0,end=0;for(u32 cut:cuts)if(cut<count){if(cut<=i)lo=maximum(lo,cut);else hi=minimum(hi,cut);}
            if(sfnt_cluster_map_find(map,i,&first,&end)!=SfntStatus::ok || first!=lo || end!=hi)ok=false;}
        if(!ok)break;
    }
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
struct Reader {ConstSlice<u8> data;u32 at=0;
    bool word(u32* v) noexcept {if(at>data.count || data.count-at<4)return false;*v=byte_load_le32(data.data+at);at+=4;return true;}
    bool take(u32 n,ConstSlice<u8>* v) noexcept {if(at>data.count || n>data.count-at)return false;*v={data.data+at,n};at+=n;return true;}
};
SfntGsubExecutableLookup lookups[128];SfntGsubExecutableSubtable subtables[256];SfntCoverage coverages[1024];SfntClassDef classes[128];
SfntGsubExecutionFrame frames[32];u32 positions[1024],rebased[1024],consumed[256],character_glyphs[1024];u8 glyph_starts[1024];SfntClusterGlyph glyphs[1024],replacements[256];
SfntGsubExecutionScratch scratch() noexcept {return {{frames,32},{positions,1024},{rebased,1024},{consumed,256},{reinterpret_cast<u8*>(replacements),sizeof(replacements)}};}
bool corpus(ConstSlice<u8> bytes) noexcept {
    if(bytes.count<12 || !bytes_equal(bytes.data,"MCGCL01\0",8))return false;Reader r{bytes,8};u32 cases=0;if(!r.word(&cases) || !cases || cases>100000)return false;
    for(u32 c=0;c<cases;++c){u32 f[13]{};for(u32& x:f)if(!r.word(&x))return false;
        if(!f[0] || f[0]>65536 || f[3]>512 || f[8]>512 || f[9]>1 || f[10]>512 || f[6]>65535 || !f[11] || f[11]>1024 || f[12]>1024)return false;
        ConstSlice<u8> gs{},gd{},input{},expected{},windows{},seed{},clusters{},flags{};
        if(!r.take(f[1],&gs)||!r.take(f[2],&gd)||!r.take(f[3]*8,&input)||!r.take(f[8]*2,&expected)||!r.take(f[10]*2,&windows)||!r.take(f[12]*4,&seed)||!r.take(f[11]*4,&clusters)||!r.take(f[10],&flags))return false;
        for(u32 i=0;i<f[12];++i)boundaries[i]=byte_load_le32(seed.data+i*4);
        SfntLayout layout{};SfntGdef gdef{};SfntGsubProgram program{};
        if(sfnt_layout_open(gs,SfntLayoutKind::substitution,&layout)!=SfntStatus::ok || sfnt_gdef_open(gd,f[0],&gdef)!=SfntStatus::ok || sfnt_gsub_program_prepare(layout,gdef,f[0],{{lookups,128},{subtables,256},{coverages,1024},{classes,128}},&program)!=SfntStatus::ok)return false;
        if(c==2){if(program.lookups.data[0].type!=4)return false;u64 complete_work=0;
            for(u64 budget=1000000;;){copy_bytes(glyphs,input.data,input.count);SfntGlyphBuffer buffer{};SfntClusterMap map{};
                if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),sizeof(glyphs)},sizeof(SfntClusterGlyph),f[3],&buffer)!=SfntStatus::ok || sfnt_cluster_map_open(f[11],{boundaries,f[12]},{bitmap,9000},&map)!=SfntStatus::ok)return false;
                copy_bytes(snapshot,bitmap,map.storage.count*sizeof(u64));SfntGsubExecutionResult result{};
                const auto status=sfnt_gsub_execute_lookup(program,0,&buffer,0,f[3],0,scratch(),{budget,100000},&result,&map);
                if(result.operations>budget)return false;
                if(!result.edits){if(!bytes_equal(bitmap,snapshot,map.storage.count*sizeof(u64)) || !bytes_equal(glyphs,input.data,input.count) || sfnt_glyph_count(sfnt_glyph_buffer_view(buffer))!=f[3])return false;}
                else {if(result.edits!=1 || sfnt_cluster_map_export(map,sfnt_glyph_buffer_view(buffer),{character_glyphs,1024})!=SfntStatus::ok)return false;
                    for(u32 i=0;i<f[11];++i)if(character_glyphs[i]!=byte_load_le32(clusters.data+i*4))return false;}
                if(budget==1000000){if(status!=SfntStatus::ok || !result.operations || result.operations>10000)return false;complete_work=result.operations;budget=0;continue;}
                ++budget_checks;if(status!=(budget<complete_work?SfntStatus::resource_limit:SfntStatus::ok))return false;if(budget==complete_work)break;++budget;
            }
        }
        for(u32 split=0;split<=f[3];++split){copy_bytes(glyphs,input.data,input.count);SfntGlyphBuffer buffer{};SfntClusterMap map{};
            if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),(maximum(f[3],f[8])+8)*sizeof(SfntClusterGlyph)},sizeof(SfntClusterGlyph),f[3],&buffer)!=SfntStatus::ok || sfnt_glyph_buffer_seek(&buffer,split)!=SfntStatus::ok || sfnt_cluster_map_open(f[11],{boundaries,f[12]},{bitmap,9000},&map)!=SfntStatus::ok)return false;
            SfntGsubExecutionResult result{};const auto status=sfnt_gsub_execute_lookup(program,static_cast<u16>(f[6]),&buffer,f[4],f[5],f[7],scratch(),{1000000,100000},&result,&map);
            const auto view=sfnt_glyph_buffer_view(buffer);bool pass=status==SfntStatus::ok && sfnt_glyph_count(view)==f[8] && result.input_end==f[8];
            const auto exported=sfnt_cluster_map_export(map,view,{character_glyphs,1024},{glyph_starts,1024});if(exported!=SfntStatus::ok)pass=false;
            for(u32 i=0;i<minimum(f[8],sfnt_glyph_count(view));++i){const u16 wanted=static_cast<u16>(expected.data[i*2]|(static_cast<u16>(expected.data[i*2+1])<<8));if(sfnt_glyph_at(view,i).glyph!=wanted)pass=false;++glyph_checks;}
            for(u32 i=0;i<f[11];++i){if(character_glyphs[i]!=byte_load_le32(clusters.data+i*4))pass=false;++character_checks;}
            for(u32 i=0;i<minimum(f[10],sfnt_glyph_count(view));++i){if(glyph_starts[i]!=flags.data[i])pass=false;++start_checks;}
            ++execution_queries;if(!pass){++failures;print(utf8_literal("case="));number(c);print(utf8_literal(" split="));number(split);print(utf8_literal(" status="));number(static_cast<u32>(status));print(utf8_literal(" export="));number(static_cast<u32>(exported));print(utf8_literal(" clusters="));for(u32 i=0;i<f[11];++i){number(character_glyphs[i]);print(utf8_literal(" "));}print(utf8_literal("\n"));}
        }
    }return r.at==r.data.count;
}
bool file_corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size) || size.QuadPart<12 || size.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping){CloseHandle(file);return false;}
    const auto* data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));const bool ok=data && corpus({data,static_cast<u32>(size.QuadPart)});if(data)UnmapViewOfFile(data);CloseHandle(mapping);CloseHandle(file);return ok;
}
int run() noexcept {
    if(!units()){print(utf8_literal("FAIL cluster units\n"));return 3;}if(!model()){print(utf8_literal("FAIL cluster model\n"));return 4;}if(!split_model()){print(utf8_literal("FAIL cluster split model\n"));return 6;}if(!guards()){print(utf8_literal("FAIL cluster guards\n"));return 5;}
    const wchar_t* command=GetCommandLineW();bool quote=false;while(*command){if(*command=='"')quote=!quote;else if(!quote && (*command==' ' || *command=='\t'))break;++command;}while(*command==' ' || *command=='\t')++command;
    bool ok=false;if(!*command)ok=corpus({cluster_execution_fixture,sizeof(cluster_execution_fixture)});
    else {wchar_t path[32768];u32 n=0;quote=*command=='"';if(quote)++command;while(*command && !(quote?*command=='"':(*command==' ' || *command=='\t'))){if(n+1>=32768)return 2;path[n++]=*command++;}if(quote){if(*command!='"')return 2;++command;}while(*command==' ' || *command=='\t')++command;if(*command || !n)return 2;path[n]=0;ok=file_corpus(path);}
    print(utf8_literal("model_queries="));number(model_queries);print(utf8_literal("\nmerge_plans="));number(merge_plans);print(utf8_literal("\nexecution_queries="));number(execution_queries);print(utf8_literal("\nglyph_checks="));number(glyph_checks);print(utf8_literal("\ncharacter_cluster_checks="));number(character_checks);print(utf8_literal("\ncluster_start_checks="));number(start_checks);print(utf8_literal("\nbudget_checks="));number(budget_checks);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\n"));
    print(utf8_literal("split_queries="));number(split_queries);print(utf8_literal("\nsplit_operations="));number(split_operations);print(utf8_literal("\nsplit_merges="));number(split_merges);print(utf8_literal("\n"));
    print(ok && !failures?utf8_literal("PASS native cluster updates\n"):utf8_literal("FAIL native cluster updates\n"));return ok && !failures?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_clusters_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
