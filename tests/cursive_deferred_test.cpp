// Manual: --benchmark times the deferred cursive attachment; the registered run is the check.
#include "maicord/text/gpos_program.hpp"
namespace maicord {namespace {
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
constexpr u32 capacity=4096;
SfntGsubGlyph glyphs[capacity];double nominal[capacity];u8 flags[capacity];
SfntPosition eager[capacity],coalesced[capacity];SfntCursiveChainNode eager_nodes[capacity],coalesced_nodes[capacity];
SfntGposAttachment attachment;SfntGposAttachmentLookup lookup;
u64 comparisons=0,budget_checks=0,long_eager_work=0,long_deferred_work=0,program_work=0;u32 guards=0;
const u8 cursive_bytes[]{0,1,0,10,0,1,0,16,0,22,0,1,0,1,0,1,0,1,0,10,0,20,0,1,2,18,0,90};
SfntGlyphView view(u32 n) noexcept {SfntGlyphBuffer buffer{};sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),n*sizeof(SfntGsubGlyph)},sizeof(SfntGsubGlyph),n,&buffer);return sfnt_glyph_buffer_view(buffer);}
void reset(u32 n,u32 kind=0) noexcept {
    for(u32 i=0;i<n;++i){glyphs[i]={1,SfntGlyphClass::base,0};nominal[i]=600;flags[i]=2;
        double y=kind==1?static_cast<double>(i%7)/8:kind==2?1099511627776.0+i:kind==3?-1099511627776.0+i:kind==4?static_cast<double>(i%7)-3:kind==5 && i==n/2?1e16:kind==6 && i==n/2?0.1:0;
        eager[i]=coalesced[i]={600,0,y};eager_nodes[i]=coalesced_nodes[i]={0xffffffffu,0xffffffffu};}
}
SfntStatus sequence(u32 n,bool deferred,bool rtl,u64 budget,u64* used) noexcept {
    SfntCursiveDeferred pending{};auto* state=deferred?coalesced:eager;auto* nodes=deferred?coalesced_nodes:eager_nodes;u64 work=budget;SfntStatus status=SfntStatus::ok;
    for(u32 i=0;i+1<n;++i){SfntCursiveExecution r{};status=sfnt_position_execute_cursive_selected(lookup,{},view(n),{nominal,n},{state,n},i,n,rtl,{}, {nodes,n},&r,work,nullptr,nullptr,deferred && n>=32?&pending:nullptr);if(r.operations>work)return SfntStatus::malformed;work-=r.operations;if(status!=SfntStatus::ok)break;}
    if(status==SfntStatus::ok && pending.head!=0xffffffffu){SfntCursiveExecution r{};status=sfnt_position_cursive_flush({state,n},{nodes,n},&pending,&r,work);if(r.operations>work)return SfntStatus::malformed;work-=r.operations;}
    *used=budget-work;return status;
}
bool compare(u32 n) noexcept {
    comparisons+=static_cast<u64>(n)*3;return bytes_equal(eager,coalesced,n*sizeof(SfntPosition)) && bytes_equal(eager_nodes,coalesced_nodes,n*sizeof(SfntCursiveChainNode));
}
bool monotonic() noexcept {
    constexpr u32 lengths[]{8,64,256,1024,4096};
    for(u32 kind=0;kind<7;++kind)for(u32 rtl=0;rtl<2;++rtl)for(u32 n:lengths){
        reset(n,kind);u64 a=0,b=0;if(sequence(n,false,rtl!=0,20000000,&a)!=SfntStatus::ok || sequence(n,true,rtl!=0,20000000,&b)!=SfntStatus::ok || !compare(n))return false;
        if(!kind){for(u32 i=0;i<n;++i)if(coalesced[i].ascender_offset!=-70.0*(n-1-i))return false;
            if(b>static_cast<u64>(n)*8)return false;if(n==4096 && !rtl){long_eager_work=a;long_deferred_work=b;}}
    }
    reset(64);u64 total=0;if(sequence(64,true,false,100000,&total)!=SfntStatus::ok)return false;
    for(u64 budget=0;budget<=total;++budget){reset(64);u64 used=0;const auto status=sequence(64,true,false,budget,&used);++budget_checks;if(status!=(budget<total?SfntStatus::resource_limit:SfntStatus::ok) || used>budget)return false;}
    return true;
}
bool interleaved() noexcept {
    u32 random=0x8612ef13;
    for(u32 trial=0;trial<128;++trial){const u32 n=63;reset(n,trial%5);SfntCursiveDeferred pending{};
        for(u32 action=0;action<128;++action){random^=random<<13;random^=random>>17;random^=random<<5;const u32 at=random%(n-1);SfntCursiveExecution r{};
            if((random&15)==0){if(sfnt_position_cursive_flush({coalesced,n},{coalesced_nodes,n},&pending,&r)!=SfntStatus::ok)return false;const double delta=(random&16)?0.125:17.0;eager[at].ascender_offset+=delta;coalesced[at].ascender_offset+=delta;}
            else {lookup.filter.flags=(random&32)?1:0;
                if(sfnt_position_execute_cursive_selected(lookup,{},view(n),{nominal,n},{eager,n},at,n,(trial&1)!=0,{}, {eager_nodes,n},&r)!=SfntStatus::ok || sfnt_position_execute_cursive_selected(lookup,{},view(n),{nominal,n},{coalesced,n},at,n,(trial&1)!=0,{}, {coalesced_nodes,n},&r,1000000,nullptr,nullptr,&pending)!=SfntStatus::ok)return false;}
        }
        SfntCursiveExecution r{};if(sfnt_position_cursive_flush({coalesced,n},{coalesced_nodes,n},&pending,&r)!=SfntStatus::ok || !compare(n))return false;
    }
    lookup.filter.flags=1;return true;
}
void be16(u8* data,u32 value) noexcept {data[0]=static_cast<u8>(value>>8);data[1]=static_cast<u8>(value);}
bool program() noexcept {
    constexpr u32 n=4096;static u8 bytes[6*n+8];static SfntCoverage coverages[n];static u32 matched[n];SfntContext context{};
    be16(bytes,3);be16(bytes+2,n);be16(bytes+4,n-1);const u32 coverage=6*n+2;
    for(u32 i=0;i<n;++i)be16(bytes+6+i*2,coverage);
    for(u32 i=0;i+1<n;++i){be16(bytes+6+2*n+i*4,i);be16(bytes+8+2*n+i*4,1);}
    be16(bytes+coverage,1);be16(bytes+coverage+2,1);be16(bytes+coverage+4,1);
    if(sfnt_context_open({bytes,sizeof(bytes)},SfntContextKind::sequence,2,2,{coverages,n},{},&context)!=SfntStatus::ok)return false;
    SfntGposExecutableLookup lookups[2]{};lookups[0].type=7;lookups[0].tables.context={&context,1};lookups[1].type=3;lookups[1].tables.attachment={&attachment,1};lookups[1].filter.flags=1;
    const SfntGposProgram prepared{{lookups,2},{},2};SfntGposExecutionFrame frame{};SfntGposExecutionResult result{};reset(n);
    const auto status=sfnt_gpos_execute_program_lookup(prepared,0,view(n),{nominal,n},{flags,n},{},{},{coalesced,n},0,n,false,{},SfntNullAnchorPolicy::skip,SfntComponentAnchorPolicy::strict,{{&frame,1},{matched,n},{},{},{coalesced_nodes,n}},{1000000,100000},&result);
    program_work=result.operations;if(status!=SfntStatus::ok || result.actions!=n-1 || result.max_depth!=1 || program_work>static_cast<u64>(n)*20)return false;
    for(u32 i=0;i<n;++i)if(coalesced[i].ascender_offset!=-70.0*(n-1-i))return false;
    return true;
}
bool guarded() noexcept {
    constexpr u32 n=63;reset(n);SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,4*page,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;DWORD old=0;
    bool ok=VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)!=0 && VirtualProtect(memory+3*page,page,PAGE_NOACCESS,&old)!=0;
    if(ok){auto* state=reinterpret_cast<SfntPosition*>(memory+page-n*sizeof(SfntPosition));auto* nodes=reinterpret_cast<SfntCursiveChainNode*>(memory+3*page-n*sizeof(SfntCursiveChainNode));copy_bytes(state,eager,n*sizeof(SfntPosition));copy_bytes(nodes,eager_nodes,n*sizeof(SfntCursiveChainNode));SfntCursiveDeferred pending{};
        for(u32 i=0;i+1<n && ok;++i){SfntCursiveExecution r{};ok=sfnt_position_execute_cursive_selected(lookup,{},view(n),{nominal,n},{state,n},i,n,false,{}, {nodes,n},&r,1000000,nullptr,nullptr,&pending)==SfntStatus::ok;}
        SfntCursiveExecution r{};if(ok)ok=sfnt_position_cursive_flush({state,n},{nodes,n},&pending,&r)==SfntStatus::ok;
        for(u32 i=0;i<n && ok;++i)ok=state[i].ascender_offset==-70.0*(n-1-i);++guards;}
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
struct Callback {SfntPosition* state;const SfntPosition* expected;u32 calls;bool valid;};
SfntStatus point_callback(void* opaque,u16 glyph,u16 point,double* x,double* y) noexcept {
    auto& c=*static_cast<Callback*>(opaque);++c.calls;c.valid=c.valid && glyph==1 && point<2 && bytes_equal(c.state,c.expected,64*sizeof(SfntPosition));*x=point?530:10;*y=point?90:20;return SfntStatus::ok;
}
bool callbacks() noexcept {
    constexpr u32 n=64;reset(n);SfntCursiveDeferred pending{};SfntCursiveExecution r{};
    for(u32 i=0;i<16;++i)if(sfnt_position_execute_cursive_selected(lookup,{},view(n),{nominal,n},{eager,n},i,n,false,{}, {eager_nodes,n},&r)!=SfntStatus::ok || sfnt_position_execute_cursive_selected(lookup,{},view(n),{nominal,n},{coalesced,n},i,n,false,{}, {coalesced_nodes,n},&r,1000000,nullptr,nullptr,&pending)!=SfntStatus::ok)return false;
    if(pending.head==0xffffffffu)return false;const u8 point_bytes[]{0,1,0,10,0,1,0,16,0,24,0,1,0,1,0,1,0,2,0,10,0,20,0,0,0,2,2,18,0,90,0,1};SfntGposAttachment point_sub{};
    if(sfnt_gpos_attachment_open({point_bytes,sizeof(point_bytes)},3,2,&point_sub)!=SfntStatus::ok)return false;SfntGposAttachmentLookup point_lookup{{&point_sub,1},lookup.filter};SfntPosition expected[n];copy_bytes(expected,eager,sizeof(expected));Callback a{eager,expected,0,true},b{coalesced,expected,0,true};
    if(sfnt_position_execute_cursive_selected(point_lookup,{},view(n),{nominal,n},{eager,n},16,n,false,{}, {eager_nodes,n},&r,1000000,point_callback,&a)!=SfntStatus::ok || sfnt_position_execute_cursive_selected(point_lookup,{},view(n),{nominal,n},{coalesced,n},16,n,false,{}, {coalesced_nodes,n},&r,1000000,point_callback,&b,&pending)!=SfntStatus::ok)return false;
    return a.valid && b.valid && a.calls==2 && b.calls==2 && pending.head==0xffffffffu && compare(n);
}
volatile u64 benchmark_witness=0;
bool benchmark() noexcept {
    LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency))return false;constexpr u32 lengths[]{8,512,4096};constexpr u32 samples=31;
    for(u32 n:lengths){u64 durations[2][samples]{};const u32 repeats=n==8?256:n==512?8:1;
        for(u32 sample=0;sample<samples+2;++sample)for(u32 order=0;order<2;++order){const bool deferred=((sample+order)&1)!=0;LARGE_INTEGER first{},last{};QueryPerformanceCounter(&first);
            for(u32 repeat=0;repeat<repeats;++repeat){reset(n);u64 used=0;if(sequence(n,deferred,false,20000000,&used)!=SfntStatus::ok)return false;
                const auto* state=deferred?coalesced:eager;u64 bits=0;copy_bytes(&bits,&state[0].ascender_offset,8);benchmark_witness=benchmark_witness^bits^used;}
            QueryPerformanceCounter(&last);if(sample>=2)durations[deferred?1:0][sample-2]=static_cast<u64>(last.QuadPart-first.QuadPart)*1000000000ull/static_cast<u64>(frequency.QuadPart)/repeats;
        }
        for(u32 mode=0;mode<2;++mode){auto* values=durations[mode];for(u32 i=1;i<samples;++i){const u64 value=values[i];u32 at=i;while(at && value<values[at-1]){values[at]=values[at-1];--at;}values[at]=value;}
            print(utf8_literal("n="));number(n);print(utf8_literal(" deferred="));number(mode);print(utf8_literal(" median_ns="));number(values[samples/2]);print(utf8_literal(" p95_ns="));number(values[29]);print(utf8_literal(" min_ns="));number(values[0]);print(utf8_literal("\n"));}
    }
    return true;
}
int run() noexcept {
    if(sfnt_gpos_attachment_open({cursive_bytes,sizeof(cursive_bytes)},3,2,&attachment)!=SfntStatus::ok)return 2;lookup={{&attachment,1},{}};lookup.filter.flags=1;
    const wchar_t* command=GetCommandLineW();constexpr wchar_t option[]=L"--benchmark";for(;*command;++command){u32 i=0;while(option[i] && command[i]==option[i])++i;if(!option[i])return benchmark()?0:7;}
    if(!monotonic()){print(utf8_literal("FAIL monotonic\n"));return 3;}if(!interleaved()){print(utf8_literal("FAIL interleaved\n"));return 4;}if(!program()){print(utf8_literal("FAIL program\n"));return 5;}if(!guarded()){print(utf8_literal("FAIL guarded\n"));return 6;}if(!callbacks()){print(utf8_literal("FAIL callbacks\n"));return 8;}
    print(utf8_literal("comparisons="));number(comparisons);print(utf8_literal("\nbudget_checks="));number(budget_checks);print(utf8_literal("\nlong_eager_work="));number(long_eager_work);print(utf8_literal("\nlong_deferred_work="));number(long_deferred_work);print(utf8_literal("\nprogram_work="));number(program_work);print(utf8_literal("\nguards="));number(guards);print(utf8_literal("\nPASS deferred cursive\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_cursive_deferred_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
