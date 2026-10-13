#include "maicord/text/run_plan.hpp"
#include "maicord/text/font_prepare.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
u64 checks=0,glyphs=0,base_bytes=0,bold_bytes=0;u32 faces=0;
void print(const char* s) noexcept{u32 n=0;while(s[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&written,nullptr);}
void number(u64 value) noexcept{char b[24];u32 n=0;do{b[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char c=b[i];b[i]=b[n-1-i];b[n-1-i]=c;}b[n]=0;print(b);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print(" face=");number(faces);print("\n");return false;}}while(0)
struct Reader {const u8* p;u32 left;bool take(u32 n,const u8** out) noexcept{if(n>left)return false;*out=p;p+=n;left-=n;return true;}bool word(u32* out) noexcept{const u8* b=nullptr;if(!take(4,&b))return false;*out=byte_load_le32(b);return true;}};
bool exercise(const wchar_t* path,u32 index,u32 count,const u8* expected) noexcept {
    NativeFontSession* session=nullptr;Arena scratch{};NativeFontHandle face{};NativeFontInstanceHandle instance{};NativeRunPlanSlot slots[2]{};NativeRunPlanStore store{};
    CHECK(native_font_session_create(&session)&&arena_initialize(&scratch,4*1024*1024,0,4096));store={native_font_session_instances(session),{slots,2}};
    struct Release {NativeFontSession*& session;Arena& scratch;NativeRunPlanStore& store;NativeFontHandle& face;NativeFontInstanceHandle& instance;
        ~Release() noexcept{for(u32 i=store.slots.count;i;--i)while(store.slots[i-1].references)native_run_plan_release(&store,{i,store.slots[i-1].generation});
            if(instance.slot)native_font_instance_release(native_font_session_instances(session),instance);if(face.slot)native_font_release(native_font_session_fonts(session),face);native_font_session_release(&session);arena_release(&scratch);}}
        release{session,scratch,store,face,instance};
    CHECK(native_font_session_face(session,path,index,&face).ok());CHECK(native_font_prepare(session,face,{},0,false,&scratch,&instance).ok());
    CHECK(native_font_release(native_font_session_fonts(session),face));face={};
    NativeRunPlanHandle failed{};
    CHECK(!native_run_plan_acquire(&store,instance,&scratch,&failed,1000000,4096,1).ok()&&!failed.slot&&!slots[0].references&&!slots[1].references);
    CHECK(!native_run_plan_acquire(&store,instance,&scratch,&failed,1000000,16*1024*1024,4).ok()&&!failed.slot);
    NativeRunPlanHandle handles[4]{};
    CHECK(native_run_plan_acquire(&store,instance,&scratch,handles,1000000,16*1024*1024,0).ok());
    const auto normal=handles[0];const auto instance_refs=native_font_session_instances(session)->slots[instance.slot-1].references;
    auto one_slot=store;one_slot.slots.count=1;
    CHECK(!native_run_plan_acquire(&one_slot,instance,&scratch,&failed,1000000,16*1024*1024,1).ok()&&!failed.slot&&slots[normal.slot-1].references==1);
    CHECK(native_run_plan_acquire(&store,instance,&scratch,handles+1,1000000,4096,1).ok());
    CHECK(native_run_plan_acquire(&store,instance,&scratch,handles+2,1000000,0,2).ok());
    CHECK(native_run_plan_acquire(&store,instance,&scratch,handles+3,1000000,0,3).ok());
    CHECK(handles[0].slot==handles[2].slot&&handles[1].slot==handles[3].slot&&handles[0].slot!=handles[1].slot);
    CHECK(native_font_session_instances(session)->slots[instance.slot-1].references==instance_refs);
    const auto nominal=native_run_plan_nominal(store,normal);if(expected)CHECK(nominal.glyph_limit==count);else count=nominal.glyph_limit;
    const auto simulated=native_run_plan_nominal(store,handles[1]);CHECK(simulated.participation.data==nominal.participation.data&&simulated.glyph_limit==nominal.glyph_limit);
    CHECK(slots[handles[1].slot-1].state.committed==4096&&slots[handles[1].slot-1].state.reserved==4096);
    base_bytes+=slots[normal.slot-1].state.committed;bold_bytes+=slots[handles[1].slot-1].state.committed;
    for(u32 independent=0;independent<2;++independent){const auto* a=native_run_plan_get(store,normal,independent!=0);const auto* b=native_run_plan_get(store,handles[1],independent!=0);
        CHECK(a&&b&&a->substitutions==b->substitutions&&a->positioning==b->positioning&&a->substitution_roots.data==b->substitution_roots.data&&a->positioning_roots.data==b->positioning_roots.data&&a->metric_context!=b->metric_context);
        for(u32 mode=0;mode<4;++mode){const auto* p=native_run_plan_get(store,handles[mode],independent!=0);
            for(u32 glyph=0;glyph<count;++glyph){double value=0;CHECK(p->advance(p->metric_context,static_cast<u16>(glyph),&value)==SfntStatus::ok);
                if(expected)CHECK(value==byte_load_le32(expected+glyph*8+(mode&1)*4));++glyphs;}}
    }
    CHECK(native_font_instance_release(native_font_session_instances(session),instance));instance={};
    CHECK(native_run_plan_release(&store,handles[0])&&native_run_plan_release(&store,handles[2])&&native_run_plan_get(store,normal));
    CHECK(native_run_plan_release(&store,handles[1])&&native_run_plan_get(store,normal));
    CHECK(native_run_plan_release(&store,handles[3])&&!native_run_plan_get(store,normal)&&!slots[0].references&&!slots[1].references);
    CHECK(!native_font_session_stats(session).instances&&!native_font_stats(*native_font_session_fonts(session)).files);++faces;return true;
}
bool run() noexcept {
    const wchar_t* p=GetCommandLineW();bool quoted=false;while(*p){if(*p=='"')quoted=!quoted;else if(!quoted&&(*p==' '||*p=='\t'))break;++p;}while(*p==' '||*p=='\t')++p;
    wchar_t path[32768];u32 n=0;quoted=*p=='"';if(quoted)++p;while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){CHECK(n<32767);path[n++]=*p++;}path[n]=0;CHECK(n);
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr),mapping=nullptr;const u8* bytes=nullptr;
    struct Release {HANDLE& f;HANDLE& m;const u8*& p;~Release() noexcept{if(p)UnmapViewOfFile(p);if(m)CloseHandle(m);if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);}} release{file,mapping,bytes};
    LARGE_INTEGER size{};CHECK(file!=INVALID_HANDLE_VALUE&&GetFileSizeEx(file,&size)&&size.QuadPart>=4&&size.QuadPart<=64*1024*1024);
    mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);CHECK(mapping);bytes=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CHECK(bytes);
    if(byte_load_le32(bytes)!=0x3150534du)return exercise(path,0,0,nullptr);
    Reader reader{bytes+4,static_cast<u32>(size.QuadPart)-4};u32 records=0;CHECK(reader.word(&records)&&records<=256);
    for(u32 record=0;record<records;++record){u32 units=0,index=0,count=0;const u8* name=nullptr;const u8* expected=nullptr;
        CHECK(reader.word(&units)&&units&&units<32768&&reader.word(&index)&&reader.word(&count)&&count&&count<=65535&&reader.take(units*2,&name)&&reader.take(count*8,&expected));
        for(u32 i=0;i<units;++i){path[i]=static_cast<wchar_t>(byte_load_le16(name+i*2));CHECK(path[i]);}path[units]=0;CHECK(exercise(path,index,count,expected));}
    CHECK(!reader.left);return true;
}
}}
extern "C" void maicord_simulation_plan_entry(){using namespace maicord;const bool ok=run();print("faces=");number(faces);print(" glyph_advances=");number(glyphs);print(" base_state_sum=");number(base_bytes);print(" bold_state_sum=");number(bold_bytes);print(" checks=");number(checks);print(ok?" PASS simulation plans\n":" FAIL simulation plans\n");ExitProcess(ok?0:1);}
