#include "maicord/platform/notification_audio.hpp"

namespace maicord {
namespace {
struct Bank {
    NotificationAudioClip clip{};u32 phase{},resolves{},gains{};float volume{1};bool reject{};
};
bool resolve(void* context,u32 id,NotificationAudioClip* out) noexcept {
    auto& b=*static_cast<Bank*>(context);++b.resolves;b.phase=1;
    if(b.reject||id!=1)return false;*out=b.clip;return true;
}
bool gain(void* context,float nominal,float* out) noexcept {
    auto& b=*static_cast<Bank*>(context);++b.gains;
    if(b.phase!=1)return false;b.phase=2;*out=nominal*b.volume;return true;
}
bool same_samples(const i16* a,const i16* b,u32 count) noexcept {return bytes_equal(a,b,count*sizeof(i16));}
struct BlockedBank {HANDLE entered{},unblock{};};
bool blocked_resolve(void* context,u32,NotificationAudioClip*) noexcept {
    auto& bank=*static_cast<BlockedBank*>(context);SetEvent(bank.entered);
    // Bounded synthetic asset stall; always rejects the clip, so the worker
    // never reaches CoCreateInstance(MMDeviceEnumerator), endpoint or playback.
    WaitForSingleObject(bank.unblock,2000);return false;
}
}
void test_native_notification_audio(void(*check)(bool,const char*) noexcept) noexcept {
    using Error=NotificationAudioError;
    constexpr wchar_t endpoint[]=L"render-id-\u03bb";
    constexpr wchar_t short_id[]{L'r',0};
    constexpr usize endpoint_chars=sizeof(endpoint)/sizeof(endpoint[0])-1;
    check(notification_audio_endpoint_matches(L"render-id-\u03bb",endpoint,endpoint_chars)&&
        !notification_audio_endpoint_matches(L"capture-id-\u03bb",endpoint,endpoint_chars)&&
        !notification_audio_endpoint_matches(L"render-id-\u039b",endpoint,endpoint_chars)&&
        !notification_audio_endpoint_matches(L"render-id-\u03bb-extra",endpoint,endpoint_chars),
        "notification endpoint filtering compares full opaque IDs exactly, including Unicode and suffixes");
    check(!notification_audio_endpoint_matches(short_id,endpoint,endpoint_chars)&&
        !notification_audio_endpoint_matches(nullptr,endpoint,endpoint_chars)&&
        !notification_audio_endpoint_matches(endpoint,nullptr,endpoint_chars)&&
        !notification_audio_endpoint_matches(endpoint,endpoint,0),
        "notification callback identity comparison stops at a short event terminator and rejects absent identities");
    NotificationAudioStatus failure_status{};
    failure_status.desired_epoch=2;failure_status.observed_epoch=1;
    failure_status.queued=4;failure_status.outstanding=5;
    failure_status.error=Error::resolve;failure_status.native_error=17;
    check(!notification_audio_record_stream_failure(&failure_status,1,static_cast<u32>(E_FAIL))&&
        failure_status.desired_epoch==2&&failure_status.observed_epoch==1&&failure_status.queued==4&&
        failure_status.outstanding==5&&failure_status.error==Error::resolve&&failure_status.native_error==17,
        "notification late old-stream close failure cannot contaminate the new epoch or its accepted requests");
    check(notification_audio_record_stream_failure(&failure_status,2,static_cast<u32>(E_FAIL))&&
        failure_status.error==Error::stream&&failure_status.native_error==static_cast<u32>(E_FAIL)&&
        failure_status.queued==4&&failure_status.outstanding==5&&
        !notification_audio_record_stream_failure(&failure_status,0,0)&&
        !notification_audio_record_stream_failure(nullptr,2,0),
        "notification matching-epoch close failure records the native transport error without changing reservations");
    constexpr i16 stereo[]{10000,-10000,20000,-20000,30000,-30000,1000,-1000};
    const NotificationAudioClip clip{{stereo,8}};
    check(notification_audio_clip_valid(clip)&&!notification_audio_clip_valid({})&&
        !notification_audio_clip_valid({{stereo,7}})&&!notification_audio_clip_valid({{nullptr,8}}),
        "notification PCM descriptor admits only nonempty immutable interleaved stereo frames");
    float nan{};const u32 nan_bits=0x7fc00000;copy_bytes(&nan,&nan_bits,sizeof(nan));
    check(notification_audio_gain_valid(0)&&notification_audio_gain_valid(1)&&
        !notification_audio_gain_valid(-0.1f)&&!notification_audio_gain_valid(1.1f)&&!notification_audio_gain_valid(nan),
        "notification gain rejects NaN, negative and out-of-range values before scheduling");
    NotificationAudioMixer mixer{};notification_audio_mixer_reset(&mixer,1);
    check(notification_audio_mixer_add(&mixer,2,clip,0.5f).error==Error::stale_epoch&&!mixer.count&&
        notification_audio_mixer_add(&mixer,1,clip,0.5f).ok(),
        "notification mixer rejects stale owner epoch without modifying active voices");
    i16 output[8]{};constexpr i16 half[]{5000,-5000,10000,-10000,15000,-15000,500,-500};
    const auto first=notification_audio_mixer_render(&mixer,{output,8});
    check(first.error==Error::none&&first.frames==4&&first.nonzero&&same_samples(output,half,8)&&
        mixer.count==1&&!notification_audio_mixer_needed(mixer),
        "notification offline gain preserves stereo channels and pins fully submitted PCM until consumed");
    check(notification_audio_mixer_played(&mixer,5)==0&&mixer.consumed==0&&
        notification_audio_mixer_played(&mixer,2)==0&&mixer.consumed==2,
        "notification padding accounts source-format frames and rejects invalid/backwards consumption");
    notification_audio_mixer_rewind(&mixer);
    const auto rewound=notification_audio_mixer_render(&mixer,{output,4});
    check(rewound.frames==2&&same_samples(output,half+4,4)&&mixer.submitted==4&&
        notification_audio_mixer_played(&mixer,0)==1&&!mixer.count&&!mixer.voices[0].clip.samples.data,
        "notification device-switch rewind reproduces only known unconsumed source PCM then retires its borrowed descriptor");

    notification_audio_mixer_reset(&mixer,2);
    notification_audio_mixer_add(&mixer,2,clip,1);
    notification_audio_mixer_render(&mixer,{output,4});
    constexpr i16 other[]{1000,2000,2000,4000};
    notification_audio_mixer_add(&mixer,2,{{other,4}},1);
    constexpr i16 overlap[]{31000,-28000,3000,3000};
    const auto mixed=notification_audio_mixer_render(&mixer,{output,4});
    check(mixed.frames==2&&same_samples(output,overlap,4)&&mixer.count==2,
        "notification voices overlap independently at the next submitted frame with unequal left/right source channels");
    check(notification_audio_mixer_played(&mixer,1)==0&&notification_audio_mixer_played(&mixer,0)==2,
        "notification overlapping voices remain owned through their final queued endpoint frames");
    constexpr i16 peaks[]{32767,-32768};
    notification_audio_mixer_add(&mixer,2,{{peaks,2}},1);notification_audio_mixer_add(&mixer,2,{{peaks,2}},1);
    const auto peak=notification_audio_mixer_render(&mixer,{output,2});
    check(peak.frames==1&&output[0]==32767&&output[1]==-32768,
        "notification summation saturates PCM16 rather than overflowing or wrapping");
    notification_audio_mixer_reset(&mixer,3);
    notification_audio_mixer_add(&mixer,3,clip,0);
    const auto silent=notification_audio_mixer_render(&mixer,{output,8});
    check(silent.frames==4&&!silent.nonzero&&output[0]==0&&output[7]==0,
        "notification zero gain consumes voice time with an explicit silent packet");
    notification_audio_mixer_reset(&mixer,4);
    notification_audio_mixer_add(&mixer,4,clip,1);
    check(notification_audio_mixer_render(&mixer,{const_cast<i16*>(stereo),8}).error==Error::invalid_input&&
        !mixer.submitted&&notification_audio_mixer_render(&mixer,{output,3}).error==Error::invalid_input&&!mixer.submitted,
        "notification mixer output cannot alias borrowed PCM or split an interleaved frame");
    check(notification_audio_mixer_add(&mixer,4,{{reinterpret_cast<const i16*>(&mixer),8}},1).error==Error::invalid_input&&mixer.count==1,
        "notification PCM cannot alias the mutable schedule owner");
    notification_audio_mixer_reset(&mixer,5);bool voices=true;
    for(u32 i=0;i<kNotificationAudioVoiceCapacity;++i)voices=voices&&notification_audio_mixer_add(&mixer,5,clip,0.5f).ok();
    check(voices&&mixer.count==64&&notification_audio_mixer_add(&mixer,5,clip,0.5f).error==Error::full_voices,
        "notification mixer reserves64 distinct voices and explicitly rejects a65th without replacement");
    notification_audio_mixer_render(&mixer,{output,8});
    check(notification_audio_mixer_add(&mixer,5,clip,0.5f).error==Error::full_voices&&
        notification_audio_mixer_played(&mixer,0)==64&&notification_audio_mixer_add(&mixer,5,clip,0.5f).ok(),
        "notification voice capacity is released on consumed completion, not merely rendering the last sample");
    notification_audio_mixer_reset(&mixer,6);
    check(!mixer.count&&!mixer.voices[0].clip.samples.data&&notification_audio_mixer_add(&mixer,5,clip,1).error==Error::stale_epoch,
        "notification reset erases all borrowed voice descriptors and invalidates old generations");
    mixer.submitted=mixer.consumed=~u64{0}-1;
    check(notification_audio_mixer_add(&mixer,6,clip,1).error==Error::timeline_exhausted&&!mixer.count,
        "notification absolute source frame counter cannot wrap into a stale schedule");

    constexpr i16 alternate_samples[]{1500,-2500,3500,-4500};
    const NotificationAudioClip alternate{{alternate_samples,4},48000};
    check(notification_audio_clip_valid(alternate)&&!notification_audio_clip_valid({{alternate_samples,4},32000}),
        "notification native clip rates admit exact44100 and48000 only, without coercing unsupported source PCM");
    notification_audio_mixer_reset(&mixer,7);
    check(notification_audio_mixer_add(&mixer,7,clip,.5f).ok()&&notification_audio_mixer_add(&mixer,7,alternate,1).ok()&&
        notification_audio_mixer_voices_rate(mixer,44100)==1&&notification_audio_mixer_voices_rate(mixer,48000)==1,
        "notification mixed-rate descriptors share capacity while retaining separate native source timelines");
    notification_audio_mixer_render(&mixer,{output,4});
    const auto alternate_mix=notification_audio_mixer_render_rate(&mixer,{output,4},48000);
    check(alternate_mix.frames==2&&same_samples(output,alternate_samples,4)&&mixer.submitted==2&&mixer.submitted_48000==2&&
        !mixer.consumed&&!mixer.consumed_48000&&notification_audio_mixer_needed(mixer)==2,
        "notification48000 rendering copies exact samples without interpolation or advancing44100 cursor");
    notification_audio_mixer_played_rate(&mixer,1,48000);notification_audio_mixer_rewind_rate(&mixer,48000);
    const auto alternate_tail=notification_audio_mixer_render_rate(&mixer,{output,2},48000);
    check(alternate_tail.frames==1&&same_samples(output,alternate_samples+2,2)&&mixer.submitted==2&&
        notification_audio_mixer_played_rate(&mixer,0,48000)==1&&mixer.count==1,
        "notification48000 padding rewind reproduces its native tail and retires only its own rate");
    notification_audio_mixer_render(&mixer,{output,4});
    check(same_samples(output,half+4,4)&&notification_audio_mixer_played(&mixer,0)==1&&!mixer.count,
        "notification44100 voice remains intact after alternate-rate consumption and completes independently");
    notification_audio_mixer_reset(&mixer,8);voices=true;
    for(u32 i=0;i<64;++i)voices=voices&&notification_audio_mixer_add(&mixer,8,i&1?alternate:clip,.5f).ok();
    check(voices&&notification_audio_mixer_add(&mixer,8,alternate,1).error==Error::full_voices&&
        notification_audio_mixer_voices_rate(mixer,44100)==32&&notification_audio_mixer_voices_rate(mixer,48000)==32,
        "notification global64 capacity is shared by both rates rather than creating128 independent voices");
    notification_audio_mixer_reset(&mixer,9);
    mixer.submitted_48000=mixer.consumed_48000=~u64{0}-1;
    check(notification_audio_mixer_add(&mixer,9,alternate,1).error==Error::timeline_exhausted&&!mixer.count&&
        notification_audio_mixer_render_rate(&mixer,{output,2},32000).error==Error::invalid_input,
        "notification alternate absolute cursor cannot overflow and unsupported render rate fails atomically");

    Bank bank{clip};NotificationAudioConfig config{};config.resolve=resolve;config.gain=gain;config.context=&bank;
    NotificationAudioRequest request{1,1,0.4f,{}};
    bank.volume=0.5f;NotificationAudioClip prepared{};float effective=-1;
    const auto ready=notification_audio_prepare_request(config,request,&prepared,&effective);
    check(ready.ok()&&bank.phase==2&&bank.resolves==1&&bank.gains==1&&prepared.samples.data==stereo&&effective>0.199f&&effective<0.201f,
        "notification worker preparation resolves immutable asset before sampling current output gain");
    notification_audio_mixer_reset(&mixer,1);notification_audio_mixer_add(&mixer,1,prepared,effective);
    bank.volume=1;
    notification_audio_mixer_render(&mixer,{output,2});
    check(output[0]>=1999&&output[0]<=2000&&output[1]>=-2000&&output[1]<=-1999,
        "notification active voice freezes loaded gain even if output preference later changes");
    bank.phase=0;bank.reject=true;
    check(notification_audio_prepare_request(config,request,&prepared,&effective).error==Error::resolve&&bank.gains==1,
        "notification failed asset load does not sample gain or publish a fabricated clip");
    bank.reject=false;bank.volume=3;bank.phase=0;
    check(notification_audio_prepare_request(config,request,&prepared,&effective).error==Error::invalid_input,
        "notification worker validates callback effective gain before mixer admission");
    config.gain=nullptr;request.clip=clip;
    check(notification_audio_prepare_request(config,request,&prepared,&effective).ok()&&effective==0.4f,
        "notification predecoded PCM and null gain callback retain direct QA gain without asset loading");

    NotificationAudio owner{};config={};config.enabled=false;
    check(notification_audio_initialize(&owner,config).ok()&&!owner.runtime&&!notification_audio_owned_bytes(&owner)&&
        !notification_audio_can_accept(&owner,1),"notification disabled metadata initialization creates no thread, COM or audio endpoint");
    check(notification_audio_submit(&owner,{1,0,0.4f,clip}).error==Error::stopped&&!owner.runtime,
        "notification disabled owner rejects even valid PCM without opening or emitting audio");
    check(notification_audio_reset(&owner,2,true).ok()&&notification_audio_can_accept(&owner,2)&&
        !notification_audio_can_accept(&owner,33)&&notification_audio_reset(&owner,2,false).error==Error::stale_epoch,
        "notification pre-pop dual-route reservation and strictly increasing cancellation epochs are explicit");
    const auto status=notification_audio_status(&owner);
    check(status.desired_epoch==2&&status.observed_epoch==2&&!status.outstanding&&!status.worker_started&&!status.stream_open,
        "notification unlaunched owner is immediately quiescent with no borrowed PCM readers");
    check(notification_audio_submit(&owner,{1,0,0.4f,clip}).error==Error::stale_epoch&&!owner.runtime&&
        notification_audio_submit(&owner,{2,0,nan,clip}).error==Error::invalid_input&&!owner.runtime,
        "notification stale and invalid requests fail before lazy allocation and thread launch");
    check(notification_audio_release(&owner,0).ok()&&!owner.initialized&&!owner.runtime&&notification_audio_release(&owner,0).ok(),
        "notification unlaunched owner teardown is idempotent and needs no worker or device action");

    // Static callback context remains alive even if an exceptional join failure
    // makes the test retain its live worker/resources for the failing harness.
    static BlockedBank blocked{};
    blocked={CreateEventW(nullptr,TRUE,FALSE,nullptr),CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    check(blocked.entered&&blocked.unblock,"notification nonemitting worker test owns two synchronization acknowledgments");
    if(!blocked.entered||!blocked.unblock){if(blocked.entered)CloseHandle(blocked.entered);if(blocked.unblock)CloseHandle(blocked.unblock);return;}
    config={};config.resolve=blocked_resolve;config.context=&blocked;
    const bool initialized=notification_audio_initialize(&owner,config).ok();
    const auto started=initialized?notification_audio_submit(&owner,{1,1,0.4f,{}}):NotificationAudioResult{Error::invalid_input};
    const bool entered=started.ok()&&WaitForSingleObject(blocked.entered,1000)==WAIT_OBJECT_0;
    check(entered,"notification worker acknowledges entering resolver before any native audio device creation");
    if(entered){
        bool admitted=true;
        for(u32 i=0;i<32;++i)admitted=admitted&&notification_audio_submit(&owner,{1,1,0.4f,{}}).ok();
        const auto queued=notification_audio_status(&owner);
        check(admitted&&queued.queued==32&&queued.outstanding==33&&!notification_audio_can_accept(&owner,1)&&
            notification_audio_submit(&owner,{1,1,0.4f,{}}).error==Error::full_queue&&
            queued.device_open_attempts==0&&queued.stream_open_attempts==0&&notification_audio_owned_handles(&owner)==4,
            "notification32 queued plus one resolver reservation are explicit; full queue never drops or opens an endpoint");
        check(notification_audio_reset(&owner,2,false).ok(),"notification owner reset wakes the worker and atomically purges waiting borrowed requests");
        const auto reset=notification_audio_status(&owner);
        check(reset.queued==0&&reset.outstanding==1&&reset.canceled==32&&reset.desired_epoch==2&&reset.observed_epoch==1,
            "notification blocked old resolver remains owned until epoch cancellation is actually acknowledged");
        const auto pending=notification_audio_release(&owner,0);
        check(pending.error==Error::join_pending&&owner.runtime&&notification_audio_owned_handles(&owner)==4&&
            notification_audio_owned_bytes(&owner)>0&&!notification_audio_can_accept(&owner,1),
            "notification stop timeout retains worker, four exact handles, metadata and resolver context instead of freeing live storage");
    }
    SetEvent(blocked.unblock);
    // No sleeps: the release waits on the actual thread's completion handle.
    const auto joined=notification_audio_release(&owner,1000);
    check(joined.ok()&&!owner.runtime&&!notification_audio_owned_handles(&owner)&&!notification_audio_owned_bytes(&owner),
        "notification acknowledged worker exit permits exact attached-handle teardown with no device or playback");
    if(joined.ok()){CloseHandle(blocked.entered);CloseHandle(blocked.unblock);}
    // If an exceptional join failure occurs, retain the context/events rather
    // than close a handle under a live callback; test harness must fail/exit.
}
} // namespace maicord
