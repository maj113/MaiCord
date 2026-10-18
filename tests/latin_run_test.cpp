#include "maicord/text/latin_run.hpp"
#include "maicord/text/alphabetic_run.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
char output[65536];u32 used=0;bool print_ok=true;u32 checks=0,cases=0,differences=0;
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
struct Reader {ConstSlice<u8> bytes;u32 at;bool word(u32* v) noexcept {if(bytes.count-at<4)return false;*v=byte_load_le32(bytes.data+at);at+=4;return true;}
    bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>bytes.count-at)return false;*v={bytes.data+at,n};at+=n;return true;}};
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
bool run() noexcept {
    const wchar_t* p=GetCommandLineW();bool quoted=false;while(*p){if(*p=='"')quoted=!quoted;else if(!quoted&&(*p==' '||*p=='\t'))break;++p;}while(*p==' '||*p=='\t')++p;
    if(!*p)return guards();
    wchar_t path[32768];u32 n=0;quoted=*p=='"';if(quoted)++p;while(*p&&!(quoted?*p=='"':(*p==' '||*p=='\t'))){CHECK(n<32767);path[n++]=*p++;}path[n]=0;CHECK(n);
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};CHECK(GetFileSizeEx(file,&size)&&size.QuadPart>=12&&size.QuadPart<=128*1024*1024);
    auto* data=static_cast<u8*>(VirtualAlloc(nullptr,static_cast<usize>(size.QuadPart),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(data);
    DWORD actual=0;const bool read=ReadFile(file,data,static_cast<DWORD>(size.QuadPart),&actual,nullptr)!=0;CloseHandle(file);CHECK(read&&actual==size.QuadPart);
    struct Release {u8* data;~Release() noexcept {if(plan.slot){native_run_plan_release(&store,plan);plan={};}arena_release(&profile_state);arena_release(&scratch);VirtualFree(data,0,MEM_RELEASE);}} release{data};
    CHECK(arena_initialize(&scratch,4*1024*1024,0,4096)&&arena_initialize(&profile_state,1024*1024,0,4096));Reader reader{{data,actual},8};const bool scripts=bytes_equal(data,"MCSCRIP1",8);CHECK(scripts||bytes_equal(data,"MCLATIN1",8));u32 rows=0;CHECK(reader.word(&rows));
    for(u32 row=0;row<rows;++row){++cases;u32 path_units=0,text_bytes=0,em_bits=0,independent=0,weight=0,glyph_count=0,source_units=0;ConstSlice<u8> path_bytes{},text{},glyphs{},clusters{},placements{};
        CHECK(reader.word(&path_units)&&path_units<32768&&reader.take(path_units*2,&path_bytes));for(u32 i=0;i<path_units;++i)path[i]=byte_load_le16(path_bytes.data+i*2);path[path_units]=0;
        u32 script=0;CHECK(reader.word(&text_bytes)&&reader.take(text_bytes,&text)&&reader.word(&em_bits)&&reader.word(&independent)&&reader.word(&weight)&&(!scripts||reader.word(&script))&&reader.word(&glyph_count)&&reader.word(&source_units));
        CHECK(glyph_count<=1024&&source_units<=256&&reader.take(glyph_count*2,&glyphs)&&reader.take(source_units*4,&clusters)&&reader.take(glyph_count*12,&placements));
        CHECK(acquire(path,path_units,weight));arena_reset(&scratch);const auto s=storage();float em=0;copy_bytes(&em,&em_bits,4);NativeAsciiResult result{};
        const auto status=shape({reinterpret_cast<const char*>(text.data),text.count},script,independent,em,s,&result);
        bool same=status==SfntStatus::ok&&result.glyph_count==glyph_count&&result.source_units==source_units;
        same=same&&bytes_equal(s.glyphs.data,glyphs.data,glyphs.count)&&bytes_equal(s.character_glyphs.data,clusters.data,clusters.count)&&bytes_equal(s.placements.data,placements.data,placements.count);
        const auto alias=shared(storage());NativeAsciiResult aliased{};
        CHECK(shape({reinterpret_cast<const char*>(text.data),text.count},script,independent,em,alias,&aliased)==status);
        CHECK(bytes_equal(&result,&aliased,sizeof(result)));
        CHECK(bytes_equal(s.glyphs.data,alias.glyphs.data,result.glyph_count*sizeof(u16)));
        CHECK(bytes_equal(s.character_glyphs.data,alias.character_glyphs.data,result.source_units*sizeof(u32)));
        CHECK(bytes_equal(s.placements.data,alias.placements.data,result.glyph_count*sizeof(SfntPlacement)));
        if(!same){++differences;if(differences<=128){print("{\"case\":");number(row);print(",\"status\":");number(static_cast<u32>(status));print(",\"native_glyphs\":[");
            for(u32 i=0;i<result.glyph_count;++i){if(i)print(",");number(s.glyphs[i]);}print("],\"expected_glyphs\":[");for(u32 i=0;i<glyph_count;++i){if(i)print(",");number(byte_load_le16(glyphs.data+i*2));}
            print("],\"native_clusters\":[");for(u32 i=0;i<result.source_units;++i){if(i)print(",");number(s.character_glyphs[i]);}print("],\"expected_clusters\":[");for(u32 i=0;i<source_units;++i){if(i)print(",");number(byte_load_le32(clusters.data+i*4));}
            print("],\"native_placement\":[");for(u32 i=0;i<result.glyph_count;++i){if(i)print(",");const auto* b=reinterpret_cast<const u8*>(s.placements.data+i);print("[");number(byte_load_le32(b));print(",");number(byte_load_le32(b+4));print(",");number(byte_load_le32(b+8));print("]");}
            print("],\"position_state\":[");for(u32 i=0;i<result.glyph_count&&!result.nominal_ranges;++i){if(i)print(",");print("[");number(s.shaping.flags[i]);const auto* position_bytes=reinterpret_cast<const u8*>(s.shaping.positions.data+i);
                for(u32 j=0;j<3;++j){u64 bits=0;copy_bytes(&bits,position_bytes+j*8,8);print(",");number(bits);}print(",");number(s.shaping.associations[i].target);print(",");number(s.shaping.associations[i].component);print("]");}
            print("],\"glyph_sources\":[");for(u32 i=0;i<result.glyph_count;++i){if(i)print(",");number(s.shaping.glyphs[i].source);}
            print("],\"glyph_components\":[");for(u32 i=0;i<result.glyph_count;++i){if(i)print(",");number(s.shaping.glyphs[i].component);}
            print("],\"nominal\":");number(result.nominal_ranges);print("}\n");}}
    }
    CHECK(reader.at==reader.bytes.count);print("differences=");number(differences);print("\n");return !differences;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_latin_run_entry() noexcept {using namespace maicord;const bool ok=run();print("cases=");number(cases);print(" checks=");number(checks);print(ok?" PASS native Latin runs\n":" FAIL native Latin runs\n");flush();ExitProcess(ok&&print_ok?0:1);}
