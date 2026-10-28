#include "maicord/platform/media_audio.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/wasapi.hpp"
#include "audio_endpoint_watch.hpp"

namespace maicord {namespace {
using Error=MediaAudioError;
bool disjoint(const void* a,u32 an,const void* b,u32 bn) noexcept {
    const ConstSlice<u8> x{static_cast<const u8*>(a),an},y{static_cast<const u8*>(b),bn};
    return byte_range_valid(x)&&byte_range_valid(y)&&!byte_ranges_overlap(x,y);
}
bool rate_valid(u32 rate) noexcept { return rate >= 8000 && rate <= 192000; }
bool ring_valid(const MediaAudioRing& r) noexcept {
    if(!r.samples.data||(reinterpret_cast<usize>(r.samples.data)&3)||!r.epoch||!rate_valid(r.sample_rate)||(r.channels!=1&&r.channels!=2)||
        r.samples.count%r.channels||r.samples.count/r.channels<kMediaAudioMinimumRingFrames||r.samples.count/r.channels>kMediaAudioMaximumRingFrames||
        !byte_range_valid({reinterpret_cast<const u8*>(r.samples.data),r.samples.count*4}))return false;
    return r.consumed<=r.submitted&&r.submitted<=r.written&&r.written-r.consumed<=r.samples.count/r.channels;
}
MediaAudioResult fail(Error e,u32 native=0) noexcept {return {e,native};}
}
bool media_audio_gain_valid(float gain) noexcept {return is_finite(gain)&&gain>=0&&gain<=1;}
bool media_audio_ring_initialize(MediaAudioRing* r,Slice<float> samples,u32 rate,u32 channels,u64 epoch) noexcept {
    if(!r||!disjoint(r,sizeof(*r),samples.data,samples.count>32768?0:samples.count*4)||samples.count>32768)return false;
    MediaAudioRing candidate{samples,epoch,0,0,0,channels,rate,false};if(!ring_valid(candidate))return false;*r=candidate;return true;
}
u32 media_audio_ring_free(const MediaAudioRing& r) noexcept {
    return ring_valid(r)?r.samples.count/r.channels-static_cast<u32>(r.written-r.consumed):0;
}
MediaAudioResult media_audio_ring_write(MediaAudioRing* r,u64 epoch,ConstSlice<float> pcm) noexcept {
    if(!r||!ring_valid(*r)||!pcm.count||pcm.count%r->channels||pcm.count/r->channels>kMediaAudioMaximumWriteFrames||
        (reinterpret_cast<usize>(pcm.data)&3)||!disjoint(r,sizeof(*r),pcm.data,pcm.count*4)||
        !disjoint(r->samples.data,r->samples.count*4,pcm.data,pcm.count*4))return fail(Error::invalid_input);
    if(epoch!=r->epoch)return fail(Error::stale_epoch);if(r->ended)return fail(Error::stopped);
    const u32 frames=pcm.count/r->channels;if(frames>media_audio_ring_free(*r))return fail(Error::capacity);
    if(r->written>~u64{0}-frames)return fail(Error::timeline);
    for(u32 i=0;i<pcm.count;++i)if(!is_finite(pcm[i]))return fail(Error::invalid_input);
    const u32 capacity=r->samples.count/r->channels;
    u32 frame_index=static_cast<u32>(r->written%capacity);
    for(u32 i=0;i<frames;++i){const u32 index=frame_index*r->channels;
        for(u32 c=0;c<r->channels;++c)r->samples[index+c]=pcm[i*r->channels+c];
        if(++frame_index==capacity)frame_index=0;}
    r->written+=frames;return {};
}
u32 media_audio_ring_render(MediaAudioRing* r,Slice<float> output,float gain) noexcept {
    if(!r||!ring_valid(*r)||!media_audio_gain_valid(gain)||!output.count||output.count%r->channels||
        output.count/r->channels>kMediaAudioMaximumWriteFrames||(reinterpret_cast<usize>(output.data)&3)||
        !disjoint(r,sizeof(*r),output.data,output.count*4)||!disjoint(r->samples.data,r->samples.count*4,output.data,output.count*4))return 0;
    const u32 count=static_cast<u32>(minimum<u64>(output.count/r->channels,r->written-r->submitted)),capacity=r->samples.count/r->channels;
    u32 frame_index=static_cast<u32>(r->submitted%capacity);
    for(u32 i=0;i<count;++i){const u32 index=frame_index*r->channels;
        for(u32 c=0;c<r->channels;++c){const float value=r->samples[index+c]*gain;output[i*r->channels+c]=value>1?1:value< -1? -1:value;}
        if(++frame_index==capacity)frame_index=0;}
    r->submitted+=count;return count;
}
bool media_audio_ring_native_queue_empty(const MediaAudioRing& r,u32 padding) noexcept {
    return ring_valid(r)&&!padding&&r.submitted==r.consumed;
}
void media_audio_ring_rewind(MediaAudioRing* r) noexcept {if(r&&ring_valid(*r))r->submitted=r->consumed;}
bool media_audio_clock_time(u64 position,u64 frequency,u64* time) noexcept {
    return media_audio_clock_frames(position,frequency,10000000,time);
}
bool media_audio_clock_frames(u64 position,u64 frequency,u32 rate,u64* frames) noexcept {
    if(!frames||!frequency||!rate)return false;
    const u64 whole=position/frequency,remainder=position%frequency;
    if(whole>~u64{0}/rate||remainder>~u64{0}/rate)return false;
    const u64 base=whole*rate,fraction=remainder*rate/frequency;
    if(base>~u64{0}-fraction)return false;*frames=base+fraction;return true;
}
namespace {
struct Runtime {
    SRWLOCK lock{};MediaAudioConfig config{};MediaAudioRing ring{};MediaAudioStatus status{},last_notified{};
    HANDLE thread{},command{},stop{},render{};u64 observed{},command_serial{};i64 origin{};float volume{};
    bool desired_play{},stopping{},failed{},wake_pending{},refill_pending{};u32 callback_handles{};
};
static_assert(sizeof(Runtime)+sizeof(PinnedEndpointNotify)<=4096);
struct Engine {
    IMMDeviceEnumerator* enumerator{};PinnedEndpointNotify* callback{};IAudioClient* client{};IAudioRenderClient* render{};
    IAudioClock* clock{};ISimpleAudioVolume* volume{};GUID session{};
    u64 frequency{},anchor_position{},anchor_frames{};u32 frames{};float applied_volume{-1};bool started{},session_ready{},anchor_valid{};
};
void notify_locked(Runtime& r) noexcept {
    r.status.written_frames=r.ring.written;r.status.submitted_frames=r.ring.submitted;r.status.consumed_frames=r.ring.consumed;
    r.status.free_frames=media_audio_ring_free(r.ring);r.status.observed_epoch=r.observed;
    r.status.owned_handles=u32(r.thread!=nullptr)+u32(r.command!=nullptr)+u32(r.stop!=nullptr)+u32(r.render!=nullptr)+r.callback_handles;
    const auto& a=r.status;const auto& b=r.last_notified;
    const bool changed=a.desired_epoch!=b.desired_epoch||a.observed_epoch!=b.observed_epoch||a.error!=b.error||a.native_error!=b.native_error||
        a.playing!=b.playing||a.paused!=b.paused||a.buffering!=b.buffering||a.ended!=b.ended||a.stream_open!=b.stream_open||
        a.worker_started!=b.worker_started||a.worker_exited!=b.worker_exited||a.clock_valid!=b.clock_valid||
        a.desired_volume!=b.desired_volume||a.applied_volume!=b.applied_volume||a.volume_valid!=b.volume_valid;
    if(!changed)return;
    if(r.config.status_event&&!SetEvent(r.config.status_event))r.status.wake_error=GetLastError();
    if(!r.wake_pending&&r.config.wake_window&&r.config.wake_message){r.wake_pending=PostMessageW(r.config.wake_window,r.config.wake_message,0,0)!=0;
        if(!r.wake_pending)r.status.wake_error=GetLastError();}r.last_notified=r.status;
}
void close_callback(Runtime& r,Engine& e) noexcept {
    const bool attached=e.callback&&e.callback->event;if(e.callback)InterlockedExchange(&e.callback->active,0);
    if(e.enumerator&&e.callback)e.enumerator->UnregisterEndpointNotificationCallback(e.callback);com_release(e.callback);
    if(attached){AcquireSRWLockExclusive(&r.lock);--r.callback_handles;ReleaseSRWLockExclusive(&r.lock);}
}
HRESULT close_client(Engine& e) noexcept {
    HRESULT h=S_OK;if(e.client){if(e.started)h=e.client->Stop();const HRESULT reset=e.client->Reset();if(SUCCEEDED(h))h=reset;}
    e.started=false;e.frames=0;e.frequency=0;e.applied_volume=-1;e.anchor_valid=false;
    com_release(e.volume);com_release(e.clock);com_release(e.render);com_release(e.client);return h;
}
void failure(Runtime& r,Engine& e,u64 epoch,u64 serial,Error error,HRESULT native) noexcept {
    close_client(e);AcquireSRWLockExclusive(&r.lock);
    if(r.ring.epoch==epoch)media_audio_ring_rewind(&r.ring);
    if(r.status.desired_epoch==epoch&&r.command_serial==serial){r.failed=true;r.status.error=error;r.status.native_error=static_cast<u32>(native);
        r.status.playing=false;r.status.paused=false;r.status.stream_open=false;r.status.clock_valid=r.status.volume_valid=false;notify_locked(r);}
    ReleaseSRWLockExclusive(&r.lock);
}
HRESULT callback(Runtime& r,Engine& e,LPWSTR* id) noexcept {
    if(!*id||!**id)return E_UNEXPECTED;if(e.callback&&e.callback->matches(*id))return S_OK;
    close_callback(r,e);void* memory=CoTaskMemAlloc(sizeof(PinnedEndpointNotify));if(!memory)return E_OUTOFMEMORY;
    e.callback=new(memory)PinnedEndpointNotify;e.callback->id=*id;*id=nullptr;while(e.callback->id[e.callback->id_chars])++e.callback->id_chars;
    if(!DuplicateHandle(GetCurrentProcess(),r.command,GetCurrentProcess(),&e.callback->event,0,FALSE,DUPLICATE_SAME_ACCESS))return HRESULT_FROM_WIN32(GetLastError());
    AcquireSRWLockExclusive(&r.lock);++r.callback_handles;ReleaseSRWLockExclusive(&r.lock);
    const HRESULT h=e.enumerator->RegisterEndpointNotificationCallback(e.callback);
    if(FAILED(h))close_callback(r,e);return h;
}
bool changed(Engine& e) noexcept {
    IMMDevice* device=nullptr;LPWSTR id=nullptr;HRESULT h=e.enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device);
    if(SUCCEEDED(h))h=device->GetId(&id);const bool different=FAILED(h)||!e.callback||!e.callback->matches(id);
    CoTaskMemFree(id);com_release(device);return different;
}
HRESULT open(Runtime& r,Engine& e) noexcept {
    HRESULT h=S_OK;if(!e.enumerator)h=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&e.enumerator));
    IMMDevice* device=nullptr;LPWSTR id=nullptr;if(SUCCEEDED(h))h=e.enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device);
    if(SUCCEEDED(h))h=device->GetId(&id);if(SUCCEEDED(h))h=callback(r,e,&id);CoTaskMemFree(id);
    if(SUCCEEDED(h))h=device->Activate(__uuidof(IAudioClient),CLSCTX_INPROC_SERVER,nullptr,reinterpret_cast<void**>(&e.client));
    if(SUCCEEDED(h)&&!e.session_ready){h=CoCreateGuid(&e.session);e.session_ready=SUCCEEDED(h);}
    const u16 channels=static_cast<u16>(r.config.channels),stride=static_cast<u16>(channels*4);
    const WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT,channels,r.config.sample_rate,r.config.sample_rate*stride,stride,32,0};
    if(SUCCEEDED(h))h=e.client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY|AUDCLNT_STREAMFLAGS_NOPERSIST,0,0,&format,&e.session);
    if(SUCCEEDED(h))h=e.client->SetEventHandle(r.render);if(SUCCEEDED(h))h=e.client->GetBufferSize(&e.frames);
    if(SUCCEEDED(h)&&(!e.frames||e.frames>kMediaAudioMaximumRingFrames))h=E_UNEXPECTED;
    if(SUCCEEDED(h))h=e.client->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void**>(&e.render));
    if(SUCCEEDED(h))h=e.client->GetService(__uuidof(IAudioClock),reinterpret_cast<void**>(&e.clock));
    if(SUCCEEDED(h))h=e.client->GetService(__uuidof(ISimpleAudioVolume),reinterpret_cast<void**>(&e.volume));
    if(SUCCEEDED(h))h=e.clock->GetFrequency(&e.frequency);if(SUCCEEDED(h)&&!e.frequency)h=E_UNEXPECTED;
    e.anchor_position=0;AcquireSRWLockShared(&r.lock);e.anchor_frames=r.ring.consumed;ReleaseSRWLockShared(&r.lock);
    com_release(device);return h;
}
HRESULT clock_snapshot(Runtime& r,Engine& e,u64 epoch,u32 padding) noexcept {
    u64 position{},elapsed{},advanced{},anchor_time{};HRESULT h=e.clock->GetPosition(&position,nullptr);
    if(FAILED(h))return h;
    // GetPosition/GetFrequency units are compatible but not necessarily frames:
    // https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition
    // Keep native100ns clock precision for presentation independently from the
    // direct single-floor source-frame conversion used for retained PCM/EOS.
    if(position<e.anchor_position||!media_audio_clock_time(position-e.anchor_position,e.frequency,&elapsed)||
        !media_audio_clock_frames(position-e.anchor_position,e.frequency,r.config.sample_rate,&advanced)||
        !media_audio_clock_time(e.anchor_frames,r.config.sample_rate,&anchor_time)||
        advanced>~u64{0}-e.anchor_frames||elapsed>~u64{0}-anchor_time)return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    AcquireSRWLockExclusive(&r.lock);
    if(r.status.desired_epoch!=epoch||r.ring.epoch!=epoch){ReleaseSRWLockExclusive(&r.lock);return S_OK;}
    if(padding>r.ring.submitted-r.ring.consumed){ReleaseSRWLockExclusive(&r.lock);return E_UNEXPECTED;}
    u64 submitted_time{};
    if(!media_audio_clock_time(r.ring.submitted,r.config.sample_rate,&submitted_time)){
        ReleaseSRWLockExclusive(&r.lock);return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);}
    const u64 content_time=minimum(anchor_time+elapsed,submitted_time);
    if(content_time>static_cast<u64>(0x7fffffffffffffffll)||r.origin>0x7fffffffffffffffll-static_cast<i64>(content_time)){
        ReleaseSRWLockExclusive(&r.lock);return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);}
    const u64 played=minimum(r.ring.submitted-padding,minimum(r.ring.submitted,e.anchor_frames+advanced));
    const u64 old=r.ring.consumed;r.ring.consumed=maximum(old,played);
    r.status.clock_valid=true;r.status.clock_frequency=e.frequency;
    r.status.presentation_time_100ns=r.origin+static_cast<i64>(content_time);
    if(r.ring.consumed>old&&media_audio_ring_free(r.ring)>=kMediaAudioMaximumWriteFrames&&!r.refill_pending&&r.config.refill_event){
        r.refill_pending=SetEvent(r.config.refill_event)!=0;if(!r.refill_pending)r.status.wake_error=GetLastError();}
    notify_locked(r);ReleaseSRWLockExclusive(&r.lock);return S_OK;
}
HRESULT render(Runtime& r,Engine& e,u64 epoch,bool playing) noexcept {
    u32 padding{};HRESULT h=e.client->GetCurrentPadding(&padding);if(FAILED(h))return h;if(padding>e.frames)return E_UNEXPECTED;
    h=clock_snapshot(r,e,epoch,padding);if(FAILED(h))return h;
    if(!playing){if(e.started){h=e.client->Stop();if(SUCCEEDED(h))e.started=false;}if(FAILED(h))return h;
        h=e.client->GetCurrentPadding(&padding);if(FAILED(h))return h;
        return clock_snapshot(r,e,epoch,padding);}
    AcquireSRWLockShared(&r.lock);const bool live=r.status.desired_epoch==epoch&&!r.stopping&&r.desired_play;
    const u32 frames=live?static_cast<u32>(minimum<u64>(e.frames-padding,r.ring.written-r.ring.submitted)):0;
    const bool queue_empty=media_audio_ring_native_queue_empty(r.ring,padding);ReleaseSRWLockShared(&r.lock);
    if(!live)return S_OK;
    // The endpoint clock can advance while its submitted prefix is empty.
    // Future producer PCM must not inherit that unsupplied elapsed interval.
    // Stop/reanchor before refill even when written is already ahead of played.
    // Pause retains its original anchor while an actual native queue remains.
    if(queue_empty){if(e.started){h=e.client->Stop();if(FAILED(h))return h;e.started=false;}e.anchor_valid=false;}
    if(frames){BYTE* target=nullptr;h=e.render->GetBuffer(frames,&target);if(FAILED(h))return h;
        bool valid=target!=nullptr;u32 at{};
        while(valid&&at<frames){const u32 chunk=minimum(kMediaAudioMaximumWriteFrames,frames-at);
            AcquireSRWLockExclusive(&r.lock);valid=!r.stopping&&r.desired_play&&r.status.desired_epoch==epoch&&r.ring.epoch==epoch;
            if(valid)valid=media_audio_ring_render(&r.ring,{reinterpret_cast<float*>(target)+at*r.config.channels,chunk*r.config.channels},1)==chunk;
            ReleaseSRWLockExclusive(&r.lock);at+=chunk;}
        AcquireSRWLockShared(&r.lock);valid=valid&&r.status.desired_epoch==epoch&&!r.stopping&&r.desired_play;ReleaseSRWLockShared(&r.lock);
        h=e.render->ReleaseBuffer(frames,valid?0:AUDCLNT_BUFFERFLAGS_SILENT);if(FAILED(h))return h;
        if(!valid){
            // A pause/reset can arrive while COM owns its buffer. Do not count
            // SILENT canceled submission as source PCM or let it shift the
            // future audio clock: reset the old client and replay retained PCM.
            h=close_client(e);AcquireSRWLockExclusive(&r.lock);
            if(r.ring.epoch==epoch){media_audio_ring_rewind(&r.ring);r.status.clock_valid=false;}
            ReleaseSRWLockExclusive(&r.lock);return h;
        }
    }
    AcquireSRWLockShared(&r.lock);const bool restart=!e.started&&r.desired_play&&!r.stopping&&r.status.desired_epoch==epoch&&r.ring.submitted>r.ring.consumed;
    ReleaseSRWLockShared(&r.lock);
    if(restart){
        if(!e.anchor_valid){u64 position{};h=e.clock->GetPosition(&position,nullptr);if(FAILED(h))return h;
            AcquireSRWLockShared(&r.lock);e.anchor_frames=r.ring.consumed;ReleaseSRWLockShared(&r.lock);e.anchor_position=position;e.anchor_valid=true;}
        h=e.client->Start();if(SUCCEEDED(h))e.started=true;
    }
    return h;
}
DWORD WINAPI worker(void* pointer) noexcept {
    auto& r=*static_cast<Runtime*>(pointer);Engine e{};
    // The command start gate publishes the creator's thread handle before any
    // worker snapshot uses it. Stop also wakes a failed launch without opening
    // COM/device resources; no suspended thread needs forced termination.
    HANDLE startup[]{r.stop,r.command};const DWORD started=WaitForMultipleObjects(2,startup,FALSE,INFINITE);
    if(started!=WAIT_OBJECT_0+1){AcquireSRWLockExclusive(&r.lock);r.status.worker_exited=true;
        if(started==WAIT_FAILED){r.status.error=Error::wait;r.status.native_error=GetLastError();}
        if(r.config.status_event)SetEvent(r.config.status_event);ReleaseSRWLockExclusive(&r.lock);return 0;}
    const HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(apartment)){AcquireSRWLockExclusive(&r.lock);r.failed=true;r.status.error=Error::device;r.status.native_error=static_cast<u32>(apartment);ReleaseSRWLockExclusive(&r.lock);}
    else for(;;){
        AcquireSRWLockShared(&r.lock);const bool stopping=r.stopping,playing=r.desired_play,failed=r.failed;
        const u64 epoch=r.status.desired_epoch,serial=r.command_serial;const float volume=r.volume;ReleaseSRWLockShared(&r.lock);if(stopping)break;
        if(r.observed!=epoch){close_client(e);close_callback(r,e);
            AcquireSRWLockExclusive(&r.lock);if(r.status.desired_epoch==epoch){r.observed=epoch;r.status.clock_valid=false;r.status.presentation_time_100ns=r.origin;
                r.status.stream_open=false;r.status.playing=false;r.status.paused=!playing;r.status.ended=false;
                // Closing prior-epoch error cannot contaminate the fresh epoch.
                notify_locked(r);}
            ReleaseSRWLockExclusive(&r.lock);}
        const LONG dirty=e.callback?InterlockedExchange(&e.callback->dirty,0):0;
        if(dirty&&e.client&&((dirty&2)||changed(e))){
            u32 padding{};if(SUCCEEDED(e.client->GetCurrentPadding(&padding)))clock_snapshot(r,e,epoch,padding);
            close_client(e);close_callback(r,e);AcquireSRWLockExclusive(&r.lock);if(r.ring.epoch==epoch)media_audio_ring_rewind(&r.ring);ReleaseSRWLockExclusive(&r.lock);}
        AcquireSRWLockShared(&r.lock);const bool current=r.status.desired_epoch==epoch&&r.observed==epoch&&!r.stopping;
        const bool available=r.ring.written!=r.ring.consumed;ReleaseSRWLockShared(&r.lock);if(!current)continue;
        if(playing&&!failed&&available&&!e.client){const HRESULT h=open(r,e);if(FAILED(h))failure(r,e,epoch,serial,Error::device,h);}
        if(e.client){HRESULT h=S_OK;if(volume!=e.applied_volume){h=e.volume->SetMasterVolume(volume,nullptr);if(SUCCEEDED(h))e.applied_volume=volume;}
            if(SUCCEEDED(h))h=render(r,e,epoch,playing);if(FAILED(h))failure(r,e,epoch,serial,Error::stream,h);}
        AcquireSRWLockExclusive(&r.lock);
        if(r.status.desired_epoch==epoch){r.status.stream_open=e.client!=nullptr;r.status.playing=e.started;
            if(!e.client)r.status.clock_valid=false;
            r.status.desired_volume=r.volume;r.status.applied_volume=e.applied_volume>=0?e.applied_volume:0;r.status.volume_valid=e.client&&e.applied_volume>=0;
            r.status.paused=!r.desired_play&&!r.failed;r.status.buffering=r.desired_play&&!e.started&&!r.ring.ended&&!r.failed;
            r.status.ended=r.ring.ended&&r.ring.written==r.ring.consumed&&!e.started;notify_locked(r);}
        const bool repeat=r.status.desired_epoch!=epoch||r.stopping;ReleaseSRWLockExclusive(&r.lock);if(repeat)continue;
        HANDLE events[]{r.stop,r.command,r.render};const DWORD waited=WaitForMultipleObjects(e.started?3u:2u,events,FALSE,INFINITE);
        if(waited==WAIT_OBJECT_0)break;if(waited==WAIT_FAILED){failure(r,e,epoch,serial,Error::wait,HRESULT_FROM_WIN32(GetLastError()));break;}
    }
    if(SUCCEEDED(apartment)){close_callback(r,e);close_client(e);com_release(e.enumerator);CoUninitialize();}
    AcquireSRWLockExclusive(&r.lock);r.status.worker_exited=true;r.status.stream_open=r.status.playing=false;r.status.clock_valid=r.status.volume_valid=false;notify_locked(r);ReleaseSRWLockExclusive(&r.lock);return 0;
}
MediaAudioResult free_runtime(MediaAudio& owner) noexcept {
    auto& r=*static_cast<Runtime*>(owner.runtime);HANDLE* handles[]{&r.thread,&r.command,&r.stop,&r.render};
    for(auto* handle:handles)if(*handle){if(!CloseHandle(*handle))return fail(Error::release,GetLastError());*handle=nullptr;}
    if(!VirtualFree(&r,0,MEM_RELEASE))return fail(Error::release,GetLastError());owner.runtime=nullptr;return {};
}
MediaAudioResult launch(MediaAudio& owner) noexcept {
    auto* r=static_cast<Runtime*>(VirtualAlloc(nullptr,sizeof(Runtime),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!r)return fail(Error::memory,GetLastError());
    owner.runtime=r;r->config=owner.config;r->ring=owner.ring;r->observed=owner.epoch;r->status.desired_epoch=r->status.observed_epoch=owner.epoch;
    r->origin=owner.config.source_origin_100ns;r->volume=owner.config.volume;r->status.desired_volume=r->volume;r->desired_play=true;r->command_serial=1;r->status.worker_started=true;
    r->command=CreateEventW(nullptr,FALSE,FALSE,nullptr);r->stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);r->render=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!r->command||!r->stop||!r->render){const DWORD error=GetLastError();r->status.worker_exited=true;free_runtime(owner);return fail(Error::event,error);}
    r->thread=CreateThread(nullptr,128*1024,worker,r,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(!r->thread){const DWORD error=GetLastError();r->status.worker_exited=true;free_runtime(owner);return fail(Error::thread,error);}
    if(!SetEvent(r->command)||(r->config.refill_event&&!SetEvent(r->config.refill_event)))return fail(Error::event,GetLastError());return {};
}
MediaAudioResult command(MediaAudio* owner,u32 kind,float volume=0) noexcept {
    if(!owner||!owner->initialized)return fail(Error::invalid_input);auto* r=static_cast<Runtime*>(owner->runtime);
    if(!r){if(kind==1)return launch(*owner);if(kind==3)owner->config.volume=volume;return {};}
    AcquireSRWLockExclusive(&r->lock);MediaAudioResult result{};
    if(r->stopping||r->status.worker_exited)result=fail(Error::stopped);
    else if(kind==1&&r->command_serial==~u64{0})result=fail(Error::timeline);
    else {if(kind==1){++r->command_serial;r->desired_play=true;r->failed=false;r->status.error=Error::none;r->status.native_error=0;}
        else if(kind==2)r->desired_play=false;else {r->volume=volume;r->status.desired_volume=volume;owner->config.volume=volume;}
        if(!SetEvent(r->command))result=fail(Error::event,GetLastError());}
    ReleaseSRWLockExclusive(&r->lock);return result;
}
}
MediaAudioResult media_audio_initialize(MediaAudio* owner,const MediaAudioConfig& config) noexcept {
    if(!owner||owner->initialized||owner->runtime||!media_audio_gain_valid(config.volume)||
        !disjoint(owner,sizeof(*owner),&config,sizeof(config))||config.pcm.count>32768||
        !disjoint(owner,sizeof(*owner),config.pcm.data,config.pcm.count*4))return fail(Error::invalid_input);
    MediaAudioRing ring{};if(!media_audio_ring_initialize(&ring,config.pcm,config.sample_rate,config.channels,config.initial_epoch))return fail(Error::invalid_input);
    owner->config=config;owner->ring=ring;owner->epoch=config.initial_epoch;owner->initialized=true;return {};
}
MediaAudioResult media_audio_play(MediaAudio* owner) noexcept{return command(owner,1);}
MediaAudioResult media_audio_pause(MediaAudio* owner) noexcept{return command(owner,2);}
MediaAudioResult media_audio_volume(MediaAudio* owner,float volume) noexcept {return media_audio_gain_valid(volume)?command(owner,3,volume):fail(Error::invalid_input);}
MediaAudioResult media_audio_write(MediaAudio* owner,u64 epoch,ConstSlice<float> pcm) noexcept {
    if(!owner||!owner->initialized||pcm.count>4096||!disjoint(owner,sizeof(*owner),pcm.data,pcm.count*4))return fail(Error::invalid_input);
    auto* r=static_cast<Runtime*>(owner->runtime);
    if(!r)return media_audio_ring_write(&owner->ring,epoch,pcm);
    if(!disjoint(r,sizeof(*r),pcm.data,pcm.count*4))return fail(Error::invalid_input);
    AcquireSRWLockExclusive(&r->lock);MediaAudioResult result=r->stopping||r->status.worker_exited?fail(Error::stopped):media_audio_ring_write(&r->ring,epoch,pcm);
    if(result.ok()){r->refill_pending=false;if(!SetEvent(r->command))result=fail(Error::event,GetLastError());}
    ReleaseSRWLockExclusive(&r->lock);return result;
}
MediaAudioResult media_audio_end(MediaAudio* owner,u64 epoch) noexcept {
    if(!owner||!owner->initialized)return fail(Error::invalid_input);auto* r=static_cast<Runtime*>(owner->runtime);
    if(!r){if(epoch!=owner->epoch)return fail(Error::stale_epoch);owner->ring.ended=true;return {};}
    AcquireSRWLockExclusive(&r->lock);MediaAudioResult result{};
    if(r->stopping||r->status.worker_exited)result=fail(Error::stopped);else if(epoch!=r->ring.epoch)result=fail(Error::stale_epoch);
    else {r->ring.ended=true;if(!SetEvent(r->command))result=fail(Error::event,GetLastError());}ReleaseSRWLockExclusive(&r->lock);return result;
}
MediaAudioResult media_audio_reset(MediaAudio* owner,u64 epoch,i64 origin) noexcept {
    if(!owner||!owner->initialized||!epoch)return fail(Error::invalid_input);if(epoch<=owner->epoch)return fail(Error::stale_epoch);
    auto* r=static_cast<Runtime*>(owner->runtime);if(!r){owner->ring.epoch=epoch;owner->ring.written=owner->ring.submitted=owner->ring.consumed=0;
        owner->ring.ended=false;owner->epoch=epoch;owner->config.source_origin_100ns=origin;return {};}
    AcquireSRWLockExclusive(&r->lock);MediaAudioResult result{};
    if(r->stopping||r->status.worker_exited)result=fail(Error::stopped);
    else {r->ring.epoch=epoch;r->ring.written=r->ring.submitted=r->ring.consumed=0;r->ring.ended=false;
        r->status.desired_epoch=epoch;r->origin=origin;r->failed=false;r->refill_pending=false;r->status.error=Error::none;r->status.native_error=0;
        owner->epoch=epoch;owner->config.source_origin_100ns=origin;
        if(!SetEvent(r->command)||(r->config.refill_event&&!SetEvent(r->config.refill_event)))result=fail(Error::event,GetLastError());}
    ReleaseSRWLockExclusive(&r->lock);return result;
}
MediaAudioStatus media_audio_status(const MediaAudio* owner) noexcept {
    if(!owner||!owner->initialized)return {};auto* r=static_cast<Runtime*>(owner->runtime);
    if(!r){MediaAudioStatus status{};status.desired_epoch=status.observed_epoch=owner->epoch;status.written_frames=owner->ring.written;
        status.free_frames=media_audio_ring_free(owner->ring);status.presentation_time_100ns=owner->config.source_origin_100ns;status.desired_volume=owner->config.volume;
        status.paused=true;status.ended=owner->ring.ended;return status;}
    AcquireSRWLockExclusive(&r->lock);notify_locked(*r);auto status=r->status;r->wake_pending=false;ReleaseSRWLockExclusive(&r->lock);return status;
}
MediaAudioResult media_audio_release(MediaAudio* owner,u32 timeout) noexcept {
    if(!owner)return fail(Error::invalid_input);auto* r=static_cast<Runtime*>(owner->runtime);if(!r){reset_value(owner);return {};}
    if(r->thread){AcquireSRWLockExclusive(&r->lock);r->stopping=true;r->desired_play=false;
        const bool signaled=SetEvent(r->stop)!=0;const DWORD error=signaled?0:GetLastError();ReleaseSRWLockExclusive(&r->lock);
        if(!signaled)return fail(Error::event,error);const DWORD wait=WaitForSingleObject(r->thread,timeout);
        if(wait!=WAIT_OBJECT_0)return fail(wait==WAIT_TIMEOUT?Error::join_pending:Error::wait,wait==WAIT_TIMEOUT?WAIT_TIMEOUT:GetLastError());}
    const auto result=free_runtime(*owner);if(result.ok())reset_value(owner);return result;
}
} // namespace maicord
