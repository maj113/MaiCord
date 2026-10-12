#include "maicord/platform/media_audio.hpp"

namespace maicord {
void test_media_audio(void (*check)(bool,const char*) noexcept) noexcept {
    static float storage[4096],input[4096],output[4096];
    for(u32 i=0;i<4096;++i){storage[i]=7;input[i]=float(i%97)/96.0f-.5f;output[i]=9;}
    MediaAudioRing ring{};
    check(media_audio_ring_initialize(&ring,{storage,4096},48000,2,1)&&media_audio_ring_free(ring)==2048&&storage[0]==7,
        "media ring: metadata binds exactly 2048 stereo frames without zeroing caller PCM");
    MediaAudioRing high_rate{};
    check(media_audio_ring_initialize(&high_rate,{storage,4096},96000,2,1)&&
        media_audio_ring_initialize(&high_rate,{storage,4096},192000,1,1)&&
        media_audio_ring_initialize(&high_rate,{storage,4096},8001,2,1),
        "media ring: native shared conversion accepts high and nonstandard source rates within the bounded PCM range");
    check(!media_audio_ring_initialize(&high_rate,{storage,4096},7999,2,1)&&high_rate.sample_rate==8001&&
        !media_audio_ring_initialize(&high_rate,{storage,4096},192001,2,1)&&high_rate.sample_rate==8001,
        "media ring: unsupported rate boundaries preserve the existing source atomically");
    check(media_audio_ring_write(&ring,1,{input,4096}).ok()&&ring.written==2048&&!media_audio_ring_free(ring)&&
        media_audio_ring_write(&ring,1,{input,2}).error==MediaAudioError::capacity&&ring.written==2048,
        "media ring: bounded full admission refuses overflow atomically");
    check(media_audio_ring_render(&ring,{output,1024},1)==512&&ring.submitted==512&&!media_audio_ring_free(ring)&&
        bytes_equal(input,output,1024*4),"media ring: rendering copies exact native float samples but does not release unplayed storage");
    check(!media_audio_ring_played(&ring,513)&&ring.consumed==0&&media_audio_ring_played(&ring,200)&&ring.consumed==312&&media_audio_ring_free(ring)==312,
        "media ring: invalid padding preserves cursors and observed consumption releases only played frames");
    media_audio_ring_rewind(&ring);
    check(ring.submitted==312&&media_audio_ring_render(&ring,{output,1024},1)==512&&bytes_equal(input+624,output,1024*4),
        "media ring: device rewind replays retained PCM from confirmed consumption without losing buffered frames");
    check(media_audio_ring_played(&ring,0)&&ring.consumed==824,"media ring: continued native drain advances monotonic played cursor");
    check(media_audio_ring_native_queue_empty(ring,0)&&ring.written>ring.consumed&&!media_audio_ring_native_queue_empty(ring,1),
        "media clock: running or paused drained submitted prefix needs reanchoring despite future PCM, and nonzero native padding never qualifies");
    // A 200ms unsupplied endpoint interval is not part of the 824-frame source
    // timeline. Reanchor at its current native position before submitting new
    // PCM; the first 480 native frames then advance by exactly 10ms at 48k.
    const u64 anchor_position=9600,anchor_frames=ring.consumed;u64 advanced{};
    check(media_audio_clock_time(10080-anchor_position,48000,&advanced)&&advanced==100000&&anchor_frames==824,
        "media clock: refill anchor excludes starvation elapsed time and retains the already played source prefix");
    for(u32 i=0;i<624;++i)output[i]=-.75f;
    check(media_audio_ring_write(&ring,1,{output,624}).ok()&&ring.written==2360,
        "media ring: producer wraps into only the exact released prefix");
    const u32 rendered=media_audio_ring_render(&ring,{output,4096},1);
    bool exact=rendered==1536;for(u32 i=0;i<1224*2&&exact;++i)exact=output[i]==input[1648+i];
    for(u32 i=1224*2;i<1536*2&&exact;++i)exact=output[i]==-.75f;
    check(exact&&media_audio_ring_played(&ring,0)&&media_audio_ring_free(ring)==2048,
        "media ring: cross-wrap render preserves old tail/new prefix ordering and final capacity");
    check(media_audio_ring_native_queue_empty(ring,0),"media clock: EOF confirms an empty submitted prefix");
    const auto before=ring;u32 nan_bits=0x7fc00000;float nan{};copy_bytes(&nan,&nan_bits,4);input[1]=nan;
    check(media_audio_ring_write(&ring,1,{input,2}).error==MediaAudioError::invalid_input&&bytes_equal(&before,&ring,sizeof(ring))&&
        media_audio_ring_write(&ring,2,{output,2}).error==MediaAudioError::stale_epoch&&
        media_audio_ring_write(&ring,1,{storage,2}).error==MediaAudioError::invalid_input,
        "media ring: NaN, stale epoch and alias failures preserve cursors atomically");
    input[0]=2;input[1]=-2;check(media_audio_ring_write(&ring,1,{input,2}).ok()&&media_audio_ring_render(&ring,{output,2},.75f)==1&&output[0]==1&&output[1]==-1,
        "media ring: finite overrange PCM applies gain and clips without a resampler");
    check(!media_audio_ring_native_queue_empty(ring,1)&&!media_audio_ring_native_queue_empty(ring,0),
        "media clock: paused or padding-drained PCM without clock-confirmed consumption preserves the existing anchor");
    media_audio_ring_rewind(&ring);
    check(media_audio_ring_native_queue_empty(ring,0)&&ring.written>ring.submitted,
        "media clock: device rewind creates an empty native prefix while preserving unplayed source PCM");
    ring.ended=true;check(media_audio_ring_write(&ring,1,{input,2}).error==MediaAudioError::stopped,"media ring: EOF seals source admission");
    u64 time=77;
    check(media_audio_clock_time(192000,96000,&time)&&time==20000000&&media_audio_clock_time(1,3,&time)&&time==3333333,
        "media clock: native position/frequency units normalize to signed-timeline-compatible 100ns precision");
    time=77;check(!media_audio_clock_time(~u64{0},1,&time)&&time==77&&!media_audio_clock_time(1,0,&time)&&time==77,
        "media clock: overflow and missing frequency reject without a fabricated time");
    u64 frames=77;
    check(media_audio_clock_frames(72256,48000,48000,&frames)&&frames==72256&&
        media_audio_clock_time(72256,48000,&time)&&time==15053333&&time*48000/10000000==72255,
        "media clock: exact48k EOS retains final72256th frame that the old100ns double floor lost");
    check(media_audio_clock_frames(44101,44100,44100,&frames)&&frames==44101&&
        media_audio_clock_time(44101,44100,&time)&&time*44100/10000000==44100,
        "media clock: exact44.1k endpoint likewise preserves the sample beyond an integer second");
    check(media_audio_clock_frames(578048,384000,48000,&frames)&&frames==72256&&
        media_audio_clock_frames(44101,44100,48000,&frames)&&frames==48001&&
        media_audio_clock_frames(48001,48000,44100,&frames)&&frames==44100&&
        media_audio_clock_frames(1,3,2,&frames)&&frames==0&&media_audio_clock_frames(2,3,2,&frames)&&frames==1,
        "media clock: arbitrary native units and mixed source rates use one exact floor, never assumed sample units or ceil");
    check(media_audio_clock_frames(~u64{0},1,1,&frames)&&frames==~u64{0}&&
        media_audio_clock_frames(~u64{0},2,2,&frames)&&frames==~u64{0}&&
        media_audio_clock_frames(0,48000,48000,&frames)&&!frames,
        "media clock: valid zero and largest representable direct results are retained exactly");
    frames=77;
    check(!media_audio_clock_frames(1,0,48000,&frames)&&frames==77&&
        !media_audio_clock_frames(1,48000,0,&frames)&&frames==77&&!media_audio_clock_frames(1,48000,48000,nullptr)&&
        !media_audio_clock_frames(~u64{0},1,2,&frames)&&frames==77&&
        !media_audio_clock_frames(~u64{0}-1,~u64{0},48000,&frames)&&frames==77&&
        !media_audio_clock_frames((~u64{0}/3)*2+1,2,3,&frames)&&frames==77,
        "media clock: missing output/rate/frequency, product overflow and sum overflow refuse atomically");
    MediaAudio owner{};MediaAudioConfig config{};config.pcm={storage,4096};config.sample_rate=48000;config.channels=2;config.source_origin_100ns=-100000;
    check(media_audio_initialize(&owner,config).ok()&&!owner.runtime&&!media_audio_owned_bytes(&owner)&&media_audio_status(&owner).presentation_time_100ns==-100000&&
        !media_audio_status(&owner).clock_valid&&!media_audio_status(&owner).owned_handles,
        "media lifecycle: metadata opens no audio device/thread and reports no fabricated hardware clock");
    input[0]=.25f;input[1]=-.25f;
    check(media_audio_write(&owner,1,{input,2}).ok()&&media_audio_volume(&owner,.4f).ok()&&owner.config.volume==.4f&&
        media_audio_volume(&owner,nan).error==MediaAudioError::invalid_input&&media_audio_pause(&owner).ok()&&!owner.runtime,
        "media lifecycle: pre-play PCM and desired volume/pause remain device-free metadata");
    const float retained=storage[0];
    check(media_audio_end(&owner,1).ok()&&media_audio_reset(&owner,2,5000).ok()&&owner.ring.written==0&&storage[0]==retained&&
        media_audio_write(&owner,1,{input,2}).error==MediaAudioError::stale_epoch&&media_audio_status(&owner).observed_epoch==2&&
        media_audio_release(&owner,0).ok()&&!owner.initialized,
        "media lifecycle: fresh pre-play epoch reset clears logical stream only and release is allocation-free");
}
}
