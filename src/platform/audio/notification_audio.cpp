#include "audio_support.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/sys/wasapi.hpp"
#include "wasapi_endpoint_watch.hpp"
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
#include "maicord/platform/audio/wasapi/notification_audio_fixture.hpp"
#endif

namespace maicord {
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
namespace { NotificationAudioFixtureHooks fixture_hooks{}; }
void notification_audio_fixture_hooks(const NotificationAudioFixtureHooks* hooks) noexcept {fixture_hooks=hooks?*hooks:NotificationAudioFixtureHooks{};}
#endif
namespace {
using audio_detail::add;
using audio_detail::fail;
}

namespace {
struct Runtime {
    SRWLOCK lock{};NotificationAudioConfig config{};
    HANDLE thread{},command{},stop{},render{};
    NotificationAudioRequest requests[kNotificationAudioQueueCapacity]{};
    NotificationAudioMixer mixer{};
    NotificationAudioStatus status{},last_notified{};
    u32 head{},count{},reserved{},callback_handles{};
    bool stopping{},wake_pending{};
};
static_assert(sizeof(Runtime)+2*sizeof(PinnedEndpointNotify)<=16*1024,"notification audio metadata remains bounded");
struct Engine {
    IMMDeviceEnumerator* enumerator{};PinnedEndpointNotify* callback{};
    IAudioClient* client{};IAudioRenderClient* render{};u32 frames{};bool started{};
};
void notify_locked(Runtime& r) noexcept {
    if(bytes_equal(&r.status,&r.last_notified,sizeof(r.status)))return;
    if(!r.wake_pending&&r.config.wake.window){
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
        r.wake_pending=fixture_hooks.post_wake&&fixture_hooks.post_wake(fixture_hooks.context,r.config.wake);
#else
        r.wake_pending=window_post(r.config.wake,0,0);
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
__forceinline void close_callback(Runtime& r,Engine& e) noexcept {
    pinned_endpoint_close(e.enumerator,&e.callback,&r.lock,&r.callback_handles);
}
void close_engine(Runtime& r,Engine& e) noexcept {
    close_callback(r,e);close_stream(r,e);com_release(e.enumerator);
}
HRESULT refresh_callback(Runtime& r,Engine& e,LPWSTR* id) noexcept {
    return pinned_endpoint_refresh(e.enumerator,&e.callback,r.command,&r.lock,&r.callback_handles,id);
}
// The default-role notification is advisory. Revalidate the actual render
// endpoint on the worker before disturbing an otherwise unchanged stream.
bool default_endpoint_changed(Engine& e) noexcept {return pinned_endpoint_default_changed(e.enumerator,e.callback);}
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
        request=r.requests[r.head];reset_value(&r.requests[r.head]);r.head=(r.head+1)%kNotificationAudioQueueCapacity;--r.count;
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
    static constexpr u32 rates[]{kNotificationAudioRate,kNotificationAudioAlternateRate};
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
    if(r->thread)CloseHandle(r->thread);if(r->command)CloseHandle(r->command);if(r->stop)CloseHandle(r->stop);if(r->render)CloseHandle(r->render);pages_release(r);
}
NotificationAudioResult launch(NotificationAudio& owner) noexcept {
    auto* r=commit_pages<Runtime>();
    if(!r)return fail(NotificationAudioError::memory,thread_last_error());
    // VirtualAlloc memory is zeroed; every queue and voice slot is written in full on insertion.
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
        config.wake.cause>wake_tray||(!config.wake.window&&config.wake.cause))return fail(NotificationAudioError::invalid_input);
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
        if(!SetEvent(r.command)){--r.count;--r.reserved;reset_value(&r.requests[(r.head+r.count)%kNotificationAudioQueueCapacity]);result=fail(NotificationAudioError::event,thread_last_error());}
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
    auto* r=static_cast<Runtime*>(owner->runtime);if(!r){reset_value(owner);return {};}
    AcquireSRWLockExclusive(&r->lock);r->stopping=true;r->status.enabled=false;owner->enabled=false;
    const bool signaled=SetEvent(r->stop)!=0;const u32 signal_error=signaled?0:thread_last_error();ReleaseSRWLockExclusive(&r->lock);
    if(!signaled)return fail(NotificationAudioError::event,signal_error);
    const DWORD wait=WaitForSingleObject(r->thread,timeout);
    if(wait!=WAIT_OBJECT_0)return fail(wait==WAIT_TIMEOUT?NotificationAudioError::join_pending:NotificationAudioError::wait,
        wait==WAIT_TIMEOUT?WAIT_TIMEOUT:thread_last_error());
    free_runtime(r);reset_value(owner);return {};
}
u32 notification_audio_owned_handles(const NotificationAudio* owner) noexcept {
    auto* r=owner?static_cast<Runtime*>(owner->runtime):nullptr;if(!r)return 0;
    AcquireSRWLockShared(&r->lock);const u32 n=u32(r->thread!=nullptr)+u32(r->command!=nullptr)+u32(r->stop!=nullptr)+u32(r->render!=nullptr)+r->callback_handles;
    ReleaseSRWLockShared(&r->lock);return n;
}
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
HANDLE notification_audio_fixture_callback_event(IMMNotificationClient* callback) noexcept {
    return callback?static_cast<PinnedEndpointNotify*>(callback)->event:nullptr;
}
#endif
} // namespace maicord
