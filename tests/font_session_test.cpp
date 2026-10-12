#include "maicord/text/font_prepare.hpp"
#include "maicord/text/font_run.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
static_assert(sizeof(NativeFontSelection)==48);
#include "generated/tt_outline_fixture.inc"
constexpr u32 session_capacity=1024;
NativeFontSession* session=nullptr;NativeFontHandle faces[session_capacity],alias;NativeFontInstanceHandle instances[session_capacity],instructed;NativeFontRunHandle runs[session_capacity];
wchar_t paths[session_capacity+1][MAX_PATH];u32 paths_created=0,checks=0;DWORD handles_before=0;
void print(const char* text) noexcept {u32 n=0;while(text[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,nullptr);}
void number(u32 value) noexcept {char digits[16];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){const char c=digits[i];digits[i]=digits[n-i-1];digits[n-i-1]=c;}digits[n]=0;print(digits);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
bool write(const wchar_t* path,ConstSlice<u8> data) noexcept {HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(file==INVALID_HANDLE_VALUE)return false;DWORD written=0;const bool ok=WriteFile(file,data.data,data.count,&written,nullptr)&&written==data.count;CloseHandle(file);return ok;}
bool same(NativeFontHandle a,NativeFontHandle b) noexcept {return a.slot==b.slot&&a.generation==b.generation;}
bool same(NativeFontInstanceHandle a,NativeFontInstanceHandle b) noexcept {return a.slot==b.slot&&a.generation==b.generation;}
bool run() noexcept {
    CHECK(native_font_session_create(&session));const auto empty=native_font_session_stats(session);
    CHECK(empty.references==1&&!empty.live.files&&!empty.instances&&empty.metadata_committed==4096);
    CHECK(native_font_session_face(session,nullptr,0,&alias).error==NativeFontError::invalid_input&&!alias.slot);
    CHECK(native_font_session_instance(session,{}, {},false,{},&instructed).error==NativeFontError::invalid_input&&!instructed.slot);
    CHECK(native_font_session_stats(session).metadata_committed==empty.metadata_committed);
    NativeFontSession* borrower=session;CHECK(native_font_session_retain(borrower)&&native_font_session_stats(session).references==2);
    CHECK(native_font_session_release(&borrower)&&!borrower&&native_font_session_stats(session).references==1);
    wchar_t folder[MAX_PATH];CHECK(GetTempPathW(MAX_PATH,folder));const ConstSlice<u8> font{tt_outline_fixture+20,byte_load_le32(tt_outline_fixture+12)};
    CHECK(font.count+20<sizeof(tt_outline_fixture));
    for(u32 i=0;i<session_capacity+1;++i){CHECK(GetTempFileNameW(folder,L"mfs",0,paths[i]));++paths_created;CHECK(write(paths[i],font));}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_before));
    const NativeFontFileSlot* first_file=nullptr;const NativeFontFace* first_face=nullptr;const NativeFontInstance* first_instance=nullptr;
    for(u32 i=0;i<session_capacity;++i){CHECK(native_font_session_face(session,paths[i],0,faces+i).ok());
        CHECK(native_font_session_instance(session,faces[i],{},false,{},instances+i).ok());
        auto* fs=native_font_session_fonts(session);auto* is=native_font_session_instances(session);
        if(!i){first_file=fs->files.data;first_face=native_font_get(*fs,faces[0]);first_instance=native_font_instance_get(*is,instances[0]);}
        CHECK(fs->files.data==first_file&&native_font_get(*fs,faces[0])==first_face&&native_font_instance_get(*is,instances[0])==first_instance&&first_instance->geometry.font==&first_face->geometry);
        if(!i||i==64||i+1==session_capacity){MEMORY_BASIC_INFORMATION region{};
            CHECK(VirtualQuery(native_font_get(*fs,faces[i]),&region,sizeof(region))==sizeof(region)&&region.AllocationBase==session);
            CHECK(VirtualQuery(native_font_instance_get(*is,instances[i]),&region,sizeof(region))==sizeof(region)&&region.AllocationBase==session);}
        u16 glyph=0;CHECK(sfnt_cmap_glyph(first_face->cmap,'a',0,&glyph)==SfntStatus::ok&&glyph);
        const auto stats=native_font_session_stats(session);CHECK(stats.live.files==i+1&&stats.live.faces==i+1&&stats.instances==i+1&&stats.metadata_committed>=empty.metadata_committed);
    }
    const auto peak=native_font_session_stats(session);CHECK(peak.peak_files==session_capacity&&peak.peak_faces==session_capacity&&peak.peak_instances==session_capacity&&peak.peak_mapped_bytes==static_cast<u64>(font.count)*session_capacity);
    DWORD handles=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles)&&handles==handles_before+session_capacity);
    const NativeFontRun* first_run=nullptr;
    for(u32 i=0;i<session_capacity;++i){NativeFontSelection source{};source.face=faces[i];CHECK(native_font_session_run(session,instances[i],source,0,runs+i).ok());
        if(!i)first_run=native_font_run_get(*native_font_session_runs(session),runs[0]);
        CHECK(first_run&&native_font_run_get(*native_font_session_runs(session),runs[0])==first_run);
        if(!i||i==64||i+1==session_capacity){MEMORY_BASIC_INFORMATION region{};
            CHECK(VirtualQuery(native_font_run_get(*native_font_session_runs(session),runs[i]),&region,sizeof(region))==sizeof(region)&&region.AllocationBase==session);}}
    NativeFontRunHandle extra{};NativeFontSelection source{};source.face=faces[0];
    CHECK(native_font_session_run(session,instances[0],source,0,&extra).ok()&&extra.slot==runs[0].slot&&extra.generation==runs[0].generation);
    CHECK(native_font_run_release(native_font_session_runs(session),extra));
    CHECK(native_font_session_run(session,instances[0],source,1,&extra).error==NativeFontError::capacity&&!extra.slot);
    CHECK(native_font_session_run(session,{},source,0,&extra).error==NativeFontError::invalid_input&&!extra.slot);
    source.face=faces[1];CHECK(native_font_session_run(session,instances[0],source,0,&extra).error==NativeFontError::invalid_input&&!extra.slot);
    const auto stale_run=runs[17];CHECK(native_font_run_release(native_font_session_runs(session),runs[17]));runs[17]={};
    CHECK(!native_font_run_get(*native_font_session_runs(session),stale_run)&&!native_font_run_retain(native_font_session_runs(session),stale_run)&&!native_font_run_release(native_font_session_runs(session),stale_run));
    source.face=faces[0];CHECK(native_font_session_run(session,instances[0],source,1,runs+17).ok()&&runs[17].slot==stale_run.slot&&runs[17].generation!=stale_run.generation);
    CHECK(native_font_run_get(*native_font_session_runs(session),runs[0])==first_run);
    for(auto& run:runs){CHECK(native_font_run_release(native_font_session_runs(session),run));run={};}
    CHECK(!native_font_session_release(&session)&&session&&native_font_session_stats(session).references==1);
    CHECK(native_font_session_face(session,paths[session_capacity],0,&alias).error==NativeFontError::capacity&&!alias.slot);
    CHECK(native_font_session_face(session,paths[0],0,&alias).ok()&&same(alias,faces[0]));
    CHECK(native_font_release(native_font_session_fonts(session),alias));alias={};
    NativeFontInstanceHandle duplicate{};CHECK(native_font_session_instance(session,faces[0],{},false,{},&duplicate).ok()&&same(duplicate,instances[0]));
    CHECK(native_font_instance_release(native_font_session_instances(session),duplicate));
    const auto stale_face=faces[17];const auto stale_instance=instances[17];
    CHECK(native_font_release(native_font_session_fonts(session),faces[17]));faces[17]={};
    CHECK(native_font_instance_release(native_font_session_instances(session),instances[17]));instances[17]={};
    CHECK(!native_font_get(*native_font_session_fonts(session),stale_face)&&!native_font_instance_get(*native_font_session_instances(session),stale_instance));
    CHECK(native_font_session_face(session,paths[session_capacity],0,faces+17).ok()&&faces[17].slot==stale_face.slot&&faces[17].generation!=stale_face.generation);
    CHECK(native_font_session_instance(session,faces[17],{},false,{},instances+17).ok()&&instances[17].slot==stale_instance.slot&&instances[17].generation!=stale_instance.generation);
    CHECK(native_font_get(*native_font_session_fonts(session),faces[0])==first_face&&native_font_instance_get(*native_font_session_instances(session),instances[0])==first_instance);
    for(u32 i=0;i<session_capacity;++i){CHECK(native_font_release(native_font_session_fonts(session),faces[i]));faces[i]={};CHECK(native_font_instance_release(native_font_session_instances(session),instances[i]));instances[i]={};}
    CHECK(!native_font_session_stats(session).live.files&&!native_font_session_stats(session).instances);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles)&&handles==handles_before);
    // Outline instruction state remains distinct, while both variants share
    // the exact mapped and parsed face. No glyph geometry is approximated.
    CHECK(native_font_session_face(session,paths[0],0,faces).ok());
    CHECK(native_font_session_instance(session,faces[0],{},false,{},instances).ok());
    i32 stack[4096];TtCallFrame frames[64];NativeFontPrepareScratch scratch{{},{},{stack,4096},{frames,64}};
    CHECK(native_font_session_instance(session,faces[0],{},true,scratch,&instructed).ok()&&!same(instructed,instances[0]));
    const auto* a=native_font_instance_get(*native_font_session_instances(session),instances[0]);const auto* b=native_font_instance_get(*native_font_session_instances(session),instructed);
    CHECK(a&&b&&a->geometry.font==b->geometry.font&&!a->instructions&&b->instructions&&same(a->face,b->face));
    CHECK(native_font_session_stats(session).live.files==1&&native_font_session_stats(session).instances==2);
    Arena arena{};CHECK(arena_initialize(&arena,1024*1024,0,4096));
    struct ReleaseArena {Arena& a;~ReleaseArena() noexcept {arena_release(&a);}} release_arena{arena};
    auto* prefix=arena_allocate<u32>(&arena);CHECK(prefix);*prefix=0x12345678;NativeFontInstanceHandle prepared{};bool oblique=true,bold=true;
    CHECK(native_font_prepare(session,faces[0],{},0,false,&arena,&prepared,&oblique,&bold).ok()&&same(prepared,instances[0])&&!oblique&&!bold&&*prefix==0x12345678);
    CHECK(native_font_instance_release(native_font_session_instances(session),prepared));
    CHECK(native_font_prepare(session,faces[0],{},0,true,&arena,&prepared).ok()&&same(prepared,instructed)&&*prefix==0x12345678);
    CHECK(native_font_instance_release(native_font_session_instances(session),prepared));
    CHECK(native_font_prepare(session,faces[0],{},3,true,&arena,&prepared,&oblique,&bold).ok()&&same(prepared,instructed)&&oblique&&bold);
    CHECK(native_font_instance_release(native_font_session_instances(session),prepared));
    CHECK(native_font_prepare(session,faces[0],{},1,true,&arena,&prepared).font_status==SfntStatus::unsupported&&!prepared.slot);
    CHECK(native_font_prepare(session,faces[0],{},2,true,&arena,&prepared).font_status==SfntStatus::unsupported);
    CHECK(native_font_prepare(session,faces[0],{},4,true,&arena,&prepared,&oblique,&bold).font_status==SfntStatus::unsupported&&!oblique&&!bold);
    CHECK(native_font_prepare(session,{}, {},0,true,&arena,&prepared).error==NativeFontError::invalid_input);
    CHECK(native_font_prepare(session,faces[0],{nullptr,1},0,true,&arena,&prepared).error==NativeFontError::invalid_input);
    CHECK(native_font_prepare(session,faces[0],{},0,true,nullptr,&prepared).error==NativeFontError::invalid_input);
    CHECK(native_font_prepare(session,faces[0],{},0,true,&arena,nullptr).error==NativeFontError::invalid_input);
    return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_session_entry() noexcept {
    using namespace maicord;bool ok=run();if(session){const auto* allocation=session;auto* fs=native_font_session_fonts(session);auto* is=native_font_session_instances(session);
        for(auto run:runs)if(run.slot)ok=native_font_run_release(native_font_session_runs(session),run)&&ok;
        if(alias.slot)ok=native_font_release(fs,alias)&&ok;if(instructed.slot)ok=native_font_instance_release(is,instructed)&&ok;
        for(u32 i=0;i<session_capacity;++i){if(faces[i].slot)ok=native_font_release(fs,faces[i])&&ok;if(instances[i].slot)ok=native_font_instance_release(is,instances[i])&&ok;}
        ok=native_font_session_release(&session)&&ok;MEMORY_BASIC_INFORMATION region{};
        ok=(VirtualQuery(allocation,&region,sizeof(region))==sizeof(region)&&region.State==MEM_FREE)&&ok;}
    for(u32 i=0;i<paths_created;++i)ok=DeleteFileW(paths[i])&&ok;
    print("checks=");number(checks);print(ok?" PASS native font session\n":" FAIL native font session\n");ExitProcess(ok?0:1);
}
