#include "audio_support.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"

// The part of media audio that has no operating system in it: the caller-owned ring, the gain rule and the clock
// conversions. The Windows (media_audio.cpp) and Linux (pulse/media_audio.cpp) transports both build on it.

namespace maicord {namespace {
using Error=MediaAudioError;
using audio_detail::disjoint;
using audio_detail::fail;
bool rate_valid(u32 rate) noexcept { return rate >= 8000 && rate <= 192000; }
bool ring_valid(const MediaAudioRing& r) noexcept {
    if(!r.samples.data||(reinterpret_cast<usize>(r.samples.data)&3)||!r.epoch||!rate_valid(r.sample_rate)||(r.channels!=1&&r.channels!=2)||
        r.samples.count%r.channels||r.samples.count/r.channels<kMediaAudioMinimumRingFrames||r.samples.count/r.channels>kMediaAudioMaximumRingFrames||
        !byte_range_valid({reinterpret_cast<const u8*>(r.samples.data),r.samples.count*4}))return false;
    return r.consumed<=r.submitted&&r.submitted<=r.written&&r.written-r.consumed<=r.samples.count/r.channels;
}
void scale_clip(float* out,const float* in,u32 count,float gain) noexcept {
#pragma clang loop vectorize(disable) unroll(disable)
    for(u32 i=0;i<count;++i){const float value=in[i]*gain;out[i]=value>1?1:value< -1? -1:value;}
}
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
    // The ring wraps at its sample count, a whole number of frames: at most two contiguous copies.
    const u32 start=static_cast<u32>(r->written%(r->samples.count/r->channels))*r->channels,first=minimum(pcm.count,r->samples.count-start);
    copy_bytes(r->samples.data+start,pcm.data,first*4);copy_bytes(r->samples.data,pcm.data+first,(pcm.count-first)*4);
    r->written+=frames;return {};
}
u32 media_audio_ring_render(MediaAudioRing* r,Slice<float> output,float gain) noexcept {
    if(!r||!ring_valid(*r)||!media_audio_gain_valid(gain)||!output.count||output.count%r->channels||
        output.count/r->channels>kMediaAudioMaximumWriteFrames||(reinterpret_cast<usize>(output.data)&3)||
        !disjoint(r,sizeof(*r),output.data,output.count*4)||!disjoint(r->samples.data,r->samples.count*4,output.data,output.count*4))return 0;
    const u32 count=static_cast<u32>(minimum<u64>(output.count/r->channels,r->written-r->submitted)),capacity=r->samples.count/r->channels;
    const u32 total=count*r->channels,start=static_cast<u32>(r->submitted%capacity)*r->channels,first=minimum(total,r->samples.count-start);
    scale_clip(output.data,r->samples.data+start,first,gain);scale_clip(output.data+first,r->samples.data,total-first,gain);
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
} // namespace maicord
