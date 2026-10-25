#include "maicord/text/font_session.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/font_run.hpp"

#include "maicord/core/win32_kernel.hpp"
namespace maicord {
namespace {constexpr u32 maximum_slots=1024,step=8,initial_slots=64;}
struct NativeFontSession {
    Arena faces,instances,runs;NativeFontStore fonts;NativeFontInstanceStore store;NativeFontRunStore run_store;
    u32 references,peak_files,peak_faces,peak_instances;u64 peak_mapped_bytes;usize header_committed;
    NativeFontFileSlot file_slots[maximum_slots];
};
// Reserve stable addresses for pressure/recovery without committing the unused
// tail. The ordinary 64-file prefix still shares the original owner page.
constexpr usize file_header_bytes=sizeof(NativeFontSession)-maximum_slots*sizeof(NativeFontFileSlot);
constexpr usize initial_owner_bytes=file_header_bytes+initial_slots*sizeof(NativeFontFileSlot);
static_assert(initial_owner_bytes<=4096&&sizeof(NativeFontSession)<=65536);
namespace {
NativeFontResult capacity_error() noexcept {return {NativeFontError::capacity,SfntStatus::resource_limit,0};}
bool extend_files(NativeFontSession& session) noexcept {
    auto& slots=session.fonts.files;if(slots.count==maximum_slots)return false;
    const u32 count=slots.count+minimum(step,maximum_slots-slots.count);
    const usize page=session.faces.commit_step;
    const usize required=(file_header_bytes+count*sizeof(NativeFontFileSlot)+page-1)&~(page-1);
    if(required>session.header_committed){auto* next=reinterpret_cast<u8*>(&session)+session.header_committed;
        if(VirtualAlloc(next,required-session.header_committed,MEM_COMMIT,PAGE_READWRITE)!=next)return false;
        session.header_committed=required;}
    slots.data=session.file_slots;slots.count=count;return true;
}
template<class T> bool extend(Arena& pool,Slice<T>& slots) noexcept {
    if(slots.count==maximum_slots)return false;
    const u32 count=minimum(step,maximum_slots-slots.count);auto* added=arena_allocate_zero<T>(&pool,count);
    if(!added)return false;if(!slots.data)slots.data=added;slots.count+=count;return true;
}
bool grow_fonts(void* context,bool files,bool faces) noexcept {
    auto& session=*static_cast<NativeFontSession*>(context);
    return (!files||extend_files(session))&&(!faces||extend(session.faces,session.fonts.faces));
}
bool full(ConstSlice<NativeFontInstanceSlot> slots) noexcept {for(u32 i=0;i<slots.count;++i)if(!slots[i].references&&slots[i].generation!=0xffffffffu)return false;return true;}
void peak(NativeFontSession& session) noexcept {
    const auto stats=native_font_session_stats(&session);session.peak_files=maximum(session.peak_files,stats.live.files);
    session.peak_faces=maximum(session.peak_faces,stats.live.faces);session.peak_instances=maximum(session.peak_instances,stats.instances);
    session.peak_mapped_bytes=maximum(session.peak_mapped_bytes,stats.live.mapped_bytes);
}
}
bool native_font_session_create(NativeFontSession** output) noexcept {
    if(!output)return false;*output=nullptr;
    Win32SystemInfo info{};GetSystemInfo(info.native());const usize page=info.dwPageSize;
    if(!page||(page&(page-1)))return false;
    const auto rounded=[&](usize bytes) noexcept {return (bytes+page-1)&~(page-1);};
    const usize files=rounded(sizeof(NativeFontSession)),faces=rounded(maximum_slots*sizeof(NativeFontFaceSlot)),
        instances=rounded(maximum_slots*sizeof(NativeFontInstanceSlot)),runs=rounded(maximum_slots*sizeof(NativeFontRunSlot));
    auto* session=static_cast<NativeFontSession*>(VirtualAlloc(nullptr,files+faces+instances+runs,MEM_RESERVE,PAGE_READWRITE));if(!session)return false;
    const usize committed=(initial_owner_bytes+page-1)&~(page-1);
    if(VirtualAlloc(session,committed,MEM_COMMIT,PAGE_READWRITE)!=session){VirtualFree(session,0,MEM_RELEASE);return false;}
    // Fresh commitment is zero initialized, including inactive file records.
    session->references=1;session->store.fonts=&session->fonts;session->run_store.session=session;session->header_committed=committed;
    // These arenas are slices of the owner's reservation. Only the owner frees
    // it; normal arena allocation commits each disjoint prefix on demand.
    auto* base=reinterpret_cast<u8*>(session)+files;
    session->faces={base,faces,0,0,0,page,0};
    session->instances={base+faces,instances,0,0,0,page,0};
    session->runs={base+faces+instances,runs,0,0,0,page,0};
    *output=session;return true;
}
bool native_font_session_retain(NativeFontSession* session) noexcept {if(!session||!session->references||session->references==0xffffffffu)return false;++session->references;return true;}
bool native_font_session_release(NativeFontSession** pointer) noexcept {
    if(!pointer||!*pointer)return false;auto* session=*pointer;
    if(session->references==1){const auto stats=native_font_session_stats(session);if(stats.live.faces||stats.instances)return false;
        VirtualFree(session,0,MEM_RELEASE);}
    else --session->references;*pointer=nullptr;return true;
}
NativeFontStore* native_font_session_fonts(NativeFontSession* session) noexcept {return session?&session->fonts:nullptr;}
NativeFontInstanceStore* native_font_session_instances(NativeFontSession* session) noexcept {return session?&session->store:nullptr;}
NativeFontRunStore* native_font_session_runs(NativeFontSession* session) noexcept {return session?&session->run_store:nullptr;}
NativeFontSessionStats native_font_session_stats(const NativeFontSession* session) noexcept {
    if(!session)return {};NativeFontSessionStats stats{};stats.live=native_font_stats(session->fonts);
    stats.metadata_committed=session->faces.committed+session->instances.committed+session->runs.committed;
    stats.metadata_committed+=session->header_committed;
    for(u32 i=0;i<session->store.slots.count;++i){const auto& instance=session->store.slots[i];if(instance.references){++stats.instances;stats.instance_committed+=instance.state.committed;}}
    stats.peak_files=session->peak_files;stats.peak_faces=session->peak_faces;stats.peak_instances=session->peak_instances;stats.peak_mapped_bytes=session->peak_mapped_bytes;stats.references=session->references;return stats;
}
NativeFontResult native_font_session_face(NativeFontSession* session,const wchar_t* path,u32 index,NativeFontHandle* output) noexcept {
    if(output)reset_value(output);if(!session||!output||!path||!*path)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    if((!session->fonts.files.count&&!extend_files(*session))||(!session->fonts.faces.count&&!extend(session->faces,session->fonts.faces)))return capacity_error();
    const auto result=native_font_acquire(&session->fonts,path,index,output,grow_fonts,session);
    if(result.ok())peak(*session);return result;
}
NativeFontResult native_font_session_instance(NativeFontSession* session,NativeFontHandle face,ConstSlice<FontAxisSetting> settings,
    bool instructions,NativeFontPrepareScratch scratch,NativeFontInstanceHandle* output) noexcept {
    if(output)reset_value(output);if(!session||!output)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    if(!native_font_get(session->fonts,face))return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    if(!session->store.slots.count&&!extend(session->instances,session->store.slots))return capacity_error();
    auto result=native_font_instance_acquire(&session->store,face,settings,instructions,scratch,output);
    if(result.error==NativeFontError::capacity&&full(ConstSlice<NativeFontInstanceSlot>{session->store.slots.data,session->store.slots.count})&&extend(session->instances,session->store.slots))
        result=native_font_instance_acquire(&session->store,face,settings,instructions,scratch,output);
    if(result.ok())peak(*session);return result;
}
NativeFontResult native_font_session_run(NativeFontSession* session,NativeFontInstanceHandle instance,const NativeFontSelection& source,u32 simulations,NativeFontRunHandle* output) noexcept {
    if(output)reset_value(output);if(!session||!output)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    auto result=native_font_run_acquire(&session->run_store,instance,source,simulations,output);
    if(result.error==NativeFontError::capacity){bool available=false;for(u32 i=0;i<session->run_store.slots.count;++i){const auto& s=session->run_store.slots[i];available|=!s.references&&s.generation!=0xffffffffu;}
        if(!available&&extend(session->runs,session->run_store.slots))result=native_font_run_acquire(&session->run_store,instance,source,simulations,output);}
    return result;
}
}
