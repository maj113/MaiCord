#include "maicord/text/latin_run.hpp"
#include "maicord/text/alphabetic_run.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
char output[65536];u32 used=0;bool print_ok=true;u32 checks=0,cases=0;
void flush() noexcept {DWORD written=0;if(used&&(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),output,used,&written,nullptr)||written!=used))print_ok=false;used=0;}
void print(const char* s) noexcept {while(*s){if(used==sizeof(output))flush();output[used++]=*s++;}}
void number(u64 value) noexcept {char s[24];u32 n=0;do{s[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(n){if(used==sizeof(output))flush();output[used++]=s[--n];}}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print(" case=");number(cases);print("\n");return false;}}while(0)
NativeFontFileSlot files[1];NativeFontFaceSlot faces[1];NativeFontInstanceSlot instances[1];NativeRunPlanSlot plans[1];
NativeFontStore fonts{{files,1},{faces,1}};NativeFontInstanceStore instances_store{&fonts,{instances,1}};NativeRunPlanStore store{&instances_store,{plans,1}};
Arena scratch{};NativeRunPlanHandle plan{};wchar_t retained_path[32768]{};u32 retained_units=0,retained_weight=0;
Arena profile_state{};NativeAlphabeticPlan profiles[6][2]{};
template<class T> Slice<T> take(u32 count) noexcept {return {arena_allocate<T>(&scratch,count),count};}
NativeAsciiStorage storage() noexcept {
    NativeAsciiStorage s{};s.mapped=take<SfntClusterGlyph>(256);s.glyphs=take<u16>(1024);s.placements=take<SfntPlacement>(1024);s.character_glyphs=take<u32>(256);
    auto& q=s.shaping;q.glyphs=take<SfntComponentGlyph>(1024);q.components=take<SfntComponentNode>(256);q.cluster_bits=take<u64>(8);q.character_glyphs=take<u32>(256);q.targets=take<u32>(1024);
    q.associations=take<SfntMarkAssociation>(1024);q.advances=take<double>(1024);q.flags=take<u8>(1024);q.positions=take<SfntPosition>(1024);q.placements=take<SfntPlacement>(1024);
    q.substitutions={take<SfntGsubExecutionFrame>(64),take<u32>(4096),take<u32>(4096),take<u32>(1024),take<u8>(1024*sizeof(SfntComponentGlyph)),take<SfntComponentUndo>(4096)};
    q.positioning={take<SfntGposExecutionFrame>(64),take<u32>(4096),take<u64>(16),take<double>(1024),take<SfntCursiveChainNode>(1024)};return s;
}
bool acquire(const wchar_t* path,u32 units,u32 weight) noexcept {
    if(plan.slot&&units==retained_units&&weight==retained_weight&&bytes_equal(path,retained_path,units*2))return true;
    if(plan.slot){CHECK(native_run_plan_release(&store,plan));plan={};}
    if(profile_state.base)arena_reset(&profile_state);for(auto& group:profiles)for(auto& profile:group)profile={};
    NativeFontHandle face{};NativeFontInstanceHandle instance{};AxisSegmentMap maps[16];double coordinates[16];
    CHECK(native_font_acquire(&fonts,path,0,&face).ok());
    NativeFontPrepareScratch preparation{{maps,16},{coordinates,16},{},{}};FontAxisSetting axis{sfnt_tag('w','g','h','t'),static_cast<double>(weight)};
    CHECK(native_font_instance_acquire(&instances_store,face,weight?ConstSlice<FontAxisSetting>{&axis,1}:ConstSlice<FontAxisSetting>{},false,preparation,&instance).ok());
    CHECK(native_run_plan_acquire(&store,instance,&scratch,&plan).ok());
    CHECK(native_font_instance_release(&instances_store,instance)&&native_font_release(&fonts,face));
    copy_bytes(retained_path,path,units*2);retained_units=units;retained_weight=weight;return true;
}
NativeAsciiStorage shared(NativeAsciiStorage s) noexcept {
    s.mapped={reinterpret_cast<SfntClusterGlyph*>(s.shaping.positions.data),256};
    s.glyphs={reinterpret_cast<u16*>(s.shaping.glyphs.data),1024};s.placements=s.shaping.placements;s.character_glyphs=s.shaping.character_glyphs;return s;
}
SfntStatus shape(Utf8View text,u32 script,u32 mode,float em,NativeAsciiStorage s,NativeAsciiResult* result) noexcept {
    const bool independent=(mode&1)!=0,force=(mode&2)!=0;
    if(!script)return native_latin_run(store,plan,text,independent,em,s,result,{},false,force);
    const u32 index=script==sfnt_tag('l','a','t','n')?0u:script==sfnt_tag('g','r','e','k')?1u:script==sfnt_tag('c','y','r','l')?2u:script==sfnt_tag('h','e','b','r')?3u:script==sfnt_tag('h','a','n','i')?4u:5u;
    auto& profile=profiles[index][independent?1:0];
    if(!profile.script_tag){const auto status=native_alphabetic_plan_prepare(store,plan,script,independent,&profile_state,&scratch,&profile);if(status!=SfntStatus::ok)return status;}
    return native_alphabetic_run(store,profile,text,em,s,result,{},false,force,(mode&4)!=0);
}
#include "latin_run_guards.inc"
bool run() noexcept {return guards();}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_latin_run_entry() noexcept {using namespace maicord;const bool ok=run();print("cases=");number(cases);print(" checks=");number(checks);print(ok?" PASS native Latin runs\n":" FAIL native Latin runs\n");flush();ExitProcess(ok&&print_ok?0:1);}
