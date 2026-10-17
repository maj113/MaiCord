#include "maicord/platform/notification_audio.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/core/byte_range.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

namespace maicord {
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
namespace { NotificationAudioFixtureHooks fixture_hooks{}; }
void notification_audio_fixture_hooks(const NotificationAudioFixtureHooks* hooks) noexcept {fixture_hooks=hooks?*hooks:NotificationAudioFixtureHooks{};}
#endif
namespace {
bool overlap(const void* a,usize an,const void* b,usize bn) noexcept {
    if(!an||!bn)return false;
    const usize x=reinterpret_cast<usize>(a),y=reinterpret_cast<usize>(b);
    return !a||!b||x>~usize{0}-an||y>~usize{0}-bn||(x<y+bn&&y<x+an);
}
void add(u64& a,u32 n=1) noexcept {a=a>~u64{0}-n?~u64{0}:a+n;}
NotificationAudioResult fail(NotificationAudioError e,u32 n=0) noexcept {return {e,n};}
float clipped(float v) noexcept {return v>32767.0f?32767.0f:v< -32768.0f? -32768.0f:v;}
bool rate_valid(u32 rate) noexcept {return rate==kNotificationAudioRate||rate==kNotificationAudioAlternateRate;}
u64& submitted(NotificationAudioMixer& m,u32 rate) noexcept {return rate==kNotificationAudioRate?m.submitted:m.submitted_48000;}
u64& consumed(NotificationAudioMixer& m,u32 rate) noexcept {return rate==kNotificationAudioRate?m.consumed:m.consumed_48000;}
u64 submitted(const NotificationAudioMixer& m,u32 rate) noexcept {return rate==kNotificationAudioRate?m.submitted:m.submitted_48000;}
}
bool notification_audio_clip_valid(NotificationAudioClip clip) noexcept {
    const auto s=clip.samples;
    return rate_valid(clip.sample_rate) && s.data && s.count && !(s.count&1u) && s.count<=20*clip.sample_rate &&
        !(reinterpret_cast<usize>(s.data)&1u) && reinterpret_cast<usize>(s.data)<=~usize{0}-usize(s.count)*sizeof(i16);
}
bool notification_audio_gain_valid(float gain) noexcept {return gain>=0.0f&&gain<=1.0f;}
bool notification_audio_endpoint_matches(const wchar_t* event_id,const wchar_t* owned_id,usize owned_chars) noexcept {
    if(!event_id||!owned_id||!owned_chars)return false;
    for(usize i=0;i<owned_chars;++i)if(!event_id[i]||event_id[i]!=owned_id[i])return false;
    return !event_id[owned_chars];
}
bool notification_audio_record_stream_failure(NotificationAudioStatus* status,u64 closed_epoch,u32 native_error) noexcept {
    if(!status||!closed_epoch||status->desired_epoch!=closed_epoch)return false;
    status->error=NotificationAudioError::stream;status->native_error=native_error;return true;
}
NotificationAudioResult notification_audio_prepare_request(const NotificationAudioConfig& config,
    const NotificationAudioRequest& request,NotificationAudioClip* clip,float* gain) noexcept {
    if(!clip||!gain||!notification_audio_gain_valid(request.gain))return fail(NotificationAudioError::invalid_input);
    NotificationAudioClip resolved=request.clip;
    if(!resolved.samples.count&&(!config.resolve||!request.clip_id||!config.resolve(config.context,request.clip_id,&resolved)))
        return fail(NotificationAudioError::resolve);
    if(!notification_audio_clip_valid(resolved))return fail(NotificationAudioError::invalid_input);
    float effective=request.gain;
    if(config.gain&&!config.gain(config.context,request.gain,&effective))return fail(NotificationAudioError::invalid_input);
    if(!notification_audio_gain_valid(effective))return fail(NotificationAudioError::invalid_input);
    *clip=resolved;*gain=effective;return {};
}
void notification_audio_mixer_reset(NotificationAudioMixer* m,u64 epoch) noexcept {if(m){clear_bytes(m,sizeof(*m));m->epoch=epoch;}}
NotificationAudioResult notification_audio_mixer_add(NotificationAudioMixer* m,u64 epoch,NotificationAudioClip clip,float gain) noexcept {
    if(!m||!epoch||!notification_audio_clip_valid(clip)||!notification_audio_gain_valid(gain)||
        overlap(m,sizeof(*m),clip.samples.data,usize(clip.samples.count)*sizeof(i16)))return fail(NotificationAudioError::invalid_input);
    if(epoch!=m->epoch)return fail(NotificationAudioError::stale_epoch);
    if(m->count==kNotificationAudioVoiceCapacity)return fail(NotificationAudioError::full_voices);
    const u64 position=submitted(*m,clip.sample_rate);
    if(position>~u64{0}-clip.samples.count/2)return fail(NotificationAudioError::timeline_exhausted);
    m->voices[m->count++]={clip,position,gain};return {};
}
u32 notification_audio_mixer_voices_rate(const NotificationAudioMixer& m,u32 rate) noexcept {
    if(!rate_valid(rate))return 0;u32 count{};for(u32 i=0;i<m.count;++i)if(m.voices[i].clip.sample_rate==rate)++count;return count;
}
u32 notification_audio_mixer_needed_rate(const NotificationAudioMixer& m,u32 rate) noexcept {
    if(!rate_valid(rate))return 0;const u64 position=submitted(m,rate);u64 end=position;
    for(u32 i=0;i<m.count;++i)if(m.voices[i].clip.sample_rate==rate)end=maximum(end,m.voices[i].start+m.voices[i].clip.samples.count/2);
    return static_cast<u32>(minimum<u64>(end-position,10*rate));
}
NotificationAudioMixResult notification_audio_mixer_render_rate(NotificationAudioMixer* m,Slice<i16> output,u32 rate) noexcept {
    if(!m||!m->epoch||!rate_valid(rate)||!output.data||!output.count||(output.count&1u)||output.count>2*kNotificationAudioMixChunkFrames||
        overlap(m,sizeof(*m),output.data,usize(output.count)*sizeof(i16)))return {NotificationAudioError::invalid_input};
    for(u32 i=0;i<m->count;++i)if(overlap(output.data,usize(output.count)*sizeof(i16),
        m->voices[i].clip.samples.data,usize(m->voices[i].clip.samples.count)*sizeof(i16)))return {NotificationAudioError::invalid_input};
    const u32 frames=minimum(output.count/2,notification_audio_mixer_needed_rate(*m,rate));
    bool nonzero=false;
    for(u32 i=0;i<frames;++i){float left=0,right=0;const u64 position=submitted(*m,rate)+i;
        for(u32 j=0;j<m->count;++j){const auto& v=m->voices[j];
            if(v.clip.sample_rate!=rate||position<v.start||position-v.start>=v.clip.samples.count/2)continue;
            const u32 sample=static_cast<u32>(position-v.start)*2;
            left+=float(v.clip.samples[sample])*v.gain;right+=float(v.clip.samples[sample+1])*v.gain;
        }
        output[2*i]=static_cast<i16>(clipped(left));output[2*i+1]=static_cast<i16>(clipped(right));
        nonzero=nonzero||output[2*i]!=0||output[2*i+1]!=0;
    }
    submitted(*m,rate)+=frames;return {NotificationAudioError::none,frames,nonzero};
}
u32 notification_audio_mixer_played_rate(NotificationAudioMixer* m,u32 padding,u32 rate) noexcept {
    if(!m||!rate_valid(rate)||padding>submitted(*m,rate)-consumed(*m,rate))return 0;
    consumed(*m,rate)=submitted(*m,rate)-padding;u32 kept=0;
    for(u32 i=0;i<m->count;++i)if(m->voices[i].clip.sample_rate!=rate||m->voices[i].start+m->voices[i].clip.samples.count/2>consumed(*m,rate))m->voices[kept++]=m->voices[i];
    const u32 retired=m->count-kept;for(u32 i=kept;i<m->count;++i)m->voices[i]={};m->count=kept;return retired;
}
void notification_audio_mixer_rewind_rate(NotificationAudioMixer* m,u32 rate) noexcept {if(m&&rate_valid(rate))submitted(*m,rate)=consumed(*m,rate);}

namespace {
// No runtime/app/PCM pointer is ever borrowed by the OS notification callback.
// Its duplicated event remains valid through its final external COM Release.
struct EndpointNotify final:IMMNotificationClient {
    volatile LONG references{1},active{1},dirty{};HANDLE command{};
    // Written before registration, then immutable through final Release. OS
    // callbacks never borrow an Engine/Runtime pointer or perform COM queries.
    LPWSTR endpoint_id{};usize endpoint_chars{};
    static void* operator new(usize,void* p) noexcept {return p;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(bytes_equal(&id,&__uuidof(IUnknown),sizeof(GUID))||bytes_equal(&id,&__uuidof(IMMNotificationClient),sizeof(GUID))){*out=this;AddRef();return S_OK;}
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return static_cast<ULONG>(InterlockedIncrement(&references));}
    ULONG STDMETHODCALLTYPE Release() override {const LONG n=InterlockedDecrement(&references);if(!n){if(command)CloseHandle(command);CoTaskMemFree(endpoint_id);this->~EndpointNotify();CoTaskMemFree(this);}return static_cast<ULONG>(n);}
    void signal(LONG reason) noexcept {if(InterlockedCompareExchange(&active,0,0)){InterlockedOr(&dirty,reason);SetEvent(command);}}
    bool matches(LPCWSTR id) const noexcept {return notification_audio_endpoint_matches(id,endpoint_id,endpoint_chars);}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,ERole role,LPCWSTR) override {if(flow==eRender&&role==eConsole)signal(1);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id,DWORD) override {if(matches(id))signal(2);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {if(matches(id))signal(2);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id,const PROPERTYKEY key) override {
        // SDK PKEY_AudioEngine_DeviceFormat, local value avoids a propsys/GUID
        // import. Volume/name property changes do not restart active playback.
        constexpr GUID format{0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}};
        if(!key.pid&&bytes_equal(&key.fmtid,&format,sizeof(format))&&matches(id))signal(2);return S_OK;
    }
};
struct Runtime {
    SRWLOCK lock{};NotificationAudioConfig config{};
    HANDLE thread{},command{},stop{},render{};
    NotificationAudioRequest requests[kNotificationAudioQueueCapacity]{};
    NotificationAudioMixer mixer{};
    NotificationAudioStatus status{},last_notified{};
    u32 head{},count{},reserved{},callback_handles{};
    bool stopping{},wake_pending{};
};
static_assert(sizeof(Runtime)+2*sizeof(EndpointNotify)<=16*1024,"notification audio metadata remains bounded");
struct Engine {
    IMMDeviceEnumerator* enumerator{};EndpointNotify* callback{};
    IAudioClient* client{};IAudioRenderClient* render{};u32 frames{};bool started{};
};
void notify_locked(Runtime& r) noexcept {
    if(bytes_equal(&r.status,&r.last_notified,sizeof(r.status)))return;
    if(!r.wake_pending&&r.config.wake_window&&r.config.wake_message){
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
        r.wake_pending=fixture_hooks.post_wake&&fixture_hooks.post_wake(fixture_hooks.context,r.config.wake_window,r.config.wake_message)!=0;
#else
        r.wake_pending=PostMessageW(r.config.wake_window,r.config.wake_message,0,0)!=0;
#endif
        if(!r.wake_pending)r.status.wake_error=thread_last_error();
    }
    r.last_notified=r.status;
}
void publish(Runtime& r) noexcept {
    AcquireSRWLockExclusive(&r.lock);r.status.queued=r.count;r.status.outstanding=r.reserved;
    r.status.active=r.mixer.count;notify_locked(r);ReleaseSRWLockExclusive(&r.lock);
}
HRESULT close_client(Engine& e) noexcept {
    HRESULT h=S_OK;
    if(e.client){if(e.started)h=e.client->Stop();const HRESULT reset=e.client->Reset();if(SUCCEEDED(h))h=reset;}
    e.started=false;e.frames=0;com_release(e.render);com_release(e.client);
    return h;
}
void close_stream(Runtime& r,Engine& e) noexcept {
    const u64 closed_epoch=r.mixer.epoch;
    const HRESULT h=close_client(e);
    if(FAILED(h)){AcquireSRWLockExclusive(&r.lock);
        if(notification_audio_record_stream_failure(&r.status,closed_epoch,static_cast<u32>(h)))notify_locked(r);
        ReleaseSRWLockExclusive(&r.lock);}
}
void close_callback(Runtime& r,Engine& e) noexcept {
    const bool duplicate=e.callback&&e.callback->command;
    if(e.callback)InterlockedExchange(&e.callback->active,0);
    if(e.enumerator&&e.callback)e.enumerator->UnregisterEndpointNotificationCallback(e.callback);
    com_release(e.callback);
    if(duplicate){AcquireSRWLockExclusive(&r.lock);--r.callback_handles;ReleaseSRWLockExclusive(&r.lock);}
}
void close_engine(Runtime& r,Engine& e) noexcept {
    close_callback(r,e);close_stream(r,e);com_release(e.enumerator);
}
HRESULT refresh_callback(Runtime& r,Engine& e,LPWSTR* id) noexcept {
    if(!*id||!**id)return E_UNEXPECTED;
    if(e.callback&&e.callback->matches(*id))return S_OK;
    close_callback(r,e);
    auto* memory=CoTaskMemAlloc(sizeof(EndpointNotify));if(!memory)return E_OUTOFMEMORY;
    e.callback=new(memory)EndpointNotify;e.callback->endpoint_id=*id;*id=nullptr;
    // GetId supplies a terminated full opaque ID; no guessed truncation/hash.
    while(e.callback->endpoint_id[e.callback->endpoint_chars])++e.callback->endpoint_chars;
    HRESULT h=S_OK;
    if(!DuplicateHandle(GetCurrentProcess(),r.command,GetCurrentProcess(),&e.callback->command,0,FALSE,DUPLICATE_SAME_ACCESS))h=HRESULT_FROM_WIN32(thread_last_error());
    else {AcquireSRWLockExclusive(&r.lock);++r.callback_handles;ReleaseSRWLockExclusive(&r.lock);}
    if(SUCCEEDED(h))h=e.enumerator->RegisterEndpointNotificationCallback(e.callback);
    if(FAILED(h))close_callback(r,e);return h;
}
bool default_endpoint_changed(Engine& e) noexcept {
    // The default-role notification is advisory. Revalidate the actual render
    // endpoint on the worker before disturbing an otherwise unchanged stream.
    IMMDevice* device=nullptr;LPWSTR id=nullptr;
    HRESULT h=e.enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device);
    if(SUCCEEDED(h))h=device->GetId(&id);
    const bool changed=FAILED(h)||!e.callback||!e.callback->matches(id);
    CoTaskMemFree(id);com_release(device);return changed;
}
HRESULT open_engine(Runtime& r,Engine& e,u32 rate) noexcept {
    HRESULT h=S_OK;
    if(!e.enumerator){
        AcquireSRWLockExclusive(&r.lock);add(r.status.device_open_attempts);ReleaseSRWLockExclusive(&r.lock);
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
        h=fixture_hooks.create?fixture_hooks.create(fixture_hooks.context,&e.enumerator):E_UNEXPECTED;
#else
        h=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&e.enumerator));
#endif
        if(FAILED(h)){close_engine(r,e);return h;}
    }
    IMMDevice* device=nullptr;LPWSTR id=nullptr;
    AcquireSRWLockExclusive(&r.lock);add(r.status.stream_open_attempts);ReleaseSRWLockExclusive(&r.lock);
    h=e.enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device);
    if(SUCCEEDED(h))h=device->GetId(&id);
    if(SUCCEEDED(h))h=refresh_callback(r,e,&id);
    CoTaskMemFree(id);
    if(SUCCEEDED(h))h=device->Activate(__uuidof(IAudioClient),CLSCTX_INPROC_SERVER,nullptr,reinterpret_cast<void**>(&e.client));
    const WAVEFORMATEX format{WAVE_FORMAT_PCM,2,rate,rate*4,4,16,0};
    if(SUCCEEDED(h))h=e.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0,0,&format,nullptr);
    // Required even if a later service/buffer-size query fails during opening.
    if(SUCCEEDED(h))h=e.client->SetEventHandle(r.render);
    if(SUCCEEDED(h))h=e.client->GetBufferSize(&e.frames);
    if(SUCCEEDED(h)&&!e.frames)h=E_UNEXPECTED;
    if(SUCCEEDED(h))h=e.client->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void**>(&e.render));
    com_release(device);if(FAILED(h))close_stream(r,e);return h;
}
bool current(Runtime& r,u64 epoch) noexcept {
    AcquireSRWLockShared(&r.lock);const bool same=!r.stopping&&r.status.enabled&&r.status.desired_epoch==epoch;ReleaseSRWLockShared(&r.lock);return same;
}
void retire_played(Runtime& r,u32 padding,u32 rate) noexcept {
    const u32 n=notification_audio_mixer_played_rate(&r.mixer,padding,rate);if(!n)return;
    AcquireSRWLockExclusive(&r.lock);r.reserved-=n;add(r.status.completed,n);ReleaseSRWLockExclusive(&r.lock);
}
void failed_audio(Runtime& r,Engine (&engines)[2],NotificationAudioError error,HRESULT native) noexcept {
    for(auto& e:engines)close_stream(r,e);
    // A failure of an old stream must not discard accepted new-epoch commands
    // that arrived while the old COM call was in progress.
    AcquireSRWLockExclusive(&r.lock);
    if(r.mixer.epoch&&r.mixer.epoch!=r.status.desired_epoch){
        const u32 old=r.mixer.count;r.reserved-=old;add(r.status.canceled,old);
        notification_audio_mixer_reset(&r.mixer,r.status.desired_epoch);
        r.status.observed_epoch=r.status.desired_epoch;r.status.stream_open=false;
        notify_locked(r);ReleaseSRWLockExclusive(&r.lock);return;
    }
    add(r.status.failed,r.reserved);
    clear_bytes(r.requests,sizeof(r.requests));r.head=r.count=r.reserved=0;
    r.status.error=error;r.status.native_error=static_cast<u32>(native);r.status.stream_open=false;
    notification_audio_mixer_reset(&r.mixer,r.status.desired_epoch);
    r.status.observed_epoch=r.status.desired_epoch;notify_locked(r);ReleaseSRWLockExclusive(&r.lock);
}
void admit_requests(Runtime& r) noexcept {
    for(u32 i=0;i<kNotificationAudioQueueCapacity;++i){
        NotificationAudioRequest request{};
        AcquireSRWLockExclusive(&r.lock);
        if(!r.count||r.stopping||r.status.desired_epoch!=r.mixer.epoch){ReleaseSRWLockExclusive(&r.lock);break;}
        request=r.requests[r.head];r.requests[r.head]={};r.head=(r.head+1)%kNotificationAudioQueueCapacity;--r.count;
        ReleaseSRWLockExclusive(&r.lock);
        NotificationAudioClip clip{};float gain{};
        const bool live=current(r,request.epoch);
        const auto prepared=live?notification_audio_prepare_request(r.config,request,&clip,&gain):fail(NotificationAudioError::stale_epoch);
        NotificationAudioResult admitted=fail(NotificationAudioError::stale_epoch);
        if(prepared.ok()&&current(r,request.epoch))admitted=notification_audio_mixer_add(&r.mixer,request.epoch,clip,gain);
        if(!admitted.ok()){
            AcquireSRWLockExclusive(&r.lock);--r.reserved;
            if(!live||r.status.desired_epoch!=request.epoch||!r.status.enabled)add(r.status.canceled);
            else {add(r.status.failed);r.status.error=prepared.ok()?admitted.error:prepared.error;r.status.native_error=0;}
            ReleaseSRWLockExclusive(&r.lock);
        }
    }
}
HRESULT render_audio(Runtime& r,Engine& e,u32 rate) noexcept {
    u32 padding{};HRESULT h=e.client->GetCurrentPadding(&padding);
    if(FAILED(h))return h;if(padding>e.frames)return E_UNEXPECTED;
    retire_played(r,padding,rate);
    const u32 frames=minimum(e.frames-padding,notification_audio_mixer_needed_rate(r.mixer,rate));
    if(!frames)return S_OK;
    BYTE* target=nullptr;h=e.render->GetBuffer(frames,&target);if(FAILED(h))return h;
    const u64 epoch=r.mixer.epoch;bool nonzero=false,canceled=false;HRESULT mix_error=S_OK;u32 offset=0;
    while(offset<frames){
        if(!current(r,epoch)){canceled=true;break;}
        const u32 n=minimum(kNotificationAudioMixChunkFrames,frames-offset);
        const auto mixed=notification_audio_mixer_render_rate(&r.mixer,{reinterpret_cast<i16*>(target)+offset*2,n*2},rate);
        if(mixed.error!=NotificationAudioError::none||mixed.frames!=n){canceled=true;mix_error=E_UNEXPECTED;break;}
        offset+=n;nonzero=nonzero||mixed.nonzero;
    }
    if(!current(r,epoch))canceled=true;
    h=e.render->ReleaseBuffer(frames,(canceled||!nonzero)?AUDCLNT_BUFFERFLAGS_SILENT:0);
    if(SUCCEEDED(h)&&FAILED(mix_error))return mix_error;
    if(SUCCEEDED(h)&&!canceled&&!e.started){h=e.client->Start();if(SUCCEEDED(h))e.started=true;}
    return h;
}
DWORD WINAPI worker_main(void* pointer) noexcept {
    auto& r=*static_cast<Runtime*>(pointer);Engine engines[2]{};
    constexpr u32 rates[]{kNotificationAudioRate,kNotificationAudioAlternateRate};
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
    const HRESULT apartment=fixture_hooks.initialize?fixture_hooks.initialize(fixture_hooks.context):E_UNEXPECTED;
#else
    const HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
#endif
    if(FAILED(apartment))failed_audio(r,engines,NotificationAudioError::device,apartment);
    else {
        for(;;){
            AcquireSRWLockShared(&r.lock);const bool stopping=r.stopping;const u64 desired=r.status.desired_epoch;ReleaseSRWLockShared(&r.lock);
            if(stopping)break;
            if(r.mixer.epoch!=desired){
                const u32 old=r.mixer.count;for(auto& e:engines)close_stream(r,e);notification_audio_mixer_reset(&r.mixer,desired);
                AcquireSRWLockExclusive(&r.lock);r.reserved-=old;add(r.status.canceled,old);
                r.status.observed_epoch=desired;r.status.stream_open=false;ReleaseSRWLockExclusive(&r.lock);
            }
            for(u32 index=0;index<2;++index){auto& engine=engines[index];const u32 rate=rates[index];
                const LONG device_change=engine.callback?InterlockedExchange(&engine.callback->dirty,0):0;
                if(device_change&&engine.client&&((device_change&2)||default_endpoint_changed(engine))){
                    u32 padding{};if(SUCCEEDED(engine.client->GetCurrentPadding(&padding)))retire_played(r,padding,rate);
                    notification_audio_mixer_rewind_rate(&r.mixer,rate);close_stream(r,engine);
                    AcquireSRWLockExclusive(&r.lock);add(r.status.switches);r.status.stream_open=false;ReleaseSRWLockExclusive(&r.lock);
                }
            }
            admit_requests(r);
            AcquireSRWLockShared(&r.lock);const bool epoch_changed=r.status.desired_epoch!=r.mixer.epoch||r.stopping;ReleaseSRWLockShared(&r.lock);
            if(epoch_changed){publish(r);continue;}
            for(u32 index=0;index<2;++index){auto& engine=engines[index];const u32 rate=rates[index];
                if(notification_audio_mixer_voices_rate(r.mixer,rate)&&!engine.client){
                    const HRESULT opened=open_engine(r,engine,rate);
                    if(FAILED(opened))failed_audio(r,engines,NotificationAudioError::device,opened);
                }
                if(engine.client){
                    const HRESULT rendered=render_audio(r,engine,rate);
                    if(FAILED(rendered))failed_audio(r,engines,NotificationAudioError::stream,rendered);
                    else if(!notification_audio_mixer_voices_rate(r.mixer,rate))close_stream(r,engine);
                }
            }
            const bool stream_open=engines[0].client||engines[1].client;
            AcquireSRWLockExclusive(&r.lock);r.status.stream_open=stream_open;ReleaseSRWLockExclusive(&r.lock);
            publish(r);
            HANDLE handles[]{r.stop,r.command,r.render};
            const u32 handle_count=(engines[0].client&&engines[0].started)||(engines[1].client&&engines[1].started)?3u:2u;
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
            if(fixture_hooks.before_wait)fixture_hooks.before_wait(fixture_hooks.context,handle_count);
#endif
            const DWORD waited=WaitForMultipleObjects(handle_count,handles,FALSE,INFINITE);
            if(waited==WAIT_OBJECT_0)break;
            if(waited==WAIT_FAILED){failed_audio(r,engines,NotificationAudioError::wait,HRESULT_FROM_WIN32(thread_last_error()));break;}
        }
        for(auto& e:engines)close_engine(r,e);
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
        if(fixture_hooks.uninitialize)fixture_hooks.uninitialize(fixture_hooks.context);
#else
        CoUninitialize();
#endif
    }
    AcquireSRWLockExclusive(&r.lock);add(r.status.canceled,r.reserved);r.reserved=r.count=r.head=0;
    clear_bytes(r.requests,sizeof(r.requests));notification_audio_mixer_reset(&r.mixer,r.status.desired_epoch);
    r.status.queued=r.status.outstanding=r.status.active=0;r.status.stream_open=false;r.status.observed_epoch=r.status.desired_epoch;
    r.status.worker_exited=true;notify_locked(r);ReleaseSRWLockExclusive(&r.lock);return 0;
}
void free_runtime(Runtime* r) noexcept {
    if(r->thread)CloseHandle(r->thread);if(r->command)CloseHandle(r->command);if(r->stop)CloseHandle(r->stop);if(r->render)CloseHandle(r->render);VirtualFree(r,0,MEM_RELEASE);
}
NotificationAudioResult launch(NotificationAudio& owner) noexcept {
    auto* r=static_cast<Runtime*>(VirtualAlloc(nullptr,sizeof(Runtime),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!r)return fail(NotificationAudioError::memory,thread_last_error());
    // Fresh committed VM is already zero. Only published/active descriptors
    // carry the clip default rate; every queue/voice insertion writes its full
    // slot, so no unused slot needs an aggregate initialization stencil.
    r->config=owner.config;r->status.desired_epoch=owner.epoch;r->status.enabled=owner.enabled;
    r->command=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!r->command){const u32 error=thread_last_error();free_runtime(r);return fail(NotificationAudioError::event,error);}
    r->stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!r->stop){const u32 error=thread_last_error();free_runtime(r);return fail(NotificationAudioError::event,error);}
    r->render=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!r->render){const u32 error=thread_last_error();free_runtime(r);return fail(NotificationAudioError::event,error);}
    r->thread=CreateThread(nullptr,128*1024,worker_main,r,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(!r->thread){const u32 error=thread_last_error();free_runtime(r);return fail(NotificationAudioError::thread,error);}
    owner.runtime=r;return {};
}
}
NotificationAudioResult notification_audio_initialize(NotificationAudio* owner,const NotificationAudioConfig& config) noexcept {
    if(!owner||owner->initialized||owner->runtime||!config.initial_epoch||
        (config.wake_window&&!config.wake_message)||(!config.wake_window&&config.wake_message))return fail(NotificationAudioError::invalid_input);
    *owner={config,nullptr,config.initial_epoch,true,config.enabled};return {};
}
bool notification_audio_can_accept(const NotificationAudio* owner,u32 needed) noexcept {
    if(!owner||!owner->initialized||!owner->enabled||!needed||needed>kNotificationAudioQueueCapacity)return false;
    auto* r=static_cast<Runtime*>(owner->runtime);if(!r)return true;
    AcquireSRWLockShared(&r->lock);const bool available=!r->stopping&&!r->status.worker_exited&&r->status.enabled&&
        needed<=kNotificationAudioQueueCapacity-r->count&&needed<=kNotificationAudioVoiceCapacity-r->reserved;ReleaseSRWLockShared(&r->lock);return available;
}
NotificationAudioResult notification_audio_submit(NotificationAudio* owner,const NotificationAudioRequest& request) noexcept {
    if(!owner||!owner->initialized||!notification_audio_gain_valid(request.gain)||
        (request.clip.samples.count?!notification_audio_clip_valid(request.clip):!owner->config.resolve||!request.clip_id))return fail(NotificationAudioError::invalid_input);
    if(request.epoch!=owner->epoch)return fail(NotificationAudioError::stale_epoch);
    if(!owner->enabled)return fail(NotificationAudioError::stopped);
    if(!owner->runtime){const auto result=launch(*owner);if(!result.ok())return result;}
    auto& r=*static_cast<Runtime*>(owner->runtime);AcquireSRWLockExclusive(&r.lock);
    NotificationAudioResult result{};
    if(r.stopping||r.status.worker_exited)result=fail(NotificationAudioError::stopped);
    else if(r.count==kNotificationAudioQueueCapacity)result=fail(NotificationAudioError::full_queue);
    else if(r.reserved==kNotificationAudioVoiceCapacity)result=fail(NotificationAudioError::full_voices);
    else {r.requests[(r.head+r.count)%kNotificationAudioQueueCapacity]=request;++r.count;++r.reserved;
        if(!SetEvent(r.command)){--r.count;--r.reserved;r.requests[(r.head+r.count)%kNotificationAudioQueueCapacity]={};result=fail(NotificationAudioError::event,thread_last_error());}
        else {add(r.status.accepted);notify_locked(r);}
    }
    ReleaseSRWLockExclusive(&r.lock);return result;
}
NotificationAudioResult notification_audio_reset(NotificationAudio* owner,u64 epoch,bool enabled) noexcept {
    if(!owner||!owner->initialized||!epoch)return fail(NotificationAudioError::invalid_input);
    if(epoch<=owner->epoch)return fail(NotificationAudioError::stale_epoch);
    auto* r=static_cast<Runtime*>(owner->runtime);
    if(r){AcquireSRWLockExclusive(&r->lock);
        if(r->stopping||r->status.worker_exited){ReleaseSRWLockExclusive(&r->lock);return fail(NotificationAudioError::stopped);}
        add(r->status.canceled,r->count);r->reserved-=r->count;r->count=r->head=0;clear_bytes(r->requests,sizeof(r->requests));
        r->status.desired_epoch=epoch;r->status.enabled=enabled;r->status.error=NotificationAudioError::none;r->status.native_error=0;owner->epoch=epoch;owner->enabled=enabled;
        const bool signal=SetEvent(r->command)!=0;const u32 error=signal?0:thread_last_error();notify_locked(*r);ReleaseSRWLockExclusive(&r->lock);
        return signal?NotificationAudioResult{}:fail(NotificationAudioError::event,error);
    }
    owner->epoch=epoch;owner->enabled=enabled;return {};
}
NotificationAudioStatus notification_audio_status(const NotificationAudio* owner) noexcept {
    if(!owner||!owner->initialized)return {};
    auto* r=static_cast<Runtime*>(owner->runtime);
    if(!r){NotificationAudioStatus status{};status.desired_epoch=status.observed_epoch=owner->epoch;status.enabled=owner->enabled;return status;}
    AcquireSRWLockExclusive(&r->lock);auto status=r->status;status.queued=r->count;status.outstanding=r->reserved;
    status.worker_started=true;status.stopping=r->stopping;r->wake_pending=false;r->last_notified=r->status;ReleaseSRWLockExclusive(&r->lock);return status;
}
NotificationAudioResult notification_audio_release(NotificationAudio* owner,u32 timeout) noexcept {
    if(!owner)return fail(NotificationAudioError::invalid_input);
    auto* r=static_cast<Runtime*>(owner->runtime);if(!r){*owner={};return {};}
    AcquireSRWLockExclusive(&r->lock);r->stopping=true;r->status.enabled=false;owner->enabled=false;
    const bool signaled=SetEvent(r->stop)!=0;const u32 signal_error=signaled?0:thread_last_error();ReleaseSRWLockExclusive(&r->lock);
    if(!signaled)return fail(NotificationAudioError::event,signal_error);
    const DWORD wait=WaitForSingleObject(r->thread,timeout);
    if(wait!=WAIT_OBJECT_0)return fail(wait==WAIT_TIMEOUT?NotificationAudioError::join_pending:NotificationAudioError::wait,
        wait==WAIT_TIMEOUT?WAIT_TIMEOUT:thread_last_error());
    free_runtime(r);*owner={};return {};
}
u32 notification_audio_owned_handles(const NotificationAudio* owner) noexcept {
    auto* r=owner?static_cast<Runtime*>(owner->runtime):nullptr;if(!r)return 0;
    AcquireSRWLockShared(&r->lock);const u32 n=u32(r->thread!=nullptr)+u32(r->command!=nullptr)+u32(r->stop!=nullptr)+u32(r->render!=nullptr)+r->callback_handles;
    ReleaseSRWLockShared(&r->lock);return n;
}
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
HANDLE notification_audio_fixture_callback_event(IMMNotificationClient* callback) noexcept {
    return callback?static_cast<EndpointNotify*>(callback)->command:nullptr;
}
#endif
} // namespace maicord
