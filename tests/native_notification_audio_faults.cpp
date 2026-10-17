#include "maicord/platform/notification_audio.hpp"
#include <audioclient.h>
#include <mmreg.h>
#if !defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
#error This test must use the isolated nonemitting audio fixture copy.
#endif

namespace maicord {
namespace audio_faults {
using Check=void(*)(bool,const char*) noexcept;
enum Stage:LONG {none,resolve_stage,stop_stage,reset_stage,padding_stage,buffer_stage,release_stage};
LONG read(volatile LONG& value) noexcept {return InterlockedCompareExchange(&value,0,0);}
bool iid(REFIID a,REFIID b) noexcept {return bytes_equal(&a,&b,sizeof(GUID));}
struct State {
    HANDLE changed{},entered{},unblock{},render_event{};
    SRWLOCK callbacks_lock{};
    IMMNotificationClient* registered[2]{};IMMNotificationClient* retained[8]{};u32 retained_count{};
    volatile LONG block{},timed_out{},default_index{},padding[2]{},padding_error{},buffer_error{},release_error{},stop_error{},reset_error{},rates_seen{};
    volatile LONG initializes{},uninitializes{},factories{},opens{},starts{},stops{},resets{},padding_calls{},buffers{},releases{},registers{},unregisters{},defaults{},waits{},wait_handles{},posts{},post_successes{},post_error{},violations{},resolves{},gains{},first_gain_percent{},volume{100};
    DWORD worker_thread{};u32 acquired[2]{};i16 output[2][16]{};i16 pcm[64]{},pcm_alternate[64]{};
};
HRESULT gate(State& s,Stage stage) noexcept {
    if(InterlockedCompareExchange(&s.block,none,stage)==stage){
        SetEvent(s.entered);
        if(WaitForSingleObject(s.unblock,2000)!=WAIT_OBJECT_0){InterlockedIncrement(&s.timed_out);return HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
    }
    return S_OK;
}
void worker_only(State& s) noexcept {if(GetCurrentThreadId()!=s.worker_thread)InterlockedIncrement(&s.violations);}
template<class T> HRESULT query(T* self,volatile LONG& refs,REFIID id,void** out) noexcept {
    if(!out)return E_POINTER;*out=nullptr;
    if(!iid(id,__uuidof(T))&&!iid(id,__uuidof(IUnknown)))return E_NOINTERFACE;
    *out=self;InterlockedIncrement(&refs);return S_OK;
}
struct Render final:IAudioRenderClient {
    State* s{};u32 rate_index{};volatile LONG refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {return query<IAudioRenderClient>(this,refs,id,out);}
    ULONG STDMETHODCALLTYPE AddRef() override {return static_cast<ULONG>(InterlockedIncrement(&refs));}
    ULONG STDMETHODCALLTYPE Release() override {return static_cast<ULONG>(InterlockedDecrement(&refs));}
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames,BYTE** out) override {
        worker_only(*s);InterlockedIncrement(&s->buffers);const HRESULT h=static_cast<HRESULT>(read(s->buffer_error));
        const HRESULT blocked=gate(*s,buffer_stage);if(FAILED(blocked))return blocked;if(FAILED(h))return h;
        if(!out||!frames||frames>8||s->acquired[rate_index]){InterlockedIncrement(&s->violations);return E_INVALIDARG;}
        s->acquired[rate_index]=frames;*out=reinterpret_cast<BYTE*>(s->output[rate_index]);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames,DWORD flags) override {
        worker_only(*s);InterlockedIncrement(&s->releases);const HRESULT h=static_cast<HRESULT>(read(s->release_error));
        const HRESULT blocked=gate(*s,release_stage);if(FAILED(blocked))return blocked;
        if(frames!=s->acquired[rate_index]||(flags&~AUDCLNT_BUFFERFLAGS_SILENT)){InterlockedIncrement(&s->violations);return E_INVALIDARG;}
        s->acquired[rate_index]=0;if(FAILED(h))return h;
        InterlockedExchangeAdd(&s->padding[rate_index],static_cast<LONG>(frames));return S_OK;
    }
};
struct Client final:IAudioClient {
    State* s{};Render* renderer{};u32 rate_index{};volatile LONG refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {return query<IAudioClient>(this,refs,id,out);}
    ULONG STDMETHODCALLTYPE AddRef() override {return static_cast<ULONG>(InterlockedIncrement(&refs));}
    ULONG STDMETHODCALLTYPE Release() override {return static_cast<ULONG>(InterlockedDecrement(&refs));}
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode,DWORD flags,REFERENCE_TIME duration,REFERENCE_TIME period,const WAVEFORMATEX* f,LPCGUID session) override {
        worker_only(*s);InterlockedIncrement(&s->opens);
        if(mode!=AUDCLNT_SHAREMODE_SHARED||flags!=(AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY)||duration||period||session||!f||f->wFormatTag!=WAVE_FORMAT_PCM||f->nChannels!=2||(f->nSamplesPerSec!=44100&&f->nSamplesPerSec!=48000)||f->nAvgBytesPerSec!=f->nSamplesPerSec*4||f->nBlockAlign!=4||f->wBitsPerSample!=16||f->cbSize){InterlockedIncrement(&s->violations);return E_INVALIDARG;}
        rate_index=f->nSamplesPerSec==48000?1u:0u;renderer->rate_index=rate_index;InterlockedOr(&s->rates_seen,rate_index?2:1);
        InterlockedExchange(&s->padding[rate_index],0);s->acquired[rate_index]=0;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* out) override {worker_only(*s);if(!out)return E_POINTER;*out=8;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) override {InterlockedIncrement(&s->violations);return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* out) override {
        worker_only(*s);InterlockedIncrement(&s->padding_calls);const HRESULT h=static_cast<HRESULT>(read(s->padding_error));
        const HRESULT blocked=gate(*s,padding_stage);if(FAILED(blocked))return blocked;if(FAILED(h))return h;
        if(!out)return E_POINTER;*out=static_cast<UINT32>(read(s->padding[rate_index]));return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE,const WAVEFORMATEX*,WAVEFORMATEX**) override {InterlockedIncrement(&s->violations);return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**) override {InterlockedIncrement(&s->violations);return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*,REFERENCE_TIME*) override {InterlockedIncrement(&s->violations);return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Start() override {worker_only(*s);InterlockedIncrement(&s->starts);return S_OK;}
    HRESULT STDMETHODCALLTYPE Stop() override {
        worker_only(*s);InterlockedIncrement(&s->stops);const HRESULT h=static_cast<HRESULT>(read(s->stop_error));
        const HRESULT blocked=gate(*s,stop_stage);return FAILED(blocked)?blocked:h;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        worker_only(*s);InterlockedIncrement(&s->resets);const HRESULT h=static_cast<HRESULT>(read(s->reset_error));
        const HRESULT blocked=gate(*s,reset_stage);InterlockedExchange(&s->padding[rate_index],0);s->acquired[rate_index]=0;return FAILED(blocked)?blocked:h;
    }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override {worker_only(*s);s->render_event=event;return event?S_OK:E_INVALIDARG;}
    HRESULT STDMETHODCALLTYPE GetService(REFIID id,void** out) override {
        worker_only(*s);if(!out)return E_POINTER;*out=nullptr;if(!iid(id,__uuidof(IAudioRenderClient)))return E_NOINTERFACE;
        renderer->AddRef();*out=static_cast<IAudioRenderClient*>(renderer);return S_OK;
    }
};
constexpr wchar_t endpoint_a[]=L"fixture.render.A",endpoint_b[]=L"fixture.render.B";
struct Device final:IMMDevice {
    State* s{};Client* clients{};u32 index{};volatile LONG refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {return query<IMMDevice>(this,refs,id,out);}
    ULONG STDMETHODCALLTYPE AddRef() override {return static_cast<ULONG>(InterlockedIncrement(&refs));}
    ULONG STDMETHODCALLTYPE Release() override {return static_cast<ULONG>(InterlockedDecrement(&refs));}
    HRESULT STDMETHODCALLTYPE Activate(REFIID id,DWORD context,PROPVARIANT* params,void** out) override {
        worker_only(*s);if(!out)return E_POINTER;*out=nullptr;if(context!=CLSCTX_INPROC_SERVER||params||!iid(id,__uuidof(IAudioClient)))return E_INVALIDARG;
        for(u32 i=0;i<2;++i)if(read(clients[i].refs)==1){clients[i].AddRef();*out=static_cast<IAudioClient*>(clients+i);return S_OK;}
        return E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD,IPropertyStore**) override {InterlockedIncrement(&s->violations);return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR* out) override {
        worker_only(*s);if(!out)return E_POINTER;*out=static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(endpoint_a)));
        if(!*out)return E_OUTOFMEMORY;copy_bytes(*out,index?endpoint_b:endpoint_a,sizeof(endpoint_a));return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetState(DWORD* out) override {worker_only(*s);if(!out)return E_POINTER;*out=DEVICE_STATE_ACTIVE;return S_OK;}
};
struct Enumerator final:IMMDeviceEnumerator {
    State* s{};Device* devices{};u32 index{};volatile LONG refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {return query<IMMDeviceEnumerator>(this,refs,id,out);}
    ULONG STDMETHODCALLTYPE AddRef() override {return static_cast<ULONG>(InterlockedIncrement(&refs));}
    ULONG STDMETHODCALLTYPE Release() override {return static_cast<ULONG>(InterlockedDecrement(&refs));}
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow,DWORD,IMMDeviceCollection**) override {InterlockedIncrement(&s->violations);return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow,ERole role,IMMDevice** out) override {
        worker_only(*s);InterlockedIncrement(&s->defaults);if(!out)return E_POINTER;*out=nullptr;
        if(flow!=eRender||role!=eConsole)return E_INVALIDARG;auto* d=devices+read(s->default_index);d->AddRef();*out=d;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR,IMMDevice**) override {InterlockedIncrement(&s->violations);return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* callback) override {
        worker_only(*s);AcquireSRWLockExclusive(&s->callbacks_lock);
        if(!callback||s->registered[index]||s->retained_count==8){ReleaseSRWLockExclusive(&s->callbacks_lock);return E_INVALIDARG;}
        s->registered[index]=callback;
        // Registration itself has no AddRef per SDK contract. This distinct
        // explicit QA observer reference deliberately survives Unregister.
        callback->AddRef();s->retained[s->retained_count++]=callback;
        InterlockedIncrement(&s->registers);ReleaseSRWLockExclusive(&s->callbacks_lock);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* callback) override {
        worker_only(*s);AcquireSRWLockExclusive(&s->callbacks_lock);
        const HRESULT h=s->registered[index]==callback?S_OK:E_NOTFOUND;if(SUCCEEDED(h)){s->registered[index]=nullptr;InterlockedIncrement(&s->unregisters);}
        ReleaseSRWLockExclusive(&s->callbacks_lock);return h;
    }
};
struct Fixture {
    State s{};Render renders[2]{};Client clients[2]{};Device devices[2]{};Enumerator enumerators[2]{};NotificationAudio owner{};
    static void* operator new(usize,void* p) noexcept {return p;}
    Fixture() noexcept {
        for(u32 i=0;i<2;++i){renders[i].s=&s;clients[i].s=&s;clients[i].renderer=renders+i;
            enumerators[i].s=&s;enumerators[i].devices=devices;enumerators[i].index=i;
            devices[i].s=&s;devices[i].clients=clients;devices[i].index=i;}
        for(u32 i=0;i<64;++i){s.pcm[i]=(i&1)?-1000:1000;s.pcm_alternate[i]=(i&1)?-2500:1500;}
    }
};
HRESULT initialize(void* p) noexcept {auto& f=*static_cast<Fixture*>(p);f.s.worker_thread=GetCurrentThreadId();InterlockedIncrement(&f.s.initializes);return S_OK;}
void uninitialize(void* p) noexcept {InterlockedIncrement(&static_cast<Fixture*>(p)->s.uninitializes);}
HRESULT create(void* p,IMMDeviceEnumerator** out) noexcept {
    auto& f=*static_cast<Fixture*>(p);worker_only(f.s);if(!out)return E_POINTER;*out=nullptr;InterlockedIncrement(&f.s.factories);
    for(u32 i=0;i<2;++i)if(read(f.enumerators[i].refs)==1){f.enumerators[i].AddRef();*out=f.enumerators+i;return S_OK;}return E_OUTOFMEMORY;
}
BOOL post(void* p,HWND,u32) noexcept {
    auto& s=static_cast<Fixture*>(p)->s;InterlockedIncrement(&s.posts);
    const LONG error=read(s.post_error);if(error){SetLastError(static_cast<DWORD>(error));return FALSE;}InterlockedIncrement(&s.post_successes);return TRUE;
}
void before_wait(void* p,u32 handles) noexcept {auto& s=static_cast<Fixture*>(p)->s;InterlockedExchange(&s.wait_handles,static_cast<LONG>(handles));InterlockedIncrement(&s.waits);SetEvent(s.changed);}
bool resolve(void* p,u32 id,NotificationAudioClip* clip) noexcept {
    auto& s=static_cast<Fixture*>(p)->s;worker_only(s);InterlockedIncrement(&s.resolves);
    if((id!=1&&id!=2)||FAILED(gate(s,resolve_stage)))return false;*clip={{id==2?s.pcm_alternate:s.pcm,64},id==2?48000u:44100u};return true;
}
bool gain(void* p,float nominal,float* out) noexcept {auto& s=static_cast<Fixture*>(p)->s;worker_only(s);const LONG percent=read(s.volume);if(InterlockedIncrement(&s.gains)==1)InterlockedExchange(&s.first_gain_percent,percent);*out=nominal*static_cast<float>(percent)/100.0f;return true;}
bool setup(Fixture& f) noexcept {
    f.s.changed=CreateEventW(nullptr,FALSE,FALSE,nullptr);f.s.entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);f.s.unblock=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!f.s.changed||!f.s.entered||!f.s.unblock){if(f.s.changed)CloseHandle(f.s.changed);if(f.s.entered)CloseHandle(f.s.entered);if(f.s.unblock)CloseHandle(f.s.unblock);return false;}
    const NotificationAudioFixtureHooks hooks{&f,initialize,uninitialize,create,post,before_wait};notification_audio_fixture_hooks(&hooks);
    NotificationAudioConfig config{};config.resolve=resolve;config.context=&f;config.gain=gain;config.wake_window=reinterpret_cast<HWND>(usize{1});config.wake_message=WM_APP+71;
    return notification_audio_initialize(&f.owner,config).ok();
}
void arm(Fixture& f,Stage stage) noexcept {ResetEvent(f.s.entered);ResetEvent(f.s.unblock);InterlockedExchange(&f.s.block,stage);}
bool entered(Fixture& f) noexcept {return WaitForSingleObject(f.s.entered,2000)==WAIT_OBJECT_0;}
void unblock(Fixture& f) noexcept {SetEvent(f.s.unblock);}
bool submit(Fixture& f,u64 epoch=1) noexcept {return notification_audio_submit(&f.owner,{epoch,1,.4f,{}}).ok();}
template<class Predicate> bool await(Fixture& f,Predicate predicate) noexcept {
    const u64 end=GetTickCount64()+2000;
    for(;;){const auto status=notification_audio_status(&f.owner);if(predicate(status))return true;const u64 now=GetTickCount64();if(now>=end)return false;
        if(WaitForSingleObject(f.s.changed,static_cast<DWORD>(end-now))!=WAIT_OBJECT_0)return false;}
}
// System process information layout (x64): each process record is followed
// by its thread records; only the IDs and the scheduler state are read.
struct SpiProcess {ULONG next,threads;u8 skip[72];HANDLE process_id;u8 rest[168];};
struct SpiThread {u8 skip[40];HANDLE process_id,thread_id;LONG priority,base_priority;ULONG context_switches,state,wait_reason;};
static_assert(sizeof(SpiProcess)==256&&sizeof(SpiThread)==80);
u8* spi{};ULONG spi_bytes{};
bool parked(DWORD thread) noexcept {
    using Query=LONG(NTAPI*)(ULONG,PVOID,ULONG,PULONG);
    constexpr LONG length_mismatch=static_cast<LONG>(0xC0000004u);
    const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation"));
    if(!query)return false;
    for(;;){
        ULONG needed{};const LONG status=spi?query(5,spi,spi_bytes,&needed):length_mismatch;
        if(status>=0)break;if(status!=length_mismatch)return false;
        if(spi)VirtualFree(spi,0,MEM_RELEASE);spi_bytes=needed+256*1024;
        spi=static_cast<u8*>(VirtualAlloc(nullptr,spi_bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!spi)return false;
    }
    const HANDLE process_id=reinterpret_cast<HANDLE>(static_cast<usize>(GetCurrentProcessId()));
    const HANDLE thread_id=reinterpret_cast<HANDLE>(static_cast<usize>(thread));
    for(const u8* at=spi;;at+=reinterpret_cast<const SpiProcess*>(at)->next){
        const auto* process=reinterpret_cast<const SpiProcess*>(at);
        if(process->process_id==process_id){const auto* threads=reinterpret_cast<const SpiThread*>(process+1);
            // Waiting (5) for UserRequest (6) or WrUserRequest (13).
            for(ULONG i=0;i<process->threads;++i)if(threads[i].thread_id==thread_id)return threads[i].state==5&&(threads[i].wait_reason==6||threads[i].wait_reason==13);
            return false;}
        if(!process->next)return false;
    }
}
bool quiet(Fixture& f) noexcept {
    // Quiet means parked: the worker is blocked in its idle wait. A blocked wait
    // has no signaled handle, so no further iteration is pending; a spinning
    // worker never parks within the bound.
    const u64 end=GetTickCount64()+2000;
    do{if(parked(f.s.worker_thread))return true;SwitchToThread();}while(GetTickCount64()<end);
    return false;
}
IMMNotificationClient* callback(Fixture& f) noexcept {
    AcquireSRWLockShared(&f.s.callbacks_lock);auto* c=f.s.registered[0];if(c)c->AddRef();ReleaseSRWLockShared(&f.s.callbacks_lock);return c;
}
bool finish(Fixture& f,Check check) noexcept {
    unblock(f);const auto joined=notification_audio_release(&f.owner,2000);
    check(joined.ok()&&!f.owner.runtime&&!notification_audio_owned_handles(&f.owner),"audio fault: successful worker join is the borrowed PCM/context barrier");
    if(!joined.ok())return false; // static storage/hooks/events survive until failing harness exits
    check(read(f.s.initializes)==1&&read(f.s.uninitializes)==1&&read(f.s.violations)==0&&read(f.s.timed_out)==0,
        "audio fault: mock COM stays worker-owned, ABI contract valid and bounded gates acknowledged");
    check(read(f.enumerators[0].refs)==1&&read(f.enumerators[1].refs)==1&&read(f.clients[0].refs)==1&&read(f.clients[1].refs)==1&&read(f.renders[0].refs)==1&&read(f.renders[1].refs)==1&&read(f.devices[0].refs)==1&&read(f.devices[1].refs)==1&&!f.s.registered[0]&&!f.s.registered[1],
        "audio fault: enumerator/device/client/render references return to explicit fixture ownership");
    for(u32 i=0;i<f.s.retained_count;++i){auto* c=f.s.retained[i];const HANDLE event=notification_audio_fixture_callback_event(c);DWORD flags{};
        check(event&&GetHandleInformation(event,&flags)&&SetEvent(event),"audio fault: retained disarmed callback owns a valid duplicate after worker runtime release");
        ResetEvent(event);c->OnDefaultDeviceChanged(eRender,eConsole,endpoint_a);
        check(WaitForSingleObject(event,0)==WAIT_TIMEOUT,"audio fault: late callback after unregister is disarmed and cannot signal released owner");
        check(c->Release()==0,"audio fault: explicit final callback reference releases its independent ID and event");
        check(!GetHandleInformation(event,&flags)&&GetLastError()==ERROR_INVALID_HANDLE,"audio fault: callback duplicate closes exactly at final COM Release");
    }
    CloseHandle(f.s.changed);CloseHandle(f.s.entered);CloseHandle(f.s.unblock);notification_audio_fixture_hooks(nullptr);return true;
}
}
void test_native_notification_audio_faults(void(*check)(bool,const char*) noexcept) noexcept {
    using namespace audio_faults;using Error=NotificationAudioError;
    // Static placement storage survives an exceptional pending join. Never
    // reuse it after a failed join; the entry exits instead of freeing readers.
    alignas(Fixture) static u8 storage[sizeof(Fixture)]{};
    auto fresh=[&]() noexcept -> Fixture* {auto* f=new(storage)Fixture;if(setup(*f))return f;check(false,"audio fault: fixture synchronization setup succeeds");return nullptr;};
    auto* f=fresh();if(!f)return;
    arm(*f,resolve_stage);check(submit(*f)&&entered(*f),"audio fault: actual worker enters bounded resolver before mocked transport");
    bool accepted=true;for(u32 i=0;i<32;++i)accepted=accepted&&submit(*f);
    auto state=notification_audio_status(&f->owner);
    check(accepted&&state.queued==32&&state.outstanding==33&&notification_audio_submit(&f->owner,{1,1,.4f,{}}).error==Error::full_queue,
        "audio fault: queue32 plus in-flight resolver remains reserved without descriptor loss");
    unblock(*f);check(await(*f,[](const auto& s) noexcept {return s.active==33&&!s.queued;}),"audio fault: queued requests become33 pinned overlapping voices");
    accepted=true;for(u32 i=0;i<31;++i)accepted=accepted&&submit(*f);
    check(accepted&&await(*f,[](const auto& s) noexcept {return s.active==64&&!s.queued;})&&
        notification_audio_submit(&f->owner,{1,1,.4f,{}}).error==Error::full_voices&&quiet(*f),
        "audio fault: exact64 outstanding voice bound rejects65th and worker has no active-padding idle spin");
    check(notification_audio_reset(&f->owner,2,false).ok()&&await(*f,[](const auto& s) noexcept {return s.observed_epoch==2&&!s.outstanding&&!s.active;})&&quiet(*f),
        "audio fault: reset cancels every mixed reservation and closes stream into two-handle idle wait");
    state=notification_audio_status(&f->owner);check(state.accepted==64&&state.canceled==64&&!state.failed&&!state.completed&&read(f->s.wait_handles)==2,
        "audio fault: accepted64 is accounted exactly by cancellation rather than premature consumption");
    if(!finish(*f,check))return;

    f=fresh();if(!f)return;arm(*f,resolve_stage);
    check(submit(*f)&&entered(*f),"audio fault: blocked asset load precedes gain sampling and epoch validation");
    InterlockedExchange(&f->s.volume,50);
    check(notification_audio_reset(&f->owner,2,true).ok()&&submit(*f,2),"audio fault: owner volume and epoch change while borrowed resolver context remains pinned");unblock(*f);
    check(await(*f,[](const auto& s) noexcept {return s.observed_epoch==2&&s.active==1&&!s.queued;})&&quiet(*f)&&
        read(f->s.first_gain_percent)==50&&read(f->s.gains)==2&&read(f->s.opens)==1&&f->s.output[0][0]>=199&&f->s.output[0][0]<=200,
        "audio fault: gain samples after resolution and stale prepared clip never opens a stream or escapes its epoch");
    state=notification_audio_status(&f->owner);check(state.canceled==1&&state.accepted==2&&state.outstanding==1&&!state.failed,"audio fault: stalled stale resolver reservation is canceled exactly once");
    if(!finish(*f,check))return;

    f=fresh();if(!f)return;arm(*f,resolve_stage);
    check(submit(*f)&&entered(*f)&&notification_audio_submit(&f->owner,{1,2,.4f,{}}).ok(),"audio fault: queued mixed native-rate clips share one worker and request queue");unblock(*f);
    check(await(*f,[](const auto& s) noexcept {return s.active==2&&!s.queued&&s.stream_open;})&&quiet(*f)&&read(f->s.rates_seen)==3&&
        read(f->s.opens)==2&&read(f->s.factories)==2&&notification_audio_owned_handles(&f->owner)==6,
        "audio fault: exact44100 and48000 Initialize calls use independent clients with only two callback duplicates");
    check(f->s.output[0][0]>=399&&f->s.output[0][0]<=400&&f->s.output[1][0]>=599&&f->s.output[1][0]<=600&&f->s.output[1][1]>=-1000&&f->s.output[1][1]<=-999,
        "audio fault: independent source-rate PCM reaches each mock client without cross-rate mixing or custom SRC");
    for(u32 i=0;i<4;++i){const LONG before=read(f->s.waits);InterlockedExchange(&f->s.padding[1],0);SetEvent(f->s.render_event);
        check(await(*f,[&](const auto&) noexcept {return read(f->s.waits)>before;})&&quiet(*f),"audio fault: shared render event services both clients and consumes only alternate padding");}
    state=notification_audio_status(&f->owner);
    check(state.active==1&&state.outstanding==1&&state.completed==1&&state.stream_open&&read(f->s.padding[0])==8&&read(f->s.stops)==1,
        "audio fault: completed48000 client closes while44100 pinned voice and stream remain active");
    check(notification_audio_submit(&f->owner,{1,2,.4f,{}}).ok()&&await(*f,[](const auto& s) noexcept {return s.active==2&&!s.queued;})&&quiet(*f),
        "audio fault: alternate-rate client reopens lazily while sharing its cached endpoint callback");
    AcquireSRWLockShared(&f->s.callbacks_lock);auto* cb44=f->s.registered[0];auto* cb48=f->s.registered[1];if(cb44)cb44->AddRef();if(cb48)cb48->AddRef();ReleaseSRWLockShared(&f->s.callbacks_lock);
    check(cb44&&cb48,"audio fault: both native-rate engines retain independently owned callbacks");
    InterlockedExchange(&f->s.default_index,1);if(cb44){cb44->OnDefaultDeviceChanged(eRender,eConsole,endpoint_b);cb44->Release();}if(cb48){cb48->OnDefaultDeviceChanged(eRender,eConsole,endpoint_b);cb48->Release();}
    check(await(*f,[](const auto& s) noexcept {return s.switches==2&&s.active==2&&s.stream_open;})&&quiet(*f)&&read(f->s.registers)==4&&read(f->s.unregisters)==2,
        "audio fault: actual default switch independently rewinds and refreshes both native-rate engines");
    check(notification_audio_reset(&f->owner,2,false).ok()&&await(*f,[](const auto& s) noexcept {return s.observed_epoch==2&&!s.outstanding&&!s.stream_open;})&&quiet(*f),
        "audio fault: epoch acknowledgement follows Stop and Reset of both source-rate streams");
    state=notification_audio_status(&f->owner);check(state.accepted==3&&state.completed==1&&state.canceled==2&&!state.failed&&read(f->s.wait_handles)==2,
        "audio fault: dual-rate completion and cancellation balance global voice reservations exactly");
    if(!finish(*f,check))return;

    constexpr Stage close_stages[]{stop_stage,reset_stage};
    for(const Stage stage:close_stages) {
        f=fresh();if(!f)return;arm(*f,resolve_stage);
        check(submit(*f)&&entered(*f)&&notification_audio_submit(&f->owner,{1,2,.4f,{}}).ok(),"audio fault: epoch race prepares both native-rate mock streams");unblock(*f);
        check(await(*f,[](const auto& s) noexcept {return s.active==2&&s.stream_open;})&&quiet(*f),"audio fault: both native-rate clients are started before old-epoch cancellation");
        arm(*f,stage);InterlockedExchange(stage==stop_stage?&f->s.stop_error:&f->s.reset_error,static_cast<LONG>(E_ABORT));
        check(notification_audio_reset(&f->owner,2,true).ok()&&entered(*f),"audio fault: old stream close blocks outside UI lock");
        check(notification_audio_reset(&f->owner,3,true).ok()&&submit(*f,3),"audio fault: UI can advance epoch and accept a new request while old close is blocked");
        InterlockedExchange(stage==stop_stage?&f->s.stop_error:&f->s.reset_error,0);unblock(*f);
        check(await(*f,[](const auto& s) noexcept {return s.observed_epoch==3&&s.active==1&&!s.queued;})&&quiet(*f),"audio fault: new epoch survives the failing old Stop or Reset and starts independently");
        state=notification_audio_status(&f->owner);
        check(state.error==Error::none&&!state.native_error&&state.accepted==3&&state.canceled==2&&state.outstanding==1&&!state.failed&&read(f->s.stops)==2&&read(f->s.resets)==2,
            "audio fault: old close error cannot overwrite new-epoch status or discard accepted new-epoch queue");
        if(!finish(*f,check))return;
    }
    f=fresh();if(!f)return;check(submit(*f)&&await(*f,[](const auto& s) noexcept {return s.active==1&&s.stream_open;})&&quiet(*f),"audio fault: failed-audio epoch race prepares active stream");
    arm(*f,stop_stage);InterlockedExchange(&f->s.padding_error,static_cast<LONG>(E_FAIL));InterlockedExchange(&f->s.stop_error,static_cast<LONG>(E_ABORT));SetEvent(f->s.render_event);
    check(entered(*f)&&notification_audio_reset(&f->owner,2,true).ok()&&submit(*f,2),"audio fault: new epoch is accepted during failed old transport cleanup");
    InterlockedExchange(&f->s.padding_error,0);InterlockedExchange(&f->s.stop_error,0);unblock(*f);
    check(await(*f,[](const auto& s) noexcept {return s.observed_epoch==2&&s.active==1&&!s.queued;})&&quiet(*f),"audio fault: failed-audio old-epoch branch preserves new queue and resumes it");
    state=notification_audio_status(&f->owner);check(state.accepted==2&&state.canceled==1&&!state.failed&&state.outstanding==1&&state.error==Error::none&&!state.native_error,
        "audio fault: old transport and close errors cannot contaminate replacement epoch");
    if(!finish(*f,check))return;
    constexpr Stage transport_stages[]{padding_stage,buffer_stage,release_stage};
    for(const Stage stage:transport_stages) {
        f=fresh();if(!f)return;arm(*f,resolve_stage);check(submit(*f)&&entered(*f)&&submit(*f),"audio fault: two pinned voices prepare transport failure cleanup");unblock(*f);
        check(await(*f,[](const auto& s) noexcept {return s.active==2&&s.stream_open;})&&quiet(*f),"audio fault: mock stream remains active with unconsumed source tail");
        arm(*f,stage);auto* error=stage==padding_stage?&f->s.padding_error:stage==buffer_stage?&f->s.buffer_error:&f->s.release_error;
        InterlockedExchange(error,static_cast<LONG>(E_FAIL));if(stage!=padding_stage)InterlockedExchange(&f->s.padding[0],0);SetEvent(f->s.render_event);
        check(entered(*f)&&submit(*f),"audio fault: pending UI request is reserved while worker transport call blocks");unblock(*f);
        check(await(*f,[](const auto& s) noexcept {return s.failed==3&&!s.active&&!s.outstanding;})&&quiet(*f),"audio fault: padding/GetBuffer/ReleaseBuffer failure retires two active plus one queued reservation without retry spin");
        state=notification_audio_status(&f->owner);check(state.accepted==3&&state.failed==3&&!state.canceled&&!state.completed&&!state.queued&&!state.stream_open&&state.error==Error::stream&&state.native_error==static_cast<u32>(E_FAIL),
            "audio fault: transport failure counts and native error are exact and borrowed descriptors are erased");
        if(!finish(*f,check))return;
    }
    f=fresh();if(!f)return;arm(*f,resolve_stage);
    check(submit(*f)&&entered(*f)&&notification_audio_submit(&f->owner,{1,2,.4f,{}}).ok(),"audio fault: mixed-rate transport failure prepares shared reservations");unblock(*f);
    check(await(*f,[](const auto& s) noexcept {return s.active==2&&s.stream_open;})&&quiet(*f),"audio fault: two clients own independently buffered source tails");
    arm(*f,padding_stage);InterlockedExchange(&f->s.padding_error,static_cast<LONG>(E_FAIL));SetEvent(f->s.render_event);
    check(entered(*f)&&notification_audio_submit(&f->owner,{1,2,.4f,{}}).ok(),"audio fault: mixed-rate queued descriptor remains reserved through blocked transport failure");unblock(*f);
    check(await(*f,[](const auto& s) noexcept {return s.failed==3&&!s.outstanding&&!s.active&&!s.stream_open;})&&quiet(*f)&&read(f->s.stops)==2&&read(f->s.resets)==2,
        "audio fault: transport failure closes both native clients before erasing mixed active and queued reservations");
    if(!finish(*f,check))return;
    f=fresh();if(!f)return;check(submit(*f)&&await(*f,[](const auto& s) noexcept {return s.active==1&&s.stream_open;})&&quiet(*f),"audio fault: endpoint filtering prepares retained source tail");
    auto* cb=callback(*f);check(cb!=nullptr,"audio fault: registration exposes explicit retained callback");if(!cb){finish(*f,check);return;}
    arm(*f,padding_stage);SetEvent(f->s.render_event);check(entered(*f),"audio fault: worker transport gate makes callback event inspection deterministic");
    constexpr PROPERTYKEY format{{0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}},0};
    cb->OnDeviceStateChanged(L"fixture.capture.C",DEVICE_STATE_DISABLED);cb->OnDeviceRemoved(L"fixture.capture.C");cb->OnPropertyValueChanged(L"fixture.capture.C",format);cb->OnDefaultDeviceChanged(eCapture,eConsole,L"fixture.capture.C");
    check(WaitForSingleObject(notification_audio_fixture_callback_event(cb),0)==WAIT_TIMEOUT,"audio fault: unrelated capture state/removal/format/default events neither signal nor restart render stream");unblock(*f);
    check(quiet(*f)&&read(f->s.opens)==1&&read(f->s.stops)==0,"audio fault: unrelated events preserve original active stream and PCM cursors");
    const LONG defaults=read(f->s.defaults);cb->OnDefaultDeviceChanged(eRender,eConsole,endpoint_a);
    check(await(*f,[&](const auto&) noexcept {return read(f->s.defaults)>defaults;})&&quiet(*f)&&read(f->s.opens)==1&&read(f->s.stops)==0,
        "audio fault: same-ID default notification is worker-revalidated without reopen");
    cb->OnPropertyValueChanged(endpoint_a,format);check(await(*f,[](const auto& s) noexcept {return s.switches==1&&s.stream_open;})&&quiet(*f)&&read(f->s.opens)==2&&read(f->s.registers)==1,
        "audio fault: relevant endpoint format restarts stream but retains same-ID callback registration");
    InterlockedExchange(&f->s.default_index,1);cb->OnDefaultDeviceChanged(eRender,eConsole,endpoint_b);cb->Release();
    check(await(*f,[](const auto& s) noexcept {return s.switches==2&&s.stream_open;})&&quiet(*f)&&read(f->s.opens)==3&&read(f->s.registers)==2&&read(f->s.unregisters)==1,
        "audio fault: actual default-ID change rewinds unconsumed source and refreshes callback ownership exactly once");
    if(!finish(*f,check))return;

    f=fresh();if(!f)return;check(submit(*f)&&await(*f,[](const auto& s) noexcept {return s.active==1&&s.stream_open;})&&quiet(*f),"audio fault: pending-stop join prepares active mocked COM owner");
    arm(*f,stop_stage);
    check(notification_audio_release(&f->owner,0).error==Error::join_pending&&entered(*f)&&f->owner.runtime,
        "audio fault: pending Stop retains runtime and borrowed bank rather than freeing a live COM caller");
    check(notification_audio_status(&f->owner).stopping&&!notification_audio_can_accept(&f->owner),"audio fault: stopping owner rejects new admission while join is pending");
    if(!finish(*f,check))return;

    f=fresh();if(!f)return;InterlockedExchange(&f->s.post_error,ERROR_NOT_ENOUGH_QUOTA);arm(*f,resolve_stage);
    check(submit(*f)&&entered(*f),"audio fault: failed status post does not reject accepted request or block resolver");
    state=notification_audio_status(&f->owner);check(state.wake_error==ERROR_NOT_ENOUGH_QUOTA&&state.accepted==1&&state.outstanding==1&&read(f->s.posts)>0&&!read(f->s.post_successes),
        "audio fault: failed wake records native queue error while ownership remains observable");
    check(notification_audio_release(&f->owner,0).error==Error::join_pending&&f->owner.runtime&&notification_audio_owned_handles(&f->owner)==4,
        "audio fault: stopping blocked resolver retains context/events until actual worker join");
    InterlockedExchange(&f->s.post_error,0);unblock(*f);if(!finish(*f,check))return;
    check(read(f->s.post_successes)>0,"audio fault: final status changes can post after queue failure recovers without retaining stopped owner");
}
} // namespace maicord

namespace maicord {
namespace notification_audio_faults_entry {
u32 failures;
void check(bool passed,const char* message) noexcept {
    if(passed)return;++failures;u32 count{};while(message[count])++count;DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,count,&written,nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),"\r\n",2,&written,nullptr);
}
}
}
extern "C" __declspec(noreturn) void WINAPI maicord_notification_audio_faults_tests_entry() noexcept {
    maicord::test_native_notification_audio_faults(maicord::notification_audio_faults_entry::check);ExitProcess(maicord::notification_audio_faults_entry::failures?1:0);
}
